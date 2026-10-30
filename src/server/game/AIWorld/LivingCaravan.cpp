/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Creature.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PointMovementGenerator.h"
#include <algorithm>
#include <limits>
#include <string_view>

namespace
{
    using Phase = LivingRoleState::Phase;
    using Stage = LivingCaravan::Stage;
    using Activity = LivingCaravan::Activity;

    bool CaravanMovement(char const* purpose)
    {
        std::string_view value(purpose ? purpose : "");
        return value == "CARAVAN_RALLY" || value == "CARAVAN_VILLAGE" || value == "CARAVAN_CITY";
    }
    bool OwnedGoal(AgentRecord const& record)
    {
        auto const& role = record.LivingRole;
        return role.SourceGoal == GoalType::LocalActivity && record.ActiveGoalState &&
            record.ActiveGoalState->Type == GoalType::LocalActivity &&
            record.ActiveGoalState->StartedAtMs == role.StartedAtMs;
    }
    bool OwnedAction(AgentRecord const& record, ActionType type)
    {
        auto const& role = record.LivingRole;
        return OwnedGoal(record) && record.ActiveActionState && record.ActiveActionState->Type == type &&
            record.ActiveActionState->SourceGoal == GoalType::LocalActivity &&
            record.ActiveActionState->GoalStartedAtMs == role.StartedAtMs &&
            record.ActiveActionState->StartedAtMs == role.StartedAtMs;
    }
    bool Bound(AgentRecord const& record, Creature const& npc)
    {
        uint32 entry = record.SpawnId == 901040 ? 66 : record.SpawnId == 901041 ? 11328 : 1423;
        return npc.GetMapId() == LivingCaravan::MapId && npc.GetSpawnId() == record.SpawnId &&
            npc.GetEntry() == entry && npc.IsInWorld() && npc.IsAlive() && !npc.IsPet() && !npc.IsCharmed() &&
            npc.GetCharmerOrOwnerGUID().IsEmpty() && !npc.IsControlledByPlayer() &&
            record.WorldState == AgentWorldState::Materialized && record.RuntimeGuid == npc.GetGUID() &&
            record.LivingRole.RuntimeGuid == npc.GetGUID();
    }
    ActionPosition PositionOf(Creature const& npc)
    { return {npc.GetMapId(), npc.GetPositionX(), npc.GetPositionY(), npc.GetPositionZ()}; }
    bool At(Creature const& npc, ActionPosition const& point)
    {
        return LivingCaravan::AtWaypoint(PositionOf(npc), npc.IsStopped(),
            npc.IsWithinLOS(point.X, point.Y, point.Z), point);
    }
}

bool AIWorldMgr::IsCaravanMember(AgentRecord const& record) const
{
    return _livingSupplyEnabled && record.Id.Value == record.SpawnId && record.WorldFaction.Value == 1 &&
        GetSimulationScope().ContainsActor(record.MapId, record.SpawnId) &&
        (LivingCaravan::CarrierCapability(record.SpawnId, record.MapId, record.Type, record.ControlMode, _livingCaravansEnabled) ||
         LivingCaravan::EscortCapability(record.SpawnId, record.MapId, record.Type, record.ControlMode, _livingCaravansEnabled));
}

bool AIWorldMgr::IsCityConsumer(AgentRecord const& record) const
{
    return _livingSupplyEnabled && _livingCaravansEnabled && record.Id.Value == record.SpawnId &&
        record.MapId == LivingCaravan::MapId && record.Type == AgentType::Civilian &&
        record.ControlMode == AgentControlMode::AIWorldControlled && record.WorldFaction.Value == 1 &&
        LivingCaravan::CityConsumer(record.SpawnId) && GetSimulationScope().ContainsActor(record.MapId, record.SpawnId);
}

void AIWorldMgr::ObserveCaravanLife(AgentRecord const& record, uint64 nowMs)
{
    auto personal = _caravanActors.find(record.SpawnId);
    auto supply = _supplyActors.find(record.SpawnId);
    auto pair = LivingCaravan::FindBySpawn(record.SpawnId);
    if (!pair || personal == _caravanActors.end() || supply == _supplyActors.end()) return;
    auto& actor = personal->second;
    bool live = supply->second.Live && !supply->second.Guid.IsEmpty();
    if (live && (!actor.Live || actor.Guid != supply->second.Guid))
    {
        // A genuine new life also invalidates coordination when the engine
        // reuses the same runtime GUID. Never reset this session watermark.
        if (actor.Life != std::numeric_limits<uint64>::max()) ++actor.Life;
        else live = false;
    }
    actor.Live = live;
    actor.Guid = live ? supply->second.Guid : ObjectGuid::Empty;
    actor.Current = Activity::None; actor.StartedAtMs = actor.TripStamp = actor.SourceStamp = 0;
    actor.RecoveryWaypoint.reset(); actor.RecoverySource = 0;
    auto shared = _caravans.find(pair->Id);
    if (shared != _caravans.end())
    {
        shared->second.Ready[*LivingCaravan::MemberIndex(record.SpawnId)] = false;
        shared->second.Reason = live ? "MEMBER_MATERIALIZED" : "MEMBER_LOST";
    }
    (void)nowMs;
}

bool AIWorldMgr::UpdateLivingCaravan(AgentRecord& record, Creature& creature, uint64 nowMs,
    std::function<bool(ActionRequest, Phase, Unit*, bool)> const& start,
    std::function<std::optional<ActionPosition>(ActionPosition const&)> const& step)
{
    if (!IsCaravanMember(record) || !Bound(record, creature)) return false;
    auto pair = LivingCaravan::FindBySpawn(record.SpawnId);
    auto shared = _caravans.find(pair->Id);
    auto personal = _caravanActors.find(record.SpawnId);
    auto supply = _supplyActors.find(record.SpawnId);
    if (shared == _caravans.end() || personal == _caravanActors.end() || supply == _supplyActors.end()) return true;
    auto& trip = shared->second;
    auto& actor = personal->second;
    auto& food = supply->second;
    auto& role = record.LivingRole;
    size_t index = *LivingCaravan::MemberIndex(record.SpawnId);
    bool carrier = index == 0;
    if (!actor.Live || actor.Guid != creature.GetGUID() || !food.Live || food.Guid != creature.GetGUID()) return true;

    struct Member
    {
        AgentRecord* Record = nullptr;
        Creature* Npc = nullptr;
        LivingCaravan::ActorState* Personal = nullptr;
        bool Bound = false, Original = false, Peaceful = false;
    };
    std::array<Member, 2> members;
    std::array<uint64, 2> spawns{pair->CarrierSpawn, pair->EscortSpawn};
    auto const& scope = GetSimulationScope();
    auto inScope = [&](Creature const& npc)
    {
        return scope.Contains(npc.GetMapId(), npc.GetZoneId(), npc.GetPositionX(), npc.GetPositionY(), npc.GetPositionZ()) &&
            scope.ContainsActor(npc.GetMapId(), npc.GetSpawnId());
    };
    for (size_t i = 0; i < members.size(); ++i)
    {
        auto& member = members[i];
        member.Record = _registry.FindBySpawn(LivingCaravan::MapId, spawns[i]);
        member.Npc = creature.GetMap()->GetCreatureBySpawnId(spawns[i]);
        auto state = _caravanActors.find(spawns[i]);
        auto ledgerActor = _supplyActors.find(spawns[i]);
        if (state == _caravanActors.end() || ledgerActor == _supplyActors.end() || !member.Record || !member.Npc) continue;
        member.Personal = &state->second;
        member.Bound = IsCaravanMember(*member.Record) && Bound(*member.Record, *member.Npc) && inScope(*member.Npc) &&
            state->second.Live && state->second.Life && state->second.Guid == member.Npc->GetGUID() &&
            ledgerActor->second.Live && ledgerActor->second.Guid == member.Npc->GetGUID();
        member.Original = member.Bound && trip.Guids[i] == member.Npc->GetGUID() && trip.Lives[i] == state->second.Life;
        auto phase = member.Record->LivingRole.CurrentPhase;
        member.Peaceful = member.Bound && !member.Npc->IsInCombat() && !member.Npc->IsInEvadeMode() &&
            (phase == Phase::Idle || phase == Phase::Moving || phase == Phase::Acting);
    }
    auto clearActivity = [&](LivingCaravan::ActorState& state)
    { state.Current = Activity::None; state.StartedAtMs = state.TripStamp = state.SourceStamp = 0; };
    auto stopOwned = [&](Member& member)
    {
        if (!member.Bound) return;
        auto& state = member.Record->LivingRole;
        bool ownMovement = state.CurrentPhase == Phase::Moving && CaravanMovement(state.MovementPurpose) &&
            OwnedGoal(*member.Record) && (!member.Record->ActiveActionState || OwnedAction(*member.Record, ActionType::MoveTo));
        bool ownActivity = member.Personal->Current != Activity::None && state.CurrentPhase == Phase::Acting &&
            state.StartedAtMs == member.Personal->StartedAtMs && OwnedAction(*member.Record, ActionType::Ambient);
        if (ownMovement || ownActivity) StopLivingRole(*member.Record, *member.Npc);
        clearActivity(*member.Personal);
        _supplyActors.at(member.Record->SpawnId).Travel = {};
    };
    auto cancelPair = [&]() { for (auto& member : members) stopOwned(member); };
    auto phase = [&](Stage next, char const* reason)
    {
        cancelPair();
        LivingCaravan::SetPhase(trip, next, nowMs, reason);
    };
    auto returnToCity = [&](char const* reason)
    {
        auto const& route = LivingCaravan::Route(trip.SourceDepotId);
        if (trip.Current == Stage::ToVillage && trip.Waypoint) --trip.Waypoint;
        else if (trip.Current == Stage::Loading) trip.Waypoint = route.size() > 1 ? uint32(route.size()-2) : 0;
        else if (trip.Current != Stage::ToCity) trip.Waypoint = 0;
        trip.ReturningSolo = !members[0].Original || !members[1].Original;
        phase(Stage::ToCity, reason);
    };
    auto finish = [&](char const* result, uint64 delay)
    {
        if (OwnedGoal(record) && (!record.ActiveActionState ||
            OwnedAction(record, role.CurrentPhase == Phase::Moving ? ActionType::MoveTo : ActionType::Ambient)))
            StopLivingRole(record, creature);
        clearActivity(actor);
        food.Phase = LivingCaravan::ToString(trip.Current); food.LastResult = result;
        food.NextAtMs = role.NextDecisionAtMs = nowMs + delay;
    };

    // Threat/defense/flee handling precedes this hook. Never stop those owners
    // or let a pre-combat loading window survive an interruption.
    if (!members[index].Peaceful)
    {
        clearActivity(actor); food.Travel = {}; trip.Ready[index] = false;
        return false;
    }

    if (!trip.Trip)
    {
        if (members[0].Peaceful && members[1].Peaceful && _nextCaravanTrip != std::numeric_limits<uint64>::max() &&
            LivingCaravan::Begin(trip, *pair, nowMs, ++_nextCaravanTrip, members[0].Npc->GetGUID(), members[1].Npc->GetGUID()))
        {
            trip.Lives = {members[0].Personal->Life, members[1].Personal->Life};
            members[0].Original = members[1].Original = true;
        }
        else { finish("WAITING_FOR_PARTNER", 2000); return true; }
    }

    bool bothOriginal = members[0].Original && members[1].Original;
    LivingCaravan::RefreshReturnMode(trip, members[0].Original, members[1].Original);
    if (!bothOriginal && trip.Current != Stage::ToCity && trip.Current != Stage::Unloading &&
        trip.Current != Stage::Rest && trip.Current != Stage::Recover)
        returnToCity("MEMBER_LOST_RETURNING");
    if (!members[0].Original && !members[1].Original && trip.Current != Stage::Recover)
    {
        phase(Stage::Recover, "REASSEMBLE_NEW_LIVES_AT_CITY");
        trip.Waypoint = 0;
    }
    if (trip.Current == Stage::Recover && (members[0].Original || members[1].Original))
    {
        // A temporary role/ownership interruption can invalidate both bindings
        // without killing the original carrier or losing its real cargo.
        // Recover along the route, retaining those original life stamps.
        auto const& recovery = LivingCaravan::Route(trip.SourceDepotId);
        auto const& npc = *members[members[0].Original ? 0 : 1].Npc;
        if (!recovery.empty())
        {
            auto closest = std::min_element(recovery.begin(), recovery.end(), [&](auto const& a, auto const& b)
                { return LivingReturnPolicy::Distance(PositionOf(npc), a.Point) < LivingReturnPolicy::Distance(PositionOf(npc), b.Point); });
            trip.Waypoint = uint32(closest - recovery.begin());
            trip.ReturningSolo = !bothOriginal;
            phase(Stage::ToCity, "ORIGINAL_LIFE_RECOVERED");
        }
    }
    if (bothOriginal && (!members[0].Peaceful || !members[1].Peaceful))
    {
        if (trip.Current != Stage::ToCity) returnToCity("DANGER_RETURNING");
        stopOwned(members[index]); trip.Ready[index] = false;
        food.Phase = "WAITING_FOR_ESCORT"; food.LastResult = "PARTNER_IN_DANGER";
        food.NextAtMs = role.NextDecisionAtMs = nowMs + 2000;
        return true;
    }
    if (LivingCaravan::TimedOut(trip, nowMs) && trip.Current != Stage::ToCity && trip.Current != Stage::Recover &&
        trip.Current != Stage::Unloading)
        returnToCity("TRIP_OR_LEG_TIMEOUT");

    // Inspect a continuing, still-owned real movement before changing the
    // shared waypoint. Completion alone cannot authorize food or a barrier.
    if (role.CurrentPhase == Phase::Moving && CaravanMovement(role.MovementPurpose))
    {
        auto* movement = dynamic_cast<PointMovementGenerator<Creature>*>(
            creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE));
        bool own = OwnedGoal(record) && (!record.ActiveActionState || OwnedAction(record, ActionType::MoveTo));
        bool arrived = own && creature.IsStopped() &&
            creature.GetExactDist(role.Destination.X, role.Destination.Y, role.Destination.Z) <= 2.0f;
        if (!arrived && own && record.ActiveActionState && movement && movement->GetId() == ActionExecutor::MovePointId &&
            role.MoveWatchdog.Continue(nowMs, PositionOf(creature), false)) return true;
        if (arrived && food.Travel.Pending && RecoveryMovement::SamePoint(food.Travel.Pending->Destination, role.Destination))
        {
            if (!food.Travel.ObserveSource(PositionOf(creature), true)) food.Travel = {};
        }
        else food.Travel.RejectPending();
        finish(arrived ? "ARRIVED_LEG" : "MOVE_INTERRUPTED", arrived ? 0 : 3000);
        if (!arrived) { trip.Ready[index] = false; return true; }
    }

    auto const& route = LivingCaravan::Route(trip.SourceDepotId);
    if (route.empty() || trip.Waypoint >= route.size())
    { finish("INVALID_ROUTE", 10000); return true; }

    if (role.CurrentPhase == Phase::Acting && actor.Current != Activity::None)
    {
        bool loading = actor.Current == Activity::Load;
        bool unloading = actor.Current == Activity::Unload;
        bool eating = actor.Current == Activity::Eat;
        auto sourcePoint = LivingCaravan::SourcePoint(actor.SourceStamp);
        ActionPosition expected = loading && sourcePoint ? *sourcePoint : LivingCaravan::CityPoint;
        auto expectedActivity = eating ? LivingRolePolicy::Activity::Eat : LivingRolePolicy::Activity::Work;
        bool owned = OwnedAction(record, ActionType::Ambient) && role.StartedAtMs == actor.StartedAtMs &&
            actor.TripStamp == trip.Trip && actor.SourceStamp == trip.SourceDepotId &&
            LivingReturnPolicy::SamePosition(actor.ActivityPoint, expected) && role.Activity == expectedActivity &&
            At(creature, expected) && creature.GetStandState() == UNIT_STAND_STATE_STAND &&
            ((loading && carrier && trip.Current == Stage::Loading && bothOriginal && members[1].Peaceful &&
                At(*members[1].Npc, expected)) ||
             (unloading && carrier && trip.Current == Stage::Unloading) ||
             (eating && (trip.Current == Stage::Rest || trip.Current == Stage::Unloading)));
        if (!owned || nowMs < actor.StartedAtMs)
        { finish("ACTIVITY_INTERRUPTED", 2000); return true; }
        if (nowMs - actor.StartedAtMs < LivingCaravan::LoadMs) return true;
        if (food.EventSerial == std::numeric_limits<uint64>::max())
        { finish("EVENT_ID_EXHAUSTED", 60000); return true; }
        LivingSupply::Result receipt = loading ? _supplyLedger.Load(record.SpawnId, ++food.EventSerial,
            trip.SourceDepotId, LivingCaravan::CargoCapacity, true) : unloading ?
            _supplyLedger.Deliver(record.SpawnId, ++food.EventSerial, LivingCaravan::CityDepot, LivingCaravan::CargoCapacity, true) :
            _supplyLedger.EatDepot(record.SpawnId, ++food.EventSerial, LivingCaravan::CityDepot, 1, true);
        if (receipt.Success && !receipt.Duplicate)
        {
            if (loading) actor.Loaded += receipt.Portions;
            else if (unloading) food.Delivered += receipt.Portions;
            else { food.Consumed += receipt.Portions; _needsSystem.SatisfyHunger(record.Needs); }
        }
        finish(receipt.Success ? loading ? "LOADED_FOOD" : unloading ? "DELIVERED_CITY" : "ATE_CITY_FOOD" : receipt.Reason, 0);
        if (loading)
            returnToCity(receipt.Success ? "FOOD_ON_BOARD" : "SOURCE_HAS_NO_SURPLUS");
        else if (unloading && !_supplyLedger.GetCargo(record.SpawnId))
        {
            if (receipt.Success && !receipt.Duplicate) ++trip.CompletedTrips;
            trip.Waypoint = 0; phase(Stage::Rest, "DELIVERY_FINISHED"); trip.NextAtMs = nowMs + LivingCaravan::RestMs;
        }
        return true;
    }

    // Replace only this actor's ordinary peaceful LocalActivity. Foreign
    // goals/actions and the native emergency movement retain ownership.
    if ((role.CurrentPhase == Phase::Moving || role.CurrentPhase == Phase::Acting) && OwnedGoal(record) &&
        (!record.ActiveActionState || OwnedAction(record, role.CurrentPhase == Phase::Moving ? ActionType::MoveTo : ActionType::Ambient)))
    { food.Travel = {}; finish("CARAVAN_TAKES_OWN_ROLE", 0); }
    if (role.CurrentPhase != Phase::Idle || record.ActiveActionState ||
        (record.ActiveGoalState && !OwnedGoal(record))) return true;
    clearActivity(actor);
    if (nowMs < food.NextAtMs || nowMs < role.NextDecisionAtMs) return true;

    auto travel = [&](ActionPosition const& destination, char const* purpose)
    {
        trip.Ready[index] = false;
        auto next = step(destination);
        if (!next)
        {
            bool deferred = role.Planning.Deferred;
            food.Phase = deferred ? "WAITING" : "NO_ROUTE";
            food.LastResult = deferred ? "PLANNING_DEFERRED" : "NO_ROUTE";
            food.NextAtMs = role.NextDecisionAtMs = nowMs + (deferred ? 100 : 10000);
            if (deferred) role.MovementPurpose = "SUPPLY_PLANNING_DEFERRED";
            return true;
        }
        ActionRequest request;
        request.Type = ActionType::MoveTo; request.SourceGoal = GoalType::LocalActivity; request.Destination = next;
        food.LastResult = "MOVE_REJECTED";
        if (start(request, Phase::Moving, nullptr, false))
        {
            role.MovementPurpose = purpose; food.Phase = LivingCaravan::ToString(trip.Current); food.LastResult = "MOVING";
        }
        else
        {
            if (food.LastResult != "MOVEMENT_OWNER_BUSY" && food.LastResult != "ACTOR_MOVEMENT_BUSY" && food.LastResult != "ACTOR_IN_COMBAT")
                food.Travel.RejectPending();
            food.NextAtMs = role.NextDecisionAtMs = nowMs + 3000;
        }
        return true;
    };
    auto activity = [&](Activity kind, ActionPosition const& point)
    {
        if (!At(creature, point)) return true;
        ActionRequest request;
        request.Type = ActionType::Ambient; request.SourceGoal = GoalType::LocalActivity;
        request.AmbientActivity = kind == Activity::Eat ? LivingRolePolicy::Activity::Eat : LivingRolePolicy::Activity::Work;
        if (start(request, Phase::Acting, nullptr, false))
        {
            actor.Current = kind; actor.StartedAtMs = role.StartedAtMs; actor.TripStamp = trip.Trip;
            actor.SourceStamp = trip.SourceDepotId; actor.ActivityPoint = point;
            food.Phase = kind == Activity::Load ? "LOADING" : kind == Activity::Unload ? "UNLOADING" : "EATING_CITY";
            food.LastResult = "STARTED"; trip.Ready[index] = false;
        }
        else { food.LastResult = "ACTIVITY_REJECTED"; food.NextAtMs = role.NextDecisionAtMs = nowMs + 3000; }
        return true;
    };
    auto readyAt = [&](size_t i, LivingCaravan::Waypoint const& waypoint)
    {
        auto const& member = members[i];
        auto target = LivingCaravan::Target(*pair, spawns[i], waypoint);
        return member.Original && member.Peaceful && target && At(*member.Npc, *target) &&
            member.Record->LivingRole.CurrentPhase == Phase::Idle && !member.Record->ActiveActionState;
    };

    // Reincarnated actors first reach the city independently; they never
    // inherit a dead member's in-flight route or claim their old cargo.
    if (!members[index].Original || trip.Current == Stage::Recover)
    {
        if (!At(creature, LivingCaravan::CityPoint))
        {
            // Grid reload may materialize a new binding on the far riverbank.
            // It returns independently via the same proved bridge hints;
            // this cursor is never readiness for the old shared expedition.
            if (!actor.RecoveryWaypoint || actor.RecoverySource != trip.SourceDepotId)
            {
                auto closest = std::min_element(route.begin(), route.end(), [&](auto const& a, auto const& b)
                    { return LivingReturnPolicy::Distance(PositionOf(creature), a.Point) < LivingReturnPolicy::Distance(PositionOf(creature), b.Point); });
                actor.RecoveryWaypoint = uint32(closest - route.begin()); actor.RecoverySource = trip.SourceDepotId;
            }
            if (*actor.RecoveryWaypoint >= route.size()) actor.RecoveryWaypoint = 0;
            if (At(creature, route[*actor.RecoveryWaypoint].Point) && *actor.RecoveryWaypoint) --*actor.RecoveryWaypoint;
            return travel(route[*actor.RecoveryWaypoint].Point, "CARAVAN_RALLY");
        }
        actor.RecoveryWaypoint.reset(); actor.RecoverySource = 0;
        bool allAtCity = members[0].Peaceful && members[1].Peaceful &&
            At(*members[0].Npc, LivingCaravan::CityPoint) && At(*members[1].Npc, LivingCaravan::CityPoint) &&
            members[0].Record->LivingRole.CurrentPhase == Phase::Idle && members[1].Record->LivingRole.CurrentPhase == Phase::Idle &&
            !members[0].Record->ActiveActionState && !members[1].Record->ActiveActionState;
        if (allAtCity && nowMs >= trip.NextAtMs && !_supplyLedger.GetCargo(pair->CarrierSpawn) && _nextCaravanTrip != std::numeric_limits<uint64>::max() &&
            LivingCaravan::Begin(trip, *pair, nowMs, ++_nextCaravanTrip, members[0].Npc->GetGUID(), members[1].Npc->GetGUID()))
            trip.Lives = {members[0].Personal->Life, members[1].Personal->Life};
        else { food.Phase = "WAITING_FOR_PARTNER"; food.LastResult = "REASSEMBLING_AT_CITY"; }
        role.NextDecisionAtMs = nowMs + 1000;
        return true;
    }

    bool cityPhase = trip.Current == Stage::Rally || trip.Current == Stage::Rest || trip.Current == Stage::Unloading;
    LivingCaravan::Waypoint waypoint = cityPhase ? route.front() : route[trip.Waypoint];
    auto target = LivingCaravan::Target(*pair, record.SpawnId, waypoint);
    if (!target) { finish("INVALID_WAYPOINT", 10000); return true; }
    if (!At(creature, *target)) return travel(*target,
        cityPhase ? "CARAVAN_RALLY" : trip.Current == Stage::ToVillage ? "CARAVAN_VILLAGE" : "CARAVAN_CITY");
    for (size_t i = 0; i < members.size(); ++i) trip.Ready[i] = readyAt(i, waypoint);
    food.Phase = LivingCaravan::ToString(trip.Current); food.LastResult = "WAITING_FOR_PARTNER";

    if (trip.Current == Stage::Rally)
    {
        if (LivingCaravan::ReadyBarrier(trip))
        { trip.Waypoint = 1; phase(Stage::ToVillage, "DEPARTING_CITY"); }
    }
    else if (trip.Current == Stage::ToVillage)
    {
        if (LivingCaravan::ReadyBarrier(trip))
        {
            if (trip.Waypoint + 1 < route.size()) { ++trip.Waypoint; trip.Ready = {}; trip.LastProgressAtMs = nowMs; }
            else phase(Stage::Loading, "ARRIVED_VILLAGE");
        }
    }
    else if (trip.Current == Stage::Loading)
    {
        if (carrier && LivingCaravan::ReadyBarrier(trip)) return activity(Activity::Load, *target);
    }
    else if (trip.Current == Stage::ToCity)
    {
        bool arrived = trip.ReturningSolo ? trip.Ready[index] : LivingCaravan::ReadyBarrier(trip);
        if (arrived)
        {
            if (trip.Waypoint) { --trip.Waypoint; trip.Ready = {}; trip.LastProgressAtMs = nowMs; }
            else if (carrier && _supplyLedger.GetCargo(record.SpawnId)) phase(Stage::Unloading, "ARRIVED_CITY_WITH_FOOD");
            else if (!_supplyLedger.GetCargo(pair->CarrierSpawn))
            { phase(Stage::Rest, "EMPTY_RETURN_FINISHED"); trip.NextAtMs = nowMs + LivingCaravan::RestMs; }
        }
    }
    else if (trip.Current == Stage::Unloading || trip.Current == Stage::Rest)
    {
        auto city = _supplyLedger.GetDepot(LivingCaravan::CityDepot);
        if (city && record.Needs.Hunger >= .65f && city->Stock) return activity(Activity::Eat, LivingCaravan::CityPoint);
        if (trip.Current == Stage::Unloading && carrier && _supplyLedger.GetCargo(record.SpawnId))
        {
            if (city && city->Stock < city->Capacity) return activity(Activity::Unload, LivingCaravan::CityPoint);
            food.LastResult = "CITY_DEPOT_FULL_CARGO_RETAINED";
        }
        else if (trip.Current == Stage::Rest && nowMs >= trip.NextAtMs && LivingCaravan::ReadyBarrier(trip) &&
            !_supplyLedger.GetCargo(pair->CarrierSpawn) && _nextCaravanTrip != std::numeric_limits<uint64>::max() &&
            LivingCaravan::Begin(trip, *pair, nowMs, ++_nextCaravanTrip, members[0].Npc->GetGUID(), members[1].Npc->GetGUID()))
            trip.Lives = {members[0].Personal->Life, members[1].Personal->Life};
    }
    role.NextDecisionAtMs = nowMs + 500;
    return true;
}
