/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AIWorldMgr.h"
#include "Agent/LivingRolePolicy.h"
#include "Agent/GroupMemberFormation.h"
#include "Agent/WolfBehaviorPolicy.h"
#include "ChaseMovementGenerator.h"
#include "CombatManager.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "PointMovementGenerator.h"
#include "ThreatManager.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    using Phase = LivingRoleState::Phase;
    using Role = LivingRolePolicy::Role;
    using Activity = LivingRolePolicy::Activity;

    bool IsService(Creature const& creature)
    {
        return creature.IsVendor() || creature.IsTrainer() || creature.IsTaxi() || creature.IsBanker() ||
            creature.IsInnkeeper() || creature.IsAuctioner() || creature.IsSpiritService() || creature.IsBattleMaster();
    }

    char const* PhaseName(Phase phase)
    {
        switch (phase)
        {
            case Phase::Moving: return "MOVING";
            case Phase::Acting: return "ACTING";
            case Phase::Hunting: return "HUNTING";
            case Phase::Feeding: return "FEEDING";
            case Phase::Defending: return "DEFENDING";
            case Phase::Fleeing: return "FLEEING";
            default: return "IDLE";
        }
    }

    bool OwnsRoleMovement(AgentRecord const& record, Creature& creature)
    {
        auto* movement = creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE);
        if (!movement)
            return true;
        if (!record.ActiveActionState)
            return false;
        auto const& action = *record.ActiveActionState;
        if (auto* point = dynamic_cast<PointMovementGenerator<Creature>*>(movement))
            return action.Type == ActionType::MoveTo && point->GetId() == ActionExecutor::MovePointId;
        if (auto* chase = dynamic_cast<ChaseMovementGenerator*>(movement))
            return action.Type == ActionType::Attack && action.Target && chase->GetTarget() &&
                chase->GetTarget()->GetGUID() == action.Target->Guid;
        return action.Type == ActionType::Flee && movement->GetMovementGeneratorType() == FLEEING_MOTION_TYPE;
    }
}

std::optional<AIWorldMgr::LivingRoleDebugInfo> AIWorldMgr::DescribeLivingRole(Creature const& creature) const
{
    AgentRecord const* record = _registry.FindBySpawn(creature.GetMapId(), creature.GetSpawnId());
    if (!record)
        return std::nullopt;
    Role role = LivingRolePolicy::Resolve(record->Type, creature.GetEntry(), IsService(creature));
    LivingRoleDebugInfo info;
    info.Enabled = _livingRolesEnabled;
    info.ControlMode = record->ControlMode;
    info.Role = LivingRolePolicy::ToString(role);
    info.Hunger = record->Needs.Hunger;
    info.Phase = PhaseName(record->LivingRole.CurrentPhase);
    info.Activity = LivingRolePolicy::ToString(record->LivingRole.Activity);
    if (record->ActiveGoalState) info.Goal = ToString(record->ActiveGoalState->Type);
    else if (record->RoutineGoalState) info.Goal = ToString(record->RoutineGoalState->Type);
    else if (record->GroupCoordinationGoalState) info.Goal = ToString(record->GroupCoordinationGoalState->Type);
    if (record->ActiveActionState) info.Action = ToString(record->ActiveActionState->Type);
    if (!_enabled) info.Status = "AIWORLD_DISABLED";
    else if (IsLivingWolf(*record)) info.Status = "WOLF_PACK_CYCLE";
    else if (!_livingRolesEnabled) info.Status = "DISABLED";
    else if (record->ControlMode != AgentControlMode::AIWorldControlled) info.Status = "OBSERVE_ONLY";
    else if (creature.GetMapId() != 0 || creature.GetZoneId() != 12) info.Status = "OUTSIDE_ELWYNN";
    else if (!LivingRolePolicy::InScope(true, record->ControlMode, creature.GetMapId(), creature.GetZoneId(),
        _spawnParticipationCatalog.Resolve(record->SpawnId))) info.Status = "PARTICIPATION_EXCLUDED";
    else if (role == Role::None) info.Status = "UNCLASSIFIED";
    else if (creature.IsPet() || creature.IsCharmed()) info.Status = "PET_OR_CHARMED";
    else if (!creature.IsAlive()) info.Status = "ACTOR_DEAD";
    else if (record->WorldState != AgentWorldState::Materialized || record->RuntimeGuid != creature.GetGUID())
        info.Status = "NOT_BOUND_TO_CURRENT_CREATURE";
    else if (record->LivingRole.CurrentPhase != Phase::Idle) info.Status = "ACTIVE";
    else if (record->HomeLocation && record->WorkLocation) info.Status = "CURATED_ROUTINE";
    else if (record->GroupCoordinationGoalState) info.Status = "GROUP_ACTIVITY";
    else info.Status = "READY";
    return info;
}

void AIWorldMgr::StopLivingRole(AgentRecord& record, Creature& creature)
{
    auto& state = record.LivingRole;
    if (state.RuntimeGuid == creature.GetGUID() && state.CurrentPhase != Phase::Idle)
    {
        if (record.ActiveActionState && record.ActiveActionState->GoalStartedAtMs == state.StartedAtMs &&
            record.ActiveActionState->SourceGoal == state.SourceGoal)
        {
            auto const action = *record.ActiveActionState;
            if (action.Type == ActionType::Attack && action.Target)
                _actionExecutor.StopAttack(creature, action.Target->Guid);
            else if (action.Type == ActionType::MoveTo)
                _actionExecutor.StopMoveTo(creature);
            else if (action.Type == ActionType::Flee)
                _actionExecutor.FinishRoleFlee(creature, state.TargetGuid);
            record.ActiveActionState.reset();
        }
        if (!record.ActiveActionState)
            _actionExecutor.StopAmbient(creature, state.OwnedStandState);
        if (record.ActiveGoalState && record.ActiveGoalState->StartedAtMs == state.StartedAtMs &&
            record.ActiveGoalState->Type == state.SourceGoal)
            record.ActiveGoalState.reset();
    }
    state.CurrentPhase = Phase::Idle;
    state.TargetGuid.Clear();
    state.Activity = Activity::None;
    state.OwnedStandState = 0;
}

// World thread, at the existing needs cadence. This owns only controlled,
// classified permanent Elwynn NPCs; the proven wolf pack retains its own cycle.
bool AIWorldMgr::UpdateLivingRole(AgentRecord& record, Creature& creature, uint64 nowMs)
{
    auto& state = record.LivingRole;
    bool scoped = LivingRolePolicy::InScope(_livingRolesEnabled, record.ControlMode, creature.GetMapId(),
        creature.GetZoneId(), _spawnParticipationCatalog.Resolve(record.SpawnId));
    bool service = IsService(creature);
    Role role = LivingRolePolicy::Resolve(record.Type, creature.GetEntry(), service);
    if (!scoped || role == Role::None || IsLivingWolf(record) || creature.IsPet() || creature.IsCharmed())
    {
        if (!state.RuntimeGuid.IsEmpty())
        {
            StopLivingRole(record, creature);
            state = {};
        }
        return false;
    }
    if (state.RuntimeGuid != creature.GetGUID())
    {
        state = {};
        state.RuntimeGuid = creature.GetGUID();
        state.NextDecisionAtMs = nowMs + record.Id.Value % 15000;
    }
    if (state.CurrentPhase != Phase::Idle && (!record.ActiveGoalState ||
        record.ActiveGoalState->Type != state.SourceGoal || record.ActiveGoalState->StartedAtMs != state.StartedAtMs ||
        (state.CurrentPhase != Phase::Moving && !record.ActiveActionState)))
        StopLivingRole(record, creature);
    if (creature.IsInEvadeMode() || creature.HasUnitState(UNIT_STATE_CASTING))
    {
        if (state.CurrentPhase == Phase::Acting || state.CurrentPhase == Phase::Feeding)
            StopLivingRole(record, creature);
        return true;
    }

    Unit* threat = creature.GetThreatManager().GetCurrentVictim();
    auto validThreat = [&](Unit* unit)
    {
        return unit && unit->IsAlive() && creature.IsValidAttackTarget(unit) &&
            unit->GetZoneId() == 12 && creature.IsWithinDistInMap(unit, 30.0f) && creature.IsWithinLOSInMap(unit);
    };
    if (!validThreat(threat))
        threat = nullptr;
    // Critters and other units without a threat list still have live combat
    // references when hit by melee or spells. They must be able to flee too.
    if (!threat)
        for (auto const& combat : creature.GetCombatManager().GetPvECombatRefs())
        {
            Unit* other = combat.second->GetOther(&creature);
            bool mayFlee = !LivingRolePolicy::Fighter(role) && other && other->IsAlive() && other->GetZoneId() == 12 &&
                creature.IsWithinDistInMap(other, 30.0f) && creature.IsWithinLOSInMap(other);
            if ((validThreat(other) || mayFlee) && (!threat || other->GetGUID() < threat->GetGUID()))
                threat = other;
        }
    bool ownPrey = state.CurrentPhase == Phase::Hunting && threat && threat->GetGUID() == state.TargetGuid;

    // Reuse a small local grid query only when due, never the entire registry.
    std::vector<Creature*> nearby;
    if (LivingRolePolicy::HelpsAllies(role) && !threat && nowMs >= state.NextSenseAtMs)
    {
        state.NextSenseAtMs = nowMs + 5000 + record.Id.Value % 1000;
        creature.GetCreatureListWithEntryInGrid(nearby, 0, 25.0f);
        std::sort(nearby.begin(), nearby.end(), [&](Creature* a, Creature* b)
        { return creature.GetExactDistSq(a) < creature.GetExactDistSq(b); });
        if (nearby.size() > 64)
            nearby.resize(64);
        for (Creature* ally : nearby)
        {
            if (ally == &creature || !ally->IsAlive() || !ally->IsInCombat() || !creature.IsWithinLOSInMap(ally))
                continue;
            AgentRecord const* member = _registry.FindBySpawn(ally->GetMapId(), ally->GetSpawnId());
            if (!member || member->ControlMode != AgentControlMode::AIWorldControlled ||
                member->WorldFaction == WorldFactions::Unaffiliated || member->WorldFaction != record.WorldFaction)
                continue;
            Unit* attacker = ally->GetThreatManager().GetCurrentVictim();
            // Assistance only responds to real combat, never a reputation label.
            if (!validThreat(attacker) || !ally->IsInCombatWith(attacker) || !creature.IsFriendlyTo(ally))
                continue;
            if (member->LivingRole.CurrentPhase == Phase::Hunting && member->LivingRole.TargetGuid == attacker->GetGUID())
                continue;
            threat = attacker;
            break;
        }
    }

    if (state.CurrentPhase == Phase::Fleeing && !threat &&
        !WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 8000))
    {
        Unit* source = ObjectAccessor::GetUnit(creature, state.TargetGuid);
        if (validThreat(source))
            threat = source;
    }

    // Build authoritative facts at dispatch time. No request itself can grant
    // prey classification, participation, a threat identity, or an animation.
    auto start = [&](ActionRequest request, Phase phase, Unit* target = nullptr) -> bool
    {
        if (!OwnsRoleMovement(record, creature))
            return false;
        ActionValidationContext context;
        context.ControlMode = record.ControlMode;
        context.Materialized = record.WorldState == AgentWorldState::Materialized;
        context.Alive = creature.IsAlive();
        context.MapId = creature.GetMapId();
        context.X = creature.GetPositionX(); context.Y = creature.GetPositionY(); context.Z = creature.GetPositionZ();
        context.InCombat = creature.IsInCombat();
        context.LivingRoleAllowed = scoped;
        context.LivingRoleZoneId = creature.GetZoneId();
        context.LivingRole = role;
        context.ExpectedAmbientActivity = request.AmbientActivity;
        context.WildlifeRestAllowed = creature.GetStandState() == UNIT_STAND_STATE_STAND;
        context.ActiveGoalType = request.SourceGoal;
        context.ActiveGoalStartedAtMs = nowMs;
        context.DefenseThreatGuid = threat ? threat->GetGUID() : ObjectGuid::Empty;
        context.FleeSourceGuid = context.DefenseThreatGuid;
        context.MealTargetGuid = state.TargetGuid;
        if (target)
        {
            context.TargetResolved = true;
            context.TargetAlive = target->IsAlive();
            context.TargetAttackable = creature.IsValidAttackTarget(target);
            context.TargetGuid = target->GetGUID();
            context.TargetEntry = target->GetEntry();
            context.TargetMapId = target->GetMapId();
            context.TargetWithinAttackRange = creature.IsWithinDistInMap(target, phase == Phase::Feeding ? 5.0f : 30.0f);
            context.TargetInLineOfSight = creature.IsWithinLOSInMap(target);
            context.TargetIsRolePrey = target->GetTypeId() == TYPEID_UNIT &&
                _agentTypeCatalog.Resolve(target->GetEntry()) == AgentType::Prey && target->GetZoneId() == 12;
            request.Target = ActionTargetRef{ target->GetGUID(), target->GetEntry() };
        }
        if (creature.GetVictim())
        {
            context.ActorCurrentVictimGuid = creature.GetVictim()->GetGUID();
            if (record.ActiveActionState && record.ActiveActionState->Type == ActionType::Attack &&
                record.ActiveActionState->Target && record.ActiveActionState->Target->Guid == context.ActorCurrentVictimGuid)
                context.ActorCurrentVictimGuid.Clear();
        }
        request.Actor = record.Id;
        request.GoalStartedAtMs = nowMs;
        request.FleeFromGuid = context.FleeSourceGuid;
        if (request.Type == ActionType::Attack && target)
        {
            // Keep the chosen world bearing for the lifetime of this chase.
            // Fill the widest free gap among current attackers; deaths do not
            // reshuffle the remaining attackers' already-running chases.
            std::vector<float> bearings;
            for (Unit* attacker : target->getAttackers())
                if (attacker != &creature && attacker->IsAlive() && target->IsWithinDistInMap(attacker, 30.0f))
                {
                    float bearing = target->GetAbsoluteAngle(attacker);
                    if (Creature* npc = attacker->ToCreature())
                        if (AgentRecord const* other = _registry.FindBySpawn(npc->GetMapId(), npc->GetSpawnId()))
                            if ((other->LivingRole.CurrentPhase == Phase::Hunting || other->LivingRole.CurrentPhase == Phase::Defending) &&
                                other->LivingRole.TargetGuid == target->GetGUID())
                                bearing = other->LivingRole.ChaseBearing;
                    bearings.push_back(bearing);
                }
            request.ChaseAngleRadians = LivingRolePolicy::FreeChaseBearing(target->GetAbsoluteAngle(&creature), std::move(bearings));
        }
        auto validation = _actionSystem.Validate(request, context);
        if (!validation.Allowed)
        {
            TC_LOG_DEBUG("ai.world", "AI living role rejected agent={} goal={} reason={}",
                record.Id.Value, ToString(request.SourceGoal), ToString(validation.Reason));
            return false;
        }

        StopLivingRole(record, creature);
        if (record.GroupCoordinationGoalState)
            StopInFlightGroupCoordination(record, "LIVING_ROLE_PRIORITY", CoordinationStopReason::PreemptedByGoal);
        // A real emergency may interrupt the existing home/work commute.
        if (record.ActiveActionState)
        {
            auto const old = *record.ActiveActionState;
            if (old.Type == ActionType::MoveTo) _actionExecutor.StopMoveTo(creature);
            else if (old.Type == ActionType::Flee) _actionExecutor.StopFlee(creature);
            else if (old.Type == ActionType::Attack && old.Target) _actionExecutor.StopAttack(creature, old.Target->Guid);
            record.ActiveActionState.reset();
        }
        record.ActiveGoalState.reset();
        record.RoutineActivityState.reset();
        record.PendingEat.reset();
        ActionResult result;
        switch (request.Type)
        {
            case ActionType::MoveTo: result = _actionExecutor.ExecuteMoveTo(request, creature); break;
            case ActionType::Attack: result = _actionExecutor.ExecuteAttack(request, creature, *target); break;
            case ActionType::Flee: result = _actionExecutor.ExecuteFlee(request, creature, *target); break;
            case ActionType::Eat: result = _actionExecutor.ExecuteEat(request, creature); break;
            case ActionType::Ambient: result = _actionExecutor.ExecuteAmbient(request, creature); break;
            default: return false;
        }
        if (result.Status != ActionExecutionStatus::Started)
            return false;
        ActiveGoal goal;
        goal.Type = request.SourceGoal;
        goal.Priority = phase == Phase::Defending || phase == Phase::Fleeing ? GoalPriority::Emergency : GoalPriority::Normal;
        goal.StartedAtMs = nowMs;
        record.ActiveGoalState = goal;
        ActiveAction action;
        action.Type = request.Type; action.SourceGoal = request.SourceGoal;
        action.GoalStartedAtMs = nowMs; action.StartedAtMs = nowMs;
        action.Destination = request.Destination; action.Target = request.Target;
        record.ActiveActionState = action;
        state.CurrentPhase = phase;
        state.Activity = request.AmbientActivity;
        state.SourceGoal = request.SourceGoal;
        state.StartedAtMs = nowMs;
        state.TargetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
        if (request.ChaseAngleRadians) state.ChaseBearing = *request.ChaseAngleRadians;
        if (request.Destination) state.Destination = *request.Destination;
        else state.Destination = { creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ() };
        if (request.AmbientActivity == Activity::Rest) state.OwnedStandState = creature.GetStandState();
        TC_LOG_DEBUG("ai.world", "AI living role agent={} role={} goal={} activity={}",
            record.Id.Value, LivingRolePolicy::ToString(role), ToString(request.SourceGoal), LivingRolePolicy::ToString(request.AmbientActivity));
        return true;
    };

    bool minimumEscape = state.CurrentPhase == Phase::Fleeing && !WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 8000);
    bool flee = threat && (minimumEscape || nowMs < state.DefenseCooldownUntilMs ||
        LivingRolePolicy::ShouldFlee(role, record.Needs.HealthPressure, state.CurrentPhase == Phase::Fleeing));
    if (threat && (!ownPrey || flee))
    {
        Phase phase = flee ? Phase::Fleeing : Phase::Defending;
        if (state.CurrentPhase == phase && state.TargetGuid == threat->GetGUID())
        {
            auto* movement = creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE);
            bool stillRunning = movement && OwnsRoleMovement(record, creature) &&
                (phase == Phase::Fleeing || creature.GetVictim() == threat);
            if (!WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 30000) && stillRunning &&
                creature.GetDistance(state.Destination.X, state.Destination.Y, state.Destination.Z) <= 30.0f)
                return true;
            if (phase == Phase::Defending)
                state.DefenseCooldownUntilMs = nowMs + 10000;
            StopLivingRole(record, creature);
            state.NextDecisionAtMs = nowMs + 5000;
            return true;
        }
        ActionRequest request;
        request.Type = flee ? ActionType::Flee : ActionType::Attack;
        request.SourceGoal = flee ? GoalType::FleeDanger : GoalType::Defend;
        start(request, phase, threat);
        return true;
    }

    if (state.CurrentPhase == Phase::Defending || state.CurrentPhase == Phase::Fleeing)
    {
        StopLivingRole(record, creature);
        state.NextDecisionAtMs = nowMs + 2000;
    }
    if (state.CurrentPhase == Phase::Hunting || state.CurrentPhase == Phase::Feeding)
    {
        Creature* prey = ObjectAccessor::GetCreature(creature, state.TargetGuid);
        if (!prey || !creature.IsWithinDistInMap(prey, 30.0f) || !creature.IsWithinLOSInMap(prey) ||
            prey->GetZoneId() != 12 || creature.GetDistance(state.Destination.X, state.Destination.Y, state.Destination.Z) > 30.0f ||
            WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 45000))
        {
            StopLivingRole(record, creature);
            state.NextDecisionAtMs = nowMs + 10000;
            return true;
        }
        if (state.CurrentPhase == Phase::Hunting)
        {
            if (prey->IsAlive() && creature.IsValidAttackTarget(prey) && creature.GetVictim() == prey)
                return true;
            ObjectGuid meal = prey->GetGUID();
            StopLivingRole(record, creature);
            if (!prey->IsAlive() && creature.IsWithinDistInMap(prey, 5.0f) && !creature.IsInCombat())
            {
                state.TargetGuid = meal;
                ActionRequest feed;
                feed.Type = ActionType::Eat; feed.SourceGoal = GoalType::Feed;
                start(feed, Phase::Feeding, prey);
            }
            return true;
        }
        if (prey->IsAlive() || creature.IsInCombat() || !creature.IsWithinDistInMap(prey, 5.0f) || !creature.IsStopped())
        {
            StopLivingRole(record, creature);
            return true;
        }
        if (WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 5000))
        {
            _needsSystem.SatisfyHunger(record.Needs);
            StopLivingRole(record, creature);
            ActionRequest rest;
            rest.Type = ActionType::Ambient; rest.SourceGoal = GoalType::LocalActivity; rest.AmbientActivity = Activity::Rest;
            start(rest, Phase::Acting);
        }
        return true;
    }
    if (state.CurrentPhase == Phase::Moving)
    {
        if (record.ActiveActionState && creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE) &&
            OwnsRoleMovement(record, creature) &&
            !WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 20000))
            return true;
        StopLivingRole(record, creature);
        state.NextDecisionAtMs = nowMs + 6000;
        return true;
    }
    if (state.CurrentPhase == Phase::Acting)
    {
        if (creature.IsInCombat() || !creature.IsStopped() ||
            (state.OwnedStandState && creature.GetStandState() != state.OwnedStandState))
        {
            StopLivingRole(record, creature);
            return true;
        }
        uint64 duration = state.Activity == Activity::Rest ? 20000 : 5000;
        if (!WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, duration))
            return true;
        if (state.Activity == Activity::Graze || state.Activity == Activity::Eat)
            _needsSystem.SatisfyHunger(record.Needs);
        if (state.Activity == Activity::Rest)
            record.Needs.Fatigue = 0.0f;
        StopLivingRole(record, creature);
        state.NextDecisionAtMs = nowMs + 8000 + record.Id.Value % 7000;
        return true;
    }

    // Preserve curated commutes and group attempts. Emergency handling above
    // still applies, but ambient cycles do not invent home/work data or wages.
    if ((record.HomeLocation && record.WorkLocation) || record.GroupCoordinationGoalState)
        return false;
    if (nowMs < state.NextDecisionAtMs || creature.IsInCombat() || !OwnsRoleMovement(record, creature))
        return true;
    state.NextDecisionAtMs = nowMs + 10000;
    uint64 cycle = ++state.Cycle + record.Id.Value;
    Position const home = creature.GetHomePosition();
    float homeDistance = creature.GetExactDist2d(home.GetPositionX(), home.GetPositionY());
    bool anchored = service || creature.IsQuestGiver();
    float radius = anchored ? 0.0f : LivingRolePolicy::RoamRadius(role);

    if (role == Role::Predator && WolfBehaviorPolicy::WantsHunt(record.Needs.Hunger, record.Needs.HealthPressure, false) && homeDistance < 20.0f)
    {
        if (nearby.empty()) creature.GetCreatureListWithEntryInGrid(nearby, 0, 25.0f);
        Creature* prey = nullptr;
        float nearest = 25.0f;
        for (Creature* candidate : nearby)
        {
            if (_agentTypeCatalog.Resolve(candidate->GetEntry()) != AgentType::Prey || candidate->IsPet() || candidate->IsCharmed() ||
                !candidate->GetOwnerGUID().IsEmpty() ||
                !validThreat(candidate) || candidate->GetZoneId() != 12)
                continue;
            float distance = creature.GetExactDist(candidate);
            if (distance < nearest)
            {
                nearest = distance;
                prey = candidate;
            }
        }
        if (prey)
        {
            PathGenerator path(&creature);
            if (path.CalculatePath(prey->GetPositionX(), prey->GetPositionY(), prey->GetPositionZ(), false) &&
                !(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)))
            {
                ActionRequest hunt;
                hunt.Type = ActionType::Attack; hunt.SourceGoal = GoalType::PredatorHunt;
                if (start(hunt, Phase::Hunting, prey)) return true;
            }
        }
    }

    uint32 dayTime = uint32(nowMs % _routineScheduleConfig.DayLengthMs);
    bool workHours = dayTime >= _routineScheduleConfig.WorkStartMs && dayTime < _routineScheduleConfig.WorkEndMs;
    Activity activity = LivingRolePolicy::IdleActivity(role, cycle, workHours, record.Needs.Hunger, record.Needs.Fatigue);
    if (anchored && (activity == Activity::Roam || activity == Activity::Rest)) activity = Activity::Look;
    if (activity == Activity::Roam || homeDistance > radius + 2.0f)
    {
        float angle = float((cycle * 137) % 360) * GroupMemberFormation::TwoPi / 360.0f;
        ActionPosition destination{ creature.GetMapId(), home.GetPositionX(), home.GetPositionY(), home.GetPositionZ() };
        if (homeDistance <= radius + 2.0f)
        {
            destination.X += radius * std::cos(angle);
            destination.Y += radius * std::sin(angle);
        }
        // Return in bounded steps after an escape, using the same path checks.
        float distance = std::hypot(destination.X - creature.GetPositionX(), destination.Y - creature.GetPositionY());
        if (distance > 30.0f)
        {
            destination.X = creature.GetPositionX() + (destination.X - creature.GetPositionX()) * 30.0f / distance;
            destination.Y = creature.GetPositionY() + (destination.Y - creature.GetPositionY()) * 30.0f / distance;
            destination.Z = creature.GetPositionZ();
        }
        Map* map = creature.GetMap();
        destination.Z = map->GetHeight(creature.GetPhaseMask(), destination.X, destination.Y, destination.Z + 4.0f, true);
        if (!std::isfinite(destination.Z) || destination.Z <= INVALID_HEIGHT ||
            map->GetZoneId(creature.GetPhaseMask(), destination.X, destination.Y, destination.Z) != 12 ||
            !creature.IsWithinLOS(destination.X, destination.Y, destination.Z))
            return true;
        PathGenerator path(&creature);
        if (!path.CalculatePath(destination.X, destination.Y, destination.Z, false) ||
            (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)))
            return true;
        ActionRequest move;
        move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity; move.Destination = destination;
        start(move, Phase::Moving);
        return true;
    }
    ActionRequest ambient;
    ambient.Type = ActionType::Ambient; ambient.SourceGoal = GoalType::LocalActivity; ambient.AmbientActivity = activity;
    start(ambient, Phase::Acting);
    return true;
}
