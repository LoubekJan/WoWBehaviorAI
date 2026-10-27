/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Agent/LivingFieldRuntime.h"
#include "Creature.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PointMovementGenerator.h"
#include <limits>
#include <string_view>

namespace
{
    using Phase = LivingRoleState::Phase;
    using Activity = LivingRolePolicy::Activity;
    using FieldStage = LivingSupply::FieldStage;
    using FieldActivity = LivingSupply::FieldActivity;

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

    bool BoundActor(AgentRecord const& record, Creature const& creature)
    {
        return creature.GetMapId() == 725 && creature.GetSpawnId() == record.SpawnId &&
            record.WorldState == AgentWorldState::Materialized && record.RuntimeGuid == creature.GetGUID() &&
            record.LivingRole.RuntimeGuid == creature.GetGUID() && creature.IsAlive() &&
            creature.IsInWorld() && !creature.IsPet() && !creature.IsCharmed() &&
            creature.GetCharmerOrOwnerGUID().IsEmpty() && !creature.IsControlledByPlayer();
    }

    bool FieldMovement(char const* purpose)
    {
        std::string_view value(purpose);
        return value == "FARM_FIELD" || value == "FARM_DEPOT" || value == "FARM_HOME";
    }
}

bool AIWorldMgr::IsFieldWorker(AgentRecord const& record) const
{
    return LivingFields::Capability(record.SpawnId, record.MapId, record.Type, record.ControlMode,
        _livingSupplyEnabled && _livingFieldsEnabled) && record.Id.Value == record.SpawnId &&
        GetSimulationScope().ContainsActor(record.MapId, record.SpawnId);
}

bool AIWorldMgr::UpdateLivingFieldWorker(AgentRecord& record, Creature& creature, uint64 nowMs,
    std::function<bool(ActionRequest, Phase, Unit*, bool)> const& start,
    std::function<std::optional<ActionPosition>(ActionPosition const&)> const& step)
{
    if (!IsFieldWorker(record) || !BoundActor(record, creature)) return false;
    auto found = _supplyActors.find(record.SpawnId);
    auto worker = LivingFields::Worker(record.SpawnId);
    auto field = worker ? LivingFields::Field(worker->Field) : nullptr;
    if (found == _supplyActors.end() || !field) return true;
    auto& actor = found->second;
    auto& state = record.LivingRole;
    auto depot = _supplyLedger.GetDepot(worker->Depot);
    auto point = LivingFields::WorkPoint(*worker, actor.FieldCycle);
    if (!depot || !point || !actor.Live || actor.Guid != creature.GetGUID()) return true;
    auto const homePosition = creature.GetHomePosition();
    ActionPosition home{725, homePosition.GetPositionX(), homePosition.GetPositionY(), homePosition.GetPositionZ()};
    auto const& scope = GetSimulationScope();
    auto inScope = [&](ActionPosition const& position)
    {
        return position.MapId == creature.GetMapId() &&
            scope.ContainsActor(creature.GetMapId(), creature.GetSpawnId()) &&
            scope.Contains(position.MapId, creature.GetMap()->GetZoneId(creature.GetPhaseMask(), position.X, position.Y, position.Z),
                position.X, position.Y, position.Z) &&
            scope.Contains(creature.GetMapId(), creature.GetZoneId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ());
    };
    auto at = [&](ActionPosition const& position)
    {
        return inScope(position) && creature.GetExactDist(position.X, position.Y, position.Z) <= 3.0f &&
            creature.IsWithinLOS(position.X, position.Y, position.Z);
    };
    auto clearActivity = [&]()
    {
        actor.FarmingActivity = FieldActivity::None;
        actor.FieldActionStartedAtMs = actor.FieldNextWorkEmoteAtMs = 0;
    };
    // Shared safety owns attacks and escapes. Its replacement action cannot
    // inherit a partly completed work window or the previous movement proof.
    if (creature.IsInCombat() || (state.CurrentPhase != Phase::Idle && state.CurrentPhase != Phase::Moving &&
        state.CurrentPhase != Phase::Acting))
    {
        clearActivity();
        actor.Travel = {};
        return false;
    }
    auto finish = [&](char const* phase, char const* result, uint64 delay)
    {
        StopLivingRole(record, creature);
        clearActivity();
        actor.Phase = phase;
        actor.LastResult = result;
        actor.NextAtMs = nowMs + delay;
        state.NextDecisionAtMs = actor.NextAtMs;
    };
    if (state.CurrentPhase == Phase::Moving && FieldMovement(state.MovementPurpose))
    {
        auto* movement = dynamic_cast<PointMovementGenerator<Creature>*>(
            creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE));
        bool ownership = state.SourceGoal == GoalType::LocalActivity && OwnedGoal(record) &&
            (!record.ActiveActionState || OwnedAction(record, ActionType::MoveTo));
        bool arrived = ownership && creature.IsStopped() &&
            creature.GetExactDist(state.Destination.X, state.Destination.Y, state.Destination.Z) <= 2.0f;
        if (!arrived && ownership && record.ActiveActionState && movement && movement->GetId() == ActionExecutor::MovePointId &&
            state.MoveWatchdog.Continue(nowMs, {725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()}, false))
            return true;
        ActionPosition reached{725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
        if (arrived && actor.Travel.Pending && RecoveryMovement::SamePoint(actor.Travel.Pending->Destination, state.Destination))
        {
            if (!actor.Travel.ObserveSource(reached, true)) actor.Travel = {};
        }
        else actor.Travel.RejectPending();
        finish(arrived ? "WAITING" : "NO_ROUTE", arrived ? "ARRIVED" : "MOVE_FAILED", arrived ? 0 : 10000);
        if (!arrived) return true;
        // Continue a proved corridor immediately after this owned physical
        // arrival. A field activity still needs the final 3D/LOS predicate.
    }
    if (state.CurrentPhase == Phase::Acting && actor.FarmingActivity != FieldActivity::None)
    {
        FieldActivity activity = actor.FarmingActivity;
        bool work = activity == FieldActivity::Work;
        bool delivery = activity == FieldActivity::Deliver;
        bool eating = activity == FieldActivity::Eat;
        bool rest = activity == FieldActivity::Rest;
        Activity expected = rest ? Activity::Rest : eating ? Activity::Eat : Activity::Work;
        ActionPosition const& expectedPoint = work ? *point : rest ? home : field->DepotPoint;
        bool owned = state.SourceGoal == GoalType::LocalActivity && OwnedAction(record, ActionType::Ambient) &&
            state.StartedAtMs == actor.FieldActionStartedAtMs && state.Activity == expected &&
            LivingReturnPolicy::SamePosition(actor.FieldActionPoint, expectedPoint) && at(expectedPoint) &&
            creature.IsStopped() && !creature.IsInCombat() &&
            creature.GetStandState() == (rest ? state.OwnedStandState : UNIT_STAND_STATE_STAND);
        if (!owned || nowMs < state.StartedAtMs)
        {
            finish("WAITING", "ACTIVITY_INTERRUPTED", 10000);
            return true;
        }
        uint64 duration = work ? 15000 : rest ? 10000 : 5000;
        if (nowMs - state.StartedAtMs < duration)
        {
            // Ambient Work dispatches the real one-shot labor animation.
            // Repeat it during long windows only while that exact action owns
            // the stopped actor at its verified work point.
            if ((work || delivery) && nowMs >= actor.FieldNextWorkEmoteAtMs)
            {
                creature.HandleEmoteCommand(EMOTE_ONESHOT_WORK_CHOPWOOD);
                actor.FieldNextWorkEmoteAtMs = nowMs + 3000;
            }
            return true;
        }
        if (rest)
        {
            record.Needs.Fatigue = 0.0f;
            actor.FieldCycle = (actor.FieldCycle + 1) % 3;
            actor.FarmingStage = FieldStage::Field;
            finish("READY_FOR_FIELD", "RESTED_HOME", 0);
            return true;
        }
        if (actor.EventSerial == std::numeric_limits<uint64>::max())
        {
            finish("WAITING", "EVENT_ID_EXHAUSTED", 60000);
            return true;
        }
        LivingSupply::Result result;
        if (work)
        {
            // Full storage or held cargo turns the same visible job into
            // maintenance. It creates no food and never satisfies hunger.
            if (depot->Stock >= depot->Capacity || _supplyLedger.GetCargo(record.SpawnId) >= 2)
                result.Reason = "FIELD_MAINTENANCE";
            else
                result = _supplyLedger.HarvestField(record.SpawnId, ++actor.EventSerial, worker->Field, 1, nowMs, true);
            if (result.Success && !result.Duplicate) actor.Harvested += result.Portions;
            bool hungry = record.Needs.Hunger >= 0.65f;
            uint32 cargo = _supplyLedger.GetCargo(record.SpawnId);
            actor.FarmingStage = LivingSupply::FieldStageAfterWork(cargo, hungry, depot->Stock, depot->Capacity,
                result.Success && !result.Duplicate && result.Portions);
            finish(result.Success ? "HARVESTED_FIELD" : "FIELD_MAINTENANCE", result.Success ? "HARVESTED_FIELD" : result.Reason, 0);
            return true;
        }
        result = delivery ? _supplyLedger.Deliver(record.SpawnId, ++actor.EventSerial, worker->Depot, 2, true) :
            _supplyLedger.EatDepot(record.SpawnId, ++actor.EventSerial, worker->Depot, 1, true);
        if (result.Success && !result.Duplicate)
        {
            if (delivery) actor.Delivered += result.Portions;
            else
            {
                actor.Consumed += result.Portions;
                _needsSystem.SatisfyHunger(record.Needs);
            }
        }
        // Finish delivery and a needed stock meal at this warehouse before
        // returning home. A full warehouse keeps any undeposited cargo.
        actor.FarmingStage = LivingSupply::FieldStageAfterWarehouse(_supplyLedger.GetCargo(record.SpawnId),
            record.Needs.Hunger >= 0.65f, depot->Stock, depot->Capacity);
        finish("WAITING", result.Success ? delivery ? "DELIVERED" : "ATE_DEPOT" : result.Reason, 0);
        return true;
    }
    // A prior generic peaceful action has no field receipt. Replace only
    // this role's owned LocalActivity so it cannot run forever under the new
    // capability or fall through to personal food/reward completion.
    if ((state.CurrentPhase == Phase::Acting || state.CurrentPhase == Phase::Moving) &&
        state.SourceGoal == GoalType::LocalActivity && OwnedGoal(record) &&
        (!record.ActiveActionState ||
            OwnedAction(record, state.CurrentPhase == Phase::Moving ? ActionType::MoveTo : ActionType::Ambient)))
    {
        actor.Travel = {};
        finish("WAITING", "ACTIVITY_INTERRUPTED", 0);
    }
    // Leave a foreign active movement owner untouched.
    if (state.CurrentPhase != Phase::Idle) return true;
    if (actor.FarmingActivity != FieldActivity::None)
    {
        clearActivity();
        actor.LastResult = "ACTIVITY_INTERRUPTED";
    }
    if (nowMs < actor.NextAtMs || nowMs < state.NextDecisionAtMs) return true;
    auto travel = [&](ActionPosition const& destination, char const* phase, char const* purpose)
    {
        auto next = step(destination);
        if (!next && state.Planning.Deferred)
        {
            actor.Phase = "WAITING"; actor.LastResult = "PLANNING_DEFERRED";
            actor.NextAtMs = state.NextDecisionAtMs = nowMs + 100;
            state.MovementPurpose = "SUPPLY_PLANNING_DEFERRED";
            return true;
        }
        if (!next)
        {
            actor.Phase = "NO_ROUTE"; actor.LastResult = "NO_ROUTE";
            actor.NextAtMs = state.NextDecisionAtMs = nowMs + 10000;
            return true;
        }
        ActionRequest move;
        move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity; move.Destination = next;
        actor.LastResult = "MOVE_REJECTED";
        if (!start(move, Phase::Moving, nullptr, false))
        {
            if (actor.LastResult != "MOVEMENT_OWNER_BUSY" && actor.LastResult != "ACTOR_MOVEMENT_BUSY" &&
                actor.LastResult != "ACTOR_IN_COMBAT") actor.Travel.RejectPending();
            actor.Phase = "NO_ROUTE"; actor.NextAtMs = state.NextDecisionAtMs = nowMs + 10000;
            return true;
        }
        state.MovementPurpose = purpose;
        actor.Phase = phase; actor.LastResult = "MOVING";
        return true;
    };
    auto activity = [&](FieldActivity farmingActivity, Activity ambient, ActionPosition const& position, char const* phase)
    {
        if (!at(position) || !creature.IsStopped())
        {
            actor.Phase = "WAITING"; actor.LastResult = "ACTIVITY_POSITION_INVALID";
            actor.NextAtMs = state.NextDecisionAtMs = nowMs + 10000;
            return true;
        }
        ActionRequest request;
        request.Type = ActionType::Ambient; request.SourceGoal = GoalType::LocalActivity; request.AmbientActivity = ambient;
        if (start(request, Phase::Acting, nullptr, false))
        {
            actor.FarmingActivity = farmingActivity;
            actor.FieldActionStartedAtMs = state.StartedAtMs;
            actor.FieldActionPoint = position;
            actor.FieldNextWorkEmoteAtMs = nowMs + 3000;
            actor.Phase = phase; actor.LastResult = "STARTED";
        }
        else
        {
            actor.Phase = "WAITING"; actor.LastResult = "ACTIVITY_REJECTED";
            actor.NextAtMs = state.NextDecisionAtMs = nowMs + 10000;
        }
        return true;
    };
    if (actor.FarmingStage == FieldStage::Home)
    {
        if (!at(home)) return travel(home, "RETURNING_HOME", "FARM_HOME");
        return activity(FieldActivity::Rest, Activity::Rest, home, "RESTING_HOME");
    }
    bool hungry = record.Needs.Hunger >= 0.65f;
    uint32 cargo = _supplyLedger.GetCargo(record.SpawnId);
    if (actor.FarmingStage == FieldStage::Field && ((hungry && depot->Stock) ||
        (cargo && (cargo >= 2 || hungry) && depot->Stock < depot->Capacity)))
        actor.FarmingStage = FieldStage::Warehouse;
    if (actor.FarmingStage == FieldStage::Warehouse)
    {
        if (!at(field->DepotPoint)) return travel(field->DepotPoint, "TO_WAREHOUSE", "FARM_DEPOT");
        if (cargo && depot->Stock < depot->Capacity)
            return activity(FieldActivity::Deliver, Activity::Work, field->DepotPoint, "DELIVERING");
        if (hungry && depot->Stock)
            return activity(FieldActivity::Eat, Activity::Eat, field->DepotPoint, "EATING_DEPOT");
        actor.FarmingStage = hungry && !cargo ? FieldStage::Field : FieldStage::Home;
        if (actor.FarmingStage == FieldStage::Home)
        {
            if (!at(home)) return travel(home, "RETURNING_HOME", "FARM_HOME");
            return activity(FieldActivity::Rest, Activity::Rest, home, "RESTING_HOME");
        }
    }
    if (!at(*point)) return travel(*point, "TO_FIELD", "FARM_FIELD");
    return activity(FieldActivity::Work, Activity::Work, *point, "WORKING_FIELD");
}
