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
#include "Agent/LivingForagePolicy.h"
#include "Agent/LivingHuntPolicy.h"
#include "Agent/LivingReturnPolicy.h"
#include "Agent/LivingRolePolicy.h"
#include "Agent/GroupMemberFormation.h"
#include "Agent/WolfBehaviorPolicy.h"
#include "ChaseMovementGenerator.h"
#include "CombatManager.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementPathBounds.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "PointMovementGenerator.h"
#include "ThreatManager.h"
#include <algorithm>
#include <cmath>
#include <string_view>
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
            case Phase::SeekingSafety: return "SEEKING_SAFETY";
            case Phase::Investigating: return "INVESTIGATING";
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

    bool IsRoleMove(Phase phase)
    {
        return phase == Phase::Moving || phase == Phase::SeekingSafety || phase == Phase::Investigating;
    }

    bool IsEscaping(Phase phase) { return phase == Phase::Fleeing || phase == Phase::SeekingSafety; }

    bool CheckRolePath(Creature& creature, ActionPosition& destination,
        ActionPosition const* danger = nullptr, float clearance = 8.0f, char const** failure = nullptr,
        Position const* homeBound = nullptr, float homeRadius = 0.0f, bool routePoint = false,
        LivingReturnPolicy::Diagnostics* diagnostics = nullptr)
    {
        auto reject = [&](char const* reason, LivingReturnPolicy::Rejection kind)
        {
            if (failure) *failure = reason;
            if (diagnostics) ++diagnostics->Rejected[kind];
            return false;
        };
        if (diagnostics)
        {
            ++diagnostics->Candidates;
            diagnostics->RequestedZ = destination.Z;
            diagnostics->ResolvedZ.reset();
            diagnostics->PathType = 0;
        }
        if (destination.MapId != creature.GetMapId() || !std::isfinite(destination.X) ||
            !std::isfinite(destination.Y) || !std::isfinite(destination.Z) ||
            creature.GetExactDist2d(destination.X, destination.Y) > 30.0f)
            return reject("RETURN_INVALID_STEP", LivingReturnPolicy::Invalid);
        Map* map = creature.GetMap();
        // A navmesh/observed point already carries its floor. Looking down
        // from a different height can silently replace it with another floor.
        // New roaming proposals, however, still need a nearby ground height.
        if (!routePoint)
        {
            float hint = destination.Z;
            std::optional<float> ground;
            for (float lift : { 4.0f, 12.0f })
            {
                float height = map->GetHeight(creature.GetPhaseMask(), destination.X, destination.Y, hint + lift, true);
                if (std::isfinite(height) && height > INVALID_HEIGHT && std::abs(height - hint) <= 12.0f &&
                    (!ground || std::abs(height - hint) < std::abs(*ground - hint)))
                    ground = height;
            }
            if (!ground) return reject("RETURN_HEIGHT_INVALID", LivingReturnPolicy::Height);
            destination.Z = *ground;
        }
        if (diagnostics) diagnostics->ResolvedZ = destination.Z;
        if (map->GetZoneId(creature.GetPhaseMask(), destination.X, destination.Y, destination.Z) != 12)
            return reject("RETURN_OUTSIDE_ZONE", LivingReturnPolicy::Zone);
        PathGenerator path(&creature);
        bool calculated = path.CalculatePath(destination.X, destination.Y, destination.Z, false);
        if (diagnostics) diagnostics->PathType = uint32(path.GetPathType());
        if (!calculated || !(path.GetPathType() & PATHFIND_NORMAL) ||
            (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORT | PATHFIND_SHORTCUT)))
            return reject("RETURN_NO_PATH", LivingReturnPolicy::Path);
        // A complete navmesh path can legitimately bend behind a wall. The
        // engine's no-mmap/flying shortcut is NOT evidence of such a route.
        if ((path.GetPathType() & PATHFIND_NOT_USING_PATH) &&
            !creature.IsWithinLOS(destination.X, destination.Y, destination.Z))
            return reject("RETURN_LOS_BLOCKED", LivingReturnPolicy::Los);
        if (!Movement::PathWithinBounds(path.GetPath(), [&](float x, float y, float z)
            { return map->GetZoneId(creature.GetPhaseMask(), x, y, z) == 12 &&
                (!homeBound || homeBound->GetExactDist2d(x, y) <= homeRadius); }))
            return reject("RETURN_PATH_BOUNDS", LivingReturnPolicy::Bounds);
        auto const& end = path.GetPath().back();
        if (std::hypot(end.x - destination.X, end.y - destination.Y) > 1.0f || std::abs(end.z - destination.Z) > 4.0f)
            return reject("RETURN_HEIGHT_INVALID", LivingReturnPolicy::Height);
        float x = creature.GetPositionX(), y = creature.GetPositionY();
        for (auto const& point : path.GetPath())
        {
            if (danger && std::hypot(point.x - x, point.y - y) > 0.1f &&
                !LivingRolePolicy::AvoidsDanger(x, y, point.x, point.y, danger->X, danger->Y, clearance))
                return reject("RETURN_DANGER_BLOCKED", LivingReturnPolicy::Danger);
            x = point.x; y = point.y;
        }
        destination.X = end.x; destination.Y = end.y; destination.Z = end.z;
        if (diagnostics) diagnostics->ResolvedZ = end.z;
        return true;
    }

    std::optional<ActionPosition> FindRoleReturnStep(Creature& creature, Position const& home,
        ActionPosition const* danger, char const*& failure, LivingRoleState& state)
    {
        state.ReturnDiagnostics = {};
        state.ReturnStrategy = "HOME_PATH";
        failure = "RETURN_NO_PATH";
        PathGenerator route(&creature);
        bool completeRoute = route.CalculatePath(home.GetPositionX(), home.GetPositionY(), home.GetPositionZ(), false) &&
            !(route.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORT));

        std::vector<ActionPosition> points;
        points.reserve(route.GetPath().size() + 1);
        points.push_back({ creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ() });
        if (completeRoute)
            for (auto const& point : route.GetPath())
                points.push_back({ creature.GetMapId(), point.x, point.y, point.z });
        else
            // A long escape can put home beyond the pathfinder's full-route
            // limit. A nearer step must still pass its own complete path check.
            points.push_back({ creature.GetMapId(), home.GetPositionX(), home.GetPositionY(), home.GetPositionZ() });

        // A bend may hide the longest waypoint. Retry nearer points on the
        // same route, with bounded work and all the ordinary safety checks.
        if (completeRoute) failure = "RETURN_STEP_BLOCKED";
        for (float budget : { LivingReturnPolicy::MaxStepLength, 14.0f, 7.0f, 3.5f, 1.75f })
            if (auto step = LivingReturnPolicy::PathStep(points, budget);
                step && creature.GetExactDist(step->X, step->Y, step->Z) > 1.0f &&
                CheckRolePath(creature, *step, danger, 8.0f, &failure, nullptr, 0.0f, completeRoute, &state.ReturnDiagnostics))
                return step;
        // Real corners can precede a bend that hides every fixed-length step.
        if (completeRoute)
        {
            uint32 checkedCorners = 0;
            for (auto step : points)
            {
                float distance = creature.GetExactDist(step.X, step.Y, step.Z);
                if (distance <= 1.0f || distance > LivingReturnPolicy::MaxStepLength)
                    continue;
                if (CheckRolePath(creature, step, danger, 8.0f, &failure, nullptr, 0.0f, true, &state.ReturnDiagnostics))
                    return step;
                if (++checkedCorners == 8)
                    break;
            }
        }
        // Try positions actually traversed on the outward trip, newest first.
        // These may initially go away from home; every leg is revalidated.
        state.ReturnStrategy = "TRAIL";
        ActionPosition current{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
        for (auto step : LivingReturnPolicy::TrailSteps(state.ReturnTrail, current))
        {
            if (CheckRolePath(creature, step, danger, 8.0f, &failure, nullptr, 0.0f, true, &state.ReturnDiagnostics))
                return step;
        }
        if (state.ReturnFailures)
        {
            state.ReturnStrategy = "DETOUR";
            ActionPosition from{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
            ActionPosition destination{creature.GetMapId(), home.GetPositionX(), home.GetPositionY(), home.GetPositionZ()};
            for (auto step : LivingReturnPolicy::Detours(from, destination, ++state.ReturnSearchSequence))
                if (CheckRolePath(creature, step, danger, 8.0f, &failure, &home, state.ReturnHomeLimit, false, &state.ReturnDiagnostics))
                    return step;
        }
        return std::nullopt;
    }

    void FailedReturn(LivingRoleState& state, uint64 nowMs, char const* reason, ActionPosition const& here)
    {
        if (!state.ReturnStalledSinceMs)
        {
            state.ReturnStalledSinceMs = nowMs;
            state.ReturnStallAnchor = here;
        }
        state.ReturnFailures = std::min(state.ReturnFailures + 1, 16u);
        state.ReturnRetryAtMs = nowMs + LivingReturnPolicy::RetryDelayMs(state.ReturnFailures);
        state.ReturnFailure = reason;
    }

    std::optional<ActionPosition> FoodSupplyStep(Creature& creature, ActionPosition const& work)
    {
        if (work.MapId != creature.GetMapId()) return std::nullopt;
        PathGenerator path(&creature);
        if (!path.CalculatePath(work.X, work.Y, work.Z, false) ||
            (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORT | PATHFIND_SHORTCUT)))
            return std::nullopt;
        std::vector<ActionPosition> points;
        for (auto const& point : path.GetPath())
            points.push_back({creature.GetMapId(), point.x, point.y, point.z});
        auto step = LivingReturnPolicy::PathStep(points);
        if (step && CheckRolePath(creature, *step, nullptr, 8.0f, nullptr, nullptr, 0.0f, true)) return step;
        return std::nullopt;
    }
}

bool AIWorldMgr::CanLivingPredatorHuntNeutralPrey(Creature const& hunter, Creature const& prey) const
{
    if (!_enabled || !_livingRolesEnabled || &hunter == &prey || hunter.GetMapId() != 0 ||
        hunter.GetZoneId() != 12 || prey.GetZoneId() != 12 || !hunter.IsInMap(&prey) ||
        !hunter.IsAlive() || !prey.IsAlive() || hunter.IsPet() || prey.IsPet() ||
        !hunter.GetCharmerOrOwnerGUID().IsEmpty() || !prey.GetCharmerOrOwnerGUID().IsEmpty() ||
        hunter.IsControlledByPlayer() || prey.IsControlledByPlayer() || IsService(prey) || prey.IsQuestGiver())
        return false;
    AgentRecord const* record = _registry.FindBySpawn(hunter.GetMapId(), hunter.GetSpawnId());
    if (!record || record->WorldState != AgentWorldState::Materialized || record->RuntimeGuid != hunter.GetGUID() ||
        IsLivingWolf(*record) || !LivingRolePolicy::InScope(true, record->ControlMode, hunter.GetMapId(),
            hunter.GetZoneId(), _spawnParticipationCatalog.Resolve(record->SpawnId)))
        return false;
    // Use the raw native lookup, never GetReactionTo/IsFriendlyTo here: those
    // call this bridge and would recurse. Both sides must actually be neutral.
    if (!hunter.GetFactionTemplateEntry() || !prey.GetFactionTemplateEntry())
        return false;
    bool neutral = WorldObject::GetFactionReactionTo(hunter.GetFactionTemplateEntry(), &prey) == REP_NEUTRAL &&
        WorldObject::GetFactionReactionTo(prey.GetFactionTemplateEntry(), &hunter) == REP_NEUTRAL;
    return LivingRolePolicy::CanHuntNeutralPrey(LivingRolePolicy::Resolve(record->Type, hunter.GetEntry(), IsService(hunter)),
        _agentTypeCatalog.Resolve(prey.GetEntry()), neutral);
}

std::optional<AIWorldMgr::LivingRoleDebugInfo> AIWorldMgr::DescribeLivingRole(Creature const& creature) const
{
    AgentRecord const* record = _registry.FindBySpawn(creature.GetMapId(), creature.GetSpawnId());
    if (!record)
        return std::nullopt;
    return DescribeLivingRole(creature, *record);
}

AIWorldMgr::LivingRoleDebugInfo AIWorldMgr::DescribeLivingRole(Creature const& creature, AgentRecord const& agent) const
{
    AgentRecord const* record = &agent;
    Role role = LivingRolePolicy::Resolve(record->Type, creature.GetEntry(), IsService(creature));
    LivingRoleDebugInfo info;
    info.Enabled = _livingRolesEnabled;
    info.ControlMode = record->ControlMode;
    info.Role = LivingRolePolicy::ToString(role);
    info.Hunger = record->Needs.Hunger;
    info.Phase = PhaseName(record->LivingRole.CurrentPhase);
    info.Activity = LivingRolePolicy::ToString(record->LivingRole.Activity);
    info.ExtensionsEnabled = _livingRoleExtensionsEnabled && _livingRolesEnabled && !IsLivingWolf(*record);
    info.Caution = LivingRolePolicy::Caution(record->Id.Value);
    info.Awareness = record->LivingRole.Awareness;
    info.MovementPurpose = record->LivingRole.MovementPurpose;
    info.Food = record->EconomyState.Food;
    info.Resource = record->EconomyState.Resource;
    info.ReturnFailures = record->LivingRole.ReturnFailures;
    info.ReturnTrailPoints = uint32(record->LivingRole.ReturnTrail.size());
    info.ReturnStrategy = record->LivingRole.ReturnStrategy;
    info.ReturnFailure = record->LivingRole.ReturnFailure;
    uint64 observedAt = GetCurrentTimeMs();
    if (record->LivingRole.ReturnStalledSinceMs && observedAt >= record->LivingRole.ReturnStalledSinceMs)
        info.ReturnStalledMs = observedAt - record->LivingRole.ReturnStalledSinceMs;
    if (Creature* companion = ObjectAccessor::GetCreature(creature, record->LivingRole.CompanionGuid))
        info.CompanionSpawnId = companion->GetSpawnId();
    if (role == Role::Predator && !IsLivingWolf(*record))
    {
        info.HuntStatus = record->LivingRole.LastHuntStatus;
        info.HuntEnd = record->LivingRole.LastHuntEnd;
        info.RunSpeed = creature.GetSpeed(MOVE_RUN);
        info.MoveSpeed = creature.IsStopped() ? 0.0f : creature.movespline->Velocity();
        if (record->LivingRole.CurrentPhase == Phase::Hunting)
            if (auto* chase = dynamic_cast<ChaseMovementGenerator*>(
                creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE));
                chase && chase->GetTarget() && chase->GetTarget()->GetGUID() == record->LivingRole.TargetGuid)
            {
                info.SprintMultiplier = chase->GetSpeedBoost().GetMultiplier();
                info.SprintRemainingMs = chase->GetSpeedBoost().GetRemainingMs();
            }
        if (Creature* prey = ObjectAccessor::GetCreature(creature, record->LivingRole.LastHuntTargetGuid))
        {
            info.HuntTargetSpawnId = prey->GetSpawnId();
            info.HuntTargetDistance = creature.GetExactDist2d(prey);
            info.PreyRunSpeed = prey->GetSpeed(MOVE_RUN);
            info.PreyMoveSpeed = prey->IsStopped() ? 0.0f : prey->movespline->Velocity();
        }
        info.HomeDistance = creature.GetExactDist2d(&creature.GetHomePosition());
        info.InCombat = creature.IsInCombat();
        info.Moving = !creature.IsStopped();
        info.MovementBlocked = creature.HasUnitState(UNIT_STATE_NOT_MOVE) || creature.IsMovementPreventedByCasting();
        info.CannotReachTarget = creature.CanNotReachTarget();
        info.Evading = creature.IsInEvadeMode();
        uint64 nowMs = GetCurrentTimeMs();
        info.DecisionWaitMs = record->LivingRole.NextDecisionAtMs > nowMs ? record->LivingRole.NextDecisionAtMs - nowMs : 0;
        info.NearbyPrey = record->LivingRole.NearbyPrey;
        info.AttackablePrey = record->LivingRole.AttackablePrey;
    }
    if (LivingRolePolicy::HelpsAllies(role))
    {
        info.AssistStatus = record->LivingRole.LastAssistStatus;
        info.NearbyAllies = record->LivingRole.NearbyAllies;
        info.AlliesInCombat = record->LivingRole.AlliesInCombat;
    }
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
    if (state.CurrentPhase == Phase::Hunting)
        state.LastHuntStatus = state.LastHuntEnd = "HUNT_INTERRUPTED";
    else if (state.CurrentPhase == Phase::Feeding)
        state.LastHuntStatus = state.LastHuntEnd = "FEED_INTERRUPTED";
    if (state.RuntimeGuid == creature.GetGUID() && state.CurrentPhase != Phase::Idle)
    {
        bool ownsAttempt = record.ActiveGoalState && record.ActiveGoalState->Type == state.SourceGoal &&
            record.ActiveGoalState->StartedAtMs == state.StartedAtMs && (!record.ActiveActionState ||
                (record.ActiveActionState->SourceGoal == state.SourceGoal && record.ActiveActionState->GoalStartedAtMs == state.StartedAtMs));
        if (record.ActiveActionState && record.ActiveActionState->GoalStartedAtMs == state.StartedAtMs &&
            record.ActiveActionState->SourceGoal == state.SourceGoal)
        {
            auto const action = *record.ActiveActionState;
            if (action.Type == ActionType::Attack && action.Target)
                _actionExecutor.StopAttack(creature, action.Target->Guid);
            else if (action.Type == ActionType::MoveTo)
            {
                _actionExecutor.StopMoveTo(creature);
            }
            else if (action.Type == ActionType::Flee)
                _actionExecutor.FinishRoleFlee(creature, state.TargetGuid);
            record.ActiveActionState.reset();
        }
        if (!record.ActiveActionState)
            _actionExecutor.StopAmbient(creature, state.OwnedStandState);
        // MovementInform may already have consumed the action on arrival.
        if (state.CurrentPhase == Phase::SeekingSafety && ownsAttempt)
            _actionExecutor.FinishRoleFlee(creature, state.TargetGuid, false);
        if (record.ActiveGoalState && record.ActiveGoalState->StartedAtMs == state.StartedAtMs &&
            record.ActiveGoalState->Type == state.SourceGoal)
            record.ActiveGoalState.reset();
    }
    state.CurrentPhase = Phase::Idle;
    state.TargetGuid.Clear();
    state.Activity = Activity::None;
    state.OwnedStandState = 0;
    state.StockMeal = false;
    state.GatheringFood = false;
    state.MovementPurpose = "NONE";
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
    if (!scoped || role == Role::None || IsLivingWolf(record) || creature.IsPet() ||
        !creature.GetCharmerOrOwnerGUID().IsEmpty() || creature.IsControlledByPlayer())
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
    Position const home = creature.GetHomePosition();
    float homeDistance = creature.GetExactDist2d(home.GetPositionX(), home.GetPositionY());
    bool anchored = service || creature.IsQuestGiver();
    float radius = anchored ? 0.0f : LivingRolePolicy::RoamRadius(role);
    ActionPosition here{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
    // Actual displacement, including an interrupted return or emergency,
    // ends a stationary episode. A new action alone is not progress.
    if (state.ReturnStalledSinceMs && (state.ReturnStallAnchor.MapId != here.MapId ||
        LivingReturnPolicy::Distance(state.ReturnStallAnchor, here) > 1.0f))
    {
        state.ReturnFailures = 0;
        state.ReturnRetryAtMs = state.ReturnStalledSinceMs = 0;
        state.ReturnFailure = "NONE";
    }
    if (homeDistance <= radius + 2.0f)
    {
        state.ReturningHome = false;
        state.ReturnHomeLimit = 0.0f;
        state.ReturnTrail.clear();
        state.ReturnFailures = 0;
        state.ReturnRetryAtMs = state.ReturnStalledSinceMs = 0;
        state.ReturnFailure = state.ReturnStrategy = "NONE";
    }
    LivingReturnPolicy::ObserveTrail(state.ReturnTrail, here, state.ReturningHome);
    if (state.CurrentPhase != Phase::Idle && (!record.ActiveGoalState ||
        record.ActiveGoalState->Type != state.SourceGoal || record.ActiveGoalState->StartedAtMs != state.StartedAtMs ||
        (!IsRoleMove(state.CurrentPhase) && !record.ActiveActionState)))
        StopLivingRole(record, creature);
    if (creature.IsInEvadeMode() || creature.HasUnitState(UNIT_STATE_CASTING))
    {
        if (state.CurrentPhase == Phase::Hunting)
        {
            if (creature.IsInEvadeMode())
            {
                StopLivingRole(record, creature);
                state.LastHuntStatus = state.LastHuntEnd = "HUNT_EVADE";
                state.NextDecisionAtMs = nowMs + 10000;
            }
            else
                state.LastHuntStatus = "HUNT_CASTING";
        }
        if (state.CurrentPhase == Phase::Acting || state.CurrentPhase == Phase::Feeding)
            StopLivingRole(record, creature);
        return true;
    }

    // Stop at a reached refuge once the pursuer is no longer close. Merely
    // retaining a passive creature's combat ref must not force another leg.
    if (state.CurrentPhase == Phase::SeekingSafety && !record.ActiveActionState &&
        creature.GetDistance(state.Destination.X, state.Destination.Y, state.Destination.Z) <= 2.0f)
    {
        Unit* source = ObjectAccessor::GetUnit(creature, state.TargetGuid);
        if (!source || !source->IsAlive() || !creature.IsWithinDistInMap(source, LivingRolePolicy::NoticeRadius(record.Id.Value) + 3.0f))
        {
            StopLivingRole(record, creature);
            state.Awareness = "REFUGE_REACHED";
            state.NextDecisionAtMs = nowMs + 6000;
            return true;
        }
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

    bool extensions = _livingRoleExtensionsEnabled;
    auto visibleDanger = [&](Unit* unit)
    {
        return unit && unit != &creature && unit->IsAlive() && unit->GetZoneId() == 12 &&
            creature.IsWithinDistInMap(unit, 30.0f) && creature.CanSeeOrDetect(unit) && creature.IsWithinLOSInMap(unit);
    };
    auto eligibleMember = [&](Creature* npc) -> AgentRecord const*
    {
        if (!npc || npc->IsPet() || !npc->GetCharmerOrOwnerGUID().IsEmpty() || npc->IsControlledByPlayer())
            return nullptr;
        AgentRecord const* member = _registry.FindBySpawn(npc->GetMapId(), npc->GetSpawnId());
        return member && member->WorldState == AgentWorldState::Materialized && member->RuntimeGuid == npc->GetGUID() &&
            LivingRolePolicy::InScope(true, member->ControlMode, npc->GetMapId(), npc->GetZoneId(),
                _spawnParticipationCatalog.Resolve(member->SpawnId)) ? member : nullptr;
    };

    // Reuse a small local grid query only when due, never the entire registry.
    std::vector<Creature*> nearby;
    bool nearbyLoaded = false;
    auto loadNearby = [&]()
    {
        if (nearbyLoaded) return;
        nearbyLoaded = true;
        creature.GetCreatureListWithEntryInGrid(nearby, 0, 25.0f);
        std::sort(nearby.begin(), nearby.end(), [&](Creature* a, Creature* b)
        {
            float da = creature.GetExactDistSq(a), db = creature.GetExactDistSq(b);
            return da == db ? a->GetGUID() < b->GetGUID() : da < db;
        });
        if (nearby.size() > 64)
            nearby.resize(64);
    };
    std::optional<ActionPosition> alarmDestination;
    auto freeSlotBearing = [&](Creature& center, float preferred)
    {
        loadNearby();
        std::vector<float> occupied;
        for (Creature* neighbor : nearby)
        {
            if (neighbor == &creature || neighbor == &center || !neighbor->IsAlive()) continue;
            AgentRecord const* member = eligibleMember(neighbor);
            if (member && IsRoleMove(member->LivingRole.CurrentPhase) &&
                center.GetExactDist2d(member->LivingRole.Destination.X, member->LivingRole.Destination.Y) <= 8.0f)
                occupied.push_back(center.GetAbsoluteAngle(member->LivingRole.Destination.X, member->LivingRole.Destination.Y));
            else if (center.IsWithinDistInMap(neighbor, 8.0f))
                occupied.push_back(center.GetAbsoluteAngle(neighbor));
        }
        return LivingRolePolicy::FreeChaseBearing(preferred, std::move(occupied));
    };
    if ((LivingRolePolicy::HelpsAllies(role) || extensions) && !threat && nowMs >= state.NextSenseAtMs)
    {
        state.NextSenseAtMs = nowMs + 1000 + (extensions ? record.Id.Value % 500 : 0);
        state.NearbyAllies = state.AlliesInCombat = 0;
        state.LastAssistStatus = "NO_ALLIES";
        state.CompanionGuid.Clear();
        state.Awareness = nowMs < state.DangerUntilMs ? "REMEMBERED_DANGER" : "QUIET";
        loadNearby();
        for (Creature* ally : nearby)
        {
            if (ally == &creature || !ally->IsAlive() || ally->GetZoneId() != 12 ||
                !creature.CanSeeOrDetect(ally) || !creature.IsWithinLOSInMap(ally))
                continue;
            AgentRecord const* member = eligibleMember(ally);
            if (!member)
                continue;
            Role otherRole = LivingRolePolicy::Resolve(member->Type, ally->GetEntry(), IsService(*ally));
            bool allied = LivingRolePolicy::CanAssistAlly(record.WorldFaction, member->WorldFaction,
                creature.IsHostileTo(ally) || ally->IsHostileTo(&creature));
            bool herd = role == Role::Prey && otherRole == Role::Prey && LivingRolePolicy::SameHerd(creature.GetEntry(), ally->GetEntry());
            if (extensions && !threat && !LivingRolePolicy::Fighter(role))
            {
                if (role == Role::Prey && otherRole == Role::Predator && ally->IsValidAttackTarget(&creature) &&
                    creature.IsWithinDistInMap(ally, LivingRolePolicy::NoticeRadius(record.Id.Value)))
                {
                    threat = ally;
                    state.Awareness = "PREDATOR_SEEN";
                }
                else if (role != Role::Prey && (otherRole == Role::Predator || otherRole == Role::Combatant) &&
                    (ally->IsHostileTo(&creature) || creature.IsHostileTo(ally)) && ally->IsValidAttackTarget(&creature) &&
                    creature.IsWithinDistInMap(ally, LivingRolePolicy::NoticeRadius(record.Id.Value)))
                {
                    threat = ally;
                    state.Awareness = "HOSTILE_APPROACH";
                }
                // A herd mate may communicate its own freshly seen danger;
                // each recipient still has to see the actual source itself.
                if (herd && IsEscaping(member->LivingRole.CurrentPhase))
                    if (Unit* source = ObjectAccessor::GetUnit(creature, member->LivingRole.TargetGuid);
                        !threat && visibleDanger(source))
                    {
                        threat = source;
                        state.Awareness = "HERD_ALARM";
                    }
            }
            if (extensions && state.CompanionGuid.IsEmpty() && !ally->IsInCombat() &&
                member->Id.Value < record.Id.Value && !IsEscaping(member->LivingRole.CurrentPhase) &&
                (herd || (role == Role::Guard && otherRole == Role::Guard && allied)) &&
                ally->GetHomePosition().GetExactDist2d(&creature.GetHomePosition()) <= 18.0f)
            {
                // Guard pairs have at most one follower; herds may be wider.
                bool occupied = role == Role::Guard && !member->LivingRole.CompanionGuid.IsEmpty();
                if (role == Role::Guard)
                    for (Creature* neighbor : nearby)
                        if (AgentRecord const* follower = eligibleMember(neighbor))
                            if (follower->Id != record.Id && (follower->LivingRole.CompanionGuid == ally->GetGUID() ||
                                follower->LivingRole.CompanionGuid == creature.GetGUID()))
                                occupied = true;
                if (!occupied) state.CompanionGuid = ally->GetGUID();
            }
            if (!allied || (LivingRolePolicy::Fighter(role) && !LivingRolePolicy::HelpsAllies(role)))
                continue;
            ++state.NearbyAllies;
            if (extensions && role == Role::Guard && !alarmDestination &&
                nowMs >= state.NextInvestigationAtMs && nowMs < member->LivingRole.AlarmUntilMs)
                alarmDestination = member->LivingRole.DangerPosition;
            if (!ally->IsInCombat())
                continue;
            ++state.AlliesInCombat;
            if (threat)
                continue;
            auto assistableThreat = [&](Unit* attacker)
            {
                if (!LivingRolePolicy::Fighter(role) &&
                    (!attacker || !creature.IsWithinDistInMap(attacker, LivingRolePolicy::NoticeRadius(record.Id.Value) + 3.0f)))
                    return false;
                if (!(LivingRolePolicy::Fighter(role) ? validThreat(attacker) : visibleDanger(attacker)) ||
                    !ally->IsInCombatWith(attacker))
                    return false;
                if (member->LivingRole.CurrentPhase == Phase::Hunting && member->LivingRole.TargetGuid == attacker->GetGUID())
                    return false;
                return !member->GroupCoordinationGoalState || member->GroupCoordinationGoalState->Type != GoalType::Hunt ||
                    member->GroupCoordinationGoalState->TargetGuid != attacker->GetGUID();
            };
            Unit* attacker = ally->GetThreatManager().GetCurrentVictim();
            if (!assistableThreat(attacker))
            {
                attacker = nullptr;
                for (auto const& combat : ally->GetCombatManager().GetPvECombatRefs())
                {
                    Unit* other = combat.second->GetOther(ally);
                    if (assistableThreat(other) && (!attacker || other->GetGUID() < attacker->GetGUID()))
                        attacker = other;
                }
            }
            if (!attacker)
                continue;
            threat = attacker;
            state.LastAssistStatus = "THREAT_FOUND";
            state.Awareness = "ALLY_IN_DANGER";
        }
        if (!threat && state.NearbyAllies)
            state.LastAssistStatus = state.AlliesInCombat ? "NO_VALID_THREAT" : "ALLIES_NOT_IN_COMBAT";
    }

    if (IsEscaping(state.CurrentPhase) && !threat &&
        !WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 8000))
    {
        Unit* source = ObjectAccessor::GetUnit(creature, state.TargetGuid);
        if (visibleDanger(source))
            threat = source;
    }

    if (extensions && threat && !ownPrey)
    {
        state.DangerPosition = { creature.GetMapId(), threat->GetPositionX(), threat->GetPositionY(), threat->GetPositionZ() };
        state.DangerUntilMs = nowMs + 60000;
        if (state.Awareness == std::string_view("QUIET") || state.Awareness == std::string_view("REMEMBERED_DANGER"))
            state.Awareness = "DIRECT_THREAT";
        if (!LivingRolePolicy::Wildlife(role) && nowMs >= state.NextAlarmAtMs)
        {
            state.AlarmThreatGuid = threat->GetGUID();
            state.AlarmUntilMs = nowMs + 15000;
            state.NextAlarmAtMs = nowMs + 20000;
        }
    }
    std::optional<ActionPosition> approvedRoleDestination;

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
        context.LivingRoleExtensionsAllowed = extensions;
        context.RoleMovementDestination = approvedRoleDestination;
        context.FreshAllyAlarm = alarmDestination.has_value();
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
            context.TargetWithinAttackRange = creature.IsWithinDistInMap(target, phase == Phase::Feeding ? 5.0f :
                request.AmbientActivity == Activity::Talk ? 6.0f : 30.0f);
            context.TargetInLineOfSight = creature.IsWithinLOSInMap(target);
            context.TargetIsRolePrey = target->GetTypeId() == TYPEID_UNIT &&
                _agentTypeCatalog.Resolve(target->GetEntry()) == AgentType::Prey && target->GetZoneId() == 12;
            if (AgentRecord const* partner = eligibleMember(target->ToCreature()))
                context.TargetIsSocialPartner = !target->IsInCombat() && target->IsStopped() &&
                    !LivingRolePolicy::Wildlife(LivingRolePolicy::Resolve(partner->Type, target->GetEntry(), false)) &&
                    LivingRolePolicy::CanAssistAlly(record.WorldFaction, partner->WorldFaction,
                        creature.IsHostileTo(target) || target->IsHostileTo(&creature));
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
        if (request.Type == ActionType::Attack && target && LivingHuntPolicy::UsesFormationBearing(request.SourceGoal))
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
                            if (other->LivingRole.CurrentPhase == Phase::Defending &&
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
            case ActionType::Flee: result = _actionExecutor.ExecuteFlee(request, creature, *target, 12); break;
            case ActionType::Eat: result = _actionExecutor.ExecuteEat(request, creature); break;
            case ActionType::Ambient: result = _actionExecutor.ExecuteAmbient(request, creature); break;
            default: return false;
        }
        if (result.Status != ActionExecutionStatus::Started)
            return false;
        ActiveGoal goal;
        goal.Type = request.SourceGoal;
        goal.Priority = phase == Phase::Defending || IsEscaping(phase) ? GoalPriority::Emergency : GoalPriority::Normal;
        goal.StartedAtMs = nowMs;
        record.ActiveGoalState = goal;
        ActiveAction action;
        action.Type = request.Type; action.SourceGoal = request.SourceGoal;
        action.GoalStartedAtMs = nowMs; action.StartedAtMs = nowMs;
        action.Destination = request.Destination; action.Target = request.Target;
        record.ActiveActionState = action;
        state.CurrentPhase = phase;
        if (IsEscaping(phase) || phase == Phase::Hunting || phase == Phase::Investigating)
        {
            state.ReturningHome = false;
            state.ReturnHomeLimit = 0.0f;
        }
        state.Activity = request.AmbientActivity;
        state.SourceGoal = request.SourceGoal;
        state.StartedAtMs = nowMs;
        state.MoveStart = { creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ() };
        state.TargetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
        if (request.ChaseAngleRadians) state.ChaseBearing = *request.ChaseAngleRadians;
        if (request.Destination) state.Destination = *request.Destination;
        else state.Destination = { creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ() };
        if (request.AmbientActivity == Activity::Rest) state.OwnedStandState = creature.GetStandState();
        if (request.AmbientActivity == Activity::Work)
            state.WorkWindowAtStart = nowMs - nowMs % _routineScheduleConfig.DayLengthMs + _routineScheduleConfig.WorkStartMs;
        TC_LOG_DEBUG("ai.world", "AI living role agent={} role={} goal={} activity={}",
            record.Id.Value, LivingRolePolicy::ToString(role), ToString(request.SourceGoal), LivingRolePolicy::ToString(request.AmbientActivity));
        return true;
    };

    bool minimumEscape = IsEscaping(state.CurrentPhase) && !WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 8000);
    bool flee = threat && (minimumEscape || nowMs < state.DefenseCooldownUntilMs ||
        (extensions ? LivingRolePolicy::ShouldFlee(role, record.Needs.HealthPressure, IsEscaping(state.CurrentPhase), record.Id.Value) :
            LivingRolePolicy::ShouldFlee(role, record.Needs.HealthPressure, IsEscaping(state.CurrentPhase))));
    if (threat && (!ownPrey || flee))
    {
        if (extensions && flee)
        {
            // Keep a valid route long enough to get somewhere. Replan only
            // when it ends, times out, or the threat has reached its refuge.
            if (state.CurrentPhase == Phase::SeekingSafety && state.TargetGuid == threat->GetGUID() &&
                record.ActiveActionState && OwnsRoleMovement(record, creature) &&
                creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE) &&
                !WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 8000) &&
                threat->GetExactDist2d(state.Destination.X, state.Destination.Y) > 6.0f)
                return true;
            if (state.CurrentPhase == Phase::Fleeing && state.TargetGuid == threat->GetGUID() && minimumEscape &&
                record.ActiveActionState && OwnsRoleMovement(record, creature) &&
                creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE))
                return true;

            loadNearby();
            std::vector<std::pair<ActionPosition, char const*>> refuges;
            float slotAngle = float(LivingRolePolicy::Personality(record.Id.Value) % 360) * GroupMemberFormation::TwoPi / 360.0f;
            if (!LivingRolePolicy::Wildlife(role))
                for (Creature* guard : nearby)
                {
                    AgentRecord const* helper = eligibleMember(guard);
                    if (guard == &creature || !helper || !guard->IsAlive() || !visibleDanger(guard) ||
                        LivingRolePolicy::Resolve(helper->Type, guard->GetEntry(), IsService(*guard)) != Role::Guard ||
                        !LivingRolePolicy::CanAssistAlly(record.WorldFaction, helper->WorldFaction,
                            creature.IsHostileTo(guard) || guard->IsHostileTo(&creature)))
                        continue;
                    float guardAngle = freeSlotBearing(*guard, slotAngle);
                    refuges.push_back({ { creature.GetMapId(), guard->GetPositionX() + 4.0f * std::cos(guardAngle),
                        guard->GetPositionY() + 4.0f * std::sin(guardAngle), guard->GetPositionZ() }, "GUARD_REFUGE" });
                    break;
                }
            Position const& home = creature.GetHomePosition();
            refuges.push_back({ { creature.GetMapId(), home.GetPositionX() + 2.0f * std::cos(slotAngle),
                home.GetPositionY() + 2.0f * std::sin(slotAngle), home.GetPositionZ() }, "HOME_REFUGE" });
            float away = threat->GetAbsoluteAngle(&creature);
            for (float distance : { 16.0f, 8.0f, 4.0f })
                for (float offset : { 0.0f, 0.55f, -0.55f, 1.05f, -1.05f })
                    refuges.push_back({ { creature.GetMapId(), creature.GetPositionX() + distance * std::cos(away + offset),
                        creature.GetPositionY() + distance * std::sin(away + offset), creature.GetPositionZ() }, "AWAY_FROM_DANGER" });
            ActionPosition source{ creature.GetMapId(), threat->GetPositionX(), threat->GetPositionY(), threat->GetPositionZ() };
            for (auto& refuge : refuges)
            {
                ActionPosition& destination = refuge.first;
                if (threat->GetExactDist2d(destination.X, destination.Y) < creature.GetExactDist2d(threat) + 2.0f ||
                    !CheckRolePath(creature, destination, &source, std::min(8.0f, creature.GetExactDist2d(threat))))
                    continue;
                approvedRoleDestination = destination;
                ActionRequest escape;
                escape.Type = ActionType::MoveTo; escape.SourceGoal = GoalType::SeekSafety; escape.Destination = destination;
                if (start(escape, Phase::SeekingSafety, threat))
                {
                    state.MovementPurpose = refuge.second;
                    return true;
                }
            }
            // The fallback generator also checks every launched path's zone.
            approvedRoleDestination.reset();
        }
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
        if (start(request, phase, threat) && flee) state.MovementPurpose = "BOUNDED_ESCAPE";
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
        auto endHunt = [&](char const* reason, uint64 retryMs = 10000)
        {
            StopLivingRole(record, creature);
            state.LastHuntStatus = state.LastHuntEnd = reason;
            state.NextDecisionAtMs = nowMs + retryMs;
        };
        char const* stopReason = nullptr;
        if (!prey) stopReason = "PREY_GONE";
        else if (prey->GetZoneId() != 12) stopReason = "PREY_OUTSIDE_ELWYNN";
        else if (!creature.IsWithinDistInMap(prey, 30.0f)) stopReason = "PREY_OUT_OF_RANGE";
        else if (!creature.IsWithinLOSInMap(prey)) stopReason = "PREY_LOST_LOS";
        else if (creature.GetDistance(state.Destination.X, state.Destination.Y, state.Destination.Z) > LivingHuntPolicy::LeashDistance)
            stopReason = "HUNT_LEASH";
        else if (WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 45000)) stopReason = "HUNT_TIMEOUT";
        if (stopReason)
        {
            endHunt(stopReason);
            return true;
        }
        if (state.CurrentPhase == Phase::Hunting)
        {
            if (prey->IsAlive())
            {
                if (!creature.IsValidAttackTarget(prey))
                {
                    endHunt("PREY_NOT_ATTACKABLE");
                    return true;
                }
                auto* chase = dynamic_cast<ChaseMovementGenerator*>(
                    creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE));
                auto motion = LivingHuntPolicy::EvaluateMotion(creature.GetVictim() == prey, creature.CanNotReachTarget(),
                    creature.HasUnitState(UNIT_STATE_NOT_MOVE) || creature.IsMovementPreventedByCasting(),
                    chase && chase->GetTarget() == prey && OwnsRoleMovement(record, creature), creature.IsWithinMeleeRange(prey));
                state.LastHuntStatus = LivingHuntPolicy::ToString(motion);
                if (!LivingHuntPolicy::CanContinue(motion))
                {
                    if (motion == LivingHuntPolicy::MotionState::PathBlocked)
                    {
                        state.UnreachablePreyGuid = prey->GetGUID();
                        state.UnreachablePreyUntilMs = nowMs + 30000;
                    }
                    endHunt(LivingHuntPolicy::ToString(motion), 2000);
                }
                return true;
            }
            ObjectGuid meal = prey->GetGUID();
            endHunt("PREY_DIED");
            if (creature.IsWithinDistInMap(prey, 5.0f) && !creature.IsInCombat())
            {
                state.TargetGuid = meal;
                ActionRequest feed;
                feed.Type = ActionType::Eat; feed.SourceGoal = GoalType::Feed;
                state.LastHuntStatus = start(feed, Phase::Feeding, prey) ? "FEEDING" : "FEED_REJECTED";
            }
            else
                state.LastHuntEnd = state.LastHuntStatus = creature.IsInCombat() ? "FEED_COMBAT_BLOCKED" : "CORPSE_TOO_FAR";
            return true;
        }
        if (prey->IsAlive() || creature.IsInCombat() || !creature.IsWithinDistInMap(prey, 5.0f) || !creature.IsStopped())
        {
            endHunt("FEED_INTERRUPTED");
            return true;
        }
        if (WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, 5000))
        {
            _needsSystem.SatisfyHunger(record.Needs);
            state.ForageUntilMs = 0;
            state.HasForageWaypoint = false;
            state.NextForageAtMs = nowMs + LivingForagePolicy::CooldownMs;
            endHunt("FED");
            ActionRequest rest;
            rest.Type = ActionType::Ambient; rest.SourceGoal = GoalType::LocalActivity; rest.AmbientActivity = Activity::Rest;
            start(rest, Phase::Acting);
        }
        return true;
    }
    if (extensions && alarmDestination && !creature.IsInCombat() && nowMs >= state.NextInvestigationAtMs)
    {
        state.NextInvestigationAtMs = nowMs + 20000;
        if (creature.GetExactDist2d(alarmDestination->X, alarmDestination->Y) > 4.0f &&
            creature.GetHomePosition().GetExactDist2d(alarmDestination->X, alarmDestination->Y) <= 35.0f &&
            CheckRolePath(creature, *alarmDestination))
        {
            approvedRoleDestination = alarmDestination;
            ActionRequest investigate;
            investigate.Type = ActionType::MoveTo; investigate.SourceGoal = GoalType::InvestigateDanger;
            investigate.Destination = alarmDestination;
            if (start(investigate, Phase::Investigating))
            {
                state.MovementPurpose = "CHECK_ALLY_ALARM";
                return true;
            }
        }
    }

    if (IsRoleMove(state.CurrentPhase))
    {
        if (record.ActiveActionState && creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE) &&
            OwnsRoleMovement(record, creature) &&
            !WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, state.CurrentPhase == Phase::SeekingSafety ? 8000 : 20000))
            return true;
        bool returningHome = std::string_view(state.MovementPurpose) == "RETURN_HOME";
        bool foraging = std::string_view(state.MovementPurpose) == "FORAGE_SEARCH";
        bool progressed = creature.GetExactDist(state.MoveStart.X, state.MoveStart.Y, state.MoveStart.Z) > 1.0f;
        StopLivingRole(record, creature);
        if (returningHome)
        {
            if (progressed)
            {
                state.ReturnFailures = 0;
                state.ReturnRetryAtMs = state.ReturnStalledSinceMs = 0;
                state.ReturnFailure = "NONE";
            }
            else FailedReturn(state, nowMs, "RETURN_NO_PROGRESS", here);
        }
        if (foraging && !progressed) state.HasForageWaypoint = false;
        state.NextDecisionAtMs = nowMs + (returningHome || foraging ? 1000 : 6000);
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
        uint64 duration = state.Activity == Activity::Rest ? 20000 : extensions && state.Activity == Activity::Work ? 15000 : 5000;
        if (!WolfBehaviorPolicy::Elapsed(nowMs, state.StartedAtMs, duration))
            return true;
        if (state.Activity == Activity::Graze || state.Activity == Activity::Eat)
        {
            // A curated stock meal is not free food if the stock was removed
            // during the animation. Interrupted animations never reach here.
            bool fed = !state.StockMeal;
            if (extensions && state.Activity == Activity::Eat && record.EconomyState.Food)
                MutateEconomyAndPersist(record, [&](AgentEconomyState& economy)
                { fed = LivingRolePolicy::ConsumeStockMeal(economy); });
            if (fed) _needsSystem.SatisfyHunger(record.Needs);
        }
        if (extensions && state.Activity == Activity::Work)
        {
            uint64 window = nowMs - nowMs % _routineScheduleConfig.DayLengthMs + _routineScheduleConfig.WorkStartMs;
            uint64 dayTime = nowMs % _routineScheduleConfig.DayLengthMs;
            if (state.GatheringFood && record.WorkLocation &&
                record.WorkLocation->MapId == creature.GetMapId() &&
                creature.GetExactDist(record.WorkLocation->X, record.WorkLocation->Y, record.WorkLocation->Z) <= 3.0f &&
                LivingRolePolicy::CanGatherFood(role, creature.GetEntry()))
                MutateEconomyAndPersist(record, [](AgentEconomyState& economy)
                { LivingRolePolicy::GatherEmergencyFood(economy); });
            else if (!state.GatheringFood && state.WorkWindowAtStart == window && dayTime >= _routineScheduleConfig.WorkStartMs &&
                dayTime < _routineScheduleConfig.WorkEndMs && record.EconomyState.LastRewardedWorkWindowId < window)
                MutateEconomyAndPersist(record, [&](AgentEconomyState& economy)
                { LivingRolePolicy::ProduceWorkStock(economy, creature.GetEntry(), window); });
        }
        if (state.Activity == Activity::Rest)
            record.Needs.Fatigue = 0.0f;
        StopLivingRole(record, creature);
        state.NextDecisionAtMs = nowMs + 8000 + record.Id.Value % 7000;
        return true;
    }

    // Fulfil existing needs between commute/work attempts, then hand control
    // back to the prepared routine. Never interrupt a running owned action.
    if (record.HomeLocation && record.WorkLocation)
    {
        if (extensions && !record.GroupCoordinationGoalState && !record.ActiveActionState &&
            !creature.IsInCombat() && creature.IsStopped() && nowMs >= state.NextDecisionAtMs)
        {
            if (!record.EconomyState.Food && record.Needs.Hunger >= 0.65f &&
                LivingRolePolicy::CanGatherFood(role, creature.GetEntry()))
            {
                auto const& work = *record.WorkLocation;
                if (work.MapId == creature.GetMapId() && creature.GetExactDist(work.X, work.Y, work.Z) <= 3.0f)
                {
                    ActionRequest gather;
                    gather.Type = ActionType::Ambient; gather.SourceGoal = GoalType::LocalActivity;
                    gather.AmbientActivity = Activity::Work;
                    if (start(gather, Phase::Acting))
                    {
                        state.GatheringFood = true;
                        state.MovementPurpose = "FOOD_SUPPLY";
                        return true;
                    }
                }
                else if (auto step = FoodSupplyStep(creature, {work.MapId, work.X, work.Y, work.Z}))
                {
                    ActionRequest move;
                    move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity; move.Destination = step;
                    if (start(move, Phase::Moving))
                    {
                        state.MovementPurpose = "FOOD_SUPPLY";
                        return true;
                    }
                }
                state.NextDecisionAtMs = nowMs + 30000;
                state.MovementPurpose = "FOOD_SUPPLY_BLOCKED";
            }
            Activity care = LivingRolePolicy::CuratedSelfCare(role, record.Needs.Hunger, record.Needs.Fatigue, record.EconomyState.Food);
            if (care != Activity::None)
            {
                ActionRequest request;
                request.Type = ActionType::Ambient; request.SourceGoal = GoalType::LocalActivity; request.AmbientActivity = care;
                if (start(request, Phase::Acting))
                {
                    state.StockMeal = care == Activity::Eat;
                    return true;
                }
            }
        }
        return false;
    }
    if (record.GroupCoordinationGoalState)
        return false;
    if (nowMs < state.NextDecisionAtMs || creature.IsInCombat() || !OwnsRoleMovement(record, creature))
        return true;
    state.NextDecisionAtMs = nowMs + 10000;
    uint64 cycle = ++state.Cycle + record.Id.Value;

    bool hungryHunter = role == Role::Predator && WolfBehaviorPolicy::WantsHunt(record.Needs.Hunger, record.Needs.HealthPressure, false);
    if (state.ForageUntilMs && (!extensions || !hungryHunter || nowMs >= state.ForageUntilMs))
    {
        state.ForageUntilMs = 0;
        state.HasForageWaypoint = false;
        state.NextForageAtMs = nowMs + LivingForagePolicy::CooldownMs;
    }

    // Also scan the current surroundings during a bounded search/recovery;
    // prey still has to be actually visible, reachable and attackable.
    if (hungryHunter && (homeDistance < 20.0f ||
        ((state.ForageUntilMs || state.ReturnFailures) && homeDistance <= LivingForagePolicy::HomeRadius)))
    {
        loadNearby();
        state.NearbyPrey = state.AttackablePrey = 0;
        state.LastHuntStatus = "NO_PREY";
        bool actionRejected = false;
        bool preyOnCooldown = false;
        std::vector<Creature*> candidates;
        for (Creature* candidate : nearby)
        {
            if (_agentTypeCatalog.Resolve(candidate->GetEntry()) != AgentType::Prey || !candidate->IsAlive() ||
                candidate->IsPet() || !candidate->GetCharmerOrOwnerGUID().IsEmpty() || candidate->IsControlledByPlayer() ||
                IsService(*candidate) || candidate->IsQuestGiver() || candidate->GetZoneId() != 12)
                continue;
            ++state.NearbyPrey;
            if (!creature.IsValidAttackTarget(candidate))
                continue;
            ++state.AttackablePrey;
            if (!validThreat(candidate))
                continue;
            if (candidate->GetGUID() == state.UnreachablePreyGuid && nowMs < state.UnreachablePreyUntilMs)
            {
                preyOnCooldown = true;
                continue;
            }
            candidates.push_back(candidate);
        }
        if (extensions)
            std::stable_sort(candidates.begin(), candidates.end(), [&](Creature* a, Creature* b)
            {
                return LivingRolePolicy::PreyScore(creature.GetExactDist2d(a), a->GetHealthPct(), a->GetEntry(), creature.GetEntry()) <
                    LivingRolePolicy::PreyScore(creature.GetExactDist2d(b), b->GetHealthPct(), b->GetEntry(), creature.GetEntry());
            });
        // One blocked nearest animal must not hide reachable prey behind it.
        if (candidates.size() > 8) candidates.resize(8);
        for (Creature* candidate : candidates)
        {
            PathGenerator path(&creature);
            if (path.CalculatePath(candidate->GetPositionX(), candidate->GetPositionY(), candidate->GetPositionZ(), false) &&
                !(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)))
            {
                ActionRequest hunt;
                hunt.Type = ActionType::Attack; hunt.SourceGoal = GoalType::PredatorHunt;
                if (start(hunt, Phase::Hunting, candidate))
                {
                    state.LastHuntStatus = "HUNT_STARTED";
                    state.LastHuntTargetGuid = candidate->GetGUID();
                    return true;
                }
                state.LastHuntStatus = "ACTION_REJECTED";
                actionRejected = true;
            }
        }
        if (!actionRejected)
            state.LastHuntStatus = candidates.empty() && preyOnCooldown ? "PREY_RETRY_DELAY" :
                state.AttackablePrey ? "NO_REACHABLE_PREY" :
                state.NearbyPrey ? "PREY_NOT_ATTACKABLE" : "NO_PREY";
    }

    ActionPosition const* rememberedDanger = extensions && nowMs < state.DangerUntilMs ? &state.DangerPosition : nullptr;
    if (extensions && hungryHunter && !anchored && !rememberedDanger)
    {
        if (!state.ForageUntilMs && homeDistance <= radius + 2.0f && nowMs >= state.NextForageAtMs &&
            !state.ReturnFailures && !state.ReturningHome)
        {
            state.ForageUntilMs = nowMs + LivingForagePolicy::DurationMs;
            state.HasForageWaypoint = false;
        }
        if (state.ForageUntilMs)
        {
            ActionPosition origin{creature.GetMapId(), home.GetPositionX(), home.GetPositionY(), home.GetPositionZ()};
            ActionPosition from{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
            for (uint32 attempt = 0; attempt < 3; ++attempt)
            {
                if (!state.HasForageWaypoint || creature.GetExactDist2d(state.ForageWaypoint.X, state.ForageWaypoint.Y) <= 3.0f)
                {
                    state.ForageWaypoint = LivingForagePolicy::Waypoint(origin, record.Id.Value, state.ForageLeg++);
                    state.HasForageWaypoint = true;
                }
                auto step = LivingForagePolicy::Step(from, state.ForageWaypoint, origin);
                if (step && CheckRolePath(creature, *step, nullptr, 8.0f, nullptr, &home, LivingForagePolicy::HomeRadius))
                {
                    ActionRequest search;
                    search.Type = ActionType::MoveTo; search.SourceGoal = GoalType::LocalActivity; search.Destination = step;
                    if (start(search, Phase::Moving))
                    {
                        state.MovementPurpose = "FORAGE_SEARCH";
                        return true;
                    }
                }
                state.HasForageWaypoint = false;
            }
            state.ForageUntilMs = 0;
            state.NextForageAtMs = nowMs + LivingForagePolicy::CooldownMs;
            state.LastHuntStatus = "FORAGE_NO_PATH";
        }
    }

    uint32 dayTime = uint32(nowMs % _routineScheduleConfig.DayLengthMs);
    bool workHours = dayTime >= _routineScheduleConfig.WorkStartMs && dayTime < _routineScheduleConfig.WorkEndMs;
    Activity activity = LivingRolePolicy::IdleActivity(role, cycle, workHours, record.Needs.Hunger, record.Needs.Fatigue);
    if (anchored && (activity == Activity::Roam || activity == Activity::Rest)) activity = Activity::Look;
    if (rememberedDanger && activity == Activity::Rest) activity = Activity::Look;

    auto recoverHere = [&]()
    {
        ActionRequest watch;
        watch.Type = ActionType::Ambient; watch.SourceGoal = GoalType::LocalActivity;
        watch.AmbientActivity = LivingRolePolicy::RecoveryActivity(role, record.Needs.Hunger, rememberedDanger != nullptr);
        if (watch.AmbientActivity == Activity::Rest && creature.GetStandState() != UNIT_STAND_STATE_STAND)
            watch.AmbientActivity = Activity::Look;
        start(watch, Phase::Acting);
        // Retain the precise failure even while grazing/resting between tries.
        state.MovementPurpose = state.ReturnFailure;
    };

    // Local cohesion is not a persisted coalition. Only a visible, compatible
    // lower-id neighbor can lead; home bounds prevent a chain across the map.
    if (extensions && !anchored && !rememberedDanger && !state.CompanionGuid.IsEmpty() && cycle % 3 == 0)
    {
        Creature* companion = ObjectAccessor::GetCreature(creature, state.CompanionGuid);
        AgentRecord const* member = eligibleMember(companion);
        if (member && companion->IsAlive() && !companion->IsInCombat() && visibleDanger(companion) &&
            !IsEscaping(member->LivingRole.CurrentPhase) && creature.GetExactDist2d(companion) > 6.0f &&
            home.GetExactDist2d(companion) <= 18.0f)
        {
            float angle = freeSlotBearing(*companion, companion->GetAbsoluteAngle(&creature));
            ActionPosition slot{ creature.GetMapId(), companion->GetPositionX() + 4.0f * std::cos(angle),
                companion->GetPositionY() + 4.0f * std::sin(angle), companion->GetPositionZ() };
            if (CheckRolePath(creature, slot))
            {
                ActionRequest move;
                move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity; move.Destination = slot;
                if (start(move, Phase::Moving))
                {
                    state.MovementPurpose = role == Role::Prey ? "HERD_COHESION" : "PATROL_COMPANION";
                    return true;
                }
            }
        }
    }
    if (activity == Activity::Roam || homeDistance > radius + 2.0f)
    {
        bool returningHome = homeDistance > radius + 2.0f;
        ActionPosition destination{ creature.GetMapId(), home.GetPositionX(), home.GetPositionY(), home.GetPositionZ() };
        char const* failure = "LOCAL_PATH_BLOCKED";
        bool pathReady;
        if (returningHome)
        {
            if (!state.ReturningHome)
            {
                state.ReturningHome = true;
                state.ReturnHomeLimit = homeDistance + 16.0f;
            }
            if (nowMs < state.ReturnRetryAtMs)
            {
                recoverHere();
                return true;
            }
            auto step = FindRoleReturnStep(creature, home, rememberedDanger, failure, state);
            pathReady = step.has_value();
            if (step) destination = *step;
        }
        else
        {
            float angle = float((cycle * 137) % 360) * GroupMemberFormation::TwoPi / 360.0f;
            destination.X += radius * std::cos(angle);
            destination.Y += radius * std::sin(angle);
            pathReady = CheckRolePath(creature, destination, rememberedDanger);
        }
        // Waiting near a refuge is preferable to immediately walking back
        // through the just-witnessed fight. Memory expires after a minute.
        if (!pathReady)
        {
            if (returningHome)
            {
                FailedReturn(state, nowMs, failure, here);
                auto const& diagnostic = state.ReturnDiagnostics;
                TC_LOG_DEBUG("ai.world", "AI role return agent={} failure={} attempts={} strategy={} retryMs={} candidates={} height={} los={} path={} bounds={} pathType={} sourceZ={} requestedZ={} resolvedZ={}",
                    record.Id.Value, failure, state.ReturnFailures, state.ReturnStrategy, state.ReturnRetryAtMs - nowMs,
                    diagnostic.Candidates, diagnostic.Rejected[LivingReturnPolicy::Height], diagnostic.Rejected[LivingReturnPolicy::Los],
                    diagnostic.Rejected[LivingReturnPolicy::Path], diagnostic.Rejected[LivingReturnPolicy::Bounds], diagnostic.PathType,
                    creature.GetPositionZ(), diagnostic.RequestedZ, diagnostic.ResolvedZ.value_or(INVALID_HEIGHT));
                recoverHere();
                return true;
            }
            if (rememberedDanger)
            {
                ActionRequest watch;
                watch.Type = ActionType::Ambient; watch.SourceGoal = GoalType::LocalActivity; watch.AmbientActivity = Activity::Look;
                if (start(watch, Phase::Acting)) state.Awareness = "WAITING_FOR_SAFETY";
            }
            state.MovementPurpose = failure;
            return true;
        }
        ActionRequest move;
        move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity; move.Destination = destination;
        if (start(move, Phase::Moving)) state.MovementPurpose = returningHome ? "RETURN_HOME" : "LOCAL_ROAM";
        else if (returningHome)
        {
            FailedReturn(state, nowMs, "RETURN_MOVE_REJECTED", here);
            recoverHere();
        }
        else state.MovementPurpose = "LOCAL_MOVE_REJECTED";
        return true;
    }
    ActionRequest ambient;
    ambient.Type = ActionType::Ambient; ambient.SourceGoal = GoalType::LocalActivity; ambient.AmbientActivity = activity;
    Unit* partner = nullptr;
    if (extensions && activity == Activity::Talk)
    {
        loadNearby();
        for (Creature* neighbor : nearby)
        {
            AgentRecord const* member = eligibleMember(neighbor);
            if (neighbor != &creature && member && neighbor->IsAlive() && !neighbor->IsInCombat() &&
                neighbor->IsStopped() && creature.IsWithinDistInMap(neighbor, 6.0f) && visibleDanger(neighbor) &&
                !LivingRolePolicy::Wildlife(LivingRolePolicy::Resolve(member->Type, neighbor->GetEntry(), IsService(*neighbor))) &&
                LivingRolePolicy::CanAssistAlly(record.WorldFaction, member->WorldFaction,
                    creature.IsHostileTo(neighbor) || neighbor->IsHostileTo(&creature)))
            {
                partner = neighbor;
                break;
            }
        }
        // An NPC talks to someone actually present, otherwise looks around.
        if (!partner) ambient.AmbientActivity = Activity::Look;
    }
    start(ambient, Phase::Acting, partner);
    return true;
}
