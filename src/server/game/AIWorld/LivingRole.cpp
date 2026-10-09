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
#include "Scheduler/PlanningWorkBudget.h"
#include "Agent/LivingHuntPolicy.h"
#include "Agent/LivingReturnPolicy.h"
#include "Agent/LivingReturnAdvice.h"
#include "Agent/LivingRecoveryPath.h"
#include "Agent/LivingRolePolicy.h"
#include "Agent/GroupMemberFormation.h"
#include "Agent/WolfBehaviorPolicy.h"
#include "ChaseMovementGenerator.h"
#include "ElwynnHuntPath.h"
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
        LivingReturnPolicy::Diagnostics* diagnostics = nullptr, bool rejoin = false, bool surfaceCorridor = false)
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
            diagnostics->Navigation = {};
        }
        if (destination.MapId != creature.GetMapId() || !std::isfinite(destination.X) ||
            !std::isfinite(destination.Y) || !std::isfinite(destination.Z) ||
            creature.GetExactDist2d(destination.X, destination.Y) > 30.0f)
            return reject("RETURN_INVALID_STEP", LivingReturnPolicy::Invalid);
        Map* map = creature.GetMap();
        // A navmesh/observed point already carries its floor. Looking down
        // from a different height can silently replace it with another floor.
        // New roaming proposals, however, still need a nearby ground height.
        // A valid recovery point in water keeps its swimming height. Looking
        // down to the river bed can replace it with an unrelated far polygon.
        if (!routePoint && !(diagnostics && LivingRecoveryPath::InSwimmableWater(creature, destination)))
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
        if (diagnostics)
        {
            if (!homeBound) return reject("RETURN_INVALID_STEP", LivingReturnPolicy::Invalid);
            diagnostics->QueryDestination = destination;
            RecoveryMovement recovery{destination, {creature.GetMapId(), homeBound->GetPositionX(),
                homeBound->GetPositionY(), homeBound->GetPositionZ()}, homeRadius,
                danger ? std::optional<ActionPosition>(*danger) : std::nullopt, rejoin};
            recovery.DangerRadius = clearance;
            recovery.SurfaceCorridor = surfaceCorridor;
            Movement::PointsArray points;
            if (!LivingRecoveryPath::Build(creature, recovery, points, diagnostics))
                return reject("RETURN_NO_PATH", LivingReturnPolicy::Path);
            auto const& end = points.back();
            destination.X = end.x; destination.Y = end.y; destination.Z = end.z;
            diagnostics->ResolvedZ = end.z;
            if (!LivingReturnPolicy::UsefulStep({creature.GetMapId(), creature.GetPositionX(),
                creature.GetPositionY(), creature.GetPositionZ()}, destination))
                return reject("RETURN_ZERO_STEP", LivingReturnPolicy::Invalid);
            return true;
        }
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
        if (!LivingReturnPolicy::UsefulStep(
            {creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()}, destination))
            return reject("RETURN_ZERO_STEP", LivingReturnPolicy::Invalid);
        return true;
    }

    std::optional<ActionPosition> FindRoleReturnStep(Creature& creature, Position const& home,
        ActionPosition const* danger, char const*& failure, LivingRoleState& state, float clearance, float arrivalRadius, uint64 nowMs)
    {
        using Stage = LivingReturnSearch::Stage;
        auto& search = state.ReturnSearch;
        if (!search.Started)
        {
            search.Started = true;
            state.ReturnDiagnostics = {};
            state.ReturnRoute.TrailTarget.reset();
            state.ReturnRoute.PendingBacktrack.reset();
        }
        state.ReturnDiagnostics.Deferred = false;
        failure = "RETURN_NO_PATH";
        ActionPosition current{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
        ActionPosition homePoint{current.MapId, home.GetPositionX(), home.GetPositionY(), home.GetPositionZ()};
        auto yield = [&]()
        { state.ReturnDiagnostics.Deferred = true; failure = "RETURN_PLANNING_DEFERRED"; };
        auto accept = [&](ActionPosition& step, bool routePoint, bool rejoin = false,
            std::optional<ActionPosition> trailPoint = std::nullopt, bool completeCorridor = false,
            bool surfaceCorridor = false)
        {
            if (state.ReturnRoute.Failed(current, step))
            {
                failure = "RETURN_FAILED_EDGE";
                ++state.ReturnDiagnostics.Rejected[LivingReturnPolicy::Invalid];
                return false;
            }
            auto work = PlanningWorkBudget::TryAcquire();
            if (!work) { yield(); return false; }
            if (!CheckRolePath(creature, step, danger, clearance, &failure, &home,
                state.ReturnHomeLimit, routePoint, &state.ReturnDiagnostics, rejoin, surfaceCorridor)) return false;
            if (rejoin)
            {
                if (state.ReturnRoute.Allows(current, step, false, true)) return true;
                failure = "RETURN_REJOIN_EXHAUSTED";
                ++state.ReturnDiagnostics.Rejected[LivingReturnPolicy::Invalid];
                return false;
            }
            if (!completeCorridor && state.ReturnRoute.Revisited(step))
            {
                if (!search.Backtrack && state.ReturnRoute.Allows(current, step, false, false))
                {
                    search.Backtrack = step; search.BacktrackTrail = trailPoint;
                    search.BacktrackDiagnostics = state.ReturnDiagnostics;
                }
                failure = "RETURN_REPEATED_STEP";
                ++state.ReturnDiagnostics.Rejected[LivingReturnPolicy::Invalid];
                return false;
            }
            return true;
        };
        auto installCorridor = [&](std::vector<ActionPosition> corridor)
        {
            state.ReturnRoute.Planned = std::move(corridor);
            state.ReturnRoute.SurfaceCorridor = state.ReturnDiagnostics.HomePath.SurfaceCorridor;
            state.ReturnRoute.Advance(current);
            // Retain the complete proof if execution validation must yield.
            search.Advance(Stage::Corridor);
        };
        // The same stage runner is used by the pipeline regression tests.
        // Cheap, validated alternatives precede a foreground terrain graph.
        return search.Continue([&](Stage stage) -> std::optional<ActionPosition>
        {
            if (stage == Stage::Corridor)
            {
                state.ReturnRoute.Advance(current);
                if (!state.ReturnRoute.Planned.empty())
                {
                    auto step = state.ReturnRoute.Planned.front();
                    state.ReturnStrategy = "CORRIDOR";
                    if (accept(step, true, false, std::nullopt, true, state.ReturnRoute.SurfaceCorridor)) return step;
                    if (state.ReturnDiagnostics.Deferred) return std::nullopt;
                    state.ReturnRoute.Planned.clear();
                    state.ReturnRoute.SurfaceCorridor = false;
                }
            }
            else if (stage == Stage::Home)
            {
                auto corridor = LivingRecoveryPath::HomeCorridor(creature, current, homePoint,
                    arrivalRadius, state.ReturnHomeLimit, danger, clearance, &state.ReturnDiagnostics, &search.Home);
                if (state.ReturnDiagnostics.Deferred) { yield(); return std::nullopt; }
                if (!corridor.empty()) installCorridor(std::move(corridor));
            }
            else if (stage == Stage::SurfaceDetour)
            {
                state.ReturnStrategy = "SURFACE_DETOUR";
                if (!search.CanContinueHomeDetour(nowMs))
                {
                    // Give the other return strategies their turn. A later
                    // retry at this exact physical context resumes the bounded
                    // graph instead of rechecking its first edges forever.
                    search.Home.Report.DetourFailure = "SURFACE_DETOUR_SLICE_LIMIT";
                    state.ReturnDiagnostics.HomePath = search.Home.Report;
                    return std::nullopt;
                }
                auto corridor = LivingRecoveryPath::SurfaceDetour(creature, search.Home, &state.ReturnDiagnostics);
                if (state.ReturnDiagnostics.Deferred) { yield(); return std::nullopt; }
                if (!corridor.empty()) installCorridor(std::move(corridor));
            }
            else if (stage == Stage::Trail)
            {
                state.ReturnStrategy = "TRAIL";
                if (search.Trials.empty())
                    for (auto const& point : LivingReturnPolicy::TrailSteps(state.ReturnTrail, current))
                        search.Trials.push_back({point, true});
                while (search.Next < search.Trials.size())
                {
                    auto original = search.Trials[search.Next].Destination, step = original;
                    if (accept(step, true, false, original))
                    {
                        state.ReturnRoute.FollowingTrail = true;
                        state.ReturnRoute.TrailTarget = original; state.ReturnRoute.TrailDestination = step;
                        return step;
                    }
                    if (state.ReturnDiagnostics.Deferred) return std::nullopt;
                    ++search.Next;
                }
            }
            else if (stage == Stage::Direct || stage == Stage::Detour)
            {
                bool direct = stage == Stage::Direct;
                state.ReturnStrategy = direct ? "HOME_PATH" : "DETOUR";
                if (search.Trials.empty())
                {
                    if (direct)
                    {
                        // Try short steps before committing the actor to a
                        // complete home-route or terrain-graph calculation.
                        for (float budget : {LivingReturnPolicy::MaxStepLength, 14.0f, 7.0f, 3.5f, 1.75f})
                            if (auto step = LivingReturnPolicy::PathStep({current, homePoint}, budget))
                                search.Trials.push_back({*step, false});
                    }
                    else if (state.ReturnFailures)
                        for (auto const& step : LivingReturnPolicy::Detours(current, homePoint, ++state.ReturnSearchSequence))
                            search.Trials.push_back({step, false});
                }
                while (search.Next < search.Trials.size())
                {
                    auto step = search.Trials[search.Next].Destination;
                    if (accept(step, search.Trials[search.Next].RoutePoint)) return step;
                    if (state.ReturnDiagnostics.Deferred) return std::nullopt;
                    ++search.Next;
                }
            }
            else if (stage == Stage::Rejoin)
            {
                bool deferred = false;
                auto candidates = LivingRecoveryPath::RejoinPositions(creature, &state.ReturnDiagnostics.Navigation,
                    &search.Probes, &deferred);
                if (deferred) { yield(); return std::nullopt; }
                state.ReturnStrategy = "NAV_REJOIN";
                while (search.Next < candidates.size())
                {
                    auto step = candidates[search.Next];
                    if (!search.ConnectorReady)
                    {
                        if (!LivingReturnPolicy::UsefulStep(current, step) || !accept(step, true, true))
                        {
                            if (state.ReturnDiagnostics.Deferred) return std::nullopt;
                            ++search.Next; search.Continuation = {}; continue;
                        }
                        search.ConnectorReady = true;
                        search.Connector = step;
                    }
                    if (search.Connector) step = *search.Connector;
                    LivingReturnPolicy::Diagnostics continuationDiagnostics;
                    auto continuation = LivingRecoveryPath::HomeCorridor(creature, step, homePoint,
                        arrivalRadius, state.ReturnHomeLimit, danger, clearance, &continuationDiagnostics, &search.Continuation);
                    state.ReturnDiagnostics.ContinuationPath = continuationDiagnostics.HomePath;
                    if (continuationDiagnostics.Deferred) { yield(); return std::nullopt; }
                    if (state.ReturnRoute.PlanRejoin(current, step, std::move(continuation),
                        continuationDiagnostics.HomePath.SurfaceCorridor))
                    {
                        return step;
                    }
                    failure = "RETURN_REJOIN_NO_CONTINUATION";
                    ++state.ReturnDiagnostics.Rejected[LivingReturnPolicy::Path];
                    ++search.Next; search.ConnectorReady = false; search.Connector.reset(); search.Continuation = {};
                }
            }
            else if (stage == Stage::Backtrack)
            {
                search.Advance(Stage::Done);
                if (search.Backtrack)
                {
                    state.ReturnStrategy = "BACKTRACK";
                    search.BacktrackDiagnostics.Candidates = state.ReturnDiagnostics.Candidates;
                    search.BacktrackDiagnostics.Rejected = state.ReturnDiagnostics.Rejected;
                    state.ReturnDiagnostics = search.BacktrackDiagnostics;
                    state.ReturnRoute.PendingBacktrack = LivingReturnPolicy::RouteMemory::Backtrack{current, *search.Backtrack};
                    state.ReturnRoute.TrailTarget = search.BacktrackTrail;
                    if (search.BacktrackTrail)
                    {
                        state.ReturnRoute.FollowingTrail = true;
                        state.ReturnRoute.TrailDestination = *search.Backtrack;
                    }
                    return search.Backtrack;
                }
            }
            return std::nullopt;
        }, [&] { return state.ReturnDiagnostics.Deferred; });
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
    info.ArrivalRadius = ((IsService(creature) || creature.IsQuestGiver()) ? 0.0f : LivingRolePolicy::RoamRadius(role)) + 2;
    info.ReturnLimit = record->LivingRole.ReturnHomeLimit;
    info.RefugeActive = record->LivingRole.Refuge.Active(GetCurrentTimeMs());
    info.RefugeEpisodes = record->LivingRole.Refuge.Episodes;
    info.RefugeMoves = record->LivingRole.Refuge.Moves;
    info.RefugeBlocked = record->LivingRole.Refuge.Blocked;
    info.AdvicePilot = HasRecoveryAdvice(record->Id);
    info.AdviceEnabled = _recoveryAdviceEnabled;
    info.AdviceStatus = record->LivingRole.Advice.Status;
    info.AdviceRequests = record->LivingRole.Advice.Requests;
    info.AdviceStarted = record->LivingRole.Advice.Started;
    info.AdviceHomeSuccess = record->LivingRole.Advice.HomeSuccess;
    info.AdviceFoodSuccess = record->LivingRole.Advice.FoodSuccess;
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
    if (state.CurrentPhase == Phase::Acting && state.Planning.CarePause.Active)
    {
        uint64 nowMs = GetCurrentTimeMs();
        state.Planning.ResumeAfterCare(nowMs);
        state.Advice.Search.CarePause.End(nowMs, state.Advice.Search.ProgressAt);
        state.NextDecisionAtMs = nowMs;
    }
    if (IsRoleMove(state.CurrentPhase) && std::string_view(state.MoveWatchdog.End) == "MOVING")
        state.MoveWatchdog.End = creature.GetExactDist(state.Destination.X, state.Destination.Y, state.Destination.Z) <= 2.0f ?
            "ARRIVED" : "INTERRUPTED";
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
        _recoveryAdviceBudget.Cancel(record.Id.Value);
        _planningWork.Cancel(record.Id);
        if (!state.RuntimeGuid.IsEmpty())
        {
            StopLivingRole(record, creature);
            state = {};
        }
        return false;
    }
    if (state.RuntimeGuid != creature.GetGUID())
    {
        _recoveryAdviceBudget.Cancel(record.Id.Value);
        _planningWork.Cancel(record.Id);
        state = {};
        state.RuntimeGuid = creature.GetGUID();
        state.Advice.LifetimeAt = nowMs;
        state.NextDecisionAtMs = nowMs + LivingRolePolicy::PauseMs(record.Id.Value, 0, 0, 14999);
    }
    Position const home = creature.GetHomePosition();
    float homeDistance = creature.GetExactDist2d(home.GetPositionX(), home.GetPositionY());
    bool anchored = service || creature.IsQuestGiver();
    float radius = anchored ? 0.0f : LivingRolePolicy::RoamRadius(role);
    ActionPosition here{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
    auto& advice = state.Advice;
    bool freshlyHurt = record.Needs.HealthPressure > state.PreviousHealthPressure + 0.001f;
    state.PreviousHealthPressure = record.Needs.HealthPressure;
    ActionPosition homePoint{here.MapId, home.GetPositionX(), home.GetPositionY(), home.GetPositionZ()};
    if (advice.PendingId && (!advice.Fresh(nowMs, here, homePoint) || creature.IsInCombat() || !creature.IsAlive()))
    { ++advice.Rejected; advice.Status = "STALE"; advice.ClearPending(); }
    if (record.Needs.Hunger >= 0.95f && creature.IsAlive())
    { if (!state.HungrySinceMs) state.HungrySinceMs = nowMs; }
    else state.HungrySinceMs = 0;
    if (state.ReturningHome && homeDistance + 2.0f < state.BestHomeDistance)
    {
        state.BestHomeDistance = homeDistance; state.HomeProgressAtMs = nowMs;
        state.ReturnRoute.Backtracks.clear();
    }
    // A verified corridor may first lead away from home around an obstacle.
    // Consuming its actual reached legs is progress; Euclidean distance alone
    // must not repeatedly interrupt that route with a fresh model decision.
    if (state.ReturningHome && state.ReturnRoute.Advance(here))
        state.HomeProgressAtMs = nowMs;
    if (advice.Active)
    {
        bool huntingForFood = !advice.ActiveReturning &&
            (state.CurrentPhase == Phase::Hunting || state.CurrentPhase == Phase::Feeding);
        if (creature.IsInCombat() && !huntingForFood)
        { advice.Active.reset(); advice.Status = "INTERRUPTED"; }
    }
    if (advice.Active)
    {
        if (!advice.StepArrived && LivingReturnPolicy::Distance(here, advice.Active->Move.Destination) <=
            (advice.Active->Move.SurfaceCorridor ? 0.5f : 2.0f))
        { advice.StepArrived = true; ++advice.Arrived; advice.Status = "STEP_REACHED"; }
        bool homeReached = advice.ActiveReturning && advice.StepArrived && homeDistance <= radius+2;
        bool foodFound = !advice.ActiveReturning && advice.StepArrived && state.CurrentPhase == Phase::Feeding &&
            record.Needs.Hunger + 0.1f < advice.ActiveHunger;
        if (creature.IsAlive() && (homeReached || foodFound))
        {
            if (homeReached) { ++advice.HomeSuccess; advice.Status = "HOME_REACHED"; advice.RememberSuccess(); }
            else { ++advice.FoodSuccess; advice.Status = "FOOD_FOUND"; advice.Food.Fed(here, nowMs); advice.Active.reset(); }
        }
        else if (creature.IsAlive() && advice.ActiveReturning && advice.StepArrived && state.ReturningHome &&
            nowMs >= advice.ActiveAt + 30000 && nowMs >= state.HomeProgressAtMs + 30000)
        {
            state.ReturnRoute.MarkIneffective(advice.ActiveOrigin, advice.Active->Move.Destination);
            advice.Active.reset(); advice.Status = "INEFFECTIVE_STEP";
        }
        else if (!creature.IsAlive() || nowMs > advice.ActiveAt + 180000)
        {
            if (creature.IsAlive() && !advice.ActiveReturning)
                advice.CooldownUntil = nowMs + advice.Food.AdviceFailed();
            advice.Active.reset(); advice.Status = "OUTCOME_EXPIRED";
        }
    }
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
        state.Refuge = {};
        state.ReturningHome = false;
        state.ReturnStartedAtMs = state.HomeProgressAtMs = 0;
        state.ReturnHomeLimit = 0.0f;
        state.ReturnTrail.clear();
        state.ReturnRoute = {};
        state.ReturnFailures = 0;
        state.ReturnRetryAtMs = state.ReturnStalledSinceMs = 0;
        state.ReturnFailure = state.ReturnStrategy = "NONE";
    }
    if (state.ReturningHome && std::string_view(state.MovementPurpose) == "RETURN_HOME" &&
        LivingReturnPolicy::UsefulStep(state.MoveStart, here))
    {
        state.ReturnRoute.Remember(here);
        state.ReturnRoute.ArriveOnTrail(here, state.ReturnTrail);
    }
    LivingReturnPolicy::ObserveTrail(state.ReturnTrail, here, state.ReturningHome);
    // Renew queue membership on every update, including rests and cooldowns
    // between local decisions. No geometry or model work happens here.
    bool stalledReturn = !state.Refuge.Active(nowMs) && state.ReturningHome && state.HomeProgressAtMs && nowMs >= state.HomeProgressAtMs + 30000 &&
        (state.ReturnFailures >= 3 || nowMs >= state.ReturnStartedAtMs + 60000);
    bool foodAdvice = !state.ReturningHome && role == Role::Predator && !anchored && !advice.Active &&
        state.HungrySinceMs && nowMs >= state.HungrySinceMs + 120000 && nowMs >= state.DangerUntilMs &&
        WolfBehaviorPolicy::WantsHunt(record.Needs.Hunger, record.Needs.HealthPressure, false);
    bool queueEligible = _recoveryAdviceEnabled && HasRecoveryAdvice(record.Id) && _aiClient && creature.IsAlive() &&
        !creature.IsInCombat() && !creature.IsInEvadeMode() && !record.GroupCoordinationGoalState &&
        nowMs >= advice.CooldownUntil && !advice.PendingId && (stalledReturn || foodAdvice);
    bool adviceDispatchable = OwnsRoleMovement(record, creature) && !creature.HasUnitState(UNIT_STATE_CASTING) &&
        (state.CurrentPhase == Phase::Idle ||
            (state.CurrentPhase == Phase::Acting && (state.Activity == Activity::Rest || state.Activity == Activity::Look)));
    _recoveryAdviceBudget.Refresh(record.Id.Value, nowMs, queueEligible, stalledReturn, adviceDispatchable);
    if (advice.Status == "WAITING_TURN" && !_recoveryAdviceBudget.WaitMs(record.Id.Value, nowMs))
        advice.Status = "QUEUE_CANCELLED";
    bool adviceOffered = _recoveryAdviceBudget.Ready(record.Id.Value, nowMs);
    if (adviceOffered) advice.Admission.Record(LivingAdviceAdmissionEvent::ReadyObserved, nowMs);
    if (adviceOffered || (advice.PendingId && advice.Responded))
    {
        // A queued actor may finish waiting without first sleeping through
        // another 20-second rest. Never interrupt feeding, work or movement.
        if (state.CurrentPhase == Phase::Acting && (state.Activity == Activity::Rest || state.Activity == Activity::Look))
            StopLivingRole(record, creature);
        if (state.CurrentPhase == Phase::Idle) state.NextDecisionAtMs = nowMs;
    }
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
        state.NextSenseAtMs = nowMs + 1000 + (extensions ? StableAgentHash(record.Id.Value) % 500 : 0);
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
    // A failed escape may discover a passive predator behind impassable
    // geometry. Recheck promptly when it moves, attacks or damages this NPC.
    if (threat && threat->GetGUID() == state.BlockedThreatGuid && nowMs < state.BlockedThreatUntilMs &&
        !freshlyHurt && !creature.IsInCombat() && threat->GetVictim() != &creature &&
        threat->GetExactDist(state.BlockedThreatPosition.X, state.BlockedThreatPosition.Y, state.BlockedThreatPosition.Z) <= 2.0f)
    {
        threat = nullptr;
        state.Awareness = "UNREACHABLE_PASSIVE_THREAT";
    }
    std::optional<ActionPosition> approvedRoleDestination;
    std::optional<RecoveryMovement> approvedRecovery;
    std::optional<NavigationDiagnostics> lastRoleExecutionNavigation;

    // Build authoritative facts at dispatch time. No request itself can grant
    // prey classification, participation, a threat identity, or an animation.
    auto start = [&](ActionRequest request, Phase phase, Unit* target = nullptr, bool preservePlanning = false) -> bool
    {
        lastRoleExecutionNavigation.reset();
        if (!OwnsRoleMovement(record, creature))
            return false;
        // Local roaming/foraging must execute with the same strict path
        // provider as recovery, including after a speed change or resume.
        if (request.Type == ActionType::MoveTo && request.SourceGoal == GoalType::LocalActivity &&
            request.Destination && !request.Recovery)
        {
            approvedRecovery = RecoveryMovement{*request.Destination, homePoint,
                std::max(LivingForagePolicy::SearchRadius(advice.Food.EmptyRounds), homeDistance + 2.0f),
                nowMs < state.DangerUntilMs ? std::optional<ActionPosition>(state.DangerPosition) : std::nullopt, false};
            approvedRecovery->DangerRadius = LivingRolePolicy::SafetyRadius(record.Id.Value);
            request.Recovery = approvedRecovery;
        }
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
        context.ApprovedRecovery = approvedRecovery;
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
        {
            lastRoleExecutionNavigation = result.RecoveryNavigation;
            if (result.RecoveryNavigation) state.ReturnDiagnostics.Navigation = *result.RecoveryNavigation;
            return false;
        }
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
        if (!preservePlanning)
        {
            state.Planning.ClearWait();
            state.Planning.Stage = "NONE";
            state.Planning.CarePause = {};
            advice.ReplyBudgetPause.End(nowMs, advice.RequestedAt);
            advice.Search = {};
            if (advice.Status == "PLANNING_DEFERRED") advice.Status = "SEARCH_INTERRUPTED";
            _planningWork.Cancel(record.Id);
            state.ReturnSearch.InterruptForAction(state.ReturningHome && phase == Phase::Acting &&
                request.Type == ActionType::Ambient);
            state.HuntRejected.clear();
        }
        if (IsEscaping(phase) || phase == Phase::Hunting || phase == Phase::Investigating)
        {
            _recoveryAdviceBudget.Cancel(record.Id.Value);
            if (advice.Status == "WAITING_TURN") advice.Status = "QUEUE_CANCELLED";
            advice.ClearPending();
            if (advice.Active && (advice.ActiveReturning || phase != Phase::Hunting))
            { advice.Active.reset(); advice.Status = "INTERRUPTED"; }
            state.ReturningHome = false;
            state.ReturnStartedAtMs = state.HomeProgressAtMs = 0;
            state.ReturnHomeLimit = 0.0f;
            state.ReturnRoute = {};
        }
        state.Activity = request.AmbientActivity;
        state.SourceGoal = request.SourceGoal;
        state.StartedAtMs = nowMs;
        state.MoveStart = { creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ() };
        state.TargetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
        if (request.ChaseAngleRadians) state.ChaseBearing = *request.ChaseAngleRadians;
        if (request.Destination) state.Destination = *request.Destination;
        else state.Destination = { creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ() };
        if (IsRoleMove(phase) || phase == Phase::Fleeing) state.MoveWatchdog.Begin(nowMs, state.MoveStart);
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
            ActionPosition danger{here.MapId, threat->GetPositionX(), threat->GetPositionY(), threat->GetPositionZ()};
            if (state.EscapeThreatGuid != threat->GetGUID())
            { state.EscapeThreatGuid = threat->GetGUID(); state.EscapeProgress = {}; }
            bool stalledEscape = state.EscapeProgress.Observe(nowMs, here, danger);
            if (stalledEscape && IsEscaping(state.CurrentPhase))
            {
                if (state.CurrentPhase == Phase::SeekingSafety)
                    state.EscapeProgress.Routes.Reject(state.MoveStart, state.Destination);
                state.MoveWatchdog.End = "NO_PROGRESS";
                StopLivingRole(record, creature);
            }
            if (!freshlyHurt && nowMs < state.EscapeProgress.RetryAt) return true;
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
            // Change the search fan after a failed fallback, while the path
            // test still requires every candidate to move away from danger.
            away += state.EscapeProgress.Failures % 2 ? 0.275f : -0.275f;
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
                if (state.EscapeProgress.Routes.Failed(here, destination)) continue;
                approvedRoleDestination = destination;
                ActionRequest escape;
                escape.Type = ActionType::MoveTo; escape.SourceGoal = GoalType::SeekSafety; escape.Destination = destination;
                if (start(escape, Phase::SeekingSafety, threat))
                {
                    state.MovementPurpose = refuge.second;
                    return true;
                }
            }
            if (stalledEscape && !creature.IsInCombat())
            {
                // A stranded actor may need to join the nearby ground mesh
                // before any ordinary refuge is reachable. Validate the exact
                // escape leg with the recovery provider, including danger.
                for (auto destination : LivingRecoveryPath::RejoinPositions(creature))
                {
                    if (state.EscapeProgress.Routes.Failed(here, destination)) continue;
                    RecoveryMovement recovery{destination, homePoint,
                        LivingReturnPolicy::HomeLimit(homeDistance), danger, true};
                    recovery.DangerRadius = std::min(8.0f, creature.GetExactDist2d(threat));
                    Movement::PointsArray points;
                    if (!LivingRecoveryPath::Build(creature, recovery, points, &state.ReturnDiagnostics)) continue;
                    approvedRoleDestination = destination;
                    approvedRecovery = recovery;
                    ActionRequest escape;
                    escape.Type = ActionType::MoveTo; escape.SourceGoal = GoalType::SeekSafety;
                    escape.Destination = destination; escape.Recovery = recovery;
                    if (start(escape, Phase::SeekingSafety, threat))
                    { state.MovementPurpose = "ESCAPE_NAV_REJOIN"; return true; }
                }
                if (!freshlyHurt && threat->ToCreature() && threat->GetVictim() != &creature)
                {
                    PathGenerator reach(threat);
                    reach.CalculatePath(here.X, here.Y, here.Z, false);
                    auto const& nav = reach.GetNavigationDiagnostics();
                    if (nav.Mesh && nav.StartTile && nav.EndTile &&
                        (reach.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)))
                    {
                        state.BlockedThreatGuid = threat->GetGUID(); state.BlockedThreatPosition = danger;
                        state.BlockedThreatUntilMs = nowMs + 15000;
                        state.Awareness = "UNREACHABLE_PASSIVE_THREAT";
                    }
                }
                state.EscapeProgress.Failed(nowMs);
                state.MovementPurpose = "ESCAPE_NO_PATH";
                return true;
            }
            // The fallback generator also checks every launched path's zone.
            approvedRoleDestination.reset();
        }
        Phase phase = flee ? Phase::Fleeing : Phase::Defending;
        if (state.CurrentPhase == phase && state.TargetGuid == threat->GetGUID())
        {
            auto* movement = creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE);
            bool stillRunning = movement && OwnsRoleMovement(record, creature) &&
                (phase == Phase::Fleeing ? state.MoveWatchdog.Continue(nowMs, here, true) : creature.GetVictim() == threat);
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
            advice.Food.Fed(here, nowMs);
            if (advice.Active && !advice.ActiveReturning && advice.StepArrived &&
                nowMs <= advice.ActiveAt + 180000 && record.Needs.Hunger + 0.1f < advice.ActiveHunger)
            {
                ++advice.FoodSuccess; advice.Status = "FOOD_FOUND"; advice.Active.reset();
            }
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
            state.MoveWatchdog.Continue(nowMs, here, state.CurrentPhase == Phase::SeekingSafety))
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
            else
            {
                state.ReturnRoute.Reject(state.MoveStart, state.Destination);
                FailedReturn(state, nowMs, "RETURN_NO_PROGRESS", here);
            }
        }
        if (foraging && !progressed) state.HasForageWaypoint = false;
        if (foraging && progressed)
        {
            advice.Food.Searched(here, nowMs);
        }
        // Short, fully proved terrain legs keep their corners. Do not add an
        // extra second of idle time at every corner; the next leg still waits
        // for this actor's scheduled update and its planning permit.
        bool continueSurface = returningHome && progressed && state.ReturnRoute.SurfaceCorridor &&
            !state.ReturnRoute.Planned.empty();
        state.NextDecisionAtMs = continueSurface ? nowMs : nowMs + (returningHome || foraging ? 1000 :
            LivingRolePolicy::PauseMs(record.Id.Value, state.Cycle, 4000, 8000));
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
        bool resumePlanning = state.Planning.CarePause.Active;
        StopLivingRole(record, creature);
        state.NextDecisionAtMs = resumePlanning ? nowMs :
            nowMs + LivingRolePolicy::PauseMs(record.Id.Value, state.Cycle, 8000, 14999);
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
    std::optional<ActionPosition> planningDanger = extensions && nowMs < state.DangerUntilMs ?
        std::optional<ActionPosition>(state.DangerPosition) : std::nullopt;
    ActionPosition const* rememberedDanger = planningDanger ? &*planningDanger : nullptr;
    uint32 planningCapabilities = uint32(role) * 8 + uint32(creature.CanWalk()) +
        uint32(creature.CanEnterWater()) * 2 + uint32(anchored) * 4;
    if (!state.ReturnRoute.Planned.empty() && !state.Planning.RouteContextMatches(homePoint, planningDanger,
        creature.GetPhaseMask(), planningCapabilities))
    {
        state.ReturnRoute.Planned.clear();
        state.ReturnRoute.SurfaceCorridor = false;
        state.ReturnRoute.PendingBacktrack.reset();
    }
    bool resumingPlan = state.Planning.Resume(nowMs, here, homePoint, planningDanger,
        creature.GetPhaseMask(), planningCapabilities);
    if (!resumingPlan)
    {
        state.ReturnSearch.RestartForDecision(state.Planning, nowMs, here, homePoint, planningDanger,
            creature.GetPhaseMask(), planningCapabilities, state.ReturningHome);
        state.Planning.Begin(nowMs, here, homePoint, planningDanger, creature.GetPhaseMask(), planningCapabilities);
        state.ReturnDiagnostics.Deferred = false;
        state.HuntRejected.clear();
        ++state.Cycle;
    }
    state.Planning.ResumeWork(nowMs);
    uint64 planningStarted = PlanningWorkBudget::StartedOperations();
    auto deferPlanning = [&]()
    {
        bool didWork = PlanningWorkBudget::StartedOperations() > planningStarted;
        if (didWork) state.Planning.MarkProgress(nowMs);
        state.Planning.MarkDeferred(nowMs, !didWork && !_planningWork.CanAdmit(record.Id) ? "ADMISSION" : "WORK_BUDGET",
            state.Planning.Stage);
        if (advice.Search.Active) advice.Search.BudgetPause.Begin(nowMs, advice.Search.ProgressAt);
        if (advice.PendingId && advice.Responded) advice.ReplyBudgetPause.Begin(nowMs, advice.RequestedAt);
        state.NextDecisionAtMs = nowMs + LivingRolePolicy::PauseMs(record.Id.Value, state.Cycle, 100, 350);
        state.MovementPurpose = "PLANNING_DEFERRED";
        return true;
    };
    auto recoverHere = [&](bool preservePendingReturn = false)
    {
        ActionRequest watch;
        watch.Type = ActionType::Ambient; watch.SourceGoal = GoalType::LocalActivity;
        bool unsafe = rememberedDanger && creature.GetExactDist2d(rememberedDanger->X, rememberedDanger->Y) <
            LivingRolePolicy::SafetyRadius(record.Id.Value) && nowMs >= state.BlockedThreatUntilMs;
        watch.AmbientActivity = LivingRolePolicy::RecoveryActivity(role, record.Needs.Hunger, unsafe);
        if (watch.AmbientActivity == Activity::Rest && creature.GetStandState() != UNIT_STAND_STATE_STAND)
            watch.AmbientActivity = Activity::Look;
        bool paused = preservePendingReturn && resumingPlan &&
            (state.ReturnSearch.Started || advice.Search.Active) && state.Planning.PauseForCare(nowMs);
        if (paused && advice.Search.Active)
        {
            advice.Search.BudgetPause.End(nowMs, advice.Search.ProgressAt);
            advice.Search.CarePause.Begin(nowMs, advice.Search.ProgressAt);
        }
        if (!start(watch, Phase::Acting, nullptr, paused) && paused)
        {
            state.Planning.ResumeAfterCare(nowMs);
            advice.Search.CarePause.End(nowMs, advice.Search.ProgressAt);
        }
        state.MovementPurpose = state.ReturnFailure;
    };
    auto rememberAdviceStart = [&](LivingAdviceCandidate const& candidate, bool returning)
    {
        advice.Active = candidate; advice.ActiveOrigin = here; advice.ActiveAt = nowMs;
        advice.ActiveHunger = record.Needs.Hunger; advice.ActiveReturning = returning; advice.StepArrived = false;
        ++advice.Started; advice.Status = "MOVE_STARTED";
        if (!returning) ++state.Forage.StepsStarted;
        if (candidate.Backtrack)
        {
            state.ReturnRoute.PendingBacktrack = LivingReturnPolicy::RouteMemory::Backtrack{here, candidate.Move.Destination};
            state.ReturnRoute.CommitBacktrack();
        }
    };
    auto startReturnAdvice = [&](LivingAdviceCandidate const& candidate)
    {
        auto const& destination = candidate.Move.Destination;
        state.ReturnDiagnostics = candidate.Diagnostics;
        state.ReturnStrategy = "AI_ADVICE";
        if (!candidate.FollowsCorridor)
            state.ReturnRoute.PlanContinuation(destination, candidate.Continuation,
                candidate.Diagnostics.ContinuationPath.SurfaceCorridor);
        state.ReturnRoute.TrailTarget.reset();
        state.ReturnRoute.PendingBacktrack.reset();
        approvedRecovery = candidate.Move;
        ActionRequest move;
        move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity;
        move.Destination = destination; move.Recovery = approvedRecovery;
        if (start(move, Phase::Moving))
        {
            state.MovementPurpose = "RETURN_HOME";
            state.ReturnRoute.CommitBacktrack();
            if (candidate.Move.Rejoin) state.ReturnRoute.CommitRejoin(here, destination);
            rememberAdviceStart(candidate, true);
        }
        else
        {
            state.ReturnRoute.Reject(here, destination);
            ++advice.Rejected; advice.Status = "EXECUTION_REJECTED";
            FailedReturn(state, nowMs, "RETURN_MOVE_REJECTED", here);
            recoverHere(true);
        }
        return true;
    };
    auto startFoodAdvice = [&](LivingAdviceCandidate const& candidate)
    {
        approvedRecovery = candidate.Move;
        ActionRequest search;
        search.Type = ActionType::MoveTo; search.SourceGoal = GoalType::LocalActivity;
        search.Destination = candidate.Move.Destination; search.Recovery = approvedRecovery;
        if (start(search, Phase::Moving))
        {
            rememberAdviceStart(candidate, false);
            state.ForageUntilMs = nowMs + LivingForagePolicy::DurationMs;
            state.HasForageWaypoint = false;
            state.MovementPurpose = "FORAGE_SEARCH";
            return true;
        }
        ++advice.Rejected; advice.Status = "EXECUTION_REJECTED";
        return false;
    };
    bool hungryHunter = role == Role::Predator && WolfBehaviorPolicy::WantsHunt(record.Needs.Hunger, record.Needs.HealthPressure, false);
    // Stationary care does not require navigation work. Preserve an unfinished
    // return across this explicit pause, without counting care as query work.
    if (state.ReturningHome && homeDistance > radius + 2.0f &&
        state.ReturnRoute.NeedsPause(nowMs, record.Needs.Hunger, record.Needs.Fatigue))
    {
        state.ReturnRoute.NextCareAtMs = nowMs + 60000;
        recoverHere(true);
        if (!state.ReturnFailures) state.MovementPurpose = "RETURN_NEEDS_BREAK";
        return true;
    }
    if (role == Role::Prey && record.Needs.Hunger >= 0.65f && rememberedDanger &&
        (creature.GetExactDist2d(rememberedDanger->X, rememberedDanger->Y) >= LivingRolePolicy::SafetyRadius(record.Id.Value) ||
            nowMs < state.BlockedThreatUntilMs))
    { recoverHere(true); state.MovementPurpose = "REFUGE_CARE"; return true; }
    _planningWork.Request(record.Id, nowMs);
    bool planningAvailable = PlanningWorkBudget::Available();
    if (!planningAvailable)
    {
        bool care = !rememberedDanger && ((role == Role::Prey && record.Needs.Hunger >= 0.65f) ||
            (!LivingRolePolicy::Wildlife(role) && record.Needs.Hunger >= 0.65f) ||
            (LivingRolePolicy::Allows(role, Activity::Rest) && record.Needs.Fatigue >= 0.8f));
        if (care)
        {
            recoverHere(state.ReturningHome);
            state.MovementPurpose = "NEEDS_CARE";
            return true;
        }
    }
    using ReturnAdviceOutcome = LivingReturnAdvice::Outcome;
    bool returningForAdvice = state.ReturningHome && homeDistance > radius + 2.0f;
    bool canTryFoodAdvice = extensions && hungryHunter && !anchored && !rememberedDanger && !state.ReturningHome;
    LivingReturnAdvice::Context adviceOrder{bool(advice.PendingId), advice.Search.Active,
        adviceOffered && queueEligible, nowMs >= state.ReturnRetryAtMs};
    std::optional<LivingAdviceCandidate> priorityAdvice;
    LivingReturnAdvice::OnceAttempt priorityAttempt([&]()
    {
        state.Planning.Stage = "ADVICE";
        priorityAdvice = TryLivingAdvice(record, creature, nowMs, returningForAdvice, rememberedDanger);
        if (advice.Status == "PLANNING_DEFERRED" && (advice.Search.Active || advice.PendingId))
            return ReturnAdviceOutcome::Deferred;
        if (advice.PendingId) return ReturnAdviceOutcome::Pending;
        return priorityAdvice ? ReturnAdviceOutcome::AdviceStep : ReturnAdviceOutcome::NoStep;
    });
    ReturnAdviceOutcome priorityOutcome = ReturnAdviceOutcome::NoStep;
    // Queue admission is cheap. Claim an offered turn before foreground
    // hunt/return work, even when its first safe query must yield. Existing
    // requests/searches keep their context; care and safety already ran above.
    if (!state.Refuge.Active(nowMs) && (returningForAdvice || canTryFoodAdvice))
        priorityOutcome = LivingReturnAdvice::TryPriority(adviceOrder, priorityAttempt);
    if (priorityOutcome == ReturnAdviceOutcome::Deferred) return deferPlanning();
    if (priorityOutcome == ReturnAdviceOutcome::Pending)
    {
        if (returningForAdvice) recoverHere(true);
        else state.NextDecisionAtMs = nowMs + 1000;
        return true;
    }
    if (priorityAdvice)
    {
        if (returningForAdvice) return startReturnAdvice(*priorityAdvice);
        if (startFoodAdvice(*priorityAdvice)) return true;
    }
    if (!planningAvailable)
    {
        PlanningWorkBudget::MarkDeferred();
        return deferPlanning();
    }
    state.NextDecisionAtMs = nowMs + LivingRolePolicy::PauseMs(record.Id.Value, state.Cycle, 8000, 12000);
    uint64 cycle = state.Cycle + record.Id.Value;

    if (state.ForageUntilMs && (!extensions || !hungryHunter || nowMs >= state.ForageUntilMs))
    {
        if (extensions && hungryHunter && nowMs >= state.ForageUntilMs) advice.Food.EmptyRound();
        state.ForageUntilMs = 0;
        state.HasForageWaypoint = false;
        state.NextForageAtMs = nowMs + LivingForagePolicy::CooldownMs;
    }

    // Also scan the current surroundings during a bounded search/recovery;
    // prey still has to be actually visible, reachable and attackable.
    if (hungryHunter && !state.Planning.HuntScanned && (homeDistance < 20.0f ||
        ((state.ForageUntilMs || state.ReturnFailures || state.ReturningHome) &&
            homeDistance <= LivingForagePolicy::SearchRadius(advice.Food.EmptyRounds))))
    {
        state.Planning.Stage = "HUNT";
        loadNearby();
        state.NearbyPrey = state.AttackablePrey = 0;
        state.Forage.ScannedAtMs = nowMs;
        state.Forage.NearbyPrey = state.Forage.AttackablePrey = state.Forage.ReachablePrey = 0;
        state.LastHuntStatus = "NO_PREY";
        bool actionRejected = false;
        bool preyOnCooldown = false;
        std::vector<Creature*> candidates;
        for (Creature* candidate : nearby)
        {
            if (state.Refuge.Active(nowMs)) continue;
            if (_agentTypeCatalog.Resolve(candidate->GetEntry()) != AgentType::Prey || !candidate->IsAlive() ||
                candidate->IsPet() || !candidate->GetCharmerOrOwnerGUID().IsEmpty() || candidate->IsControlledByPlayer() ||
                IsService(*candidate) || candidate->IsQuestGiver() || candidate->GetZoneId() != 12)
                continue;
            ++state.NearbyPrey;
            ++state.Forage.NearbyPrey;
            if (!creature.IsValidAttackTarget(candidate))
                continue;
            ++state.AttackablePrey;
            ++state.Forage.AttackablePrey;
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
            ActionPosition preyPoint{candidate->GetMapId(), candidate->GetPositionX(), candidate->GetPositionY(), candidate->GetPositionZ()};
            // A moving rejected animal must not restart the same decision's
            // first probes on every yield and starve forage/return/Advice.
            // The next completed decision scans again; accepted hunts still
            // build and dispatch against the target's current live position.
            if (std::any_of(state.HuntRejected.begin(), state.HuntRejected.end(), [&](LivingHuntProbe const& probe)
                { return probe.Prey == candidate->GetGUID(); }))
                continue;
            auto work = PlanningWorkBudget::TryAcquire();
            if (!work) return deferPlanning();
            PathGenerator path(&creature);
            Movement::PointsArray huntPath;
            if (Movement::BuildElwynnHuntPath(creature, *candidate, path, huntPath))
            {
                ++state.Forage.ReachablePrey;
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
            auto rejected = std::find_if(state.HuntRejected.begin(), state.HuntRejected.end(),
                [&](LivingHuntProbe const& probe) { return probe.Prey == candidate->GetGUID(); });
            if (rejected != state.HuntRejected.end()) rejected->Position = preyPoint;
            else
            {
                state.HuntRejected.push_back({candidate->GetGUID(), preyPoint});
                if (state.HuntRejected.size() == 8) break;
            }
        }
        if (!actionRejected)
            state.LastHuntStatus = candidates.empty() && preyOnCooldown ? "PREY_RETRY_DELAY" :
                state.AttackablePrey ? "NO_REACHABLE_PREY" :
                state.NearbyPrey ? "PREY_NOT_ATTACKABLE" : "NO_PREY";
        state.Planning.HuntScanned = true;
    }

    if (!priorityAttempt.Tried() && extensions && hungryHunter && !anchored && !rememberedDanger && !state.ReturningHome)
    {
        state.Planning.Stage = "ADVICE";
        if (auto candidate = TryLivingAdvice(record, creature, nowMs, false, nullptr))
        {
            if (startFoodAdvice(*candidate)) return true;
        }
        if (advice.Status == "PLANNING_DEFERRED" && (advice.Search.Active || advice.PendingId)) return deferPlanning();
        if (advice.PendingId) { state.NextDecisionAtMs = nowMs+1000; return true; }
    }
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
            state.Planning.Stage = "FORAGE";
            ActionPosition origin{creature.GetMapId(), home.GetPositionX(), home.GetPositionY(), home.GetPositionZ()};
            auto forageWorkStarted = [&]()
            {
                state.Forage.SearchAtMs = nowMs;
                if (!state.Planning.ForageAttemptStarted) ++state.Forage.RouteAttempts;
                state.Planning.ForageAttemptStarted = true;
            };
            auto nextForageCandidate = [&]()
            {
                state.HasForageWaypoint = false;
                state.Planning.ForageAttemptStarted = false;
                state.Planning.ForageGround = {};
                state.Planning.ForageStep.reset();
            };
            for (; state.Planning.ForageAttempts < 6; ++state.Planning.ForageAttempts)
            {
                uint32 attempt = state.Planning.ForageAttempts;
                if (!state.HasForageWaypoint || creature.GetExactDist2d(state.ForageWaypoint.X, state.ForageWaypoint.Y) <= 3.0f)
                {
                    auto work = PlanningWorkBudget::TryAcquire();
                    if (!work) return deferPlanning();
                    forageWorkStarted();
                    if (!state.Planning.ForageGround.Started)
                    {
                        auto hint = attempt < 3 ? advice.Food.FoodHint(origin,
                            LivingForagePolicy::SearchRadius(advice.Food.EmptyRounds), nowMs) : std::nullopt;
                        state.ForageWaypoint = hint ? *hint : attempt < 3 ? LivingForagePolicy::Waypoint(origin, record.Id.Value,
                            state.ForageLeg++, advice.Food.EmptyRounds) : LivingForagePolicy::LocalWaypoint(here, record.Id.Value,
                                state.ForageLeg++, advice.Food.EmptyRounds);
                        state.HasForageWaypoint = hint.has_value();
                        if (!hint && attempt < 3)
                        {
                            float height = creature.GetMap()->GetHeight(creature.GetPhaseMask(), state.ForageWaypoint.X,
                                state.ForageWaypoint.Y, here.Z + 4, true);
                            if (!std::isfinite(height) || height <= INVALID_HEIGHT || std::abs(height - here.Z) > 12)
                            {
                                ++state.Forage.HeightRejected; state.Forage.Navigation = {};
                                state.Forage.Navigation.Failure = "HEIGHT_INVALID";
                                nextForageCandidate();
                                continue;
                            }
                            state.ForageWaypoint.Z = height;
                            state.HasForageWaypoint = true;
                        }
                    }
                    if (!state.HasForageWaypoint)
                    {
                        // Retain the same candidate while its expected-floor
                        // walk yields. Each permit covers at most eight samples.
                        auto status = LivingRecoveryPath::GroundLocalForageTarget(creature, here,
                            state.ForageWaypoint, state.Planning.ForageGround, attempt >= 3);
                        work.Finish();
                        if (status == LivingSurfaceCorridor::Status::Pending) return deferPlanning();
                        if (status == LivingSurfaceCorridor::Status::Rejected)
                        {
                            ++state.Forage.HeightRejected; state.Forage.Navigation = {};
                            state.Forage.Navigation.Failure = "HEIGHT_INVALID";
                            nextForageCandidate();
                            continue;
                        }
                        state.ForageWaypoint = *state.Planning.ForageGround.Resolved;
                        state.HasForageWaypoint = true;
                        state.Planning.ForageGround = {};
                    }
                }
                if (!state.Planning.ForageStep)
                {
                    auto work = PlanningWorkBudget::TryAcquire();
                    if (!work) return deferPlanning();
                    forageWorkStarted();
                    state.Planning.ForageStep = LivingRecoveryPath::Toward(creature, state.ForageWaypoint, origin,
                        LivingForagePolicy::SearchRadius(advice.Food.EmptyRounds), &state.Forage.Navigation);
                }
                if (state.Planning.ForageStep)
                {
                    auto work = PlanningWorkBudget::TryAcquire();
                    if (!work) return deferPlanning();
                    ActionRequest search;
                    search.Type = ActionType::MoveTo; search.SourceGoal = GoalType::LocalActivity;
                    search.Destination = state.Planning.ForageStep;
                    if (start(search, Phase::Moving))
                    {
                        ++state.Forage.StepsStarted;
                        state.MovementPurpose = "FORAGE_SEARCH";
                        return true;
                    }
                    if (lastRoleExecutionNavigation) state.Forage.Navigation = *lastRoleExecutionNavigation;
                    else state.Forage.Navigation.Failure = "EXECUTION_REJECTED";
                }
                ++state.Forage.PathRejected;
                advice.Food.Unreachable(state.ForageWaypoint, nowMs);
                nextForageCandidate();
            }
            state.ForageUntilMs = 0;
            advice.Food.EmptyRound();
            state.NextForageAtMs = nowMs + LivingForagePolicy::CooldownMs;
            state.LastHuntStatus = "FORAGE_NO_PATH";
        }
    }

    uint32 dayTime = uint32(nowMs % _routineScheduleConfig.DayLengthMs);
    bool workHours = dayTime >= _routineScheduleConfig.WorkStartMs && dayTime < _routineScheduleConfig.WorkEndMs;
    Activity activity = LivingRolePolicy::IdleActivity(role, cycle, workHours, record.Needs.Hunger, record.Needs.Fatigue);
    if (anchored && (activity == Activity::Roam || activity == Activity::Rest)) activity = Activity::Look;
    if (rememberedDanger && activity == Activity::Rest) activity = Activity::Look;

    // Retain return timers and the real spawn. These local steps are not home
    // progress and must not turn an unresolved return into a telemetry PASS.
    if (extensions && _livingRecoverySpawnId && creature.GetSpawnId() == _livingRecoverySpawnId &&
        !anchored && !rememberedDanger && state.ReturningHome &&
        (role == Role::Predator || role == Role::Prey))
    {
        if (state.Refuge.Begin(nowMs, state.ReturnStartedAtMs, state.HomeProgressAtMs, here, homePoint, state.ReturnHomeLimit))
        {
            _recoveryAdviceBudget.Cancel(record.Id.Value);
            advice.ClearPending(); advice.Active.reset(); advice.Status = "LOCAL_RECOVERY";
        }
        if (state.Refuge.Active(nowMs))
        {
            // Alternate actual role care (including grazing) with movement.
            if (cycle % 2 == 0)
            {
                state.Planning.Stage = "RETURN";
                for (; state.Planning.RefugeAttempts < 8; ++state.Planning.RefugeAttempts)
                {
                    unsigned i = state.Planning.RefugeAttempts;
                    auto work = PlanningWorkBudget::TryAcquire();
                    if (!work) return deferPlanning();
                    float angle = float((cycle * 137 + i * 45) % 360) * GroupMemberFormation::TwoPi / 360.0f;
                    auto target = *state.Refuge.Anchor;
                    target.X += 8.0f * std::cos(angle); target.Y += 8.0f * std::sin(angle);
                    float ground = creature.GetMapHeight(target.X, target.Y, here.Z);
                    if (!std::isfinite(ground) || ground <= INVALID_HEIGHT || std::abs(ground-here.Z) > 3) continue;
                    target.Z = ground + creature.GetHoverOffset();
                    if (!state.Refuge.Contains(target)) continue;
                    RecoveryMovement recovery{target, state.Refuge.Home, state.Refuge.HomeLimit, {}, false};
                    Movement::PointsArray points;
                    if (!LivingRecoveryPath::Build(creature, recovery, points)) continue;
                    if (!Movement::PathWithinBounds(points, [&](float x, float y, float z)
                        { return state.Refuge.Contains({here.MapId, x, y, z}); })) continue;
                    approvedRecovery = recovery;
                    ActionRequest move;
                    move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity;
                    move.Destination = target; move.Recovery = recovery;
                    if (start(move, Phase::Moving))
                    { ++state.Refuge.Moves; state.MovementPurpose = "LOCAL_RECOVERY_MOVE"; return true; }
                }
                ++state.Refuge.Blocked;
            }
            recoverHere(); state.MovementPurpose = "LOCAL_RECOVERY_CARE";
            return true;
        }
    }

    // Local cohesion is not a persisted coalition. Only a visible, compatible
    // lower-id neighbor can lead; home bounds prevent a chain across the map.
    if (extensions && !state.Planning.CohesionChecked && !anchored && !rememberedDanger && !state.ReturningHome &&
        homeDistance <= radius + 2.0f && !state.CompanionGuid.IsEmpty() && cycle % 3 == 0)
    {
        Creature* companion = ObjectAccessor::GetCreature(creature, state.CompanionGuid);
        AgentRecord const* member = eligibleMember(companion);
        if (member && companion->IsAlive() && !companion->IsInCombat() && visibleDanger(companion) &&
            !IsEscaping(member->LivingRole.CurrentPhase) && creature.GetExactDist2d(companion) > 6.0f &&
            home.GetExactDist2d(companion) <= 18.0f)
        {
            state.Planning.Stage = "DECISION";
            auto work = PlanningWorkBudget::TryAcquire();
            if (!work) return deferPlanning();
            state.Planning.CohesionChecked = true;
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
        bool pathReady = false;
        std::optional<LivingAdviceCandidate> advised;
        if (returningHome)
        {
            if (!state.ReturningHome)
            {
                state.ReturningHome = true;
                state.ReturnHomeLimit = LivingReturnPolicy::HomeLimit(homeDistance);
                state.ReturnStartedAtMs = state.HomeProgressAtMs = nowMs;
                state.BestHomeDistance = homeDistance;
                state.ReturnRoute = {};
                state.ReturnRoute.Remember(here);
                state.ReturnRoute.NextCareAtMs = nowMs + 60000;
            }
            auto outcome = LivingReturnAdvice::Select({bool(advice.PendingId), advice.Search.Active,
                adviceOffered && queueEligible && stalledReturn, nowMs >= state.ReturnRetryAtMs}, [&]()
            {
                state.Planning.Stage = "RETURN";
                auto step = FindRoleReturnStep(creature, home, rememberedDanger, failure, state,
                    LivingRolePolicy::SafetyRadius(record.Id.Value), radius + 2.0f, nowMs);
                if (state.ReturnDiagnostics.Deferred) return ReturnAdviceOutcome::Deferred;
                if (!step) return ReturnAdviceOutcome::NoStep;
                destination = *step;
                return ReturnAdviceOutcome::ReturnStep;
            }, [&]()
            {
                if (priorityAttempt.Tried() && returningForAdvice)
                {
                    // An empty/invalid priority attempt falls through to the
                    // original return cursor without a second admission/search.
                    advised = priorityAdvice;
                    return priorityAttempt();
                }
                state.Planning.Stage = "ADVICE";
                advised = TryLivingAdvice(record, creature, nowMs, true, rememberedDanger);
                if (advice.Status == "PLANNING_DEFERRED" && (advice.Search.Active || advice.PendingId))
                    return ReturnAdviceOutcome::Deferred;
                if (advice.PendingId) return ReturnAdviceOutcome::Pending;
                return advised ? ReturnAdviceOutcome::AdviceStep : ReturnAdviceOutcome::NoStep;
            });
            if (outcome == ReturnAdviceOutcome::Deferred) return deferPlanning();
            if (outcome == ReturnAdviceOutcome::Pending || outcome == ReturnAdviceOutcome::RetryWait)
            { recoverHere(outcome == ReturnAdviceOutcome::Pending); return true; }
            pathReady = outcome == ReturnAdviceOutcome::ReturnStep || outcome == ReturnAdviceOutcome::AdviceStep;
            if (advised) return startReturnAdvice(*advised);
        }
        else
        {
            state.Planning.Stage = "DECISION";
            auto work = PlanningWorkBudget::TryAcquire();
            if (!work) return deferPlanning();
            float angle = float((cycle * 137) % 360) * GroupMemberFormation::TwoPi / 360.0f;
            destination.X += radius * std::cos(angle);
            destination.Y += radius * std::sin(angle);
            pathReady = CheckRolePath(creature, destination, rememberedDanger, LivingRolePolicy::SafetyRadius(record.Id.Value));
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
        if (returningHome)
        {
            approvedRecovery = RecoveryMovement{destination, {creature.GetMapId(), home.GetPositionX(),
                home.GetPositionY(), home.GetPositionZ()}, state.ReturnHomeLimit,
                rememberedDanger ? std::optional<ActionPosition>(*rememberedDanger) : std::nullopt,
                std::string_view(state.ReturnStrategy) == "NAV_REJOIN"};
            approvedRecovery->QueryDestination = state.ReturnDiagnostics.QueryDestination;
            approvedRecovery->DangerRadius = LivingRolePolicy::SafetyRadius(record.Id.Value);
            approvedRecovery->SurfaceCorridor = state.ReturnRoute.SurfaceCorridor &&
                std::string_view(state.ReturnStrategy) == "CORRIDOR";
            move.Recovery = approvedRecovery;
        }
        if (start(move, Phase::Moving))
        {
            state.MovementPurpose = returningHome ? "RETURN_HOME" : "LOCAL_ROAM";
            if (returningHome)
            {
                state.ReturnRoute.CommitBacktrack();
                if (approvedRecovery && approvedRecovery->Rejoin)
                    state.ReturnRoute.CommitRejoin(here, destination);
            }
        }
        else if (returningHome)
        {
            state.ReturnRoute.Reject(here, destination);
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
