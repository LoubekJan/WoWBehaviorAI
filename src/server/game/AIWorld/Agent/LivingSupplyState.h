/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGSUPPLYSTATE_H
#define AIWORLD_LIVINGSUPPLYSTATE_H

#include "Define.h"
#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <utility>

// World-thread runtime state only. The caller validates materialization, action
// ownership, live creature/corpse identity, distance and completed activity.
// Merely reaching a point is never permission to create, transfer or eat food.
namespace LivingSupply
{
    constexpr uint32 CarcassPortions = 6;
    constexpr uint32 PredatorMealPortions = 3;
    constexpr uint32 HunterCargoCapacity = 6;
    constexpr uint32 DepotCapacity = 30;
    constexpr uint32 DepotTarget = 12;
    constexpr uint32 DepotLow = 4;

    struct CarcassKey
    {
        uint64 Source = 0;
        uint64 Life = 0;
        bool operator==(CarcassKey const& other) const { return Source == other.Source && Life == other.Life; }
    };
    enum class ClaimKind : uint8 { Harvest, PredatorMeal };
    struct Token
    {
        uint64 Actor = 0;
        uint64 Serial = 0;
        bool operator==(Token const& other) const { return Actor == other.Actor && Serial == other.Serial; }
    };
    struct Result
    {
        bool Success = false;
        bool Duplicate = false;
        uint32 Portions = 0;
        char const* Reason = "REJECTED";
        explicit operator bool() const { return Success; }
    };
    struct Depot
    {
        uint32 Id = 0;
        uint32 Stock = 0;
        uint32 Capacity = 0;
        uint32 Target = 0;
        uint32 Low = 0;
    };
    struct Carcass
    {
        CarcassKey Key;
        uint32 Remaining = 0;
        uint32 Reserved = 0;
        uint64 ExpiresAtMs = 0;
        bool Active = false;
    };
    struct Counters
    {
        uint64 InitialFood = 0;
        // Includes meat committed directly to predator consumption. The carcass
        // potential becomes accounted food only at a completed extraction.
        uint64 HarvestedTotal = 0;
        uint64 ConsumedTotal = 0;
        uint64 LostTotal = 0;
        uint64 DeliveredTotal = 0;
        uint64 PredatorConsumed = 0;
        uint64 DepotConsumed = 0;
        uint64 PotentialCreated = 0;
        uint64 PotentialLost = 0;
    };

    class Ledger
    {
    public:
        bool ConfigureDepot(uint32 id, uint32 initial = 0, uint32 capacity = DepotCapacity,
            uint32 target = DepotTarget, uint32 low = DepotLow)
        {
            if (!id || !capacity || low > target || target > capacity || initial > capacity)
                return false;
            if (auto current = GetDepot(id))
                return current->Capacity == capacity && current->Target == target && current->Low == low &&
                    _initialDepotStock.at(id) == initial;
            if (_depots.size() >= 2)
                return false;
            _depots.emplace(id, Depot{id, initial, capacity, target, low});
            _initialDepotStock.emplace(id, initial);
            _counters.InitialFood += initial;
            return true;
        }

        bool RegisterActor(uint64 actor, uint32 depotId = 0, uint32 cargoCapacity = HunterCargoCapacity,
            bool predator = false, bool consumer = false)
        {
            if (!actor || (predator && consumer) || (consumer ? (!GetDepot(depotId) || cargoCapacity) :
                predator ? (depotId || cargoCapacity) :
                (!GetDepot(depotId) || !cargoCapacity || cargoCapacity > HunterCargoCapacity)))
                return false;
            auto current = _actors.find(actor);
            if (current != _actors.end())
                return current->second.DepotId == depotId && current->second.Capacity == cargoCapacity &&
                    current->second.Predator == predator && current->second.Consumer == consumer;
            if (_actors.size() >= 128)
                return false;
            _actors.emplace(actor, Actor{depotId, cargoCapacity, predator, consumer});
            return true;
        }

        // Source identifies a reviewed prey spawn; Life must strictly increase
        // for each genuine new life. Expired/exhausted sources retain a bounded
        // per-source tombstone so a repeated death observation cannot mint meat.
        bool RegisterCarcass(CarcassKey key, uint64 nowMs, uint64 expiresAtMs,
            uint32 portions = CarcassPortions)
        {
            if (!key.Source || !key.Life || expiresAtMs <= nowMs || !portions || portions > CarcassPortions)
                return false;
            auto current = _carcasses.find(key.Source);
            if (current != _carcasses.end())
            {
                if (key.Life <= current->second.Key.Life)
                    return false;
                CloseCarcass(current->second.Key);
            }
            else if (_carcasses.size() >= 256)
                return false;
            _carcasses[key.Source] = {key, portions, 0, expiresAtMs, true};
            _counters.PotentialCreated += portions;
            return true;
        }

        std::optional<Token> Reserve(uint64 actorId, CarcassKey key, ClaimKind kind,
            uint32 requested, uint64 nowMs, uint64 claimTtlMs = 15000)
        {
            Expire(nowMs);
            auto actor = _actors.find(actorId);
            auto carcass = FindCarcass(key);
            if (actor == _actors.end() || !carcass || !carcass->Active || !requested || !claimTtlMs ||
                (kind != ClaimKind::Harvest && kind != ClaimKind::PredatorMeal))
                return std::nullopt;
            auto& owner = actor->second;
            if (owner.Consumer || owner.Predator != (kind == ClaimKind::PredatorMeal) ||
                requested > (owner.Predator ? PredatorMealPortions : owner.Capacity))
                return std::nullopt;
            if (owner.Pending)
            {
                auto const& pending = *owner.Pending;
                if (pending.Key == key && pending.Kind == kind && pending.Requested == requested)
                    return pending.Id;
                return std::nullopt;
            }
            uint32 amount = std::min(requested, carcass->Remaining - carcass->Reserved);
            if (!owner.Predator)
                amount = std::min(amount, owner.Capacity - owner.Cargo);
            if (!amount || _nextClaimSerial == std::numeric_limits<uint64>::max())
                return std::nullopt;
            uint64 expires = claimTtlMs > std::numeric_limits<uint64>::max() - nowMs ?
                std::numeric_limits<uint64>::max() : nowMs + claimTtlMs;
            Token token{actorId, ++_nextClaimSerial};
            owner.Pending = Claim{token, key, kind, requested, amount, std::min(expires, carcass->ExpiresAtMs)};
            carcass->Reserved += amount;
            return token;
        }

        Result Commit(Token token, uint64 nowMs)
        {
            Expire(nowMs);
            auto found = _actors.find(token.Actor);
            if (!token.Serial || found == _actors.end())
                return Rejected("UNKNOWN_CLAIM");
            auto& actor = found->second;
            if (actor.LastCommitted && actor.LastCommitted->first == token)
                return Replayed(actor.LastCommitted->second);
            if (!actor.Pending || !(actor.Pending->Id == token))
                return Rejected("UNKNOWN_CLAIM");
            auto claim = *actor.Pending;
            auto carcass = FindCarcass(claim.Key);
            if (!carcass || !carcass->Active || nowMs >= claim.ExpiresAtMs ||
                carcass->Reserved < claim.Amount || carcass->Remaining < claim.Amount ||
                (claim.Kind == ClaimKind::Harvest && actor.Cargo > actor.Capacity - claim.Amount))
            {
                Cancel(token);
                return Rejected("CLAIM_INVALIDATED");
            }
            carcass->Remaining -= claim.Amount;
            carcass->Reserved -= claim.Amount;
            actor.Pending.reset();
            _counters.HarvestedTotal += claim.Amount;
            if (claim.Kind == ClaimKind::PredatorMeal)
            {
                _counters.ConsumedTotal += claim.Amount;
                _counters.PredatorConsumed += claim.Amount;
            }
            else
                actor.Cargo += claim.Amount;
            Result result{true, false, claim.Amount, "COMMITTED"};
            actor.LastCommitted = std::make_pair(token, result);
            return result;
        }

        bool Cancel(Token token)
        {
            auto actor = _actors.find(token.Actor);
            if (actor == _actors.end() || !actor->second.Pending || !(actor->second.Pending->Id == token))
                return false;
            Release(*actor->second.Pending);
            actor->second.Pending.reset();
            return true;
        }

        bool CancelActor(uint64 actor)
        {
            auto found = _actors.find(actor);
            return found != _actors.end() && found->second.Pending && Cancel(found->second.Pending->Id);
        }

        void CloseCarcass(CarcassKey key)
        {
            auto carcass = FindCarcass(key);
            if (!carcass || !carcass->Active)
                return;
            for (auto& [_, actor] : _actors)
                if (actor.Pending && actor.Pending->Key == key)
                {
                    Release(*actor.Pending);
                    actor.Pending.reset();
                }
            _counters.PotentialLost += carcass->Remaining;
            carcass->Remaining = 0;
            carcass->Active = false;
        }

        void Expire(uint64 nowMs)
        {
            for (auto& [_, actor] : _actors)
                if (actor.Pending && nowMs >= actor.Pending->ExpiresAtMs)
                {
                    Release(*actor.Pending);
                    actor.Pending.reset();
                }
            for (auto& [_, carcass] : _carcasses)
                if (carcass.Active && nowMs >= carcass.ExpiresAtMs)
                    CloseCarcass(carcass.Key);
        }

        // eventSerial is an owned completed-action identity, monotonically
        // increasing per actor across delivery and meals. Exact latest repeats
        // report the original receipt; altered or older repeats cannot mutate.
        Result Deliver(uint64 actorId, uint64 eventSerial, uint32 depotId,
            uint32 requested, bool physicallyCompleted)
        {
            auto actor = _actors.find(actorId);
            if (actor == _actors.end())
                return Rejected("UNKNOWN_ACTOR");
            auto& owner = actor->second;
            if (auto replay = ReplayEvent(owner, eventSerial, EventKind::Delivery, depotId, requested))
                return *replay;
            auto depot = _depots.find(depotId);
            if (!physicallyCompleted || !eventSerial || !requested || owner.Predator || owner.Consumer ||
                owner.DepotId != depotId || depot == _depots.end())
                return Rejected("DELIVERY_NOT_AUTHORIZED");
            uint32 amount = std::min({requested, owner.Cargo, depot->second.Capacity - depot->second.Stock});
            if (!amount)
                return Rejected(owner.Cargo ? "DEPOT_FULL" : "NO_CARGO");
            owner.Cargo -= amount;
            depot->second.Stock += amount;
            _counters.DeliveredTotal += amount;
            return SaveEvent(owner, eventSerial, EventKind::Delivery, depotId, requested, amount);
        }

        Result EatDepot(uint64 actorId, uint64 eventSerial, uint32 depotId,
            uint32 portions, bool physicallyCompleted)
        {
            auto actor = _actors.find(actorId);
            if (actor == _actors.end())
                return Rejected("UNKNOWN_ACTOR");
            auto& owner = actor->second;
            if (auto replay = ReplayEvent(owner, eventSerial, EventKind::Meal, depotId, portions))
                return *replay;
            auto depot = _depots.find(depotId);
            if (!physicallyCompleted || !eventSerial || !portions || owner.Predator ||
                owner.DepotId != depotId || depot == _depots.end())
                return Rejected("MEAL_NOT_AUTHORIZED");
            if (depot->second.Stock < portions)
                return Rejected("NO_FOOD");
            depot->second.Stock -= portions;
            _counters.ConsumedTotal += portions;
            _counters.DepotConsumed += portions;
            return SaveEvent(owner, eventSerial, EventKind::Meal, depotId, portions, portions);
        }

        uint32 DropCargo(uint64 actorId)
        {
            auto actor = _actors.find(actorId);
            if (actor == _actors.end())
                return 0;
            CancelActor(actorId);
            auto& owner = actor->second;
            uint32 amount = owner.Cargo;
            _counters.LostTotal += amount;
            owner.Cargo = 0;
            // A new materialization cannot accept a previous life's receipt.
            owner.LastCommitted.reset();
            owner.LastEvent.reset();
            return amount;
        }

        uint32 GetCargo(uint64 actor) const
        { auto found = _actors.find(actor); return found == _actors.end() ? 0 : found->second.Cargo; }
        Depot const* GetDepot(uint32 id) const
        { auto found = _depots.find(id); return found == _depots.end() ? nullptr : &found->second; }
        Carcass const* GetCarcass(CarcassKey key) const
        { auto found = _carcasses.find(key.Source); return found == _carcasses.end() || !(found->second.Key == key) ? nullptr : &found->second; }
        Counters const& GetCounters() const { return _counters; }
        uint64 DepotFood() const
        { uint64 sum = 0; for (auto const& [_, depot] : _depots) sum += depot.Stock; return sum; }
        uint64 CargoFood() const
        { uint64 sum = 0; for (auto const& [_, actor] : _actors) sum += actor.Cargo; return sum; }
        uint64 RemainingPotential() const
        { uint64 sum = 0; for (auto const& [_, carcass] : _carcasses) sum += carcass.Remaining; return sum; }
        bool Conserved() const
        { return _counters.InitialFood + _counters.HarvestedTotal == DepotFood() + CargoFood() + _counters.ConsumedTotal + _counters.LostTotal; }
        bool PotentialConserved() const
        { return _counters.PotentialCreated == RemainingPotential() + _counters.HarvestedTotal + _counters.PotentialLost; }

    private:
        struct Claim
        {
            Token Id;
            CarcassKey Key;
            ClaimKind Kind;
            uint32 Requested;
            uint32 Amount;
            uint64 ExpiresAtMs;
        };
        enum class EventKind : uint8 { Delivery, Meal };
        struct Event
        {
            uint64 Serial;
            EventKind Kind;
            uint32 DepotId;
            uint32 Requested;
            Result Receipt;
        };
        struct Actor
        {
            uint32 DepotId;
            uint32 Capacity;
            bool Predator;
            bool Consumer;
            uint32 Cargo = 0;
            std::optional<Claim> Pending;
            std::optional<std::pair<Token, Result>> LastCommitted;
            uint64 LastEventSerial = 0;
            std::optional<Event> LastEvent;
        };
        static Result Rejected(char const* reason) { return {false, false, 0, reason}; }
        static Result Replayed(Result receipt) { receipt.Duplicate = true; return receipt; }
        static std::optional<Result> ReplayEvent(Actor const& actor, uint64 serial,
            EventKind kind, uint32 depotId, uint32 requested)
        {
            if (serial > actor.LastEventSerial)
                return std::nullopt;
            if (actor.LastEvent && actor.LastEvent->Serial == serial && actor.LastEvent->Kind == kind &&
                actor.LastEvent->DepotId == depotId && actor.LastEvent->Requested == requested)
                return Replayed(actor.LastEvent->Receipt);
            return Rejected("STALE_OR_CHANGED_EVENT");
        }
        static Result SaveEvent(Actor& actor, uint64 serial, EventKind kind, uint32 depotId,
            uint32 requested, uint32 amount)
        {
            Result result{true, false, amount, "COMPLETED"};
            actor.LastEventSerial = serial;
            actor.LastEvent = Event{serial, kind, depotId, requested, result};
            return result;
        }
        Carcass* FindCarcass(CarcassKey key)
        { auto found = _carcasses.find(key.Source); return found == _carcasses.end() || !(found->second.Key == key) ? nullptr : &found->second; }
        void Release(Claim const& claim)
        {
            if (auto carcass = FindCarcass(claim.Key))
                carcass->Reserved -= claim.Amount;
        }
        std::map<uint32, Depot> _depots;
        std::map<uint32, uint32> _initialDepotStock;
        std::map<uint64, Actor> _actors;
        std::map<uint64, Carcass> _carcasses;
        uint64 _nextClaimSerial = 0;
        Counters _counters;
    };
}
#endif
