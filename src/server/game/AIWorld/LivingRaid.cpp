/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Creature.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PointMovementGenerator.h"
#include "Agent/LivingFieldRuntime.h"
#include <algorithm>
#include <limits>
#include <string_view>

namespace
{
    using Phase = LivingRoleState::Phase;
    using Activity = LivingRolePolicy::Activity;
    bool OwnedGoal(AgentRecord const& record)
    {
        return record.ActiveGoalState && record.ActiveGoalState->Type == record.LivingRole.SourceGoal &&
            record.ActiveGoalState->StartedAtMs == record.LivingRole.StartedAtMs;
    }
    bool OwnedAction(AgentRecord const& record, ActionType type)
    {
        auto const& state = record.LivingRole;
        return OwnedGoal(record) && record.ActiveActionState && record.ActiveActionState->Type == type &&
            record.ActiveActionState->SourceGoal == state.SourceGoal &&
            record.ActiveActionState->GoalStartedAtMs == state.StartedAtMs &&
            record.ActiveActionState->StartedAtMs == state.StartedAtMs;
    }
    bool Bound(AgentRecord const& record, Creature const& creature, bool alive = true)
    {
        return creature.GetMapId() == 725 && creature.GetSpawnId() == record.SpawnId &&
            record.WorldState == AgentWorldState::Materialized && record.RuntimeGuid == creature.GetGUID() &&
            (!alive || (creature.IsAlive() && record.LivingRole.RuntimeGuid == creature.GetGUID())) &&
            creature.IsInWorld() && !creature.IsPet() && !creature.IsCharmed() &&
            creature.GetCharmerOrOwnerGUID().IsEmpty() && !creature.IsControlledByPlayer();
    }
    bool Near(Creature const& creature, ActionPosition const& point)
    {
        return creature.GetMapId() == point.MapId && creature.GetExactDist(point.X, point.Y, point.Z) <= 3.0f &&
            creature.IsWithinLOS(point.X, point.Y, point.Z);
    }
    bool RaidMovement(char const* purpose)
    {
        std::string_view p(purpose);
        return p == "RAID_SOURCE" || p == "RAID_CAMP" || p == "RAID_EMPTY_RETURN" ||
            p == "GUARD_HOME" || p == "GUARD_POST";
    }
}

bool AIWorldMgr::IsSupplyRaider(AgentRecord const& record) const
{
    return LivingRaid::RaiderCapability(record.SpawnId, record.MapId, record.Type, record.ControlMode,
        _livingSupplyEnabled && _livingConsumersEnabled && _livingRaidsEnabled) &&
        record.Id.Value == record.SpawnId && GetSimulationScope().ContainsActor(record.MapId, record.SpawnId);
}

bool AIWorldMgr::IsRaidGuard(AgentRecord const& record) const
{
    return LivingRaid::GuardCapability(record.SpawnId, record.MapId, record.Type, record.ControlMode,
        _livingSupplyEnabled && _livingConsumersEnabled && _livingRaidsEnabled) &&
        record.Id.Value == record.SpawnId && GetSimulationScope().ContainsActor(record.MapId, record.SpawnId);
}

std::optional<FoodDepotTelemetry> AIWorldMgr::DescribeFoodDepot(uint32 id) const
{
    auto depot = _supplyLedger.GetDepot(id);
    auto location = LivingSupply::DepotLocation(id);
    auto field = _livingFieldsEnabled ? LivingFields::Field(id) : nullptr;
    if (!depot || (!location && !field && (id != LivingRaid::CampDepot || !_livingRaidsEnabled))) return std::nullopt;
    FoodDepotTelemetry result;
    result.Id = id;
    result.Name = location ? location->Name : field ? field->Name : "Defias camp";
    result.Position = location ? location->Point : field ? field->DepotPoint : LivingRaid::Camp;
    result.Stock = depot->Stock; result.Capacity = depot->Capacity; result.LowThreshold = depot->Low;
    for (auto const& [spawn, actor] : _supplyActors)
    {
        auto hunter = LivingSupply::Hunter(spawn);
        auto consumer = LivingSupply::Consumer(spawn);
        auto farmer = _livingFieldsEnabled ? LivingFields::Worker(spawn) : nullptr;
        uint32 owner = hunter ? hunter->Depot : consumer ? consumer->Depot :
            farmer ? farmer->Depot :
            spawn == LivingRaid::GuardSpawn ? LivingRaid::SourceDepot :
            spawn == LivingRaid::RaiderSpawn ? LivingRaid::CampDepot : 0;
        if (owner == id) { result.Delivered += actor.Delivered; result.Consumed += actor.Consumed; }
        if (id == LivingRaid::SourceDepot && spawn == LivingRaid::RaiderSpawn) result.Stolen += actor.Stolen;
    }
    return result;
}

std::optional<LivingRaidTelemetry> AIWorldMgr::DescribeLivingRaid(Creature const& creature, AgentRecord const& record) const
{
    bool raider = IsSupplyRaider(record);
    if ((!raider && !IsRaidGuard(record)) || !Bound(record, creature, false)) return std::nullopt;
    auto const& raid = raider ? _raidRaider : _raidGuard;
    if (raid.Guid != creature.GetGUID()) return std::nullopt;
    LivingRaidTelemetry result;
    result.Enabled = true; result.Role = raider ? "RAIDER" : "GUARD";
    result.Phase = raid.Phase; result.Outcome = raid.Outcome;
    result.AttemptId = raid.AttemptId;
    result.TargetSpawnId = raider ? LivingRaid::GuardSpawn : LivingRaid::RaiderSpawn;
    result.Noticed = raid.Noticed; result.Interceptions = raid.Interceptions;
    result.Thefts = raid.Thefts; result.Delivered = raid.Delivered; result.Prevented = raid.Prevented;
    result.SourceDepotId = LivingRaid::SourceDepot; result.CampDepotId = LivingRaid::CampDepot;
    result.SourceDepot = DescribeFoodDepot(result.SourceDepotId); result.CampDepot = DescribeFoodDepot(result.CampDepotId);
    return result;
}

Unit* AIWorldMgr::LivingRaidThreat(AgentRecord& record, Creature& creature, uint64 nowMs)
{
    bool raider = IsSupplyRaider(record), guard = IsRaidGuard(record);
    if ((!raider && !guard) || !Bound(record, creature)) return nullptr;
    uint64 otherSpawn = raider ? LivingRaid::GuardSpawn : LivingRaid::RaiderSpawn;
    Creature* other = creature.GetMap()->GetCreatureBySpawnId(otherSpawn);
    AgentRecord const* peer = _registry.FindBySpawn(725, otherSpawn);
    bool visible = other && peer && Bound(*peer, *other) &&
        creature.IsWithinDistInMap(other, guard ? 14.0f : 30.0f) && creature.CanSeeOrDetect(other) &&
        creature.IsWithinLOSInMap(other) && creature.IsValidAttackTarget(other);
    if (guard)
    {
        if (!visible || !IsSupplyRaider(*peer) || !_raidRaider.Active ||
            other->GetExactDist(LivingRaid::Source.X, LivingRaid::Source.Y, LivingRaid::Source.Z) > 45.0f)
            return nullptr;
        auto& raid = _raidGuard;
        raid.AttemptId = _raidRaider.AttemptId;
        raid.Phase = "INTERCEPTING";
        if (raid.LastNoticedAttempt != raid.AttemptId)
        {
            raid.LastNoticedAttempt = raid.AttemptId;
            ++raid.Noticed;
            raid.Outcome = "NOTICED_INTRUDER";
        }
        return other;
    }
    // A guard must actually own an attack against this incarnation. Its
    // patrol timer or remote state alone never informs the thief.
    bool guardAttack = visible && IsRaidGuard(*peer) && _raidGuard.AttemptId == _raidRaider.AttemptId &&
        peer->LivingRole.CurrentPhase == Phase::Defending && peer->LivingRole.TargetGuid == creature.GetGUID() &&
        OwnedAction(*peer, ActionType::Attack) && peer->ActiveActionState->Target &&
        peer->ActiveActionState->Target->Guid == creature.GetGUID();
    if (_raidRaider.Active && (guardAttack || creature.IsInCombat()))
    {
        auto& raid = _raidRaider;
        auto& actor = _supplyActors.at(record.SpawnId);
        bool beforeTheft = actor.Stolen == raid.StolenAtStart;
        if (beforeTheft) ++raid.Prevented;
        raid.Active = false; raid.Returning = true;
        raid.Leg = std::min<uint32>(raid.Leg, LivingRaid::ToSource.size()-1);
        raid.Phase = "FLEEING";
        raid.Outcome = guardAttack ? beforeTheft ? "INTERCEPTED" : "CAUGHT_WITH_CARGO" : "INTERRUPTED_COMBAT";
        actor.Travel.RejectPending();
        actor.NextAtMs = nowMs;
        StopLivingRole(record, creature);
    }
    return guardAttack ? other : nullptr;
}

bool AIWorldMgr::UpdateLivingRaid(AgentRecord& record, Creature& creature, uint64 nowMs,
    std::function<bool(ActionRequest, Phase, Unit*, bool)> const& start,
    std::function<std::optional<ActionPosition>(ActionPosition const&)> const& step)
{
    bool raider = IsSupplyRaider(record), guard = IsRaidGuard(record);
    if ((!raider && !guard) || !Bound(record, creature)) return false;
    auto found = _supplyActors.find(record.SpawnId);
    if (found == _supplyActors.end()) return false;
    auto& actor = found->second;
    auto& state = record.LivingRole;
    auto& raid = raider ? _raidRaider : _raidGuard;
    auto finish = [&](char const* result, uint64 delay)
    {
        StopLivingRole(record, creature);
        actor.LastResult = result;
        actor.NextAtMs = nowMs + delay;
        state.NextDecisionAtMs = actor.NextAtMs;
    };
    if (state.CurrentPhase == Phase::Moving && RaidMovement(state.MovementPurpose))
    {
        auto* point = dynamic_cast<PointMovementGenerator<Creature>*>(creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE));
        bool ownership = state.SourceGoal == GoalType::LocalActivity && OwnedGoal(record) &&
            (!record.ActiveActionState || OwnedAction(record, ActionType::MoveTo));
        bool arrived = ownership && creature.IsStopped() &&
            creature.GetExactDist(state.Destination.X, state.Destination.Y, state.Destination.Z) <= 2.0f;
        if (!arrived && ownership && record.ActiveActionState && point && point->GetId() == ActionExecutor::MovePointId &&
            state.MoveWatchdog.Continue(nowMs, {725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()}, false)) return true;
        ActionPosition reached{725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
        if (arrived && actor.Travel.Pending && RecoveryMovement::SamePoint(actor.Travel.Pending->Destination, state.Destination))
        { if (!actor.Travel.ObserveSource(reached, true)) actor.Travel = {}; }
        else actor.Travel.RejectPending();
        finish(arrived ? "ARRIVED" : "MOVE_FAILED", arrived ? 0 : 10000);
        if (!arrived) return true;
    }
    // Guard meals retain the existing physical depot consumer chain.
    if (guard)
    {
        if (record.Needs.Hunger >= 0.65f || actor.ReturningToHome || std::string_view(actor.Phase) == "EATING_DEPOT" ||
            (state.CurrentPhase == Phase::Moving && (std::string_view(state.MovementPurpose) == "GET_DEPOT_FOOD" ||
            std::string_view(state.MovementPurpose) == "RETURN_CONSUMER_HOME"))) return false;
        if (state.CurrentPhase == Phase::Acting)
        {
            if (OwnedAction(record, ActionType::Ambient) && nowMs >= state.StartedAtMs && nowMs-state.StartedAtMs < 5000) return true;
            finish("DUTY_OBSERVED", 1000);
            return true;
        }
    }
    else if (state.CurrentPhase == Phase::Acting)
    {
        bool theft = raid.Phase == "STEALING", delivery = raid.Phase == "DELIVERING", meal = raid.Phase == "EATING_CAMP";
        if (!theft && !delivery && !meal) { finish("ACTIVITY_INTERRUPTED", 1000); return true; }
        auto const& depot = theft ? LivingRaid::Source : LivingRaid::Camp;
        bool owned = state.SourceGoal == GoalType::LocalActivity && OwnedAction(record, ActionType::Ambient) &&
            state.Activity == (meal ? Activity::Eat : Activity::Work) && Near(creature, depot) && creature.IsStopped() &&
            !creature.IsInCombat() && creature.GetStandState() == UNIT_STAND_STATE_STAND;
        if (!owned || nowMs < state.StartedAtMs)
        {
            finish("ACTIVITY_INTERRUPTED", 1000);
            if (theft) { raid.Active = false; raid.Returning = true; raid.Leg = LivingRaid::ToSource.size()-1; raid.Outcome = "ACTIVITY_INTERRUPTED"; }
            raid.Phase = raid.Returning ? "RETURNING_EMPTY" : "COOLDOWN";
            return true;
        }
        if (nowMs-state.StartedAtMs < 5000) return true;
        if (actor.EventSerial == std::numeric_limits<uint64>::max()) { finish("EVENT_ID_EXHAUSTED", 60000); return true; }
        auto result = theft ? _supplyLedger.Steal(record.SpawnId, ++actor.EventSerial, LivingRaid::SourceDepot, LivingRaid::Portions, true) :
            delivery ? _supplyLedger.Deliver(record.SpawnId, ++actor.EventSerial, LivingRaid::CampDepot, LivingRaid::Portions, true) :
            _supplyLedger.EatDepot(record.SpawnId, ++actor.EventSerial, LivingRaid::CampDepot, 1, true);
        if (result.Success && !result.Duplicate)
        {
            if (theft)
            {
                actor.Stolen += result.Portions; ++raid.Thefts;
                raid.Returning = true; raid.Leg = LivingRaid::ToSource.size()-1;
                raid.Phase = "TO_CAMP"; raid.Outcome = "STOLEN";
            }
            else if (delivery)
            {
                actor.Delivered += result.Portions; ++raid.Delivered;
                raid.Returning = _supplyLedger.GetCargo(record.SpawnId) != 0;
                raid.Active = raid.Returning;
                raid.Phase = raid.Returning ? "TO_CAMP" : "COOLDOWN";
                raid.Outcome = "DELIVERED"; raid.NextAtMs = nowMs+LivingRaid::CooldownMs;
            }
            else { actor.Consumed += result.Portions; _needsSystem.SatisfyHunger(record.Needs); raid.Phase = "COOLDOWN"; }
        }
        else if (theft)
        {
            raid.Active = false; raid.Returning = true; raid.Leg = LivingRaid::ToSource.size()-1;
            raid.Phase = "RETURNING_EMPTY"; raid.Outcome = "SOURCE_EMPTY";
        }
        finish(result.Success ? theft ? "STOLEN" : delivery ? "DELIVERED" : "ATE_DEPOT" : result.Reason, 1000);
        return true;
    }
    // Escape/refuge and other shared motions keep their existing owner and
    // completion watchdog when the immediate threat disappears.
    if (state.CurrentPhase != Phase::Idle) return false;
    if (nowMs < actor.NextAtMs || nowMs < state.NextDecisionAtMs) return true;
    auto travel = [&](ActionPosition const& destination, char const* phase, char const* purpose)
    {
        raid.Phase = phase; actor.Phase = phase;
        auto next = step(destination);
        if (!next)
        {
            bool deferred = state.Planning.Deferred;
            actor.LastResult = deferred ? "PLANNING_DEFERRED" : "NO_ROUTE";
            actor.NextAtMs = nowMs+(deferred ? 100 : 10000); state.NextDecisionAtMs = actor.NextAtMs;
            if (deferred) state.MovementPurpose = "SUPPLY_PLANNING_DEFERRED";
            return true;
        }
        ActionRequest move; move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity; move.Destination = next;
        if (start(move, Phase::Moving, nullptr, false))
        { state.MovementPurpose = purpose; actor.LastResult = "MOVING"; }
        else
        {
            if (actor.LastResult != "MOVEMENT_OWNER_BUSY" && actor.LastResult != "ACTOR_MOVEMENT_BUSY" && actor.LastResult != "ACTOR_IN_COMBAT") actor.Travel.RejectPending();
            actor.NextAtMs = nowMs+10000;
        }
        return true;
    };
    auto activity = [&](Activity activity, char const* phase, char const* purpose)
    {
        ActionRequest request; request.Type = ActionType::Ambient; request.SourceGoal = GoalType::LocalActivity; request.AmbientActivity = activity;
        if (start(request, Phase::Acting, nullptr, false))
        { raid.Phase = phase; actor.Phase = phase; state.MovementPurpose = purpose; actor.LastResult = "STARTED"; }
        else { actor.LastResult = "ACTIVITY_REJECTED"; actor.NextAtMs = nowMs+10000; }
        return true;
    };
    if (guard)
    {
        raid.DutyAtPost = ((nowMs-_raidStartedAtMs)/LivingRaid::DutyMs)%2 != 0;
        auto home = creature.GetHomePosition();
        ActionPosition destination = raid.DutyAtPost ? LivingRaid::GuardPost : ActionPosition{725,home.GetPositionX(),home.GetPositionY(),home.GetPositionZ()};
        char const* phase = raid.DutyAtPost ? "GUARD_POST" : "GUARD_HOME";
        if (!Near(creature, destination)) return travel(destination, phase, phase);
        return activity(Activity::Look, phase, phase);
    }
    uint32 cargo = _supplyLedger.GetCargo(record.SpawnId);
    if (cargo || raid.Returning)
    {
        // Consume only physically reached anchors. The fixed route bounds the
        // administrative pass; at most one new movement is launched below.
        for (size_t reached = 0; reached <= LivingRaid::ToSource.size(); ++reached)
        {
            auto destination = raid.Leg ? LivingRaid::ToSource[raid.Leg-1] : LivingRaid::Camp;
            if (!Near(creature, destination)) return travel(destination, cargo ? "TO_CAMP" : "RETURNING_EMPTY", cargo ? "RAID_CAMP" : "RAID_EMPTY_RETURN");
            if (!raid.Leg) break;
            --raid.Leg; actor.Travel = {}; actor.NextAtMs = nowMs;
        }
        if (cargo)
        {
            auto depot = _supplyLedger.GetDepot(LivingRaid::CampDepot);
            if (depot && depot->Stock >= depot->Capacity)
            {
                if (record.Needs.Hunger >= 0.65f) return activity(Activity::Eat,"EATING_CAMP","RAID_MEAL");
                actor.LastResult = "DEPOT_FULL"; actor.NextAtMs = nowMs+10000;
                return true;
            }
            return activity(Activity::Work, "DELIVERING", "RAID_DELIVERY");
        }
        raid.Returning = raid.Active = false; raid.Phase = "COOLDOWN"; raid.NextAtMs = nowMs+LivingRaid::CooldownMs;
        return true;
    }
    auto camp = _supplyLedger.GetDepot(LivingRaid::CampDepot);
    if (record.Needs.Hunger >= 0.65f && camp && camp->Stock)
    {
        if (!Near(creature, LivingRaid::Camp)) return travel(LivingRaid::Camp,"TO_CAMP","RAID_CAMP");
        return activity(Activity::Eat,"EATING_CAMP","RAID_MEAL");
    }
    if (!raid.Active)
    {
        auto source = _supplyLedger.GetDepot(LivingRaid::SourceDepot);
        actor.Phase = nowMs < raid.NextAtMs ? "COOLDOWN" : "WAITING_SUPPLY"; raid.Phase = actor.Phase;
        if (nowMs < raid.NextAtMs || record.Needs.Hunger < 0.65f || (camp && camp->Stock) || !source || source->Stock < LivingRaid::StartStock)
        { actor.NextAtMs = nowMs+1000; return true; }
        if (raid.AttemptId == std::numeric_limits<uint64>::max()) { actor.NextAtMs = nowMs+60000; return true; }
        ++raid.AttemptId; raid.Active = true; raid.Leg = 0; raid.StartedAtMs = nowMs;
        raid.StolenAtStart = actor.Stolen; raid.Outcome = "STARTED";
    }
    for (size_t reached = 0; reached < LivingRaid::ToSource.size(); ++reached)
    {
        auto const& destination = LivingRaid::ToSource[std::min<size_t>(raid.Leg, LivingRaid::ToSource.size()-1)];
        if (!Near(creature, destination)) return travel(destination,"TO_SOURCE","RAID_SOURCE");
        if (raid.Leg+1 >= LivingRaid::ToSource.size()) break;
        ++raid.Leg; actor.Travel = {}; actor.NextAtMs = nowMs;
    }
    return activity(Activity::Work,"STEALING","RAID_THEFT");
}
