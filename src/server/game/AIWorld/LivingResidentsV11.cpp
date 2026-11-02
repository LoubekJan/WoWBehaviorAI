/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Agent/LivingResidentsV11Locations.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PointMovementGenerator.h"
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{
    using namespace LivingResidentsV11;
    using Phase = LivingRoleState::Phase;
    using Ambient = LivingRolePolicy::Activity;

    DepotSpec const* Location(uint32 id)
    {
        for (auto const& value : Locations::Depots) if (value.Id == id) return &value;
        return nullptr;
    }
    FieldSpec const* ResidentField(uint32 id)
    {
        for (auto const& value : Locations::Fields) if (value.Id == id) return &value;
        return nullptr;
    }
    bool OwnedGoal(AgentRecord const& record)
    {
        auto const& state = record.LivingRole;
        return record.ActiveGoalState && record.ActiveGoalState->Type == state.SourceGoal &&
            record.ActiveGoalState->StartedAtMs == state.StartedAtMs;
    }
    bool OwnedAction(AgentRecord const& record, ActionType type)
    {
        auto const& state = record.LivingRole;
        return OwnedGoal(record) && record.ActiveActionState && record.ActiveActionState->Type == type &&
            record.ActiveActionState->SourceGoal == state.SourceGoal &&
            record.ActiveActionState->GoalStartedAtMs == state.StartedAtMs &&
            record.ActiveActionState->StartedAtMs == state.StartedAtMs;
    }
    bool Bound(AgentRecord const& record, Creature const& creature)
    {
        return record.WorldState == AgentWorldState::Materialized && record.RuntimeGuid == creature.GetGUID() &&
            record.LivingRole.RuntimeGuid == creature.GetGUID() && creature.IsAlive() && creature.IsInWorld() &&
            creature.GetMapId() == record.MapId && creature.GetSpawnId() == record.SpawnId &&
            !creature.IsPet() && !creature.IsCharmed() && creature.GetCharmerOrOwnerGUID().IsEmpty() &&
            !creature.IsControlledByPlayer();
    }
    bool At(Creature const& creature, ActionPosition const& point)
    {
        return creature.GetMapId() == point.MapId && creature.GetExactDist(point.X, point.Y, point.Z) <= 1.9f &&
            creature.IsWithinLOS(point.X, point.Y, point.Z);
    }

}

void AIWorldMgr::InitializeLivingResidentsV11()
{
    _residentActors.clear();
    if (!_livingResidentsV11Enabled) return;
    bool valid = _enabled && _livingRolesEnabled && _livingRoleExtensionsEnabled &&
        _restrictAgentsToSimulationScope && LivingResidentsV11::ScopeValid(GetSimulationScope()) &&
        !_livingSupplyEnabled && !_livingConsumersEnabled && !_livingRaidsEnabled && !_livingFieldsEnabled &&
        !_livingDefiasEnabled && !_livingCaravansEnabled;
    _supplyLedger = {};
    _supplyActors.clear();
    for (auto const& depot : Locations::Depots)
        valid = _supplyLedger.ConfigureDepot(depot.Id, 0, depot.Id == 5 ? 120 : 30,
            depot.Id == 5 ? 48 : 12, depot.Id == 5 ? 12 : 4) && valid;
    valid = _supplyLedger.ConfigureResidentsV11() && valid;
    for (auto const& field : Locations::Fields)
        valid = _supplyLedger.ConfigureField(field.Id, GetCurrentTimeMs()) && valid;
    for (uint64 spawn = First; spawn <= Last; ++spawn)
    {
        auto role = Role(spawn);
        uint32 depot = Depot(spawn);
        if (role == Job::Farmer) valid = _supplyLedger.RegisterFarmer(spawn, depot, depot) && valid;
        else if (role == Job::Carrier) valid = _supplyLedger.RegisterCarrier(spawn, 5) && valid;
        else if (role == Job::Raider) valid = _supplyLedger.RegisterRaider(spawn, 6, 1, 2, true) && valid;
        else valid = _supplyLedger.RegisterActor(spawn, depot, 0, false, true) && valid;
        _supplyActors.emplace(spawn, LivingSupply::ActorRuntime{});
        _residentActors.emplace(spawn, State{});
    }
    if (!valid)
        throw std::runtime_error("Living residents v11 require the exact scoped 45-actor population, exclusive runtime and six verified depots");
    TC_LOG_INFO("ai.world", "Living residents v11 enabled: farmers=12 carriers=2 consumers=21 raiders=10 depots=6 initialFood=0");
}

bool AIWorldMgr::IsLivingResidentV11(AgentRecord const& record) const
{
    return record.Id.Value == record.SpawnId &&
        LivingResidentsV11::Capability(record.SpawnId, record.MapId, record.Type, record.ControlMode, _livingResidentsV11Enabled) &&
        GetSimulationScope().ContainsActor(record.MapId, record.SpawnId);
}

void AIWorldMgr::ObserveLivingResidentV11(AgentRecord const& record, Creature const* creature, uint64 nowMs)
{
    if (!IsLivingResidentV11(record)) return;
    auto found = _supplyActors.find(record.SpawnId);
    auto state = _residentActors.find(record.SpawnId);
    if (found == _supplyActors.end() || state == _residentActors.end()) return;
    auto& actor = found->second;
    bool matches = creature && creature->GetMapId() == 725 && creature->GetSpawnId() == record.SpawnId;
    bool live = matches && creature->IsAlive() && creature->IsInWorld();
    if (!live || !actor.Live || actor.Guid != creature->GetGUID())
    {
        actor.Lost += _supplyLedger.DropCargo(record.SpawnId);
        actor.Travel = {};
        actor.Phase = live ? "READY" : "UNAVAILABLE";
        actor.LastResult = live ? "MATERIALIZED" : "NOT_ALIVE";
        actor.NextAtMs = nowMs + 1000;
        state->second = {};
    }
    actor.Guid = matches ? creature->GetGUID() : ObjectGuid::Empty;
    actor.Live = live;
    _supplyLedger.Expire(nowMs);
}

bool AIWorldMgr::UpdateLivingResidentV11(AgentRecord& record, Creature& creature, uint64 nowMs,
    std::function<bool(ActionRequest, Phase, Unit*, bool)> const& start,
    std::function<std::optional<ActionPosition>(ActionPosition const&)> const& step)
{
    if (!IsLivingResidentV11(record) || !Bound(record, creature)) return false;
    auto found = _supplyActors.find(record.SpawnId);
    auto personal = _residentActors.find(record.SpawnId);
    if (found == _supplyActors.end() || personal == _residentActors.end()) return true;
    auto& actor = found->second;
    auto& trip = personal->second;
    auto& state = record.LivingRole;
    if (!actor.Live || actor.Guid != creature.GetGUID()) return true;
    auto job = Role(record.SpawnId);
    uint32 ownDepot = Depot(record.SpawnId);
    auto own = Location(ownDepot);
    auto depot = _supplyLedger.GetDepot(ownDepot);
    if (!own || !depot) return true;
    auto const& nativeHome = creature.GetHomePosition();
    ActionPosition home{725, nativeHome.GetPositionX(), nativeHome.GetPositionY(), nativeHome.GetPositionZ()};
    auto finish = [&](char const* phase, char const* result, uint64 delay)
    {
        StopLivingRole(record, creature);
        actor.Phase = phase; actor.LastResult = result;
        actor.NextAtMs = state.NextDecisionAtMs = nowMs + delay;
    };
    auto clearActivity = [&]() { trip.Pending = Activity::None; trip.ActionStartedAtMs = trip.NextWorkEmoteAtMs = 0; };
    bool hungry = record.Needs.Hunger >= .65f;
    bool journey = state.CurrentPhase == Phase::Moving && state.MovementPurpose &&
        std::string_view(state.MovementPurpose) == "RESIDENT_JOURNEY";
    if (journey)
    {
        auto* movement = dynamic_cast<PointMovementGenerator<Creature>*>(
            creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE));
        bool owned = state.SourceGoal == GoalType::LocalActivity && OwnedGoal(record) &&
            (!record.ActiveActionState || OwnedAction(record, ActionType::MoveTo));
        bool arrived = owned && creature.IsStopped() &&
            creature.GetExactDist(state.Destination.X, state.Destination.Y, state.Destination.Z) <= 2.0f;
        if (!arrived && owned && record.ActiveActionState && movement && movement->GetId() == ActionExecutor::MovePointId &&
            state.MoveWatchdog.Continue(nowMs, {725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()}, false))
            return true;
        if (arrived && actor.Travel.Pending && RecoveryMovement::SamePoint(actor.Travel.Pending->Destination, state.Destination))
        {
            if (!actor.Travel.ObserveSource({725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()}, true))
                actor.Travel = {};
        }
        else actor.Travel.RejectPending();
        finish(arrived ? "WAITING" : "NO_ROUTE", arrived ? "ARRIVED" : "MOVE_FAILED", arrived ? 0 : 10000);
        if (!arrived) return true;
    }
    if (trip.Pending != Activity::None)
    {
        auto activity = trip.Pending;
        Ambient expected = activity == Activity::Eat ? Ambient::Eat : activity == Activity::Rest ? Ambient::Rest : Ambient::Work;
        bool owned = state.CurrentPhase == Phase::Acting && state.SourceGoal == GoalType::LocalActivity &&
            OwnedAction(record, ActionType::Ambient) && state.Activity == expected &&
            state.StartedAtMs == trip.ActionStartedAtMs && creature.IsStopped() && !creature.IsInCombat() &&
            At(creature, trip.ActionPoint) &&
            (activity == Activity::Rest || creature.GetStandState() == UNIT_STAND_STATE_STAND);
        if (!owned || nowMs < trip.ActionStartedAtMs)
        {
            clearActivity(); finish("WAITING", "ACTIVITY_INTERRUPTED", 1000); return true;
        }
        if (nowMs - trip.ActionStartedAtMs < Duration(activity))
        {
            if (expected == Ambient::Work && nowMs >= trip.NextWorkEmoteAtMs)
            {
                creature.HandleEmoteCommand(EMOTE_ONESHOT_WORK_CHOPWOOD);
                trip.NextWorkEmoteAtMs = nowMs + 3000;
            }
            return true;
        }
        clearActivity();
        if (activity == Activity::Rest)
        {
            record.Needs.Fatigue = 0;
            trip.RestAtHome = trip.ReturningHome = false;
            trip.FieldCycle = (trip.FieldCycle + 1) % 3;
            finish("READY", "RESTED_HOME", 0);
            return true;
        }
        if (actor.EventSerial == std::numeric_limits<uint64>::max())
        { finish("WAITING", "EVENT_ID_EXHAUSTED", 60000); return true; }
        LivingSupply::Result result;
        uint64 serial = ++actor.EventSerial;
        if (activity == Activity::Work)
        {
            result = _supplyLedger.HarvestField(record.SpawnId, serial, ownDepot, 1, nowMs, true);
            if (result.Success && !result.Duplicate) actor.Harvested += result.Portions;
            if (!result.Success) trip.RestAtHome = true;
        }
        else if (activity == Activity::Deliver)
        {
            result = _supplyLedger.Deliver(record.SpawnId, serial, job == Job::Carrier ? trip.DestinationDepot : ownDepot,
                job == Job::Carrier ? 6 : 2, true);
            if (result.Success && !result.Duplicate)
            {
                actor.Delivered += result.Portions;
                if (!_supplyLedger.GetCargo(record.SpawnId))
                {
                    trip.RestAtHome = true;
                    trip.HomeViaCity = job == Job::Carrier;
                    trip.SourceDepot = 0;
                    ++trip.CompletedTrips;
                    if (job == Job::Raider) trip.NextTripAtMs = nowMs + 120000;
                }
            }
        }
        else if (activity == Activity::Eat)
        {
            uint32 mealDepot = job == Job::Carrier ? trip.DestinationDepot : ownDepot;
            result = _supplyLedger.EatDepot(record.SpawnId, serial, mealDepot, 1, true);
            if (result.Success && !result.Duplicate)
            {
                actor.Consumed += result.Portions;
                _needsSystem.SatisfyHunger(record.Needs);
                trip.ReturningHome = job == Job::Consumer;
            }
        }
        else if (activity == Activity::Load)
        {
            result = _supplyLedger.Load(record.SpawnId, serial, trip.SourceDepot, 6, true);
            if (result.Success && !result.Duplicate) trip.Loaded += result.Portions;
            if (!result.Success) { trip.SourceDepot = 0; trip.NextTripAtMs = nowMs + 15000; }
        }
        else if (activity == Activity::Steal)
        {
            result = _supplyLedger.Steal(record.SpawnId, serial, trip.SourceDepot, 2, true);
            if (result.Success && !result.Duplicate) { actor.Stolen += result.Portions; ++trip.Thefts; }
            if (!result.Success) { trip.SourceDepot = 0; trip.NextTripAtMs = nowMs + 15000; }
        }
        finish(result.Success ? "COMPLETED" : "WAITING", result.Success ?
            activity == Activity::Work ? "HARVESTED_FIELD" : activity == Activity::Deliver ? "DELIVERED" :
            activity == Activity::Eat ? "ATE_DEPOT" : activity == Activity::Load ? "LOADED" : "STOLEN" : result.Reason,
            result.Success ? 0 : 10000);
        return true;
    }
    // Safety and combat keep precedence. Replace only an owned peaceful action
    // when a physical job needs it; unrelated engine movement is left alone.
    bool wantsJob = job != Job::Consumer || hungry || trip.ReturningHome;
    if (!wantsJob) { actor.Phase = "ORDINARY"; return false; }
    if ((state.CurrentPhase == Phase::Moving || state.CurrentPhase == Phase::Acting) &&
        state.SourceGoal == GoalType::LocalActivity && OwnedGoal(record) &&
        (!record.ActiveActionState || OwnedAction(record, state.CurrentPhase == Phase::Moving ? ActionType::MoveTo : ActionType::Ambient)))
    {
        actor.Travel = {};
        finish("WAITING", "PEACEFUL_ACTIVITY_REPLACED", 0);
    }
    if (state.CurrentPhase != Phase::Idle) return true;
    if (nowMs < actor.NextAtMs || nowMs < state.NextDecisionAtMs) return true;

    auto travel = [&](ActionPosition const& destination, uint32 targetDepot, char const* phase)
    {
        if (!LivingReturnPolicy::SamePosition(trip.TravelGoal, destination))
        {
            uint32 nearest = 0;
            float distance = std::numeric_limits<float>::max();
            for (auto const& location : Locations::Depots)
            {
                float candidate = creature.GetExactDist(location.Point.X, location.Point.Y, location.Point.Z);
                if (candidate < distance) { nearest = location.Id; distance = candidate; }
            }
            RetargetJourney(trip, destination, targetDepot, nearest, Locations::Routes);
        }
        while (trip.Waypoint < trip.Waypoints.size() && At(creature, trip.Waypoints[trip.Waypoint])) ++trip.Waypoint;
        if (trip.Waypoint == trip.Waypoints.size()) return true;
        auto next = step(trip.Waypoints[trip.Waypoint]);
        if (!next)
        {
            bool deferred = state.Planning.Deferred;
            actor.Phase = deferred ? "WAITING" : "NO_ROUTE";
            actor.LastResult = deferred ? "PLANNING_DEFERRED" : "NO_ROUTE";
            actor.NextAtMs = state.NextDecisionAtMs = nowMs + (deferred ? 100 : 10000);
            if (deferred) state.MovementPurpose = "SUPPLY_PLANNING_DEFERRED";
            return true;
        }
        ActionRequest move;
        move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity; move.Destination = next;
        if (!start(move, Phase::Moving, nullptr, false))
        {
            actor.Travel.RejectPending();
            finish("WAITING", "MOVE_REJECTED", 10000); return true;
        }
        state.MovementPurpose = "RESIDENT_JOURNEY";
        actor.Phase = phase; actor.LastResult = "MOVING";
        return true;
    };
    auto activity = [&](Activity pending, ActionPosition const& point, char const* phase)
    {
        if (!At(creature, point) || !creature.IsStopped())
        { finish("WAITING", "ACTIVITY_POSITION_INVALID", 1000); return true; }
        ActionRequest request;
        request.Type = ActionType::Ambient; request.SourceGoal = GoalType::LocalActivity;
        request.AmbientActivity = pending == Activity::Eat ? Ambient::Eat : pending == Activity::Rest ? Ambient::Rest : Ambient::Work;
        if (start(request, Phase::Acting, nullptr, false))
        {
            trip.Pending = pending; trip.ActionPoint = point;
            trip.ActionStartedAtMs = state.StartedAtMs; trip.NextWorkEmoteAtMs = nowMs + 3000;
            actor.Phase = phase; actor.LastResult = "STARTED";
        }
        else finish("WAITING", "ACTIVITY_REJECTED", 1000);
        return true;
    };
    if (trip.ReturningHome || trip.RestAtHome)
    {
        // Finish a needed meal from the stock just physically delivered.
        // A hauler at the southern settlement need not return hungry to an
        // empty city while standing beside the food it has carried there.
        uint32 mealDepot = job == Job::Carrier ? trip.DestinationDepot : ownDepot;
        auto meal = Location(mealDepot);
        auto mealStock = _supplyLedger.GetDepot(mealDepot);
        if (trip.RestAtHome && hungry && meal && mealStock && mealStock->Stock && At(creature, meal->Point))
            return activity(Activity::Eat, meal->Point, "EATING_DEPOT");
        // The southern settlement is separated by the authored coast route.
        // Return through the proved city connection before the final local
        // home approach; a long direct corridor must not cut that bend.
        if (trip.HomeViaCity)
        {
            if (!At(creature, own->Point)) return travel(own->Point, ownDepot, "RETURNING_HOME");
            trip.HomeViaCity = false;
        }
        if (!At(creature, home)) return travel(home, 0, "RETURNING_HOME");
        if (trip.RestAtHome) return activity(Activity::Rest, home, "RESTING_HOME");
        trip.ReturningHome = false; actor.Phase = "ORDINARY"; return false;
    }
    uint32 cargo = _supplyLedger.GetCargo(record.SpawnId);
    if (job == Job::Farmer)
    {
        auto field = ResidentField(ownDepot);
        if (!field) return true;
        if (cargo >= 2 || (cargo && hungry) || (hungry && depot->Stock))
        {
            if (!At(creature, own->Point)) return travel(own->Point, ownDepot, "TO_WAREHOUSE");
            if (cargo && depot->Stock < depot->Capacity) return activity(Activity::Deliver, own->Point, "DELIVERING");
            if (hungry && depot->Stock) return activity(Activity::Eat, own->Point, "EATING_DEPOT");
            trip.RestAtHome = true; return true;
        }
        if (depot->Stock >= depot->Capacity)
        { trip.RestAtHome = true; return true; }
        auto const& point = field->WorkPoints[WorkPoint(record.SpawnId, trip.FieldCycle)];
        if (!At(creature, point)) return travel(point, 0, "TO_FIELD");
        return activity(Activity::Work, point, "WORKING_FIELD");
    }
    if (job == Job::Consumer)
    {
        if (!depot->Stock) { finish("NO_FOOD", "NO_FOOD", 15000); return true; }
        if (!At(creature, own->Point)) return travel(own->Point, ownDepot, "TO_DEPOT");
        return activity(Activity::Eat, own->Point, "EATING_DEPOT");
    }
    if (job == Job::Carrier && !trip.DestinationDepot)
    {
        auto south = _supplyLedger.GetDepot(4), city = _supplyLedger.GetDepot(5);
        trip.DestinationDepot = DeliveryDepot(south ? south->Stock : 0, city ? city->Stock : 0);
    }
    uint32 destinationId = job == Job::Raider ? 6 : trip.DestinationDepot;
    auto destination = Location(destinationId);
    auto destinationStock = _supplyLedger.GetDepot(destinationId);
    if (hungry && job == Job::Raider && depot->Stock)
    {
        if (!At(creature, own->Point)) return travel(own->Point, ownDepot, "TO_DEPOT");
        return activity(Activity::Eat, own->Point, "EATING_DEPOT");
    }
    if (cargo)
    {
        if (!destination || !destinationStock) { finish("WAITING", "NO_DESTINATION", 10000); return true; }
        if (!At(creature, destination->Point)) return travel(destination->Point, destinationId, "RETURNING_CARGO");
        if (destinationStock->Stock < destinationStock->Capacity)
            return activity(Activity::Deliver, destination->Point, "DELIVERING");
        if (job == Job::Carrier) trip.DestinationDepot = destinationId == 4 ? 5 : 4;
        else { trip.RestAtHome = true; trip.NextTripAtMs = nowMs + 15000; }
        return true;
    }
    if (hungry)
    {
        uint32 mealDepot = job == Job::Carrier ? 5 : 6;
        auto stock = _supplyLedger.GetDepot(mealDepot);
        if (stock && stock->Stock)
        {
            auto point = Location(mealDepot)->Point;
            if (!At(creature, point)) return travel(point, mealDepot, "TO_DEPOT");
            trip.DestinationDepot = mealDepot;
            return activity(Activity::Eat, point, "EATING_DEPOT");
        }
    }
    if (nowMs < trip.NextTripAtMs) { finish("COOLDOWN", "WAITING_TRIP", 10000); return true; }
    if (job == Job::Carrier)
    {
        auto south = _supplyLedger.GetDepot(4), city = _supplyLedger.GetDepot(5);
        trip.DestinationDepot = DeliveryDepot(south ? south->Stock : 0, city ? city->Stock : 0);
        if (!trip.DestinationDepot) { trip.RestAtHome = true; return true; }
    }
    if (!trip.SourceDepot)
    {
        std::array<uint32, 4> stocks{};
        for (uint32 id = 1; id <= 4; ++id) if (auto source = _supplyLedger.GetDepot(id)) stocks[id - 1] = source->Stock;
        auto selected = Source(stocks, trip.PreviousSource, job == Job::Raider);
        if (!selected) { finish("NO_FOOD", "NO_STOCKED_SOURCE", 15000); return true; }
        trip.SourceDepot = *selected; trip.PreviousSource = *selected;
        ++trip.Attempts;
    }
    auto source = Location(trip.SourceDepot);
    auto sourceStock = _supplyLedger.GetDepot(trip.SourceDepot);
    if (!source || !sourceStock)
    { trip.SourceDepot = 0; finish("WAITING", "SOURCE_UNAVAILABLE", 1000); return true; }
    if (!At(creature, source->Point)) return travel(source->Point, source->Id, "TO_SOURCE");
    // A trip reaches its chosen physical warehouse before learning that its
    // stock has gone. Retargeting at every coast waypoint would abandon the
    // only proved exit merely because a remote consumer ate a portion.
    if (sourceStock->Stock < (job == Job::Raider ? 2u : 5u))
    { trip.SourceDepot = 0; finish("WAITING", "SOURCE_EMPTY", 1000); return true; }
    bool result = activity(job == Job::Raider ? Activity::Steal : Activity::Load, source->Point,
        job == Job::Raider ? "STEALING" : "LOADING");
    return result;
}

std::optional<FoodDepotTelemetry> AIWorldMgr::DescribeResidentFoodDepot(uint32 id) const
{
    auto depot = _supplyLedger.GetDepot(id);
    auto location = Location(id);
    if (!_livingResidentsV11Enabled || !depot || !location) return std::nullopt;
    FoodDepotTelemetry result;
    result.Id = id; result.Name = location->Name; result.Position = location->Point;
    result.Stock = depot->Stock; result.Capacity = depot->Capacity; result.LowThreshold = depot->Low;
    result.Stolen = depot->Stolen; result.Exported = depot->Exported;
    result.Delivered = depot->Delivered;
    result.Consumed = depot->Consumed;
    return result;
}

std::optional<LivingSupplyTelemetry> AIWorldMgr::DescribeLivingResidentV11(Creature const& creature, AgentRecord const& record) const
{
    if (!IsLivingResidentV11(record) || creature.GetMapId() != 725 || creature.GetSpawnId() != record.SpawnId ||
        record.WorldState != AgentWorldState::Materialized || record.RuntimeGuid != creature.GetGUID()) return std::nullopt;
    auto found = _supplyActors.find(record.SpawnId);
    auto personal = _residentActors.find(record.SpawnId);
    if (found == _supplyActors.end() || personal == _residentActors.end() || found->second.Guid != creature.GetGUID()) return std::nullopt;
    auto const& actor = found->second;
    auto const& trip = personal->second;
    auto job = Role(record.SpawnId);
    LivingSupplyTelemetry info;
    info.Enabled = true; info.Role = RoleName(job); info.Phase = actor.Phase; info.LastResult = actor.LastResult;
    info.DepotId = Depot(record.SpawnId); info.Depot = DescribeResidentFoodDepot(info.DepotId);
    info.Cargo = _supplyLedger.GetCargo(record.SpawnId);
    info.Capacity = job == Job::Carrier ? 6 : job == Job::Farmer || job == Job::Raider ? 2 : 0;
    info.Harvested = actor.Harvested; info.Delivered = actor.Delivered; info.Consumed = actor.Consumed;
    info.Lost = actor.Lost; info.Stolen = actor.Stolen;
    if (job == Job::Farmer)
    {
        auto field = ResidentField(info.DepotId);
        auto crops = _supplyLedger.GetField(info.DepotId);
        info.FieldId = info.DepotId; info.FieldWorkpointIndex = WorkPoint(record.SpawnId, trip.FieldCycle);
        if (field) info.FieldWorkpoint = field->WorkPoints[*info.FieldWorkpointIndex];
        if (crops) { info.FieldRemaining = crops->Remaining; info.FieldCapacity = crops->Capacity; }
    }
    auto const& counts = _supplyLedger.GetCounters();
    FoodAccountingTelemetry accounting;
    accounting.InitialFood = counts.InitialFood; accounting.Harvested = counts.HarvestedTotal;
    accounting.InDepots = _supplyLedger.DepotFood(); accounting.InCargo = _supplyLedger.CargoFood();
    accounting.Consumed = counts.ConsumedTotal; accounting.Lost = counts.LostTotal;
    accounting.Conserved = _supplyLedger.Conserved(); info.Accounting = accounting;
    return info;
}

std::optional<LivingRaidTelemetry> AIWorldMgr::DescribeResidentRaid(Creature const& creature, AgentRecord const& record) const
{
    if (!IsLivingResidentV11(record) || Role(record.SpawnId) != Job::Raider ||
        record.WorldState != AgentWorldState::Materialized || record.RuntimeGuid != creature.GetGUID()) return std::nullopt;
    auto found = _supplyActors.find(record.SpawnId);
    auto state = _residentActors.find(record.SpawnId);
    if (found == _supplyActors.end() || state == _residentActors.end() || found->second.Guid != creature.GetGUID()) return std::nullopt;
    LivingRaidTelemetry result;
    result.Enabled = true; result.Role = "RAIDER"; result.Phase = found->second.Phase; result.Outcome = found->second.LastResult;
    result.AttemptId = state->second.Attempts; result.Thefts = state->second.Thefts; result.Delivered = found->second.Delivered;
    result.SourceDepotId = state->second.SourceDepot; result.CampDepotId = 6;
    result.SourceDepot = DescribeResidentFoodDepot(result.SourceDepotId); result.CampDepot = DescribeResidentFoodDepot(6);
    return result;
}
