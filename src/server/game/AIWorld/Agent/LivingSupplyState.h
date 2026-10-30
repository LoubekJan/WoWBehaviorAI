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
    constexpr uint32 RaiderCargoCapacity = 2;
    constexpr uint32 FarmerCargoCapacity = 2;
    constexpr uint32 CarrierCargoCapacity = 6;
    constexpr uint32 CityDepotId = 7;
    constexpr uint32 FieldCapacity = 24;
    constexpr uint64 FieldRegrowMs = 30000;
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
        uint64 Stolen = 0;
        uint64 Exported = 0;
    };
    struct Carcass
    {
        CarcassKey Key;
        uint32 Remaining = 0;
        uint32 Reserved = 0;
        uint64 ExpiresAtMs = 0;
        bool Active = false;
    };
    struct Field
    {
        uint32 Id = 0;
        uint32 DepotId = 0;
        uint32 Remaining = 0;
        uint32 Capacity = 0;
        uint64 RegrowMs = 0;
        uint64 LastGrowthAtMs = 0;
        uint64 HarvestedTotal = 0;
        uint64 RegrownTotal = 0;
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
        // A transfer of existing accounted food, never a new food source.
        uint64 StolenTotal = 0;
        uint64 LoadedTotal = 0;
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
            if (_depots.size() >= 7)
                return false;
            _depots.emplace(id, Depot{id, initial, capacity, target, low});
            _initialDepotStock.emplace(id, initial);
            _counters.InitialFood += initial;
            return true;
        }

        // The reviewed three crop sources each belong to their same-numbered
        // village depot. Crops are potential, not carried or stored food.
        bool ConfigureField(uint32 id, uint64 nowMs, uint32 initial = FieldCapacity,
            uint32 capacity = FieldCapacity, uint64 regrowMs = FieldRegrowMs)
        {
            if (id < 4 || id > 6 || !GetDepot(id) || !capacity || capacity > FieldCapacity ||
                initial > capacity || regrowMs < FieldRegrowMs)
                return false;
            if (auto current = GetField(id))
                return current->Capacity == capacity && current->RegrowMs == regrowMs &&
                    _initialFieldCrops.at(id) == initial;
            if (_fields.size() >= 3)
                return false;
            _fields.emplace(id, Field{id, id, initial, capacity, regrowMs, nowMs});
            _initialFieldCrops.emplace(id, initial);
            _counters.PotentialCreated += initial;
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
                    current->second.Predator == predator && current->second.Consumer == consumer &&
                    !current->second.RaidSourceDepotId && !current->second.FieldId && !current->second.Carrier;
            if (_actors.size() >= 128)
                return false;
            _actors.emplace(actor, Actor{depotId, cargoCapacity, predator, consumer});
            return true;
        }

        bool RegisterFarmer(uint64 actor, uint32 depotId, uint32 fieldId,
            uint32 cargoCapacity = FarmerCargoCapacity)
        {
            auto field = GetField(fieldId);
            if (!actor || !field || field->DepotId != depotId || !GetDepot(depotId) ||
                !cargoCapacity || cargoCapacity > FarmerCargoCapacity)
                return false;
            auto current = _actors.find(actor);
            if (current != _actors.end())
                return current->second.DepotId == depotId && current->second.Capacity == cargoCapacity &&
                    current->second.FieldId == fieldId && !current->second.RaidSourceDepotId &&
                    !current->second.Predator && !current->second.Consumer && !current->second.Carrier;
            if (_actors.size() >= 128)
                return false;
            Actor farmer{depotId, cargoCapacity, false, false};
            farmer.FieldId = fieldId;
            _actors.emplace(actor, std::move(farmer));
            return true;
        }

        // Theft is a separate narrow capability, not an ordinary carrier's
        // permission to withdraw from an arbitrary warehouse. Registration
        // never upgrades or replaces an existing participant's capability.
        bool RegisterRaider(uint64 actor, uint32 ownDepotId = 3, uint32 sourceDepotId = 1,
            uint32 cargoCapacity = RaiderCargoCapacity, bool allVillageSources = false)
        {
            if (!actor || ownDepotId != 3 || sourceDepotId != 1 || cargoCapacity != RaiderCargoCapacity ||
                !GetDepot(ownDepotId) || !GetDepot(sourceDepotId))
                return false;
            if (allVillageSources && (!GetDepot(4) || !GetDepot(5) || !GetDepot(6)))
                return false;
            auto current = _actors.find(actor);
            if (current != _actors.end())
                return current->second.DepotId == ownDepotId && current->second.Capacity == cargoCapacity &&
                    current->second.RaidSourceDepotId == sourceDepotId && !current->second.Predator &&
                    !current->second.Consumer && !current->second.FieldId &&
                    current->second.AllVillageRaidSources == allVillageSources && !current->second.Carrier;
            if (_actors.size() >= 128) return false;
            Actor raider{ownDepotId, cargoCapacity, false, false, sourceDepotId};
            raider.AllVillageRaidSources = allVillageSources;
            _actors.emplace(actor, std::move(raider));
            return true;
        }

        // Friendly hauling is a separate capability from harvesting or theft.
        // The reviewed four village sources may supply only the city depot;
        // registration cannot replace any existing participant's permissions.
        bool RegisterCarrier(uint64 actor, uint32 ownDepotId = CityDepotId,
            uint32 cargoCapacity = CarrierCargoCapacity)
        {
            if (!actor || ownDepotId != CityDepotId || cargoCapacity != CarrierCargoCapacity ||
                !GetDepot(ownDepotId) || !GetDepot(1) || !GetDepot(4) || !GetDepot(5) || !GetDepot(6))
                return false;
            auto current = _actors.find(actor);
            if (current != _actors.end())
                return current->second.Carrier && current->second.DepotId == ownDepotId &&
                    current->second.Capacity == cargoCapacity && !current->second.Predator &&
                    !current->second.Consumer && !current->second.RaidSourceDepotId && !current->second.FieldId;
            if (_actors.size() >= 128) return false;
            Actor carrier{ownDepotId, cargoCapacity, false, false};
            carrier.Carrier = true;
            _actors.emplace(actor, std::move(carrier));
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
            if (owner.Consumer || owner.Carrier || owner.RaidSourceDepotId || owner.FieldId || owner.Predator != (kind == ClaimKind::PredatorMeal) ||
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
            for (auto& [_, field] : _fields)
                Regrow(field, nowMs);
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

        // Only an owned, physically completed work action can turn this
        // farmer's crop potential into cargo. Arrival alone does not harvest.
        Result HarvestField(uint64 actorId, uint64 eventSerial, uint32 fieldId,
            uint32 requested, uint64 nowMs, bool physicallyCompleted)
        {
            Expire(nowMs);
            auto actor = _actors.find(actorId);
            if (actor == _actors.end())
                return Rejected("UNKNOWN_ACTOR");
            auto& owner = actor->second;
            if (auto replay = ReplayEvent(owner, eventSerial, EventKind::FieldHarvest, fieldId, requested))
                return *replay;
            auto field = _fields.find(fieldId);
            if (!physicallyCompleted || !eventSerial || !requested || requested > owner.Capacity ||
                !owner.FieldId || owner.FieldId != fieldId || field == _fields.end() ||
                field->second.DepotId != owner.DepotId || owner.Predator || owner.Consumer || owner.Carrier || owner.RaidSourceDepotId)
                return Rejected("FIELD_HARVEST_NOT_AUTHORIZED");
            uint32 amount = std::min({requested, owner.Capacity - owner.Cargo, field->second.Remaining});
            if (!amount)
                return Rejected(owner.Cargo == owner.Capacity ? "CARGO_FULL" : "FIELD_EMPTY");
            field->second.Remaining -= amount;
            field->second.HarvestedTotal += amount;
            owner.Cargo += amount;
            _counters.HarvestedTotal += amount;
            return SaveEvent(owner, eventSerial, EventKind::FieldHarvest, fieldId, requested, amount);
        }

        // eventSerial is an owned completed-action identity, monotonically
        // increasing per actor across harvest, delivery, loading, theft and meals. Exact latest repeats
        // report the original receipt; altered or older repeats cannot mutate.
        Result Steal(uint64 actorId, uint64 eventSerial, uint32 sourceDepotId,
            uint32 requested, bool physicallyCompleted)
        {
            auto actor = _actors.find(actorId);
            if (actor == _actors.end()) return Rejected("UNKNOWN_ACTOR");
            auto& owner = actor->second;
            if (auto replay = ReplayEvent(owner, eventSerial, EventKind::Theft, sourceDepotId, requested))
                return *replay;
            auto source = _depots.find(sourceDepotId);
            if (!physicallyCompleted || !eventSerial || !requested || requested > RaiderCargoCapacity ||
                owner.Predator || owner.Consumer || owner.Carrier || !owner.RaidSourceDepotId ||
                (owner.RaidSourceDepotId != sourceDepotId && !(owner.AllVillageRaidSources &&
                    sourceDepotId >= 4 && sourceDepotId <= 6)) ||
                source == _depots.end())
                return Rejected("THEFT_NOT_AUTHORIZED");
            uint32 amount = std::min(requested, owner.Capacity - owner.Cargo);
            if (!amount) return Rejected("CARGO_FULL");
            // Recheck the complete debit at commit. Another consumer/theft
            // may have used these portions since the runtime began its action.
            if (source->second.Stock < amount) return Rejected("NO_FOOD");
            source->second.Stock -= amount;
            source->second.Stolen += amount;
            owner.Cargo += amount;
            _counters.StolenTotal += amount;
            return SaveEvent(owner, eventSerial, EventKind::Theft, sourceDepotId, requested, amount);
        }

        Result Load(uint64 actorId, uint64 eventSerial, uint32 sourceDepotId,
            uint32 requested, bool physicallyCompleted)
        {
            auto actor = _actors.find(actorId);
            if (actor == _actors.end()) return Rejected("UNKNOWN_ACTOR");
            auto& owner = actor->second;
            if (auto replay = ReplayEvent(owner, eventSerial, EventKind::Loading, sourceDepotId, requested))
                return *replay;
            auto source = _depots.find(sourceDepotId);
            if (!physicallyCompleted || !eventSerial || !requested || requested > CarrierCargoCapacity ||
                !owner.Carrier || owner.DepotId != CityDepotId || owner.Predator || owner.Consumer ||
                owner.RaidSourceDepotId || owner.FieldId || source == _depots.end() ||
                (sourceDepotId != 1 && sourceDepotId != 4 && sourceDepotId != 5 && sourceDepotId != 6))
                return Rejected("LOAD_NOT_AUTHORIZED");
            if (owner.Cargo >= owner.Capacity) return Rejected("CARGO_FULL");
            // Recheck actual stock after any intervening meal, theft or other
            // carrier's load. A village always keeps its configured reserve.
            uint32 available = source->second.Stock > source->second.Low ?
                source->second.Stock - source->second.Low : 0;
            uint32 amount = std::min({requested, owner.Capacity - owner.Cargo, available});
            if (!amount) return Rejected("VILLAGE_RESERVE");
            source->second.Stock -= amount;
            source->second.Exported += amount;
            owner.Cargo += amount;
            _counters.LoadedTotal += amount;
            return SaveEvent(owner, eventSerial, EventKind::Loading, sourceDepotId, requested, amount);
        }

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
            // Keep LastEventSerial so an old harvest can never mint new cargo.
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
        Field const* GetField(uint32 id) const
        { auto found = _fields.find(id); return found == _fields.end() ? nullptr : &found->second; }
        Counters const& GetCounters() const { return _counters; }
        uint64 DepotFood() const
        { uint64 sum = 0; for (auto const& [_, depot] : _depots) sum += depot.Stock; return sum; }
        uint64 CargoFood() const
        { uint64 sum = 0; for (auto const& [_, actor] : _actors) sum += actor.Cargo; return sum; }
        uint64 RemainingPotential() const
        {
            uint64 sum = 0;
            for (auto const& [_, carcass] : _carcasses) sum += carcass.Remaining;
            for (auto const& [_, field] : _fields) sum += field.Remaining;
            return sum;
        }
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
        enum class EventKind : uint8 { Delivery, Meal, Theft, FieldHarvest, Loading };
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
            uint32 RaidSourceDepotId = 0;
            uint32 FieldId = 0;
            uint32 Cargo = 0;
            std::optional<Claim> Pending;
            std::optional<std::pair<Token, Result>> LastCommitted;
            uint64 LastEventSerial = 0;
            std::optional<Event> LastEvent;
            bool AllVillageRaidSources = false;
            bool Carrier = false;
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
        void Regrow(Field& field, uint64 nowMs)
        {
            // A backwards clock never rewinds the growth anchor. Time spent
            // full cannot be banked and redeemed by a later harvest.
            if (nowMs <= field.LastGrowthAtMs)
                return;
            if (field.Remaining == field.Capacity)
            {
                field.LastGrowthAtMs = nowMs;
                return;
            }
            uint32 growth = uint32(std::min<uint64>((nowMs - field.LastGrowthAtMs) / field.RegrowMs,
                field.Capacity - field.Remaining));
            if (!growth)
                return;
            field.Remaining += growth;
            field.RegrownTotal += growth;
            _counters.PotentialCreated += growth;
            field.LastGrowthAtMs = field.Remaining == field.Capacity ? nowMs :
                field.LastGrowthAtMs + uint64(growth) * field.RegrowMs;
        }
        void Release(Claim const& claim)
        {
            if (auto carcass = FindCarcass(claim.Key))
                carcass->Reserved -= claim.Amount;
        }
        std::map<uint32, Depot> _depots;
        std::map<uint32, uint32> _initialDepotStock;
        std::map<uint32, Field> _fields;
        std::map<uint32, uint32> _initialFieldCrops;
        std::map<uint64, Actor> _actors;
        std::map<uint64, Carcass> _carcasses;
        uint64 _nextClaimSerial = 0;
        Counters _counters;
    };
}
#endif
