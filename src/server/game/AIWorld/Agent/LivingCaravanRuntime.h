/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGCARAVANRUNTIME_H
#define AIWORLD_LIVINGCARAVANRUNTIME_H

#include "AgentType.h"
#include "GroupMemberFormation.h"
#include "LivingFieldRuntime.h"
#include "LivingReturnPolicy.h"
#include "LivingSupplyRuntime.h"
#include "ObjectGuid.h"
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// Session-only coordination over the four authored caravan actors. This
// layer never moves an actor, changes a home, cancels a foreign action, or
// commits food. Runtime must supply current bound incarnations and recheck
// the actual ground/body/LOS of every issued leg.
namespace LivingCaravan
{
    constexpr uint32 MapId = 725, CityDepot = 7;
    constexpr uint32 CityCapacity = 120, CityTarget = 60, CityLow = 12;
    constexpr uint32 CargoCapacity = 6, Reserve = 4;
    constexpr uint64 LoadMs = 5000, UnloadMs = 5000, RestMs = 45000;
    constexpr uint64 RallyTimeoutMs = 180000, LegTimeoutMs = 180000, TripTimeoutMs = 1800000;
    constexpr float HomeRadius = 900.0f, FormationRadius = 1.5f;
    // Outdoor, existing grounded guard-901000 home. It is a delivery anchor,
    // not a replacement home for any member of either caravan.
    inline constexpr ActionPosition CityPoint{MapId, 78.84711682419382f, 591.7054193209677f, .15f};

    struct PairSpec
    {
        uint32 Id;
        uint64 CarrierSpawn;
        AgentType CarrierType;
        uint64 EscortSpawn;
        std::array<uint32, 2> Sources;
    };
    inline constexpr std::array<PairSpec, 2> Pairs{{
        {1, 901040, AgentType::Merchant, 901042, {4, 5}},
        {2, 901041, AgentType::Civilian, 901043, {6, 1}}
    }};
    inline constexpr std::array<LivingSupply::ConsumerSpec, 2> CityConsumers{{
        {901014, CityDepot, AgentType::Civilian}, {901015, CityDepot, AgentType::Civilian}
    }};
    inline PairSpec const* Pair(uint32 id)
    {
        for (auto const& pair : Pairs) if (pair.Id == id) return &pair;
        return nullptr;
    }
    inline PairSpec const* FindBySpawn(uint64 spawn)
    {
        for (auto const& pair : Pairs)
            if (pair.CarrierSpawn == spawn || pair.EscortSpawn == spawn) return &pair;
        return nullptr;
    }
    inline std::optional<size_t> MemberIndex(uint64 spawn)
    {
        auto pair = FindBySpawn(spawn);
        if (!pair) return std::nullopt;
        return pair->CarrierSpawn == spawn ? 0 : 1;
    }
    inline bool IsCarrier(uint64 spawn)
    {
        auto pair = FindBySpawn(spawn);
        return pair && pair->CarrierSpawn == spawn;
    }
    inline LivingSupply::ConsumerSpec const* CityConsumer(uint64 spawn)
    {
        for (auto const& spec : CityConsumers) if (spec.Spawn == spawn) return &spec;
        return nullptr;
    }
    inline bool CarrierCapability(uint64 spawn, uint32 map, AgentType type, AgentControlMode control, bool enabled)
    {
        auto pair = FindBySpawn(spawn);
        return enabled && map == MapId && pair && pair->CarrierSpawn == spawn && pair->CarrierType == type &&
            control == AgentControlMode::AIWorldControlled;
    }
    inline bool EscortCapability(uint64 spawn, uint32 map, AgentType type, AgentControlMode control, bool enabled)
    {
        auto pair = FindBySpawn(spawn);
        return enabled && map == MapId && pair && pair->EscortSpawn == spawn && type == AgentType::Guard &&
            control == AgentControlMode::AIWorldControlled;
    }
    inline uint32 SelectSource(PairSpec const& pair, uint32 previous)
    {
        // Fair fixed rotation is an order, not omniscient remote stock memory.
        return previous == pair.Sources[0] ? pair.Sources[1] : pair.Sources[0];
    }
    inline bool OwnSource(PairSpec const& pair, uint32 source)
    {
        return source == pair.Sources[0] || source == pair.Sources[1];
    }
    inline ActionPosition const* SourcePoint(uint32 depot)
    {
        if (depot == 1) return &LivingSupply::Depots[0].Point;
        auto field = LivingFields::Field(depot);
        return field ? &field->DepotPoint : nullptr;
    }

    struct Waypoint { ActionPosition Point; bool SingleFile = false; };
    inline std::vector<Waypoint> const& Route(uint32 depot)
    {
        // Hints divide long journeys into bounded searches. Their actual
        // execution always retains live collision, surface and body proof.
        // Endpoint anchors are shared; staging offsets never authorize food.
        static std::vector<Waypoint> const top{
            {CityPoint, true}, {{MapId,160,620,.15f}}, {{MapId,260,640,.15f}},
            {{MapId,362.66656494140625f,661.333251953125f,.15f}}, {LivingFields::Fields[0].DepotPoint,true}};
        static std::vector<Waypoint> const left{
            {CityPoint,true}, {{MapId,63.999980926513672f,661.333251953125f,.15f}},
            {{MapId,58,740,.15f}}, {{MapId,44,820,.15f}}, {LivingFields::Fields[1].DepotPoint,true}};
        static std::vector<Waypoint> const right{
            {CityPoint,true}, {{MapId,85.33331298828125f,597.33331298828125f,.15f}},
            {{MapId,95.999969482421875f,539.73333740234375f,.48f}},
            {{MapId,106.66663360595703f,426.66653442382812f,.15f}},
            {{MapId,132,350,.15f}}, {LivingFields::Fields[2].DepotPoint,true}};
        static std::vector<Waypoint> const south{
            {CityPoint,true}, {{MapId,63.999980926513672f,661.333251953125f,.15f}},
            {{MapId,40,740,.15f}}, {{MapId,-20,790,.15f}}, {{MapId,-80,840,.15f}},
            {{MapId,-160,860,.15f},true},
            {{MapId,-201.6000671387f,861.0665283203f,-2.2201099396f},true},
            {{MapId,-214,861.0665283203f,.15f},true},
            {{MapId,-235,790,-.62f}}, {{MapId,-235,725,.15f}},
            {{MapId,-264,680,.15f}}, {{MapId,-340,580,.15f}}, {LivingSupply::Depots[0].Point,true}};
        static std::vector<Waypoint> const empty;
        switch (depot) { case 4: return top; case 5: return left; case 6: return right; case 1: return south; default: return empty; }
    }
    inline bool ValidRoute(uint32 depot)
    {
        auto const& route = Route(depot);
        auto source = SourcePoint(depot);
        if (!source || route.size() < 2 || route.size() > 20 ||
            !LivingReturnPolicy::SamePosition(route.front().Point, CityPoint) ||
            !LivingReturnPolicy::SamePosition(route.back().Point, *source)) return false;
        for (size_t i = 0; i < route.size(); ++i)
            if (!LivingReturnPolicy::Finite(route[i].Point) || route[i].Point.MapId != MapId ||
                (i && LivingReturnPolicy::Distance(route[i-1].Point, route[i].Point) > 150.0f)) return false;
        return true;
    }
    inline std::optional<ActionPosition> Target(PairSpec const& pair, uint64 spawn, Waypoint const& waypoint)
    {
        if ((spawn != pair.CarrierSpawn && spawn != pair.EscortSpawn) ||
            !LivingReturnPolicy::Finite(waypoint.Point) || waypoint.Point.MapId != MapId) return std::nullopt;
        if (waypoint.SingleFile) return waypoint.Point;
        auto slot = GroupMemberFormation::GetSlot(AgentId{spawn},
            std::vector<AgentId>{AgentId{pair.CarrierSpawn},AgentId{pair.EscortSpawn}},FormationRadius);
        if (!slot) return std::nullopt;
        return ActionPosition{MapId,waypoint.Point.X+slot->X,waypoint.Point.Y+slot->Y,waypoint.Point.Z};
    }
    inline bool AtWaypoint(ActionPosition const& position, bool stopped, bool lineOfSight,
        ActionPosition const& target, float arrivalRadius = 1.9f)
    {
        return stopped && lineOfSight && LivingReturnPolicy::Finite(position) && LivingReturnPolicy::Finite(target) &&
            position.MapId == MapId && target.MapId == MapId && std::isfinite(arrivalRadius) &&
            arrivalRadius > 0 && arrivalRadius <= 3 && LivingReturnPolicy::Distance(position,target) <= arrivalRadius;
    }

    enum class Stage : uint8 { Rally, ToVillage, Loading, ToCity, Unloading, Rest, Recover };
    inline char const* ToString(Stage stage)
    {
        switch (stage)
        {
            case Stage::Rally: return "RALLY"; case Stage::ToVillage: return "TO_VILLAGE";
            case Stage::Loading: return "LOADING"; case Stage::ToCity: return "TO_CITY";
            case Stage::Unloading: return "UNLOADING"; case Stage::Rest: return "REST";
            case Stage::Recover: return "RECOVER"; default: return "UNKNOWN";
        }
    }
    struct State
    {
        Stage Current = Stage::Rally;
        uint64 Trip = 0, StartedAtMs = 0, PhaseStartedAtMs = 0, LastProgressAtMs = 0, NextAtMs = 0, CompletedTrips = 0;
        uint32 SourceDepotId = 0, Waypoint = 0, SourceCursor = 0;
        std::array<ObjectGuid,2> Guids;
        std::array<uint64,2> Lives{};
        std::array<bool,2> Ready{};
        bool ReturningSolo = false;
        std::string Reason = "NOT_STARTED";
    };
    enum class Activity : uint8 { None, Load, Unload, Eat };
    struct ActorState
    {
        ObjectGuid Guid;
        uint64 Life = 0;
        bool Live = false;
        Activity Current = Activity::None;
        uint64 StartedAtMs = 0, TripStamp = 0, Loaded = 0, CompletedTrips = 0;
        uint32 SourceStamp = 0;
        ActionPosition ActivityPoint;
        std::optional<uint32> RecoveryWaypoint;
        uint32 RecoverySource = 0;
    };
    inline bool Elapsed(uint64 now, uint64 since, uint64 limit)
    { return since && now >= since && now-since >= limit; }
    inline void SetPhase(State& state, Stage phase, uint64 now, char const* reason)
    {
        state.Current = phase; state.PhaseStartedAtMs = state.LastProgressAtMs = now;
        state.Ready = {}; state.Reason = reason;
    }
    inline bool BoundLives(State const& state, std::array<ObjectGuid,2> const& guids)
    {
        return !state.Guids[0].IsEmpty() && !state.Guids[1].IsEmpty() && state.Guids == guids;
    }
    inline bool BoundLives(State const& state, std::array<ObjectGuid,2> const& guids,
        std::array<uint64,2> const& lives)
    {
        // Creature GUIDs can be reused on an in-place respawn. The live
        // observer's monotonically increasing incarnation counters must
        // also match before the pair can borrow an earlier trip's orders.
        return BoundLives(state,guids) && state.Lives[0] && state.Lives[1] && state.Lives == lives;
    }
    inline bool ReadyBarrier(State const& state)
    { return !state.ReturningSolo && !state.Guids[0].IsEmpty() && !state.Guids[1].IsEmpty() && state.Ready[0] && state.Ready[1]; }
    inline void RefreshReturnMode(State& state, bool carrierOriginal, bool escortOriginal)
    {
        // A missing original member must not hold an already-returning
        // survivor at an impossible two-member barrier. Once released, the
        // trip never recruits a respawned replacement; only Begin may restore
        // paired travel after a real city reunion and fresh incarnation bind.
        if (state.Current == Stage::ToCity && (!carrierOriginal || !escortOriginal)) state.ReturningSolo = true;
    }
    inline bool TimedOut(State const& state, uint64 now)
    {
        if (state.Current == Stage::Rest) return false;
        return Elapsed(now,state.StartedAtMs,TripTimeoutMs) ||
            Elapsed(now,state.LastProgressAtMs,state.Current == Stage::Rally ? RallyTimeoutMs : LegTimeoutMs);
    }
    inline bool Begin(State& state, PairSpec const& pair, uint64 now, uint64 trip,
        ObjectGuid carrier, ObjectGuid escort)
    {
        auto registered = Pair(pair.Id);
        if (!registered || registered->CarrierSpawn != pair.CarrierSpawn || registered->EscortSpawn != pair.EscortSpawn ||
            registered->CarrierType != pair.CarrierType || registered->Sources != pair.Sources ||
            !now || !trip || trip <= state.Trip || carrier.IsEmpty() || escort.IsEmpty() || carrier == escort) return false;
        uint32 source = SelectSource(pair,state.SourceDepotId);
        if (!ValidRoute(source)) return false;
        state.Trip = trip; state.StartedAtMs = now; state.NextAtMs = now;
        state.SourceDepotId = source; state.SourceCursor = source == pair.Sources[1] ? 1 : 0;
        state.Waypoint = 0; state.Guids = {carrier,escort}; state.Lives = {}; state.ReturningSolo = false;
        SetPhase(state,Stage::Rally,now,"ASSEMBLING_AT_CITY");
        return true;
    }
    inline void ResetForLife(State& state, uint64 now, char const* reason)
    {
        // Neither a death nor a new incarnation can reset the fair rotation
        // or lend its readiness to an old trip. Historical counters survive.
        state.Guids = {}; state.Lives = {}; state.Ready = {}; state.ReturningSolo = false;
        state.Waypoint = 0; state.NextAtMs = now;
        SetPhase(state,Stage::Recover,now,reason);
    }
}
#endif
