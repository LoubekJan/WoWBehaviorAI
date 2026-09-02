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
#include "Action/ArrivalTolerance.h"
#include "Agent/AgentGroupIntentProjector.h"
#include "Agent/AgentGroupIntentSystem.h"
#include "Agent/AgentGroupRecord.h"
#include "Agent/AgentGroupRuntimeView.h"
#include "Agent/AgentSnapshot.h"
#include "Agent/CoalitionFormationProfileKind.h"
#include "Agent/CoalitionMaintenanceSystem.h"
#include "Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "IoContext.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "Memory/MemoryImportance.h"
#include "Metric.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PointMovementGenerator.h"
#include "Reconciliation/CreatureSpawnCensus.h"
#include "Reconciliation/CreatureSpawnZoneFilter.h"
#include "Reconciliation/SpawnReconciliationPlan.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace
{
    uint64 CurrentTimeMs()
    {
        return uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
            GameTime::GetSystemTime().time_since_epoch()).count());
    }

    // Milestone 2.12G2R P2 fix (STATIC review): how long the ROAM
    // lifecycle test hooks (CheckTestPreemptOnActiveRoam()/
    // CheckTestLeaveOnActiveRoam()/CheckTestDissolveOnActiveRoam()) wait,
    // once triggered, for their own postcondition to actually be
    // observed before giving up and logging FAILED - not configurable,
    // this is a one-off manual test tool, not a tunable gameplay value.
    // 30s comfortably covers the default cadences everything involved
    // already runs on (~1s NeedsSystem update, 5s GroupCoordinationIntervalMs)
    // with ample margin.
    constexpr uint64 TestHookVerifyTimeoutMs = 30000;

    // Milestone 2.8F P2 fix: is AIWorld's own MoveTo generator still
    // present in actor's MOTION_SLOT_ACTIVE - searches the whole slot, not
    // just the current/top generator, since an interrupted-but-not-yet-
    // cleaned-up MoveTo can be buried (deactivated, not removed) under a
    // higher-priority one. Same lookup StopMoveTo() uses to find the
    // generator to remove; here it's only ever read, never removed.
    bool HasOwnMoveToGenerator(Creature& actor)
    {
        return actor.GetMotionMaster()->GetMovementGenerator([](MovementGenerator const* gen)
        {
            if (gen->GetMovementGeneratorType() != POINT_MOTION_TYPE)
                return false;

            auto const* point = dynamic_cast<PointMovementGenerator<Creature> const*>(gen);
            return point && point->GetId() == ActionExecutor::MovePointId;
        }, MOTION_SLOT_ACTIVE) != nullptr;
    }

    // Milestone 2.11C: the single place that knows GoalType::GoToWork/
    // GoHome are Action-layer identity tags RoutineSystem produces, never
    // something GoalSystem produces or AgentRecord::ActiveGoalState ever
    // holds - see GoalType.h. Anything that needs to branch on "does this
    // ActionRequest/ActiveAction::SourceGoal belong to ActiveGoalState or
    // RoutineGoalState" goes through here rather than re-deriving the
    // distinction inline.
    bool IsRoutineSourceGoal(GoalType type)
    {
        return type == GoalType::GoToWork || type == GoalType::GoHome;
    }

    // Milestone 2.12G2/2.12G3C2: the group-coordination counterpart to
    // IsRoutineSourceGoal() above - the single place that knows
    // GoalType::Regroup/Roam/Hunt are all GroupCoordinationGoalState-owned
    // source tags (see GroupCoordinationGoal.h), so every ownership/
    // preemption/stop check that used to compare only against
    // GoalType::Regroup now goes through here instead of silently
    // treating a Roam- or Hunt-sourced attempt as if it belonged to no
    // owner at all. Extending this one function is what makes HUNT
    // automatically inherit the exact same Emergency/Normal ActiveGoal and
    // RoutineGoal preemption UpdateNeeds()'s own COORDINATION_PREEMPTED_BY_GOAL
    // block already enforces for Regroup/Roam - no separate Hunt-specific
    // preemption logic exists anywhere.
    bool IsCoordinationSourceGoal(GoalType type)
    {
        return type == GoalType::Regroup || type == GoalType::Roam || type == GoalType::Hunt;
    }

    // Milestone 2.12D P2 fix (STATIC review): every AgentRecord now names a
    // real, individually-bindable creature spawn - an AgentGroup is no
    // longer an AgentRecord at all (see GroupId.h), so there is nothing
    // left for this to exclude. Kept as a named wrapper (rather than
    // inlining Map::GetCreatureBySpawnId() at every call site) purely for
    // the null-map convenience every call site already relies on.
    Creature* ResolveLiveCreature(AgentRecord const& record, Map* map)
    {
        return map ? map->GetCreatureBySpawnId(record.SpawnId) : nullptr;
    }

    // Milestone 2.12D P2 fix (STATIC review): same reasoning as
    // ResolveLiveCreature() above - AgentRegistry::FindBySpawn() alone is
    // now sufficient, there is no AgentGroup identity left in this
    // (map_id, spawn_id) space to accidentally match against. Kept as a
    // named wrapper for symmetry with ResolveLiveCreature() and to keep
    // every live-spawn-enrichment call site going through one place.
    AgentRecord* FindLiveAgentBySpawn(AgentRegistry& registry, uint32 mapId, uint64 spawnId)
    {
        return registry.FindBySpawn(mapId, spawnId);
    }

    // Milestone 2.12C/2.12D: a fresh, transient snapshot of an AgentGroup's
    // membership - one plain AgentRegistry::Find() lookup per
    // AgentGroupMembership, never cached anywhere past this call. Never
    // touches Map*/Creature* itself - each member's own individual-agent
    // WorldState is already the authority for whether it is currently
    // materialized, kept current by that member's own bind/unbind
    // bookkeeping wherever it is processed as an ordinary agent. Never
    // forces anything to load: an unresolvable or still-Abstract member
    // simply reads as not loaded. Observability only (2.12D P2 fix) -
    // LoadedMembers no longer gates AgentGroupSimulationSystem::Update(),
    // see AgentGroupRecord.h.
    AgentGroupRuntimeView ResolveAgentGroupRuntimeView(AgentRegistry& registry, AgentGroupRecord const& group)
    {
        AgentGroupRuntimeView view;
        view.TotalMembers = uint32(group.Members.size());

        for (AgentGroupMembership const& membership : group.Members)
        {
            AgentRecord const* member = registry.Find(membership.Member);
            if (member && member->WorldState == AgentWorldState::Materialized)
                ++view.LoadedMembers;
        }

        return view;
    }
}

AIWorldMgr* AIWorldMgr::instance()
{
    static AIWorldMgr instance;
    return &instance;
}

void AIWorldMgr::Initialize(Trinity::Asio::IoContext& ioContext)
{
    _enabled = sConfigMgr->GetBoolDefault("AIWorld.Enable", false);
    if (!_enabled)
    {
        TC_LOG_INFO("ai.world", "AIWorld disabled (AIWorld.Enable = 0)");
        return;
    }

    // Milestone 2.10B: scheduler-poll cadence (how often RunDecisionScheduler()
    // even looks for due agents) is deliberately its own, faster knob than
    // either per-agent decision interval below - see RunDecisionScheduler().
    int32 decisionSchedulerIntervalMs = sConfigMgr->GetIntDefault("AIWorld.DecisionSchedulerIntervalMs", 250);
    if (decisionSchedulerIntervalMs < 50)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DecisionSchedulerIntervalMs ({}) is invalid or too low, clamping to 50ms", decisionSchedulerIntervalMs);
        decisionSchedulerIntervalMs = 50;
    }
    _decisionSchedulerIntervalMs = uint32(decisionSchedulerIntervalMs);
    _decisionSchedulerTimer = 0;

    int32 decisionNearbyIntervalMs = sConfigMgr->GetIntDefault("AIWorld.DecisionNearbyIntervalMs", 1000);
    if (decisionNearbyIntervalMs < 100)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DecisionNearbyIntervalMs ({}) is invalid or too low, clamping to 100ms", decisionNearbyIntervalMs);
        decisionNearbyIntervalMs = 100;
    }
    _decisionNearbyIntervalMs = uint32(decisionNearbyIntervalMs);

    int32 decisionActiveIntervalMs = sConfigMgr->GetIntDefault("AIWorld.DecisionActiveIntervalMs", 5000);
    if (decisionActiveIntervalMs < 100)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DecisionActiveIntervalMs ({}) is invalid or too low, clamping to 100ms", decisionActiveIntervalMs);
        decisionActiveIntervalMs = 100;
    }
    _decisionActiveIntervalMs = uint32(decisionActiveIntervalMs);

    float decisionNearbyPlayerRange = sConfigMgr->GetFloatDefault("AIWorld.DecisionNearbyPlayerRange", 60.0f);
    if (decisionNearbyPlayerRange < 1.0f)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DecisionNearbyPlayerRange ({}) is invalid or too low, clamping to 1.0", decisionNearbyPlayerRange);
        decisionNearbyPlayerRange = 1.0f;
    }
    _decisionNearbyPlayerRange = decisionNearbyPlayerRange;

    // Milestone 2.10D: coarse Background tick cadence - not a decision
    // cadence at all, see RunDecisionScheduler()'s own comment.
    int32 backgroundSimulationIntervalMs = sConfigMgr->GetIntDefault("AIWorld.BackgroundSimulationIntervalMs", 60000);
    if (backgroundSimulationIntervalMs < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.BackgroundSimulationIntervalMs ({}) is invalid or too low, clamping to 1000ms", backgroundSimulationIntervalMs);
        backgroundSimulationIntervalMs = 1000;
    }
    _backgroundSimulationIntervalMs = uint32(backgroundSimulationIntervalMs);

    // Milestone 2.12D P2 fix (STATIC review): renamed from
    // AIWorld.AbstractSimulationIntervalMs now that AgentGroups no longer
    // go through SimulationTier::Abstract at all (that tier is gone - see
    // SimulationTier.h) - this governs RunDecisionScheduler()'s own group
    // coarse-tick loop instead.
    int32 groupSimulationIntervalMs = sConfigMgr->GetIntDefault("AIWorld.GroupSimulationIntervalMs", 300000);
    if (groupSimulationIntervalMs < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.GroupSimulationIntervalMs ({}) is invalid or too low, clamping to 1000ms", groupSimulationIntervalMs);
        groupSimulationIntervalMs = 1000;
    }
    _groupSimulationIntervalMs = uint32(groupSimulationIntervalMs);

    // Milestone 2.10D P2 fix: the hard per-pass bound
    // CoarseSimulationScheduler admits against - see its own header
    // comment for why an unbounded coarse tick would both spike
    // world-thread work and permanently phase-lock every Background agent
    // onto the same tick pass.
    int32 coarseSimulationMaxPerPass = sConfigMgr->GetIntDefault("AIWorld.CoarseSimulationMaxPerPass", 50);
    if (coarseSimulationMaxPerPass < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.CoarseSimulationMaxPerPass ({}) is invalid or too low, clamping to 1", coarseSimulationMaxPerPass);
        coarseSimulationMaxPerPass = 1;
    }
    _coarseSimulationMaxPerPass = uint32(coarseSimulationMaxPerPass);

    // Milestone 2.12D P2 fix (STATIC review): the group coarse tick's own
    // hard per-pass bound, GroupCoarseSimulationScheduler's sibling to
    // AIWorld.CoarseSimulationMaxPerPass above - see its own header comment
    // for why groups need this too, now that dynamic LOOSE coalitions mean
    // group cardinality is not fixed the way a small set of scripted
    // groups would be.
    int32 groupSimulationMaxPerPass = sConfigMgr->GetIntDefault("AIWorld.GroupSimulationMaxPerPass", 50);
    if (groupSimulationMaxPerPass < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.GroupSimulationMaxPerPass ({}) is invalid or too low, clamping to 1", groupSimulationMaxPerPass);
        groupSimulationMaxPerPass = 1;
    }
    _groupSimulationMaxPerPass = uint32(groupSimulationMaxPerPass);

    // Milestone 2.12B/2.12D: not a tuned gameplay value - see
    // AgentGroupSimulationRates.h for why. No HungerPerSecond any more
    // (2.12D P2 fix) - see AgentGroupRecord.h.
    _agentGroupSimulationRates.ResourcesPerSecond = std::clamp(
        sConfigMgr->GetFloatDefault("AIWorld.AgentGroupResourcesRatePerSecond", 0.0005f), 0.0f, 1.0f);

    TC_LOG_INFO("ai.world", "AI agent group simulation configured resourcesRate={:.6f}",
        _agentGroupSimulationRates.ResourcesPerSecond);

    // Milestone 2.12E3B: AgentGroupPolicySystem itself never reads config -
    // see AgentGroupPolicyConfig.h for why. Min is clamped to at least 1 (a
    // group of 0 makes no sense as either a floor or a ceiling), Max is
    // clamped to be at least Min - a config that would let LooseMinMembers
    // exceed LooseMaxMembers (or the Stable equivalent) would make
    // AgentGroupPolicySystem::CanJoin() and ::ShouldDissolve() disagree
    // with each other about whether a group of exactly Min members is
    // viable, which is a real risk to fail-closed against here rather than
    // let each method quietly interpret it differently.
    int32 looseGroupMinMembers = sConfigMgr->GetIntDefault("AIWorld.LooseGroupMinMembers", 2);
    if (looseGroupMinMembers < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.LooseGroupMinMembers ({}) is invalid or too low, clamping to 1", looseGroupMinMembers);
        looseGroupMinMembers = 1;
    }
    _groupPolicyConfig.LooseMinMembers = uint32(looseGroupMinMembers);

    int32 looseGroupMaxMembers = sConfigMgr->GetIntDefault("AIWorld.LooseGroupMaxMembers", 5);
    if (looseGroupMaxMembers < looseGroupMinMembers)
    {
        TC_LOG_WARN("ai.world", "AIWorld.LooseGroupMaxMembers ({}) is lower than AIWorld.LooseGroupMinMembers ({}), clamping to match",
            looseGroupMaxMembers, looseGroupMinMembers);
        looseGroupMaxMembers = looseGroupMinMembers;
    }
    _groupPolicyConfig.LooseMaxMembers = uint32(looseGroupMaxMembers);

    int32 stableGroupMinMembers = sConfigMgr->GetIntDefault("AIWorld.StableGroupMinMembers", 2);
    if (stableGroupMinMembers < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.StableGroupMinMembers ({}) is invalid or too low, clamping to 1", stableGroupMinMembers);
        stableGroupMinMembers = 1;
    }
    _groupPolicyConfig.StableMinMembers = uint32(stableGroupMinMembers);

    int32 stableGroupMaxMembers = sConfigMgr->GetIntDefault("AIWorld.StableGroupMaxMembers", 8);
    if (stableGroupMaxMembers < stableGroupMinMembers)
    {
        TC_LOG_WARN("ai.world", "AIWorld.StableGroupMaxMembers ({}) is lower than AIWorld.StableGroupMinMembers ({}), clamping to match",
            stableGroupMaxMembers, stableGroupMinMembers);
        stableGroupMaxMembers = stableGroupMinMembers;
    }
    _groupPolicyConfig.StableMaxMembers = uint32(stableGroupMaxMembers);

    TC_LOG_INFO("ai.world", "AI agent group policy configured looseMin={} looseMax={} stableMin={} stableMax={}",
        _groupPolicyConfig.LooseMinMembers, _groupPolicyConfig.LooseMaxMembers,
        _groupPolicyConfig.StableMinMembers, _groupPolicyConfig.StableMaxMembers);

    // Milestone 2.12E4A: off by default - this is a new, not yet
    // runtime-verified feature (see RunCoalitionFormation()'s own
    // comment). A wolf pack IS a Loose AgentGroup, so its own size bounds
    // are never configured here - see CoalitionFormationProfile.h for
    // why MinMembers/MaxMembers are always copied from
    // AIWorld.LooseGroupMinMembers/LooseGroupMaxMembers above instead of a
    // second, independently-tunable pair.
    _wolfGroupAutoFormation = sConfigMgr->GetBoolDefault("AIWorld.WolfGroupAutoFormation", false);

    _wolfGroupCreatureEntry = uint32(sConfigMgr->GetIntDefault("AIWorld.WolfGroupCreatureEntry", 1423));

    int32 wolfGroupFormationIntervalMs = sConfigMgr->GetIntDefault("AIWorld.WolfGroupFormationIntervalMs", 5000);
    if (wolfGroupFormationIntervalMs < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupFormationIntervalMs ({}) is invalid or too low, clamping to 1000ms", wolfGroupFormationIntervalMs);
        wolfGroupFormationIntervalMs = 1000;
    }
    _wolfGroupFormationIntervalMs = uint32(wolfGroupFormationIntervalMs);
    _wolfGroupFormationTimer = 0;

    float wolfGroupFormationRadius = sConfigMgr->GetFloatDefault("AIWorld.WolfGroupFormationRadius", 30.0f);
    if (wolfGroupFormationRadius < 1.0f)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupFormationRadius ({:.1f}) is invalid or too low, clamping to 1.0", wolfGroupFormationRadius);
        wolfGroupFormationRadius = 1.0f;
    }
    _wolfGroupFormationRadius = wolfGroupFormationRadius;

    // Milestone 2.12E4R: AIWorldMgr's own one existing
    // CoalitionFormationProfile - see its own declaration comment.
    // MinMembers/MaxMembers always mirror _groupPolicyConfig's own Loose
    // bounds (already clamped above), never independently configured.
    _wolfLooseFormationProfile.Id = CoalitionFormationProfileId::WolfLoose;
    _wolfLooseFormationProfile.Kind = AgentGroupKind::Loose;
    _wolfLooseFormationProfile.CreatureEntry = _wolfGroupCreatureEntry;
    _wolfLooseFormationProfile.MinMembers = _groupPolicyConfig.LooseMinMembers;
    _wolfLooseFormationProfile.MaxMembers = _groupPolicyConfig.LooseMaxMembers;
    _wolfLooseFormationProfile.FormationRadius = _wolfGroupFormationRadius;

    // Milestone 2.12G1: DefiasLoose - the second real profile, built the
    // exact same way as _wolfLooseFormationProfile above, off by default.
    // A Defias Thug cluster IS also a Loose AgentGroup, so it shares the
    // same _groupPolicyConfig.Loose{Min,Max}Members bounds - not an
    // independently-tunable pair, same reasoning as WolfLoose above.
    _defiasGroupAutoFormation = sConfigMgr->GetBoolDefault("AIWorld.DefiasGroupAutoFormation", false);

    _defiasGroupCreatureEntry = uint32(sConfigMgr->GetIntDefault("AIWorld.DefiasGroupCreatureEntry", 38));

    int32 defiasGroupFormationIntervalMs = sConfigMgr->GetIntDefault("AIWorld.DefiasGroupFormationIntervalMs", 5000);
    if (defiasGroupFormationIntervalMs < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupFormationIntervalMs ({}) is invalid or too low, clamping to 1000ms", defiasGroupFormationIntervalMs);
        defiasGroupFormationIntervalMs = 1000;
    }
    _defiasGroupFormationIntervalMs = uint32(defiasGroupFormationIntervalMs);
    _defiasGroupFormationTimer = 0;

    float defiasGroupFormationRadius = sConfigMgr->GetFloatDefault("AIWorld.DefiasGroupFormationRadius", 30.0f);
    if (defiasGroupFormationRadius < 1.0f)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupFormationRadius ({:.1f}) is invalid or too low, clamping to 1.0", defiasGroupFormationRadius);
        defiasGroupFormationRadius = 1.0f;
    }
    _defiasGroupFormationRadius = defiasGroupFormationRadius;

    _defiasLooseFormationProfile.Id = CoalitionFormationProfileId::DefiasLoose;
    _defiasLooseFormationProfile.Kind = AgentGroupKind::Loose;
    _defiasLooseFormationProfile.CreatureEntry = _defiasGroupCreatureEntry;
    _defiasLooseFormationProfile.MinMembers = _groupPolicyConfig.LooseMinMembers;
    _defiasLooseFormationProfile.MaxMembers = _groupPolicyConfig.LooseMaxMembers;
    _defiasLooseFormationProfile.FormationRadius = _defiasGroupFormationRadius;

    // Milestone 2.12G1: its own summary log is deferred to right after the
    // WolfLoose one below (not logged here) - _coalitionFormationMaxInFlight
    // is not actually populated from AIWorld.CoalitionFormationMaxInFlight
    // until later in this same function, so logging it at this point would
    // print its stale default-member-initializer value, not the real
    // configured one.

    // Milestone 2.12E4C2: deliberately larger than AIWorld.WolfGroupFormationRadius
    // - see CoalitionMaintenanceProfile.h's own LeaveRadius comment for the
    // hysteresis this gap exists to give. Clamped to never fall below
    // FormationRadius (a LeaveRadius smaller than FormationRadius would
    // defeat that hysteresis entirely - a member could form a coalition
    // and immediately be eligible to leave it again the very next
    // maintenance pass).
    // Milestone 2.12F2 P2 fix, round 3 (STATIC review): deliberately no
    // upper bound against ActionSystem::CoordinationMoveToRangeYards() here
    // any more - an earlier version of this fix clamped both this and
    // AIWorld.WolfGroupFormationRadius above against it, which coupled
    // Formation/Maintenance policy (a capability with its own independent
    // enable flag, AIWorld.WolfGroupAutoFormation/AIWorld.CoalitionMaintenance)
    // to a completely different capability's own execution-layer limit
    // (AIWorld.GroupCoordination, off by default) - a group could form and
    // be maintained at a radius that has nothing to do with whether
    // automatic Regroup movement is even enabled, let alone what range it
    // happens to be configured for right now. F2 (coordination) has no
    // business narrowing what F1/maintenance (formation/leave) already
    // allowed. The actual protection against an unreachable Regroup lives
    // downstream instead, where it belongs - AIWorldMgr::
    // DispatchGroupMemberActionProposal()'s own explicit "unreachable
    // coordination member" check skips a proposal whose target is out of
    // ActionSystem's own range, rather than a proposal never being buildable
    // for such a member in the first place. See CoalitionMaintenanceProfile.h's
    // own LeaveRadius comment for the one invariant this value still keeps:
    // never below FormationRadius.
    float wolfGroupLeaveRadius = sConfigMgr->GetFloatDefault("AIWorld.WolfGroupLeaveRadius", 60.0f);
    if (wolfGroupLeaveRadius < _wolfGroupFormationRadius)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupLeaveRadius ({:.1f}) is lower than AIWorld.WolfGroupFormationRadius ({:.1f}), clamping to match",
            wolfGroupLeaveRadius, _wolfGroupFormationRadius);
        wolfGroupLeaveRadius = _wolfGroupFormationRadius;
    }
    _wolfGroupLeaveRadius = wolfGroupLeaveRadius;

    // Milestone 2.12G1: AIWorld.DefiasGroupLeaveRadius - same hysteresis
    // rule as AIWorld.WolfGroupLeaveRadius above, clamped against its own
    // AIWorld.DefiasGroupFormationRadius, never against WolfLoose's.
    float defiasGroupLeaveRadius = sConfigMgr->GetFloatDefault("AIWorld.DefiasGroupLeaveRadius", 60.0f);
    if (defiasGroupLeaveRadius < _defiasGroupFormationRadius)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupLeaveRadius ({:.1f}) is lower than AIWorld.DefiasGroupFormationRadius ({:.1f}), clamping to match",
            defiasGroupLeaveRadius, _defiasGroupFormationRadius);
        defiasGroupLeaveRadius = _defiasGroupFormationRadius;
    }
    _defiasGroupLeaveRadius = defiasGroupLeaveRadius;

    // Milestone 2.12E4C2: the maintenance pass's own cadence/bound - a
    // dedicated timer, not reused from any existing one (a maintenance
    // pass is neither a formation pass nor a group coarse simulation
    // tick), and its own GroupCoarseSimulationScheduler-backed bounded
    // admission (_maintenanceScheduler/_maintenanceSchedule) so a world
    // with many existing groups cannot spike world-thread work on this
    // pass either - the same reasoning GroupCoarseSimulationScheduler.h's
    // own header comment already gives for the group coarse tick.
    int32 coalitionMaintenanceIntervalMs = sConfigMgr->GetIntDefault("AIWorld.CoalitionMaintenanceIntervalMs", 5000);
    if (coalitionMaintenanceIntervalMs < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.CoalitionMaintenanceIntervalMs ({}) is invalid or too low, clamping to 1000ms", coalitionMaintenanceIntervalMs);
        coalitionMaintenanceIntervalMs = 1000;
    }
    _coalitionMaintenanceIntervalMs = uint32(coalitionMaintenanceIntervalMs);
    _coalitionMaintenanceTimer = 0;

    int32 coalitionMaintenanceMaxPerPass = sConfigMgr->GetIntDefault("AIWorld.CoalitionMaintenanceMaxPerPass", 20);
    if (coalitionMaintenanceMaxPerPass < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.CoalitionMaintenanceMaxPerPass ({}) is invalid or too low, clamping to 1", coalitionMaintenanceMaxPerPass);
        coalitionMaintenanceMaxPerPass = 1;
    }
    _coalitionMaintenanceMaxPerPass = uint32(coalitionMaintenanceMaxPerPass);

    // Milestone 2.12E4C2 P3 fix (STATIC review): the bound on DISCOVERY,
    // separate from _coalitionMaintenanceMaxPerPass's own bound on actual
    // per-group work - see RunCoalitionMaintenance()'s own comment.
    int32 coalitionMaintenanceScanMaxPerPass = sConfigMgr->GetIntDefault("AIWorld.CoalitionMaintenanceScanMaxPerPass", 100);
    if (coalitionMaintenanceScanMaxPerPass < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.CoalitionMaintenanceScanMaxPerPass ({}) is invalid or too low, clamping to 1", coalitionMaintenanceScanMaxPerPass);
        coalitionMaintenanceScanMaxPerPass = 1;
    }
    _coalitionMaintenanceScanMaxPerPass = uint32(coalitionMaintenanceScanMaxPerPass);

    _maintenanceSchedule.clear();
    _maintenanceInFlight.clear();
    _maintenanceScanCursor = GroupId{};
    _maintenanceScanCycleHighWater = GroupId{};

    // Milestone 2.12E4C2 P2 fix (STATIC review): a SEPARATE enable gate
    // from AIWorld.WolfGroupAutoFormation - see
    // _coalitionMaintenanceEnabled's own declaration comment for why
    // creation and maintenance must be independently controllable. Off by
    // default, same as every other not-yet-runtime-verified piece of this
    // feature.
    _coalitionMaintenanceEnabled = sConfigMgr->GetBoolDefault("AIWorld.CoalitionMaintenance", false);

    // Milestone 2.12E4C2: AIWorldMgr's own one existing
    // CoalitionMaintenanceProfile, paired with _wolfLooseFormationProfile
    // above (same ProfileId/Kind, see CoalitionMaintenanceProfile.h for
    // why MinMembers is never independently configured either).
    _wolfLooseMaintenanceProfile.ProfileId = CoalitionFormationProfileId::WolfLoose;
    _wolfLooseMaintenanceProfile.Kind = AgentGroupKind::Loose;
    _wolfLooseMaintenanceProfile.MinMembers = _groupPolicyConfig.LooseMinMembers;
    _wolfLooseMaintenanceProfile.LeaveRadius = _wolfGroupLeaveRadius;

    TC_LOG_INFO("ai.world", "AI wolf coalition maintenance configured enabled={} leaveRadius={:.1f} interval={}ms maxPerPass={} scanMaxPerPass={}",
        _coalitionMaintenanceEnabled, _wolfGroupLeaveRadius, _coalitionMaintenanceIntervalMs, _coalitionMaintenanceMaxPerPass, _coalitionMaintenanceScanMaxPerPass);

    // Milestone 2.12G1: paired with _defiasLooseFormationProfile above -
    // _coalitionMaintenanceEnabled/interval/maxPerPass/scanMaxPerPass are
    // shared across every profile (already logged just above), so this
    // summary only restates DefiasLoose's own LeaveRadius.
    _defiasLooseMaintenanceProfile.ProfileId = CoalitionFormationProfileId::DefiasLoose;
    _defiasLooseMaintenanceProfile.Kind = AgentGroupKind::Loose;
    _defiasLooseMaintenanceProfile.MinMembers = _groupPolicyConfig.LooseMinMembers;
    _defiasLooseMaintenanceProfile.LeaveRadius = _defiasGroupLeaveRadius;

    TC_LOG_INFO("ai.world", "AI defias coalition maintenance configured leaveRadius={:.1f}", _defiasGroupLeaveRadius);

    // Milestone 2.12F2: a SEPARATE enable gate from AIWorld.WolfGroupAutoFormation/
    // AIWorld.CoalitionMaintenance - see _groupCoordinationEnabled's own
    // declaration comment for why. Off by default, same as every other
    // not-yet-runtime-verified piece of this feature.
    _groupCoordinationEnabled = sConfigMgr->GetBoolDefault("AIWorld.GroupCoordination", false);

    bool wolfGroupRegroupEnabled = sConfigMgr->GetBoolDefault("AIWorld.WolfGroupRegroupEnabled", false);

    // Milestone 2.12F2: deliberately no relationship enforced against
    // AIWorld.WolfGroupFormationRadius/AIWorld.WolfGroupLeaveRadius the way
    // LeaveRadius is clamped against FormationRadius above - unlike that
    // pair (which exists purely to create hysteresis around a single
    // shared trigger), RegroupRadius answers a different question
    // ("how far from the group's own fixed territory point before a
    // member is considered dispersed", see AgentGroupIntentSystem.h) and a
    // profile is free to set it independently of either.
    float wolfGroupRegroupRadius = sConfigMgr->GetFloatDefault("AIWorld.WolfGroupRegroupRadius", 20.0f);
    if (wolfGroupRegroupRadius < 1.0f)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupRegroupRadius ({:.1f}) is invalid or too low, clamping to 1.0", wolfGroupRegroupRadius);
        wolfGroupRegroupRadius = 1.0f;
    }

    // Milestone 2.12F2 P3 fix (STATIC review): unlike AIWorld.WolfGroupFormationRadius/
    // AIWorld.WolfGroupLeaveRadius (deliberately NOT clamped against this
    // bound any more - see AIWorld.WolfGroupLeaveRadius's own comment
    // above for why Formation/Maintenance policy must stay independent of
    // Coordination's own execution limit), RegroupRadius genuinely IS
    // Coordination-layer config - it is the trigger threshold
    // AgentGroupIntentSystem itself uses to decide a member is dispersed
    // enough to propose a Regroup at all, so clamping it here is this
    // capability bounding its own config, not one capability bounding
    // another's. A RegroupRadius past this bound would mean no Regroup
    // this profile ever proposes could actually pass ValidateMoveTo() to
    // begin with.
    if (wolfGroupRegroupRadius > ActionSystem::CoordinationMoveToRangeYards())
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupRegroupRadius ({:.1f}) exceeds the ActionSystem Regroup range bound ({:.1f}), clamping to match",
            wolfGroupRegroupRadius, ActionSystem::CoordinationMoveToRangeYards());
        wolfGroupRegroupRadius = ActionSystem::CoordinationMoveToRangeYards();
    }

    // Milestone 2.12G2 P2 fix, P3 fix (STATIC review): off by default
    // (AIWorld.WolfGroupRoamEnabled), same as every other not-yet-runtime-
    // verified piece of this feature. RoamDistance/RoamArrivalRadius used
    // to be clamped only against RegroupRadius, on the (undocumented,
    // unenforced) assumption that RegroupRadius < LeaveRadius - but
    // RegroupRadius is deliberately independent maintenance-unrelated
    // Coordination-layer config (see its own comment above), so a
    // perfectly valid RegroupRadius > LeaveRadius let ROAM legally send a
    // member past LeaveRadius, where CoalitionMaintenanceSystem's own
    // automatic Leave would then remove it - two generic policies fighting
    // each other. roamEnvelopeMax below is the REAL bound - whichever of
    // RegroupRadius/LeaveRadius is tighter - so a Roam target can never
    // exceed either. Landing EXACTLY at either boundary is still safe:
    // both AgentGroupIntentSystem's own Regroup check and
    // CoalitionMaintenanceSystem's own Leave check trigger only on
    // strictly-GREATER-than their own radius (distanceSq <= radiusSq is
    // "not yet triggered" in both), so an inclusive upper clamp is
    // correct, not just a conservative approximation.
    //
    // The old independent clamp chain (floor first, then a ">=" ceiling
    // that could recompute a value BELOW its own just-enforced floor, e.g.
    // RoamDistance clamped up to 1.0 then immediately back down to
    // RegroupRadius*0.5 = 0.5 when RegroupRadius itself was only 1.0) is
    // replaced by one consistent computation: first prove the configured
    // envelope actually has room for both documented floors at once
    // (MinRoamDistance/MinRoamArrivalRadius below) - if it does not,
    // ROAM is fail-closed disabled for this profile entirely, rather than
    // silently accepting a value that violates its own documented
    // minimum. Only once that is proven does clamping proceed, and every
    // subsequent clamp target (roamEnvelopeMax itself, or
    // wolfGroupRoamDistance which is by then already >= MinRoamDistance)
    // is provably still >= the floor it is being compared against.
    bool wolfGroupRoamEnabled = sConfigMgr->GetBoolDefault("AIWorld.WolfGroupRoamEnabled", false);
    float wolfGroupRoamDistance = sConfigMgr->GetFloatDefault("AIWorld.WolfGroupRoamDistance", 10.0f);
    float wolfGroupRoamArrivalRadius = sConfigMgr->GetFloatDefault("AIWorld.WolfGroupRoamArrivalRadius", 5.0f);

    int32 wolfGroupRoamIntervalMsRaw = sConfigMgr->GetIntDefault("AIWorld.WolfGroupRoamIntervalMs", 15000);
    if (wolfGroupRoamIntervalMsRaw < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupRoamIntervalMs ({}) is invalid or too low, clamping to 1000ms", wolfGroupRoamIntervalMsRaw);
        wolfGroupRoamIntervalMsRaw = 1000;
    }
    uint32 wolfGroupRoamIntervalMs = uint32(wolfGroupRoamIntervalMsRaw);

    constexpr float MinRoamDistance = 1.0f;
    constexpr float MinRoamArrivalRadius = 0.5f;

    float wolfRoamEnvelopeMax = std::min(wolfGroupRegroupRadius, _wolfGroupLeaveRadius);

    // Milestone 2.12G2 P2 fix, round 2 (STATIC review): feasibility is NOT
    // "does the envelope fit both floors ADDED TOGETHER"
    // (MinRoamDistance + MinRoamArrivalRadius) - RoamArrivalRadius only
    // has to stay strictly below whatever RoamDistance ends up being, not
    // below the envelope minus RoamDistance's own floor, so the two
    // floors do not each need their own separate slice of the envelope.
    // A valid (RoamDistance, RoamArrivalRadius) pair exists iff the
    // envelope can hold RoamDistance's own floor at all
    // (wolfRoamEnvelopeMax >= MinRoamDistance) AND that same envelope
    // leaves ANY room strictly below it for RoamArrivalRadius's own floor
    // (wolfRoamEnvelopeMax > MinRoamArrivalRadius) - taking RoamDistance
    // == wolfRoamEnvelopeMax itself is always the most permissive choice,
    // so if that specific pair does not fit, no pair does. With
    // MinRoamDistance(1.0) > MinRoamArrivalRadius(0.5), envelope == 1.0
    // (RoamDistance=1.0, RoamArrivalRadius=0.5) is a genuinely valid pair
    // that the old "sum of minima" (1.5) check wrongly rejected.
    bool wolfRoamEnvelopeFeasible = wolfRoamEnvelopeMax >= MinRoamDistance && wolfRoamEnvelopeMax > MinRoamArrivalRadius;

    if (wolfGroupRoamEnabled && !wolfRoamEnvelopeFeasible)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.WolfGroupRoamEnabled is set, but min(AIWorld.WolfGroupRegroupRadius={:.1f}, AIWorld.WolfGroupLeaveRadius={:.1f})={:.1f} leaves no room for any RoamDistance/RoamArrivalRadius pair respecting their own {:.1f}/{:.1f} minimums - disabling WolfLoose ROAM",
            wolfGroupRegroupRadius, _wolfGroupLeaveRadius, wolfRoamEnvelopeMax, MinRoamDistance, MinRoamArrivalRadius);
        wolfGroupRoamEnabled = false;
    }

    if (wolfGroupRoamDistance < MinRoamDistance)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupRoamDistance ({:.1f}) is invalid or too low, clamping to {:.1f}", wolfGroupRoamDistance, MinRoamDistance);
        wolfGroupRoamDistance = MinRoamDistance;
    }
    if (wolfGroupRoamDistance > wolfRoamEnvelopeMax)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupRoamDistance ({:.1f}) exceeds min(RegroupRadius, LeaveRadius) ({:.1f}) - a roam target past either could immediately trigger REGROUP or an automatic Leave, clamping to match",
            wolfGroupRoamDistance, wolfRoamEnvelopeMax);
        wolfGroupRoamDistance = wolfRoamEnvelopeMax;
    }

    if (wolfGroupRoamArrivalRadius < MinRoamArrivalRadius)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupRoamArrivalRadius ({:.1f}) is invalid or too low, clamping to {:.1f}", wolfGroupRoamArrivalRadius, MinRoamArrivalRadius);
        wolfGroupRoamArrivalRadius = MinRoamArrivalRadius;
    }
    if (wolfGroupRoamArrivalRadius >= wolfGroupRoamDistance)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupRoamArrivalRadius ({:.1f}) is not smaller than the effective AIWorld.WolfGroupRoamDistance ({:.1f}) - no roam target could ever count as reached, clamping to {:.1f}",
            wolfGroupRoamArrivalRadius, wolfGroupRoamDistance, wolfGroupRoamDistance * 0.5f);
        wolfGroupRoamArrivalRadius = wolfGroupRoamDistance * 0.5f;
    }

    // Milestone 2.12G3C2: off by default (AIWorld.WolfGroupHuntEnabled) -
    // the same "not-yet-runtime-verified capability defaults off" discipline
    // every other new coordination behavior gets. Validated the exact same
    // way HuntIntentSystem::Evaluate() itself already fails closed
    // (HuntTargetCreatureEntry == 0, or HuntAcquisitionRadius non-finite/
    // <= 0) - re-checked here purely for OPERATOR VISIBILITY: without this,
    // an admin who sets WolfGroupHuntEnabled=1 but leaves
    // HuntTargetCreatureEntry at its 0 default would see HUNT silently
    // never trigger, with no log line explaining why. HuntIntentSystem's
    // own fail-closed checks are the actual safety guarantee either way -
    // this config-time check adds visibility, not a second independent
    // enforcement point.
    bool wolfGroupHuntEnabled = sConfigMgr->GetBoolDefault("AIWorld.WolfGroupHuntEnabled", false);
    uint32 wolfGroupHuntTargetCreatureEntry = uint32(sConfigMgr->GetIntDefault("AIWorld.WolfGroupHuntTargetCreatureEntry", 0));
    float wolfGroupHuntAcquisitionRadius = sConfigMgr->GetFloatDefault("AIWorld.WolfGroupHuntAcquisitionRadius", 30.0f);
    uint32 wolfGroupHuntObservationMaxAgeMs = uint32(std::max(0, sConfigMgr->GetIntDefault("AIWorld.WolfGroupHuntObservationMaxAgeMs", 5000)));

    if (wolfGroupHuntEnabled && (wolfGroupHuntTargetCreatureEntry == 0 || !std::isfinite(wolfGroupHuntAcquisitionRadius) ||
        wolfGroupHuntAcquisitionRadius <= 0.0f || wolfGroupHuntObservationMaxAgeMs == 0))
    {
        TC_LOG_ERROR("ai.world", "AIWorld.WolfGroupHuntEnabled is set, but HuntTargetCreatureEntry={}/HuntAcquisitionRadius={:.1f}/HuntObservationMaxAgeMs={} is invalid - disabling WolfLoose HUNT",
            wolfGroupHuntTargetCreatureEntry, wolfGroupHuntAcquisitionRadius, wolfGroupHuntObservationMaxAgeMs);
        wolfGroupHuntEnabled = false;
    }

    _wolfLooseCoordinationProfile.ProfileId = CoalitionFormationProfileId::WolfLoose;
    _wolfLooseCoordinationProfile.Kind = AgentGroupKind::Loose;
    _wolfLooseCoordinationProfile.RegroupEnabled = wolfGroupRegroupEnabled;
    _wolfLooseCoordinationProfile.RegroupRadius = wolfGroupRegroupRadius;
    _wolfLooseCoordinationProfile.RoamEnabled = wolfGroupRoamEnabled;
    _wolfLooseCoordinationProfile.RoamDistance = wolfGroupRoamDistance;
    _wolfLooseCoordinationProfile.RoamIntervalMs = wolfGroupRoamIntervalMs;
    _wolfLooseCoordinationProfile.RoamArrivalRadius = wolfGroupRoamArrivalRadius;
    _wolfLooseCoordinationProfile.HuntEnabled = wolfGroupHuntEnabled;
    _wolfLooseCoordinationProfile.HuntTargetCreatureEntry = wolfGroupHuntTargetCreatureEntry;
    _wolfLooseCoordinationProfile.HuntAcquisitionRadius = wolfGroupHuntAcquisitionRadius;
    _wolfLooseCoordinationProfile.HuntObservationMaxAgeMs = wolfGroupHuntObservationMaxAgeMs;

    TC_LOG_INFO("ai.world", "AI wolf coalition coordination configured regroupEnabled={} regroupRadius={:.1f} roamEnabled={} roamDistance={:.1f} roamIntervalMs={} roamArrivalRadius={:.1f} "
        "huntEnabled={} huntTargetCreatureEntry={} huntAcquisitionRadius={:.1f} huntObservationMaxAgeMs={}",
        wolfGroupRegroupEnabled, wolfGroupRegroupRadius, wolfGroupRoamEnabled, wolfGroupRoamDistance, wolfGroupRoamIntervalMs, wolfGroupRoamArrivalRadius,
        wolfGroupHuntEnabled, wolfGroupHuntTargetCreatureEntry, wolfGroupHuntAcquisitionRadius, wolfGroupHuntObservationMaxAgeMs);

    // Milestone 2.12G1: same shape as WolfLoose above - RegroupRadius is
    // still Coordination-layer config (see AIWorld.WolfGroupRegroupRadius's
    // own comment) so it is still independently clamped against
    // ActionSystem::CoordinationMoveToRangeYards() here, per-profile.
    bool defiasGroupRegroupEnabled = sConfigMgr->GetBoolDefault("AIWorld.DefiasGroupRegroupEnabled", false);

    float defiasGroupRegroupRadius = sConfigMgr->GetFloatDefault("AIWorld.DefiasGroupRegroupRadius", 20.0f);
    if (defiasGroupRegroupRadius < 1.0f)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupRegroupRadius ({:.1f}) is invalid or too low, clamping to 1.0", defiasGroupRegroupRadius);
        defiasGroupRegroupRadius = 1.0f;
    }
    if (defiasGroupRegroupRadius > ActionSystem::CoordinationMoveToRangeYards())
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupRegroupRadius ({:.1f}) exceeds the ActionSystem Regroup range bound ({:.1f}), clamping to match",
            defiasGroupRegroupRadius, ActionSystem::CoordinationMoveToRangeYards());
        defiasGroupRegroupRadius = ActionSystem::CoordinationMoveToRangeYards();
    }

    // Milestone 2.12G2 P2 fix, P3 fix (STATIC review): same shape as
    // WolfLoose above - see its own comment for why the clamp bound is
    // min(RegroupRadius, LeaveRadius), computed once, with a fail-closed
    // disable of ROAM when the configured envelope leaves no room for
    // either documented floor, rather than a clamp chain that could
    // undercut its own minimum.
    bool defiasGroupRoamEnabled = sConfigMgr->GetBoolDefault("AIWorld.DefiasGroupRoamEnabled", false);
    float defiasGroupRoamDistance = sConfigMgr->GetFloatDefault("AIWorld.DefiasGroupRoamDistance", 10.0f);
    float defiasGroupRoamArrivalRadius = sConfigMgr->GetFloatDefault("AIWorld.DefiasGroupRoamArrivalRadius", 5.0f);

    int32 defiasGroupRoamIntervalMsRaw = sConfigMgr->GetIntDefault("AIWorld.DefiasGroupRoamIntervalMs", 15000);
    if (defiasGroupRoamIntervalMsRaw < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupRoamIntervalMs ({}) is invalid or too low, clamping to 1000ms", defiasGroupRoamIntervalMsRaw);
        defiasGroupRoamIntervalMsRaw = 1000;
    }
    uint32 defiasGroupRoamIntervalMs = uint32(defiasGroupRoamIntervalMsRaw);

    float defiasRoamEnvelopeMax = std::min(defiasGroupRegroupRadius, _defiasGroupLeaveRadius);

    // Milestone 2.12G2 P2 fix, round 2 (STATIC review): see
    // wolfRoamEnvelopeFeasible's own comment above - feasibility is
    // whether ANY valid (RoamDistance, RoamArrivalRadius) pair fits, not
    // whether the envelope fits both floors added together.
    bool defiasRoamEnvelopeFeasible = defiasRoamEnvelopeMax >= MinRoamDistance && defiasRoamEnvelopeMax > MinRoamArrivalRadius;

    if (defiasGroupRoamEnabled && !defiasRoamEnvelopeFeasible)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.DefiasGroupRoamEnabled is set, but min(AIWorld.DefiasGroupRegroupRadius={:.1f}, AIWorld.DefiasGroupLeaveRadius={:.1f})={:.1f} leaves no room for any RoamDistance/RoamArrivalRadius pair respecting their own {:.1f}/{:.1f} minimums - disabling DefiasLoose ROAM",
            defiasGroupRegroupRadius, _defiasGroupLeaveRadius, defiasRoamEnvelopeMax, MinRoamDistance, MinRoamArrivalRadius);
        defiasGroupRoamEnabled = false;
    }

    if (defiasGroupRoamDistance < MinRoamDistance)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupRoamDistance ({:.1f}) is invalid or too low, clamping to {:.1f}", defiasGroupRoamDistance, MinRoamDistance);
        defiasGroupRoamDistance = MinRoamDistance;
    }
    if (defiasGroupRoamDistance > defiasRoamEnvelopeMax)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupRoamDistance ({:.1f}) exceeds min(RegroupRadius, LeaveRadius) ({:.1f}) - a roam target past either could immediately trigger REGROUP or an automatic Leave, clamping to match",
            defiasGroupRoamDistance, defiasRoamEnvelopeMax);
        defiasGroupRoamDistance = defiasRoamEnvelopeMax;
    }

    if (defiasGroupRoamArrivalRadius < MinRoamArrivalRadius)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupRoamArrivalRadius ({:.1f}) is invalid or too low, clamping to {:.1f}", defiasGroupRoamArrivalRadius, MinRoamArrivalRadius);
        defiasGroupRoamArrivalRadius = MinRoamArrivalRadius;
    }
    if (defiasGroupRoamArrivalRadius >= defiasGroupRoamDistance)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DefiasGroupRoamArrivalRadius ({:.1f}) is not smaller than the effective AIWorld.DefiasGroupRoamDistance ({:.1f}) - no roam target could ever count as reached, clamping to {:.1f}",
            defiasGroupRoamArrivalRadius, defiasGroupRoamDistance, defiasGroupRoamDistance * 0.5f);
        defiasGroupRoamArrivalRadius = defiasGroupRoamDistance * 0.5f;
    }

    // Milestone 2.12G3C2: same shape as WolfLoose above - see its own
    // comment for why this config-time check is visibility-only,
    // HuntIntentSystem::Evaluate() already fails closed either way.
    bool defiasGroupHuntEnabled = sConfigMgr->GetBoolDefault("AIWorld.DefiasGroupHuntEnabled", false);
    uint32 defiasGroupHuntTargetCreatureEntry = uint32(sConfigMgr->GetIntDefault("AIWorld.DefiasGroupHuntTargetCreatureEntry", 0));
    float defiasGroupHuntAcquisitionRadius = sConfigMgr->GetFloatDefault("AIWorld.DefiasGroupHuntAcquisitionRadius", 30.0f);
    uint32 defiasGroupHuntObservationMaxAgeMs = uint32(std::max(0, sConfigMgr->GetIntDefault("AIWorld.DefiasGroupHuntObservationMaxAgeMs", 5000)));

    if (defiasGroupHuntEnabled && (defiasGroupHuntTargetCreatureEntry == 0 || !std::isfinite(defiasGroupHuntAcquisitionRadius) ||
        defiasGroupHuntAcquisitionRadius <= 0.0f || defiasGroupHuntObservationMaxAgeMs == 0))
    {
        TC_LOG_ERROR("ai.world", "AIWorld.DefiasGroupHuntEnabled is set, but HuntTargetCreatureEntry={}/HuntAcquisitionRadius={:.1f}/HuntObservationMaxAgeMs={} is invalid - disabling DefiasLoose HUNT",
            defiasGroupHuntTargetCreatureEntry, defiasGroupHuntAcquisitionRadius, defiasGroupHuntObservationMaxAgeMs);
        defiasGroupHuntEnabled = false;
    }

    _defiasLooseCoordinationProfile.ProfileId = CoalitionFormationProfileId::DefiasLoose;
    _defiasLooseCoordinationProfile.Kind = AgentGroupKind::Loose;
    _defiasLooseCoordinationProfile.RegroupEnabled = defiasGroupRegroupEnabled;
    _defiasLooseCoordinationProfile.RegroupRadius = defiasGroupRegroupRadius;
    _defiasLooseCoordinationProfile.RoamEnabled = defiasGroupRoamEnabled;
    _defiasLooseCoordinationProfile.RoamDistance = defiasGroupRoamDistance;
    _defiasLooseCoordinationProfile.RoamIntervalMs = defiasGroupRoamIntervalMs;
    _defiasLooseCoordinationProfile.RoamArrivalRadius = defiasGroupRoamArrivalRadius;
    _defiasLooseCoordinationProfile.HuntEnabled = defiasGroupHuntEnabled;
    _defiasLooseCoordinationProfile.HuntTargetCreatureEntry = defiasGroupHuntTargetCreatureEntry;
    _defiasLooseCoordinationProfile.HuntAcquisitionRadius = defiasGroupHuntAcquisitionRadius;
    _defiasLooseCoordinationProfile.HuntObservationMaxAgeMs = defiasGroupHuntObservationMaxAgeMs;

    TC_LOG_INFO("ai.world", "AI defias coalition coordination configured regroupEnabled={} regroupRadius={:.1f} roamEnabled={} roamDistance={:.1f} roamIntervalMs={} roamArrivalRadius={:.1f} "
        "huntEnabled={} huntTargetCreatureEntry={} huntAcquisitionRadius={:.1f} huntObservationMaxAgeMs={}",
        defiasGroupRegroupEnabled, defiasGroupRegroupRadius, defiasGroupRoamEnabled, defiasGroupRoamDistance, defiasGroupRoamIntervalMs, defiasGroupRoamArrivalRadius,
        defiasGroupHuntEnabled, defiasGroupHuntTargetCreatureEntry, defiasGroupHuntAcquisitionRadius, defiasGroupHuntObservationMaxAgeMs);

    int32 groupCoordinationIntervalMs = sConfigMgr->GetIntDefault("AIWorld.GroupCoordinationIntervalMs", 5000);
    if (groupCoordinationIntervalMs < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.GroupCoordinationIntervalMs ({}) is invalid or too low, clamping to 1000ms", groupCoordinationIntervalMs);
        groupCoordinationIntervalMs = 1000;
    }
    _groupCoordinationIntervalMs = uint32(groupCoordinationIntervalMs);
    _groupCoordinationTimer = 0;

    int32 groupCoordinationScanMaxPerPass = sConfigMgr->GetIntDefault("AIWorld.GroupCoordinationScanMaxPerPass", 100);
    if (groupCoordinationScanMaxPerPass < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.GroupCoordinationScanMaxPerPass ({}) is invalid or too low, clamping to 1", groupCoordinationScanMaxPerPass);
        groupCoordinationScanMaxPerPass = 1;
    }
    _groupCoordinationScanMaxPerPass = uint32(groupCoordinationScanMaxPerPass);

    _coordinationScanCursor = GroupId{};
    _coordinationScanCycleHighWater = GroupId{};

    TC_LOG_INFO("ai.world", "AI group coordination configured enabled={} regroupEnabled={} regroupRadius={:.1f} interval={}ms scanMaxPerPass={}",
        _groupCoordinationEnabled, wolfGroupRegroupEnabled, wolfGroupRegroupRadius, _groupCoordinationIntervalMs, _groupCoordinationScanMaxPerPass);

    // Milestone 2.12G1: enabled/interval/scanMaxPerPass are the same
    // shared AIWorld.GroupCoordination settings already logged just above.
    TC_LOG_INFO("ai.world", "AI defias group coordination configured regroupEnabled={} regroupRadius={:.1f}",
        defiasGroupRegroupEnabled, defiasGroupRegroupRadius);

    // Milestone 2.12E4R P3 fix (STATIC review): the hard ceiling on how
    // many DIFFERENT profiles' formations can be in flight at once - see
    // _coalitionFormationMaxInFlight's own declaration comment. 1 (the
    // default) reproduces 2.12E4B's own "at most one automatic formation
    // in flight, period" behavior exactly for a single profile - now that
    // 2.12G1 adds DefiasLoose alongside WolfLoose, the default of 1 also
    // means the two profiles' own automatic formation sagas cannot run
    // concurrently (whichever gets there first blocks the other's own
    // RunCoalitionFormation() call until its CreateGroup/Join chain
    // resolves) - deliberately left at its existing default rather than
    // raised as part of this milestone; an operator who wants concurrent
    // formation across profiles can raise AIWorld.CoalitionFormationMaxInFlight
    // explicitly.
    int32 coalitionFormationMaxInFlight = sConfigMgr->GetIntDefault("AIWorld.CoalitionFormationMaxInFlight", 1);
    if (coalitionFormationMaxInFlight < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.CoalitionFormationMaxInFlight ({}) is invalid or too low, clamping to 1", coalitionFormationMaxInFlight);
        coalitionFormationMaxInFlight = 1;
    }
    _coalitionFormationMaxInFlight = uint32(coalitionFormationMaxInFlight);

    _formationInFlight.clear();
    _formationReservedMembers.clear();

    TC_LOG_INFO("ai.world", "AI wolf coalition formation configured autoFormation={} creatureEntry={} interval={}ms radius={:.1f} maxInFlight={}",
        _wolfGroupAutoFormation, _wolfGroupCreatureEntry, _wolfGroupFormationIntervalMs, _wolfGroupFormationRadius, _coalitionFormationMaxInFlight);

    TC_LOG_INFO("ai.world", "AI defias coalition formation configured autoFormation={} creatureEntry={} interval={}ms radius={:.1f} maxInFlight={}",
        _defiasGroupAutoFormation, _defiasGroupCreatureEntry, _defiasGroupFormationIntervalMs, _defiasGroupFormationRadius, _coalitionFormationMaxInFlight);

    // Milestone 2.12E3B: off by default - see RunGroupPolicySmokeTest()'s
    // own comment. Read here (not inline at the call site below) purely to
    // sit next to the config values its pure half exercises.
    bool testGroupPolicy = sConfigMgr->GetBoolDefault("AIWorld.TestGroupPolicy", false);

    // Milestone 2.12E4C1: off by default - see RunCoalitionMaintenanceSmokeTest()'s
    // own comment. Read here for the same reason testGroupPolicy is: sits
    // next to the one other "pure smoke test" toggle Initialize() already
    // has.
    bool testCoalitionMaintenance = sConfigMgr->GetBoolDefault("AIWorld.TestCoalitionMaintenance", false);

    // Milestone 2.12F1: off by default - see RunGroupIntentSmokeTest()'s
    // own comment. Read here for the same reason the other pure smoke
    // test toggles are: sits next to them rather than off on its own.
    bool testGroupIntent = sConfigMgr->GetBoolDefault("AIWorld.TestGroupIntent", false);

    // Milestone 2.12F2: off by default - see RunGroupIntentProjectorSmokeTest()'s
    // own comment.
    bool testGroupIntentProjector = sConfigMgr->GetBoolDefault("AIWorld.TestGroupIntentProjector", false);

    // Milestone 2.12G3B: off by default - see RunHuntIntentSmokeTest()'s
    // own comment. Read here for the same reason the other pure smoke
    // test toggles are: sits next to them rather than off on its own.
    bool testHuntIntent = sConfigMgr->GetBoolDefault("AIWorld.TestHuntIntent", false);

    // Milestone 2.12G3C1: off by default - see
    // RunHuntActionValidationSmokeTest()'s own comment.
    bool testHuntActionValidation = sConfigMgr->GetBoolDefault("AIWorld.TestHuntActionValidation", false);

    // Milestone 2.12G3D1: off by default - see
    // RunHuntArrivalOwnershipSmokeTest()'s own comment.
    bool testHuntArrivalOwnership = sConfigMgr->GetBoolDefault("AIWorld.TestHuntArrivalOwnership", false);

    // Milestone 2.12F4A: off by default (0 - the same 0-means-disabled
    // convention every other AgentId-keyed test hook in this file already
    // uses) - see RunControlModeSmokeTest()'s own comment. Names a real,
    // already-registered AgentId the hook runs its pure ActionSystem::
    // Validate() ControlMode-gate proof against - never a fake/ghost
    // record. Fail-closed parsing: sConfigMgr->GetIntDefault() returns a
    // signed int32, and AgentId::Value is unsigned (see AgentId.h); a
    // negative value is rejected here rather than silently reinterpreted
    // into some enormous, never-real AgentId.
    int32 testControlModeAgentIdRaw = sConfigMgr->GetIntDefault("AIWorld.TestControlMode", 0);
    if (testControlModeAgentIdRaw < 0)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.TestControlMode ({}) is negative, which cannot name a real AgentId, disabling this test hook",
            testControlModeAgentIdRaw);
        testControlModeAgentIdRaw = 0;
    }
    AgentId testControlModeAgentId{ uint64(testControlModeAgentIdRaw) };

    uint32 testMapId = uint32(sConfigMgr->GetIntDefault("AIWorld.TestMapId", 0));
    uint64 testSpawnId = uint64(sConfigMgr->GetIntDefault("AIWorld.TestSpawnId", 0));

    // Milestone 2.12E1 P2 fix (STATIC review, round 2): three EXISTING
    // AgentIds, not spawn ids - see RunGroupLifecycleSmokeTest()'s own
    // comment for why this no longer fabricates test member agents itself.
    // All three default to 0 (unset); the smoke test only runs once all
    // three resolve to a real, already-registered AgentRecord.
    AgentId testGroupMemberAgentId1{ uint64(sConfigMgr->GetIntDefault("AIWorld.TestGroupMemberAgentId1", 0)) };
    AgentId testGroupMemberAgentId2{ uint64(sConfigMgr->GetIntDefault("AIWorld.TestGroupMemberAgentId2", 0)) };
    AgentId testGroupMemberAgentId3{ uint64(sConfigMgr->GetIntDefault("AIWorld.TestGroupMemberAgentId3", 0)) };

    // Milestone 2.12E4R test hook: an existing GroupId to Manually dissolve
    // once, at this startup only - see RunTestDissolveGroup()'s own comment.
    // Default 0 (unset) means disabled, the same 0-means-disabled
    // convention AIWorld.TestSpawnId/TestGroupMemberAgentId1-3 already use.
    GroupId testDissolveGroupId{ uint64(sConfigMgr->GetIntDefault("AIWorld.TestDissolveGroupId", 0)) };

    // Milestone 2.12E4C2 P2 fix (STATIC review): the controlled legacy-
    // provenance adoption path - see RunGroupProfileAdoption()'s own
    // comment. Default 0 (unset) means disabled, the same convention every
    // other one-shot GroupId hook here already uses.
    GroupId adoptGroupId{ uint64(sConfigMgr->GetIntDefault("AIWorld.AdoptGroupId", 0)) };

    // Milestone 2.12E4C2 P3 hardening (STATIC review): the raw config
    // value is validated BEFORE any cast to CoalitionFormationProfileId,
    // not after - an earlier version cast straight to uint8 first (e.g.
    // AIWorld.AdoptGroupProfileId = 257 would truncate to 1/WolfLoose and
    // pass RunGroupProfileAdoption()'s own allow-list, silently accepting
    // a value that was never actually 1), which is fail-OPEN config
    // parsing, not fail-closed. An explicit switch over int32 means only
    // the exact literal values CoalitionFormationProfileId actually
    // defines are ever accepted; anything else - including a value that
    // would alias to a real one after truncation - stays Invalid and logs
    // why, and RunGroupProfileAdoption() itself still refuses Invalid
    // (via GetCoalitionProfileKind()) if AIWorld.AdoptGroupId is set.
    int32 rawAdoptGroupProfileId = sConfigMgr->GetIntDefault("AIWorld.AdoptGroupProfileId", 0);
    CoalitionFormationProfileId adoptGroupProfileId = CoalitionFormationProfileId::Invalid;
    switch (rawAdoptGroupProfileId)
    {
        case int32(CoalitionFormationProfileId::Invalid):
            break; // unset/disabled - stays Invalid
        case int32(CoalitionFormationProfileId::WolfLoose):
            adoptGroupProfileId = CoalitionFormationProfileId::WolfLoose;
            break;
        case int32(CoalitionFormationProfileId::DefiasLoose):
            adoptGroupProfileId = CoalitionFormationProfileId::DefiasLoose;
            break;
        default:
            TC_LOG_ERROR("ai.world", "AIWorld.AdoptGroupProfileId ({}) is not a recognized CoalitionFormationProfileId - "
                "group profile adoption (if AIWorld.AdoptGroupId is set) will be refused", rawAdoptGroupProfileId);
            break;
    }

    std::string aiHost = sConfigMgr->GetStringDefault("AIWorld.AIHost", "ai-server");
    std::string aiPort = std::to_string(sConfigMgr->GetIntDefault("AIWorld.AIPort", 8000));

    int32 requestTimeoutMs = sConfigMgr->GetIntDefault("AIWorld.RequestTimeoutMs", 1000);
    if (requestTimeoutMs < 100)
    {
        TC_LOG_WARN("ai.world", "AIWorld.RequestTimeoutMs ({}) is invalid or too low, clamping to 100ms", requestTimeoutMs);
        requestTimeoutMs = 100;
    }

    // Milestone 2.10A: the hard global cap RunDecisionScheduler() admits
    // against, also enforced independently by AIClient itself (defense in
    // depth - see AIClient::SubmitDecision()).
    int32 decisionMaxInFlight = sConfigMgr->GetIntDefault("AIWorld.DecisionMaxInFlight", 4);
    if (decisionMaxInFlight < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.DecisionMaxInFlight ({}) is invalid or too low, clamping to 1", decisionMaxInFlight);
        decisionMaxInFlight = 1;
    }
    _decisionMaxInFlight = uint32(decisionMaxInFlight);

    int32 healthIntervalMs = sConfigMgr->GetIntDefault("AIWorld.HealthIntervalMs", 10000);
    if (healthIntervalMs < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.HealthIntervalMs ({}) is invalid or too low, clamping to 1000ms", healthIntervalMs);
        healthIntervalMs = 1000;
    }
    _healthIntervalMs = uint32(healthIntervalMs);
    _healthTimer = 0;

    int32 sightRange = sConfigMgr->GetIntDefault("AIWorld.PerceptionSightRange", 40);
    if (sightRange < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.PerceptionSightRange ({}) is invalid or too low, clamping to 1", sightRange);
        sightRange = 1;
    }
    _perceptionSightRange = uint32(sightRange);

    int32 nearbyPerceptionIntervalMs = sConfigMgr->GetIntDefault("AIWorld.NearbyPerceptionIntervalMs", 1000);
    if (nearbyPerceptionIntervalMs < 100)
    {
        TC_LOG_WARN("ai.world", "AIWorld.NearbyPerceptionIntervalMs ({}) is invalid or too low, clamping to 100ms", nearbyPerceptionIntervalMs);
        nearbyPerceptionIntervalMs = 100;
    }
    _nearbyPerceptionIntervalMs = uint32(nearbyPerceptionIntervalMs);
    _nearbyPerceptionTimer = 0;

    int32 shortTermMemoryTtlMs = sConfigMgr->GetIntDefault("AIWorld.ShortTermMemoryTtlMs", 30000);
    if (shortTermMemoryTtlMs < 1000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.ShortTermMemoryTtlMs ({}) is invalid or too low, clamping to 1000ms", shortTermMemoryTtlMs);
        shortTermMemoryTtlMs = 1000;
    }
    _shortTermMemoryTtlMs = uint32(shortTermMemoryTtlMs);
    _memoryMaintenanceTimer = 0;

    _longTermMemoryMinImportance = std::clamp(
        sConfigMgr->GetFloatDefault("AIWorld.LongTermMemoryMinImportance", 0.75f), 0.0f, 1.0f);

    int32 memoryRetrievalTopN = sConfigMgr->GetIntDefault("AIWorld.MemoryRetrievalTopN", 5);
    if (memoryRetrievalTopN < 1)
    {
        TC_LOG_WARN("ai.world", "AIWorld.MemoryRetrievalTopN ({}) is invalid or too low, clamping to 1", memoryRetrievalTopN);
        memoryRetrievalTopN = 1;
    }
    else if (memoryRetrievalTopN > 20)
    {
        TC_LOG_WARN("ai.world", "AIWorld.MemoryRetrievalTopN ({}) is too high, clamping to 20", memoryRetrievalTopN);
        memoryRetrievalTopN = 20;
    }
    _memoryRetrievalTopN = uint32(memoryRetrievalTopN);

    int32 needsUpdateIntervalMs = sConfigMgr->GetIntDefault("AIWorld.NeedsUpdateIntervalMs", 1000);
    if (needsUpdateIntervalMs < 100)
    {
        TC_LOG_WARN("ai.world", "AIWorld.NeedsUpdateIntervalMs ({}) is invalid or too low, clamping to 100ms", needsUpdateIntervalMs);
        needsUpdateIntervalMs = 100;
    }
    _needsUpdateIntervalMs = uint32(needsUpdateIntervalMs);
    _needsUpdateTimer = 0;

    _needsRates.HungerPerSecond = std::clamp(
        sConfigMgr->GetFloatDefault("AIWorld.NeedsHungerRatePerSecond", 0.0002f), 0.0f, 1.0f);
    _needsRates.FatiguePerSecond = std::clamp(
        sConfigMgr->GetFloatDefault("AIWorld.NeedsFatigueRatePerSecond", 0.0001f), 0.0f, 1.0f);
    _needsRates.ResourcePressurePerSecond = std::clamp(
        sConfigMgr->GetFloatDefault("AIWorld.NeedsResourcePressureRatePerSecond", 0.00005f), 0.0f, 1.0f);

    TC_LOG_INFO("ai.world", "AI needs configured interval={}ms hungerRate={:.6f} fatigueRate={:.6f} resourceRate={:.6f}",
        _needsUpdateIntervalMs, _needsRates.HungerPerSecond, _needsRates.FatiguePerSecond, _needsRates.ResourcePressurePerSecond);

    // Milestone 2.8E: default off. The only food target source for now -
    // a single fixed, config-driven world point, not per-agent or
    // nearest-anything. See FoodTargetResolver and UpdateNeeds().
    _foodTargetConfig.Enabled = sConfigMgr->GetBoolDefault("AIWorld.TestFoodTargetEnabled", false);
    _foodTargetConfig.MapId = uint32(sConfigMgr->GetIntDefault("AIWorld.TestFoodTargetMapId", 0));
    _foodTargetConfig.X = sConfigMgr->GetFloatDefault("AIWorld.TestFoodTargetX", 0.0f);
    _foodTargetConfig.Y = sConfigMgr->GetFloatDefault("AIWorld.TestFoodTargetY", 0.0f);
    _foodTargetConfig.Z = sConfigMgr->GetFloatDefault("AIWorld.TestFoodTargetZ", 0.0f);

    // Milestone 2.11B: a synthetic, config-driven day - deliberately not
    // wall-clock (the rest of AIWorld's own GameTime::GetSystemTime()
    // convention) or TrinityCore's in-game day/night cycle, both too slow
    // to make GO_TO_WORK/GO_HOME switching runtime-observable. See
    // RoutineScheduleConfig.h.
    int32 routineDayLengthMs = sConfigMgr->GetIntDefault("AIWorld.RoutineDayLengthMs", 1200000);
    if (routineDayLengthMs < 60000)
    {
        TC_LOG_WARN("ai.world", "AIWorld.RoutineDayLengthMs ({}) is invalid or too low, clamping to 60000ms", routineDayLengthMs);
        routineDayLengthMs = 60000;
    }

    int32 routineWorkStartMs = sConfigMgr->GetIntDefault("AIWorld.RoutineWorkStartMs", 400000);
    int32 routineWorkEndMs = sConfigMgr->GetIntDefault("AIWorld.RoutineWorkEndMs", 800000);
    if (routineWorkStartMs < 0 || routineWorkEndMs <= routineWorkStartMs || routineWorkEndMs > routineDayLengthMs)
    {
        TC_LOG_WARN("ai.world", "AIWorld.RoutineWorkStartMs/RoutineWorkEndMs ({}, {}) is invalid for day length {}ms, clamping to 1/3 and 2/3 of day length",
            routineWorkStartMs, routineWorkEndMs, routineDayLengthMs);

        // int64 intermediates: routineDayLengthMs * 2 would overflow int32
        // above roughly INT32_MAX / 2 - unreachable with the default
        // 1200000ms, but this path is also what a bad config value falls
        // through to, so it must stay correct at any int32 input.
        routineWorkStartMs = int32(int64(routineDayLengthMs) / 3);
        routineWorkEndMs = int32((int64(routineDayLengthMs) * 2) / 3);
    }

    _routineScheduleConfig.DayLengthMs = uint32(routineDayLengthMs);
    _routineScheduleConfig.WorkStartMs = uint32(routineWorkStartMs);
    _routineScheduleConfig.WorkEndMs = uint32(routineWorkEndMs);

    TC_LOG_INFO("ai.world", "AI routine configured dayLength={}ms workStart={}ms workEnd={}ms",
        _routineScheduleConfig.DayLengthMs, _routineScheduleConfig.WorkStartMs, _routineScheduleConfig.WorkEndMs);

    // Milestone 2.11E2: the only economy mutation that exists yet - applied
    // once per WORK ActionCompletion reaching Succeeded/Performed (see
    // UpdateNeeds()'s 2.11E1/2.11E2 activity block), never on Started
    // alone and never for REST.
    int32 workMoneyReward = sConfigMgr->GetIntDefault("AIWorld.WorkMoneyReward", 1);
    if (workMoneyReward < 0)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WorkMoneyReward ({}) is invalid, clamping to 0", workMoneyReward);
        workMoneyReward = 0;
    }
    _workMoneyReward = uint32(workMoneyReward);

    _aiClient = std::make_unique<AIClient>(ioContext, aiHost, aiPort, uint32(requestTimeoutMs), _decisionMaxInFlight);

    TC_LOG_INFO("ai.world", "AIWorld enabled");

    // Rebuild _registry from characters.ai_agents before touching anything
    // spawn-specific below - every agent this produces is Abstract with an
    // empty RuntimeGuid regardless of what it was before shutdown.
    _persistence.LoadAgents(_registry);

    // Milestone 2.12F4B2 (STATIC review): gated behind
    // AIWorld.EnableSpawnReconciliation (default false = disabled), unlike
    // LoadAgents() itself, AND - as of 2.12F4B2 - a fail-closed combination
    // with AIWorld.SpawnReconciliationZoneId. 2.12F4B's own engine already
    // measured a real, unscoped run against this server's full
    // world.creature dataset (census=128849) and observed a world-thread
    // performance regression before 2.12F4C's scale hardening exists - so
    // simply enabling AIWorld.EnableSpawnReconciliation and forgetting to
    // also set a nonzero AIWorld.SpawnReconciliationZoneId must NEVER
    // silently re-run that same unscoped population. There is deliberately
    // no unscoped/global code path left to fall through to: a global mode
    // gets its own explicit future override once 2.12F4C/2.12F4D exist
    // (see AIWorld_Current_Roadmap.md's own "2.12F4B2" section).
    bool enableSpawnReconciliation = sConfigMgr->GetBoolDefault("AIWorld.EnableSpawnReconciliation", false);
    if (enableSpawnReconciliation)
    {
        int32 spawnReconciliationZoneIdRaw = sConfigMgr->GetIntDefault("AIWorld.SpawnReconciliationZoneId", 0);
        if (spawnReconciliationZoneIdRaw <= 0)
        {
            TC_LOG_ERROR("ai.world", "AIWorld.EnableSpawnReconciliation is set but AIWorld.SpawnReconciliationZoneId ({}) is not a positive zoneId - refusing to run reconciliation (an unscoped/global run is not permitted before 2.12F4C scale hardening exists, see AIWorld_Current_Roadmap.md's own 2.12F4B2 section)",
                spawnReconciliationZoneIdRaw);
        }
        else
        {
            // Right after LoadAgents() and before anything else (group/
            // memory loaders below, the AIWorld.TestSpawnId fixture)
            // consumes _registry, so every downstream startup step sees
            // the final, reconciled state (in particular: an orphaned
            // agent's group membership/memory rows below are then
            // correctly treated as unresolvable, the same tolerance they
            // already have for any other agent missing from _registry -
            // see LoadGroupMembers()'s own comment a few lines down).
            RunSpawnReconciliation(uint32(spawnReconciliationZoneIdRaw));
        }
    }
    else
        TC_LOG_INFO("ai.world", "AI spawn reconciliation disabled (AIWorld.EnableSpawnReconciliation = 0)");

    // Milestone 2.12F4B3: gated behind AIWorld.EnableZoneControlActivation
    // (default false), fail-closed combined with AIWorld.ControlZoneId the
    // same way AIWorld.EnableSpawnReconciliation/AIWorld.SpawnReconciliationZoneId
    // above are - there is no unscoped/global promotion code path.
    // Deliberately independent of whether AIWorld.EnableSpawnReconciliation
    // just ran THIS startup: RunZoneControlActivation() recomputes its own
    // scoped census/promotion set fresh from the currently-loaded
    // _registry (already rebuilt from ai_agents by LoadAgents() above,
    // regardless of whether reconciliation ran again this run) and from
    // world.creature, so it is safe to run on a later restart against a
    // population that was reconciled in a previous one.
    bool enableZoneControlActivation = sConfigMgr->GetBoolDefault("AIWorld.EnableZoneControlActivation", false);
    if (enableZoneControlActivation)
    {
        int32 controlZoneIdRaw = sConfigMgr->GetIntDefault("AIWorld.ControlZoneId", 0);
        if (controlZoneIdRaw <= 0)
        {
            TC_LOG_ERROR("ai.world", "AIWorld.EnableZoneControlActivation is set but AIWorld.ControlZoneId ({}) is not a positive zoneId - refusing to run control activation (an unscoped/global promotion is not permitted)",
                controlZoneIdRaw);
        }
        else
            RunZoneControlActivation(uint32(controlZoneIdRaw));
    }
    else
        TC_LOG_INFO("ai.world", "AI zone control activation disabled (AIWorld.EnableZoneControlActivation = 0)");

    // Milestone 2.12E1 P2 fix (STATIC review, round 2): the persistent
    // GroupId allocator - see AgentGroupPersistence::LoadGroupIdSequence()
    // and ai_agent_group_id_sequence's own migration comment for why this
    // is a dedicated append-only table rather than MAX(group_id) over
    // ai_agent_groups. No dependency on _registry/_groupRegistry; order
    // relative to LoadGroups()/LoadGroupMembers() below does not matter.
    _groupPersistence.LoadGroupIdSequence();

    // Milestone 2.12D (STATIC review P2 fix): AgentGroup identity/state is
    // its own registry/table now (see GroupId.h/AgentGroupRegistry.h/
    // AgentGroupPersistence.h) - LoadGroups() has no dependency on
    // _registry, but LoadGroupMembers() must come after both LoadGroups()
    // (a membership row needs an already-registered group to attach to)
    // and _persistence.LoadAgents() above (a membership row's member must
    // already resolve to a registered AgentRecord - no forward-reference
    // tolerance any more, see AgentGroupMembership.h). Purely identity -
    // RunDecisionScheduler() resolves live member presence from this fresh
    // every group coarse tick, never cached here past this load.
    _groupPersistence.LoadGroups(_groupRegistry);
    _groupPersistence.LoadGroupMembers(_groupRegistry, _registry);

    // Same idea as _registry, for long-term memory: rebuilt from the DB
    // every startup. Must come after LoadAgents() - it needs _registry
    // populated to skip an orphaned row's agent_id.
    _memoryPersistence.LoadLongTermMemories(_longTermMemory, _registry);

    // Does not require the spawn's Creature/grid to be loaded - the agent
    // exists in _registry as soon as this returns, Abstract until
    // RunDecisionScheduler()/ProcessAgent() finds a live Creature for it.
    // Milestone 2.10A: no longer remembered in a dedicated _testAgentId
    // member - RunDecisionScheduler() iterates every AgentRegistry::
    // GetAgents() entry, so this one just needs to end up in _registry,
    // the same as any other agent would.
    if (testSpawnId)
    {
        // Milestone 2.12F4A P2 fix (STATIC review, round 2): promotion to
        // AIWorldControlled must not depend on whether this (map_id,
        // spawn_id) binding is being created fresh this run or already
        // existed (loaded by LoadAgents() above, possibly still
        // ObserveOnly from before this config value was ever set) -
        // "ensure binding exists, then ensure/persist AIWorldControlled,
        // then registry/runtime ownership" applies either way. The
        // previous shape only ran the promotion inside the fresh-create
        // branch, so an already-existing ObserveOnly row for
        // AIWorld.TestSpawnId would silently stay ObserveOnly forever.
        AgentRecord* existing = _registry.FindBySpawn(testMapId, testSpawnId);
        AgentId agentId = existing ? existing->Id : AgentId{};

        if (!existing)
        {
            agentId = _persistence.CreateCreatureAgent(AgentType::Guard, testMapId, testSpawnId);
            if (!agentId)
            {
                // AgentPersistence already logged why. Nothing goes into
                // _registry without a real, DB-confirmed AgentId - the
                // whole point of this milestone is that the two never
                // disagree.
                TC_LOG_ERROR("ai.world", "AI persistent agent creation failed for map={} spawn={}, test agent disabled this run",
                    testMapId, testSpawnId);
            }
        }

        if (agentId)
        {
            // Milestone 2.12F4A P2 fix (STATIC review, round 2):
            // SetControlMode() now confirms the write via read-back (see
            // its own comment) before this may treat the agent as
            // AIWorldControlled anywhere in memory - the same "DB-
            // confirmed or it didn't happen" discipline the fresh-create
            // branch above already applies to CreateCreatureAgent()'s own
            // AgentId.
            bool controlModeConfirmed = _persistence.SetControlMode(agentId, AgentControlMode::AIWorldControlled);
            if (!controlModeConfirmed)
                TC_LOG_ERROR("ai.world", "AI persistent test agent id={} map={} spawn={} ControlMode UPDATE not confirmed",
                    agentId.Value, testMapId, testSpawnId);

            if (existing)
            {
                // Fail closed: only advance an already-loaded record to
                // AIWorldControlled once confirmed. On failure it is left
                // exactly as LoadAgents() populated it - never downgraded,
                // never optimistically upgraded.
                if (controlModeConfirmed)
                    existing->ControlMode = AgentControlMode::AIWorldControlled;

                TC_LOG_INFO("ai.world", "AI persistent test agent id={} map={} spawn={} controlMode={}",
                    agentId.Value, testMapId, testSpawnId, ToString(existing->ControlMode));
            }
            else
            {
                AgentRecord record;
                record.Id = agentId;
                record.Type = AgentType::Guard;
                record.MapId = testMapId;
                record.SpawnId = testSpawnId;
                record.WorldState = AgentWorldState::Abstract;

                // Fail closed: a freshly created record only starts
                // AIWorldControlled once the UPDATE is confirmed - on
                // failure it keeps the field's own ObserveOnly default,
                // matching what the DB row itself actually holds (the
                // INSERT never sets control_mode, see CHAR_INS_AI_AGENT's
                // own comment), rather than going untracked by this
                // process for the rest of its run.
                record.ControlMode = controlModeConfirmed ? AgentControlMode::AIWorldControlled : AgentControlMode::ObserveOnly;

                if (_registry.Add(record))
                {
                    TC_LOG_INFO("ai.world", "AI persistent agent created id={} type={} map={} spawn={} controlMode={}",
                        agentId.Value, ToString(AgentType::Guard), testMapId, testSpawnId, ToString(record.ControlMode));
                }
            }
        }
    }

    // Milestone 2.12E4R test hook: runs first among this Initialize()'s own
    // manual test actions - see RunTestDissolveGroup()'s own comment for
    // why (clearing a specific pre-existing group out of the way before
    // AIWorld.WolfGroupAutoFormation's own timer gets a chance to run is
    // the whole point of this hook). Off by default (AIWorld.TestDissolveGroupId
    // = 0).
    if (testDissolveGroupId)
        RunTestDissolveGroup(testDissolveGroupId);

    // Milestone 2.12F3 test hook: an existing GroupId to Manually dissolve
    // once its own REGROUP is actually observed in flight - see
    // CheckTestDissolveOnActiveRegroup()'s own comment for why
    // AIWorld.TestDissolveGroupId itself (a startup-only, Initialize()-time
    // hook, immediately above) cannot prove this: it fires before Update()
    // has ever run RunCoalitionCoordination() even once, long before any
    // REGROUP could possibly be in flight yet. Read/validated here,
    // deliberately after LoadGroups()/LoadGroupMembers() above (not
    // alongside the rest of Initialize()'s own coordination config, which
    // runs before groups are even loaded) so the existence check below has
    // something to check against.
    //
    // Milestone 2.12F3 P3 fix (STATIC review): fail-closed parsing, not a
    // bare cast - sConfigMgr->GetIntDefault() returns a signed int32, and
    // GroupId::Value is unsigned (see GroupId.h); an earlier version cast
    // straight through, so a negative/misconfigured value (e.g. -1) would
    // silently become some enormous, never-real GroupId instead of being
    // rejected, leaving the hook enabled but permanently unable to ever
    // find what it is waiting for.
    int32 testDissolveOnActiveRegroupGroupIdRaw = sConfigMgr->GetIntDefault("AIWorld.TestDissolveOnActiveRegroupGroupId", 0);
    if (testDissolveOnActiveRegroupGroupIdRaw < 0)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.TestDissolveOnActiveRegroupGroupId ({}) is negative, which cannot name a real GroupId, disabling this test hook",
            testDissolveOnActiveRegroupGroupIdRaw);
        testDissolveOnActiveRegroupGroupIdRaw = 0;
    }
    _testDissolveOnActiveRegroupGroupId = GroupId{ uint64(testDissolveOnActiveRegroupGroupIdRaw) };

    // _testDissolveOnActiveRegroupFired doubles as this hook's own enabled/
    // disabled latch, not just "already fired successfully" - reset false
    // here (every Initialize(), so a runtime reload never leaves a stale
    // latch from a previous run behind), then immediately set true below
    // if the configured GroupId does not resolve, so
    // CheckTestDissolveOnActiveRegroup() never has to discover that for
    // itself by polling forever.
    _testDissolveOnActiveRegroupFired = false;

    if (_testDissolveOnActiveRegroupGroupId)
    {
        // Milestone 2.12F3 P3 fix (STATIC review): validated once, right
        // now, against the registry LoadGroups()/LoadGroupMembers() above
        // just populated - a configured GroupId that does not exist would
        // otherwise leave CheckTestDissolveOnActiveRegroup() silently
        // polling it, once per coordination pass, forever. LoadGroups()
        // never adds anything after this point in Initialize(), so "does
        // not exist now" means "will never exist this process's lifetime"
        // for this specific config value - disabling outright (the same
        // fired=true latch a completed one-shot ends in) is correct, not
        // merely a shortcut.
        if (!_groupRegistry.Find(_testDissolveOnActiveRegroupGroupId))
        {
            TC_LOG_ERROR("ai.world", "AIWorld.TestDissolveOnActiveRegroupGroupId={} does not resolve to a registered group, disabling this test hook",
                _testDissolveOnActiveRegroupGroupId.Value);
            _testDissolveOnActiveRegroupFired = true;
        }
        else
        {
            TC_LOG_INFO("ai.world", "AI test dissolve-on-active-regroup: watching group={} for an active REGROUP (AIWorld.TestDissolveOnActiveRegroupGroupId)",
                _testDissolveOnActiveRegroupGroupId.Value);
        }
    }

    // Milestone 2.12G2R: the ROAM lifecycle runtime closure - three
    // independent one-shot test hooks, same fail-closed parsing/existence-
    // check shape as AIWorld.TestDissolveOnActiveRegroupGroupId above, just
    // an AgentId for the two agent-scoped hooks (preempt/leave) and a
    // GroupId for the group-scoped one (dissolve). See CheckTestPreemptOnActiveRoam()/
    // CheckTestLeaveOnActiveRoam()/CheckTestDissolveOnActiveRoam()'s own
    // comments for what each actually does once triggered.
    int32 testPreemptOnActiveRoamAgentIdRaw = sConfigMgr->GetIntDefault("AIWorld.TestPreemptOnActiveRoamAgentId", 0);
    if (testPreemptOnActiveRoamAgentIdRaw < 0)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.TestPreemptOnActiveRoamAgentId ({}) is negative, which cannot name a real AgentId, disabling this test hook",
            testPreemptOnActiveRoamAgentIdRaw);
        testPreemptOnActiveRoamAgentIdRaw = 0;
    }
    _testPreemptOnActiveRoamAgentId = AgentId{ uint64(testPreemptOnActiveRoamAgentIdRaw) };
    _testPreemptOnActiveRoamFired = false;
    // 2.12G2R P2 fix (STATIC review): a runtime reload must never resume
    // a stale in-progress TRIGGER/VERIFY cycle from a previous run.
    _testPreemptOnActiveRoamTriggered = false;
    _testPreemptOnActiveRoamCapturedGroup = GroupId{};
    _testPreemptOnActiveRoamCapturedStartedAtMs = 0;
    _testPreemptOnActiveRoamVerifyDeadlineMs = 0;

    if (_testPreemptOnActiveRoamAgentId)
    {
        if (!_registry.Find(_testPreemptOnActiveRoamAgentId))
        {
            TC_LOG_ERROR("ai.world", "AIWorld.TestPreemptOnActiveRoamAgentId={} does not resolve to a registered agent, disabling this test hook",
                _testPreemptOnActiveRoamAgentId.Value);
            _testPreemptOnActiveRoamFired = true;
        }
        else
        {
            TC_LOG_INFO("ai.world", "AI test preempt-on-active-roam: watching agent={} for an active ROAM (AIWorld.TestPreemptOnActiveRoamAgentId)",
                _testPreemptOnActiveRoamAgentId.Value);
        }
    }

    int32 testLeaveOnActiveRoamAgentIdRaw = sConfigMgr->GetIntDefault("AIWorld.TestLeaveOnActiveRoamAgentId", 0);
    if (testLeaveOnActiveRoamAgentIdRaw < 0)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.TestLeaveOnActiveRoamAgentId ({}) is negative, which cannot name a real AgentId, disabling this test hook",
            testLeaveOnActiveRoamAgentIdRaw);
        testLeaveOnActiveRoamAgentIdRaw = 0;
    }
    _testLeaveOnActiveRoamAgentId = AgentId{ uint64(testLeaveOnActiveRoamAgentIdRaw) };
    _testLeaveOnActiveRoamFired = false;
    // 2.12G2R P2 fix (STATIC review): same reasoning as
    // _testPreemptOnActiveRoamTriggered's own reset above.
    _testLeaveOnActiveRoamTriggered = false;
    _testLeaveOnActiveRoamCapturedGroup = GroupId{};
    _testLeaveOnActiveRoamCapturedStartedAtMs = 0;
    _testLeaveOnActiveRoamVerifyDeadlineMs = 0;

    if (_testLeaveOnActiveRoamAgentId)
    {
        if (!_registry.Find(_testLeaveOnActiveRoamAgentId))
        {
            TC_LOG_ERROR("ai.world", "AIWorld.TestLeaveOnActiveRoamAgentId={} does not resolve to a registered agent, disabling this test hook",
                _testLeaveOnActiveRoamAgentId.Value);
            _testLeaveOnActiveRoamFired = true;
        }
        else
        {
            TC_LOG_INFO("ai.world", "AI test leave-on-active-roam: watching agent={} for an active ROAM (AIWorld.TestLeaveOnActiveRoamAgentId)",
                _testLeaveOnActiveRoamAgentId.Value);
        }
    }

    int32 testDissolveOnActiveRoamGroupIdRaw = sConfigMgr->GetIntDefault("AIWorld.TestDissolveOnActiveRoamGroupId", 0);
    if (testDissolveOnActiveRoamGroupIdRaw < 0)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.TestDissolveOnActiveRoamGroupId ({}) is negative, which cannot name a real GroupId, disabling this test hook",
            testDissolveOnActiveRoamGroupIdRaw);
        testDissolveOnActiveRoamGroupIdRaw = 0;
    }
    _testDissolveOnActiveRoamGroupId = GroupId{ uint64(testDissolveOnActiveRoamGroupIdRaw) };
    _testDissolveOnActiveRoamFired = false;
    // 2.12G2R P2 fix (STATIC review): same reasoning as
    // _testPreemptOnActiveRoamTriggered's own reset above.
    _testDissolveOnActiveRoamTriggered = false;
    _testDissolveOnActiveRoamCapturedMembers.clear();
    _testDissolveOnActiveRoamVerifyDeadlineMs = 0;

    if (_testDissolveOnActiveRoamGroupId)
    {
        if (!_groupRegistry.Find(_testDissolveOnActiveRoamGroupId))
        {
            TC_LOG_ERROR("ai.world", "AIWorld.TestDissolveOnActiveRoamGroupId={} does not resolve to a registered group, disabling this test hook",
                _testDissolveOnActiveRoamGroupId.Value);
            _testDissolveOnActiveRoamFired = true;
        }
        else
        {
            TC_LOG_INFO("ai.world", "AI test dissolve-on-active-roam: watching group={} for an active ROAM (AIWorld.TestDissolveOnActiveRoamGroupId)",
                _testDissolveOnActiveRoamGroupId.Value);
        }
    }

    // Milestone 2.12G3D1: same fail-closed parsing/existence-check shape
    // as the active-Roam hooks above - see CheckTestObserveActiveHunt()'s
    // own comment for what this purely-observational hook actually does
    // once triggered (nothing changes/starts; it only logs).
    int32 testObserveActiveHuntAgentIdRaw = sConfigMgr->GetIntDefault("AIWorld.TestObserveActiveHuntAgentId", 0);
    if (testObserveActiveHuntAgentIdRaw < 0)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.TestObserveActiveHuntAgentId ({}) is negative, which cannot name a real AgentId, disabling this test hook",
            testObserveActiveHuntAgentIdRaw);
        testObserveActiveHuntAgentIdRaw = 0;
    }
    _testObserveActiveHuntAgentId = AgentId{ uint64(testObserveActiveHuntAgentIdRaw) };
    _testObserveActiveHuntFired = false;

    if (_testObserveActiveHuntAgentId)
    {
        if (!_registry.Find(_testObserveActiveHuntAgentId))
        {
            TC_LOG_ERROR("ai.world", "AIWorld.TestObserveActiveHuntAgentId={} does not resolve to a registered agent, disabling this test hook",
                _testObserveActiveHuntAgentId.Value);
            _testObserveActiveHuntFired = true;
        }
        else
        {
            TC_LOG_INFO("ai.world", "AI test observe-active-hunt: watching agent={} for a fully-owned in-flight HUNT approach (AIWorld.TestObserveActiveHuntAgentId)",
                _testObserveActiveHuntAgentId.Value);
        }
    }

    // Milestone 2.12E4C2 P2 fix (STATIC review): runs right after the
    // dissolve hook above, for the same reason - a one-shot corrective
    // action that must run before AIWorld.WolfGroupAutoFormation's/
    // AIWorld.CoalitionMaintenance's own timers get a chance to observe
    // (or fail to observe) the group's provenance. Off by default
    // (AIWorld.AdoptGroupId = 0).
    if (adoptGroupId)
        RunGroupProfileAdoption(adoptGroupId, adoptGroupProfileId);

    // Milestone 2.12E1 P2 fix (STATIC review, round 2): manual proof only,
    // and only runs once all three configured member AgentIds are set -
    // see RunGroupLifecycleSmokeTest()'s own comment for why it no longer
    // has an "off by default" flag of its own: three unset (default 0)
    // AgentIds already means "disabled", the same as AIWorld.TestSpawnId's
    // own 0-means-disabled convention.
    if (testGroupMemberAgentId1 && testGroupMemberAgentId2 && testGroupMemberAgentId3)
        RunGroupLifecycleSmokeTest(testGroupMemberAgentId1, testGroupMemberAgentId2, testGroupMemberAgentId3);

    // Milestone 2.12E3B: manual proof only, gated behind AIWorld.TestGroupPolicy
    // (default off) - see RunGroupPolicySmokeTest()'s own comment. Its pure
    // half runs regardless of testGroupMemberAgentId1; its integration half
    // (a real Stable test group) only runs if that resolves in _registry,
    // which RunGroupPolicySmokeTest() itself checks.
    if (testGroupPolicy)
        RunGroupPolicySmokeTest(testGroupMemberAgentId1);

    // Milestone 2.12E4C1: manual proof only, gated behind
    // AIWorld.TestCoalitionMaintenance (default off) - see
    // RunCoalitionMaintenanceSmokeTest()'s own comment. Entirely pure, no
    // integration half at all yet (2.12E4C1 deliberately adds no lifecycle
    // orchestration).
    if (testCoalitionMaintenance)
        RunCoalitionMaintenanceSmokeTest();

    // Milestone 2.12F1: manual proof only, gated behind
    // AIWorld.TestGroupIntent (default off) - see RunGroupIntentSmokeTest()'s
    // own comment. Entirely pure, no AgentGroupIntentProjector/ActionSystem
    // integration at all yet (2.12F1 deliberately adds none).
    if (testGroupIntent)
        RunGroupIntentSmokeTest();

    // Milestone 2.12F2: manual proof only, gated behind
    // AIWorld.TestGroupIntentProjector (default off) - see
    // RunGroupIntentProjectorSmokeTest()'s own comment. Entirely pure, no
    // ActionSystem/orchestration integration at all yet (this milestone's
    // own pure-layer commit deliberately adds none).
    if (testGroupIntentProjector)
        RunGroupIntentProjectorSmokeTest();

    // Milestone 2.12G3B: manual proof only, gated behind
    // AIWorld.TestHuntIntent (default off) - see RunHuntIntentSmokeTest()'s
    // own comment. Entirely pure, no GoalType::Hunt/ActionSystem/
    // RunCoalitionCoordination() integration at all yet (this milestone's
    // own pure-layer commit deliberately adds none).
    if (testHuntIntent)
        RunHuntIntentSmokeTest();

    // Milestone 2.12G3C1: manual proof only, gated behind
    // AIWorld.TestHuntActionValidation (default off) - see
    // RunHuntActionValidationSmokeTest()'s own comment. Entirely pure, no
    // RunCoalitionCoordination()/DispatchHuntProposal() integration at all
    // yet (this milestone's own contract commit deliberately adds none).
    if (testHuntActionValidation)
        RunHuntActionValidationSmokeTest();

    // Milestone 2.12G3D1: manual proof only, gated behind
    // AIWorld.TestHuntArrivalOwnership (default off) - see
    // RunHuntArrivalOwnershipSmokeTest()'s own comment. Entirely pure -
    // ActionCompletion/AgentRecord are both plain values, no live
    // Creature/Map/registry access needed to exercise
    // HandleActionCompletion() directly.
    if (testHuntArrivalOwnership)
        RunHuntArrivalOwnershipSmokeTest();

    // Milestone 2.12F4A: manual proof only, gated behind
    // AIWorld.TestControlMode (default 0/disabled) - see
    // RunControlModeSmokeTest()'s own comment. Runs against a real,
    // already-registered AgentId (never a fake/ghost record) purely
    // against ActionSystem::Validate() itself - no live Creature/Map
    // needed, no physical movement attempted.
    if (testControlModeAgentId)
        RunControlModeSmokeTest(testControlModeAgentId);

    TC_LOG_INFO("ai.world", "AI bridge target {}:{} (timeout={}ms, health interval={}ms, max in-flight decisions={}, "
        "scheduler interval={}ms, nearby interval={}ms, active interval={}ms, nearby player range={:.1f}, "
        "background interval={}ms, group interval={}ms, coarse max/pass={})",
        aiHost, aiPort, requestTimeoutMs, _healthIntervalMs, _decisionMaxInFlight,
        _decisionSchedulerIntervalMs, _decisionNearbyIntervalMs, _decisionActiveIntervalMs, _decisionNearbyPlayerRange,
        _backgroundSimulationIntervalMs, _groupSimulationIntervalMs, _coarseSimulationMaxPerPass);

    // Last step: only from here on can PublishWorldEvent() actually enqueue
    // anything. Ordered after everything above so a map worker can't race
    // a WorldEvent into _eventBus before _registry/_aiClient exist.
    _acceptEvents.store(true, std::memory_order_release);
}

void AIWorldMgr::RequestDissolveGroup(GroupId groupId, std::function<void(bool)> onComplete)
{
    // 2.12F2 P2 fix (STATIC review): captured NOW, before the dissolve is
    // even submitted - AgentGroupLifecycleSystem::_pendingGroupOperations
    // guarantees no Join/Leave for this same groupId can land between this
    // capture and the completion below (this call is about to mark groupId
    // pending itself, synchronously, before returning), and the completion
    // itself runs only after AgentGroupRegistry has already erased this
    // GroupId's own AgentGroupRecord - there is no later moment membership
    // could instead be read fresh from. See StopGroupCoordinationForMember()'s
    // own comment for why every former member needs to be checked here.
    std::vector<AgentId> formerMembers;
    if (AgentGroupRecord const* group = _groupRegistry.Find(groupId))
    {
        formerMembers.reserve(group->Members.size());
        for (AgentGroupMembership const& membership : group->Members)
            formerMembers.push_back(membership.Member);
    }

    _groupLifecycleSystem.RequestDissolveGroup(groupId, _groupRegistry, _groupPersistence, _groupLifecyclePending,
        [this, groupId, formerMembers = std::move(formerMembers), onComplete = std::move(onComplete)](bool success)
        {
            // 2.12E2 hardening: _groupSimulationSchedule is AIWorldMgr-only
            // scheduling bookkeeping (see its own declaration comment) -
            // AgentGroupLifecycleSystem has no access to it and no business
            // knowing it exists. Erased only on a confirmed dissolve, the
            // same "mutate only after confirmation" discipline everything
            // else here follows; a failed dissolve leaves it alone, since
            // the group (and therefore its schedule entry) still exists.
            //
            // Milestone 2.12E4C2 P3 fix (STATIC review): _maintenanceSchedule
            // is the exact same class of AIWorldMgr-only bookkeeping,
            // GroupId-keyed, and was NOT being cleared here - since GroupIds
            // are never recycled (see GroupId.h), a long-running world with
            // many dynamic create/dissolve cycles would grow this map
            // without bound, keyed by historical group count rather than
            // current group count. This is THE one authoritative confirmed-
            // dissolve completion every dissolve path funnels through
            // (Manual, AutomaticPolicy, formation-abort cleanup, maintenance
            // dissolve) - fixing it here, not only in
            // RunCoalitionMaintenanceDissolveGroup()'s own completion, is
            // what makes cleanup unconditional on every successful dissolve,
            // not just one triggered by maintenance itself.
            // _maintenanceInFlight/_groupProfileAdoptionInFlight are cleared
            // here too, purely as defense in depth - each already clears
            // its own entry from inside its own completion, but a group
            // dissolved through some other path while either was
            // (improbably) still marked in-flight for it should not leave
            // a permanently-stuck entry behind.
            if (success)
            {
                _groupSimulationSchedule.erase(groupId.Value);
                _maintenanceSchedule.erase(groupId.Value);
                _maintenanceInFlight.erase(groupId.Value);
                _groupProfileAdoptionInFlight.erase(groupId.Value);

                // Milestone 2.12G2 P2 fix, round 2 (STATIC review): same
                // class of AIWorldMgr-only, GroupId-keyed bookkeeping as
                // the maps above - _roamAttemptPinnedNowMs records a pin
                // the moment a ROAM intent is produced (RunCoalitionCoordination()),
                // but only ever CLEARS it when that same GroupId is later
                // re-discovered with no in-flight attempt. A dissolved
                // GroupId is removed from _groupRegistry entirely
                // (AgentGroupRegistry::Remove()), so AgentGroupRegistry::
                // GetGroupsAfterUntil() can never discover it again -
                // RunCoalitionCoordination() would never revisit this
                // GroupId to do that clearing itself. Without this, a
                // dissolved-while-still-pinned group's entry would linger
                // in this map forever (GroupIds are never recycled, see
                // GroupId.h), growing unbounded over a long-running
                // server's own churn of created/dissolved groups. This is
                // the one authoritative confirmed-dissolve completion
                // every dissolve path funnels through (see this method's
                // own comment above), so clearing it only here is enough.
                _roamAttemptPinnedNowMs.erase(groupId.Value);

                // 2.12F2 P2 fix (STATIC review): every former member's own
                // in-flight Regroup (if any) targeting this now-dissolved
                // group must be stopped, not left to run to its own natural
                // conclusion toward a group that no longer exists - see
                // StopGroupCoordinationForMember()'s own comment.
                for (AgentId memberId : formerMembers)
                    StopGroupCoordinationForMember(memberId, groupId);
            }

            onComplete(success);
        });
}

void AIWorldMgr::RequestDissolveGroupWithPolicy(GroupId groupId, AgentGroupOperationSource source, std::function<void(bool)> onComplete)
{
    // Manual is always allowed, the same rule CanLeave() already gives it -
    // no policy question to ask, straight to the raw dissolve path.
    if (source == AgentGroupOperationSource::Manual)
    {
        RequestDissolveGroup(groupId, std::move(onComplete));
        return;
    }

    AgentGroupRecord const* group = _groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_WARN("ai.world", "AIWorldMgr::RequestDissolveGroupWithPolicy: group id={} does not exist, nothing to do", groupId.Value);
        onComplete(false);
        return;
    }

    AgentGroupPolicyContext context;
    context.Config = _groupPolicyConfig;
    context.Source = source;

    if (!_groupPolicySystem.ShouldDissolve(*group, context))
    {
        // ShouldDissolve() already encodes both halves of the rule this
        // needs (Stable -> always false; Loose -> only below
        // LooseGroupMinMembers) - this only picks the right log reason,
        // it does not re-derive the decision itself.
        char const* reason = group->Kind == AgentGroupKind::Stable ? "STABLE_GROUP_PROTECTED" : "GROUP_NOT_BELOW_MINIMUM";
        TC_LOG_INFO("ai.world", "AI agent group dissolve REJECTED group={} source={} reason={}", groupId.Value, ToString(source), reason);
        onComplete(false);
        return;
    }

    RequestDissolveGroup(groupId, std::move(onComplete));
}

// Milestone 2.12E4R test hook: called at most once, from Initialize(),
// only when AIWorld.TestDissolveGroupId is set (non-zero) - a one-shot way
// for an operator to clear a specific, already-existing AgentGroup out of
// the way before anything else in this same Initialize() run (in
// particular AIWorld.WolfGroupAutoFormation's own timer, which only starts
// ticking once Update() is first called, strictly after Initialize()
// returns) can observe it. Exists purely to make a clean "fresh formation"
// regression repeatable: dissolve a leftover group from a previous run,
// then let RunCoalitionFormation() prove it can form a brand new one for
// the same now-ungrouped members.
//
// Goes through RequestDissolveGroupWithPolicy(..., Manual) - the exact
// same policy-gated entry point any other deliberately-authorized
// individual dissolve request already uses (Manual is always allowed for
// either AgentGroupKind, the same rule CanLeave()/ShouldDissolve() already
// give it - see AgentGroupPolicySystem.h) - never AgentGroupRegistry::
// Remove() or a raw DELETE against ai_agent_groups/ai_agent_group_members.
// An unknown groupId just fails and logs; there is nothing to retry, and
// nothing else in this run depends on this succeeding.
void AIWorldMgr::RunTestDissolveGroup(GroupId groupId)
{
    TC_LOG_INFO("ai.world", "AI test dissolve: requesting Manual dissolve of group={} (AIWorld.TestDissolveGroupId)", groupId.Value);

    RequestDissolveGroupWithPolicy(groupId, AgentGroupOperationSource::Manual,
        [groupId](bool success)
        {
            if (success)
                TC_LOG_INFO("ai.world", "AI test dissolve PASSED: group={} dissolved", groupId.Value);
            else
                TC_LOG_ERROR("ai.world", "AI test dissolve FAILED: group={} could not be dissolved (does it exist?)", groupId.Value);
        });
}

// Milestone 2.12G2R: extracted from CheckTestDissolveOnActiveRegroup()'s
// own inline loop body (2.12F3 P3 fix, round 2's six-part fail-closed
// ownership-tuple proof, unchanged in substance) so the three active-Roam
// test hooks below can share the exact same proof, generalized only over
// which GoalType and (optionally) which specific SourceGroup to require -
// see this method's own declaration comment in AIWorldMgr.h for the full
// reasoning. Behavior for the Regroup call site below is byte-identical
// to before this refactor.
bool AIWorldMgr::HasActiveCoordinationMoveTo(AgentRecord const& record, GoalType goalType, GroupId requiredSourceGroup) const
{
    if (!record.ActiveActionState || record.ActiveActionState->Type != ActionType::MoveTo
        || record.ActiveActionState->SourceGoal != goalType)
        return false;

    if (!record.GroupCoordinationGoalState || record.GroupCoordinationGoalState->Type != goalType)
        return false;

    if (requiredSourceGroup && record.GroupCoordinationGoalState->SourceGroup != requiredSourceGroup)
        return false;

    if (record.GroupCoordinationGoalState->StartedAtMs != record.ActiveActionState->GoalStartedAtMs)
        return false;

    if (record.WorldState != AgentWorldState::Materialized)
        return false;

    Map* map = sMapMgr->FindBaseNonInstanceMap(record.MapId);
    Creature* creature = ResolveLiveCreature(record, map);
    if (!creature || !HasOwnMoveToGenerator(*creature))
        return false;

    return true;
}

std::optional<AIWorldMgr::TestHuntAttemptIdentity> AIWorldMgr::CaptureActiveHuntAttempt(AgentRecord const& record) const
{
    // Reuses the exact same six-part ownership-tuple proof
    // HasActiveCoordinationMoveTo() already established for Regroup/Roam -
    // generalized here, not duplicated - then extended below with HUNT's
    // own additional target-identity requirements. GroupId{} accepts any
    // currently-owning group; this hook watches one specific AgentId, not
    // one specific GroupId.
    if (!HasActiveCoordinationMoveTo(record, GoalType::Hunt))
        return std::nullopt;

    if (!record.ActiveActionState->Target)
        return std::nullopt;

    // ActiveAction and GroupCoordinationGoal must agree on WHICH target
    // this attempt is for, not merely on goal/group/timestamp.
    if (record.ActiveActionState->Target->Guid != record.GroupCoordinationGoalState->TargetGuid ||
        record.ActiveActionState->Target->Entry != record.GroupCoordinationGoalState->TargetEntry)
        return std::nullopt;

    // The same GUID/entry-binding discipline ActionSystem::
    // ValidateHuntTarget() already enforces at dispatch time, re-proven
    // here rather than trusted to still hold.
    if (!record.ActiveActionState->Target->Guid.IsCreature())
        return std::nullopt;

    if (record.ActiveActionState->Target->Guid.GetEntry() != record.ActiveActionState->Target->Entry)
        return std::nullopt;

    // Never trusts that SourceGroup/membership still hold by the time this
    // specific poll runs.
    GroupId sourceGroup = record.GroupCoordinationGoalState->SourceGroup;
    AgentGroupRecord const* group = _groupRegistry.Find(sourceGroup);
    if (!group)
        return std::nullopt;

    bool stillMember = std::any_of(group->Members.begin(), group->Members.end(),
        [&record](AgentGroupMembership const& membership) { return membership.Member == record.Id; });
    if (!stillMember)
        return std::nullopt;

    // Transient, already-loaded-object lookup only - never a force-load,
    // never stored past this call. HasActiveCoordinationMoveTo() already
    // proved the MEMBER's own live Creature resolves; this re-resolves it
    // fresh rather than threading a pointer through from that call.
    // 2.12G3D1 P2 fix (STATIC review): ResolveLiveCreature() only looks up
    // a pointer by spawn - it does not check liveness. IsAlive() must be
    // checked explicitly here to actually satisfy the declared
    // "materialized/alive actor" fail-closed requirement.
    Map* map = sMapMgr->FindBaseNonInstanceMap(record.MapId);
    Creature* creature = ResolveLiveCreature(record, map);
    if (!creature || !creature->IsAlive())
        return std::nullopt;

    Creature* target = ObjectAccessor::GetCreature(*creature, record.ActiveActionState->Target->Guid);
    if (!target || !target->IsAlive())
        return std::nullopt;

    if (target->GetMapId() != creature->GetMapId())
        return std::nullopt;

    if (target->GetEntry() != record.ActiveActionState->Target->Entry)
        return std::nullopt;

    if (!creature->IsValidAttackTarget(target))
        return std::nullopt;

    TestHuntAttemptIdentity identity;
    identity.Member = record.Id;
    identity.SourceGroup = sourceGroup;
    identity.StartedAtMs = record.ActiveActionState->GoalStartedAtMs;
    identity.Target = *record.ActiveActionState->Target;
    identity.TargetObservedAtMs = record.GroupCoordinationGoalState->TargetObservedAtMs;
    return identity;
}

// Milestone 2.12G2R P2 fix, round 2 (STATIC review): see
// CoordinationStopVerification's own declaration comment in AIWorldMgr.h
// for why this reports four independent facts rather than one combined
// bool.
AIWorldMgr::CoordinationStopVerification AIWorldMgr::VerifyCoordinationStop(AgentRecord const& record, CoordinationStopReason expectedReason,
    GoalType goalType, GroupId sourceGroup, uint64 startedAtMs) const
{
    CoordinationStopVerification result;

    result.StopEventMatches = record.LastCoordinationStop
        && record.LastCoordinationStop->Reason == expectedReason
        && record.LastCoordinationStop->SourceGoal == goalType
        && record.LastCoordinationStop->SourceGroup == sourceGroup
        && record.LastCoordinationStop->StartedAtMs == startedAtMs;

    result.GroupCoordinationGoalGone = !record.GroupCoordinationGoalState
        || record.GroupCoordinationGoalState->Type != goalType
        || record.GroupCoordinationGoalState->SourceGroup != sourceGroup
        || record.GroupCoordinationGoalState->StartedAtMs != startedAtMs;

    result.ActiveActionStateGone = !record.ActiveActionState
        || record.ActiveActionState->SourceGoal != goalType
        || record.ActiveActionState->GoalStartedAtMs != startedAtMs;

    // Milestone 2.12G2R P2 fix, round 3 (STATIC review): BOTH halves
    // required, not just the "after" one - see CoordinationStopEvent.h's
    // own comment for why a stop that found nothing running in the first
    // place is not genuine evidence of a production stop interrupting a
    // live movement.
    result.EngineGeneratorGone = result.StopEventMatches
        && record.LastCoordinationStop->EngineGeneratorWasRunningBeforeStop
        && record.LastCoordinationStop->EngineGeneratorConfirmedStoppedAfterStop;

    // Milestone 2.12G2R P2 fix, round 3 (STATIC review): forwarded
    // straight from the matching event itself - see
    // CoordinationPreemptingOwner's own comment for why this must come
    // from the event, captured synchronously at the stop moment, not
    // re-derived by a caller from AgentRecord's own CURRENT state.
    if (result.StopEventMatches)
    {
        result.PreemptingOwner = record.LastCoordinationStop->PreemptingOwner;
        result.PreemptingGoal = record.LastCoordinationStop->PreemptingGoal;
    }

    return result;
}

// Milestone 2.12F3 test hook: called from Update(), only right after a
// RunCoalitionCoordination() pass actually runs (2.12F3 P3 fix, round 2,
// STATIC review - an earlier version polled every single world tick
// regardless; a Regroup can only ever be freshly DISPATCHED from inside
// RunCoalitionCoordination() itself, via DispatchGroupMemberActionProposal(),
// so there is nothing to find in between two passes that the previous
// pass's own check did not already see - checking here ties this hook's
// entire cost to the same AIWorld.GroupCoordinationIntervalMs cadence
// RunCoalitionCoordination() already runs on, instead of every tick
// forever for as long as the condition stays unmet), only while
// AIWorld.TestDissolveOnActiveRegroupGroupId is set (non-zero) and has not
// fired yet. The runtime-proof counterpart to RunTestDissolveGroup()'s own
// startup-only hook, which cannot exercise this specific race at all: it
// runs from Initialize(), before Update() has ever called
// RunCoalitionCoordination() even once, so no REGROUP could possibly be in
// flight yet when it fires.
//
// Milestone 2.12F3 P3 fix, round 2 (STATIC review): the trigger condition
// is now the full provenance tuple, fail-closed, not just SourceGoal/
// SourceGroup - an earlier version accepted ActiveActionState->SourceGoal
// == Regroup + GroupCoordinationGoalState->SourceGroup == groupId alone,
// which does not actually prove a Regroup MOVE_TO is really in flight:
// ActiveActionState is deliberately allowed to sit briefly stale if an
// engine event has not landed yet (see HasOwnMoveToGenerator()'s own
// call site in UpdateNeeds(), which exists specifically to reconcile
// that), and neither ActiveActionState::Type nor
// GroupCoordinationGoalState::Type nor the two attempts' own
// GoalStartedAtMs/StartedAtMs identity were checked at all - the same
// three-part ownership proof ProcessActionEngineEvent()'s own
// ownedByCurrentAttempt discrimination already requires for Regroup.
// Now requires, in order: ActiveActionState::Type == MoveTo,
// ActiveActionState::SourceGoal == Regroup, GroupCoordinationGoalState::Type
// == Regroup, GroupCoordinationGoalState::SourceGroup == the configured
// group, the two attempts' own identity matching
// (GroupCoordinationGoalState::StartedAtMs == ActiveActionState::
// GoalStartedAtMs), the member actually Materialized with a live,
// transiently-resolved Creature (ResolveLiveCreature() - the same no-
// force-load lookup every other AIWorld call site uses, never stored past
// this call), and HasOwnMoveToGenerator() actually true on it - the
// engine's own live confirmation that a MOVE_TO this AIWorld's own point
// generator owns is really running right now, not merely that AgentRecord
// bookkeeping still claims one.
//
// Once triggered, requests a Manual dissolve through the same
// RequestDissolveGroupWithPolicy() entry point RunTestDissolveGroup()
// itself already uses - never AgentGroupRegistry::Remove(), never a raw
// StopMoveTo() called directly from this hook, never a raw DELETE against
// ai_agent_groups/ai_agent_group_members. What stops the in-flight
// movement (if the dissolve is confirmed) is AIWorldMgr::
// RequestDissolveGroup()'s own existing StopGroupCoordinationForMember()
// call, per former member - the exact same production path a real
// dissolve-while-regrouping would go through, proving that path end to
// end rather than a special-cased test-only shortcut.
//
// Exists to make the 2.12F2 P2 fix (STATIC review) - a confirmed Leave/
// Dissolve stops any in-flight Regroup that named the now-gone group,
// rather than letting it run to a stale territory point - runtime-provable
// under AIWorld.GroupCoordination = 1, not just statically reviewable.
//
// _testDissolveOnActiveRegroupFired is set BEFORE the dissolve request is
// even submitted, unconditionally, regardless of what RequestDissolveGroupWithPolicy()
// itself eventually reports - a one-shot test hook fires at most once ever,
// the same guarantee every other AIWorld.Test* hook in this file gives,
// not something worth retrying automatically if this particular attempt
// happens to fail.
void AIWorldMgr::CheckTestDissolveOnActiveRegroup()
{
    AgentGroupRecord const* group = _groupRegistry.Find(_testDissolveOnActiveRegroupGroupId);
    if (!group)
    {
        // 2.12F3 P3 fix, round 3 (STATIC review): the group can legitimately
        // dissolve through the normal lifecycle path before this hook ever
        // observes an active REGROUP (Initialize()'s own existence check
        // only ever catches a target that is already gone at startup - it
        // cannot see one that dissolves later). Without this, a vanished
        // target would leave _testDissolveOnActiveRegroupFired false
        // forever, so Update() keeps calling this once per coordination
        // pass indefinitely - only an O(1) registry lookup once the group
        // is gone, but still unbounded recurring work, and it contradicts
        // this hook's own "a target that cannot resolve gets disabled, not
        // polled forever" contract (see Initialize()'s own existence-check
        // comment). Logged once, then disabled the same way that startup
        // check disables it - this hook never gets a second chance to
        // observe a group that no longer exists.
        TC_LOG_ERROR("ai.world", "AIWorld.TestDissolveOnActiveRegroupGroupId={} no longer resolves to a registered group, disabling this test hook",
            _testDissolveOnActiveRegroupGroupId.Value);
        _testDissolveOnActiveRegroupFired = true;
        return;
    }

    bool anyMemberRegrouping = false;
    AgentId regroupingMember;

    for (AgentGroupMembership const& membership : group->Members)
    {
        AgentRecord const* record = _registry.Find(membership.Member);
        if (!record)
            continue;

        if (!HasActiveCoordinationMoveTo(*record, GoalType::Regroup, _testDissolveOnActiveRegroupGroupId))
            continue;

        anyMemberRegrouping = true;
        regroupingMember = membership.Member;
        break;
    }

    if (!anyMemberRegrouping)
        return;

    _testDissolveOnActiveRegroupFired = true;

    GroupId groupId = _testDissolveOnActiveRegroupGroupId;

    TC_LOG_INFO("ai.world", "AI test dissolve-on-active-regroup: group={} member={} has an active REGROUP in flight, requesting Manual dissolve (AIWorld.TestDissolveOnActiveRegroupGroupId)",
        groupId.Value, regroupingMember.Value);

    RequestDissolveGroupWithPolicy(groupId, AgentGroupOperationSource::Manual,
        [groupId](bool success)
        {
            // Milestone 2.12F3 P3 fix (STATIC review): CONFIRMED, not
            // PASSED - success alone only proves the dissolve itself
            // committed, not that it actually stopped a still-running
            // REGROUP. Between this dissolve being submitted and its own
            // async confirmation, the triggering member's REGROUP could
            // naturally have already arrived/completed/been preempted on
            // its own, entirely independent of this dissolve - so this
            // line must not claim more than success itself proves.
            // Whether the dissolve actually stopped a live movement is a
            // separate fact, visible in AIWorldMgr::StopInFlightGroupCoordination()'s
            // own COORDINATION_STOPPED_BY_LIFECYCLE log line if and when
            // it fires for this same member.
            if (success)
                TC_LOG_INFO("ai.world", "AI test dissolve-on-active-regroup CONFIRMED: group={} dissolved after active-REGROUP trigger", groupId.Value);
            else
                TC_LOG_ERROR("ai.world", "AI test dissolve-on-active-regroup FAILED: group={} could not be dissolved", groupId.Value);
        });
}

// Milestone 2.12G2R P2 fix, round 2 (STATIC review): TRIGGER/VERIFY
// two-phase, VERIFY now provenance-aware - see this method's own
// declaration comment in AIWorldMgr.h for the full shape and why polling
// "is the old attempt gone" alone (round 1) was still not enough: it
// cannot tell WHY the attempt is gone (a natural ARRIVED clears the exact
// same fields a genuine preemption does), and re-deriving "is GetFood
// active" on a LATER poll could itself go stale if GetFood already
// finished by then. Called from Update() only right after a
// RunCoalitionCoordination() pass actually runs, same reasoning as
// CheckTestDissolveOnActiveRegroup()'s own comment.
void AIWorldMgr::CheckTestPreemptOnActiveRoam()
{
    AgentRecord* record = _registry.Find(_testPreemptOnActiveRoamAgentId);
    if (!record)
    {
        // Same reasoning as CheckTestDissolveOnActiveRegroup()'s own
        // round-3 fix - the configured agent could in principle stop
        // resolving (e.g. a future admin removal path) before this hook
        // ever observes an active ROAM, or mid-verification; disable
        // outright rather than poll a target that can never resolve
        // again for the rest of this process's lifetime.
        TC_LOG_ERROR("ai.world", "AIWorld.TestPreemptOnActiveRoamAgentId={} no longer resolves to a registered agent, disabling this test hook",
            _testPreemptOnActiveRoamAgentId.Value);
        _testPreemptOnActiveRoamFired = true;
        return;
    }

    if (!_testPreemptOnActiveRoamTriggered)
    {
        if (!HasActiveCoordinationMoveTo(*record, GoalType::Roam))
            return;

        _testPreemptOnActiveRoamCapturedGroup = record->GroupCoordinationGoalState->SourceGroup;
        _testPreemptOnActiveRoamCapturedStartedAtMs = record->GroupCoordinationGoalState->StartedAtMs;
        _testPreemptOnActiveRoamVerifyDeadlineMs = CurrentTimeMs() + TestHookVerifyTimeoutMs;
        _testPreemptOnActiveRoamTriggered = true;

        TC_LOG_INFO("ai.world", "AI test preempt-on-active-roam: agent={} group={} has an active ROAM in flight, raising Hunger to 1.0 to trigger production GET_FOOD preemption, verifying postcondition (AIWorld.TestPreemptOnActiveRoamAgentId)",
            record->Id.Value, _testPreemptOnActiveRoamCapturedGroup.Value);

        record->Needs.Hunger = 1.0f;
        return;
    }

    CoordinationStopVerification verification = VerifyCoordinationStop(*record, CoordinationStopReason::PreemptedByGoal,
        GoalType::Roam, _testPreemptOnActiveRoamCapturedGroup, _testPreemptOnActiveRoamCapturedStartedAtMs);

    if (!verification.StopEventMatches)
    {
        if (CurrentTimeMs() >= _testPreemptOnActiveRoamVerifyDeadlineMs)
        {
            TC_LOG_ERROR("ai.world", "AI test preempt-on-active-roam FAILED: agent={} group={} no matching PreemptedByGoal stop event was ever observed for the original ROAM attempt after the verification timeout (most likely a natural ARRIVED, which never records one)",
                record->Id.Value, _testPreemptOnActiveRoamCapturedGroup.Value);
            _testPreemptOnActiveRoamFired = true;
        }
        return;
    }

    _testPreemptOnActiveRoamFired = true;

    // Milestone 2.12G2R P2 fix, round 3 (STATIC review): read straight
    // from the matching event's own PreemptingGoal - captured
    // synchronously by UpdateNeeds() itself at the exact stop moment,
    // NOT re-derived from record->ActiveGoalState's own CURRENT state
    // here, which could already have moved on by this later poll (the
    // preempting goal may have already finished, or a different one may
    // have taken its place, in the time since the stop actually
    // happened).
    bool preemptedByGetFood = verification.PreemptingGoal.has_value() && *verification.PreemptingGoal == GoalType::GetFood;

    if (verification.FullyConfirmed() && preemptedByGetFood)
        TC_LOG_INFO("ai.world", "AI test preempt-on-active-roam PASSED: agent={} group={} a genuine PreemptedByGoal stop was observed for the original ROAM attempt, coordination state/engine generator are fully clear, and the recorded preemptor was a real individual GET_FOOD goal",
            record->Id.Value, _testPreemptOnActiveRoamCapturedGroup.Value);
    else
        TC_LOG_ERROR("ai.world", "AI test preempt-on-active-roam FAILED: agent={} group={} stopEventMatches={} groupCoordinationGoalGone={} activeActionStateGone={} engineGeneratorGone={} preemptingOwner={} preemptingGoal={} preemptedByGetFood={}",
            record->Id.Value, _testPreemptOnActiveRoamCapturedGroup.Value, verification.StopEventMatches,
            verification.GroupCoordinationGoalGone, verification.ActiveActionStateGone, verification.EngineGeneratorGone,
            verification.PreemptingOwner ? ToString(*verification.PreemptingOwner) : "NONE",
            verification.PreemptingGoal ? ToString(*verification.PreemptingGoal) : "NONE", preemptedByGetFood);
}

// Milestone 2.12G2R P2 fix, round 2 (STATIC review): TRIGGER/VERIFY
// two-phase, VERIFY now provenance-aware - same shape/reasoning as
// CheckTestPreemptOnActiveRoam() above, gated on
// CoordinationStopReason::StoppedByLifecycle instead of ::PreemptedByGoal.
// An earlier version (round 1) logged PASSED purely from "membership
// removed AND the old attempt's bookkeeping happens to be gone", which
// could not tell a genuine StopGroupCoordinationForMember() stop apart
// from the ROAM having simply arrived naturally at roughly the same time
// membership was independently removed.
void AIWorldMgr::CheckTestLeaveOnActiveRoam()
{
    AgentRecord* record = _registry.Find(_testLeaveOnActiveRoamAgentId);
    if (!record)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.TestLeaveOnActiveRoamAgentId={} no longer resolves to a registered agent, disabling this test hook",
            _testLeaveOnActiveRoamAgentId.Value);
        _testLeaveOnActiveRoamFired = true;
        return;
    }

    if (!_testLeaveOnActiveRoamTriggered)
    {
        if (!HasActiveCoordinationMoveTo(*record, GoalType::Roam))
            return;

        _testLeaveOnActiveRoamCapturedGroup = record->GroupCoordinationGoalState->SourceGroup;
        _testLeaveOnActiveRoamCapturedStartedAtMs = record->GroupCoordinationGoalState->StartedAtMs;
        _testLeaveOnActiveRoamVerifyDeadlineMs = CurrentTimeMs() + TestHookVerifyTimeoutMs;
        _testLeaveOnActiveRoamTriggered = true;

        AgentId memberId = _testLeaveOnActiveRoamAgentId;
        GroupId groupId = _testLeaveOnActiveRoamCapturedGroup;

        TC_LOG_INFO("ai.world", "AI test leave-on-active-roam: agent={} group={} has an active ROAM in flight, requesting Manual leave, verifying postcondition (AIWorld.TestLeaveOnActiveRoamAgentId)",
            memberId.Value, groupId.Value);

        // Only logs the leave REQUEST's own outcome - never sets *Fired
        // and never claims PASSED/FAILED itself; the VERIFY phase below,
        // polled independently, is the sole authority for that.
        RequestLeaveGroupWithPolicy(groupId, memberId, AgentGroupOperationSource::Manual,
            [memberId, groupId](bool success, AgentGroupPolicyDecision decision)
            {
                if (!success)
                    TC_LOG_ERROR("ai.world", "AI test leave-on-active-roam: leave request itself FAILED for agent={} group={} (decision={}) - verification will time out",
                        memberId.Value, groupId.Value, ToString(decision));
            });
        return;
    }

    CoordinationStopVerification verification = VerifyCoordinationStop(*record, CoordinationStopReason::StoppedByLifecycle,
        GoalType::Roam, _testLeaveOnActiveRoamCapturedGroup, _testLeaveOnActiveRoamCapturedStartedAtMs);

    AgentGroupRecord const* group = _groupRegistry.Find(_testLeaveOnActiveRoamCapturedGroup);
    AgentId memberId = _testLeaveOnActiveRoamAgentId;
    bool stillMember = group && std::any_of(group->Members.begin(), group->Members.end(),
        [memberId](AgentGroupMembership const& membership) { return membership.Member == memberId; });

    if (!verification.StopEventMatches)
    {
        if (CurrentTimeMs() >= _testLeaveOnActiveRoamVerifyDeadlineMs)
        {
            TC_LOG_ERROR("ai.world", "AI test leave-on-active-roam FAILED: agent={} group={} no matching StoppedByLifecycle stop event was ever observed for the original ROAM attempt after the verification timeout (stillMember={})",
                memberId.Value, _testLeaveOnActiveRoamCapturedGroup.Value, stillMember);
            _testLeaveOnActiveRoamFired = true;
        }
        return;
    }

    _testLeaveOnActiveRoamFired = true;

    if (verification.FullyConfirmed() && !stillMember)
        TC_LOG_INFO("ai.world", "AI test leave-on-active-roam PASSED: agent={} group={} a genuine StoppedByLifecycle stop was observed for the original ROAM attempt, coordination state/engine generator are fully clear, and membership is confirmed removed",
            memberId.Value, _testLeaveOnActiveRoamCapturedGroup.Value);
    else
        TC_LOG_ERROR("ai.world", "AI test leave-on-active-roam FAILED: agent={} group={} stopEventMatches={} groupCoordinationGoalGone={} activeActionStateGone={} engineGeneratorGone={} stillMember={}",
            memberId.Value, _testLeaveOnActiveRoamCapturedGroup.Value, verification.StopEventMatches,
            verification.GroupCoordinationGoalGone, verification.ActiveActionStateGone, verification.EngineGeneratorGone, stillMember);
}

// Milestone 2.12G2R P2 fix, round 2 (STATIC review): TRIGGER/VERIFY
// two-phase, VERIFY now provenance-aware, AND captures EVERY currently-
// roaming member of the configured group, not just the first one found
// while iterating Members - a group-wide dissolve must be proven to stop
// every one of them, not merely whichever member this hook happened to
// check first. An earlier version (round 1) logged PASSED purely from
// "the group is gone AND every captured member's own bookkeeping happens
// to be gone", which could not tell a genuine
// StopGroupCoordinationForMember() stop apart from a member's own ROAM
// having simply arrived naturally at roughly the same time the group was
// dissolved.
void AIWorldMgr::CheckTestDissolveOnActiveRoam()
{
    if (!_testDissolveOnActiveRoamTriggered)
    {
        AgentGroupRecord const* group = _groupRegistry.Find(_testDissolveOnActiveRoamGroupId);
        if (!group)
        {
            TC_LOG_ERROR("ai.world", "AIWorld.TestDissolveOnActiveRoamGroupId={} no longer resolves to a registered group, disabling this test hook",
                _testDissolveOnActiveRoamGroupId.Value);
            _testDissolveOnActiveRoamFired = true;
            return;
        }

        std::vector<TestRoamAttemptIdentity> roamingMembers;
        for (AgentGroupMembership const& membership : group->Members)
        {
            AgentRecord const* record = _registry.Find(membership.Member);
            if (!record)
                continue;

            if (!HasActiveCoordinationMoveTo(*record, GoalType::Roam, _testDissolveOnActiveRoamGroupId))
                continue;

            roamingMembers.push_back({ membership.Member, record->GroupCoordinationGoalState->StartedAtMs });
        }

        if (roamingMembers.empty())
            return;

        _testDissolveOnActiveRoamCapturedMembers = std::move(roamingMembers);
        _testDissolveOnActiveRoamVerifyDeadlineMs = CurrentTimeMs() + TestHookVerifyTimeoutMs;
        _testDissolveOnActiveRoamTriggered = true;

        GroupId groupId = _testDissolveOnActiveRoamGroupId;

        TC_LOG_INFO("ai.world", "AI test dissolve-on-active-roam: group={} has {} member(s) with an active ROAM in flight, requesting Manual dissolve, verifying postcondition (AIWorld.TestDissolveOnActiveRoamGroupId)",
            groupId.Value, uint32(_testDissolveOnActiveRoamCapturedMembers.size()));

        // Only logs the dissolve REQUEST's own outcome - never sets
        // *Fired and never claims PASSED/FAILED itself; the VERIFY phase
        // below, polled independently, is the sole authority for that.
        RequestDissolveGroupWithPolicy(groupId, AgentGroupOperationSource::Manual,
            [groupId](bool success)
            {
                if (!success)
                    TC_LOG_ERROR("ai.world", "AI test dissolve-on-active-roam: dissolve request itself FAILED for group={} - verification will time out", groupId.Value);
            });
        return;
    }

    bool groupGone = !_groupRegistry.Find(_testDissolveOnActiveRoamGroupId);

    if (!groupGone)
    {
        if (CurrentTimeMs() >= _testDissolveOnActiveRoamVerifyDeadlineMs)
        {
            TC_LOG_ERROR("ai.world", "AI test dissolve-on-active-roam FAILED: group={} still resolves after the verification timeout - dissolve never confirmed",
                _testDissolveOnActiveRoamGroupId.Value);
            _testDissolveOnActiveRoamFired = true;
        }
        return;
    }

    // The group itself is confirmed gone - now independently verify EVERY
    // captured member's own attempt was genuinely stopped via
    // StopGroupCoordinationForMember() (CoordinationStopReason::
    // StoppedByLifecycle), not merely that it happens to be gone.
    bool allStopped = true;
    for (TestRoamAttemptIdentity const& attempt : _testDissolveOnActiveRoamCapturedMembers)
    {
        AgentRecord const* record = _registry.Find(attempt.Member);
        if (!record)
        {
            // The member itself is gone entirely (a separate, legitimate
            // terminal state) - nothing left to verify for it.
            continue;
        }

        CoordinationStopVerification verification = VerifyCoordinationStop(*record, CoordinationStopReason::StoppedByLifecycle,
            GoalType::Roam, _testDissolveOnActiveRoamGroupId, attempt.StartedAtMs);

        if (!verification.FullyConfirmed())
        {
            allStopped = false;
            TC_LOG_ERROR("ai.world", "AI test dissolve-on-active-roam: member={} stopEventMatches={} groupCoordinationGoalGone={} activeActionStateGone={} engineGeneratorGone={} after group={} dissolved",
                attempt.Member.Value, verification.StopEventMatches, verification.GroupCoordinationGoalGone,
                verification.ActiveActionStateGone, verification.EngineGeneratorGone, _testDissolveOnActiveRoamGroupId.Value);
        }
    }

    _testDissolveOnActiveRoamFired = true;

    if (allStopped)
        TC_LOG_INFO("ai.world", "AI test dissolve-on-active-roam PASSED: group={} dissolved and all {} captured ROAM attempt(s) show a genuine StoppedByLifecycle stop with coordination state/engine generator fully clear",
            _testDissolveOnActiveRoamGroupId.Value, uint32(_testDissolveOnActiveRoamCapturedMembers.size()));
    else
        TC_LOG_ERROR("ai.world", "AI test dissolve-on-active-roam FAILED: group={} dissolved but not every captured ROAM attempt was genuinely confirmed stopped",
            _testDissolveOnActiveRoamGroupId.Value);
}

void AIWorldMgr::CheckTestObserveActiveHunt()
{
    AgentRecord const* record = _registry.Find(_testObserveActiveHuntAgentId);
    if (!record)
    {
        TC_LOG_ERROR("ai.world", "AIWorld.TestObserveActiveHuntAgentId={} no longer resolves to a registered agent, disabling this test hook",
            _testObserveActiveHuntAgentId.Value);
        _testObserveActiveHuntFired = true;
        return;
    }

    // Purely observational - never submits an action, never mutates
    // record, never calls StopMoveTo(). Simply polls until a fully-owned
    // in-flight HUNT approach is genuinely observed.
    std::optional<TestHuntAttemptIdentity> identity = CaptureActiveHuntAttempt(*record);
    if (!identity)
        return;

    TC_LOG_INFO("ai.world", "AI HUNT approach runtime test PASSED: agent={} group={} startedAtMs={} targetGuid={} targetEntry={} targetObservedAtMs={} engineGeneratorRunning=true",
        identity->Member.Value, identity->SourceGroup.Value, identity->StartedAtMs,
        identity->Target.Guid.ToString(), identity->Target.Entry, identity->TargetObservedAtMs);

    _testObserveActiveHuntFired = true;
}

void AIWorldMgr::RunGroupProfileAdoption(GroupId groupId, CoalitionFormationProfileId profileId)
{
    // Milestone 2.12E4C2 P3 hardening (STATIC review): GetCoalitionProfileKind()
    // is both the recognized-profile allow-list (nullopt for Invalid or
    // any unrecognized value - a stray/garbage config value must be
    // refused here, not deferred to the next restart's own LoadGroups()
    // fail-closed switch to catch) AND the single source of truth for
    // which AgentGroupKind this profile actually expects - see its own
    // comment for why this is centralized rather than re-derived per
    // caller.
    std::optional<AgentGroupKind> expectedKind = GetCoalitionProfileKind(profileId);
    if (!expectedKind)
    {
        TC_LOG_ERROR("ai.world", "AI group profile adoption FAILED: profile={} (AIWorld.AdoptGroupProfileId) is not a recognized, adoptable profile - refusing to adopt group={}",
            ToString(profileId), groupId.Value);
        return;
    }

    AgentGroupRecord const* group = _groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_ERROR("ai.world", "AI group profile adoption FAILED: group={} (AIWorld.AdoptGroupId) does not exist", groupId.Value);
        return;
    }

    // Milestone 2.12E4C2 P3 hardening (STATIC review): a group whose own
    // Kind does not match what this profile expects is refused too - e.g.
    // adopting a Stable group into WolfLoose would persist a nonsensical
    // provenance combination that RunCoalitionMaintenance() would then
    // never act on (its own Kind check already stops that), but the
    // stored ProfileId would still be misleading.
    if (group->Kind != *expectedKind)
    {
        TC_LOG_ERROR("ai.world", "AI group profile adoption FAILED: group={} has kind={}, but profile={} expects kind={} - refusing to adopt",
            groupId.Value, ToString(group->Kind), ToString(profileId), ToString(*expectedKind));
        return;
    }

    if (group->ProfileId == profileId)
    {
        TC_LOG_INFO("ai.world", "AI group profile adoption: group={} already has profile={}, nothing to do", groupId.Value, ToString(profileId));
        return;
    }

    TC_LOG_INFO("ai.world", "AI group profile adoption: requesting profile={} for group={} (was {})",
        ToString(profileId), groupId.Value, ToString(group->ProfileId));

    // 2.12E4C2 P2 fix, round 3 (STATIC review): marks groupId in-flight
    // BEFORE the async write is even submitted, so the group coarse tick's
    // own SaveGroupState() (see _groupProfileAdoptionInFlight's own
    // declaration comment for the cross-write race this closes) skips this
    // group starting this exact tick, not only once the async call
    // "started" from some other thread's perspective - everything here
    // still only ever runs on the world thread, so there is no true
    // concurrency to race, only two independently-submitted async DB
    // writes whose commit order is not otherwise guaranteed.
    _groupProfileAdoptionInFlight.insert(groupId.Value);

    // 2.12E4C2 P2 fix, round 2 (STATIC review): confirmed write - see this
    // method's own header comment for why AgentGroupRecord::ProfileId is
    // only ever mutated inside AdoptGroupProfileAsync()'s own completion,
    // once success is known, never optimistically here before the DB has
    // actually confirmed anything.
    TransactionCallback callback = _groupPersistence.AdoptGroupProfileAsync(groupId, profileId,
        [this, groupId, profileId](bool success)
        {
            // Cleared first, regardless of outcome - the same "always
            // released before onComplete runs" shape
            // AgentGroupLifecycleSystem::_pendingGroupOperations already
            // uses, see _groupProfileAdoptionInFlight's own declaration
            // comment.
            _groupProfileAdoptionInFlight.erase(groupId.Value);

            if (!success)
            {
                TC_LOG_ERROR("ai.world", "AI group profile adoption FAILED: group={} could not adopt profile={} (DB write failed)",
                    groupId.Value, ToString(profileId));
                return;
            }

            // Re-resolved here, not the pointer captured before this async
            // call started - the same "a completion never trusts request-
            // time validity" discipline AgentGroupLifecycleSystem.h's own
            // header comment documents (the group could in principle have
            // been dissolved while this write was in flight).
            AgentGroupRecord* current = _groupRegistry.Find(groupId);
            if (!current)
            {
                TC_LOG_WARN("ai.world", "AI group profile adoption: group={} no longer exists by the time the DB write for profile={} confirmed",
                    groupId.Value, ToString(profileId));
                return;
            }

            current->ProfileId = profileId;

            TC_LOG_INFO("ai.world", "AI group profile adoption PASSED: group={} adopted profile={}", groupId.Value, ToString(profileId));
        });

    _groupLifecyclePending.AddCallback(std::move(callback));
}

// Milestone 2.12E2: called at most once, from Initialize(), only when all
// three AIWorld.TestGroupMemberAgentId1/2/3 are set - manual proof that
// AgentGroupLifecycleSystem's async Request* API never touches Creature/
// WorldState and never blocks the world thread, exercising exactly the
// Create -> Join x3 -> Leave -> Dissolve sequence the milestone's own
// acceptance criteria describes, entirely as a chain of completions (see
// RunGroupLifecycleSmokeTestJoinStep()).
//
// Deliberately does NOT create anything: an earlier (2.12E1) version
// minted three fresh AgentType::Civilian AgentRecords at
// testSpawnId+1/+2/+3 via CreateCreatureAgent() - review rejected this.
// CreateCreatureAgent() only checks (map_id, spawn_id) uniqueness in
// ai_agents; it never confirms a matching TrinityCore creature spawn
// actually exists, so this could either produce a permanent ghost
// AgentRecord with no real Creature ever able to bind it, or - worse - if
// that (map, spawn) combination happened to already name a real creature,
// silently claim it as a fake Civilian out from under whatever AgentType
// it should have had. Every AgentRecord is supposed to name a real,
// individually-bindable creature spawn (see AgentRecord.h) - a test
// harness is not exempt from that invariant. Instead, this requires three
// AgentIds the operator has explicitly configured to already-existing,
// already-registered agents (e.g. three real test wolves created some
// other way) - see the three AIWorld.TestGroupMemberAgentId* config
// values read in Initialize().
void AIWorldMgr::RunGroupLifecycleSmokeTest(AgentId memberId1, AgentId memberId2, AgentId memberId3)
{
    std::array<AgentId, 3> memberIds{ memberId1, memberId2, memberId3 };

    for (AgentId memberId : memberIds)
    {
        if (!_registry.Find(memberId))
        {
            TC_LOG_ERROR("ai.world", "AI group lifecycle smoke test: configured member agent id={} does not exist in _registry, aborting - "
                "AIWorld.TestGroupMemberAgentId1/2/3 must all name real, already-registered agents", memberId.Value);
            return;
        }
    }

    TC_LOG_INFO("ai.world", "AI group lifecycle smoke test: members={} {} {}",
        memberIds[0].Value, memberIds[1].Value, memberIds[2].Value);

    // Milestone 2.12E4C2 P2 fix (STATIC review): Invalid - a manual smoke
    // test group is not created by any automatic formation profile, so it
    // must never be implicitly eligible for one's own automatic
    // maintenance (see AgentGroupRecord::ProfileId).
    _groupLifecycleSystem.RequestCreateGroup(AgentGroupKind::Loose, 0, 0.0f, 0.0f, 0.0f, 1.0f, CoalitionFormationProfileId::Invalid,
        _groupRegistry, _groupPersistence, _groupLifecyclePending,
        [this, memberIds](std::optional<GroupId> groupId)
        {
            if (!groupId)
            {
                TC_LOG_ERROR("ai.world", "AI group lifecycle smoke test FAILED: CreateGroup failed, aborting");
                return;
            }

            RunGroupLifecycleSmokeTestJoinStep(*groupId, memberIds, 0);
        });
}

void AIWorldMgr::RunGroupLifecycleSmokeTestJoinStep(GroupId groupId, std::array<AgentId, 3> memberIds, std::size_t index)
{
    if (index == memberIds.size())
    {
        // All joined - leave the last member, then dissolve. Both chained
        // the same way CreateGroup() -> RunGroupLifecycleSmokeTestJoinStep()
        // already are: only issued from the previous step's own confirmed
        // completion.
        _groupLifecycleSystem.RequestLeaveGroup(groupId, memberIds[2], _groupRegistry, _groupPersistence, _groupLifecyclePending,
            [this, groupId, memberIds](bool success)
            {
                if (!success)
                {
                    RunGroupLifecycleSmokeTestAbort(groupId, "LeaveGroup", memberIds[2]);
                    return;
                }

                RequestDissolveGroup(groupId, [groupId, memberIds](bool success)
                {
                    if (!success)
                    {
                        TC_LOG_ERROR("ai.world", "AI group lifecycle smoke test FAILED: DissolveGroup failed for group={}, giving up", groupId.Value);
                        return;
                    }

                    TC_LOG_INFO("ai.world", "AI group lifecycle smoke test PASSED: members {} {} {} remain ordinary AgentRecords, untouched by group lifecycle",
                        memberIds[0].Value, memberIds[1].Value, memberIds[2].Value);
                });
            });
        return;
    }

    uint64 nowMs = CurrentTimeMs();
    _groupLifecycleSystem.RequestJoinGroup(groupId, memberIds[index], nowMs, _groupRegistry, _registry, _groupPersistence, _groupLifecyclePending,
        [this, groupId, memberIds, index](bool success)
        {
            if (!success)
            {
                RunGroupLifecycleSmokeTestAbort(groupId, "JoinGroup", memberIds[index]);
                return;
            }

            RunGroupLifecycleSmokeTestJoinStep(groupId, memberIds, index + 1);
        });
}

void AIWorldMgr::RunGroupLifecycleSmokeTestAbort(GroupId groupId, char const* step, AgentId memberId)
{
    TC_LOG_ERROR("ai.world", "AI group lifecycle smoke test FAILED: {} failed for member id={}, group={} - attempting best-effort cleanup",
        step, memberId.Value, groupId.Value);

    RequestDissolveGroup(groupId, [groupId](bool success)
    {
        if (success)
            TC_LOG_INFO("ai.world", "AI group lifecycle smoke test cleanup: group={} dissolved after earlier failure", groupId.Value);
        else
            TC_LOG_ERROR("ai.world", "AI group lifecycle smoke test cleanup FAILED: group={} could not be dissolved, left as-is", groupId.Value);
    });
}

void AIWorldMgr::RequestJoinGroupWithPolicy(GroupId groupId, AgentId memberId, uint64 joinedAtMs, AgentGroupOperationSource source,
    std::function<void(bool, AgentGroupPolicyDecision)> onComplete)
{
    AgentGroupRecord const* group = _groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_ERROR("ai.world", "AIWorldMgr::RequestJoinGroupWithPolicy: group id={} does not exist, refusing to join member id={}",
            groupId.Value, memberId.Value);
        onComplete(false, AgentGroupPolicyDecision::InvalidOperation);
        return;
    }

    AgentGroupPolicyContext context;
    context.Config = _groupPolicyConfig;
    context.Source = source;

    AgentGroupPolicyDecision decision = _groupPolicySystem.CanJoin(*group, memberId, context);
    if (decision != AgentGroupPolicyDecision::Allowed)
    {
        TC_LOG_INFO("ai.world", "AI agent group join REJECTED group={} member={} source={} reason={}",
            groupId.Value, memberId.Value, ToString(source), ToString(decision));
        onComplete(false, decision);
        return;
    }

    _groupLifecycleSystem.RequestJoinGroup(groupId, memberId, joinedAtMs, _groupRegistry, _registry, _groupPersistence, _groupLifecyclePending,
        [this, memberId, onComplete = std::move(onComplete)](bool success)
        {
            // 2.12F2 P2 fix (STATIC review): a confirmed join can newly
            // make memberId's own group membership coordination-ambiguous
            // (a second RegroupEnabled group now claims it) while a Regroup
            // from whichever group already owned it before this join is
            // still actively running - see ReconcileGroupCoordinationForMember()'s
            // own comment for why this must be checked here, not only on
            // Leave/Dissolve (StopGroupCoordinationForMember() already
            // covers those - a group disappearing/losing a member can only
            // ever REDUCE ambiguity, never create it, so only Join needs
            // this reconciliation).
            if (success)
                ReconcileGroupCoordinationForMember(memberId);

            // Reached lifecycle - policy already said Allowed above, so
            // that is what onComplete reports regardless of the DB
            // outcome; success alone carries whether the write itself
            // landed.
            onComplete(success, AgentGroupPolicyDecision::Allowed);
        });
}

void AIWorldMgr::RequestLeaveGroupWithPolicy(GroupId groupId, AgentId memberId, AgentGroupOperationSource source,
    std::function<void(bool, AgentGroupPolicyDecision)> onComplete)
{
    AgentGroupRecord const* group = _groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_WARN("ai.world", "AIWorldMgr::RequestLeaveGroupWithPolicy: group id={} does not exist, nothing to do for member id={}",
            groupId.Value, memberId.Value);
        onComplete(false, AgentGroupPolicyDecision::InvalidOperation);
        return;
    }

    AgentGroupPolicyContext context;
    context.Config = _groupPolicyConfig;
    context.Source = source;

    AgentGroupPolicyDecision decision = _groupPolicySystem.CanLeave(*group, memberId, context);
    if (decision != AgentGroupPolicyDecision::Allowed)
    {
        TC_LOG_INFO("ai.world", "AI agent group leave REJECTED group={} member={} source={} reason={}",
            groupId.Value, memberId.Value, ToString(source), ToString(decision));
        onComplete(false, decision);
        return;
    }

    _groupLifecycleSystem.RequestLeaveGroup(groupId, memberId, _groupRegistry, _groupPersistence, _groupLifecyclePending,
        [this, groupId, memberId, onComplete = std::move(onComplete)](bool success)
        {
            // 2.12F2 P2 fix (STATIC review): memberId's own in-flight
            // Regroup (if any) targeting groupId must be stopped on a
            // confirmed leave, the same reasoning RequestDissolveGroup()'s
            // own completion now applies per former member - see
            // StopGroupCoordinationForMember()'s own comment.
            if (success)
                StopGroupCoordinationForMember(memberId, groupId);

            onComplete(success, AgentGroupPolicyDecision::Allowed);
        });
}

// Milestone 2.12E3B: part one - pure, synchronous, no registry/DB touched.
// Every AgentGroupRecord here is a local value never added to
// _groupRegistry, matching AgentGroupPolicySystem's own "pure value
// transform" contract (see its class comment). Runs unconditionally
// whenever this method is called at all (gated only by
// AIWorld.TestGroupPolicy at the call site in Initialize()).
void AIWorldMgr::RunGroupPolicySmokeTest(AgentId testMemberId)
{
    bool allPassed = true;
    auto check = [&allPassed](char const* name, bool condition)
    {
        if (condition)
            TC_LOG_INFO("ai.world", "AI group policy smoke test: {} PASSED", name);
        else
        {
            TC_LOG_ERROR("ai.world", "AI group policy smoke test: {} FAILED", name);
            allPassed = false;
        }
    };

    AgentGroupPolicyContext context;
    context.Config = _groupPolicyConfig;

    // LOOSE: fill to one below capacity, then to exactly capacity.
    AgentGroupRecord looseGroup;
    looseGroup.Id = GroupId{ 1 };
    looseGroup.Kind = AgentGroupKind::Loose;
    for (uint32 i = 0; i + 1 < _groupPolicyConfig.LooseMaxMembers; ++i)
        looseGroup.Members.push_back(AgentGroupMembership{ AgentId{ 1000 + i }, 0 });

    check("LOOSE CanJoin below capacity",
        _groupPolicySystem.CanJoin(looseGroup, AgentId{ 9001 }, context) == AgentGroupPolicyDecision::Allowed);

    looseGroup.Members.push_back(AgentGroupMembership{ AgentId{ 2000 }, 0 });
    check("LOOSE CanJoin at capacity is GROUP_FULL",
        _groupPolicySystem.CanJoin(looseGroup, AgentId{ 9001 }, context) == AgentGroupPolicyDecision::GroupFull);

    context.Source = AgentGroupOperationSource::AutomaticPolicy;
    check("LOOSE AutomaticPolicy CanLeave is ALLOWED",
        _groupPolicySystem.CanLeave(looseGroup, looseGroup.Members.front().Member, context) == AgentGroupPolicyDecision::Allowed);

    // LOOSE below minimum -> ShouldDissolve.
    AgentGroupRecord shrunkLooseGroup;
    shrunkLooseGroup.Id = GroupId{ 2 };
    shrunkLooseGroup.Kind = AgentGroupKind::Loose;
    for (uint32 i = 0; i + 1 < _groupPolicyConfig.LooseMinMembers; ++i)
        shrunkLooseGroup.Members.push_back(AgentGroupMembership{ AgentId{ 3000 + i }, 0 });
    check("LOOSE ShouldDissolve below minimum",
        _groupPolicySystem.ShouldDissolve(shrunkLooseGroup, context));

    // STABLE: below capacity is ALLOWED the same way LOOSE is; the
    // Manual-vs-AutomaticPolicy leave distinction is what actually differs.
    // 2.12E3B P3 fix (STATIC review): built the same "fill to one below
    // capacity, then push exactly one more" way LOOSE already is above -
    // an earlier version always seeded exactly one member regardless of
    // StableGroupMaxMembers, so a valid StableGroupMaxMembers=1
    // configuration made the "below capacity" check below false-FAIL
    // (Members.size()==1 already >= max==1, i.e. GroupFull, not Allowed).
    // Also adds the same "at capacity is GROUP_FULL" check LOOSE already
    // has, which the original Stable section was missing entirely - the
    // extra push_back before that check is also what guarantees
    // stableGroup.Members is never empty by the time .front() is used
    // below, for any StableGroupMaxMembers >= 1.
    AgentGroupRecord stableGroup;
    stableGroup.Id = GroupId{ 3 };
    stableGroup.Kind = AgentGroupKind::Stable;
    for (uint32 i = 0; i + 1 < _groupPolicyConfig.StableMaxMembers; ++i)
        stableGroup.Members.push_back(AgentGroupMembership{ AgentId{ 4000 + i }, 0 });

    context.Source = AgentGroupOperationSource::Manual;
    check("STABLE CanJoin below capacity",
        _groupPolicySystem.CanJoin(stableGroup, AgentId{ 9002 }, context) == AgentGroupPolicyDecision::Allowed);

    stableGroup.Members.push_back(AgentGroupMembership{ AgentId{ 5000 }, 0 });
    check("STABLE CanJoin at capacity is GROUP_FULL",
        _groupPolicySystem.CanJoin(stableGroup, AgentId{ 9002 }, context) == AgentGroupPolicyDecision::GroupFull);

    context.Source = AgentGroupOperationSource::AutomaticPolicy;
    check("STABLE AutomaticPolicy CanLeave is STABLE_GROUP_PROTECTED",
        _groupPolicySystem.CanLeave(stableGroup, stableGroup.Members.front().Member, context) == AgentGroupPolicyDecision::StableGroupProtected);
    check("STABLE ShouldDissolve is never automatic",
        !_groupPolicySystem.ShouldDissolve(stableGroup, context));

    context.Source = AgentGroupOperationSource::Manual;
    check("STABLE Manual CanLeave is ALLOWED",
        _groupPolicySystem.CanLeave(stableGroup, stableGroup.Members.front().Member, context) == AgentGroupPolicyDecision::Allowed);

    TC_LOG_INFO("ai.world", "AI group policy smoke test (pure) {}", allPassed ? "PASSED" : "FAILED");

    // Part two - integration: only if testMemberId is a real, registered
    // agent. Proves the policy gate actually protects
    // RequestLeaveGroupWithPolicy(), not just that AgentGroupPolicySystem's
    // own rules are internally consistent.
    if (!testMemberId || !_registry.Find(testMemberId))
    {
        TC_LOG_INFO("ai.world", "AI group policy smoke test (integration): skipped - AIWorld.TestGroupMemberAgentId1 is not set to a real, already-registered agent");
        return;
    }

    // Milestone 2.12E4C2 P2 fix (STATIC review): Invalid - same reasoning
    // as RunGroupLifecycleSmokeTest()'s own create call.
    _groupLifecycleSystem.RequestCreateGroup(AgentGroupKind::Stable, 0, 0.0f, 0.0f, 0.0f, 1.0f, CoalitionFormationProfileId::Invalid,
        _groupRegistry, _groupPersistence, _groupLifecyclePending,
        [this, testMemberId](std::optional<GroupId> groupId)
        {
            if (!groupId)
            {
                TC_LOG_ERROR("ai.world", "AI group policy smoke test (integration) FAILED: CreateGroup failed, aborting");
                return;
            }

            GroupId stableGroupId = *groupId;
            uint64 nowMs = CurrentTimeMs();

            RequestJoinGroupWithPolicy(stableGroupId, testMemberId, nowMs, AgentGroupOperationSource::Manual,
                [this, stableGroupId, testMemberId](bool joined, AgentGroupPolicyDecision joinDecision)
                {
                    if (!joined)
                    {
                        TC_LOG_ERROR("ai.world", "AI group policy smoke test (integration) FAILED: setup JoinGroup failed (decision={}), group={} - attempting best-effort cleanup",
                            ToString(joinDecision), stableGroupId.Value);
                        RequestDissolveGroup(stableGroupId, [](bool) {});
                        return;
                    }

                    // AutomaticPolicy leave on a Stable group must be
                    // rejected - the policy gate itself, never
                    // AgentGroupLifecycleSystem, is what stops this.
                    //
                    // 2.12E3B P3 fix (STATIC review): asserts decision ==
                    // StableGroupProtected specifically, not just
                    // "automaticLeaveSucceeded == false" - a bare bool
                    // could not tell "policy rejected this" apart from
                    // "policy allowed it but the DB write happened to
                    // fail", so a regression that dropped the policy check
                    // entirely could previously still report this gate as
                    // PASSED (submitted to lifecycle, DB failed for an
                    // unrelated reason, member still not removed). Since
                    // RequestLeaveGroupWithPolicy() only ever reports
                    // decision == Allowed once a request has actually
                    // reached AgentGroupLifecycleSystem/the DB (see its own
                    // header comment), decision == StableGroupProtected
                    // here is direct proof the DB was never touched at all
                    // for this call, not just an inference from its
                    // side effects.
                    RequestLeaveGroupWithPolicy(stableGroupId, testMemberId, AgentGroupOperationSource::AutomaticPolicy,
                        [this, stableGroupId, testMemberId](bool automaticLeaveSucceeded, AgentGroupPolicyDecision automaticLeaveDecision)
                        {
                            bool rejectedByPolicy = !automaticLeaveSucceeded && automaticLeaveDecision == AgentGroupPolicyDecision::StableGroupProtected;
                            bool stillMember = false;
                            if (AgentGroupRecord const* group = _groupRegistry.Find(stableGroupId))
                                stillMember = std::any_of(group->Members.begin(), group->Members.end(),
                                    [testMemberId](AgentGroupMembership const& membership) { return membership.Member == testMemberId; });

                            if (!rejectedByPolicy || !stillMember)
                                TC_LOG_ERROR("ai.world", "AI group policy smoke test (integration): AutomaticPolicy leave gate FAILED (decision={}, stillMember={})",
                                    ToString(automaticLeaveDecision), stillMember);
                            else
                                TC_LOG_INFO("ai.world", "AI group policy smoke test (integration): AutomaticPolicy leave gate PASSED (decision={})",
                                    ToString(automaticLeaveDecision));

                            // Manual leave for the same member must now
                            // succeed - Stable is protected from automatic
                            // thinning, never from a deliberate request.
                            // decision == Allowed here confirms this one
                            // actually reached the DB, rather than having
                            // failed for the same (wrong) reason a broken
                            // policy check might reject it.
                            RequestLeaveGroupWithPolicy(stableGroupId, testMemberId, AgentGroupOperationSource::Manual,
                                [this, stableGroupId](bool manualLeaveSucceeded, AgentGroupPolicyDecision manualLeaveDecision)
                                {
                                    if (manualLeaveSucceeded && manualLeaveDecision == AgentGroupPolicyDecision::Allowed)
                                        TC_LOG_INFO("ai.world", "AI group policy smoke test (integration): Manual leave gate PASSED");
                                    else
                                        TC_LOG_ERROR("ai.world", "AI group policy smoke test (integration): Manual leave gate FAILED (decision={})",
                                            ToString(manualLeaveDecision));

                                    RequestDissolveGroup(stableGroupId, [stableGroupId](bool dissolved)
                                    {
                                        if (dissolved)
                                            TC_LOG_INFO("ai.world", "AI group policy smoke test (integration) complete: group={} cleaned up", stableGroupId.Value);
                                        else
                                            TC_LOG_ERROR("ai.world", "AI group policy smoke test (integration) cleanup FAILED: group={} left as-is", stableGroupId.Value);
                                    });
                                });
                        });
                });
        });
}

std::vector<CoalitionCandidate> AIWorldMgr::CollectCoalitionCandidates()
{
    std::vector<CoalitionCandidate> candidates;

    for (AgentId id : _registry.GetAgents())
    {
        AgentRecord* record = _registry.Find(id);
        if (!record || record->WorldState != AgentWorldState::Materialized)
            continue;

        // Milestone 2.12F4A: ControlMode gate (performance/early-rejection
        // - ActionSystem::Validate() is the actual mandatory safety
        // boundary regardless, see its own comment) - an ObserveOnly
        // agent is never an automatic-formation candidate. Membership
        // eligibility could eventually lead to a group coordination
        // dispatch (DispatchGroupMemberActionProposal(), which already
        // gates ControlMode independently too), so excluding it here as
        // well means an ObserveOnly agent never even gets swept into a
        // group by an automatic profile in the first place.
        if (record->ControlMode != AgentControlMode::AIWorldControlled)
            continue;

        Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
        Creature* creature = ResolveLiveCreature(*record, map);
        if (!creature || !creature->IsAlive())
            continue;

        CoalitionCandidate candidate;
        candidate.Id = id;
        candidate.MapId = creature->GetMapId();
        candidate.X = creature->GetPositionX();
        candidate.Y = creature->GetPositionY();
        candidate.Z = creature->GetPositionZ();
        candidate.CreatureEntry = creature->GetEntry();
        candidates.push_back(candidate);
    }

    return candidates;
}

std::unordered_set<uint64> AIWorldMgr::CollectMemberIdsOfKind(AgentGroupKind kind) const
{
    std::unordered_set<uint64> memberIds;

    for (GroupId groupId : _groupRegistry.GetGroups())
    {
        AgentGroupRecord const* group = _groupRegistry.Find(groupId);
        if (!group || group->Kind != kind)
            continue;

        for (AgentGroupMembership const& membership : group->Members)
            memberIds.insert(membership.Member.Value);
    }

    return memberIds;
}

void AIWorldMgr::RunCoalitionFormationPass(uint32 diff)
{
    // Milestone 2.12G1 P2 fix (STATIC review): each profile's own gate/
    // cadence stays per-profile DATA, advanced here - only the discovery/
    // dispatch below is shared. Entirely gated on each profile's own
    // AutoFormation flag (off by default) the same "no cost at all unless
    // the feature is on" treatment AIWorld.TestGroupPolicy's own one-shot
    // smoke test gets at Initialize(), just on a recurring cadence here
    // instead - a disabled profile's timer does not even advance.
    //
    // Milestone 2.12G1 P2 fix, round 2 (STATIC review): a due profile's
    // own timer is deliberately NOT reset here any more - only once that
    // specific profile is actually EVALUATED below (see the loop's own
    // comment for why a profile blocked purely by the shared budget must
    // stay due rather than silently waiting out a full interval again).
    bool wolfDue = false;
    if (_wolfGroupAutoFormation)
    {
        _wolfGroupFormationTimer += diff;
        wolfDue = _wolfGroupFormationTimer >= _wolfGroupFormationIntervalMs;
    }

    bool defiasDue = false;
    if (_defiasGroupAutoFormation)
    {
        _defiasGroupFormationTimer += diff;
        defiasDue = _defiasGroupFormationTimer >= _defiasGroupFormationIntervalMs;
    }

    if (!wolfDue && !defiasDue)
        return;

    // Milestone 2.12G1 P2 fix, round 2 (STATIC review): if the shared
    // budget is already fully occupied by an attempt still resolving from
    // an EARLIER pass, skip discovery entirely - every due profile stays
    // due (its own timer is untouched above), so it is reconsidered the
    // very next tick once the budget frees up, instead of waiting out a
    // full interval again. This also avoids a CollectCoalitionCandidates()/
    // CollectMemberIdsOfKind() scan whose result could not be used by
    // anything this pass anyway.
    if (uint32(_formationInFlight.size()) >= _coalitionFormationMaxInFlight)
        return;

    // Milestone 2.12G1 P2 fix (STATIC review): ONE shared candidate
    // discovery for this whole pass, regardless of how many profiles are
    // due - not one CollectCoalitionCandidates() call per profile.
    std::vector<CoalitionCandidate> candidates = CollectCoalitionCandidates();

    struct DueFormationProfile
    {
        CoalitionFormationProfile* Profile;
        uint32* Timer;
    };

    std::vector<DueFormationProfile> dueProfiles;
    if (wolfDue)
        dueProfiles.push_back({ &_wolfLooseFormationProfile, &_wolfGroupFormationTimer });
    if (defiasDue)
        dueProfiles.push_back({ &_defiasLooseFormationProfile, &_defiasGroupFormationTimer });

    // Milestone 2.12G1 P2 fix (STATIC review): which due profile is
    // evaluated FIRST (and therefore gets first claim on the shared
    // budget) alternates instead of always favoring WolfLoose - only
    // actually matters when more than one profile is due in the same
    // pass. Milestone 2.12G1 P2 fix, round 2 (STATIC review): this now
    // only flips once, at the END of this pass, and only if some profile
    // actually STARTED an attempt (see below) - flipping unconditionally
    // every pass (the previous version) meant the toggle could keep
    // alternating in lockstep with a long-running async formation saga
    // regardless of who actually held the budget, letting the SAME
    // profile keep winning admission every time it happened to be
    // evaluated while the toggle favored it, starving the other one for
    // as long as that saga (or a string of them) kept the budget
    // occupied on exactly the "wrong" passes.
    if (_formationPassFavorDefiasFirst && dueProfiles.size() > 1)
        std::reverse(dueProfiles.begin(), dueProfiles.end());

    // Milestone 2.12G1 P2 fix (STATIC review): CollectMemberIdsOfKind()
    // cached per DISTINCT AgentGroupKind actually needed this pass, not
    // recomputed per profile that happens to share the same Kind - today
    // WolfLoose/DefiasLoose are both Loose, so this collapses to exactly
    // one call for both, not two. Still correct if a future profile of a
    // different Kind is added: each distinct Kind still gets its own
    // cached lookup.
    std::unordered_map<AgentGroupKind, std::unordered_set<uint64>> excludedByKind;

    bool anyStarted = false;
    for (DueFormationProfile const& due : dueProfiles)
    {
        // Milestone 2.12G1 P2 fix, round 2 (STATIC review): if an EARLIER
        // profile in this same pass just consumed the shared budget, this
        // one is blocked for the exact same "unfair, not this profile's
        // fault" reason as the pass-level pre-check above - stays due
        // (its own timer is left untouched, NOT reset to 0), so it gets a
        // fair shot again next tick rather than losing its place for a
        // full interval.
        if (uint32(_formationInFlight.size()) >= _coalitionFormationMaxInFlight)
            continue;

        auto it = excludedByKind.find(due.Profile->Kind);
        if (it == excludedByKind.end())
            it = excludedByKind.emplace(due.Profile->Kind, CollectMemberIdsOfKind(due.Profile->Kind)).first;

        // Milestone 2.12G1 P2 fix, round 2 (STATIC review): the timer
        // resets here, once this profile is genuinely EVALUATED - whether
        // it actually started an attempt, found nothing eligible to
        // propose, or was already mid-attempt from a previous pass, all
        // three are normal per-profile cadence outcomes, not unfair
        // budget starvation, so the interval legitimately restarts.
        *due.Timer = 0;

        if (RunCoalitionFormation(*due.Profile, candidates, it->second))
            anyStarted = true;
    }

    // Milestone 2.12G1 P2 fix, round 2 (STATIC review): only advance the
    // rotation cursor on genuine admission (see this method's own
    // declaration comment) - a pass where every due profile was blocked,
    // already in flight, or found nothing to propose leaves next pass's
    // priority order exactly as it was.
    if (anyStarted)
        _formationPassFavorDefiasFirst = !_formationPassFavorDefiasFirst;
}

bool AIWorldMgr::RunCoalitionFormation(CoalitionFormationProfile const& profile,
    std::vector<CoalitionCandidate> const& candidates,
    std::unordered_set<uint64> const& excludedMembers)
{
    // 2.12E4R P3 fix (STATIC review): fail-closed, not fail-open - see
    // CoalitionFormationProfileId.h for why Invalid exists and why a
    // profile silently defaulting to WolfLoose was the wrong failure mode.
    if (profile.Id == CoalitionFormationProfileId::Invalid)
    {
        TC_LOG_ERROR("ai.world", "AIWorldMgr::RunCoalitionFormation: refusing to run for an Invalid profile id");
        return false;
    }

    if (_formationInFlight.count(profile.Id))
        return false;

    // 2.12E4R P3 fix (STATIC review): the global ceiling across ALL
    // profiles - see _coalitionFormationMaxInFlight's own declaration
    // comment. Checked after the per-profile check above so a profile
    // already in flight is rejected for that reason, not misreported as
    // budget-exhausted. Milestone 2.12G1 P2 fix, round 2 (STATIC review):
    // RunCoalitionFormationPass() above already checks this same budget
    // before ever calling here, so this should be unreachable in
    // practice - kept as defense in depth (this method must never itself
    // assume its only caller is that pass).
    if (uint32(_formationInFlight.size()) >= _coalitionFormationMaxInFlight)
    {
        TC_LOG_DEBUG("ai.world", "AI coalition formation: global in-flight budget ({}) reached, skipping profile={} this pass",
            _coalitionFormationMaxInFlight, ToString(profile.Id));
        return false;
    }

    // Milestone 2.12G1 P2 fix (STATIC review): `candidates`/`excludedMembers`
    // are the caller's (RunCoalitionFormationPass()'s) own shared discovery
    // for this pass - see this method's own header comment.
    std::vector<CoalitionCandidate> eligible;
    eligible.reserve(candidates.size());
    for (CoalitionCandidate const& candidate : candidates)
    {
        if (excludedMembers.count(candidate.Id.Value))
            continue;

        // 2.12E4R P2 fix (STATIC review): a member already RESERVED by
        // another in-flight formation attempt of the same Kind is not
        // eligible either - see CoalitionFormationReservationKey.h for the
        // cross-profile race this closes. excludedMembers above only ever
        // reflects CONFIRMED AgentGroupRegistry membership, which is
        // exactly the window this reservation exists to cover instead.
        if (_formationReservedMembers.count(CoalitionFormationReservationKey{ candidate.Id.Value, profile.Kind }))
            continue;

        eligible.push_back(candidate);
    }

    std::optional<CoalitionProposal> proposal = _coalitionFormationSystem.Propose(eligible, profile);
    if (!proposal)
        return false;

    TC_LOG_INFO("ai.world", "AI coalition formation proposal: profile={} kind={} map={} territory=({:.1f}, {:.1f}, {:.1f}) members={}",
        ToString(profile.Id), ToString(proposal->Kind), proposal->TerritoryMapId,
        proposal->TerritoryX, proposal->TerritoryY, proposal->TerritoryZ, proposal->Members.size());

    _formationInFlight.insert(profile.Id);

    // 2.12E4R P2 fix (STATIC review): reserve every proposed member for
    // this Kind BEFORE submitting RequestCreateGroup() - this, not the
    // per-join re-check in RunCoalitionJoinStep(), is what actually closes
    // the cross-profile race: it makes every proposed member ineligible
    // for any OTHER profile's own candidate filtering above, starting
    // immediately, not only once this attempt's own joins start
    // confirming in AgentGroupRegistry.
    for (AgentId member : proposal->Members)
        _formationReservedMembers.insert(CoalitionFormationReservationKey{ member.Value, proposal->Kind });

    CoalitionFormationProfileId profileId = profile.Id;
    CoalitionProposal proposalValue = *proposal;

    // Milestone 2.12E4C2 P2 fix (STATIC review): profileId (the WolfLoose
    // profile's own Id) is threaded through so the resulting
    // AgentGroupRecord::ProfileId actually names this profile - without
    // it, RunCoalitionMaintenance()'s own profile-identity candidate
    // filter would never recognize a group this very call just formed.
    _groupLifecycleSystem.RequestCreateGroup(proposalValue.Kind, proposalValue.TerritoryMapId,
        proposalValue.TerritoryX, proposalValue.TerritoryY, proposalValue.TerritoryZ, 1.0f, profileId,
        _groupRegistry, _groupPersistence, _groupLifecyclePending,
        [this, profileId, proposalValue](std::optional<GroupId> groupId)
        {
            if (!groupId)
            {
                TC_LOG_ERROR("ai.world", "AI coalition formation FAILED: CreateGroup failed for profile={}, aborting", ToString(profileId));
                ReleaseCoalitionFormationReservations(proposalValue);
                _formationInFlight.erase(profileId);
                return;
            }

            CoalitionFormationAttempt attempt;
            attempt.ProfileId = profileId;
            attempt.Proposal = proposalValue;
            attempt.Group = *groupId;
            attempt.NextMemberIndex = 0;

            RunCoalitionJoinStep(attempt);
        });

    // Milestone 2.12G1 P2 fix, round 2 (STATIC review): true means "an
    // attempt genuinely started this call" (a real proposal was found and
    // the CreateGroup/Join chain was just submitted) - this, not merely
    // having been evaluated, is what RunCoalitionFormationPass() uses to
    // decide whether to advance its own fairness rotation cursor.
    return true;
}

void AIWorldMgr::RunCoalitionJoinStep(CoalitionFormationAttempt attempt)
{
    if (attempt.NextMemberIndex == attempt.Proposal.Members.size())
    {
        TC_LOG_INFO("ai.world", "AI coalition formation PASSED: profile={} group={} members={}",
            ToString(attempt.ProfileId), attempt.Group.Value, attempt.Proposal.Members.size());
        ReleaseCoalitionFormationReservations(attempt.Proposal);
        _formationInFlight.erase(attempt.ProfileId);
        return;
    }

    AgentId memberId = attempt.Proposal.Members[attempt.NextMemberIndex];

    // 2.12E4A/B P2 fix (STATIC review), preserved by the 2.12E4R
    // generalization: the proposal's own eligibility snapshot
    // (CollectCoalitionCandidates()/CollectMemberIdsOfKind(), taken before
    // the async CreateGroup round trip and every earlier join in this
    // chain even started) can go stale by the time this specific join is
    // actually issued - see this method's own header comment.
    // RequestJoinGroupWithPolicy()'s CanJoin() only ever validates against
    // attempt.Group itself, so it cannot catch memberId having joined some
    // OTHER group of the same Kind (or having been removed from _registry
    // entirely) in the meantime - only this re-check can.
    if (!_registry.Find(memberId) || _groupRegistry.IsMemberOfKind(memberId, attempt.Proposal.Kind))
    {
        TC_LOG_ERROR("ai.world", "AI coalition formation: member id={} no longer eligible (missing, or already a {} member), profile={} group={}",
            memberId.Value, ToString(attempt.Proposal.Kind), ToString(attempt.ProfileId), attempt.Group.Value);
        AbortCoalitionFormation(attempt, memberId);
        return;
    }

    uint64 nowMs = CurrentTimeMs();
    GroupId groupId = attempt.Group;
    RequestJoinGroupWithPolicy(groupId, memberId, nowMs, AgentGroupOperationSource::AutomaticPolicy,
        [this, attempt, memberId](bool success, AgentGroupPolicyDecision decision)
        {
            if (!success)
            {
                TC_LOG_ERROR("ai.world", "AI coalition formation: join FAILED for member id={} (decision={}), profile={} group={}",
                    memberId.Value, ToString(decision), ToString(attempt.ProfileId), attempt.Group.Value);
                AbortCoalitionFormation(attempt, memberId);
                return;
            }

            CoalitionFormationAttempt nextAttempt = attempt;
            nextAttempt.NextMemberIndex += 1;
            RunCoalitionJoinStep(nextAttempt);
        });
}

void AIWorldMgr::AbortCoalitionFormation(CoalitionFormationAttempt const& attempt, AgentId failedMember)
{
    TC_LOG_ERROR("ai.world", "AI coalition formation FAILED: member id={} could not join, profile={} group={} - attempting best-effort cleanup",
        failedMember.Value, ToString(attempt.ProfileId), attempt.Group.Value);

    CoalitionFormationProfileId profileId = attempt.ProfileId;
    GroupId groupId = attempt.Group;
    CoalitionProposal proposal = attempt.Proposal;

    RequestDissolveGroup(groupId, [this, profileId, groupId, proposal](bool success)
    {
        if (success)
            TC_LOG_INFO("ai.world", "AI coalition formation cleanup: profile={} group={} dissolved after earlier failure", ToString(profileId), groupId.Value);
        else
            TC_LOG_ERROR("ai.world", "AI coalition formation cleanup FAILED: profile={} group={} could not be dissolved, left as-is", ToString(profileId), groupId.Value);

        ReleaseCoalitionFormationReservations(proposal);
        _formationInFlight.erase(profileId);
    });
}

void AIWorldMgr::ReleaseCoalitionFormationReservations(CoalitionProposal const& proposal)
{
    for (AgentId member : proposal.Members)
        _formationReservedMembers.erase(CoalitionFormationReservationKey{ member.Value, proposal.Kind });
}

// Milestone 2.12E4C1: entirely pure - every AgentGroupRecord/
// CoalitionMemberObservation here is a local value never added to
// _groupRegistry, and coalitionMaintenanceSystem itself is a stack-local
// instance, matching CoalitionMaintenanceSystem's own "pure value
// transform" contract (see its class comment). Runs unconditionally
// whenever this method is called at all (gated only by
// AIWorld.TestCoalitionMaintenance at the call site in Initialize()).
void AIWorldMgr::RunCoalitionMaintenanceSmokeTest() const
{
    bool allPassed = true;
    auto check = [&allPassed](char const* name, bool condition)
    {
        if (condition)
            TC_LOG_INFO("ai.world", "AI coalition maintenance smoke test: {} PASSED", name);
        else
        {
            TC_LOG_ERROR("ai.world", "AI coalition maintenance smoke test: {} FAILED", name);
            allPassed = false;
        }
    };

    CoalitionMaintenanceSystem coalitionMaintenanceSystem;

    CoalitionMaintenanceProfile profile;
    profile.ProfileId = CoalitionFormationProfileId::WolfLoose;
    profile.Kind = AgentGroupKind::Loose;
    profile.MinMembers = 2;
    profile.LeaveRadius = 60.0f;

    AgentGroupRecord looseGroup;
    looseGroup.Id = GroupId{ 1 };
    looseGroup.Kind = AgentGroupKind::Loose;
    looseGroup.ProfileId = CoalitionFormationProfileId::WolfLoose;
    looseGroup.TerritoryMapId = 0;
    looseGroup.TerritoryX = 0.0f;
    looseGroup.TerritoryY = 0.0f;
    looseGroup.TerritoryZ = 0.0f;
    looseGroup.Members.push_back(AgentGroupMembership{ AgentId{ 1 }, 0 });
    looseGroup.Members.push_back(AgentGroupMembership{ AgentId{ 2 }, 0 });

    auto makeObservation = [](AgentId id, bool materialized, bool alive, float x)
    {
        CoalitionMemberObservation observation;
        observation.MemberId = id;
        observation.Materialized = materialized;
        observation.Alive = alive;
        observation.MapId = 0;
        observation.X = x;
        return observation;
    };

    // LOOSE: materialized/alive member well past LeaveRadius -> LeaveMember.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 10.0f),
            makeObservation(AgentId{ 2 }, true, true, 70.0f)
        };
        CoalitionMaintenanceDecision decision = coalitionMaintenanceSystem.Evaluate(looseGroup, profile, members);
        check("LOOSE materialized member 70yd away proposes LEAVE_MEMBER",
            decision.Type == CoalitionMaintenanceDecisionType::LeaveMember && decision.Member == AgentId{ 2 });
    }

    // LOOSE: materialized/alive member well within LeaveRadius -> None.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 10.0f),
            makeObservation(AgentId{ 2 }, true, true, 40.0f)
        };
        CoalitionMaintenanceDecision decision = coalitionMaintenanceSystem.Evaluate(looseGroup, profile, members);
        check("LOOSE materialized member 40yd away proposes NONE",
            decision.Type == CoalitionMaintenanceDecisionType::None);
    }

    // LOOSE: unloaded member "last known" past LeaveRadius -> never a
    // candidate, regardless of its stale recorded position.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 10.0f),
            makeObservation(AgentId{ 2 }, false, false, 70.0f)
        };
        CoalitionMaintenanceDecision decision = coalitionMaintenanceSystem.Evaluate(looseGroup, profile, members);
        check("LOOSE unloaded member is never a LEAVE candidate",
            decision.Type == CoalitionMaintenanceDecisionType::None);
    }

    // LOOSE: dead (Materialized but not Alive) member past LeaveRadius ->
    // never a candidate either - death alone must not trigger a Leave.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 10.0f),
            makeObservation(AgentId{ 2 }, true, false, 70.0f)
        };
        CoalitionMaintenanceDecision decision = coalitionMaintenanceSystem.Evaluate(looseGroup, profile, members);
        check("LOOSE dead member is never a LEAVE candidate",
            decision.Type == CoalitionMaintenanceDecisionType::None);
    }

    // LOOSE: already below MinMembers -> DissolveGroup, independent of
    // distance.
    {
        AgentGroupRecord shrunkGroup;
        shrunkGroup.Id = GroupId{ 2 };
        shrunkGroup.Kind = AgentGroupKind::Loose;
        shrunkGroup.ProfileId = CoalitionFormationProfileId::WolfLoose;
        shrunkGroup.Members.push_back(AgentGroupMembership{ AgentId{ 1 }, 0 });

        std::vector<CoalitionMemberObservation> members{ makeObservation(AgentId{ 1 }, true, true, 0.0f) };
        CoalitionMaintenanceDecision decision = coalitionMaintenanceSystem.Evaluate(shrunkGroup, profile, members);
        check("LOOSE below MinMembers proposes DISSOLVE_GROUP",
            decision.Type == CoalitionMaintenanceDecisionType::DissolveGroup);
    }

    // STABLE: Evaluate() still proposes LEAVE_MEMBER for a far-away member,
    // exactly as readily as for LOOSE - Stable protection is deliberately
    // NOT this class's job, see its own class comment. Actual protection
    // (StableGroupProtected) only exists once 2.12E4C2's own
    // RequestLeaveGroupWithPolicy(AutomaticPolicy) call runs this decision
    // through AgentGroupPolicySystem - not exercised here, since 2.12E4C1
    // adds no lifecycle orchestration at all.
    {
        AgentGroupRecord stableGroup;
        stableGroup.Id = GroupId{ 3 };
        stableGroup.Kind = AgentGroupKind::Stable;
        stableGroup.ProfileId = CoalitionFormationProfileId::WolfLoose;
        stableGroup.Members.push_back(AgentGroupMembership{ AgentId{ 1 }, 0 });
        stableGroup.Members.push_back(AgentGroupMembership{ AgentId{ 2 }, 0 });

        CoalitionMaintenanceProfile stableProfile = profile;
        stableProfile.Kind = AgentGroupKind::Stable;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 10.0f),
            makeObservation(AgentId{ 2 }, true, true, 70.0f)
        };
        CoalitionMaintenanceDecision decision = coalitionMaintenanceSystem.Evaluate(stableGroup, stableProfile, members);
        check("STABLE Evaluate() still proposes LEAVE_MEMBER (protection lives in AgentGroupPolicySystem, not here)",
            decision.Type == CoalitionMaintenanceDecisionType::LeaveMember && decision.Member == AgentId{ 2 });
    }

    // P3 hardening (STATIC review): an Invalid profile never proposes
    // anything, even for an otherwise-textbook far-away member - fail
    // closed rather than silently evaluating MinMembers/LeaveRadius values
    // that were never actually configured for any real profile.
    {
        CoalitionMaintenanceProfile invalidProfile = profile;
        invalidProfile.ProfileId = CoalitionFormationProfileId::Invalid;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 10.0f),
            makeObservation(AgentId{ 2 }, true, true, 70.0f)
        };
        CoalitionMaintenanceDecision decision = coalitionMaintenanceSystem.Evaluate(looseGroup, invalidProfile, members);
        check("Invalid profile proposes NONE",
            decision.Type == CoalitionMaintenanceDecisionType::None);
    }

    // P3 hardening (STATIC review): a profile whose Kind does not match
    // the group being evaluated (a caller mismatch) never proposes
    // anything either, rather than applying the wrong Kind's own
    // thresholds to this group.
    {
        CoalitionMaintenanceProfile mismatchedProfile = profile;
        mismatchedProfile.Kind = AgentGroupKind::Stable;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 10.0f),
            makeObservation(AgentId{ 2 }, true, true, 70.0f)
        };
        CoalitionMaintenanceDecision decision = coalitionMaintenanceSystem.Evaluate(looseGroup, mismatchedProfile, members);
        check("profile.Kind mismatch (Stable profile vs. LOOSE group) proposes NONE",
            decision.Type == CoalitionMaintenanceDecisionType::None);
    }

    // 2.12E4C2 P2 fix (STATIC review): a group with no known automatic
    // formation profile (ProfileId == Invalid - a manually/admin-created
    // group, or any group predating the profile_id column) never proposes
    // anything for a real profile either, even though its Kind matches -
    // this is the actual fix for the P2 that let WolfLoose maintenance
    // apply to every LOOSE group regardless of provenance.
    {
        AgentGroupRecord unclassifiedGroup = looseGroup;
        unclassifiedGroup.Id = GroupId{ 4 };
        unclassifiedGroup.ProfileId = CoalitionFormationProfileId::Invalid;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 10.0f),
            makeObservation(AgentId{ 2 }, true, true, 70.0f)
        };
        CoalitionMaintenanceDecision decision = coalitionMaintenanceSystem.Evaluate(unclassifiedGroup, profile, members);
        check("group.ProfileId mismatch (manual/unclassified LOOSE group vs. WolfLoose profile) proposes NONE",
            decision.Type == CoalitionMaintenanceDecisionType::None);
    }

    TC_LOG_INFO("ai.world", "AI coalition maintenance smoke test {}", allPassed ? "PASSED" : "FAILED");
}

// Milestone 2.12F1: entirely pure - every AgentGroupRecord/
// CoalitionMemberObservation/AgentGroupCoordinationProfile here is a
// local value never added to _groupRegistry, and agentGroupIntentSystem
// itself is a stack-local instance, matching AgentGroupIntentSystem's own
// "pure value transform" contract (see its class comment). Runs
// unconditionally whenever this method is called at all (gated only by
// AIWorld.TestGroupIntent at the call site in Initialize()).
void AIWorldMgr::RunGroupIntentSmokeTest() const
{
    bool allPassed = true;
    auto check = [&allPassed](char const* name, bool condition)
    {
        if (condition)
            TC_LOG_INFO("ai.world", "AI group intent smoke test: {} PASSED", name);
        else
        {
            TC_LOG_ERROR("ai.world", "AI group intent smoke test: {} FAILED", name);
            allPassed = false;
        }
    };

    AgentGroupIntentSystem agentGroupIntentSystem;

    // Milestone 2.12G2: a fixed, arbitrary "now" - Evaluate() is a pure
    // function of its inputs including this, so any fixed value proves
    // the same thing; what matters for the ROAM cases below is that every
    // call in this test uses the SAME nowMs, the same way a real
    // RunCoalitionCoordination() pass uses one shared CurrentTimeMs() for
    // every group it evaluates.
    constexpr uint64 nowMs = 100000;

    AgentGroupCoordinationProfile profile;
    profile.ProfileId = CoalitionFormationProfileId::WolfLoose;
    profile.Kind = AgentGroupKind::Loose;
    profile.RegroupEnabled = true;
    profile.RegroupRadius = 20.0f;

    AgentGroupRecord looseGroup;
    looseGroup.Id = GroupId{ 1 };
    looseGroup.Kind = AgentGroupKind::Loose;
    looseGroup.ProfileId = CoalitionFormationProfileId::WolfLoose;
    looseGroup.TerritoryMapId = 0;
    looseGroup.TerritoryX = 0.0f;
    looseGroup.TerritoryY = 0.0f;
    looseGroup.TerritoryZ = 0.0f;
    looseGroup.Members.push_back(AgentGroupMembership{ AgentId{ 1 }, 0 });
    looseGroup.Members.push_back(AgentGroupMembership{ AgentId{ 2 }, 0 });

    auto makeObservation = [](AgentId id, bool materialized, bool alive, float x)
    {
        CoalitionMemberObservation observation;
        observation.MemberId = id;
        observation.Materialized = materialized;
        observation.Alive = alive;
        observation.MapId = 0;
        observation.X = x;
        return observation;
    };

    // LOOSE, RegroupEnabled: materialized/alive member well past
    // RegroupRadius -> Regroup, targeting the group's own territory.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 30.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, profile, members, nowMs);
        check("LOOSE materialized member 30yd past RegroupRadius=20 proposes REGROUP at territory",
            intent.Type == AgentGroupIntentType::Regroup && intent.MapId == looseGroup.TerritoryMapId &&
            intent.X == looseGroup.TerritoryX && intent.Y == looseGroup.TerritoryY && intent.Z == looseGroup.TerritoryZ);
    }

    // LOOSE, RegroupEnabled: materialized/alive member well within
    // RegroupRadius -> None.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 10.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, profile, members, nowMs);
        check("LOOSE materialized member 10yd within RegroupRadius=20 proposes NONE",
            intent.Type == AgentGroupIntentType::None);
    }

    // LOOSE, RegroupEnabled: unloaded member "last known" past
    // RegroupRadius -> never a trigger, regardless of its stale recorded
    // position.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            makeObservation(AgentId{ 2 }, false, false, 30.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, profile, members, nowMs);
        check("LOOSE unloaded member is never a REGROUP trigger",
            intent.Type == AgentGroupIntentType::None);
    }

    // LOOSE, RegroupEnabled: dead (Materialized but not Alive) member past
    // RegroupRadius -> never a trigger either - death alone must not
    // trigger a coordination fact.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            makeObservation(AgentId{ 2 }, true, false, 30.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, profile, members, nowMs);
        check("LOOSE dead member is never a REGROUP trigger",
            intent.Type == AgentGroupIntentType::None);
    }

    // LOOSE, RegroupEnabled: materialized/alive member on a DIFFERENT map
    // than the group's own territory (map 1, group is on map 0) past
    // RegroupRadius -> never a trigger either, regardless of its recorded
    // distance - the same "not the same map as the group's own territory"
    // exclusion Rule 1 already applies before any distance check runs.
    {
        CoalitionMemberObservation differentMapMember;
        differentMapMember.MemberId = AgentId{ 2 };
        differentMapMember.Materialized = true;
        differentMapMember.Alive = true;
        differentMapMember.MapId = 1;
        differentMapMember.X = 30.0f;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            differentMapMember
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, profile, members, nowMs);
        check("different-map member is never a REGROUP trigger",
            intent.Type == AgentGroupIntentType::None);
    }

    // RegroupEnabled=false: proposes NONE even for an otherwise-textbook
    // dispersed member - a profile that has not opted into automatic
    // regrouping never gets one.
    {
        AgentGroupCoordinationProfile disabledProfile = profile;
        disabledProfile.RegroupEnabled = false;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 30.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, disabledProfile, members, nowMs);
        check("RegroupEnabled=false proposes NONE",
            intent.Type == AgentGroupIntentType::None);
    }

    // Invalid profile never proposes anything, the same fail-closed
    // discipline CoalitionMaintenanceSystem::Evaluate() already holds.
    {
        AgentGroupCoordinationProfile invalidProfile = profile;
        invalidProfile.ProfileId = CoalitionFormationProfileId::Invalid;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 30.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, invalidProfile, members, nowMs);
        check("Invalid profile proposes NONE",
            intent.Type == AgentGroupIntentType::None);
    }

    // profile.Kind mismatch (a caller mismatch) never proposes anything
    // either, rather than applying the wrong Kind's own thresholds to
    // this group.
    {
        AgentGroupCoordinationProfile mismatchedProfile = profile;
        mismatchedProfile.Kind = AgentGroupKind::Stable;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 30.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, mismatchedProfile, members, nowMs);
        check("profile.Kind mismatch (Stable profile vs. LOOSE group) proposes NONE",
            intent.Type == AgentGroupIntentType::None);
    }

    // group.ProfileId mismatch (a manually/unclassified LOOSE group vs. a
    // real profile) never proposes anything either - this is the actual
    // guard that stops WolfLoose coordination from applying to every
    // LOOSE group regardless of provenance.
    {
        AgentGroupRecord unclassifiedGroup = looseGroup;
        unclassifiedGroup.Id = GroupId{ 2 };
        unclassifiedGroup.ProfileId = CoalitionFormationProfileId::Invalid;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 30.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(unclassifiedGroup, profile, members, nowMs);
        check("group.ProfileId mismatch (manual/unclassified LOOSE group vs. WolfLoose profile) proposes NONE",
            intent.Type == AgentGroupIntentType::None);
    }

    // Milestone 2.12G2: Roam's own rules, using a profile with
    // RegroupEnabled=false (so these cases only exercise Roam, never
    // accidentally pass because Regroup already fired) unless a case is
    // explicitly testing the REGROUP > ROAM priority order.
    AgentGroupCoordinationProfile roamProfile;
    roamProfile.ProfileId = CoalitionFormationProfileId::WolfLoose;
    roamProfile.Kind = AgentGroupKind::Loose;
    roamProfile.RegroupEnabled = false;
    roamProfile.RoamEnabled = true;
    roamProfile.RoamDistance = 10.0f;
    roamProfile.RoamIntervalMs = 10000;
    roamProfile.RoamArrivalRadius = 3.0f;

    // RoamEnabled=false proposes NONE even for an otherwise-eligible
    // cohesive group - a profile that has not opted into automatic
    // territory movement never gets one.
    {
        AgentGroupCoordinationProfile disabledRoamProfile = roamProfile;
        disabledRoamProfile.RoamEnabled = false;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0.0f),
            makeObservation(AgentId{ 2 }, true, true, 0.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, disabledRoamProfile, members, nowMs);
        check("RoamEnabled=false proposes NONE", intent.Type == AgentGroupIntentType::None);
    }

    // REGROUP > ROAM: a profile with BOTH RegroupEnabled and RoamEnabled,
    // given a dispersed group, proposes Regroup - a roam target must never
    // pull a dispersed group even further apart.
    {
        AgentGroupCoordinationProfile bothProfile = roamProfile;
        bothProfile.RegroupEnabled = true;
        bothProfile.RegroupRadius = 20.0f;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 30.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, bothProfile, members, nowMs);
        check("RegroupEnabled and RoamEnabled both true, dispersed group proposes REGROUP not ROAM",
            intent.Type == AgentGroupIntentType::Regroup);
    }

    // ROAM due: a cohesive group (every member already at the territory
    // anchor, well inside RoamArrivalRadius of the anchor itself but NOT
    // necessarily of the actual roam target) with RoamEnabled and no
    // dispersion proposes Roam, targeting a point within RoamDistance of
    // the group's own territory. This specific (nowMs, roamProfile) pair
    // was hand-verified to land on one of the 8 compass-offset slots, not
    // the anchor-itself slot (RoamPhaseHash(1, WolfLoose, 10) % 9 == 6 -
    // see AgentGroupIntentSystem.cpp) - the anchor slot would make the
    // group already-arrived (distance 0 <= RoamArrivalRadius) and
    // correctly propose None instead, which is a real, valid outcome of
    // the deterministic selection, just not the one this specific case
    // means to exercise.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0.0f),
            makeObservation(AgentId{ 2 }, true, true, 0.0f)
        };
        AgentGroupIntent intent = agentGroupIntentSystem.Evaluate(looseGroup, roamProfile, members, nowMs);

        float dx = intent.X - looseGroup.TerritoryX;
        float dy = intent.Y - looseGroup.TerritoryY;
        float distance = std::sqrt(dx * dx + dy * dy);

        check("cohesive group with RoamEnabled proposes ROAM within RoamDistance of territory",
            intent.Type == AgentGroupIntentType::Roam && intent.MapId == looseGroup.TerritoryMapId &&
            distance <= roamProfile.RoamDistance + 0.01f);
    }

    // Deterministic target: two independent Evaluate() calls given the
    // exact same group/profile/members/nowMs (hence the same roam phase)
    // agree on the exact same ROAM target - no rand()/urand() anywhere.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0.0f),
            makeObservation(AgentId{ 2 }, true, true, 0.0f)
        };
        AgentGroupIntent first = agentGroupIntentSystem.Evaluate(looseGroup, roamProfile, members, nowMs);
        AgentGroupIntent second = agentGroupIntentSystem.Evaluate(looseGroup, roamProfile, members, nowMs);
        check("same group + same roam phase (nowMs) proposes the exact same ROAM target",
            first.Type == AgentGroupIntentType::Roam && second.Type == AgentGroupIntentType::Roam &&
            first.X == second.X && first.Y == second.Y && first.Z == second.Z);
    }

    TC_LOG_INFO("ai.world", "AI group intent smoke test {}", allPassed ? "PASSED" : "FAILED");
}

// Milestone 2.12F2: entirely pure - every AgentGroupIntent/
// AgentGroupCoordinationProfile/CoalitionMemberObservation here is a local
// value, and agentGroupIntentProjector itself is a stack-local instance,
// matching AgentGroupIntentProjector's own "pure value transform" contract
// (see its class comment). Runs unconditionally whenever this method is
// called at all (gated only by AIWorld.TestGroupIntentProjector at the
// call site in Initialize()).
void AIWorldMgr::RunGroupIntentProjectorSmokeTest() const
{
    bool allPassed = true;
    auto check = [&allPassed](char const* name, bool condition)
    {
        if (condition)
            TC_LOG_INFO("ai.world", "AI group intent projector smoke test: {} PASSED", name);
        else
        {
            TC_LOG_ERROR("ai.world", "AI group intent projector smoke test: {} FAILED", name);
            allPassed = false;
        }
    };

    AgentGroupIntentProjector agentGroupIntentProjector;

    AgentGroupCoordinationProfile profile;
    profile.ProfileId = CoalitionFormationProfileId::WolfLoose;
    profile.Kind = AgentGroupKind::Loose;
    profile.RegroupEnabled = true;
    profile.RegroupRadius = 20.0f;

    AgentGroupIntent regroupIntent;
    regroupIntent.Group = GroupId{ 1 };
    regroupIntent.Type = AgentGroupIntentType::Regroup;
    regroupIntent.MapId = 0;
    regroupIntent.X = 0.0f;
    regroupIntent.Y = 0.0f;
    regroupIntent.Z = 0.0f;

    auto makeObservation = [](AgentId id, bool materialized, bool alive, uint32 mapId, float x)
    {
        CoalitionMemberObservation observation;
        observation.MemberId = id;
        observation.Materialized = materialized;
        observation.Alive = alive;
        observation.MapId = mapId;
        observation.X = x;
        return observation;
    };

    // REGROUP intent: a materialized/alive/same-map member well past
    // RegroupRadius gets a proposal targeting the intent's own point; one
    // well within it gets none.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 0, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(regroupIntent, profile, members);
        check("REGROUP proposes exactly one member (the 30yd one), targeting the intent's own point",
            proposals.size() == 1 && proposals[0].Member == AgentId{ 2 } && proposals[0].SourceGroup == regroupIntent.Group &&
            proposals[0].SourceIntent == AgentGroupIntentType::Regroup && proposals[0].MapId == regroupIntent.MapId &&
            proposals[0].X == regroupIntent.X && proposals[0].Y == regroupIntent.Y && proposals[0].Z == regroupIntent.Z);
    }

    // REGROUP intent: unloaded member "last known" past RegroupRadius never
    // gets a proposal, regardless of its stale recorded position.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 5.0f),
            makeObservation(AgentId{ 2 }, false, false, 0, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(regroupIntent, profile, members);
        check("unloaded member never gets a proposal", proposals.empty());
    }

    // REGROUP intent: dead (Materialized but not Alive) member past
    // RegroupRadius never gets a proposal either.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 5.0f),
            makeObservation(AgentId{ 2 }, true, false, 0, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(regroupIntent, profile, members);
        check("dead member never gets a proposal", proposals.empty());
    }

    // REGROUP intent: a materialized/alive member on a DIFFERENT map than
    // the intent's own point never gets a proposal either, regardless of
    // its recorded distance.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 1, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(regroupIntent, profile, members);
        check("different-map member never gets a proposal", proposals.empty());
    }

    // intent.Type == None produces no proposals for anyone, regardless of
    // how dispersed the members are.
    {
        AgentGroupIntent noneIntent;
        noneIntent.Group = GroupId{ 1 };
        noneIntent.Type = AgentGroupIntentType::None;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 5.0f),
            makeObservation(AgentId{ 2 }, true, true, 0, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(noneIntent, profile, members);
        check("intent.Type == None produces no proposals", proposals.empty());
    }

    // Milestone 2.12G2: Roam's own rules - the same materialized/alive/
    // same-map/unloaded/dead/different-map treatment as Regroup above,
    // just compared against RoamArrivalRadius instead of RegroupRadius.
    AgentGroupCoordinationProfile roamProfile = profile;
    roamProfile.RoamEnabled = true;
    roamProfile.RoamDistance = 10.0f;
    roamProfile.RoamIntervalMs = 10000;
    roamProfile.RoamArrivalRadius = 3.0f;

    AgentGroupIntent roamIntent;
    roamIntent.Group = GroupId{ 1 };
    roamIntent.Type = AgentGroupIntentType::Roam;
    roamIntent.MapId = 0;
    roamIntent.X = 8.0f;
    roamIntent.Y = 0.0f;
    roamIntent.Z = 0.0f;

    // ROAM intent: a materialized/alive/same-map member well past
    // RoamArrivalRadius gets a proposal targeting the intent's own point;
    // one well within it gets none.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 7.0f),
            makeObservation(AgentId{ 2 }, true, true, 0, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(roamIntent, roamProfile, members);
        check("ROAM proposes exactly one member (the 30yd one), targeting the intent's own point",
            proposals.size() == 1 && proposals[0].Member == AgentId{ 2 } && proposals[0].SourceGroup == roamIntent.Group &&
            proposals[0].SourceIntent == AgentGroupIntentType::Roam && proposals[0].MapId == roamIntent.MapId &&
            proposals[0].X == roamIntent.X && proposals[0].Y == roamIntent.Y && proposals[0].Z == roamIntent.Z);
    }

    // ROAM intent: unloaded member never gets a proposal, regardless of
    // its stale recorded position.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 7.0f),
            makeObservation(AgentId{ 2 }, false, false, 0, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(roamIntent, roamProfile, members);
        check("ROAM: unloaded member never gets a proposal", proposals.empty());
    }

    // ROAM intent: dead (Materialized but not Alive) member never gets a
    // proposal either.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 7.0f),
            makeObservation(AgentId{ 2 }, true, false, 0, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(roamIntent, roamProfile, members);
        check("ROAM: dead member never gets a proposal", proposals.empty());
    }

    // ROAM intent: a materialized/alive member on a DIFFERENT map than the
    // intent's own point never gets a proposal either.
    {
        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 7.0f),
            makeObservation(AgentId{ 2 }, true, true, 1, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(roamIntent, roamProfile, members);
        check("ROAM: different-map member never gets a proposal", proposals.empty());
    }

    // An unrecognized AgentGroupIntentType value (neither None, Regroup,
    // nor Roam) fails closed to no proposals at all - the same explicit-
    // switch discipline Project() itself holds to (2.12F2 P3 fix, STATIC
    // review), now proven against a genuinely-unrecognized value rather
    // than only against the two recognized ones.
    {
        AgentGroupIntent unknownIntent;
        unknownIntent.Group = GroupId{ 1 };
        unknownIntent.Type = static_cast<AgentGroupIntentType>(99);
        unknownIntent.MapId = 0;
        unknownIntent.X = 8.0f;
        unknownIntent.Y = 0.0f;
        unknownIntent.Z = 0.0f;

        std::vector<CoalitionMemberObservation> members{
            makeObservation(AgentId{ 1 }, true, true, 0, 30.0f)
        };
        std::vector<GroupMemberActionProposal> proposals = agentGroupIntentProjector.Project(unknownIntent, roamProfile, members);
        check("unrecognized AgentGroupIntentType value produces no proposals", proposals.empty());
    }

    TC_LOG_INFO("ai.world", "AI group intent projector smoke test {}", allPassed ? "PASSED" : "FAILED");
}

// Milestone 2.12G3B: manual proof of HuntIntentSystem::Evaluate()'s and
// HuntIntentProjector::Project()'s own rules, entirely pure - synthetic
// AgentGroupRecord/AgentGroupCoordinationProfile/CoalitionMemberObservation/
// HuntTargetObservation/HuntIntent values built on the stack, fed straight
// to stack-local HuntIntentSystem/HuntIntentProjector instances, no
// registry/DB/AIWorldMgr member state touched at all, and no live
// Creature*/Map*/Unit* or ObjectAccessor lookup anywhere (the same "pure
// layer first, zero orchestration" scoping RunGroupIntentSmokeTest()/
// RunGroupIntentProjectorSmokeTest() already established for 2.12F1/F2 -
// this milestone deliberately adds no GoalType::Hunt, no combat
// ActionType, no ActionSystem wiring, and no RunCoalitionCoordination()
// integration; see HuntIntentSystem.h/HuntIntentProjector.h's own class
// comments). Always runs when this method is called at all (see
// AIWorld.TestHuntIntent).
void AIWorldMgr::RunHuntIntentSmokeTest() const
{
    bool allPassed = true;
    auto check = [&allPassed](char const* name, bool condition)
    {
        if (condition)
            TC_LOG_INFO("ai.world", "AI HUNT intent smoke test: {} PASSED", name);
        else
        {
            TC_LOG_ERROR("ai.world", "AI HUNT intent smoke test: {} FAILED", name);
            allPassed = false;
        }
    };

    HuntIntentSystem huntIntentSystem;
    HuntIntentProjector huntIntentProjector;

    constexpr uint64 nowMs = 100000;
    constexpr uint32 huntCreatureEntry = 5000;
    constexpr uint32 otherCreatureEntry = 5001;

    AgentGroupCoordinationProfile profile;
    profile.ProfileId = CoalitionFormationProfileId::WolfLoose;
    profile.Kind = AgentGroupKind::Loose;
    profile.HuntEnabled = true;
    profile.HuntTargetCreatureEntry = huntCreatureEntry;
    profile.HuntAcquisitionRadius = 40.0f;
    profile.HuntObservationMaxAgeMs = 5000;

    AgentGroupRecord group;
    group.Id = GroupId{ 1 };
    group.Kind = AgentGroupKind::Loose;
    group.ProfileId = CoalitionFormationProfileId::WolfLoose;
    group.TerritoryMapId = 0;
    group.TerritoryX = 0.0f;
    group.TerritoryY = 0.0f;
    group.TerritoryZ = 0.0f;
    group.Members.push_back(AgentGroupMembership{ AgentId{ 1 }, 0 });
    group.Members.push_back(AgentGroupMembership{ AgentId{ 2 }, 0 });

    auto makeMember = [](AgentId id, bool materialized, bool alive, uint32 mapId)
    {
        CoalitionMemberObservation observation;
        observation.MemberId = id;
        observation.Materialized = materialized;
        observation.Alive = alive;
        observation.MapId = mapId;
        return observation;
    };

    std::vector<CoalitionMemberObservation> members{
        makeMember(AgentId{ 1 }, true, true, 0),
        makeMember(AgentId{ 2 }, true, true, 0)
    };

    auto makeTarget = [](ObjectGuid guid, uint32 entry, bool alive, uint32 mapId, uint64 observedAtMs)
    {
        HuntTargetProvenance target;
        target.TargetGuid = guid;
        target.TargetEntry = entry;
        target.Alive = alive;
        target.MapId = mapId;
        target.ObservedAtMs = observedAtMs;
        return target;
    };

    auto makeSighting = [](AgentId observer, HuntTargetProvenance const& target, float distance, bool los)
    {
        HuntTargetObservation observation;
        observation.Observer = observer;
        observation.Target = target;
        observation.Distance = distance;
        observation.LineOfSight = los;
        return observation;
    };

    ObjectGuid targetGuidA = ObjectGuid::Create<HighGuid::Unit>(huntCreatureEntry, 100);
    ObjectGuid targetGuidB = ObjectGuid::Create<HighGuid::Unit>(huntCreatureEntry, 200);

    // ---- Intent selection (HuntIntentSystem::Evaluate()) ----

    // A validly observed target -> a HuntIntent is produced, naming that
    // exact target, with StartedAtMs == nowMs while Target.ObservedAtMs is
    // preserved unchanged (the attempt identity and the target-freshness
    // clock are two different fields, never conflated - see HuntIntent.h).
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("valid target produces a HuntIntent naming that exact target",
            intent.has_value() && intent->Group == group.Id && intent->Target.TargetGuid == targetGuidA);
        check("StartedAtMs equals nowMs; Target.ObservedAtMs stays the original observation time, not the attempt time",
            intent.has_value() && intent->StartedAtMs == nowMs && intent->Target.ObservedAtMs == nowMs - 1000);
    }

    // HuntEnabled=false -> nullopt even for an otherwise-textbook target.
    {
        AgentGroupCoordinationProfile disabledProfile = profile;
        disabledProfile.HuntEnabled = false;

        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, disabledProfile, members, targets, nowMs);
        check("HuntEnabled=false selects nullopt", !intent.has_value());
    }

    // Invalid profile, and a profile.Kind mismatch, both fail closed to
    // nullopt - the same three-way profile guard AgentGroupIntentSystem
    // already holds to.
    {
        AgentGroupCoordinationProfile invalidProfile = profile;
        invalidProfile.ProfileId = CoalitionFormationProfileId::Invalid;

        AgentGroupCoordinationProfile mismatchedProfile = profile;
        mismatchedProfile.Kind = AgentGroupKind::Stable;

        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };

        std::optional<HuntIntent> invalidIntent = huntIntentSystem.Evaluate(group, invalidProfile, members, targets, nowMs);
        std::optional<HuntIntent> mismatchedIntent = huntIntentSystem.Evaluate(group, mismatchedProfile, members, targets, nowMs);
        check("Invalid profile selects nullopt", !invalidIntent.has_value());
        check("profile.Kind mismatch selects nullopt", !mismatchedIntent.has_value());
    }

    // group.ProfileId mismatch (an unclassified/manually-owned LOOSE group
    // vs. a real profile) fails closed too - this is the actual guard that
    // stops WolfLoose HUNT selection from applying to every LOOSE group
    // regardless of provenance, mirroring RunGroupIntentSmokeTest()'s own
    // "group.ProfileId mismatch" case.
    {
        AgentGroupRecord unclassifiedGroup = group;
        unclassifiedGroup.ProfileId = CoalitionFormationProfileId::Invalid;

        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(unclassifiedGroup, profile, members, targets, nowMs);
        check("group.ProfileId mismatch selects nullopt", !intent.has_value());
    }

    // An unconfigured policy (HuntTargetCreatureEntry == 0) can select
    // nothing - the same "zero means disabled" reading RoamIntervalMs == 0
    // already gets for ROAM.
    {
        AgentGroupCoordinationProfile zeroEntryProfile = profile;
        zeroEntryProfile.HuntTargetCreatureEntry = 0;

        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, zeroEntryProfile, members, targets, nowMs);
        check("HuntTargetCreatureEntry == 0 selects nullopt", !intent.has_value());
    }

    // HuntAcquisitionRadius <= 0, or non-finite (NaN/Inf), both fail closed
    // - a NaN radius in particular must never reach the per-observation
    // Distance comparison, where it would silently break every comparison
    // against it.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };

        AgentGroupCoordinationProfile zeroRadiusProfile = profile;
        zeroRadiusProfile.HuntAcquisitionRadius = 0.0f;
        std::optional<HuntIntent> zeroIntent = huntIntentSystem.Evaluate(group, zeroRadiusProfile, members, targets, nowMs);
        check("HuntAcquisitionRadius == 0 selects nullopt", !zeroIntent.has_value());

        AgentGroupCoordinationProfile negativeRadiusProfile = profile;
        negativeRadiusProfile.HuntAcquisitionRadius = -10.0f;
        std::optional<HuntIntent> negativeIntent = huntIntentSystem.Evaluate(group, negativeRadiusProfile, members, targets, nowMs);
        check("negative HuntAcquisitionRadius selects nullopt", !negativeIntent.has_value());

        AgentGroupCoordinationProfile nanRadiusProfile = profile;
        nanRadiusProfile.HuntAcquisitionRadius = std::numeric_limits<float>::quiet_NaN();
        std::optional<HuntIntent> nanIntent = huntIntentSystem.Evaluate(group, nanRadiusProfile, members, targets, nowMs);
        check("NaN HuntAcquisitionRadius selects nullopt", !nanIntent.has_value());

        AgentGroupCoordinationProfile infiniteRadiusProfile = profile;
        infiniteRadiusProfile.HuntAcquisitionRadius = std::numeric_limits<float>::infinity();
        std::optional<HuntIntent> infiniteIntent = huntIntentSystem.Evaluate(group, infiniteRadiusProfile, members, targets, nowMs);
        check("infinite HuntAcquisitionRadius selects nullopt", !infiniteIntent.has_value());
    }

    // An unconfigured max observation age (HuntObservationMaxAgeMs == 0)
    // can select nothing either.
    {
        AgentGroupCoordinationProfile zeroMaxAgeProfile = profile;
        zeroMaxAgeProfile.HuntObservationMaxAgeMs = 0;

        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, zeroMaxAgeProfile, members, targets, nowMs);
        check("HuntObservationMaxAgeMs == 0 selects nullopt", !intent.has_value());
    }

    // nowMs == 0 is never a real CurrentTimeMs() reading - selects nullopt.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, 0);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, uint64(0));
        check("nowMs == 0 selects nullopt", !intent.has_value());
    }

    // Empty TargetGuid -> nullopt.
    {
        HuntTargetProvenance target = makeTarget(ObjectGuid::Empty, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("empty TargetGuid selects nullopt", !intent.has_value());
    }

    // A non-creature GUID (e.g. a Player) paired with a self-reported
    // TargetEntry must never be trusted purely on TargetEntry's own say-so.
    {
        ObjectGuid playerGuid = ObjectGuid::Create<HighGuid::Player>(400);
        HuntTargetProvenance target = makeTarget(playerGuid, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("non-creature GUID selects nullopt", !intent.has_value());
    }

    // TargetEntry must be provably the SAME entry already encoded inside
    // TargetGuid itself - a GUID whose own embedded entry disagrees with
    // the declared TargetEntry fails closed here, before profile
    // eligibility is even considered.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, otherCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("TargetGuid's own embedded entry disagreeing with declared TargetEntry selects nullopt", !intent.has_value());
    }

    // An internally-consistent GUID/TargetEntry pair (the GUID's own
    // embedded entry matches the declared TargetEntry) that simply is not
    // the profile's own eligible entry still fails closed.
    {
        ObjectGuid otherEntryGuid = ObjectGuid::Create<HighGuid::Unit>(otherCreatureEntry, 300);
        HuntTargetProvenance target = makeTarget(otherEntryGuid, otherCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("TargetEntry not matching the profile's own eligible entry selects nullopt", !intent.has_value());
    }

    // Dead target -> nullopt.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, false, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("dead target selects nullopt", !intent.has_value());
    }

    // Stale observation (older than HuntObservationMaxAgeMs) -> nullopt.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - profile.HuntObservationMaxAgeMs - 1);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("stale observation selects nullopt", !intent.has_value());
    }

    // Future-dated ObservedAtMs (> nowMs) -> nullopt - never trustworthy,
    // regardless of how small the offset is.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs + 1);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("future-dated observation selects nullopt", !intent.has_value());
    }

    // Target on a different map than the group's own territory -> nullopt
    // (2.12G3A: HUNT is restricted to the group's own base-world map).
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 1, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("different-map target selects nullopt", !intent.has_value());
    }

    // No line-of-sight -> nullopt.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, false) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("missing line-of-sight selects nullopt", !intent.has_value());
    }

    // Distance beyond HuntAcquisitionRadius -> nullopt.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, profile.HuntAcquisitionRadius + 1.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("out-of-radius target selects nullopt", !intent.has_value());
    }

    // A negative or NaN Distance previously passed the single
    // "> HuntAcquisitionRadius" comparison unrejected - both must fail
    // closed now.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);

        std::vector<HuntTargetObservation> negativeTargets{ makeSighting(AgentId{ 1 }, target, -5.0f, true) };
        std::optional<HuntIntent> negativeIntent = huntIntentSystem.Evaluate(group, profile, members, negativeTargets, nowMs);
        check("negative Distance selects nullopt", !negativeIntent.has_value());

        std::vector<HuntTargetObservation> nanTargets{ makeSighting(AgentId{ 1 }, target, std::numeric_limits<float>::quiet_NaN(), true) };
        std::optional<HuntIntent> nanIntent = huntIntentSystem.Evaluate(group, profile, members, nanTargets, nowMs);
        check("NaN Distance selects nullopt", !nanIntent.has_value());

        std::vector<HuntTargetObservation> infiniteTargets{ makeSighting(AgentId{ 1 }, target, std::numeric_limits<float>::infinity(), true) };
        std::optional<HuntIntent> infiniteIntent = huntIntentSystem.Evaluate(group, profile, members, infiniteTargets, nowMs);
        check("infinite Distance selects nullopt", !infiniteIntent.has_value());
    }

    // Non-finite target coordinates (NaN or Inf) must never propagate into
    // a produced HuntIntent/HuntProposal, even though this class's own
    // selection math never reads them directly.
    {
        HuntTargetProvenance nanCoordinateTarget = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        nanCoordinateTarget.X = std::numeric_limits<float>::quiet_NaN();
        std::vector<HuntTargetObservation> nanCoordinateTargets{ makeSighting(AgentId{ 1 }, nanCoordinateTarget, 10.0f, true) };
        std::optional<HuntIntent> nanCoordinateIntent = huntIntentSystem.Evaluate(group, profile, members, nanCoordinateTargets, nowMs);
        check("NaN target coordinate selects nullopt", !nanCoordinateIntent.has_value());

        HuntTargetProvenance infiniteCoordinateTarget = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        infiniteCoordinateTarget.Z = std::numeric_limits<float>::infinity();
        std::vector<HuntTargetObservation> infiniteCoordinateTargets{ makeSighting(AgentId{ 1 }, infiniteCoordinateTarget, 10.0f, true) };
        std::optional<HuntIntent> infiniteCoordinateIntent = huntIntentSystem.Evaluate(group, profile, members, infiniteCoordinateTargets, nowMs);
        check("infinite target coordinate selects nullopt", !infiniteCoordinateIntent.has_value());
    }

    // An unloaded, dead, or non-member observer's sighting is never
    // trusted, regardless of how valid the target itself otherwise is.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);

        std::vector<CoalitionMemberObservation> unloadedObserverMembers{
            makeMember(AgentId{ 1 }, false, false, 0),
            makeMember(AgentId{ 2 }, true, true, 0)
        };
        std::vector<HuntTargetObservation> unloadedTargets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> unloadedIntent = huntIntentSystem.Evaluate(group, profile, unloadedObserverMembers, unloadedTargets, nowMs);
        check("unloaded observer's sighting selects nullopt", !unloadedIntent.has_value());

        std::vector<CoalitionMemberObservation> deadObserverMembers{
            makeMember(AgentId{ 1 }, true, false, 0),
            makeMember(AgentId{ 2 }, true, true, 0)
        };
        std::vector<HuntTargetObservation> deadTargets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> deadIntent = huntIntentSystem.Evaluate(group, profile, deadObserverMembers, deadTargets, nowMs);
        check("dead observer's sighting selects nullopt", !deadIntent.has_value());

        std::vector<HuntTargetObservation> nonMemberTargets{ makeSighting(AgentId{ 99 }, target, 10.0f, true) };
        std::optional<HuntIntent> nonMemberIntent = huntIntentSystem.Evaluate(group, profile, members, nonMemberTargets, nowMs);
        check("non-member observer's sighting selects nullopt", !nonMemberIntent.has_value());

        std::vector<CoalitionMemberObservation> differentMapObserverMembers{
            makeMember(AgentId{ 1 }, true, true, 1),
            makeMember(AgentId{ 2 }, true, true, 0)
        };
        std::vector<HuntTargetObservation> differentMapTargets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> differentMapIntent = huntIntentSystem.Evaluate(group, profile, differentMapObserverMembers, differentMapTargets, nowMs);
        check("observer on a different map than the group's own territory selects nullopt", !differentMapIntent.has_value());
    }

    // Two conflicting CoalitionMemberObservation entries for the SAME
    // MemberId (disagreeing on Alive) make that observer's own state
    // unresolvable - its sighting is rejected, never resolved by trusting
    // whichever entry happened to come first in members.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<CoalitionMemberObservation> conflictingObserverMembers{
            makeMember(AgentId{ 1 }, true, true, 0),
            makeMember(AgentId{ 1 }, true, false, 0)
        };
        std::vector<HuntTargetObservation> targets{ makeSighting(AgentId{ 1 }, target, 10.0f, true) };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, conflictingObserverMembers, targets, nowMs);
        check("conflicting member observations of the same observer selects nullopt", !intent.has_value());
    }

    // Of two valid targets, the nearer one wins.
    {
        HuntTargetProvenance nearTarget = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        HuntTargetProvenance farTarget = makeTarget(targetGuidB, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> targets{
            makeSighting(AgentId{ 1 }, nearTarget, 10.0f, true),
            makeSighting(AgentId{ 2 }, farTarget, 20.0f, true)
        };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("of two valid targets, the nearer one wins",
            intent.has_value() && intent->Target.TargetGuid == targetGuidA);

        // Reordering the same two candidates never changes the result.
        std::vector<HuntTargetObservation> reorderedTargets{
            makeSighting(AgentId{ 2 }, farTarget, 20.0f, true),
            makeSighting(AgentId{ 1 }, nearTarget, 10.0f, true)
        };
        std::optional<HuntIntent> reorderedIntent = huntIntentSystem.Evaluate(group, profile, members, reorderedTargets, nowMs);
        check("reordering candidates does not change the selected target",
            reorderedIntent.has_value() && reorderedIntent->Target.TargetGuid == intent->Target.TargetGuid);
    }

    // Equal distance -> a stable TargetGuid tie-break (the lower raw GUID
    // value wins), never dependent on which order the candidates were
    // given in.
    {
        HuntTargetProvenance targetA = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        HuntTargetProvenance targetB = makeTarget(targetGuidB, huntCreatureEntry, true, 0, nowMs - 1000);
        ObjectGuid expectedWinner = targetGuidA.GetRawValue() < targetGuidB.GetRawValue() ? targetGuidA : targetGuidB;

        std::vector<HuntTargetObservation> targets{
            makeSighting(AgentId{ 1 }, targetA, 15.0f, true),
            makeSighting(AgentId{ 2 }, targetB, 15.0f, true)
        };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("equal-distance candidates use a stable lower-TargetGuid tie-break",
            intent.has_value() && intent->Target.TargetGuid == expectedWinner);

        std::vector<HuntTargetObservation> reorderedTargets{
            makeSighting(AgentId{ 2 }, targetB, 15.0f, true),
            makeSighting(AgentId{ 1 }, targetA, 15.0f, true)
        };
        std::optional<HuntIntent> reorderedIntent = huntIntentSystem.Evaluate(group, profile, members, reorderedTargets, nowMs);
        check("equal-distance tie-break does not depend on candidate order",
            reorderedIntent.has_value() && reorderedIntent->Target.TargetGuid == expectedWinner);
    }

    // Two observations of the SAME TargetGuid, sharing the exact same
    // (ObservedAtMs, Observer) pair, but disagreeing on Distance, make that
    // whole TargetGuid ambiguous - excluded entirely from selection, never
    // resolved by trusting whichever happened to come first in targets.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> conflictingTargets{
            makeSighting(AgentId{ 1 }, target, 10.0f, true),
            makeSighting(AgentId{ 1 }, target, 25.0f, true)
        };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, conflictingTargets, nowMs);
        check("conflicting duplicate observations of the same target selects nullopt", !intent.has_value());

        std::vector<HuntTargetObservation> reorderedConflictingTargets{
            makeSighting(AgentId{ 1 }, target, 25.0f, true),
            makeSighting(AgentId{ 1 }, target, 10.0f, true)
        };
        std::optional<HuntIntent> reorderedIntent = huntIntentSystem.Evaluate(group, profile, members, reorderedConflictingTargets, nowMs);
        check("conflicting duplicate exclusion does not depend on candidate order", !reorderedIntent.has_value());
    }

    // An EXACT duplicate observation (every field identical) of the same
    // target is harmless and does not make the target ambiguous.
    {
        HuntTargetProvenance target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
        std::vector<HuntTargetObservation> exactDuplicateTargets{
            makeSighting(AgentId{ 1 }, target, 10.0f, true),
            makeSighting(AgentId{ 1 }, target, 10.0f, true)
        };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, exactDuplicateTargets, nowMs);
        check("an exact duplicate observation does not make the target ambiguous",
            intent.has_value() && intent->Target.TargetGuid == targetGuidA);
    }

    // Of two observations of the SAME target from two different observers
    // at two different times, the freshest one's own provenance wins.
    {
        HuntTargetProvenance freshTarget = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 500);
        HuntTargetProvenance staleTarget = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 3000);
        std::vector<HuntTargetObservation> targets{
            makeSighting(AgentId{ 2 }, staleTarget, 5.0f, true),
            makeSighting(AgentId{ 1 }, freshTarget, 15.0f, true)
        };
        std::optional<HuntIntent> intent = huntIntentSystem.Evaluate(group, profile, members, targets, nowMs);
        check("of two observations of the same target, the freshest one wins",
            intent.has_value() && intent->Target.ObservedAtMs == nowMs - 500);
    }

    // ---- Projection (HuntIntentProjector::Project()) ----

    HuntIntent validIntent;
    validIntent.Group = group.Id;
    validIntent.Target = makeTarget(targetGuidA, huntCreatureEntry, true, 0, nowMs - 1000);
    validIntent.StartedAtMs = nowMs;

    // Two materialized/alive/same-map members -> two proposals, each
    // carrying the exact target/group/attempt timestamp the intent itself
    // had. HuntProposal.h itself never declares a pointer-typed field, so
    // "no proposal contains a pointer" is a structural guarantee of that
    // struct's own shape, not something a runtime condition below needs to
    // (or can) independently re-verify.
    {
        std::vector<HuntProposal> proposals = huntIntentProjector.Project(validIntent, members);
        check("two valid members produce two proposals", proposals.size() == 2);

        bool everyProposalMatchesIntent = true;
        for (HuntProposal const& proposal : proposals)
        {
            if (proposal.SourceGroup != validIntent.Group || proposal.StartedAtMs != validIntent.StartedAtMs ||
                proposal.Target.TargetGuid != validIntent.Target.TargetGuid || proposal.Target.TargetEntry != validIntent.Target.TargetEntry ||
                proposal.Target.MapId != validIntent.Target.MapId || proposal.Target.Alive != validIntent.Target.Alive ||
                proposal.Target.ObservedAtMs != validIntent.Target.ObservedAtMs)
            {
                everyProposalMatchesIntent = false;
            }
        }
        check("every proposal carries the exact target/group/attempt timestamp the intent had", everyProposalMatchesIntent);
    }

    // Unloaded member -> no proposal for it.
    {
        std::vector<CoalitionMemberObservation> unloadedMembers{
            makeMember(AgentId{ 1 }, false, false, 0),
            makeMember(AgentId{ 2 }, true, true, 0)
        };
        std::vector<HuntProposal> proposals = huntIntentProjector.Project(validIntent, unloadedMembers);
        check("unloaded member gets no proposal", proposals.size() == 1 && proposals[0].Member == AgentId{ 2 });
    }

    // Dead member -> no proposal for it.
    {
        std::vector<CoalitionMemberObservation> deadMembers{
            makeMember(AgentId{ 1 }, true, false, 0),
            makeMember(AgentId{ 2 }, true, true, 0)
        };
        std::vector<HuntProposal> proposals = huntIntentProjector.Project(validIntent, deadMembers);
        check("dead member gets no proposal", proposals.size() == 1 && proposals[0].Member == AgentId{ 2 });
    }

    // Member on a different map than the target -> no proposal for it.
    {
        std::vector<CoalitionMemberObservation> differentMapMembers{
            makeMember(AgentId{ 1 }, true, true, 1),
            makeMember(AgentId{ 2 }, true, true, 0)
        };
        std::vector<HuntProposal> proposals = huntIntentProjector.Project(validIntent, differentMapMembers);
        check("different-map member gets no proposal", proposals.size() == 1 && proposals[0].Member == AgentId{ 2 });
    }

    // An invalid intent (default-constructed: no Group, empty Target)
    // produces no proposals at all, regardless of how many valid members
    // are given.
    {
        HuntIntent invalidIntent;
        std::vector<HuntProposal> proposals = huntIntentProjector.Project(invalidIntent, members);
        check("invalid intent produces no proposals", proposals.empty());
    }

    TC_LOG_INFO("ai.world", "AI HUNT intent smoke test {}", allPassed ? "PASSED" : "FAILED");
}

// Milestone 2.12G3C1: manual proof of ActionSystem::Validate()'s own
// authoritative HUNT approach validation contract, entirely pure -
// synthetic ActionRequest/ActionValidationContext values built on the
// stack, fed straight to _actionSystem.Validate(), no registry/DB/live
// Creature/Map/grid access at all, no ActionExecutor/movement (the same
// "pure layer first" scoping RunControlModeSmokeTest() already
// established for the ControlMode gate - this milestone deliberately adds
// no RunCoalitionCoordination()/DispatchHuntProposal() integration, see
// ActionSystem::ValidateHuntTarget()'s own class comment). Always runs
// when this method is called at all (see AIWorld.TestHuntActionValidation).
void AIWorldMgr::RunHuntActionValidationSmokeTest() const
{
    bool allPassed = true;
    auto check = [&allPassed](char const* name, bool condition)
    {
        if (condition)
            TC_LOG_INFO("ai.world", "AI HUNT action validation smoke test: {} PASSED", name);
        else
        {
            TC_LOG_ERROR("ai.world", "AI HUNT action validation smoke test: {} FAILED", name);
            allPassed = false;
        }
    };

    constexpr uint32 huntCreatureEntry = 5000;
    constexpr uint32 otherCreatureEntry = 5001;
    ObjectGuid targetGuid = ObjectGuid::Create<HighGuid::Unit>(huntCreatureEntry, 100);
    ObjectGuid otherGuid = ObjectGuid::Create<HighGuid::Unit>(huntCreatureEntry, 200);

    // A fully honest HUNT approach: request.Target names exactly what the
    // context claims to have resolved, and request.Destination is exactly
    // the target's own current position. Every subsequent case below
    // starts from a copy of this pair and breaks exactly one fact.
    auto makeValidRequest = [&]()
    {
        ActionRequest request;
        request.Actor = AgentId{ 1 };
        request.Type = ActionType::MoveTo;
        request.SourceGoal = GoalType::Hunt;
        request.GoalStartedAtMs = 1000;
        request.Target = ActionTargetRef{ targetGuid, huntCreatureEntry };

        ActionPosition destination;
        destination.MapId = 0;
        destination.X = 25.0f;
        destination.Y = 0.0f;
        destination.Z = 0.0f;
        request.Destination = destination;
        return request;
    };

    auto makeValidContext = [&]()
    {
        ActionValidationContext context;
        context.Materialized = true;
        context.Alive = true;
        context.ControlMode = AgentControlMode::AIWorldControlled;
        context.ActiveGoalType = GoalType::Hunt;
        context.ActiveGoalStartedAtMs = 1000;
        context.MapId = 0;
        context.X = 0.0f;
        context.Y = 0.0f;
        context.Z = 0.0f;
        context.HasActiveMovement = false;
        context.TargetResolved = true;
        context.TargetAlive = true;
        context.TargetAttackable = true;
        context.TargetGuid = targetGuid;
        context.TargetEntry = huntCreatureEntry;
        context.TargetMapId = 0;
        context.TargetX = 25.0f;
        context.TargetY = 0.0f;
        context.TargetZ = 0.0f;
        return context;
    };

    // A fully honest HUNT approach is ALLOWED.
    {
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), makeValidContext());
        check("valid HUNT approach is ALLOWED", result.Allowed && result.Reason == ActionRejectReason::None);
    }

    // request.Target missing entirely.
    {
        ActionRequest request = makeValidRequest();
        request.Target.reset();
        ActionValidationResult result = _actionSystem.Validate(request, makeValidContext());
        check("request without a Target is REJECTED with TargetMissing",
            !result.Allowed && result.Reason == ActionRejectReason::TargetMissing);
    }

    // request.Target->Guid empty.
    {
        ActionRequest request = makeValidRequest();
        request.Target->Guid = ObjectGuid::Empty;
        ActionValidationResult result = _actionSystem.Validate(request, makeValidContext());
        check("empty request target GUID is REJECTED with TargetMissing",
            !result.Allowed && result.Reason == ActionRejectReason::TargetMissing);
    }

    // request.Target->Entry == 0.
    {
        ActionRequest request = makeValidRequest();
        request.Target->Entry = 0;
        ActionValidationResult result = _actionSystem.Validate(request, makeValidContext());
        check("zero request target entry is REJECTED with TargetMissing",
            !result.Allowed && result.Reason == ActionRejectReason::TargetMissing);
    }

    // request AND context self-consistently agree on a non-creature GUID
    // (e.g. a Player) - self-consistency alone must never be enough; the
    // GUID must provably BE a creature.
    {
        ObjectGuid playerGuid = ObjectGuid::Create<HighGuid::Player>(400);

        ActionRequest request = makeValidRequest();
        request.Target->Guid = playerGuid;

        ActionValidationContext context = makeValidContext();
        context.TargetGuid = playerGuid;

        ActionValidationResult result = _actionSystem.Validate(request, context);
        check("self-consistent non-creature GUID is REJECTED with TargetIdentityMismatch",
            !result.Allowed && result.Reason == ActionRejectReason::TargetIdentityMismatch);
    }

    // request AND context self-consistently agree on a creature GUID whose
    // own embedded entry disagrees with the claimed Entry - self-
    // consistency alone must never be enough; Entry must provably be the
    // SAME entry already encoded inside the GUID itself.
    {
        ObjectGuid mismatchedEntryGuid = ObjectGuid::Create<HighGuid::Unit>(otherCreatureEntry, 300);

        ActionRequest request = makeValidRequest();
        request.Target->Guid = mismatchedEntryGuid;
        request.Target->Entry = huntCreatureEntry;

        ActionValidationContext context = makeValidContext();
        context.TargetGuid = mismatchedEntryGuid;
        context.TargetEntry = huntCreatureEntry;

        ActionValidationResult result = _actionSystem.Validate(request, context);
        check("self-consistent GUID/entry pair whose GUID-embedded entry disagrees with Entry is REJECTED with TargetEntryMismatch",
            !result.Allowed && result.Reason == ActionRejectReason::TargetEntryMismatch);
    }

    // context.TargetResolved == false.
    {
        ActionValidationContext context = makeValidContext();
        context.TargetResolved = false;
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), context);
        check("unresolved context target is REJECTED with TargetNotResolved",
            !result.Allowed && result.Reason == ActionRejectReason::TargetNotResolved);
    }

    // context.TargetAlive == false.
    {
        ActionValidationContext context = makeValidContext();
        context.TargetAlive = false;
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), context);
        check("dead context target is REJECTED with TargetDead",
            !result.Allowed && result.Reason == ActionRejectReason::TargetDead);
    }

    // context.TargetAttackable == false.
    {
        ActionValidationContext context = makeValidContext();
        context.TargetAttackable = false;
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), context);
        check("non-attackable context target is REJECTED with TargetNotAttackable",
            !result.Allowed && result.Reason == ActionRejectReason::TargetNotAttackable);
    }

    // request.Target->Guid disagrees with context.TargetGuid.
    {
        ActionValidationContext context = makeValidContext();
        context.TargetGuid = otherGuid;
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), context);
        check("request/context target GUID mismatch is REJECTED with TargetIdentityMismatch",
            !result.Allowed && result.Reason == ActionRejectReason::TargetIdentityMismatch);
    }

    // request.Target->Entry disagrees with context.TargetEntry.
    {
        ActionValidationContext context = makeValidContext();
        context.TargetEntry = otherCreatureEntry;
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), context);
        check("request/context target entry mismatch is REJECTED with TargetEntryMismatch",
            !result.Allowed && result.Reason == ActionRejectReason::TargetEntryMismatch);
    }

    // Target itself not on the actor's own map.
    {
        ActionValidationContext context = makeValidContext();
        context.TargetMapId = 1;
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), context);
        check("target on a different map than the actor is REJECTED with TargetMapMismatch",
            !result.Allowed && result.Reason == ActionRejectReason::TargetMapMismatch);
    }

    // request.Destination on a different map than the actor - the
    // pre-existing generic DestinationMapMismatch check still applies to a
    // HUNT request exactly as it does to any other MoveTo.
    {
        ActionRequest request = makeValidRequest();
        request.Destination->MapId = 1;
        ActionValidationResult result = _actionSystem.Validate(request, makeValidContext());
        check("destination on a different map than the actor is REJECTED with the pre-existing DestinationMapMismatch",
            !result.Allowed && result.Reason == ActionRejectReason::DestinationMapMismatch);
    }

    // request.Destination does not match the target's own current position.
    {
        ActionRequest request = makeValidRequest();
        request.Destination->Y = 5.0f;
        ActionValidationResult result = _actionSystem.Validate(request, makeValidContext());
        check("destination not matching the target's own current position is REJECTED with TargetPositionMismatch",
            !result.Allowed && result.Reason == ActionRejectReason::TargetPositionMismatch);
    }

    // context.TargetX/Y/Z not finite.
    {
        ActionValidationContext context = makeValidContext();
        context.TargetX = std::numeric_limits<float>::quiet_NaN();
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), context);
        check("non-finite target position is REJECTED with TargetPositionMismatch",
            !result.Allowed && result.Reason == ActionRejectReason::TargetPositionMismatch);
    }

    // context.ControlMode != AIWorldControlled - the pre-existing mandatory
    // ControlMode gate still applies unconditionally to a HUNT request.
    {
        ActionValidationContext context = makeValidContext();
        context.ControlMode = AgentControlMode::ObserveOnly;
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), context);
        check("ObserveOnly actor is REJECTED with the pre-existing ControlModeNotAllowed",
            !result.Allowed && result.Reason == ActionRejectReason::ControlModeNotAllowed);
    }

    // Actor not materialized / dead - the pre-existing common checks still
    // apply unconditionally to a HUNT request.
    {
        ActionValidationContext unmaterializedContext = makeValidContext();
        unmaterializedContext.Materialized = false;
        ActionValidationResult unmaterializedResult = _actionSystem.Validate(makeValidRequest(), unmaterializedContext);
        check("unmaterialized actor is REJECTED with the pre-existing ActorNotMaterialized",
            !unmaterializedResult.Allowed && unmaterializedResult.Reason == ActionRejectReason::ActorNotMaterialized);

        ActionValidationContext deadContext = makeValidContext();
        deadContext.Alive = false;
        ActionValidationResult deadResult = _actionSystem.Validate(makeValidRequest(), deadContext);
        check("dead actor is REJECTED with the pre-existing ActorDead",
            !deadResult.Allowed && deadResult.Reason == ActionRejectReason::ActorDead);
    }

    // Goal type/timestamp dishonesty - the pre-existing common
    // SourceGoal/GoalStartedAtMs honesty checks still apply unconditionally
    // to a HUNT request.
    {
        ActionValidationContext goalMismatchContext = makeValidContext();
        goalMismatchContext.ActiveGoalType = GoalType::GetFood;
        ActionValidationResult goalMismatchResult = _actionSystem.Validate(makeValidRequest(), goalMismatchContext);
        check("goal type mismatch is REJECTED with the pre-existing GoalMismatch",
            !goalMismatchResult.Allowed && goalMismatchResult.Reason == ActionRejectReason::GoalMismatch);

        ActionValidationContext timestampMismatchContext = makeValidContext();
        timestampMismatchContext.ActiveGoalStartedAtMs = 999;
        ActionValidationResult timestampMismatchResult = _actionSystem.Validate(makeValidRequest(), timestampMismatchContext);
        check("goal attempt timestamp mismatch is REJECTED with the pre-existing GoalMismatch",
            !timestampMismatchResult.Allowed && timestampMismatchResult.Reason == ActionRejectReason::GoalMismatch);
    }

    // context.HasActiveMovement - the pre-existing common ActorMovementBusy
    // check still applies unconditionally to a HUNT request.
    {
        ActionValidationContext context = makeValidContext();
        context.HasActiveMovement = true;
        ActionValidationResult result = _actionSystem.Validate(makeValidRequest(), context);
        check("actor with an already-active movement is REJECTED with the pre-existing ActorMovementBusy",
            !result.Allowed && result.Reason == ActionRejectReason::ActorMovementBusy);
    }

    // Target beyond the shared coordination MOVE_TO range - Destination
    // still exactly matches the target's own current position (so
    // TargetPositionMismatch does not fire first), but that position is
    // farther from the actor than ActionSystem::CoordinationMoveToRangeYards().
    {
        ActionRequest request = makeValidRequest();
        ActionValidationContext context = makeValidContext();
        float farX = ActionSystem::CoordinationMoveToRangeYards() + 50.0f;
        request.Destination->X = farX;
        context.TargetX = farX;
        ActionValidationResult result = _actionSystem.Validate(request, context);
        check("target beyond the shared coordination MOVE_TO range is REJECTED with the pre-existing DestinationTooFar",
            !result.Allowed && result.Reason == ActionRejectReason::DestinationTooFar);
    }

    // An ordinary Regroup MoveTo (no Target at all) still validates exactly
    // as it did before this milestone - HUNT's own additional checks must
    // never leak into a non-Hunt SourceGoal.
    {
        ActionRequest request;
        request.Actor = AgentId{ 1 };
        request.Type = ActionType::MoveTo;
        request.SourceGoal = GoalType::Regroup;
        request.GoalStartedAtMs = 1000;

        ActionPosition destination;
        destination.MapId = 0;
        destination.X = 10.0f;
        destination.Y = 0.0f;
        destination.Z = 0.0f;
        request.Destination = destination;

        ActionValidationContext context;
        context.Materialized = true;
        context.Alive = true;
        context.ControlMode = AgentControlMode::AIWorldControlled;
        context.ActiveGoalType = GoalType::Regroup;
        context.ActiveGoalStartedAtMs = 1000;
        context.MapId = 0;
        context.X = 0.0f;
        context.Y = 0.0f;
        context.Z = 0.0f;
        context.HasActiveMovement = false;

        ActionValidationResult result = _actionSystem.Validate(request, context);
        check("an ordinary Regroup MoveTo without a Target is unaffected and still ALLOWED",
            result.Allowed && result.Reason == ActionRejectReason::None);
    }

    TC_LOG_INFO("ai.world", "AI HUNT action validation smoke test {}", allPassed ? "PASSED" : "FAILED");
}

// Milestone 2.12G3D1 P2 fix (runtime finding): manual proof of
// HandleActionCompletion()'s own HUNT arrival-ownership retention -
// entirely pure, synthetic AgentRecord/ActionCompletion values built on
// the stack, fed straight to HandleActionCompletion() itself (both types
// are plain values - no live Creature/Map/registry access needed). Not
// const, unlike the other pure Hunt smoke tests above: HandleActionCompletion()
// itself is a non-const private method (it is the same production
// method every real MOVE_TO completion already goes through, never a
// reimplementation), so this cannot be called from a const context.
//
// Proves specifically: a genuine SUCCESSFUL Arrived completion for a
// HUNT-sourced action retains GroupCoordinationGoalState (same group/
// target/StartedAtMs identity), explicitly transitioned to
// HuntPhase::AtTarget (2.12G3D2A - never merely inferred from
// ActiveActionState's own absence), while still releasing
// ActiveActionState - the fix that stops RunCoalitionCoordination()'s
// very next pass from freshly re-selecting the same still-eligible
// target and dispatching a second, effectively zero-distance MOVE_TO.
// Every OTHER HUNT completion (Failed/DestinationNotReached,
// GoalInterrupted, ActorDematerialized, ActorDead, EngineStopped) still
// releases ownership exactly like before - retaining it there would
// leave a member that never actually reached its target permanently
// stuck. Regroup/Roam arrival is also proven unaffected - the retention
// exception is HUNT-only, never generalized.
//
// This does NOT simulate two full RunCoalitionCoordination() passes
// end to end (that would need a live registry/Map/Creature, the same
// live-server dependency AIWorld.TestObserveActiveHuntAgentId itself
// already has, and is out of scope for a pure smoke test) - it instead
// directly proves the ONE mechanism the loop-prevention actually depends
// on: that GroupCoordinationGoalState is honestly retained (with an
// honest AtTarget phase) after a HUNT arrival and honestly released
// after everything else. DispatchHuntProposal()'s own re-approach rule
// (2.12G3D2A - a member already AtTarget for the SAME attempt is only
// ever re-dispatched once the target has genuinely moved beyond
// ArrivalToleranceYards, reusing the same attempt identity) is live-
// Creature-dependent and is not exercised by this pure test either - see
// AIWorld.TestObserveActiveHuntAgentId's own "stage=REAPPROACH"/
// "reason=ALREADY_AT_TARGET" diagnostic logging for that part's live
// proof.
void AIWorldMgr::RunHuntArrivalOwnershipSmokeTest()
{
    bool allPassed = true;
    auto check = [&allPassed](char const* name, bool condition)
    {
        if (condition)
            TC_LOG_INFO("ai.world", "AI HUNT arrival ownership smoke test: {} PASSED", name);
        else
        {
            TC_LOG_ERROR("ai.world", "AI HUNT arrival ownership smoke test: {} FAILED", name);
            allPassed = false;
        }
    };

    auto makeCoordinationRecord = [](GoalType type, uint64 startedAtMs)
    {
        AgentRecord record;
        record.Id = AgentId{ 1 };

        ActiveAction action;
        action.Type = ActionType::MoveTo;
        action.SourceGoal = type;
        action.GoalStartedAtMs = startedAtMs;
        action.StartedAtMs = startedAtMs;
        record.ActiveActionState = action;

        GroupCoordinationGoal goal;
        goal.Type = type;
        goal.SourceGroup = GroupId{ 1 };
        goal.StartedAtMs = startedAtMs;
        goal.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(5000, 100);
        goal.TargetEntry = 5000;
        goal.TargetObservedAtMs = startedAtMs > 1000 ? startedAtMs - 1000 : 0;
        record.GroupCoordinationGoalState = goal;

        return record;
    };

    auto makeCompletion = [](AgentId actor, GoalType sourceGoal, uint64 goalStartedAtMs,
        ActionCompletionStatus status, ActionCompletionReason reason)
    {
        ActionCompletion completion;
        completion.Actor = actor;
        completion.Type = ActionType::MoveTo;
        completion.SourceGoal = sourceGoal;
        completion.GoalStartedAtMs = goalStartedAtMs;
        completion.Status = status;
        completion.Reason = reason;
        completion.CompletedAtMs = goalStartedAtMs + 1000;
        return completion;
    };

    // A genuine successful HUNT arrival retains GroupCoordinationGoalState
    // (the exact same group/target/StartedAtMs identity) while still
    // releasing ActiveActionState - the movement itself is over, but the
    // group's own ownership of this attempt is not.
    {
        AgentRecord record = makeCoordinationRecord(GoalType::Hunt, 1000);
        ActionCompletion completion = makeCompletion(record.Id, GoalType::Hunt, 1000,
            ActionCompletionStatus::Succeeded, ActionCompletionReason::Arrived);

        HandleActionCompletion(record, completion);

        check("a genuine HUNT arrival releases ActiveActionState",
            !record.ActiveActionState.has_value());
        check("a genuine HUNT arrival retains GroupCoordinationGoalState",
            record.GroupCoordinationGoalState.has_value());
        check("retained ownership keeps the exact same group/target/startedAt identity",
            record.GroupCoordinationGoalState && record.GroupCoordinationGoalState->Type == GoalType::Hunt &&
            record.GroupCoordinationGoalState->SourceGroup == GroupId{ 1 } &&
            record.GroupCoordinationGoalState->StartedAtMs == 1000 &&
            record.GroupCoordinationGoalState->TargetEntry == 5000);
        check("retained ownership is explicitly transitioned to HuntPhase::AtTarget",
            record.GroupCoordinationGoalState && record.GroupCoordinationGoalState->Phase == HuntPhase::AtTarget);
    }

    // Every OTHER HUNT completion reason still releases ownership exactly
    // like before - only a genuine Succeeded/Arrived retains it. Retaining
    // it for any of these would leave a member that never actually
    // reached its target permanently stuck, with no path back to being
    // re-dispatched.
    {
        struct FailureCase { ActionCompletionStatus Status; ActionCompletionReason Reason; char const* Name; };
        FailureCase failureCases[] = {
            { ActionCompletionStatus::Failed, ActionCompletionReason::DestinationNotReached, "destination not reached" },
            { ActionCompletionStatus::Failed, ActionCompletionReason::GoalInterrupted, "goal interrupted" },
            { ActionCompletionStatus::Failed, ActionCompletionReason::ActorDematerialized, "actor dematerialized" },
            { ActionCompletionStatus::Failed, ActionCompletionReason::ActorDead, "actor dead" },
            { ActionCompletionStatus::Failed, ActionCompletionReason::EngineStopped, "engine stopped" },
        };

        for (FailureCase const& failureCase : failureCases)
        {
            AgentRecord record = makeCoordinationRecord(GoalType::Hunt, 1000);
            ActionCompletion completion = makeCompletion(record.Id, GoalType::Hunt, 1000, failureCase.Status, failureCase.Reason);

            HandleActionCompletion(record, completion);

            check("a non-arrival HUNT completion still releases ownership",
                !record.ActiveActionState.has_value() && !record.GroupCoordinationGoalState.has_value());
        }
    }

    // Regroup/Roam arrival still releases ownership unconditionally - the
    // retention exception is HUNT-only, never generalized to the other
    // GroupCoordinationGoal-sourced goals.
    {
        for (GoalType type : { GoalType::Regroup, GoalType::Roam })
        {
            AgentRecord record = makeCoordinationRecord(type, 1000);
            ActionCompletion completion = makeCompletion(record.Id, type, 1000,
                ActionCompletionStatus::Succeeded, ActionCompletionReason::Arrived);

            HandleActionCompletion(record, completion);

            check("Regroup/Roam arrival still releases ownership unconditionally",
                !record.ActiveActionState.has_value() && !record.GroupCoordinationGoalState.has_value());
        }
    }

    // Milestone 2.12G3D2A P2 fix (STATIC review): StopInFlightGroupCoordination()
    // must still honestly record a CoordinationStopEvent for a
    // HuntPhase::AtTarget member - one with NO ActiveActionState at all,
    // since nothing is currently running. WorldState is deliberately left
    // at its own default (AgentWorldState::Abstract, "not materialized")
    // so this exercises the pure value path only - no live Map/Creature
    // lookup is ever attempted, matching this whole smoke test's own
    // registry-free scope.
    {
        AgentRecord record;
        record.Id = AgentId{ 1 };

        GroupCoordinationGoal goal;
        goal.Type = GoalType::Hunt;
        goal.SourceGroup = GroupId{ 1 };
        goal.StartedAtMs = 1000;
        goal.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(5000, 100);
        goal.TargetEntry = 5000;
        goal.TargetObservedAtMs = 500;
        goal.Phase = HuntPhase::AtTarget;
        record.GroupCoordinationGoalState = goal;
        // Deliberately no ActiveActionState - this is exactly what
        // HuntPhase::AtTarget means.

        StopInFlightGroupCoordination(record, "SMOKE_TEST_AT_TARGET_STOP", CoordinationStopReason::StoppedByTargetInvalid);

        check("stopping an AtTarget member releases GroupCoordinationGoalState",
            !record.GroupCoordinationGoalState.has_value());
        check("stopping an AtTarget member records a CoordinationStopEvent (not silently dropped)",
            record.LastCoordinationStop.has_value());
        check("the recorded stop event carries the AtTarget goal's own exact identity",
            record.LastCoordinationStop && record.LastCoordinationStop->Reason == CoordinationStopReason::StoppedByTargetInvalid &&
            record.LastCoordinationStop->SourceGoal == GoalType::Hunt &&
            record.LastCoordinationStop->SourceGroup == GroupId{ 1 } &&
            record.LastCoordinationStop->StartedAtMs == 1000 &&
            record.LastCoordinationStop->TargetGuid == goal.TargetGuid &&
            record.LastCoordinationStop->TargetEntry == 5000);
        check("the recorded stop event never claims an engine generator was running",
            record.LastCoordinationStop && !record.LastCoordinationStop->EngineGeneratorWasRunningBeforeStop &&
            !record.LastCoordinationStop->EngineGeneratorConfirmedStoppedAfterStop);
    }

    // The same stop path for a member still genuinely Approaching (a live
    // ActiveActionState present) continues to record identity correctly
    // too - regression check that the 2.12G3D2A rework did not change
    // this already-established case.
    {
        AgentRecord record = makeCoordinationRecord(GoalType::Hunt, 1000);

        StopInFlightGroupCoordination(record, "SMOKE_TEST_APPROACHING_STOP", CoordinationStopReason::StoppedByLifecycle);

        check("stopping an Approaching member releases both ActiveActionState and GroupCoordinationGoalState",
            !record.ActiveActionState.has_value() && !record.GroupCoordinationGoalState.has_value());
        check("the recorded stop event still carries the correct identity while Approaching",
            record.LastCoordinationStop && record.LastCoordinationStop->Reason == CoordinationStopReason::StoppedByLifecycle &&
            record.LastCoordinationStop->SourceGoal == GoalType::Hunt &&
            record.LastCoordinationStop->SourceGroup == GroupId{ 1 } &&
            record.LastCoordinationStop->StartedAtMs == 1000 &&
            record.LastCoordinationStop->TargetEntry == 5000);
    }

    // Milestone 2.12G3D2A P2 fix, round 2 (STATIC review): a member whose
    // CURRENT ActiveActionState belongs to a different, unrelated goal
    // (here GetFood, e.g. already dispatched after this HUNT attempt
    // reached HuntPhase::AtTarget) must NOT have that unrelated action
    // touched at all when the HUNT ownership itself is stopped - only
    // ownsStoppedMovement's own exact match (SourceGoal+GoalStartedAtMs,
    // and for HUNT also Target) may ever gate an engine stop or an
    // ActiveActionState reset. This is the regression the ROLLED_BACK
    // diagnostic logs cannot cover on their own, since reaching them
    // requires a live Creature/Map/registry chain this smoke test has no
    // access to - this part is fully provable with synthetic values alone.
    {
        AgentRecord record;
        record.Id = AgentId{ 1 };

        GroupCoordinationGoal huntGoal;
        huntGoal.Type = GoalType::Hunt;
        huntGoal.SourceGroup = GroupId{ 1 };
        huntGoal.StartedAtMs = 1000;
        huntGoal.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(5000, 100);
        huntGoal.TargetEntry = 5000;
        huntGoal.TargetObservedAtMs = 500;
        huntGoal.Phase = HuntPhase::AtTarget;
        record.GroupCoordinationGoalState = huntGoal;

        ActiveAction unrelatedAction;
        unrelatedAction.Type = ActionType::MoveTo;
        unrelatedAction.SourceGoal = GoalType::GetFood;
        unrelatedAction.GoalStartedAtMs = 2000;
        unrelatedAction.StartedAtMs = 2000;
        record.ActiveActionState = unrelatedAction;

        StopInFlightGroupCoordination(record, "SMOKE_TEST_UNRELATED_ACTION_STOP", CoordinationStopReason::StoppedByTargetInvalid);

        check("stopping AtTarget HUNT ownership releases the HUNT goal even with an unrelated action present",
            !record.GroupCoordinationGoalState.has_value());
        check("the recorded stop event still carries the HUNT goal's own identity, not the unrelated action's",
            record.LastCoordinationStop && record.LastCoordinationStop->Reason == CoordinationStopReason::StoppedByTargetInvalid &&
            record.LastCoordinationStop->SourceGoal == GoalType::Hunt &&
            record.LastCoordinationStop->SourceGroup == GroupId{ 1 } &&
            record.LastCoordinationStop->StartedAtMs == 1000 &&
            record.LastCoordinationStop->TargetEntry == 5000);
        check("the unrelated GetFood ActiveActionState is left completely untouched",
            record.ActiveActionState.has_value() &&
            record.ActiveActionState->SourceGoal == GoalType::GetFood &&
            record.ActiveActionState->GoalStartedAtMs == 2000);
    }

    TC_LOG_INFO("ai.world", "AI HUNT arrival ownership smoke test {}", allPassed ? "PASSED" : "FAILED");
}

// Milestone 2.12F4A: manual proof only, gated behind AIWorld.TestControlMode
// (default 0/disabled) - runs at most once, from Initialize(), only when
// that config resolves to a real, already-registered AgentId (never a
// fake/ghost AgentRecord manufactured for the test). Proves the one thing
// that actually matters for this milestone: ActionSystem::Validate() is a
// mandatory, authoritative gate on AgentControlMode, not merely something
// every caller happens to check today. Deliberately pure - no live
// Creature/Map/grid access at all, since ActionValidationContext.ControlMode
// is a plain value and Validate() itself never touches TrinityCore; the
// same otherwise-fully-valid MOVE_TO ActionRequest/context (destination ==
// the context's own claimed position, so every OTHER ValidateMoveTo() check
// trivially passes regardless of ControlMode) is validated twice, with only
// context.ControlMode changed between the two calls - so a difference in
// outcome can only ever be attributed to the ControlMode gate itself, never
// to some other check incidentally also failing for ObserveOnly.
void AIWorldMgr::RunControlModeSmokeTest(AgentId testAgentId) const
{
    AgentRecord const* record = _registry.Find(testAgentId);
    if (!record)
    {
        TC_LOG_ERROR("ai.world", "AI ControlMode smoke FAILED: AIWorld.TestControlMode agent id={} does not exist in _registry - configure a real, already-registered AgentId",
            testAgentId.Value);
        return;
    }

    bool allPassed = true;
    auto check = [&allPassed](char const* name, bool condition)
    {
        if (condition)
            TC_LOG_INFO("ai.world", "AI ControlMode smoke check: {} PASSED", name);
        else
        {
            TC_LOG_ERROR("ai.world", "AI ControlMode smoke check: {} FAILED", name);
            allPassed = false;
        }
    };

    ActionRequest request;
    request.Actor = testAgentId;
    request.Type = ActionType::MoveTo;
    request.SourceGoal = GoalType::GetFood;
    request.GoalStartedAtMs = 1;

    ActionPosition destination;
    destination.MapId = record->MapId;
    destination.X = 0.0f;
    destination.Y = 0.0f;
    destination.Z = 0.0f;
    request.Destination = destination;

    ActionValidationContext context;
    context.Materialized = true;
    context.Alive = true;
    context.ActiveGoalType = GoalType::GetFood;
    context.ActiveGoalStartedAtMs = 1;
    context.MapId = record->MapId;
    context.X = 0.0f;
    context.Y = 0.0f;
    context.Z = 0.0f;
    context.HasActiveMovement = false;

    context.ControlMode = AgentControlMode::ObserveOnly;
    ActionValidationResult observeOnlyResult = _actionSystem.Validate(request, context);
    check("ObserveOnly context REJECTS an otherwise-valid MOVE_TO with ControlModeNotAllowed",
        !observeOnlyResult.Allowed && observeOnlyResult.Reason == ActionRejectReason::ControlModeNotAllowed);

    context.ControlMode = AgentControlMode::AIWorldControlled;
    ActionValidationResult controlledResult = _actionSystem.Validate(request, context);
    check("AIWorldControlled context proceeds past the ControlMode gate - the same otherwise-valid MOVE_TO is ALLOWED",
        controlledResult.Allowed && controlledResult.Reason == ActionRejectReason::None);

    TC_LOG_INFO("ai.world", "AI ControlMode smoke {}", allPassed ? "PASSED" : "FAILED");
}

void AIWorldMgr::RunSpawnReconciliation(uint32 zoneId)
{
    std::vector<CreatureSpawnIdentity> fullCensus = BuildCreatureSpawnCensus();

    // Milestone 2.12F4B2: scope the eligible census down to the configured
    // zone BEFORE it ever reaches BuildReconciliationPlan() - the 2.12F4B
    // engine itself (diff/reconcile) is otherwise completely unchanged.
    // Reads creature.zoneId directly (FetchCreatureSpawnIdsForZone(), its
    // own narrow WorldDatabase query) rather than computing zone/area at
    // runtime - see AIWorld_Current_Roadmap.md's own "2.12F4B2" section
    // for why (no live Map*/vmap dependency).
    std::unordered_set<uint64> zoneSpawnIds = FetchCreatureSpawnIdsForZone(zoneId);
    std::vector<CreatureSpawnIdentity> census;
    census.reserve(fullCensus.size());
    for (CreatureSpawnIdentity const& identity : fullCensus)
        if (zoneSpawnIds.find(identity.SpawnId) != zoneSpawnIds.end())
            census.push_back(identity);

    // Milestone 2.12F4B P2 fix (STATIC review): a SpawnId absent from the
    // eligible census may still name a real world.creature spawn that is
    // simply out of 2.12F4 scope (e.g. instance/raid) - allKnownSpawnIds
    // is needed to tell that apart from a spawn actually removed from the
    // DB, see BuildAllKnownCreatureSpawnIds()'s own comment. Milestone
    // 2.12F4B2: deliberately NOT zone-filtered - an existing agent outside
    // the configured zone must fall into OutOfScopeCount below, never
    // Orphaned, exactly the same way an out-of-2.12F4-scope instance spawn
    // already does.
    std::unordered_set<uint64> allKnownSpawnIds = BuildAllKnownCreatureSpawnIds();

    // Milestone 2.12F4B P2 fix (STATIC review): the diff's "what does
    // ai_agents PHYSICALLY hold" input is the raw table
    // (_persistence.LoadAllBindings()), NOT built from _registry. A row
    // AgentPersistence::LoadAgents() itself already quarantined (agent_id
    // != spawn_id, 2.12F4A2) never reached _registry - if this used
    // _registry here, its (map_id, spawn_id) binding would look Missing
    // and the resulting INSERT would collide with that quarantined row's
    // own UNIQUE (map_id, spawn_id) key. See SpawnReconciliationPlan.h's
    // own comment for the full reasoning.
    std::vector<AgentSpawnBinding> physicalBindings = _persistence.LoadAllBindings();
    SpawnReconciliationPlan plan = BuildReconciliationPlan(census, allKnownSpawnIds, physicalBindings);

    // ORPHANED: the world.creature spawn no longer exists or is no longer
    // eligible. CONFLICTED: the spawn is still eligible, but the row's own
    // stored MapId disagrees with what world.creature actually says - a
    // data inconsistency, never silently treated as valid. Both reconcile
    // the same way: fail-closed quarantine only, removed from _registry so
    // it can never run as a live agent again this process, but its
    // ai_agents row is deliberately left alone (no aggressive DELETE/auto-
    // repair): lifecycle/repair policy for either case isn't decided yet,
    // and an explicit quarantine state is safer than deleting or rewriting
    // persistent state (memory, economy, ...) a future policy might still
    // want.
    for (AgentId orphanId : plan.Orphaned)
    {
        AgentRecord const* record = _registry.Find(orphanId);
        if (record)
            TC_LOG_ERROR("ai.world", "AI agent id={} map={} spawn={} is orphaned (world.creature spawn no longer exists/eligible) - quarantined, removed from the registry this run (2.12F4B)",
                orphanId.Value, record->MapId, record->SpawnId);
        _registry.Remove(orphanId);
    }

    for (AgentId conflictId : plan.Conflicted)
    {
        AgentRecord const* record = _registry.Find(conflictId);
        if (record)
            TC_LOG_ERROR("ai.world", "AI agent id={} map={} spawn={} has a MapId conflicting with world.creature - quarantined, removed from the registry this run (2.12F4B)",
                conflictId.Value, record->MapId, record->SpawnId);
        _registry.Remove(conflictId);
    }

    // Milestone 2.12F4B P2 fix (STATIC review): a spawn whose intended new
    // AgentId (== SpawnId) already exists as a DIFFERENT physical row's
    // own agent_id - never inserted (would violate ai_agents' own PRIMARY
    // KEY), never added to AgentRegistry (there is nothing to add). Logged
    // loudly so an operator can resolve the conflicting historical row.
    for (CreatureSpawnIdentity const& blocked : plan.AgentIdCollisions)
        TC_LOG_ERROR("ai.world", "AI spawn reconciliation: map={} spawn={} entry={} cannot be created - its intended agent_id={} already belongs to a different ai_agents row (2.12F4B)",
            blocked.MapId, blocked.SpawnId, blocked.Entry, blocked.SpawnId);

    // MISSING: create as ObserveOnly, never AIWorldControlled - this bulk
    // bootstrap must never mass-grant ownership (see AIWorld_Current_
    // Roadmap.md's own 2.12F4B section). CreateCreatureAgentsBatch() only
    // returns entries it actually confirmed via read-back; anything not
    // confirmed is simply not added here and reappears as MISSING on the
    // next reconciliation.
    std::vector<PendingCreatureAgent> created = _persistence.CreateCreatureAgentsBatch(plan.Missing);
    uint32 addedCount = 0;
    for (PendingCreatureAgent const& pending : created)
    {
        AgentRecord record;
        record.Id = AgentId{ pending.SpawnId };
        record.Type = pending.Type;
        record.MapId = pending.MapId;
        record.SpawnId = pending.SpawnId;
        record.WorldState = AgentWorldState::Abstract;
        record.ControlMode = AgentControlMode::ObserveOnly;

        if (_registry.Add(record))
            ++addedCount;
    }

    // Milestone 2.12F4B2: zoneScope/rawZoneSpawns logged alongside the
    // eligible census size - the eligible count is expected to differ
    // from the raw WHERE zoneId=? count (further filtered by persistent/
    // non-instance/valid-MapEntry eligibility, see AIWorld_Current_
    // Roadmap.md's own "2.12F4B2" section), and both numbers together are
    // what makes that difference auditable instead of a single opaque
    // figure.
    TC_LOG_INFO("ai.world", "AI spawn reconciliation: zoneScope={} rawZoneSpawns={} census={} valid={} missing={} created={} orphaned={} conflicted={} quarantined={} outOfScope={} agentIdCollisions={}",
        zoneId, zoneSpawnIds.size(), census.size(), plan.ValidCount, plan.Missing.size(), addedCount, plan.Orphaned.size(), plan.Conflicted.size(),
        plan.QuarantinedCount, plan.OutOfScopeCount, plan.AgentIdCollisions.size());
}

void AIWorldMgr::RunZoneControlActivation(uint32 zoneId)
{
    // Milestone 2.12F4B3: recomputes the same scoped identity set
    // RunSpawnReconciliation() itself would for this zoneId, independent
    // of whether reconciliation ran this same startup - see this
    // method's own header comment.
    std::vector<CreatureSpawnIdentity> fullCensus = BuildCreatureSpawnCensus();
    std::unordered_set<uint64> zoneSpawnIds = FetchCreatureSpawnIdsForZone(zoneId);

    std::vector<AgentId> candidates;
    uint32 scopedEligibleCount = 0;
    uint32 alreadyControlled = 0;
    uint32 notYetReconciled = 0;
    for (CreatureSpawnIdentity const& identity : fullCensus)
    {
        if (zoneSpawnIds.find(identity.SpawnId) == zoneSpawnIds.end())
            continue;

        ++scopedEligibleCount;

        // AgentId == SpawnId is already guaranteed for anything
        // AgentRegistry holds (LoadAgents()/reconciliation both fail
        // closed on a mismatch before adding), so looking up by
        // AgentId{identity.SpawnId} is exactly "does this scoped-eligible
        // spawn already have a real AgentRecord". The MapId re-check is
        // defensive: if 2.12F4B2 reconciliation has not run THIS startup,
        // a Conflicted row (real spawn, wrong stored MapId) could still
        // be sitting in _registry unquarantined - never promote that.
        AgentRecord* record = _registry.Find(AgentId{ identity.SpawnId });
        if (!record || record->MapId != identity.MapId)
        {
            ++notYetReconciled;
            continue;
        }

        if (record->ControlMode == AgentControlMode::AIWorldControlled)
        {
            ++alreadyControlled;
            continue;
        }

        candidates.push_back(record->Id);
    }

    // Milestone 2.12F4B3 P2 fix (STATIC review): fail-closed whole-zone
    // activation, not partial. Promoting `candidates` while some other
    // scoped-eligible spawn is still notYetReconciled would leave the
    // zone in a mixed Controlled/ObserveOnly state and the server would
    // carry on as if nothing were wrong - exactly the outcome this
    // milestone's own target state ("Elwynn = 3540 Controlled, 0
    // ObserveOnly") rules out. Verify the WHOLE scoped set resolved to a
    // real AgentRecord first; only then attempt any promotion at all.
    if (notYetReconciled != 0)
    {
        TC_LOG_ERROR("ai.world", "AI zone control activation REFUSED: zone={} has {} scoped-eligible spawn(s) with no valid AgentRecord yet (run 2.12F4B reconciliation for this zone first) - promoting the rest would leave a partially-Controlled zone",
            zoneId, notYetReconciled);
        return;
    }

    // Only entries PromoteControlModeBatch() itself confirms via read-back
    // are reflected here - never optimistically mark a candidate promoted
    // just because it was requested. Unconfirmed candidates simply remain
    // ObserveOnly and can be retried by a later run of this same method.
    std::vector<AgentId> promoted = _persistence.PromoteControlModeBatch(candidates);
    for (AgentId const& id : promoted)
    {
        AgentRecord* record = _registry.Find(id);
        if (record)
            record->ControlMode = AgentControlMode::AIWorldControlled;
    }

    TC_LOG_INFO("ai.world", "AI zone control activation: zone={} rawZoneSpawns={} eligible={} alreadyControlled={} candidates={} promoted={}",
        zoneId, zoneSpawnIds.size(), scopedEligibleCount, alreadyControlled, candidates.size(), promoted.size());
}

std::vector<CoalitionMemberObservation> AIWorldMgr::CollectCoalitionMemberObservations(AgentGroupRecord const& group) const
{
    std::vector<CoalitionMemberObservation> observations;
    observations.reserve(group.Members.size());

    for (AgentGroupMembership const& membership : group.Members)
    {
        CoalitionMemberObservation observation;
        observation.MemberId = membership.Member;

        AgentRecord const* record = _registry.Find(membership.Member);
        if (!record || record->WorldState != AgentWorldState::Materialized)
        {
            observations.push_back(observation);
            continue;
        }

        Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
        Creature* creature = ResolveLiveCreature(*record, map);
        if (!creature)
        {
            observations.push_back(observation);
            continue;
        }

        observation.Materialized = true;
        observation.Alive = creature->IsAlive();
        observation.MapId = creature->GetMapId();
        observation.X = creature->GetPositionX();
        observation.Y = creature->GetPositionY();
        observation.Z = creature->GetPositionZ();
        observations.push_back(observation);
    }

    return observations;
}

std::vector<HuntTargetObservation> AIWorldMgr::CollectHuntTargetObservations(AgentGroupRecord const& group, uint64 nowMs) const
{
    std::vector<HuntTargetObservation> observations;

    for (AgentGroupMembership const& membership : group.Members)
    {
        std::vector<MemoryRecord> records = _shortTermMemory.GetActiveForAgent(membership.Member, nowMs);

        for (MemoryRecord const& memory : records)
        {
            if (memory.Type != ObservationType::CreatureSeen)
                continue;

            // 2.12G3C2 P3 fix (STATIC review): the record's own claimed
            // Owner, not the membership.Member loop variable used to fetch
            // it - provenance must not be reassigned in the conversion;
            // HuntIntentSystem::Evaluate() itself is what verifies this
            // Observer actually names a real, materialized/alive/same-map
            // member of the group.
            HuntTargetObservation observation;
            observation.Observer = memory.Owner;
            observation.Target.TargetGuid = memory.Target.Guid;
            observation.Target.TargetEntry = memory.Target.Entry;
            observation.Target.MapId = memory.Location.MapId;
            observation.Target.X = memory.Location.X;
            observation.Target.Y = memory.Location.Y;
            observation.Target.Z = memory.Location.Z;
            // PerceptionSystem::ObserveNearbyCreature() only ever produces
            // a CreatureSeen Observation for a target alive AT OBSERVATION
            // TIME - see this method's own declaration comment for why
            // that is an honest fact about this record, not an assumption
            // it still holds now.
            observation.Target.Alive = true;
            observation.Target.ObservedAtMs = memory.LastObservedAtMs;
            observation.Distance = memory.LastDistance;
            observation.LineOfSight = memory.LastLineOfSight;
            observations.push_back(observation);
        }
    }

    return observations;
}

std::optional<CoalitionMaintenanceProfile> AIWorldMgr::ResolveMaintenanceProfile(CoalitionFormationProfileId profileId) const
{
    if (profileId == CoalitionFormationProfileId::WolfLoose)
        return _wolfLooseMaintenanceProfile;

    // Milestone 2.12G1: the second real profile - just another case, no
    // change to RunCoalitionMaintenance()'s own discovery/admission shape.
    if (profileId == CoalitionFormationProfileId::DefiasLoose)
        return _defiasLooseMaintenanceProfile;

    return std::nullopt;
}

void AIWorldMgr::RunCoalitionMaintenance()
{
    uint64 nowMs = CurrentTimeMs();

    // Milestone 2.12E4C2 P3 fix (STATIC review): bounded discovery, not
    // O(all groups) - see RunCoalitionMaintenance()'s own header comment
    // and AgentGroupRegistry::GetGroupsAfterUntil(). An earlier version
    // called _groupRegistry.GetGroups() here, materializing every
    // registered group's own GroupId every pass regardless of how few (if
    // any) were ever actually candidates.
    //
    // Milestone 2.12E4C2 P2 fix, round 2 (STATIC review): ONE global scan,
    // not one per profile - see this method's own header comment and
    // _maintenanceScanCursor's own declaration comment for the cross-
    // profile starvation a per-profile cursor would otherwise cause.
    //
    // Milestone 2.12E4C2 P2 fix, round 3 (STATIC review): a scan CYCLE
    // begins whenever the cursor is at GroupId{} - takes a fresh
    // _maintenanceScanCycleHighWater snapshot right then, so this cycle
    // only ever scans groups that already existed at that moment. Without
    // this, a registry growing faster than the scan can keep up (new
    // GroupIds always appearing ahead of the cursor, since GroupId is
    // monotonic) would mean the scan never reaches empty, never wraps,
    // and the earliest-created groups starve indefinitely - see
    // GetGroupsAfterUntil()'s own comment.
    if (!_maintenanceScanCursor)
        _maintenanceScanCycleHighWater = _groupRegistry.GetHighestGroupId();

    std::vector<GroupId> discovered = _groupRegistry.GetGroupsAfterUntil(
        _maintenanceScanCursor, _maintenanceScanCycleHighWater, _coalitionMaintenanceScanMaxPerPass);

    if (discovered.empty() && _maintenanceScanCursor)
    {
        // Reached the end of the CURRENT cycle's own high-water mark -
        // start a fresh cycle (new snapshot, taken from whatever the
        // registry actually holds right now - which may include groups
        // created during the cycle that just ended, correctly deferred to
        // this new one) and retry once, the same "don't waste a whole
        // pass on an empty result right after starting a new cycle"
        // reasoning the earlier version already had for its own wrap.
        _maintenanceScanCursor = GroupId{};
        _maintenanceScanCycleHighWater = _groupRegistry.GetHighestGroupId();
        discovered = _groupRegistry.GetGroupsAfterUntil(
            _maintenanceScanCursor, _maintenanceScanCycleHighWater, _coalitionMaintenanceScanMaxPerPass);
    }

    if (!discovered.empty())
        _maintenanceScanCursor = discovered.back();

    // Milestone 2.12E4C2 P2 fix, round 2 (STATIC review): each discovered
    // group resolves its OWN maintenance profile from its own persistent
    // ProfileId (ResolveMaintenanceProfile()) - not a check against one
    // fixed profile passed in for the whole pass, since this pass's own
    // discovered batch can mix groups governed by different profiles once
    // more than one exists. A group whose ProfileId does not resolve
    // (Invalid, or a profile this process has no maintenance rules
    // configured for) is simply not a maintenance candidate this pass -
    // the same "never silently sweep in a manually/admin-created group"
    // guarantee the old per-profile ProfileId check already gave, just
    // expressed as "did this resolve at all" instead of "does this equal
    // the one profile this call was given".
    std::vector<GroupId> candidates;
    std::unordered_map<uint64, CoalitionMaintenanceProfile> candidateProfiles;
    for (GroupId groupId : discovered)
    {
        AgentGroupRecord const* group = _groupRegistry.Find(groupId);
        if (!group)
            continue;

        std::optional<CoalitionMaintenanceProfile> profile = ResolveMaintenanceProfile(group->ProfileId);
        if (!profile)
            continue;

        candidates.push_back(groupId);
        candidateProfiles.emplace(groupId.Value, *profile);
    }

    // Same phase-offset-on-first-sight pattern RunDecisionScheduler()'s own
    // group coarse tick already uses (StableAgentHash, see that loop's own
    // comment) - a batch of groups all existing on the same pass does not
    // permanently phase-lock onto the same due time forever afterwards.
    for (GroupId groupId : candidates)
    {
        SimulationScheduleState& scheduleState = _maintenanceSchedule[groupId.Value];
        if (scheduleState.LastTickAtMs == 0 && scheduleState.NextTickAtMs == 0)
        {
            uint64 phaseMs = StableAgentHash(groupId.Value) % _coalitionMaintenanceIntervalMs;
            scheduleState.LastTickAtMs = nowMs;
            scheduleState.NextTickAtMs = nowMs + phaseMs;
        }
    }

    // Bounded globally by AIWorld.CoalitionMaintenanceMaxPerPass, across
    // every profile mixed together in candidates - not per profile.
    GroupCoarseSimulationScheduler::SelectionResult selection = _maintenanceScheduler.SelectDue(
        _maintenanceSchedule, candidates, nowMs, _coalitionMaintenanceMaxPerPass);

    for (GroupId groupId : selection.Admitted)
    {
        SimulationScheduleState& scheduleState = _maintenanceSchedule[groupId.Value];
        scheduleState.LastTickAtMs = nowMs;
        scheduleState.NextTickAtMs = nowMs + _coalitionMaintenanceIntervalMs;

        RunCoalitionMaintenanceForGroup(groupId, candidateProfiles.at(groupId.Value));
    }
}

void AIWorldMgr::RunCoalitionMaintenanceForGroup(GroupId groupId, CoalitionMaintenanceProfile const& profile)
{
    if (_maintenanceInFlight.count(groupId.Value))
        return;

    AgentGroupRecord const* group = _groupRegistry.Find(groupId);
    if (!group)
        return;

    std::vector<CoalitionMemberObservation> observations = CollectCoalitionMemberObservations(*group);
    CoalitionMaintenanceDecision decision = _coalitionMaintenanceSystem.Evaluate(*group, profile, observations);

    switch (decision.Type)
    {
        case CoalitionMaintenanceDecisionType::LeaveMember:
        {
            _maintenanceInFlight.insert(groupId.Value);

            TC_LOG_INFO("ai.world", "AI coalition maintenance: proposing AutomaticPolicy leave member={} group={}",
                decision.Member.Value, groupId.Value);

            CoalitionMaintenanceProfile profileValue = profile;
            RequestLeaveGroupWithPolicy(groupId, decision.Member, AgentGroupOperationSource::AutomaticPolicy,
                [this, groupId, profileValue](bool success, AgentGroupPolicyDecision policyDecision)
                {
                    if (!success)
                    {
                        TC_LOG_INFO("ai.world", "AI coalition maintenance: leave REJECTED/FAILED (decision={}) group={}",
                            ToString(policyDecision), groupId.Value);
                        _maintenanceInFlight.erase(groupId.Value);
                        return;
                    }

                    TC_LOG_INFO("ai.world", "AI coalition maintenance: leave PASSED group={}", groupId.Value);
                    RunCoalitionMaintenanceAfterLeave(groupId, profileValue);
                });
            break;
        }
        case CoalitionMaintenanceDecisionType::DissolveGroup:
            _maintenanceInFlight.insert(groupId.Value);
            TC_LOG_INFO("ai.world", "AI coalition maintenance: group={} below minimum, requesting AutomaticPolicy dissolve", groupId.Value);
            RunCoalitionMaintenanceDissolveGroup(groupId);
            break;
        case CoalitionMaintenanceDecisionType::None:
        default:
            break;
    }
}

void AIWorldMgr::RunCoalitionMaintenanceAfterLeave(GroupId groupId, CoalitionMaintenanceProfile profile)
{
    // 2.12E4C2: never trusts the AgentGroupRecord* RunCoalitionMaintenanceForGroup()
    // itself resolved before this leave was even submitted - re-resolves
    // fresh here, the same "a completion never trusts request-time
    // validity" discipline AgentGroupLifecycleSystem.h's own header
    // comment already documents.
    AgentGroupRecord const* current = _groupRegistry.Find(groupId);
    if (!current)
    {
        _maintenanceInFlight.erase(groupId.Value);
        return;
    }

    if (current->Kind == AgentGroupKind::Loose && uint32(current->Members.size()) < profile.MinMembers)
    {
        TC_LOG_INFO("ai.world", "AI coalition maintenance: group={} dropped below minimum after leave, requesting AutomaticPolicy dissolve", groupId.Value);
        RunCoalitionMaintenanceDissolveGroup(groupId);
        return;
    }

    _maintenanceInFlight.erase(groupId.Value);
}

void AIWorldMgr::RunCoalitionMaintenanceDissolveGroup(GroupId groupId)
{
    RequestDissolveGroupWithPolicy(groupId, AgentGroupOperationSource::AutomaticPolicy,
        [this, groupId](bool success)
        {
            if (success)
                TC_LOG_INFO("ai.world", "AI coalition maintenance: dissolve PASSED group={}", groupId.Value);
            else
                TC_LOG_INFO("ai.world", "AI coalition maintenance: dissolve REJECTED/FAILED group={}", groupId.Value);

            _maintenanceInFlight.erase(groupId.Value);
        });
}

std::optional<AgentGroupCoordinationProfile> AIWorldMgr::ResolveCoordinationProfile(CoalitionFormationProfileId profileId) const
{
    if (profileId == CoalitionFormationProfileId::WolfLoose)
        return _wolfLooseCoordinationProfile;

    // Milestone 2.12G1: the second real profile - just another case, the
    // same "profile identity is data" discipline ResolveMaintenanceProfile()
    // already established.
    if (profileId == CoalitionFormationProfileId::DefiasLoose)
        return _defiasLooseCoordinationProfile;

    return std::nullopt;
}

bool AIWorldMgr::HasInFlightHuntAttempt(AgentGroupRecord const& group) const
{
    for (AgentGroupMembership const& membership : group.Members)
    {
        AgentRecord const* member = _registry.Find(membership.Member);
        if (!member || !member->GroupCoordinationGoalState)
            continue;

        if (member->GroupCoordinationGoalState->Type == GoalType::Hunt
            && member->GroupCoordinationGoalState->SourceGroup == group.Id)
            return true;
    }

    return false;
}

std::optional<HuntIntent> AIWorldMgr::ResolveHuntIntentForGroup(AgentGroupRecord const& group, AgentGroupCoordinationProfile const& profile,
    std::vector<CoalitionMemberObservation> const& observations, uint64 nowMs) const
{
    // Reconstruct in-flight HUNT identity purely from members' own
    // GroupCoordinationGoalState - see this method's own header comment
    // for why no separate manager-level pin exists.
    std::optional<GroupCoordinationGoal> pinnedGoal;
    bool ambiguous = false;

    for (AgentGroupMembership const& membership : group.Members)
    {
        AgentRecord const* member = _registry.Find(membership.Member);
        if (!member || !member->GroupCoordinationGoalState)
            continue;

        GroupCoordinationGoal const& goal = *member->GroupCoordinationGoalState;
        if (goal.Type != GoalType::Hunt || goal.SourceGroup != group.Id)
            continue;

        if (ambiguous)
            continue;

        if (!pinnedGoal)
        {
            pinnedGoal = goal;
            continue;
        }

        if (pinnedGoal->TargetGuid != goal.TargetGuid || pinnedGoal->TargetEntry != goal.TargetEntry ||
            pinnedGoal->StartedAtMs != goal.StartedAtMs)
        {
            ambiguous = true;
            pinnedGoal.reset();
        }
    }

    if (ambiguous)
    {
        TC_LOG_ERROR("ai.world", "AI HUNT coordination: group={} members disagree on their own in-flight HUNT attempt identity - ambiguous, dispatching nothing new this pass",
            group.Id.Value);
        return std::nullopt;
    }

    std::vector<HuntTargetObservation> huntObservations = CollectHuntTargetObservations(group, nowMs);

    if (pinnedGoal)
    {
        // Milestone 2.12G3C2 P2 fix (STATIC review): an in-flight attempt
        // already exists - the SAME target/attempt identity must be
        // reused, never a freshly-selected one, while it is still active.
        // An earlier version hand-picked the freshest observation naming
        // this exact TargetGuid purely by GUID+timestamp, which bypassed
        // every OTHER fail-closed rule HuntIntentSystem::Evaluate() itself
        // enforces - staleness against HuntObservationMaxAgeMs, observer
        // alive/materialized/same-map, LOS/acquisition-distance, profile
        // entry eligibility, and conflicting-duplicate detection. Filter
        // huntObservations down to ONLY this pinned TargetGuid and send
        // that filtered set through the SAME Evaluate() every fresh
        // selection already goes through - if this target no longer
        // passes even one of those rules, Evaluate() itself now correctly
        // returns nullopt, exactly the same fail-closed outcome a fresh
        // selection would give it. Only the resulting StartedAtMs is
        // overridden with the pinned attempt's own stable identity - never
        // Evaluate()'s own nowMs - so every member dispatched into this
        // same ongoing attempt keeps a consistent one.
        std::vector<HuntTargetObservation> pinnedObservations;
        for (HuntTargetObservation const& observation : huntObservations)
            if (observation.Target.TargetGuid == pinnedGoal->TargetGuid)
                pinnedObservations.push_back(observation);

        std::optional<HuntIntent> reevaluated = _huntIntentSystem.Evaluate(group, profile, observations, pinnedObservations, nowMs);
        if (!reevaluated)
            return std::nullopt;

        reevaluated->StartedAtMs = pinnedGoal->StartedAtMs;
        return reevaluated;
    }

    return _huntIntentSystem.Evaluate(group, profile, observations, huntObservations, nowMs);
}

void AIWorldMgr::ReconcileActiveHuntTargetsForGroup(AgentGroupRecord const& group)
{
    for (AgentGroupMembership const& membership : group.Members)
    {
        AgentRecord* member = _registry.Find(membership.Member);
        if (!member || !member->GroupCoordinationGoalState)
            continue;

        GroupCoordinationGoal const& goal = *member->GroupCoordinationGoalState;
        if (goal.Type != GoalType::Hunt || goal.SourceGroup != group.Id)
            continue;

        // Nothing here can be honestly checked one way or the other for a
        // member with no live Creature - left untouched, never invalidated
        // just because it currently cannot be verified (the same
        // "cannot resolve, cannot claim" discipline
        // StopInFlightGroupCoordination() itself already holds for its own
        // engine-generator facts). Existing dematerialization handling
        // elsewhere governs this member's own state independent of HUNT.
        if (member->WorldState != AgentWorldState::Materialized)
            continue;

        Map* map = sMapMgr->FindBaseNonInstanceMap(member->MapId);
        Creature* creature = ResolveLiveCreature(*member, map);
        if (!creature)
            continue;

        // Transient, already-loaded-object lookup only - never a
        // force-load (ObjectAccessor::GetCreature() -> Map::GetCreature()).
        Creature* target = ObjectAccessor::GetCreature(*creature, goal.TargetGuid);

        bool targetValid = target && target->IsAlive() && target->GetMapId() == creature->GetMapId() &&
            target->GetEntry() == goal.TargetEntry && creature->IsValidAttackTarget(target);

        if (!targetValid)
        {
            TC_LOG_DEBUG("ai.world", "AI HUNT coordination: member={} group={} target guid={} entry={} is no longer valid - stopping in-flight HUNT approach",
                member->Id.Value, group.Id.Value, goal.TargetGuid.GetRawValue(), goal.TargetEntry);
            StopInFlightGroupCoordination(*member, "COORDINATION_STOPPED_BY_TARGET_INVALID", CoordinationStopReason::StoppedByTargetInvalid);
        }
    }
}

void AIWorldMgr::RunCoalitionCoordination()
{
    // Same two-stage bounded-discovery shape RunCoalitionMaintenance()
    // already established, with its own dedicated cursor pair - see
    // _coordinationScanCursor/_coordinationScanCycleHighWater's own
    // declaration comments.
    if (!_coordinationScanCursor)
        _coordinationScanCycleHighWater = _groupRegistry.GetHighestGroupId();

    std::vector<GroupId> discovered = _groupRegistry.GetGroupsAfterUntil(
        _coordinationScanCursor, _coordinationScanCycleHighWater, _groupCoordinationScanMaxPerPass);

    if (discovered.empty() && _coordinationScanCursor)
    {
        _coordinationScanCursor = GroupId{};
        _coordinationScanCycleHighWater = _groupRegistry.GetHighestGroupId();
        discovered = _groupRegistry.GetGroupsAfterUntil(
            _coordinationScanCursor, _coordinationScanCycleHighWater, _groupCoordinationScanMaxPerPass);
    }

    if (!discovered.empty())
        _coordinationScanCursor = discovered.back();

    // No second bounded admission tier - see RunCoalitionCoordination()'s
    // own header comment for why a dispatched MOVE_TO proposal (cheap,
    // synchronous, in-memory) needs no GroupCoarseSimulationScheduler the
    // way an async DB-writing maintenance action does.
    //
    // 2.12F2 P2 fix (STATIC review): every group's own proposals are
    // collected into one combined batch first - not dispatched immediately
    // per group - purely so the overlap check below runs once per proposal
    // in a single pass over `proposals`, not interleaved with dispatch.
    std::vector<GroupMemberActionProposal> proposals;

    // Milestone 2.12G3C2: HUNT's own combined batch, entirely separate
    // from `proposals` above - HuntProposal is a different type (carries a
    // full HuntTargetProvenance, not a flat MapId/X/Y/Z point) and gets
    // its own overlap-arbitration/dispatch loop below, sharing
    // CountCoordinationEnabledMemberships()'s own definition of
    // "coordination-ambiguous" with the Regroup/Roam batch.
    std::vector<HuntProposal> huntProposals;

    // Milestone 2.12G2: one shared `nowMs` for this whole pass, not a
    // fresh CurrentTimeMs() per group - AgentGroupIntentSystem::Evaluate()'s
    // own deterministic ROAM phase bucket (nowMs / profile.RoamIntervalMs)
    // must not be able to land on two different buckets for two groups
    // discovered in the same RunCoalitionCoordination() call purely
    // because evaluating them took a few milliseconds.
    uint64 nowMs = CurrentTimeMs();

    for (GroupId groupId : discovered)
    {
        // 2.12F2 P2 fix (STATIC review): a group with a Join/Leave/Dissolve
        // still in flight is skipped entirely this pass - see this method's
        // own header comment for the stale-membership race this closes.
        if (_groupLifecycleSystem.HasPendingOperation(groupId))
            continue;

        AgentGroupRecord const* group = _groupRegistry.Find(groupId);
        if (!group)
        {
            // Defense in depth only - in normal operation a dissolved
            // GroupId is removed from _groupRegistry entirely (see
            // AgentGroupRegistry::Remove()) and can therefore never be
            // discovered here again in the first place, so
            // RequestDissolveGroup()'s own confirmed-dissolve completion
            // is the actual, always-reached place this same erase happens
            // (see its own comment for why). Kept here too purely so this
            // map can never accumulate a stale entry through some future
            // path that reaches "discovered, but no longer resolves"
            // without going through that completion.
            _roamAttemptPinnedNowMs.erase(groupId.Value);
            continue;
        }

        std::optional<AgentGroupCoordinationProfile> profile = ResolveCoordinationProfile(group->ProfileId);
        if (!profile)
            continue;

        // Milestone 2.12G3C2: reconcile every member's own in-flight HUNT
        // attempt against LIVE target reality before this pass decides
        // anything new - see ReconcileActiveHuntTargetsForGroup()'s own
        // comment. Unconditional, not gated on profile->HuntEnabled: an
        // attempt already in flight must still be honestly reconciled
        // even if this profile's own config no longer enables HUNT.
        ReconcileActiveHuntTargetsForGroup(*group);

        // Milestone 2.12G2 P2 fix (STATIC review): a ROAM target is a
        // pure function of nowMs (via nowMs / profile.RoamIntervalMs in
        // AgentGroupIntentSystem::Evaluate()) - without pinning, a phase
        // bucket boundary crossing WHILE this group's own previous ROAM
        // attempt is still in flight for some of its members would let
        // one group coordinate different members toward two different
        // targets at once: e.g. member 1 dispatched toward phase P's
        // target, arrives; member 2 is still mid-move toward that same
        // phase-P target when nowMs ticks into phase P+1; the next pass
        // would otherwise evaluate member 2's own group fresh against
        // P+1's target while member 1 (now idle) also gets re-evaluated
        // against P+1 - two members of the same AgentGroup now converging
        // on different points, one old, one new. Pinning nowMs to
        // whatever it was on the pass THIS group's still-in-flight attempt
        // was first evaluated with keeps every member converging on the
        // exact same target until the whole attempt concludes (see
        // _roamAttemptPinnedNowMs's own declaration comment).
        uint64 effectiveNowMs = nowMs;
        if (HasInFlightRoamAttempt(*group))
        {
            auto pinnedIt = _roamAttemptPinnedNowMs.find(groupId.Value);
            if (pinnedIt != _roamAttemptPinnedNowMs.end())
                effectiveNowMs = pinnedIt->second;
        }
        else
        {
            // No member of this group currently has an in-flight ROAM
            // attempt - any pin left over from a just-concluded one is
            // stale and must not linger, or the group would be stuck
            // reusing an old phase's target forever.
            _roamAttemptPinnedNowMs.erase(groupId.Value);
        }

        std::vector<CoalitionMemberObservation> observations = CollectCoalitionMemberObservations(*group);
        AgentGroupIntent intent = _agentGroupIntentSystem.Evaluate(*group, *profile, observations, effectiveNowMs);

        // Milestone 2.12G3C2: REGROUP always outranks everything else,
        // including an already-active HUNT - a dispersed group must never
        // be left mid-hunt while it is also trying to pull itself back
        // together, the same reasoning REGROUP already outranked ROAM for
        // (see AgentGroupIntentSystem.h's own comment).
        //
        // Milestone 2.12G3C2 P2 fix (STATIC review): outranking on PAPER is
        // not enough - DispatchGroupMemberActionProposal() itself refuses
        // any member that already has an ActiveActionState, so a REGROUP
        // proposal for a member still mid-HUNT would previously just be
        // silently dropped there, leaving the HUNT running unopposed
        // despite REGROUP's own declared priority. Stop any in-flight HUNT
        // for THIS group's own members FIRST - clearing both
        // ActiveActionState and GroupCoordinationGoalState via the same
        // StopInFlightGroupCoordination() every other production stop path
        // already uses - so the REGROUP proposal about to be dispatched
        // below actually finds an idle member, not a busy one.
        if (intent.Type == AgentGroupIntentType::Regroup)
        {
            for (AgentGroupMembership const& membership : group->Members)
            {
                AgentRecord* member = _registry.Find(membership.Member);
                if (!member || !member->GroupCoordinationGoalState)
                    continue;

                if (member->GroupCoordinationGoalState->Type != GoalType::Hunt || member->GroupCoordinationGoalState->SourceGroup != group->Id)
                    continue;

                StopInFlightGroupCoordination(*member, "COORDINATION_STOPPED_BY_REGROUP_PREEMPTION", CoordinationStopReason::PreemptedByRegroup);
            }

            std::vector<GroupMemberActionProposal> groupProposals = _agentGroupIntentProjector.Project(intent, *profile, observations);
            proposals.insert(proposals.end(), groupProposals.begin(), groupProposals.end());
            continue;
        }

        // Milestone 2.12G3C2: HUNT outranks ROAM - an already in-flight
        // HUNT attempt (reconstructed purely from members' own
        // GroupCoordinationGoalState by ResolveHuntIntentForGroup()/
        // HasInFlightHuntAttempt(), never a separate manager-level pin)
        // keeps owning this group's action slot even on a pass where it
        // has nothing NEW to propose, so a still-settling ROAM phase can
        // never steal it back mid-hunt. Checked regardless of whether
        // intent.Type above came back Roam or None - HUNT selection is
        // entirely independent of AgentGroupIntentSystem's own Regroup/
        // Roam rules.
        bool huntInFlight = profile->HuntEnabled && HasInFlightHuntAttempt(*group);
        std::optional<HuntIntent> huntIntent;
        if (profile->HuntEnabled)
            huntIntent = ResolveHuntIntentForGroup(*group, *profile, observations, nowMs);

        // Milestone 2.12G3D1 diagnostic-only addition (STATIC review):
        // traces the INTENT/PROJECT pipeline stages for the ONE watched
        // agent - never changes state, selection, or dispatch, and never
        // sets _testObserveActiveHuntFired (only CheckTestObserveActiveHunt()
        // does that, on a genuine full PASS). Gated on this group actually
        // containing the watched agent, since HuntIntent itself is a
        // group-level fact, not a per-member one.
        bool huntDiagnosticsWatchingThisGroup = _testObserveActiveHuntAgentId && !_testObserveActiveHuntFired &&
            std::any_of(group->Members.begin(), group->Members.end(),
                [this](AgentGroupMembership const& membership) { return membership.Member == _testObserveActiveHuntAgentId; });

        if (huntDiagnosticsWatchingThisGroup)
            TC_LOG_INFO("ai.world", "AI HUNT approach runtime diagnostic: agent={} group={} stage=INTENT result={}",
                _testObserveActiveHuntAgentId.Value, group->Id.Value, huntIntent ? "SELECTED" : "NONE");

        if (huntIntent)
        {
            std::vector<HuntProposal> groupHuntProposals = _huntIntentProjector.Project(*huntIntent, observations);
            huntProposals.insert(huntProposals.end(), groupHuntProposals.begin(), groupHuntProposals.end());

            if (huntDiagnosticsWatchingThisGroup)
            {
                bool proposedForWatchedAgent = std::any_of(groupHuntProposals.begin(), groupHuntProposals.end(),
                    [this](HuntProposal const& huntProposal) { return huntProposal.Member == _testObserveActiveHuntAgentId; });
                TC_LOG_INFO("ai.world", "AI HUNT approach runtime diagnostic: agent={} group={} stage=PROJECT result={}",
                    _testObserveActiveHuntAgentId.Value, group->Id.Value, proposedForWatchedAgent ? "PROPOSED" : "NO_PROPOSAL");
            }
        }

        if (huntInFlight || huntIntent)
            continue;

        // Neither REGROUP nor HUNT claimed this group's action slot this
        // pass - ROAM's own existing 2.12G2 rules apply unchanged.
        if (intent.Type != AgentGroupIntentType::Roam)
            continue;

        // Milestone 2.12G2 P2 fix (STATIC review): record the nowMs this
        // ROAM intent was actually produced with, so a FUTURE pass -
        // once this attempt is genuinely in flight (HasInFlightRoamAttempt()
        // will see the GroupCoordinationGoalState DispatchGroupMemberActionProposal()
        // is about to set below) - pins to this exact value instead of
        // silently drifting to a later phase mid-attempt. try_emplace, not
        // an unconditional overwrite: if a pin already exists for this
        // group (this call itself used it as effectiveNowMs above), it
        // must not be clobbered with a slightly-later value from this
        // same pass.
        _roamAttemptPinnedNowMs.try_emplace(groupId.Value, effectiveNowMs);

        std::vector<GroupMemberActionProposal> groupProposals = _agentGroupIntentProjector.Project(intent, *profile, observations);
        proposals.insert(proposals.end(), groupProposals.begin(), groupProposals.end());
    }

    // 2.12F2 P2 fix, round 2 (STATIC review): arbitrated against how many
    // coordination-enabled groups (2.12G2: RegroupEnabled OR RoamEnabled -
    // see CountCoordinationEnabledMemberships()'s own comment for why this
    // must not stay scoped to RegroupEnabled alone) this specific
    // proposal's own Member currently belongs to (via AgentGroupRegistry::
    // GetGroupsOfMember(), 2.12F2 P3 fix, round 2 - see that method's own
    // comment), not against how many proposals this pass's own bounded
    // discovery batch happened to produce. A member who structurally
    // belongs to more than one coordination-enabled group gets no proposal
    // dispatched at all, even if only one of those groups actually
    // proposed something this exact pass (the other could easily propose
    // next pass instead - the conservative MVP rule this method's own
    // header comment describes is about membership overlap, not about
    // which passes happen to collide).
    //
    // 2.12F2 P3 fix, round 2 (STATIC review): GetGroupsOfMember() is O(k)
    // where k is however many groups THIS proposal's own Member is in
    // (almost always 0 or 1) - an earlier version of this fix built a
    // coordinationMembershipCount map with one upfront O(all groups + all
    // memberships) pass over the WHOLE registry every single call, via
    // AgentGroupRegistry::GetGroups() (itself materializing every
    // registered GroupId) followed by a Find() per id - STATIC review
    // correctly identified that as the exact same class of recurring,
    // registry-size-scaling world-thread work already eliminated from
    // maintenance discovery (see RunCoalitionMaintenance()'s own scan
    // bound). Checking per-proposal instead means the total cost of this
    // arbitration step scales with how many proposals THIS pass actually
    // produced, never with total registry size.
    //
    // 2.12F2 P3 fix (STATIC review): logged at DEBUG, at most once per
    // conflicted AgentId per pass (loggedOverlap below), not once per
    // proposal at WARN - an earlier version logged a WARN inside this same
    // loop, which meant a persistent overlap (an expected, ongoing
    // arbitration state, not a server error) produced a fresh WARN every
    // single AIWorld.GroupCoordinationIntervalMs indefinitely, worse with
    // more overlapping groups.
    std::unordered_set<uint64> loggedOverlap;
    for (GroupMemberActionProposal const& proposal : proposals)
    {
        // Shared with ReconcileGroupCoordinationForMember(), 2.12F2 P2 fix
        // round 4 (STATIC review) - the exact same "how many coordination-
        // enabled groups does this member currently belong to" question,
        // asked here at dispatch time and there at confirmed-Join time.
        uint32 coordinationEnabledMemberships = CountCoordinationEnabledMemberships(proposal.Member);

        if (coordinationEnabledMemberships > 1)
        {
            if (loggedOverlap.insert(proposal.Member.Value).second)
                TC_LOG_DEBUG("ai.world", "AI group coordination: member={} belongs to {} coordination-enabled groups at once, dispatching no {} for it this pass (one of them: group={})",
                    proposal.Member.Value, coordinationEnabledMemberships, ToString(proposal.SourceIntent), proposal.SourceGroup.Value);
            continue;
        }

        DispatchGroupMemberActionProposal(proposal);
    }

    // Milestone 2.12G3C2: the HUNT counterpart to the overlap-arbitration
    // loop above - the exact same CountCoordinationEnabledMemberships()
    // definition of "coordination-ambiguous membership" (now also
    // covering HuntEnabled), applied to `huntProposals` in its own
    // separate pass since HuntProposal is a different type from
    // GroupMemberActionProposal.
    std::unordered_set<uint64> loggedHuntOverlap;
    for (HuntProposal const& huntProposal : huntProposals)
    {
        uint32 coordinationEnabledMemberships = CountCoordinationEnabledMemberships(huntProposal.Member);

        if (coordinationEnabledMemberships > 1)
        {
            if (loggedHuntOverlap.insert(huntProposal.Member.Value).second)
                TC_LOG_DEBUG("ai.world", "AI group coordination: member={} belongs to {} coordination-enabled groups at once, dispatching no HUNT for it this pass (one of them: group={})",
                    huntProposal.Member.Value, coordinationEnabledMemberships, huntProposal.SourceGroup.Value);
            continue;
        }

        DispatchHuntProposal(huntProposal);
    }
}

void AIWorldMgr::DispatchGroupMemberActionProposal(GroupMemberActionProposal const& proposal)
{
    // 2.12F2 P3 fix (STATIC review): fail-closed the same way
    // AgentGroupIntentProjector::Project() itself now is - see its own
    // comment. Today Project() never produces anything but Regroup/Roam
    // (2.12G2), so this is currently unreachable in practice, but this
    // dispatcher must never silently start a group-coordination MOVE_TO
    // for a proposal it cannot honestly attribute to a known intent.
    GoalType sourceGoal;
    switch (proposal.SourceIntent)
    {
        case AgentGroupIntentType::Regroup:
            sourceGoal = GoalType::Regroup;
            break;
        case AgentGroupIntentType::Roam:
            sourceGoal = GoalType::Roam;
            break;
        case AgentGroupIntentType::None:
        default:
            return;
    }

    AgentRecord* record = _registry.Find(proposal.Member);
    if (!record)
        return;

    // Milestone 2.12F4A: ControlMode gate (performance/early-rejection -
    // ActionSystem::Validate() is the actual mandatory safety boundary
    // regardless, see its own comment). An ObserveOnly member is dropped
    // from group coordination dispatch the same way it is from every
    // other action-proposing path - group intent/observation collection
    // upstream of this call still tracks it as an ordinary member.
    if (record->ControlMode != AgentControlMode::AIWorldControlled)
        return;

    // Re-confirms SourceGroup/membership fresh - never trusts that either
    // still holds by the time this specific proposal (possibly several
    // members deep into RunCoalitionCoordination()'s own batch) actually
    // runs. See this method's own declaration comment.
    AgentGroupRecord const* group = _groupRegistry.Find(proposal.SourceGroup);
    if (!group)
        return;

    bool stillMember = std::any_of(group->Members.begin(), group->Members.end(),
        [&proposal](AgentGroupMembership const& membership) { return membership.Member == proposal.Member; });
    if (!stillMember)
        return;

    if (record->WorldState != AgentWorldState::Materialized)
        return;

    Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
    Creature* creature = ResolveLiveCreature(*record, map);
    if (!creature || !creature->IsAlive())
        return;

    // Regroup/Roam are both the LOWEST MOVE_TO tier - any higher-tier
    // individual reason already owns this agent's own action slot this
    // pass, and this proposal is simply dropped, not queued.
    if (record->ActiveGoalState || record->RoutineGoalState)
        return;

    if (record->ActiveActionState)
        return;

    // 2.12F2 P3 fix, round 2 (STATIC review): explicit "unreachable
    // coordination member" semantics, checked here rather than left
    // implicit in ValidateMoveTo()'s own generic DestinationTooFar
    // rejection. AgentGroupIntentProjector::Project() only ever compares
    // distance against profile.RegroupRadius - a TRIGGER threshold ("is
    // this member dispersed enough to bother proposing a move at all"),
    // never against ActionSystem's own execution-layer maximum range, by
    // design: the pure projector layer must not need to know an
    // execution-layer constant (see AgentGroupIntentProjector.h's own
    // class comment on staying profile-agnostic).
    //
    // Milestone 2.12F2 P2 fix, round 3 (STATIC review): this is now the
    // ONLY place that enforces reachability at all - AIWorld.WolfGroupFormationRadius/
    // AIWorld.WolfGroupLeaveRadius are deliberately NOT clamped against
    // ActionSystem::CoordinationMoveToRangeYards() any more (see
    // AIWorld.WolfGroupLeaveRadius's own Initialize() comment for why: that
    // coupled Formation/Maintenance policy, its own independent capability,
    // to Coordination's own execution-layer limit, changing what radius a
    // group could form/be maintained at purely because Coordination happens
    // to be configured with a narrower one, even while Coordination itself
    // is disabled). AIWorld.WolfGroupRegroupRadius alone still is clamped
    // against it (see that key's own comment) - it is genuinely a
    // Coordination-layer trigger threshold, unlike Formation/LeaveRadius.
    // So a member can legitimately be a group member (LeaveRadius) or even
    // trigger a Regroup intent (RegroupRadius, once clamped, always <= this
    // bound) from farther away than this dispatcher can actually reach -
    // that gap is expected, not a misconfiguration, and this check is what
    // turns it into a clean, named "nothing to do" instead of a rejected
    // ActionRequest rebuilt and re-rejected as DestinationTooFar every
    // single pass, forever, indistinguishable from an ordinary one-off
    // rejection (busy movement, momentary map mismatch, ...).
    float unreachableDx = proposal.X - creature->GetPositionX();
    float unreachableDy = proposal.Y - creature->GetPositionY();
    float unreachableDz = proposal.Z - creature->GetPositionZ();
    float unreachableDistanceSq = unreachableDx * unreachableDx + unreachableDy * unreachableDy + unreachableDz * unreachableDz;
    float maxCoordinationRangeYards = ActionSystem::CoordinationMoveToRangeYards();

    if (unreachableDistanceSq > maxCoordinationRangeYards * maxCoordinationRangeYards)
    {
        TC_LOG_DEBUG("ai.world", "AI group coordination: member={} group={} is farther than the {} range bound ({:.1f}yd) from its own target - UNREACHABLE, no automatic {} attempted this pass",
            record->Id.Value, proposal.SourceGroup.Value, ToString(sourceGoal), maxCoordinationRangeYards, ToString(sourceGoal));
        return;
    }

    // Milestone 2.12G2 P2 fix (STATIC review): a Roam target, unlike a
    // Regroup one, is a NEWLY COMPUTED point (AgentGroupIntentSystem's own
    // deterministic compass offset around the group's territory anchor) -
    // it was never a real recorded location the way TerritoryX/Y/Z itself
    // is (captured once, at formation, from an actual member's own
    // position). ActionSystem::ValidateMoveTo() only ever checks
    // map/finite-coordinates/distance, never navigability, and
    // MotionMaster::MovePoint()'s own PathGenerator use is NOT fail-closed
    // - a PATHFIND_NOPATH result still falls back to a straight two-point
    // spline to the raw coordinates (MoveSplineInit::MoveTo()), which
    // could walk a member through a wall, off a cliff, or under/above the
    // terrain, and would still fire an ordinary arrival completion once
    // it (perhaps implausibly) reaches those raw coordinates. So a Roam
    // target's ground point and path are resolved and validated HERE,
    // against the world this member's own Map already has loaded
    // (Map::GetHeight()/PathGenerator both only ever read already-resident
    // grid/vmap/navmesh data around the actor's own current position -
    // neither one forces anything new to load), and this dispatch fails
    // closed for THIS member - not proposing a still-unvalidated raw
    // offset the pure AgentGroupIntentSystem layer itself has no way to
    // check - if no valid ground point or real navigable path to it
    // exists. Regroup is deliberately NOT put through this - its own
    // target is already a real, previously-occupied location, not a
    // synthesized one.
    float roamDestinationZ = proposal.Z;
    if (proposal.SourceIntent == AgentGroupIntentType::Roam)
    {
        float groundZ = map->GetHeight(creature->GetPhaseMask(), proposal.X, proposal.Y, creature->GetPositionZ() + 20.0f, true);
        if (groundZ <= INVALID_HEIGHT)
        {
            TC_LOG_DEBUG("ai.world", "AI group coordination: member={} group={} ROAM target ({:.1f},{:.1f}) has no resolvable ground height in the already-loaded world - UNREACHABLE, no automatic Roam attempted this pass",
                record->Id.Value, proposal.SourceGroup.Value, proposal.X, proposal.Y);
            return;
        }

        // A modest vertical sanity bound - a technically-resolvable height
        // (e.g. atop a nearby building or cliff far above/below the
        // group's own territory) is still not a reasonable roam point for
        // a ground-bound Loose group, even once it does pass the
        // PathGenerator check below (a navmesh can legitimately connect
        // two very different elevations, e.g. a switchback trail).
        if (std::fabs(groundZ - creature->GetPositionZ()) > 20.0f)
        {
            TC_LOG_DEBUG("ai.world", "AI group coordination: member={} group={} ROAM target ground height ({:.1f}) is implausibly far from current Z ({:.1f}) - UNREACHABLE, no automatic Roam attempted this pass",
                record->Id.Value, proposal.SourceGroup.Value, groundZ, creature->GetPositionZ());
            return;
        }

        // Same PathGenerator/PATHFIND_NOPATH convention MoveSplineInit.cpp/
        // ChaseMovementGenerator.cpp already use to decide "is there a
        // real path here" - PATHFIND_INCOMPLETE is deliberately still
        // accepted (a real, partial navmesh path toward the target, not a
        // straight-line fallback; ArrivalTolerance-based completion
        // already handles a short arrival honestly, see
        // ActionEngineEvent.h's own comment), only PATHFIND_NOPATH (no
        // real path at all, MoveSplineInit's own straight-line fallback)
        // is rejected here.
        PathGenerator roamPath(creature);
        bool pathCalculated = roamPath.CalculatePath(proposal.X, proposal.Y, groundZ, false);
        if (!pathCalculated || (roamPath.GetPathType() & PATHFIND_NOPATH))
        {
            TC_LOG_DEBUG("ai.world", "AI group coordination: member={} group={} ROAM target ({:.1f},{:.1f},{:.1f}) has no navigable path - UNREACHABLE, no automatic Roam attempted this pass",
                record->Id.Value, proposal.SourceGroup.Value, proposal.X, proposal.Y, groundZ);
            return;
        }

        roamDestinationZ = groundZ;
    }

    uint64 nowMs = CurrentTimeMs();

    ActionPosition destination;
    destination.MapId = proposal.MapId;
    destination.X = proposal.X;
    destination.Y = proposal.Y;
    destination.Z = roamDestinationZ;

    ActionRequest moveRequest;
    moveRequest.Actor = proposal.Member;
    moveRequest.Type = ActionType::MoveTo;
    moveRequest.SourceGoal = sourceGoal;
    moveRequest.GoalStartedAtMs = nowMs;
    moveRequest.Destination = destination;

    // Milestone 2.12G1: sourceGroupProfile added alongside sourceGroup -
    // genericity runtime proof needs to see, per dispatch, which profile
    // (WolfLoose vs. DefiasLoose) a given group actually belongs to, not
    // just its opaque GroupId. `group` was already resolved above.
    // Milestone 2.12G2: sourceGoal now honestly varies (REGROUP or ROAM)
    // instead of always reporting REGROUP regardless of proposal.SourceIntent.
    TC_LOG_DEBUG("ai.world", "AI action request agent={} type={} sourceGoal={} destination=({:.1f},{:.1f},{:.1f}) sourceGroup={} sourceGroupProfile={}",
        record->Id.Value, ToString(moveRequest.Type), ToString(moveRequest.SourceGoal),
        destination.X, destination.Y, destination.Z, proposal.SourceGroup.Value, ToString(group->ProfileId));

    // Set BEFORE Validate() - same order the routine MOVE_TO dispatch in
    // UpdateNeeds() already uses, so ActiveGoalType/ActiveGoalStartedAtMs
    // below can honestly name this attempt. Rolled back below on any
    // rejection/failure to start - see GroupCoordinationGoal.h for why
    // this must never survive naming an attempt that never actually ran.
    GroupCoordinationGoal coordinationGoal;
    coordinationGoal.Type = sourceGoal;
    coordinationGoal.SourceGroup = proposal.SourceGroup;
    coordinationGoal.StartedAtMs = nowMs;
    record->GroupCoordinationGoalState = coordinationGoal;

    ActionValidationContext moveContext;
    moveContext.Materialized = true;
    moveContext.Alive = true;
    moveContext.ControlMode = record->ControlMode;
    moveContext.ActiveGoalType = sourceGoal;
    moveContext.ActiveGoalStartedAtMs = nowMs;
    moveContext.MapId = creature->GetMapId();
    moveContext.X = creature->GetPositionX();
    moveContext.Y = creature->GetPositionY();
    moveContext.Z = creature->GetPositionZ();
    moveContext.HasActiveMovement = creature->GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE) != nullptr;

    ActionValidationResult moveValidation = _actionSystem.Validate(moveRequest, moveContext);

    TC_LOG_DEBUG("ai.world", "AI action validation agent={} type={} result={} reason={}",
        record->Id.Value, ToString(moveRequest.Type), moveValidation.Allowed ? "ALLOWED" : "REJECTED",
        ToString(moveValidation.Reason));

    if (!moveValidation.Allowed)
    {
        record->GroupCoordinationGoalState.reset();
        return;
    }

    ActionResult moveResult = _actionExecutor.ExecuteMoveTo(moveRequest, *creature);

    TC_LOG_DEBUG("ai.world", "AI action execution agent={} type={} status={} reason={}",
        record->Id.Value, ToString(moveResult.Type), ToString(moveResult.Status), ToString(moveResult.Reason));

    if (moveResult.Status != ActionExecutionStatus::Started)
    {
        record->GroupCoordinationGoalState.reset();
        return;
    }

    ActiveAction action;
    action.Type = moveRequest.Type;
    action.SourceGoal = moveRequest.SourceGoal;
    action.GoalStartedAtMs = moveRequest.GoalStartedAtMs;
    action.StartedAtMs = nowMs;
    action.Destination = moveRequest.Destination;
    record->ActiveActionState = action;
}

void AIWorldMgr::DispatchHuntProposal(HuntProposal const& proposal)
{
    // Milestone 2.12G3D1 diagnostic-only addition (STATIC review): a
    // narrow, read-only trace of exactly which fail-closed check rejects
    // the ONE watched agent's own proposal, if any - never changes state,
    // selection, or dispatch, and never sets _testObserveActiveHuntFired
    // (only CheckTestObserveActiveHunt() does that, on a genuine full
    // PASS).
    //
    // Milestone 2.12G3D2A: deliberately NOT also gated on
    // !_testObserveActiveHuntFired any more - that latch is specific to
    // CheckTestObserveActiveHunt()'s own one-shot "the approach pipeline
    // works at all" proof, which fires early (during the very first
    // Approaching phase). Observing "no further MOVE_TO for a still-
    // standing target" across several LATER coordination passes is
    // exactly what this diagnostic now also needs to prove, well after
    // that earlier latch has already been set - tying dispatch-stage
    // diagnostics to it would silence them at exactly the point they
    // become useful.
    bool huntDiagnosticsEnabled = proposal.Member == _testObserveActiveHuntAgentId;
    auto logDispatchRejected = [&](char const* reason)
    {
        if (huntDiagnosticsEnabled)
            TC_LOG_INFO("ai.world", "AI HUNT approach runtime diagnostic: agent={} stage=DISPATCH result=REJECTED reason={}",
                proposal.Member.Value, reason);
    };

    AgentRecord* record = _registry.Find(proposal.Member);
    if (!record)
    {
        logDispatchRejected("AGENT_MISSING");
        return;
    }

    if (record->ControlMode != AgentControlMode::AIWorldControlled)
    {
        logDispatchRejected("CONTROL_MODE");
        return;
    }

    AgentGroupRecord const* group = _groupRegistry.Find(proposal.SourceGroup);
    if (!group)
    {
        logDispatchRejected("GROUP_MISSING");
        return;
    }

    // 2.12G3C2's own extra defense-in-depth check - unlike
    // DispatchGroupMemberActionProposal(), which relies entirely on
    // RunCoalitionCoordination()'s own upstream discovery-time filter,
    // never trust that a lifecycle operation could not have started
    // between this proposal's own construction and this specific dispatch.
    if (_groupLifecycleSystem.HasPendingOperation(proposal.SourceGroup))
    {
        logDispatchRejected("LIFECYCLE_PENDING");
        return;
    }

    bool stillMember = std::any_of(group->Members.begin(), group->Members.end(),
        [&proposal](AgentGroupMembership const& membership) { return membership.Member == proposal.Member; });
    if (!stillMember)
    {
        logDispatchRejected("NOT_MEMBER");
        return;
    }

    std::optional<AgentGroupCoordinationProfile> profile = ResolveCoordinationProfile(group->ProfileId);
    if (!profile || !profile->HuntEnabled)
    {
        logDispatchRejected("PROFILE_DISABLED");
        return;
    }

    if (record->WorldState != AgentWorldState::Materialized)
    {
        logDispatchRejected("ACTOR_NOT_MATERIALIZED");
        return;
    }

    // Milestone 2.12G3D1 diagnostic-only change: split into two checks
    // (was one combined "ActiveGoalState || RoutineGoalState" check) so
    // each has its own distinct reason - behaviorally identical, still
    // rejects on either being set.
    if (record->ActiveGoalState)
    {
        logDispatchRejected("ACTIVE_GOAL");
        return;
    }

    if (record->RoutineGoalState)
    {
        logDispatchRejected("ROUTINE_GOAL");
        return;
    }

    if (record->ActiveActionState)
    {
        logDispatchRejected("ACTIVE_ACTION");
        return;
    }

    // Milestone 2.12G3D2A: no longer an unconditional reject. A member
    // already AtTarget for THIS EXACT attempt (same group/target/
    // StartedAtMs) may still be re-approached if the target has since
    // moved away - reusing the SAME attempt identity, never a fresh one
    // (checked further below, once the target's own live position is
    // resolved). Anything else with a GroupCoordinationGoalState already
    // set - a different attempt, a different target, or this exact
    // attempt still Approaching (ActiveActionState's own check above
    // should already have caught that case, but this is verified
    // independently, never assumed) - is still refused outright: HUNT's
    // own GroupCoordinationGoalState carries target identity that must
    // never be silently overwritten.
    bool alreadyAtTargetForThisAttempt = false;
    if (record->GroupCoordinationGoalState)
    {
        bool sameAttempt = record->GroupCoordinationGoalState->Type == GoalType::Hunt
            && record->GroupCoordinationGoalState->SourceGroup == proposal.SourceGroup
            && record->GroupCoordinationGoalState->TargetGuid == proposal.Target.TargetGuid
            && record->GroupCoordinationGoalState->TargetEntry == proposal.Target.TargetEntry
            && record->GroupCoordinationGoalState->StartedAtMs == proposal.StartedAtMs;

        if (!sameAttempt || record->GroupCoordinationGoalState->Phase != HuntPhase::AtTarget)
        {
            logDispatchRejected("COORDINATION_GOAL");
            return;
        }

        alreadyAtTargetForThisAttempt = true;
    }

    if (CountCoordinationEnabledMemberships(proposal.Member) > 1)
    {
        logDispatchRejected("MEMBERSHIP_AMBIGUOUS");
        return;
    }

    Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
    Creature* creature = ResolveLiveCreature(*record, map);
    if (!creature || !creature->IsAlive())
    {
        logDispatchRejected("ACTOR_UNRESOLVED_OR_DEAD");
        return;
    }

    // Transient, already-loaded-object lookup only - never a force-load.
    Creature* target = ObjectAccessor::GetCreature(*creature, proposal.Target.TargetGuid);
    if (!target || !target->IsAlive())
    {
        logDispatchRejected("TARGET_UNRESOLVED_OR_DEAD");
        return;
    }

    // The request must be an honest continuation of what was actually
    // resolved, not proposal.Target's own (possibly stale) claim - see
    // this method's own header comment.
    if (target->GetEntry() != proposal.Target.TargetEntry || target->GetMapId() != proposal.Target.MapId)
    {
        logDispatchRejected("TARGET_IDENTITY_MISMATCH");
        return;
    }

    if (target == creature)
    {
        logDispatchRejected("SELF_TARGET");
        return;
    }

    if (!creature->IsValidAttackTarget(target))
    {
        logDispatchRejected("TARGET_NOT_ATTACKABLE");
        return;
    }

    // Milestone 2.12G3C2 P2 fix (STATIC review): loaded into locals and
    // verified finite HERE, before ANY use for distance/LOS/pathing/
    // destination below - an earlier version used target->GetPosition*()
    // directly for distance and IsWithinLOS() first, only checking
    // isfinite() afterward, so a non-finite position could already have
    // reached both.
    uint32 targetMapId = target->GetMapId();
    float targetX = target->GetPositionX();
    float targetY = target->GetPositionY();
    float targetZ = target->GetPositionZ();

    if (!std::isfinite(targetX) || !std::isfinite(targetY) || !std::isfinite(targetZ))
    {
        logDispatchRejected("TARGET_POSITION_INVALID");
        return;
    }

    // Milestone 2.12G3D2A: a member already AtTarget for this exact
    // attempt is only re-approached if the target has genuinely moved
    // away - never while still effectively standing at it. Reuses the
    // same ArrivalToleranceYards bound MOVE_TO arrival/Eat/Work-Rest
    // already share (ArrivalTolerance.h), rather than inventing a new
    // HUNT-specific one. Checked here, before the heavier range/LOS/
    // PathGenerator work below, since this is the expected common case
    // every pass a member spends idling at its own target.
    if (alreadyAtTargetForThisAttempt)
    {
        ActionPosition targetPosition;
        targetPosition.MapId = targetMapId;
        targetPosition.X = targetX;
        targetPosition.Y = targetY;
        targetPosition.Z = targetZ;

        if (IsWithinArrivalTolerance(targetPosition, creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()))
        {
            logDispatchRejected("ALREADY_AT_TARGET");
            return;
        }

        // Milestone 2.12G3D2A P3 fix (STATIC review): the "stage=REAPPROACH
        // result=APPROACHING" diagnostic itself is logged much later, only
        // after a genuinely successful ExecuteMoveTo() - not here, where
        // range/LOS/PathGenerator/Validate()/Execute() could still reject
        // this re-approach attempt.
    }

    float rangeDx = targetX - creature->GetPositionX();
    float rangeDy = targetY - creature->GetPositionY();
    float rangeDz = targetZ - creature->GetPositionZ();
    float rangeDistanceSq = rangeDx * rangeDx + rangeDy * rangeDy + rangeDz * rangeDz;

    if (rangeDistanceSq > profile->HuntAcquisitionRadius * profile->HuntAcquisitionRadius)
    {
        logDispatchRejected("TARGET_OUT_OF_RANGE");
        return;
    }

    float maxCoordinationRangeYards = ActionSystem::CoordinationMoveToRangeYards();
    if (rangeDistanceSq > maxCoordinationRangeYards * maxCoordinationRangeYards)
    {
        TC_LOG_DEBUG("ai.world", "AI HUNT coordination: member={} group={} is farther than the {:.1f}yd range bound from its own live target - UNREACHABLE, no automatic HUNT approach attempted this pass",
            record->Id.Value, proposal.SourceGroup.Value, maxCoordinationRangeYards);
        logDispatchRejected("TARGET_OUT_OF_RANGE");
        return;
    }

    if (!creature->IsWithinLOS(targetX, targetY, targetZ))
    {
        logDispatchRejected("TARGET_NO_LOS");
        return;
    }

    // Same PathGenerator/PATHFIND_NOPATH convention Roam's own dispatch-time
    // path check already uses - PATHFIND_INCOMPLETE (a real, partial path)
    // is still accepted, only PATHFIND_NOPATH (MoveSplineInit's own
    // straight-line fallback) is rejected.
    PathGenerator huntPath(creature);
    bool pathCalculated = huntPath.CalculatePath(targetX, targetY, targetZ, false);
    if (!pathCalculated || (huntPath.GetPathType() & PATHFIND_NOPATH))
    {
        TC_LOG_DEBUG("ai.world", "AI HUNT coordination: member={} group={} has no navigable path to its own live target - UNREACHABLE, no automatic HUNT approach attempted this pass",
            record->Id.Value, proposal.SourceGroup.Value);
        logDispatchRejected("TARGET_NO_PATH");
        return;
    }

    uint64 nowMs = CurrentTimeMs();

    // The MOVE_TO destination is the target's ACTUAL CURRENT position -
    // never proposal.Target.X/Y/Z, which came from a HuntTargetObservation
    // that can already be stale by the time this runs.
    ActionPosition destination;
    destination.MapId = targetMapId;
    destination.X = targetX;
    destination.Y = targetY;
    destination.Z = targetZ;

    ActionRequest moveRequest;
    moveRequest.Actor = proposal.Member;
    moveRequest.Type = ActionType::MoveTo;
    moveRequest.SourceGoal = GoalType::Hunt;
    // Deliberately the group-wide HUNT ATTEMPT's own stable identity, not
    // a fresh per-dispatch CurrentTimeMs() - see this method's own header
    // comment for why every member subsequently dispatched into the SAME
    // ongoing attempt must get the exact same value.
    moveRequest.GoalStartedAtMs = proposal.StartedAtMs;
    moveRequest.Destination = destination;
    moveRequest.Target = ActionTargetRef{ target->GetGUID(), target->GetEntry() };

    TC_LOG_DEBUG("ai.world", "AI action request agent={} type={} sourceGoal={} destination=({:.1f},{:.1f},{:.1f}) target={} sourceGroup={} sourceGroupProfile={}",
        record->Id.Value, ToString(moveRequest.Type), ToString(moveRequest.SourceGoal),
        destination.X, destination.Y, destination.Z, target->GetEntry(), proposal.SourceGroup.Value, ToString(group->ProfileId));

    // Milestone 2.12G3D2A P2 fix (STATIC review): captured BEFORE
    // overwriting below, so a validation/execution failure can restore it
    // rather than unconditionally resetting to nullopt. For a fresh
    // dispatch (alreadyAtTargetForThisAttempt == false) this is already
    // nullopt, so the restore-on-failure path below is a no-op and
    // behavior is unchanged from before this fix. For a re-approach, this
    // is the member's own existing AtTarget-phase GroupCoordinationGoalState -
    // resetting to nullopt on a failed re-approach attempt would silently
    // discard a still-valid, still-owned HUNT attempt's own pin (a
    // single-member group would then have nothing left to reconstruct
    // that attempt's own identity from, and the next pass would freshly
    // select the target again under a NEW StartedAtMs, breaking stable
    // attempt identity).
    std::optional<GroupCoordinationGoal> previousCoordinationGoal = record->GroupCoordinationGoalState;

    // Set BEFORE Validate() - the same order/rollback-on-rejection
    // discipline DispatchGroupMemberActionProposal() already holds to.
    GroupCoordinationGoal coordinationGoal;
    coordinationGoal.Type = GoalType::Hunt;
    coordinationGoal.SourceGroup = proposal.SourceGroup;
    coordinationGoal.StartedAtMs = proposal.StartedAtMs;
    coordinationGoal.TargetGuid = target->GetGUID();
    coordinationGoal.TargetEntry = target->GetEntry();
    coordinationGoal.TargetObservedAtMs = proposal.Target.ObservedAtMs;
    // Explicit even though it is also HuntPhase's own default - this call
    // may be overwriting an existing AtTarget-phase GroupCoordinationGoalState
    // (the re-approach path above), and a freshly (re-)dispatched MOVE_TO
    // is always Approaching again, never silently inherited from whatever
    // phase the prior state happened to be in.
    coordinationGoal.Phase = HuntPhase::Approaching;
    record->GroupCoordinationGoalState = coordinationGoal;

    ActionValidationContext moveContext;
    moveContext.Materialized = true;
    moveContext.Alive = true;
    moveContext.ControlMode = record->ControlMode;
    moveContext.ActiveGoalType = GoalType::Hunt;
    moveContext.ActiveGoalStartedAtMs = proposal.StartedAtMs;
    moveContext.MapId = creature->GetMapId();
    moveContext.X = creature->GetPositionX();
    moveContext.Y = creature->GetPositionY();
    moveContext.Z = creature->GetPositionZ();
    moveContext.HasActiveMovement = creature->GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE) != nullptr;
    moveContext.TargetResolved = true;
    moveContext.TargetAlive = target->IsAlive();
    moveContext.TargetAttackable = creature->IsValidAttackTarget(target);
    moveContext.TargetGuid = target->GetGUID();
    moveContext.TargetEntry = target->GetEntry();
    moveContext.TargetMapId = targetMapId;
    moveContext.TargetX = targetX;
    moveContext.TargetY = targetY;
    moveContext.TargetZ = targetZ;

    ActionValidationResult moveValidation = _actionSystem.Validate(moveRequest, moveContext);

    TC_LOG_DEBUG("ai.world", "AI action validation agent={} type={} result={} reason={}",
        record->Id.Value, ToString(moveRequest.Type), moveValidation.Allowed ? "ALLOWED" : "REJECTED",
        ToString(moveValidation.Reason));

    if (!moveValidation.Allowed)
    {
        if (alreadyAtTargetForThisAttempt && huntDiagnosticsEnabled)
            TC_LOG_INFO("ai.world", "AI HUNT approach runtime diagnostic: agent={} stage=REAPPROACH result=ROLLED_BACK reason=VALIDATION_REJECTED",
                proposal.Member.Value);

        record->GroupCoordinationGoalState = previousCoordinationGoal;
        return;
    }

    ActionResult moveResult = _actionExecutor.ExecuteMoveTo(moveRequest, *creature);

    TC_LOG_DEBUG("ai.world", "AI action execution agent={} type={} status={} reason={}",
        record->Id.Value, ToString(moveResult.Type), ToString(moveResult.Status), ToString(moveResult.Reason));

    if (moveResult.Status != ActionExecutionStatus::Started)
    {
        if (alreadyAtTargetForThisAttempt && huntDiagnosticsEnabled)
            TC_LOG_INFO("ai.world", "AI HUNT approach runtime diagnostic: agent={} stage=REAPPROACH result=ROLLED_BACK reason=EXECUTION_FAILED",
                proposal.Member.Value);

        record->GroupCoordinationGoalState = previousCoordinationGoal;
        return;
    }

    ActiveAction action;
    action.Type = moveRequest.Type;
    action.SourceGoal = moveRequest.SourceGoal;
    action.GoalStartedAtMs = moveRequest.GoalStartedAtMs;
    action.StartedAtMs = nowMs;
    action.Destination = moveRequest.Destination;
    action.Target = moveRequest.Target;

    // Milestone 2.12G3D2A P3 fix (STATIC review): moved here, after a
    // genuinely successful ExecuteMoveTo() and ActiveActionState
    // creation, not at the moment re-approach eligibility was merely
    // determined (before range/LOS/PathGenerator/Validate()/Execute()
    // could still reject it) - an earlier version logged this too early
    // and could claim APPROACHING even when the re-approach never
    // actually started.
    if (alreadyAtTargetForThisAttempt && huntDiagnosticsEnabled)
        TC_LOG_INFO("ai.world", "AI HUNT approach runtime diagnostic: agent={} stage=REAPPROACH result=APPROACHING group={} target={} targetEntry={} startedAtMs={}",
            proposal.Member.Value, proposal.SourceGroup.Value, target->GetGUID().ToString(), target->GetEntry(), proposal.StartedAtMs);
    record->ActiveActionState = action;
}

bool AIWorldMgr::HasInFlightRoamAttempt(AgentGroupRecord const& group) const
{
    for (AgentGroupMembership const& membership : group.Members)
    {
        AgentRecord const* member = _registry.Find(membership.Member);
        if (!member || !member->GroupCoordinationGoalState)
            continue;

        if (member->GroupCoordinationGoalState->Type == GoalType::Roam
            && member->GroupCoordinationGoalState->SourceGroup == group.Id)
            return true;
    }

    return false;
}

uint32 AIWorldMgr::CountCoordinationEnabledMemberships(AgentId member) const
{
    uint32 count = 0;
    for (GroupId groupId : _groupRegistry.GetGroupsOfMember(member))
    {
        AgentGroupRecord const* group = _groupRegistry.Find(groupId);
        if (!group)
            continue;

        // Milestone 2.12G2/2.12G3C2: RegroupEnabled OR RoamEnabled OR
        // HuntEnabled, not any one alone - "coordination-ambiguous
        // membership" is a fact about whether more than one group could
        // dispatch SOME automatic movement for this member, regardless of
        // which specific intent each one would actually propose. Scoping
        // this to fewer than all three would have let a member belong to
        // one RegroupEnabled group and one HuntEnabled-only group at once
        // with neither this overlap check nor
        // ReconcileGroupCoordinationForMember() ever noticing - exactly
        // the kind of safety net G1's own STATIC review already warned
        // must generalize with the capability, not stay hard-coded to
        // whichever ones existed first.
        std::optional<AgentGroupCoordinationProfile> profile = ResolveCoordinationProfile(group->ProfileId);
        if (profile && (profile->RegroupEnabled || profile->RoamEnabled || profile->HuntEnabled))
            ++count;
    }

    return count;
}

void AIWorldMgr::StopInFlightGroupCoordination(AgentRecord& record, char const* reason, CoordinationStopReason stopReason)
{
    // Milestone 2.12G3D2A P2 fix (STATIC review): captured BEFORE the
    // reset below, and used as the SOLE source of this stop event's own
    // identity (Type/SourceGroup/StartedAtMs/TargetGuid/TargetEntry) -
    // never ActiveActionState's own SourceGoal/GoalStartedAtMs, which
    // HuntPhase::AtTarget (2.12G3D2A) means may legitimately not exist at
    // all: ownership can persist with NO ActiveActionState (nothing is
    // currently running). By the ownership invariant Dispatch*Proposal()
    // already establishes - GroupCoordinationGoalState and
    // ActiveActionState, whenever BOTH exist, always agree on Type/
    // StartedAtMs, since both are set together and neither is ever
    // modified independently - sourcing identity from
    // GroupCoordinationGoalState alone is always at least as correct as
    // the previous ActiveActionState-sourced version for Regroup/Roam
    // (which never have an AtTarget-equivalent state), and is now also
    // the only source that still works once ActiveActionState is gone.
    std::optional<GroupCoordinationGoal> priorCoordinationGoal = record.GroupCoordinationGoalState;

    record.GroupCoordinationGoalState.reset();

    // Nothing was owned at all, or it was not a coordination-sourced goal
    // (should not happen given this field's own contract, but never
    // assumed) - nothing to record.
    if (!priorCoordinationGoal || !IsCoordinationSourceGoal(priorCoordinationGoal->Type))
        return;

    // Milestone 2.12G2R P2 fix, round 3 (STATIC review): captured before
    // the reset below - see CoordinationStopEvent.h's own comment for why
    // this exists (a verifying test hook needs to know THIS specific
    // attempt was stopped BY a genuine production stop path, tagged with
    // the CALLER's own stopReason - StoppedByLifecycle or
    // StoppedByMembershipAmbiguity, never a shared/ambiguous one - not
    // merely that it is gone, which a natural ARRIVED would also
    // produce). Both engine-generator fields default false (unverified) -
    // if no live Creature resolves below, NEITHER can be honestly
    // confirmed, and leaving them false (never defaulted to a claimed
    // success) is the correct fail-closed outcome, not "assume nothing
    // was running so trivially count as stopped". Milestone 2.12G3D2A:
    // this is exactly the honest outcome for a HuntPhase::AtTarget stop
    // too - there genuinely is no engine movement to observe running, so
    // both fields correctly read false rather than being skipped/assumed.
    // Milestone 2.12G3D2A P2 fix, round 2 (STATIC review): StopMoveTo()
    // (ActionExecutor) recognizes only the shared AIWorld MovePointId - it
    // cannot itself tell WHICH GoalType/attempt owns whatever engine
    // movement is currently running - so this method must verify that
    // BEFORE ever touching the engine, not assume it. Without this, a
    // member whose current ActiveActionState is unrelated to
    // priorCoordinationGoal (already reassigned to, say, GetFood, after
    // this coordination attempt reached HuntPhase::AtTarget or was
    // otherwise superseded) would have ITS unrelated movement silently
    // stopped by a coordination-goal cleanup that has nothing to do with
    // it, while that unrelated ActiveActionState itself is left claiming
    // to still be in flight. Type+StartedAtMs is enough for Regroup/Roam
    // (which never name a target), but not for HUNT - see
    // ActiveAction::Target's own comment for why target identity is
    // carried separately - so a HUNT match additionally requires the
    // live ActiveActionState's own Target to still agree with
    // priorCoordinationGoal's TargetGuid/TargetEntry.
    bool ownsStoppedMovement =
        record.ActiveActionState &&
        record.ActiveActionState->Type == ActionType::MoveTo &&
        record.ActiveActionState->SourceGoal == priorCoordinationGoal->Type &&
        record.ActiveActionState->GoalStartedAtMs == priorCoordinationGoal->StartedAtMs;

    if (ownsStoppedMovement && priorCoordinationGoal->Type == GoalType::Hunt)
    {
        ownsStoppedMovement = record.ActiveActionState->Target &&
            record.ActiveActionState->Target->Guid == priorCoordinationGoal->TargetGuid &&
            record.ActiveActionState->Target->Entry == priorCoordinationGoal->TargetEntry;
    }

    CoordinationStopEvent stopEvent;
    stopEvent.Reason = stopReason;
    stopEvent.SourceGoal = priorCoordinationGoal->Type;
    stopEvent.SourceGroup = priorCoordinationGoal->SourceGroup;
    stopEvent.StartedAtMs = priorCoordinationGoal->StartedAtMs;
    stopEvent.TargetGuid = priorCoordinationGoal->TargetGuid;
    stopEvent.TargetEntry = priorCoordinationGoal->TargetEntry;
    stopEvent.StoppedAtMs = CurrentTimeMs();

    // Milestone 2.12G3D2A P2 fix, round 2: every engine touch below is now
    // gated on ownsStoppedMovement too - a HuntPhase::AtTarget member (no
    // ActiveActionState at all) and a member whose ActiveActionState
    // belongs to an unrelated goal both correctly skip this block, and
    // both fields are honestly left at their false default (no engine
    // movement was ever confirmed to belong to this attempt, so neither
    // "was running" nor "confirmed stopped" can be claimed).
    if (ownsStoppedMovement && record.WorldState == AgentWorldState::Materialized)
    {
        Map* map = sMapMgr->FindBaseNonInstanceMap(record.MapId);
        if (Creature* creature = ResolveLiveCreature(record, map))
        {
            // Milestone 2.12G2R P2 fix, round 3 (STATIC review): observed
            // BEFORE StopMoveTo() is called, not just after - an earlier
            // version only checked HasOwnMoveToGenerator() AFTER
            // StopMoveTo() ran, which reads "confirmed stopped" just as
            // readily when nothing was running in the first place (the
            // attempt had already arrived naturally moments before this
            // call, so StopMoveTo() found nothing to remove) as when a
            // genuinely in-flight movement was actually just interrupted -
            // only the latter is real evidence this stop path did
            // meaningful work, which is exactly what a verifying test hook
            // needs to be able to tell apart.
            stopEvent.EngineGeneratorWasRunningBeforeStop = HasOwnMoveToGenerator(*creature);

            _actionExecutor.StopMoveTo(*creature);

            TC_LOG_DEBUG("ai.world", "AI action stop agent={} type={} reason={} sourceGoal={} sourceGroup={}",
                record.Id.Value, ToString(ActionType::MoveTo), reason, ToString(stopEvent.SourceGoal), stopEvent.SourceGroup.Value);

            // Freshly re-observed right after StopMoveTo() just ran, not
            // assumed true - see CoordinationStopEvent.h's own comment.
            stopEvent.EngineGeneratorConfirmedStoppedAfterStop = !HasOwnMoveToGenerator(*creature);
        }
    }

    record.LastCoordinationStop = stopEvent;

    // Milestone 2.12G3D2A P2 fix, round 2: reset ActiveActionState only
    // when ownsStoppedMovement already confirmed it is THIS attempt's own
    // - the same exact-match check that gated the engine stop above, now
    // reused here instead of the weaker IsCoordinationSourceGoal(SourceGoal)
    // check this replaced (which could match a coordination-sourced
    // ActiveActionState belonging to a DIFFERENT attempt/target). Absent
    // entirely for HuntPhase::AtTarget, which is not an error - there is
    // simply nothing left to reset.
    if (ownsStoppedMovement)
        record.ActiveActionState.reset();
}

void AIWorldMgr::StopGroupCoordinationForMember(AgentId memberId, GroupId groupId)
{
    AgentRecord* record = _registry.Find(memberId);
    if (!record || !record->GroupCoordinationGoalState || record->GroupCoordinationGoalState->SourceGroup != groupId)
        return;

    StopInFlightGroupCoordination(*record, "COORDINATION_STOPPED_BY_LIFECYCLE", CoordinationStopReason::StoppedByLifecycle);
}

void AIWorldMgr::ReconcileGroupCoordinationForMember(AgentId memberId)
{
    AgentRecord* record = _registry.Find(memberId);
    if (!record || !record->GroupCoordinationGoalState)
        return;

    // Milestone 2.12F2 P2 fix, round 4 (STATIC review): a confirmed Join
    // can newly make memberId's own membership coordination-ambiguous (see
    // RunCoalitionCoordination()'s own overlap-arbitration comment for the
    // "member belongs to more than one coordination-enabled group" rule
    // this mirrors - 2.12G2: RegroupEnabled OR RoamEnabled, see
    // CountCoordinationEnabledMemberships()) while a Regroup/Roam
    // dispatched BEFORE this join - back when the member's own membership
    // was still unambiguous - is still actively running. Without this,
    // the arbitration rule only ever applied to NEW dispatches: an
    // existing in-flight action would keep running to its own natural
    // conclusion purely because it happened to already be in flight
    // before the join confirmed, an implicit "whoever got there first
    // keeps it" priority the arbitration rule exists specifically to
    // avoid. Only relevant if a Regroup/Roam is actually in flight right
    // now (the check above) - most joins never reach this far.
    if (CountCoordinationEnabledMemberships(memberId) <= 1)
        return;

    StopInFlightGroupCoordination(*record, "COORDINATION_STOPPED_BY_MEMBERSHIP_AMBIGUITY", CoordinationStopReason::StoppedByMembershipAmbiguity);
}

void AIWorldMgr::Update(uint32 diff)
{
    if (!_enabled)
        return;

    // Drained first: sMapMgr->Update(diff) (which runs immediately before
    // AIWorldMgr::Update() in World::Update()) is where map/combat workers
    // publish whatever happened this tick.
    for (WorldEvent& event : _eventBus.Drain())
        ProcessWorldEvent(event);

    // Milestone 2.8F: same reasoning as _eventBus above - drained right
    // after it, still before anything in this tick's UpdateNeeds() could
    // start a new ActiveAction, so a stale event from an already-replaced
    // ActiveAction never gets a chance to be misread as belonging to the
    // new one. ProcessActionEngineEvent() does the actual validation;
    // nothing here trusts the event's contents yet.
    for (ActionEngineEvent& event : _actionEngineEventBus.Drain())
        ProcessActionEngineEvent(event);

    // Milestone 2.12E2: delivers every AgentGroupLifecycleSystem Request*
    // completion that has landed since the last tick - the same
    // "enqueue, poll every Update()" shape World::Update() already uses
    // for _queryProcessor.ProcessReadyCallbacks(). Every completion (and
    // therefore every AgentGroupRegistry mutation it makes) runs from
    // inside this call, so always on the world thread, never blocking it -
    // see AgentGroupLifecycleSystem.h for the full design.
    _groupLifecyclePending.ProcessReadyCallbacks();

    // Milestone 2.10A/2.10B: runs the bounded multi-agent scheduler pass -
    // AIWorld.DecisionSchedulerIntervalMs (default 250ms), deliberately
    // its own, faster cadence than either per-agent decision interval
    // RunDecisionScheduler() itself schedules against (Nearby/Active) - see
    // that function's own comment for why scheduler-poll cadence and
    // per-agent decision cadence are two different things.
    _decisionSchedulerTimer += diff;
    if (_decisionSchedulerTimer >= _decisionSchedulerIntervalMs)
    {
        _decisionSchedulerTimer = 0;
        RunDecisionScheduler();
    }

    // Milestone 2.12G1 P2 fix (STATIC review): a single shared call, every
    // tick, for every registered formation profile - see
    // RunCoalitionFormationPass()'s own comment for why this replaced two
    // independent per-profile timer/branch blocks here (duplicated
    // discovery, deterministic WolfLoose-first bias). Cheap when nothing
    // is due - the "no cost unless enabled" treatment every other
    // AIWorld.* capability gate in this class already gets is now
    // enforced per-profile INSIDE the pass, not by skipping the call
    // itself.
    RunCoalitionFormationPass(diff);

    // Milestone 2.12E4C2 P2 fix (STATIC review): gated on its OWN
    // AIWorld.CoalitionMaintenance flag, deliberately NOT
    // _wolfGroupAutoFormation - see _coalitionMaintenanceEnabled's own
    // declaration comment for why an operator stopping new automatic
    // formation must not also silently freeze maintenance of groups that
    // already exist (persisted across a restart, or created manually/by
    // an admin tool).
    if (_coalitionMaintenanceEnabled)
    {
        _coalitionMaintenanceTimer += diff;
        if (_coalitionMaintenanceTimer >= _coalitionMaintenanceIntervalMs)
        {
            _coalitionMaintenanceTimer = 0;
            RunCoalitionMaintenance();
        }
    }

    // Milestone 2.12F2: gated on its OWN AIWorld.GroupCoordination flag,
    // deliberately NOT _wolfGroupAutoFormation or _coalitionMaintenanceEnabled -
    // see _groupCoordinationEnabled's own declaration comment for why.
    if (_groupCoordinationEnabled)
    {
        _groupCoordinationTimer += diff;
        if (_groupCoordinationTimer >= _groupCoordinationIntervalMs)
        {
            _groupCoordinationTimer = 0;
            RunCoalitionCoordination();

            // Milestone 2.12F3 test hook, P3 fix round 2 (STATIC review):
            // checked only right here, immediately after a
            // RunCoalitionCoordination() pass actually ran - not on its
            // own per-tick check the way an earlier version did. A
            // Regroup is only ever freshly dispatched from inside
            // RunCoalitionCoordination() itself (DispatchGroupMemberActionProposal()),
            // so there is nothing new for this hook to observe in between
            // two passes that the previous pass's own check did not
            // already see - an earlier version's per-tick poll paid for a
            // full scan of the configured group's own Members on every
            // single world tick, unboundedly, for as long as the
            // condition stayed unmet, with no relationship to how often
            // the state it is checking could actually change. Tying this
            // to the same cadence RunCoalitionCoordination() already runs
            // on removes that unbounded per-tick cost entirely - gated on
            // AIWorld.TestDissolveOnActiveRegroupGroupId being set AND not
            // having fired yet, so it costs nothing at all once disabled
            // or once fired.
            if (_testDissolveOnActiveRegroupGroupId && !_testDissolveOnActiveRegroupFired)
                CheckTestDissolveOnActiveRegroup();

            // Milestone 2.12G2R: same "checked only right after a
            // RunCoalitionCoordination() pass actually ran" reasoning as
            // the Regroup hook immediately above - a Roam MOVE_TO is only
            // ever freshly dispatched from inside that same pass. Each
            // gated on its own config being set AND not yet fired, so all
            // three cost nothing once disabled or fired - and only one is
            // meant to be enabled at a time per 2.12G2R's own test
            // sequence (preempt, then leave, then dissolve last).
            if (_testPreemptOnActiveRoamAgentId && !_testPreemptOnActiveRoamFired)
                CheckTestPreemptOnActiveRoam();

            if (_testLeaveOnActiveRoamAgentId && !_testLeaveOnActiveRoamFired)
                CheckTestLeaveOnActiveRoam();

            if (_testDissolveOnActiveRoamGroupId && !_testDissolveOnActiveRoamFired)
                CheckTestDissolveOnActiveRoam();

            // Milestone 2.12G3D1: same "checked only right after a
            // RunCoalitionCoordination() pass actually ran" reasoning as
            // the hooks above - a HUNT MOVE_TO is only ever freshly
            // dispatched from inside that same pass (DispatchHuntProposal()).
            if (_testObserveActiveHuntAgentId && !_testObserveActiveHuntFired)
                CheckTestObserveActiveHunt();
        }
    }

    _nearbyPerceptionTimer += diff;
    if (_nearbyPerceptionTimer >= _nearbyPerceptionIntervalMs)
    {
        _nearbyPerceptionTimer = 0;
        ScanNearbyEntities();
    }

    _memoryMaintenanceTimer += diff;
    if (_memoryMaintenanceTimer >= 1000)
    {
        _memoryMaintenanceTimer = 0;
        _shortTermMemory.Expire(CurrentTimeMs());
    }

    _needsUpdateTimer += diff;
    if (_needsUpdateTimer >= _needsUpdateIntervalMs)
    {
        uint32 elapsedMs = _needsUpdateTimer;
        _needsUpdateTimer = 0;
        UpdateNeeds(elapsedMs);
    }

    _healthTimer += diff;
    if (_healthTimer >= _healthIntervalMs)
    {
        _healthTimer = 0;
        _aiClient->SubmitHealthCheck();
    }

    // World thread drains whatever AIClient's worker threads finished since
    // the last tick. AIClient already logged the raw outcome (submitted,
    // succeeded, failed, or timed out); what happens here is the world
    // thread's own judgment of whether a decision is still usable.
    AIResponse response;
    while (_aiClient->TryPopResponse(response))
    {
        if (response.Type != AIRequestType::Decision)
        {
            TC_LOG_DEBUG("ai.world", "AI response id={} consumed by world thread (status={}, latency={}ms)",
                response.RequestId, response.StatusCode, response.LatencyMs);
            continue;
        }

        // Milestone 2.10A: clears the scheduler's per-agent duplicate
        // guard unconditionally for every Decision-type response - success,
        // failure, or timeout alike, since AIClient guarantees exactly one
        // terminal response per submitted request (RequestTimeoutMs bounds
        // how long that can take). Must run before any of the discard/
        // stale checks below (they all "continue") - this agent's specific
        // in-flight request is resolved either way, whether or not the
        // resulting decision goes on to be used for anything.
        if (auto it = _decisionSchedule.find(response.Agent.Value); it != _decisionSchedule.end())
            it->second.AwaitingResponse = false;

        if (!response.Success)
            continue; // AIClient already logged the failure/timeout

        AgentRecord* record = _registry.Find(response.Agent);
        if (!record)
        {
            TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} is no longer registered, discarding",
                response.RequestId, response.Agent.Value);
            // Milestone 2.9D: a discard, not an invalid decision - the
            // response itself was a perfectly valid answer to a question
            // that simply stopped being current while it was in flight.
            // See ValidateDecisionIntent() for the discard_reason vocabulary.
            TC_METRIC_VALUE("ai.world.decision.discard", uint64(1), TC_METRIC_TAG("discard_reason", "not_materialized"));
            continue;
        }

        // A response can still be for the right snapshot sequence and
        // arrive after the agent already dematerialized (e.g. the Creature
        // despawned while the request was in flight). Nothing here mutates
        // world state yet, so this is currently just correctness hygiene -
        // it stops mattering only once decisions never do anything either
        // way, which won't stay true once a real Action API exists.
        if (record->WorldState != AgentWorldState::Materialized)
        {
            TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} discarded: agent is no longer materialized",
                response.RequestId, response.Agent.Value, response.SnapshotSequence);
            TC_METRIC_VALUE("ai.world.decision.discard", uint64(1), TC_METRIC_TAG("discard_reason", "not_materialized"));
            continue;
        }

        // Not just "<": a response claiming a snapshot sequence newer than
        // anything this agent has actually captured is just as wrong as an
        // old one, and must not be accepted either. Sequence is per-agent
        // now, not a single manager-wide counter.
        if (response.SnapshotSequence != record->SnapshotSequence)
        {
            TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} is STALE (latest snapshot={}), discarding",
                response.RequestId, response.Agent.Value, response.SnapshotSequence, record->SnapshotSequence);
            TC_METRIC_VALUE("ai.world.decision.discard", uint64(1), TC_METRIC_TAG("discard_reason", "stale_snapshot"));
            continue;
        }

        if (!response.Decision)
        {
            TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} succeeded but carries no decision payload, discarding",
                response.RequestId, response.Agent.Value, response.SnapshotSequence);
            continue;
        }

        // Milestone 2.9C: DecisionIntent -> authoritative ActionRequest ->
        // ActionSystem::Validate() - see ValidateDecisionIntent() for why
        // this still never calls ActionExecutor.
        ValidateDecisionIntent(response.Agent, *record, response);
    }
}

// Milestone 2.9C: translates a structured DecisionIntent into an
// authoritative ActionRequest and runs it through ActionSystem::Validate()
// - the AI-proposes/TrinityCore-decides boundary applied to the async
// decision path for the first time, not just the deterministic Goal/Action
// pipeline. Called only once Update()'s own generic gating (agent still
// registered, still Materialized as of this tick's snapshot, snapshot
// fresh, decision payload present) has already passed. Deliberately still
// never calls ActionExecutor: for FLEE, UpdateNeeds() already builds,
// validates, AND executes a FLEE ActionRequest of its own the tick
// FLEE_DANGER is Activated/Interrupted, through its own fully
// deterministic Goal/Action pipeline (see UpdateNeeds()). If this path
// also executed, a materialized FLEE_DANGER agent would have two
// independent owners racing to drive the same TrinityCore movement -
// exactly what must not happen. This stays a dry run - propose, validate,
// log - until a later milestone actually retires (or otherwise reconciles
// with) the deterministic path for whichever ActionType it takes over.
void AIWorldMgr::ValidateDecisionIntent(AgentId id, AgentRecord const& record, AIResponse const& response)
{
    DecisionIntent const& intent = response.Decision->Intent;

    if (intent.Type == DecisionIntentType::None)
    {
        TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} intent=NONE (no-op)",
            response.RequestId, id.Value, response.SnapshotSequence);

        // Milestone 2.9D P2 fix: ai.world.decision.validity is the one
        // series "invalid decision rate" is meant to be computed from -
        // INVALID/(VALID+INVALID) - and it must count every decision that
        // actually reached this point, not just the ones with an
        // ActionSystem::Validate() call behind them (see the comment above
        // the FLEE validation metric below for why ai.world.decision.
        // validation alone can't answer this). A fresh NONE is a valid
        // decision - ai-server correctly proposed doing nothing.
        TC_METRIC_VALUE("ai.world.decision.validity", uint64(1), TC_METRIC_TAG("validity", "valid"));
        return;
    }

    if (intent.Type != DecisionIntentType::Flee)
    {
        // MOVE_TO/EAT are deliberately not translated in 2.9C: GET_FOOD
        // already owns an authoritative target-resolution and Eat
        // provenance mechanism of its own (FoodTargetResolver,
        // PendingEatContinuation/ActionValidationContext::ArrivedDestination) -
        // accepting a remote MOVE_TO/EAT here would create a second,
        // competing owner of the same action. Fail-closed: reject rather
        // than guess at a translation.
        TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} intent={} rejected: unsupported remote intent",
            response.RequestId, id.Value, response.SnapshotSequence, ToString(intent.Type));

        // ai.world.decision.validation is kept as a detailed diagnostic
        // series (intent + ALLOWED/REJECTED + reason, reusing
        // ActionRejectReason::UnsupportedAction's own string even though
        // ActionSystem was never actually called - there is no
        // ActionRequest to validate for an intent this build doesn't
        // translate, but it's the same rejection category). It is NOT
        // where "invalid decision rate" should be computed from, though -
        // see ai.world.decision.validity below.
        TC_METRIC_VALUE("ai.world.decision.validation", uint64(1),
            TC_METRIC_TAG("intent", ToString(intent.Type)), TC_METRIC_TAG("result", "REJECTED"),
            TC_METRIC_TAG("validation_reason", ToString(ActionRejectReason::UnsupportedAction)));

        // Milestone 2.9D P2 fix: an unsupported remote intent is an
        // invalid decision by the roadmap's own definition (alongside
        // malformed/unknown responses and an ActionSystem REJECTED) - see
        // the FLEE validation metric below for the full series.
        TC_METRIC_VALUE("ai.world.decision.validity", uint64(1),
            TC_METRIC_TAG("validity", "invalid"), TC_METRIC_TAG("reason", "unsupported_intent"));
        return;
    }

    // The response's own SnapshotSequence match (already checked by the
    // caller before this is ever reached) only proves this agent's
    // AgentSnapshot hasn't moved on - Needs/ActiveGoal/GoalStartedAtMs/the
    // actor's threat victim can all change between two snapshot captures
    // without SnapshotSequence itself advancing (Needs/Goal update on
    // their own ~1s cadence, independent of the snapshot/decision
    // cadence). Provenance is what the world thread actually asked about
    // when this decision was requested (see DecisionProvenance.h) - if the
    // agent's goal, or this specific goal attempt, has since moved on,
    // this decision no longer answers a question that's still being
    // asked and must be discarded outright, never translated into a
    // request naming whatever goal happens to be active now (that would
    // silently launder a stale decision onto an unrelated goal attempt).
    if (!response.Provenance.Goal || !record.ActiveGoalState ||
        *response.Provenance.Goal != record.ActiveGoalState->Type ||
        response.Provenance.GoalStartedAtMs != record.ActiveGoalState->StartedAtMs)
    {
        TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} intent=FLEE discarded: STALE_CONTEXT (goal attempt changed since request)",
            response.RequestId, id.Value, response.SnapshotSequence);
        TC_METRIC_VALUE("ai.world.decision.discard", uint64(1), TC_METRIC_TAG("discard_reason", "stale_context"));
        return;
    }

    // Re-resolve the actual live Creature* rather than trusting the
    // WorldState==Materialized flag Update() already checked - that flag
    // can be a tick stale, and FLEE's own request needs a real
    // ThreatManager to ask about the actor's current threat victim. Same
    // pattern as ProcessAgent(): never force a grid to load, and no
    // pointer from here is ever stored anywhere past this call.
    Map* map = sMapMgr->FindBaseNonInstanceMap(record.MapId);
    Creature* creature = ResolveLiveCreature(record, map);
    if (!creature)
    {
        if (record.WorldState == AgentWorldState::Materialized)
            _registry.UnbindCreature(id);

        TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} intent=FLEE discarded: no live Creature resolved",
            response.RequestId, id.Value, response.SnapshotSequence);
        TC_METRIC_VALUE("ai.world.decision.discard", uint64(1), TC_METRIC_TAG("discard_reason", "not_materialized"));
        return;
    }

    // 2.9C P2 fix: a SpawnId identifies a spawn, not a runtime object (see
    // AgentRegistry::BindCreature()) - the Creature this decision was
    // actually requested about can despawn and a new Creature for the same
    // SpawnId (a different runtime incarnation, new ObjectGuid) can take
    // over the same AgentRecord/WorldState==Materialized/even the same
    // ActiveGoalState attempt, all without SnapshotSequence advancing
    // either. Goal/GoalStartedAtMs provenance above doesn't catch this -
    // RuntimeGuid (the client's own echo of the request-time
    // AgentContext::Self.Guid, see DecisionProvenance.h) must match both
    // the Creature just re-resolved and AgentRecord's own current
    // RuntimeGuid, or this decision would otherwise get validated against
    // a Creature it was never actually asked about.
    if (response.Provenance.RuntimeGuid.IsEmpty() ||
        creature->GetGUID() != response.Provenance.RuntimeGuid ||
        record.RuntimeGuid != response.Provenance.RuntimeGuid)
    {
        TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} intent=FLEE discarded: STALE_CREATURE_INSTANCE",
            response.RequestId, id.Value, response.SnapshotSequence);
        TC_METRIC_VALUE("ai.world.decision.discard", uint64(1), TC_METRIC_TAG("discard_reason", "stale_creature"));
        return;
    }

    // Resolved once here and reused for both the request's claimed
    // FleeFromGuid and the validation context's actual FleeSourceGuid -
    // the same "honesty, not trust" pattern UpdateNeeds()'s own
    // deterministic FLEE build already uses (see there for why).
    Unit* fleeSource = creature->GetThreatManager().GetCurrentVictim();

    ActionRequest request;
    request.Actor = id;
    request.Type = ActionType::Flee;
    request.SourceGoal = record.ActiveGoalState->Type;
    request.GoalStartedAtMs = record.ActiveGoalState->StartedAtMs;
    request.FleeFromGuid = fleeSource ? fleeSource->GetGUID() : ObjectGuid::Empty;

    TC_LOG_DEBUG("ai.world", "AI decision action request agent={} intent=FLEE sourceGoal={} goalStartedAt={} fleeFrom={}",
        id.Value, ToString(request.SourceGoal), request.GoalStartedAtMs, request.FleeFromGuid.ToString());

    ActionValidationContext validationContext;
    validationContext.Materialized = true;
    validationContext.Alive = creature->IsAlive();
    validationContext.ControlMode = record.ControlMode;
    validationContext.ActiveGoalType = record.ActiveGoalState->Type;
    validationContext.ActiveGoalStartedAtMs = record.ActiveGoalState->StartedAtMs;
    validationContext.FleeSourceGuid = request.FleeFromGuid;

    ActionValidationResult validation = _actionSystem.Validate(request, validationContext);

    // Dry run only - see this function's own header comment for why
    // ActionExecutor is never called here.
    TC_LOG_DEBUG("ai.world", "AI decision action validation agent={} intent=FLEE result={} reason={} execution=SUPPRESSED_EXISTING_OWNER (dry-run, deterministic pipeline owns execution)",
        id.Value, validation.Allowed ? "ALLOWED" : "REJECTED", ToString(validation.Reason));

    // ai.world.decision.validation stays the detailed diagnostic series
    // for FLEE specifically (intent + ALLOWED/REJECTED + the exact
    // ActionRejectReason).
    TC_METRIC_VALUE("ai.world.decision.validation", uint64(1),
        TC_METRIC_TAG("intent", "FLEE"), TC_METRIC_TAG("result", validation.Allowed ? "ALLOWED" : "REJECTED"),
        TC_METRIC_TAG("validation_reason", ToString(validation.Reason)));

    // Milestone 2.9D P2 fix: ai.world.decision.validation alone
    // undercounts "invalid decision rate" - its denominator is only
    // decisions that reached an ActionSystem::Validate() call, which
    // excludes every fresh NONE (never invalid, but never counted either)
    // and every unsupported remote intent/malformed response (both
    // invalid, but validation's own denominator never sees them - the
    // malformed case never even reaches AIWorldMgr, and unsupported
    // intents skip ActionSystem entirely). ai.world.decision.validity is
    // the single series meant to answer "what fraction of decisions that
    // could be judged were invalid": every fresh NONE, every FLEE
    // ALLOWED/REJECTED, every unsupported remote intent, and every
    // malformed/unknown response (see AIClient.cpp's DecisionSession::
    // Complete()) all land in exactly one of VALID or INVALID here -
    // "invalid decision rate" = INVALID/(VALID+INVALID) on this series
    // alone. Stale/discard cases (stale_snapshot/stale_context/
    // stale_creature/not_materialized) and pure transport outcomes
    // (timeout/transport_error/http_error/protocol_mismatch) deliberately
    // never appear here at all - there is no decision content to judge
    // the quality of in either case.
    if (validation.Allowed)
        TC_METRIC_VALUE("ai.world.decision.validity", uint64(1), TC_METRIC_TAG("validity", "valid"));
    else
        TC_METRIC_VALUE("ai.world.decision.validity", uint64(1),
            TC_METRIC_TAG("validity", "invalid"), TC_METRIC_TAG("reason", ToString(validation.Reason)));
}

// Bridges a registered agent to whatever its Creature is doing right now,
// without ever holding onto that Creature past this call: materializes it
// (Abstract -> Materialized) the tick it's found loaded, dematerializes it
// (Materialized -> Abstract) the tick it stops being found - the agent
// itself, and its SnapshotSequence, live in _registry across either
// transition. Never forces a grid to load. Returns the built AIRequest for
// the caller (RunDecisionScheduler(), Milestone 2.10A) to collect into a
// batch and submit itself - nullopt if this agent isn't actually
// materialized right now, whatever AgentRegistry's own WorldState flag
// claimed when it was selected as a scheduling candidate.
//
// BindCreature() is called unconditionally (not just out of Abstract): a
// SpawnId identifies a TrinityCore spawn, not a runtime object, so the old
// Creature can despawn and a new one for the same spawn appear between two
// polls without WorldState ever passing through Abstract in between.
// BindCreature() is idempotent and only actually changes anything when the
// runtime GUID doesn't already match.
std::optional<AIRequest> AIWorldMgr::ProcessAgent(AgentId id)
{
    AgentRecord* record = _registry.Find(id);
    if (!record)
        return std::nullopt;

    Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
    Creature* creature = ResolveLiveCreature(*record, map);

    if (!creature)
    {
        if (record->WorldState == AgentWorldState::Materialized)
            _registry.UnbindCreature(id);
        return std::nullopt;
    }

    _registry.BindCreature(id, *creature);

    return CaptureAgentContext(id, *record, *creature);
}

AIRequest AIWorldMgr::CaptureAgentContext(AgentId id, AgentRecord& record, Creature& creature)
{
    AgentSnapshot snapshot;
    snapshot.Agent = id;
    snapshot.SpawnId = record.SpawnId;
    snapshot.Guid = creature.GetGUID();
    snapshot.Entry = creature.GetEntry();
    snapshot.MapId = creature.GetMapId();
    snapshot.X = creature.GetPositionX();
    snapshot.Y = creature.GetPositionY();
    snapshot.Z = creature.GetPositionZ();
    snapshot.Orientation = creature.GetOrientation();
    snapshot.Health = creature.GetHealth();
    snapshot.MaxHealth = creature.GetMaxHealth();
    snapshot.Alive = creature.IsAlive();
    snapshot.InCombat = creature.IsInCombat();
    snapshot.SnapshotSequence = ++record.SnapshotSequence;

    TC_LOG_INFO("ai.agent", "snapshot agent={} seq={} spawn={} entry={} hp={}/{} position=({:.1f}, {:.1f}, {:.1f}) combat={}",
        id.Value, snapshot.SnapshotSequence, snapshot.SpawnId, snapshot.Entry,
        snapshot.Health, snapshot.MaxHealth,
        snapshot.X, snapshot.Y, snapshot.Z,
        snapshot.InCombat);

    // Milestone 2.5C/2.9A: deterministic retrieval over this agent's
    // memories, scoped to "now" and the snapshot's own position/map -
    // feeds AgentContext::RelevantMemories below.
    MemoryQueryContext memoryContext;
    memoryContext.Agent = snapshot.Agent;
    memoryContext.NowMs = CurrentTimeMs();
    memoryContext.MapId = snapshot.MapId;
    memoryContext.X = snapshot.X;
    memoryContext.Y = snapshot.Y;
    memoryContext.Z = snapshot.Z;

    std::vector<MemoryRecord> shortTermMemories = _shortTermMemory.GetActiveForAgent(snapshot.Agent, memoryContext.NowMs);
    std::vector<LongTermMemoryRecord> longTermMemories = _longTermMemory.GetForAgent(snapshot.Agent);
    std::vector<RetrievedMemory> relevantMemories = _memoryRetrieval.Retrieve(
        memoryContext, shortTermMemories, longTermMemories, _memoryRetrievalTopN);

    TC_LOG_DEBUG("ai.world",
        "AI memory retrieval agent={} snapshot={} short={} long={} selected={}",
        snapshot.Agent.Value, snapshot.SnapshotSequence, shortTermMemories.size(), longTermMemories.size(), relevantMemories.size());

    for (std::size_t rank = 0; rank < relevantMemories.size(); ++rank)
    {
        RetrievedMemory const& memory = relevantMemories[rank];
        char const* sourceEventType = memory.SourceEventType ? ToString(*memory.SourceEventType) : "NONE";

        TC_LOG_DEBUG("ai.world",
            "AI relevant memory rank={} agent={} tier={} memory={} type={} relevance={:.3f} importance={:.2f} sourceEvent={} sourceEventType={}",
            rank + 1, snapshot.Agent.Value, ToString(memory.Tier), memory.MemoryId, ToString(memory.Type),
            memory.Relevance, memory.Importance, memory.SourceEventId, sourceEventType);
    }

    // Milestone 2.9A: everything this agent is allowed to know, as one
    // pure AgentContext - Self already carries Agent/SnapshotSequence (see
    // AgentContext.h), so they aren't set again here. AvailableActions is
    // today's full ActionType catalog (see ActionType.h) - not yet
    // filtered down to what's actually valid for this agent right now.
    AgentContext context;
    context.Self = snapshot;
    context.Needs = record.Needs;
    context.Goal = record.ActiveGoalState;
    context.AvailableActions = { ActionType::Flee, ActionType::MoveTo, ActionType::Eat };

    // 2.9A P2 fix: sanitize each RetrievedMemory down to DecisionMemory
    // here, at the same world-thread boundary that builds the rest of
    // AgentContext, rather than leaving it to AIClient's JSON builder.
    context.RelevantMemories.reserve(relevantMemories.size());
    for (RetrievedMemory const& memory : relevantMemories)
        context.RelevantMemories.push_back(ToDecisionMemory(memory));

    AIRequest request;
    request.Decision.Context = std::move(context);

    // Milestone 2.10A: building the request is this function's whole job
    // now - RunDecisionScheduler() collects one of these per admitted
    // agent into a single batch and submits them together through
    // SubmitDecisionContexts().
    return request;
}

// Milestone 2.9E/2.10A: thin batch boundary between AIWorldMgr's own
// capture logic and AIClient::SubmitDecisions() - pure passthrough, no
// admission policy of its own (that's RunDecisionScheduler(), on the
// caller side), no wire format, no ActionExecutor. requests is already the
// same Creature*/Player*/Map*/Unit*-free value transport AIRequest always
// was - this never resolves a TrinityCore entity, just moves DTOs along.
// Returns AIClient's own per-agent results so the caller can update its
// scheduling state - see RunDecisionScheduler().
std::vector<DecisionSubmitResult> AIWorldMgr::SubmitDecisionContexts(std::vector<AIRequest> requests)
{
    return _aiClient->SubmitDecisions(std::move(requests));
}

// Milestone 2.10A/2.10B: world-thread-only bounded admission over every
// registered agent - replaces the old single-_testAgentId cadence now that
// Needs/Goal/Action (UpdateNeeds()) and perception (ScanNearbyEntities())
// already iterate the full registry themselves; this was the one part of
// the pipeline still gated behind a single hardcoded agent. Never touches
// ActionExecutor and never changes FLEE/GET_FOOD's existing deterministic
// execution ownership (see UpdateNeeds()) - this only decides which
// agents' AgentContext gets submitted for an (still audit-only, see
// ValidateDecisionIntent()) async decision this pass. No unbounded queue:
// an agent skipped for capacity is simply left due for the next pass,
// never buffered anywhere.
//
// Milestone 2.10B: this pass's own cadence (AIWorld.DecisionSchedulerIntervalMs,
// default 250ms) is deliberately much faster than either per-agent
// decision interval it schedules against (DecisionNearbyIntervalMs/
// DecisionActiveIntervalMs, default 1000/5000ms) - a fine-grained polling
// cadence over coarse-grained per-agent intervals, not one cadence doing
// both jobs the way 2.10A's single _snapshotIntervalMs did. This is what
// lets a player walking up to an agent get that agent onto the faster
// Nearby cadence within one poll interval, instead of waiting for
// whatever the previous (possibly much longer) interval happened to be.
//
// Milestone 2.10C: also the one place SimulationTier (see SimulationTier.h)
// gets computed and logged for every registered agent, decision-eligible
// or not - Background agents are observable via UpdateSimulationTier() the
// same as Active/Nearby ones, they just never become a
// DecisionScheduler::Candidate. This does not add any simulation of its
// own for Background agents (no Needs drift, no economy) - purely
// lifecycle observability on top of the existing Materialized/Abstract
// bind/unbind bookkeeping this loop was already doing.
//
// Milestone 2.10D: Background agents now also get their own coarse tick
// (AIWorld.BackgroundSimulationIntervalMs, default 60s) on top of the tier
// logging above - still purely observability ("AI simulation tick
// agent=... tier=... dt=...ms"), not a second simulation path: no
// Map/Creature lookup beyond the one this loop already performs to find
// out an agent has no live Creature in the first place, no /decision, no
// ActionExecutor, no Needs/goal/memory change. Materialized (Active/
// Nearby) agents never touch this - they already have the real decision
// scheduler above. The seam later background-simulation milestones are
// meant to build on, not the simulation itself.
//
// Milestone 2.10D P2 fix: the coarse tick is bounded and deterministically
// ordered the same way the decision scheduler already is
// (CoarseSimulationScheduler::SelectDue(), admitted up to
// AIWorld.CoarseSimulationMaxPerPass per pass) - an earlier version ticked
// every due Background agent in the same pass with no cap, which both
// spikes world-thread work at scale and, since every one of them would
// then share the exact same LastTickAtMs, permanently phase-locks them
// into ticking together forever afterwards. UpdateSimulationTier()'s
// return value also drives a second fix here: SimulationScheduleState::
// LastTickAtMs is reset to "now" (not left at whatever it was) the moment
// an agent (re-)enters Background, so a coarse dt/due-time can never reach
// back across a stretch spent Materialized in between two Background
// stints, or across the moment the agent was first observed.
//
// Milestone 2.12D P2 fix (STATIC review): AgentGroups no longer take part
// in any of the above - see this function's own group coarse-tick loop,
// entirely separate from the per-agent SimulationTier/CoarseSimulationScheduler
// machinery this comment describes (a group is not an AgentRecord any
// more, see GroupId.h).
void AIWorldMgr::RunDecisionScheduler()
{
    uint64 nowMs = CurrentTimeMs();

    // Milestone 2.10B: the live Creature is already needed here for
    // GetPlayerListInGrid() (proximity classification), so bind/unbind
    // bookkeeping happens in this same loop too - the same idempotent
    // pattern ProcessAgent()/ScanNearbyEntities()/UpdateNeeds() each
    // already follow independently. Classification itself is never
    // persisted (see DecisionCadenceClass.h) - purely a function of this
    // pass's own world-thread-resolved proximity, so a player walking up
    // to (or away from) an agent changes its class, and therefore its
    // cadence, the very next scheduler pass, with no agent-side state to
    // migrate and no restart required.
    std::vector<DecisionScheduler::Candidate> candidates;
    std::vector<AgentId> coarseCandidates;
    for (AgentId id : _registry.GetAgents())
    {
        AgentRecord* record = _registry.Find(id);
        if (!record)
            continue;

        Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
        Creature* creature = ResolveLiveCreature(*record, map);

        if (!creature)
        {
            if (record->WorldState == AgentWorldState::Materialized)
                _registry.UnbindCreature(id);

            // Milestone 2.10C: BACKGROUND - no live Creature at all, so
            // this agent is never a decision candidate: no ProcessAgent(),
            // no AIClient call, no grid force-load.
            SimulationTier tier = DeriveSimulationTier(AgentWorldState::Abstract, false);
            bool tierChanged = UpdateSimulationTier(id, tier);

            // Milestone 2.10D P2 fixes: reset the coarse-tick epoch
            // exactly when this agent (re-)enters Background -
            // from any other tier, or from never having a tier recorded
            // at all - so LastTickAtMs (and therefore any dt computed from
            // it) never reaches back across a stretch spent Materialized
            // in between, or across the moment this agent was first ever
            // observed. NextTickAtMs gets a one-time phase offset here
            // too (StableAgentHash(id) % interval, never a plain
            // AgentId % interval - see StableAgentHash.h for why that
            // would still cluster consecutive AgentIds into the same
            // scheduler pass) so a batch of agents entering the tier at
            // the same moment (e.g. a mass grid unload) do not all pile
            // onto the same due time forever afterwards - only entry gets
            // this treatment, a real tick below always resumes the plain
            // "+interval" cadence. See SimulationScheduleState.h for the
            // full reasoning. Only ever collected as a candidate here -
            // the actual tick (bounded, deterministic) happens after this
            // loop.
            if (tierChanged)
            {
                uint64 phaseMs = StableAgentHash(id.Value) % _backgroundSimulationIntervalMs;

                SimulationScheduleState& scheduleState = _simulationSchedule[id.Value];
                scheduleState.LastTickAtMs = nowMs;
                scheduleState.NextTickAtMs = nowMs + phaseMs;
            }

            coarseCandidates.push_back(id);
            continue;
        }

        _registry.BindCreature(id, *creature);

        std::vector<Player*> nearbyPlayers;
        creature->GetPlayerListInGrid(nearbyPlayers, _decisionNearbyPlayerRange);
        bool isNearby = !nearbyPlayers.empty();

        // Milestone 2.10C: ACTIVE/NEARBY - only these two tiers are ever
        // decision-eligible, and DecisionCadenceClass's own two values
        // (used purely for scheduler admission ranking, see
        // DecisionScheduler.h) map onto them 1:1.
        UpdateSimulationTier(id, DeriveSimulationTier(AgentWorldState::Materialized, isNearby));

        DecisionCadenceClass cadenceClass = isNearby ? DecisionCadenceClass::Nearby : DecisionCadenceClass::Active;

        // Milestone 2.12F4A: ControlMode gate (performance/early-rejection -
        // ActionSystem::Validate() is the actual mandatory safety boundary
        // regardless, see its own comment). BindCreature()/simulation-tier
        // tracking above still runs for every materialized agent
        // unconditionally - state-tracking is allowed for ObserveOnly - but
        // an ObserveOnly agent is never scheduled for a remote /decision
        // call, which could never lead anywhere but a request
        // ActionSystem would reject anyway.
        if (record->ControlMode == AgentControlMode::AIWorldControlled)
            candidates.push_back({ id, cadenceClass });
    }

    // Milestone 2.10D P2 fixes: bounded, deterministic admission for the
    // coarse tick - see CoarseSimulationScheduler.h for why an unbounded
    // "every due agent ticks this pass" design both spikes world-thread
    // work with enough Background agents and permanently
    // phase-locks them onto the same pass once they do. A
    // capacity-skipped agent's NextTickAtMs is left untouched, so it stays
    // at the front of the due set next pass rather than losing its turn -
    // the same no-unbounded-queue guarantee the decision scheduler already
    // gives Nearby/Active agents.
    CoarseSimulationScheduler::SelectionResult coarseSelection = _coarseSimulationScheduler.SelectDue(
        _simulationSchedule, coarseCandidates, nowMs, _coarseSimulationMaxPerPass);

    for (AgentId id : coarseSelection.Admitted)
    {
        SimulationScheduleState& tickState = _simulationSchedule[id.Value];
        uint64 dtMs = tickState.LastTickAtMs != 0 ? nowMs - tickState.LastTickAtMs : 0;

        // _agentSimulationTier was already updated for every candidate in
        // the loop above, in this same pass - safe to look up directly
        // rather than re-deriving it.
        auto tierIt = _agentSimulationTier.find(id.Value);
        SimulationTier tier = tierIt != _agentSimulationTier.end() ? tierIt->second : SimulationTier::Background;

        TC_LOG_DEBUG("ai.world", "AI simulation tick agent={} tier={} dt={}ms", id.Value, ToString(tier), dtMs);

        // Runtime review P2 fix: a real tick always resumes the plain
        // "+interval" cadence - no phase offset here, that is applied only
        // once, on tier entry above, via StableAgentHash.
        tickState.LastTickAtMs = nowMs;
        tickState.NextTickAtMs = nowMs + _backgroundSimulationIntervalMs;
    }

    // Milestone 2.12D P2 fix (STATIC review): AgentGroups get their own
    // coarse tick here, entirely separate from the per-agent loop above -
    // a group is no longer an AgentRecord/SimulationTier candidate at all
    // (see GroupId.h). Bounded and deterministically ordered via
    // GroupCoarseSimulationScheduler - its own GroupId-keyed sibling to
    // CoarseSimulationScheduler above, not a reuse of it (see its header
    // comment for why an earlier, unbounded version of this same loop was
    // rejected by review once dynamic LOOSE coalitions - not a small fixed
    // set of scripted groups - entered the picture). Phase-offset on first
    // sight (StableAgentHash, same reasoning as the per-agent loop above)
    // so a batch of groups loaded at startup does not all pile onto the
    // same due time forever afterwards, computed for every candidate
    // before admission so a capacity-skipped group still gets its one-time
    // offset this pass rather than being reconsidered "never scheduled"
    // again next pass. Runs unconditionally for every admitted group
    // (2.12D P2 fix) - no longer gated on whether any member happens to be
    // materialized right now, see AgentGroupRecord.h for why that pausing
    // was itself a symptom of the aggregate-replaces-members model this
    // rename was meant to remove.
    std::vector<GroupId> groupCandidates = _groupRegistry.GetGroups();
    for (GroupId groupId : groupCandidates)
    {
        SimulationScheduleState& scheduleState = _groupSimulationSchedule[groupId.Value];
        if (scheduleState.LastTickAtMs == 0 && scheduleState.NextTickAtMs == 0)
        {
            uint64 phaseMs = StableAgentHash(groupId.Value) % _groupSimulationIntervalMs;
            scheduleState.LastTickAtMs = nowMs;
            scheduleState.NextTickAtMs = nowMs + phaseMs;
        }
    }

    GroupCoarseSimulationScheduler::SelectionResult groupSelection = _groupCoarseSimulationScheduler.SelectDue(
        _groupSimulationSchedule, groupCandidates, nowMs, _groupSimulationMaxPerPass);

    for (GroupId groupId : groupSelection.Admitted)
    {
        AgentGroupRecord* group = _groupRegistry.Find(groupId);
        if (!group)
            continue;

        // 2.12E4C2 P2 fix, round 3 (STATIC review): a RunGroupProfileAdoption()
        // write is in flight for this group - skip it entirely this pass
        // (no Update(), no SaveGroupState(), schedule state left
        // untouched so it stays due and is retried the very next group
        // coarse tick pass, the same "skip capacity, don't advance,
        // nothing here starves it" treatment a capacity-skipped group
        // already gets from GroupCoarseSimulationScheduler itself) rather
        // than risk this call's own SaveGroupState() committing its
        // whole-row snapshot (built from whatever ProfileId the RAM copy
        // held before the adoption's own completion updates it) either
        // before or after the adoption's own targeted UPDATE - see
        // _groupProfileAdoptionInFlight's own declaration comment for the
        // exact divergence this prevents.
        if (_groupProfileAdoptionInFlight.count(groupId.Value))
            continue;

        SimulationScheduleState& scheduleState = _groupSimulationSchedule[groupId.Value];
        uint64 dtMs = scheduleState.LastTickAtMs != 0 ? nowMs - scheduleState.LastTickAtMs : 0;

        AgentGroupRuntimeView runtimeView = ResolveAgentGroupRuntimeView(_registry, *group);

        TC_LOG_DEBUG("ai.world", "AI agent group presence group={} totalMembers={} loadedMembers={}",
            groupId.Value, runtimeView.TotalMembers, runtimeView.LoadedMembers);

        // Milestone 2.12D P3 fix (confirmed from a live log - resources
        // sitting at its 0.0 floor across many consecutive ticks, version
        // still climbing every pass): once Resources has settled at either
        // clamp bound, AgentGroupSimulationSystem::Update() is a no-op, so
        // persisting again would only bump Version and issue an async DB
        // write for a row that is otherwise byte-identical to what is
        // already stored. With one group this is noise; with many (dynamic
        // LOOSE coalitions - see GroupCoarseSimulationScheduler.h) it is
        // permanent, unbounded per-group DB churn for no observable state
        // change. Only SaveGroupState() (and its own Version bump) is
        // skipped - LastTickAtMs/NextTickAtMs below still advance on the
        // normal cadence either way, so a group that starts changing again
        // resumes persisting on its very next due tick, nothing here
        // latches "never check again".
        //
        // Milestone 2.12D P3 fix (STATIC review, round 2): "did it change"
        // comes from Update()'s own return value, not a Resources-only
        // comparison kept here - this class has no business knowing which
        // AgentGroupRecord fields AgentGroupSimulationSystem does or does
        // not touch; see that class's own header comment for why.
        bool changed = _agentGroupSimulationSystem.Update(*group, dtMs, _agentGroupSimulationRates);

        if (changed)
        {
            // Version bump happens inside SaveGroupState() itself (2.11E2
            // P3's "bump lives in the persistence API, not the caller"
            // precedent) - logged after, not before, so this reflects the
            // value actually being persisted.
            _groupPersistence.SaveGroupState(groupId, *group);

            TC_LOG_DEBUG("ai.world", "AI agent group simulation group={} kind={} members={} resources={:.4f} version={}",
                groupId.Value, ToString(group->Kind), runtimeView.TotalMembers, group->Resources, group->Version);
        }
        else
        {
            TC_LOG_DEBUG("ai.world", "AI agent group simulation group={} kind={} members={} resources={:.4f} unchanged, not persisted",
                groupId.Value, ToString(group->Kind), runtimeView.TotalMembers, group->Resources);
        }

        // Runtime review P2 fix precedent (see the per-agent loop above):
        // a real tick always resumes the plain "+interval" cadence - no
        // phase offset here, that is applied only once, on first sight,
        // above.
        scheduleState.LastTickAtMs = nowMs;
        scheduleState.NextTickAtMs = nowMs + _groupSimulationIntervalMs;
    }

    uint32 inFlight = 0;
    for (auto const& entry : _decisionSchedule)
        if (entry.second.AwaitingResponse)
            ++inFlight;

    uint32 available = _decisionMaxInFlight > inFlight ? _decisionMaxInFlight - inFlight : 0;

    // Milestone 2.10B P2 fix: nearbyIntervalMs/activeIntervalMs are passed
    // in so SelectDue() can recompute each candidate's effective due time
    // from its CURRENT class every call, rather than trusting a deadline
    // locked in at whatever class was true the last time it was admitted -
    // see DecisionScheduleState.h/DecisionScheduler.h for why.
    DecisionScheduler::SelectionResult selection = _decisionScheduler.SelectDue(_decisionSchedule, candidates, nowMs,
        available, _decisionNearbyIntervalMs, _decisionActiveIntervalMs);

    // Milestone 2.9D-style observability: a capacity skip is visible
    // through the same ai.world.decision.submit series SubmitDecision()
    // itself already uses for submitted/skipped_in_flight - these agents
    // never even reach AIClient, so nothing else would ever record them.
    // No AgentId in the tag (low-cardinality metric tags only, per 2.9D) -
    // which agent got skipped is what the TC_LOG_DEBUG batch summary below
    // is for.
    for (std::size_t i = 0; i < selection.SkippedCapacity.size(); ++i)
        TC_METRIC_VALUE("ai.world.decision.submit", uint64(1), TC_METRIC_TAG("result", "skipped_capacity"));

    std::vector<AIRequest> requests;
    requests.reserve(selection.Admitted.size());

    for (AgentId id : selection.Admitted)
    {
        std::optional<AIRequest> request = ProcessAgent(id);
        if (!request)
        {
            // AgentRegistry's WorldState flag was already stale by the
            // time ProcessAgent() actually tried to resolve a live
            // Creature for it. Still stamps LastDecisionSubmittedAtMs
            // (2.10B P2 fix) even though nothing was actually submitted -
            // otherwise this agent would be immediately due again (0 means
            // "never attempted") on the very next, much shorter (2.10B)
            // scheduler pass instead of waiting a normal interval; the
            // effective-due recompute in SelectDue() means it still gets
            // retried sooner if it becomes Nearby in the meantime, just
            // not on literally every single poll.
            _decisionSchedule[id.Value].LastDecisionSubmittedAtMs = nowMs;
            continue;
        }

        requests.push_back(std::move(*request));
    }

    std::vector<DecisionSubmitResult> results = SubmitDecisionContexts(std::move(requests));

    uint32 submitted = 0;
    for (DecisionSubmitResult const& result : results)
    {
        // SkippedInFlight shouldn't actually happen here in today's
        // single-scheduler-caller world - this pass's own inFlight count
        // is computed from the same AwaitingResponse bookkeeping that
        // gates admission, so it should already agree with AIClient's own
        // counter. Handled anyway (AIClient's admission is authoritative,
        // never just trusted from this pass's own estimate): leaves
        // LastDecisionSubmittedAtMs untouched, so this agent's existing
        // effective-due time (whatever last got it admitted) still applies
        // next pass rather than losing its turn.
        if (result.Status != DecisionSubmitStatus::Submitted)
            continue;

        DecisionScheduleState& state = _decisionSchedule[result.Agent.Value];
        state.LastDecisionSubmittedAtMs = nowMs;
        state.AwaitingResponse = true;
        ++submitted;
    }

    TC_LOG_DEBUG("ai.world", "AI decision batch eligible={} admitted={} capacity_skipped={} in_flight={}",
        selection.Admitted.size() + selection.SkippedCapacity.size(), submitted, selection.SkippedCapacity.size(), inFlight + submitted);
}

// Milestone 2.10C/2.10D P2 fix: records tier as this agent's current
// SimulationTier and logs iff that is actually a change (or the first
// tier ever observed for it) - called once per RunDecisionScheduler()
// pass for every registered agent, regardless of whether it ends up
// decision-eligible, so Background agents are just as observable
// as Active/Nearby ones. Never touches AgentRecord/AgentRegistry -
// _agentSimulationTier is its own bookkeeping, same reasoning as
// _decisionSchedule. Returns whether this call actually produced a new or
// changed tier assignment (initial or real transition alike) - the
// caller uses that to know when to reset the coarse-tick epoch in
// _simulationSchedule (see RunDecisionScheduler()'s own comment), which
// this function itself never touches.
bool AIWorldMgr::UpdateSimulationTier(AgentId id, SimulationTier tier)
{
    auto it = _agentSimulationTier.find(id.Value);
    if (it == _agentSimulationTier.end())
    {
        _agentSimulationTier.emplace(id.Value, tier);
        TC_LOG_DEBUG("ai.world", "AI simulation tier agent={} initial tier={}", id.Value, ToString(tier));
        return true;
    }

    if (it->second == tier)
        return false;

    SimulationTier previous = it->second;
    it->second = tier;

    TC_LOG_DEBUG("ai.world", "AI simulation tier agent={} from={} to={} reason={}",
        id.Value, ToString(previous), ToString(tier), DeriveTransitionReason(previous, tier));
    return true;
}

// Safe to call from any thread - see the declaration in AIWorldMgr.h and
// EventBus's own comments for why. Deliberately does nothing beyond the
// atomic check and the publish itself: no Creature/AgentRecord lookups, no
// ai-server calls, no world mutation. That happens later, in
// ProcessWorldEvent() on the world thread.
void AIWorldMgr::PublishWorldEvent(WorldEvent event)
{
    if (!_acceptEvents.load(std::memory_order_acquire))
        return;

    _eventBus.Publish(std::move(event));
}

// Safe to call from any thread - see the declaration in AIWorldMgr.h and
// ActionEngineEventBus's own comments for why. Same _acceptEvents gate as
// PublishWorldEvent(), for the same reason. Deliberately does nothing
// beyond the atomic check and the publish itself: no Creature/AgentRecord
// lookups, no ai-server calls, no world mutation. That happens later, in
// ProcessActionEngineEvent() on the world thread.
void AIWorldMgr::PublishActionEngineEvent(ActionEngineEvent event)
{
    if (!_acceptEvents.load(std::memory_order_acquire))
        return;

    _actionEngineEventBus.Publish(std::move(event));
}

// Safe to call from a map-updater thread - see the declaration comment in
// AIWorldMgr.h for why. Read-only: never mutates _registry, never touches
// Creature/Map, never calls ai-server. _enabled is a plain (non-atomic)
// bool that, unlike _acceptEvents, is normally only ever touched from the
// world thread - this is its one exception. In practice Initialize()'s
// write happens before the update loop (and therefore before any
// map-updater thread exists) and Shutdown()'s write happens as maps are
// torn down, so there is effectively no window where this races a
// concurrent write; even in the unverified worst case, a stale read here
// is harmless the same way a stale _acceptEvents read is (see Shutdown()'s
// comment) - it either falls through to normal AI selection or does one
// extra harmless registry lookup, never a crash or a mutation.
bool AIWorldMgr::OwnsSpawn(uint32 mapId, uint64 spawnId) const
{
    if (!_enabled)
        return false;

    // Milestone 2.12D P2 fix (STATIC review): every AgentRecord now names
    // a real, individually-bindable creature spawn - an AgentGroup is not
    // an AgentRecord any more (see GroupId.h), so there is no exclusion
    // left to apply here.
    AgentRecord const* record = _registry.FindBySpawn(mapId, spawnId);
    if (!record)
        return false;

    // Milestone 2.12F4A: existence of an AgentRecord is no longer
    // sufficient by itself - see AgentControlMode's own comment (AgentType.h)
    // for why. An ObserveOnly agent falls through to TrinityCore's normal
    // pet/scripted/AIName/Permissible AI selection exactly as if it had no
    // AgentRecord at all; this read is safe from the map-updater thread
    // this method can be called from (see this method's own declaration
    // comment in AIWorldMgr.h) for the same reason record != nullptr
    // already was - ControlMode is only ever set at load time in this
    // milestone (no runtime transition command yet), never mutated
    // concurrently with a map-updater thread's own read here.
    return record->ControlMode == AgentControlMode::AIWorldControlled;
}

// World thread only (called from Update(), right after EventBus::Drain()).
// Enriches Actor/Target with whatever AgentId _registry currently has for
// their SpawnId - this is the earliest point that lookup is safe to do,
// since whoever published the event (a map/combat worker) must not touch
// _registry itself. Still no Memory, no agent reaction - Milestone 2.4A
// only adds the witnessed-event -> Observation step below the debug log.
void AIWorldMgr::ProcessWorldEvent(WorldEvent& event)
{
    if (event.Actor.SpawnId)
    {
        if (AgentRecord* agent = FindLiveAgentBySpawn(_registry, event.Location.MapId, event.Actor.SpawnId))
            event.Actor.Agent = agent->Id;
    }

    if (event.Target.SpawnId)
    {
        if (AgentRecord* agent = FindLiveAgentBySpawn(_registry, event.Location.MapId, event.Target.SpawnId))
            event.Target.Agent = agent->Id;
    }

    TC_LOG_DEBUG("ai.world",
        "AI event id={} type={} corr={} cause={} map={} actorGuid={} actorSpawn={} actorAgent={} targetGuid={} targetEntry={} targetSpawn={} targetAgent={}",
        event.EventId, ToString(event.Type), event.CorrelationId, event.CauseEventId, event.Location.MapId,
        event.Actor.Guid.ToString(), event.Actor.SpawnId, event.Actor.Agent.Value,
        event.Target.Guid.ToString(), event.Target.Entry, event.Target.SpawnId, event.Target.Agent.Value);

    // Linear over every registered agent - fine for the single-digit/dozens
    // agent counts this milestone targets. Worth a spatial index only once
    // there are hundreds+ agents; premature before that.
    for (AgentId id : _registry.GetAgents())
    {
        AgentRecord* record = _registry.Find(id);
        if (!record)
            continue;

        if (record->MapId != event.Location.MapId)
            continue;

        // Whether a Creature actually exists right now is the authority
        // for whether this agent can perceive anything - not
        // record->WorldState, which is only as fresh as the last poll that
        // happened to touch this agent (RunDecisionScheduler()/
        // ScanNearbyEntities()/UpdateNeeds() each bind/unbind
        // independently, on their own cadences). Gating on WorldState here
        // would produce false-negative perception for however long a grid
        // can be loaded before the next poll catches up: exactly the gap
        // that must not exist going into Memory, where it would show up as
        // random, poll-timing-dependent holes rather than a real absence
        // of perception.
        Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
        Creature* observer = ResolveLiveCreature(*record, map);

        if (!observer)
        {
            if (record->WorldState == AgentWorldState::Materialized)
                _registry.UnbindCreature(id);
            continue;
        }

        // Bring the registry in line with what was actually just found -
        // BindCreature() is idempotent, so this is a no-op unless the
        // agent was still Abstract or bound to a stale RuntimeGuid.
        _registry.BindCreature(id, *observer);

        if (std::optional<Observation> observation = _perception.ObserveEvent(id, *observer, event, float(_perceptionSightRange)))
            ProcessObservation(*observation);
    }
}

// World thread only, on its own ~1s cadence (_nearbyPerceptionIntervalMs),
// independent of any WorldEvent - Milestone 2.4B/2.4C. Same "live Creature
// existence is the authority, not record->WorldState" rule as
// ProcessWorldEvent()'s perception loop, for the same reason: trusting
// WorldState here would reintroduce the exact false-negative gap that was
// just closed there.
void AIWorldMgr::ScanNearbyEntities()
{
    for (AgentId id : _registry.GetAgents())
    {
        AgentRecord* record = _registry.Find(id);
        if (!record)
            continue;

        Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
        Creature* observer = ResolveLiveCreature(*record, map);

        if (!observer)
        {
            if (record->WorldState == AgentWorldState::Materialized)
                _registry.UnbindCreature(id);
            continue;
        }

        _registry.BindCreature(id, *observer);

        std::vector<Player*> players;
        observer->GetPlayerListInGrid(players, float(_perceptionSightRange));

        for (Player* player : players)
        {
            if (std::optional<Observation> observation = _perception.ObserveNearbyPlayer(id, *observer, *player, float(_perceptionSightRange)))
                ProcessObservation(*observation);
        }

        // Default-constructed FindCreatureOptions: no filters. Final
        // alive/self/range/LOS gating happens in ObserveNearbyCreature(),
        // not here.
        std::vector<Creature*> creatures;
        observer->GetCreatureListWithOptionsInGrid(creatures, float(_perceptionSightRange), FindCreatureOptions{});

        for (Creature* seen : creatures)
        {
            std::optional<Observation> observation = _perception.ObserveNearbyCreature(id, *observer, *seen, float(_perceptionSightRange));
            if (!observation)
                continue;

            // PerceptionSystem never touches AgentRegistry - if the seen
            // creature is itself a registered agent, that enrichment
            // happens here, the same way ProcessWorldEvent() enriches
            // Actor/Target for a WorldEvent.
            if (observation->Target.SpawnId)
            {
                if (AgentRecord* seenAgent = FindLiveAgentBySpawn(_registry, observation->Location.MapId, observation->Target.SpawnId))
                    observation->Target.Agent = seenAgent->Id;
            }

            ProcessObservation(*observation);
        }
    }
}

// World thread only, on its own ~1s cadence (_needsUpdateIntervalMs),
// independent of the decision scheduler's cadence - Milestone
// 2.6A/2.6B1/2.6B2. Only
// Materialized agents drift; Abstract agents are frozen rather than
// dead-reckoned, so this deliberately does not become background
// simulation before that's its own milestone. record->WorldState is only
// as fresh as the last poll that touched this agent (ProcessAgent()/
// ProcessWorldEvent()/ScanNearbyEntities()) - same
// live-Creature-existence-is-the-authority rule as those, so a live lookup
// is required here too rather than trusting the flag. The lookup itself
// stays in AIWorldMgr on the world thread; NeedsSystem only ever sees the
// plain-value NeedsUpdateContext built from it, never the Creature*.
void AIWorldMgr::UpdateNeeds(uint32 elapsedMs)
{
    uint64 nowMs = CurrentTimeMs();

    for (AgentId id : _registry.GetAgents())
    {
        AgentRecord* record = _registry.Find(id);
        if (!record)
            continue;

        Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
        Creature* creature = ResolveLiveCreature(*record, map);

        if (!creature)
        {
            // Milestone 2.8F: whatever engine movement ActiveActionState
            // claims is running went away along with the Creature - clear
            // it rather than let it keep claiming a movement that no
            // longer exists. ActiveGoalState is left alone; per existing
            // rules it may still survive a rematerialize, but 2.8F does not
            // automatically re-plan a new action for it on reload.
            if (record->ActiveActionState)
            {
                TC_LOG_DEBUG("ai.world", "AI action completion agent={} type={} status={} reason={} sourceGoal={}",
                    record->Id.Value, ToString(record->ActiveActionState->Type), ToString(ActionCompletionStatus::Cancelled),
                    ToString(ActionCompletionReason::ActorDematerialized), ToString(record->ActiveActionState->SourceGoal));
                record->ActiveActionState.reset();
            }

            // Milestone 2.12F2: same reasoning - a dematerialized agent's
            // own in-flight Regroup attempt (if any) is over too. Harmless
            // no-op when there was none.
            record->GroupCoordinationGoalState.reset();

            // Milestone 2.8G P2 fix: a pending Eat continuation for a
            // Creature that no longer resolves has nothing left to
            // validate against (TryEat() needs a live Creature&) - drop it
            // rather than let it linger for a future re-materialize.
            record->PendingEat.reset();

            // Milestone 2.11D: unlike ActiveGoalState/RoutineGoalState (both
            // deliberately left alone here - they are intent, allowed to
            // survive a rematerialize), WORK/REST requires Materialized as
            // one of its own reality checks (see RoutineActivityContext.h)
            // - it must not keep claiming an activity for an agent that no
            // longer has a live Creature at all.
            if (record->RoutineActivityState)
            {
                TC_LOG_DEBUG("ai.world", "AI routine activity agent={} from={} to=NONE reason=DEMATERIALIZED",
                    record->Id.Value, ToString(record->RoutineActivityState->Type));
                record->RoutineActivityState.reset();
            }

            if (record->WorldState == AgentWorldState::Materialized)
                _registry.UnbindCreature(id);
            continue;
        }

        _registry.BindCreature(id, *creature);

        NeedsUpdateContext context;
        context.Health = creature->GetHealth();
        context.MaxHealth = creature->GetMaxHealth();
        context.Alive = creature->IsAlive();
        context.InCombat = creature->IsInCombat();

        // Same deterministic retrieval pipeline CaptureAgentContext()
        // already runs at snapshot cadence - reused here at Needs cadence
        // to turn recent dangerous memories into a danger signal that
        // outlives combat itself (2.6B2). Retrieval::Relevance is not used
        // here - see NeedsSystem::EvaluateMemorySafety()'s comment for why.
        MemoryQueryContext memoryContext;
        memoryContext.Agent = id;
        memoryContext.NowMs = nowMs;
        memoryContext.MapId = creature->GetMapId();
        memoryContext.X = creature->GetPositionX();
        memoryContext.Y = creature->GetPositionY();
        memoryContext.Z = creature->GetPositionZ();

        std::vector<MemoryRecord> shortTermMemories = _shortTermMemory.GetActiveForAgent(id, nowMs);
        std::vector<LongTermMemoryRecord> longTermMemories = _longTermMemory.GetForAgent(id);

        // Deliberately NOT _memoryRetrievalTopN: that limit exists to keep
        // a decision context small, and truncating to it here could rank a
        // recent dangerous memory below the cut (general relevance, not
        // danger, decides the ranking) and hide it from
        // EvaluateMemorySafety() entirely. Safety must see every retained
        // memory, so the limit is sized to never truncate.
        std::size_t safetyMemoryLimit = shortTermMemories.size() + longTermMemories.size();
        std::vector<RetrievedMemory> safetyMemories = _memoryRetrieval.Retrieve(
            memoryContext, shortTermMemories, longTermMemories, safetyMemoryLimit);

        context.MemorySafetyPressure = _needsSystem.EvaluateMemorySafety(safetyMemories, nowMs, _shortTermMemoryTtlMs);

        _needsSystem.Update(record->Needs, context, elapsedMs, _needsRates);

        TC_LOG_DEBUG("ai.world",
            "AI needs agent={} dt={}ms alive={} inCombat={} memorySafety={:.4f} healthPressure={:.4f} hunger={:.4f} fatigue={:.4f} safetyPressure={:.4f} resourcePressure={:.4f}",
            record->Id.Value, elapsedMs, context.Alive, context.InCombat, context.MemorySafetyPressure,
            record->Needs.HealthPressure, record->Needs.Hunger, record->Needs.Fatigue,
            record->Needs.SafetyPressure, record->Needs.ResourcePressure);

        // Milestone 2.6C: audit/debug log only - no Goal System, no
        // ActionRequest, no EventBus publish. An agent's own hunger/danger
        // is not automatically observable by anyone else, so this
        // deliberately never becomes a WorldEvent.
        for (NeedsThresholdEvent const& event : _needsSystem.EvaluateThresholds(record->Needs, record->NeedsThresholds))
        {
            TC_LOG_DEBUG("ai.world", "AI needs threshold agent={} type={} value={:.4f}",
                record->Id.Value, ToString(event.Type), event.Value);
        }

        // Milestone 2.7B1 P2 fix: a dead agent must not have a Goal.
        // HealthPressure/SafetyPressure/Hunger freeze at whatever value
        // they held at the moment of death (NeedsSystem::Update()'s early
        // return - see the roadmap's 2.6B1 open question), so without this
        // check a frozen SafetyPressure=1.0/Hunger=1.0 would keep
        // generating FLEE_DANGER/GET_FOOD candidates - and an already-
        // active goal would just sit there - for a corpse. Skip candidate
        // generation and goal selection entirely for this tick; any
        // ActiveGoal is released outright rather than left to decay
        // through the normal retention check.
        if (!context.Alive)
        {
            // Milestone 2.8F P2 fix: TrinityCore's own death handling
            // (MotionMaster::StopOnDeath()) already stops/clears the
            // actor's movement, so ActiveActionState must not keep
            // claiming a MOVE_TO that no longer exists engine-side.
            // Checked before the ActiveGoalState release below, same
            // ordering as the dematerialization cleanup above.
            if (record->ActiveActionState)
            {
                TC_LOG_DEBUG("ai.world", "AI action completion agent={} type={} status={} reason={} sourceGoal={}",
                    record->Id.Value, ToString(record->ActiveActionState->Type), ToString(ActionCompletionStatus::Cancelled),
                    ToString(ActionCompletionReason::ActorDead), ToString(record->ActiveActionState->SourceGoal));
                record->ActiveActionState.reset();
            }

            // Milestone 2.12F2: same reasoning as ActiveGoalState below - a
            // corpse has no in-flight Regroup attempt either. Harmless
            // no-op when there was none.
            record->GroupCoordinationGoalState.reset();

            if (record->ActiveGoalState)
            {
                TC_LOG_DEBUG("ai.world", "AI goal transition agent={} transition={} goal={} reason=DEAD",
                    record->Id.Value, ToString(GoalTransition::Released), ToString(record->ActiveGoalState->Type));
                record->ActiveGoalState.reset();
            }

            // Milestone 2.8G P2 fix: same reasoning as ActiveGoalState
            // above - a dead agent must not have a pending Eat either.
            record->PendingEat.reset();

            // Milestone 2.11B: same reasoning again - a corpse has no
            // routine destination either.
            if (record->RoutineGoalState)
            {
                TC_LOG_DEBUG("ai.world", "AI routine transition agent={} from={} to=NONE reason=DEAD",
                    record->Id.Value, ToString(record->RoutineGoalState->Type));
                record->RoutineGoalState.reset();
            }

            // Milestone 2.11D: same reasoning again - a corpse is not WORK
            // or REST either.
            if (record->RoutineActivityState)
            {
                TC_LOG_DEBUG("ai.world", "AI routine activity agent={} from={} to=NONE reason=DEAD",
                    record->Id.Value, ToString(record->RoutineActivityState->Type));
                record->RoutineActivityState.reset();
            }

            continue;
        }

        // Milestone 2.8F P2 fix: reconciliation for a completion that never
        // arrived as an event - ActionEngineEventBus is a bounded queue and
        // drops under overflow, which is acceptable for a perception event
        // but not for a lifecycle transition; this also catches any other
        // way the engine's movement state could stop matching
        // ActiveActionState without ProcessActionEngineEvent() ever running
        // for it. Runs before this tick's own goal/action logic below, so
        // it only ever reconciles state left over from a previous tick,
        // never something this tick is only just about to start.
        if (record->ActiveActionState && record->ActiveActionState->Type == ActionType::MoveTo && !HasOwnMoveToGenerator(*creature))
        {
            ActionCompletion completion;
            completion.Actor = record->Id;
            completion.Type = record->ActiveActionState->Type;
            completion.SourceGoal = record->ActiveActionState->SourceGoal;
            completion.GoalStartedAtMs = record->ActiveActionState->GoalStartedAtMs;
            completion.CompletedAtMs = nowMs;

            if (record->ActiveActionState->Destination
                && IsWithinArrivalTolerance(*record->ActiveActionState->Destination,
                    creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()))
            {
                completion.Status = ActionCompletionStatus::Succeeded;
                completion.Reason = ActionCompletionReason::Arrived;
            }
            else
            {
                completion.Status = ActionCompletionStatus::Failed;
                completion.Reason = ActionCompletionReason::EngineStopped;
            }

            HandleActionCompletion(*record, completion);
        }

        // Milestone 2.7A: level-triggered, not derived from the edge-
        // triggered NeedsThresholdEvent above - a candidate must exist for
        // as long as its Need stays at/above threshold, not just on the
        // tick it crosses.
        std::vector<GoalCandidate> candidates = _goalSystem.GenerateCandidates(record->Needs);
        for (GoalCandidate const& candidate : candidates)
        {
            TC_LOG_DEBUG("ai.world", "AI goal candidate agent={} type={} priority={} utility={:.4f} source={}",
                record->Id.Value, ToString(candidate.Type), ToString(candidate.Priority),
                candidate.Utility, ToString(candidate.Source));
        }

        // Milestone 2.7B1/2.7B2: select/retain/interrupt/complete the
        // agent's single ActiveGoal. Still no Action API, no /decision
        // change, no world mutation - transitions are logged, nothing
        // acts on them.
        std::optional<ActiveGoal> previousGoal = record->ActiveGoalState;
        GoalSelectionResult selection = _goalSystem.UpdateActiveGoal(record->ActiveGoalState, record->Needs, candidates, nowMs);
        record->ActiveGoalState = selection.Goal;

        switch (selection.Transition)
        {
            case GoalTransition::Activated:
                TC_LOG_DEBUG("ai.world", "AI goal transition agent={} transition={} goal={} priority={} utility={:.4f}",
                    record->Id.Value, ToString(selection.Transition), ToString(selection.Goal->Type),
                    ToString(selection.Goal->Priority), selection.Goal->Utility);
                break;
            case GoalTransition::Interrupted:
                TC_LOG_DEBUG("ai.world", "AI goal transition agent={} transition={} from={} to={} priority={} utility={:.4f}",
                    record->Id.Value, ToString(selection.Transition), ToString(previousGoal->Type), ToString(selection.Goal->Type),
                    ToString(selection.Goal->Priority), selection.Goal->Utility);

                // Milestone 2.8D P2 fix: MoveTo uses MOTION_PRIORITY_NORMAL,
                // lower than FLEE - TrinityCore deactivates rather than
                // removes a lower-priority generator when a higher-priority
                // one is added, so an interrupted-away GET_FOOD's MOVE_TO
                // would otherwise sit dormant under FLEE and resume once
                // FLEE ends, sending the actor to a destination from an
                // already-ended goal attempt. Cancel it before FLEE starts.
                // StopMoveTo() is already correctly scoped (own point id
                // only) and a no-op if MOVE_TO already finished naturally.
                if (selection.Goal->Type == GoalType::FleeDanger)
                {
                    _actionExecutor.StopMoveTo(*creature);
                    record->ActiveActionState.reset();

                    TC_LOG_DEBUG("ai.world", "AI action stop agent={} type={} reason=GOAL_INTERRUPTED",
                        record->Id.Value, ToString(ActionType::MoveTo));
                }
                break;
            case GoalTransition::Succeeded:
            case GoalTransition::Failed:
                TC_LOG_DEBUG("ai.world", "AI goal transition agent={} transition={} goal={} reason={} durationMs={}",
                    record->Id.Value, ToString(selection.Transition), ToString(selection.Completion->Type),
                    ToString(selection.Completion->Reason), selection.Completion->CompletedAtMs - selection.Completion->StartedAtMs);

                // Every goal type that can own a live TrinityCore movement
                // generator must end it on completion, the same ownership
                // rule as the Interrupted case above - an action must never
                // outlive the goal attempt that started it. Ends only the
                // specific generator each ExecuteX() started (FLEEING_MOTION_TYPE
                // for Flee, AIWorld's own point id for MoveTo), never
                // MotionMaster::Clear().
                if (selection.Completion->Type == GoalType::FleeDanger)
                {
                    _actionExecutor.StopFlee(*creature);

                    TC_LOG_DEBUG("ai.world", "AI action stop agent={} type={} reason={}",
                        record->Id.Value, ToString(ActionType::Flee),
                        selection.Transition == GoalTransition::Succeeded ? "GOAL_SUCCEEDED" : "GOAL_FAILED");
                }
                else if (selection.Completion->Type == GoalType::GetFood)
                {
                    _actionExecutor.StopMoveTo(*creature);
                    record->ActiveActionState.reset();

                    TC_LOG_DEBUG("ai.world", "AI action stop agent={} type={} reason={}",
                        record->Id.Value, ToString(ActionType::MoveTo),
                        selection.Transition == GoalTransition::Succeeded ? "GOAL_SUCCEEDED" : "GOAL_FAILED");
                }
                break;
            case GoalTransition::Released:
                TC_LOG_DEBUG("ai.world", "AI goal transition agent={} transition={} goal={}",
                    record->Id.Value, ToString(selection.Transition), ToString(previousGoal->Type));
                break;
            case GoalTransition::None:
                break;
        }

        // Milestone 2.11B: independent of the goal transition above (see
        // RoutineSystem.h for why this is not folded into GoalSystem's own
        // Needs-driven selection) - runs after ActiveGoalState is finalized
        // for this tick, so an Emergency goal Activated/Interrupted-into
        // this same tick already suppresses routine output rather than
        // racing it a tick late. No hysteresis: recomputed fresh every
        // tick, so the only thing worth logging is an edge (previous vs.
        // this tick's Type differs, or presence itself flips) - not every
        // tick it stays unchanged. Still no Action API, no world mutation -
        // a later milestone reconciles this into MOVE_TO.
        std::optional<RoutineGoal> previousRoutineGoal = record->RoutineGoalState;
        record->RoutineGoalState = _routineSystem.DeriveGoal(record->ActiveGoalState,
            record->HomeLocation, record->WorkLocation, nowMs, _routineScheduleConfig);

        bool routineChanged = previousRoutineGoal.has_value() != record->RoutineGoalState.has_value()
            || (previousRoutineGoal && record->RoutineGoalState && previousRoutineGoal->Type != record->RoutineGoalState->Type);

        if (routineChanged)
        {
            TC_LOG_DEBUG("ai.world", "AI routine transition agent={} from={} to={}",
                record->Id.Value,
                previousRoutineGoal ? ToString(previousRoutineGoal->Type) : "NONE",
                record->RoutineGoalState ? ToString(record->RoutineGoalState->Type) : "NONE");
        }

        // Milestone 2.11C: single-owner arbitration - Emergency ActiveGoal
        // > Normal ActiveGoal > RoutineGoal (RoutineSystem::DeriveGoal()
        // already handles Emergency by returning nullopt; this handles
        // Normal too, since GET_FOOD is allowed to coexist with a non-null
        // RoutineGoalState - see its own P3 note). Any ActiveGoalState at
        // all outranks a routine-owned MOVE_TO, so it must yield the
        // instant one exists - not just on this tick's own Activated/
        // Interrupted edge: a fresh FLEE_DANGER Activated from no prior
        // ActiveGoalState (the one case the FleeDanger-specific stop above
        // does not cover) can still be racing an in-flight routine MOVE_TO.
        // Scoped to IsRoutineSourceGoal() only - a GET_FOOD-owned MOVE_TO
        // being interrupted by FLEE_DANGER is already handled above and is
        // untouched here.
        if (record->ActiveGoalState && record->ActiveActionState && IsRoutineSourceGoal(record->ActiveActionState->SourceGoal))
        {
            _actionExecutor.StopMoveTo(*creature);

            TC_LOG_DEBUG("ai.world", "AI action stop agent={} type={} reason=ROUTINE_PREEMPTED_BY_GOAL sourceGoal={}",
                record->Id.Value, ToString(ActionType::MoveTo), ToString(record->ActiveActionState->SourceGoal));

            record->ActiveActionState.reset();
        }

        // Milestone 2.12F2: Regroup/Roam (2.12G2 - both the lowest-priority
        // tier, see GroupCoordinationGoal.h) yield to EITHER a Need-driven
        // ActiveGoalState or a Routine-owned MOVE_TO the instant either
        // exists - unlike Routine (which only yields to ActiveGoalState,
        // see the block above), Coordination sits below both. Scoped by
        // IsCoordinationSourceGoal(), the same tag-based discrimination
        // IsRoutineSourceGoal() already establishes for Routine - an
        // agent's own individual goals always win the action slot back
        // from an automatic group nudge. Regroup/Roam are themselves never
        // dispatched (AIWorldMgr::DispatchGroupMemberActionProposal())
        // while either already exists, so this only ever fires for the
        // same kind of "appeared fresh, racing an already in-flight lower-
        // priority MOVE_TO" case the ROUTINE_PREEMPTED_BY_GOAL block above
        // exists for.
        if ((record->ActiveGoalState || record->RoutineGoalState) && record->ActiveActionState
            && IsCoordinationSourceGoal(record->ActiveActionState->SourceGoal))
        {
            // Milestone 2.12G2R P2 fix, round 3 (STATIC review): observed
            // BEFORE StopMoveTo() is called - see StopInFlightGroupCoordination()'s
            // own comment for why "confirmed stopped after" alone is not
            // honest evidence of a genuine interruption unless it is also
            // known something was actually running beforehand. `creature`
            // is already guaranteed live/non-null this far into
            // UpdateNeeds()'s own per-agent loop (see its own `if
            // (!creature) continue;` above), so unlike
            // StopInFlightGroupCoordination() there is no "cannot resolve
            // a live Creature at all" case to handle here.
            bool generatorWasRunning = HasOwnMoveToGenerator(*creature);

            _actionExecutor.StopMoveTo(*creature);

            TC_LOG_DEBUG("ai.world", "AI action stop agent={} type={} reason=COORDINATION_PREEMPTED_BY_GOAL sourceGoal={}",
                record->Id.Value, ToString(ActionType::MoveTo), ToString(record->ActiveActionState->SourceGoal));

            // Milestone 2.12G2R P2 fix, round 2/3 (STATIC review): recorded
            // BEFORE the resets below erase the very state this event
            // needs to describe - see CoordinationStopEvent.h for why this
            // exists (a verifying test hook needs to know THIS specific
            // attempt was stopped BY a genuine preemption, not merely that
            // it is gone, which a natural ARRIVED would also produce).
            // PreemptingOwner/PreemptingGoal capture WHICH of
            // ActiveGoalState/RoutineGoalState actually did the preempting
            // (ActiveGoalState takes precedence when both happen to be
            // set, matching this same if-condition's own OR and this
            // file's own established ActiveGoalState > RoutineGoalState
            // arbitration) - captured synchronously, right here, so a
            // verifying test hook never has to re-derive "what preempted
            // this" from AgentRecord's own CURRENT state on some later
            // poll, which could already have moved on.
            CoordinationStopEvent stopEvent;
            stopEvent.Reason = CoordinationStopReason::PreemptedByGoal;
            stopEvent.SourceGoal = record->ActiveActionState->SourceGoal;
            stopEvent.SourceGroup = record->GroupCoordinationGoalState->SourceGroup;
            stopEvent.StartedAtMs = record->ActiveActionState->GoalStartedAtMs;
            // Milestone 2.12G3C2 P2 fix (STATIC review): captured HERE,
            // while GroupCoordinationGoalState is still set (its own reset
            // below would otherwise erase this before it could ever be
            // recorded) - see CoordinationStopEvent::TargetGuid's own
            // comment.
            stopEvent.TargetGuid = record->GroupCoordinationGoalState->TargetGuid;
            stopEvent.TargetEntry = record->GroupCoordinationGoalState->TargetEntry;
            stopEvent.EngineGeneratorWasRunningBeforeStop = generatorWasRunning;
            stopEvent.EngineGeneratorConfirmedStoppedAfterStop = !HasOwnMoveToGenerator(*creature);
            if (record->ActiveGoalState)
            {
                stopEvent.PreemptingOwner = CoordinationPreemptingOwner::ActiveGoal;
                stopEvent.PreemptingGoal = record->ActiveGoalState->Type;
            }
            else if (record->RoutineGoalState)
            {
                stopEvent.PreemptingOwner = CoordinationPreemptingOwner::RoutineGoal;
                stopEvent.PreemptingGoal = record->RoutineGoalState->Type;
            }
            stopEvent.StoppedAtMs = nowMs;
            record->LastCoordinationStop = stopEvent;

            record->ActiveActionState.reset();
            record->GroupCoordinationGoalState.reset();
        }

        // Milestone 2.11C: the routine's own MOVE_TO - only proposed while
        // no gameplay ActiveGoal exists at all (the preemption check above
        // already cleared a routine-owned action the moment one appeared,
        // so this never races GET_FOOD/FLEE_DANGER for the action slot).
        // GET_FOOD's own MOVE_TO/FLEE_DANGER's own Flee below never check
        // RoutineGoalState either way, so this has no effect on them.
        //
        // Milestone 2.12F4A: ControlMode gate (performance/early-rejection
        // - ActionSystem::Validate() is the actual mandatory safety
        // boundary regardless of this check, see its own comment).
        // RoutineGoalState itself is still computed/tracked above for
        // every agent unconditionally (pure state, allowed for
        // ObserveOnly) - only the ActionRequest build/dispatch below is
        // skipped.
        if (record->ControlMode == AgentControlMode::AIWorldControlled && !record->ActiveGoalState && record->RoutineGoalState)
        {
            bool alreadyExecuting = record->ActiveActionState
                && record->ActiveActionState->Type == ActionType::MoveTo
                && record->ActiveActionState->SourceGoal == record->RoutineGoalState->Type;

            if (!alreadyExecuting)
            {
                // The routine target changed (GO_TO_WORK <-> GO_HOME) while
                // the previous one was still in flight - stop it first, in
                // the same tick, the same "stop before issuing the
                // replacement" pattern FLEE-interrupting-GET_FOOD already
                // uses above, so ValidateMoveTo()'s HasActiveMovement check
                // below sees an idle actor.
                if (record->ActiveActionState && record->ActiveActionState->Type == ActionType::MoveTo
                    && IsRoutineSourceGoal(record->ActiveActionState->SourceGoal))
                {
                    _actionExecutor.StopMoveTo(*creature);

                    TC_LOG_DEBUG("ai.world", "AI action stop agent={} type={} reason=ROUTINE_TARGET_CHANGED sourceGoal={}",
                        record->Id.Value, ToString(ActionType::MoveTo), ToString(record->ActiveActionState->SourceGoal));

                    record->ActiveActionState.reset();
                }

                // 2.11C static review P2 fix: HandleActionCompletion()
                // clears ActiveActionState on arrival but deliberately
                // leaves RoutineGoalState alone (routine still correctly
                // says "you should be home/at work" - it is just satisfied
                // right now, not stale, the same way an ActiveGoal's own
                // Need can sit below its candidate threshold without the
                // goal itself needing to change). Without this check,
                // every following Needs tick alreadyExecuting would be
                // false again (ActiveActionState was just cleared) and
                // this block would re-issue an identical MOVE_TO forever -
                // ActionSystem::ValidateMoveTo() only checks map/finite/
                // range/busy, not "already there", so a zero-distance
                // request passes it every time. Same map + within
                // ArrivalToleranceYards of the target is "nothing to do",
                // not "target reached, forget it" - the instant routine
                // phase flips to the other location, this is no longer
                // true and MOVE_TO proposes normally again.
                ActionPosition routineDestination;
                routineDestination.MapId = record->RoutineGoalState->Target.MapId;
                routineDestination.X = record->RoutineGoalState->Target.X;
                routineDestination.Y = record->RoutineGoalState->Target.Y;
                routineDestination.Z = record->RoutineGoalState->Target.Z;

                bool alreadyAtTarget = creature->GetMapId() == routineDestination.MapId
                    && IsWithinArrivalTolerance(routineDestination,
                        creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());

                if (!alreadyAtTarget)
                {
                    ActionRequest moveRequest;
                    moveRequest.Actor = id;
                    moveRequest.Type = ActionType::MoveTo;
                    moveRequest.SourceGoal = record->RoutineGoalState->Type;

                    // No StartedAtMs of its own to reuse (RoutineGoal is
                    // stateless, recomputed fresh every tick - see
                    // RoutineGoal.h) - nowMs, the moment this specific
                    // attempt is actually issued, is this attempt's
                    // identity instead.
                    moveRequest.GoalStartedAtMs = nowMs;
                    moveRequest.Destination = routineDestination;

                    TC_LOG_DEBUG("ai.world", "AI action request agent={} type={} sourceGoal={} destination=({:.1f},{:.1f},{:.1f})",
                        record->Id.Value, ToString(moveRequest.Type), ToString(moveRequest.SourceGoal),
                        routineDestination.X, routineDestination.Y, routineDestination.Z);

                    ActionValidationContext moveContext;
                    moveContext.Materialized = record->WorldState == AgentWorldState::Materialized;
                    moveContext.Alive = context.Alive;
                    moveContext.ControlMode = record->ControlMode;

                    // No AgentRecord::ActiveGoalState to read here (that is
                    // exactly the branch condition above) - RoutineGoalState's
                    // own Type/this attempt's nowMs is what Validate() checks
                    // the request's honesty against instead. See
                    // ActionValidationContext.h.
                    moveContext.ActiveGoalType = record->RoutineGoalState->Type;
                    moveContext.ActiveGoalStartedAtMs = moveRequest.GoalStartedAtMs;
                    moveContext.MapId = creature->GetMapId();
                    moveContext.X = creature->GetPositionX();
                    moveContext.Y = creature->GetPositionY();
                    moveContext.Z = creature->GetPositionZ();
                    moveContext.HasActiveMovement = creature->GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE) != nullptr;

                    ActionValidationResult moveValidation = _actionSystem.Validate(moveRequest, moveContext);

                    TC_LOG_DEBUG("ai.world", "AI action validation agent={} type={} result={} reason={}",
                        record->Id.Value, ToString(moveRequest.Type), moveValidation.Allowed ? "ALLOWED" : "REJECTED",
                        ToString(moveValidation.Reason));

                    if (moveValidation.Allowed)
                    {
                        ActionResult moveResult = _actionExecutor.ExecuteMoveTo(moveRequest, *creature);

                        TC_LOG_DEBUG("ai.world", "AI action execution agent={} type={} status={} reason={}",
                            record->Id.Value, ToString(moveResult.Type), ToString(moveResult.Status), ToString(moveResult.Reason));

                        // Same rule 2.8F already established for GET_FOOD: only
                        // after the executor actually returned Started, never
                        // before validation and never on a Failed result.
                        if (moveResult.Status == ActionExecutionStatus::Started)
                        {
                            ActiveAction action;
                            action.Type = moveRequest.Type;
                            action.SourceGoal = moveRequest.SourceGoal;
                            action.GoalStartedAtMs = moveRequest.GoalStartedAtMs;
                            action.StartedAtMs = nowMs;
                            action.Destination = moveRequest.Destination;
                            record->ActiveActionState = action;
                        }
                    }
                }
            }
        }

        // Milestone 2.11D: RoutineGoal -> MOVE_TO -> ARRIVED -> RoutineActivity
        // (Work/Rest) - runs unconditionally every materialized/alive tick
        // (unlike the MOVE_TO block above, not gated behind
        // !record->ActiveGoalState), since it must also be the thing that
        // notices ActiveGoalState/ActiveActionState just appeared and drops
        // WORK/REST back to NONE this same tick, not a tick late. First
        // commit: state + transition log only - no ActionRequest, no emote,
        // no ResourcePressure/money change.
        RoutineActivityContext activityContext;
        activityContext.CurrentRoutineGoal = record->RoutineGoalState
            ? std::optional<GoalType>(record->RoutineGoalState->Type) : std::nullopt;
        activityContext.Materialized = record->WorldState == AgentWorldState::Materialized;
        activityContext.Alive = context.Alive;
        activityContext.HasActiveGoal = record->ActiveGoalState.has_value();
        activityContext.HasActiveAction = record->ActiveActionState.has_value();

        // 2.11D P3 fix: authoritative engine fact, independent of whatever
        // ActiveActionState happens to claim - the same expression
        // moveContext.HasActiveMovement above already uses.
        activityContext.ActorMoving = creature->GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE) != nullptr;

        if (record->RoutineGoalState)
        {
            ActionPosition routineTarget;
            routineTarget.MapId = record->RoutineGoalState->Target.MapId;
            routineTarget.X = record->RoutineGoalState->Target.X;
            routineTarget.Y = record->RoutineGoalState->Target.Y;
            routineTarget.Z = record->RoutineGoalState->Target.Z;

            activityContext.AtRoutineTarget = creature->GetMapId() == routineTarget.MapId
                && IsWithinArrivalTolerance(routineTarget, creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        }

        std::optional<RoutineActivityType> previousActivity = record->RoutineActivityState
            ? std::optional<RoutineActivityType>(record->RoutineActivityState->Type) : std::nullopt;
        std::optional<RoutineActivityType> derivedActivity = _routineActivitySystem.DeriveActivity(activityContext);

        if (derivedActivity != previousActivity)
        {
            if (derivedActivity)
            {
                TC_LOG_DEBUG("ai.world", "AI routine activity agent={} from={} to={}",
                    record->Id.Value, previousActivity ? ToString(*previousActivity) : "NONE", ToString(*derivedActivity));

                RoutineActivity activity;
                activity.Type = *derivedActivity;
                activity.StartedAtMs = nowMs;
                record->RoutineActivityState = activity;

                // Milestone 2.11E1: exactly one ActionRequest, right here
                // on the transition edge into Work/Rest - this whole
                // "if (derivedActivity)" body only ever runs once per
                // activity attempt (derivedActivity == previousActivity on
                // every following tick the same activity holds, so the
                // outer "if (derivedActivity != previousActivity)" simply
                // does not fire again), which is what gives "one request
                // per RoutineActivity::StartedAtMs" for free, with no
                // separate one-shot latch needed. Deliberately never sets
                // ActiveActionState afterwards, win or lose - unlike
                // MOVE_TO/GET_FOOD, Work/Rest must not claim the action
                // slot RoutineActivitySystem's own HasActiveAction check
                // reads, or the activity that just started it would be
                // suppressed again the very next tick. RoutineGoalState is
                // guaranteed set here - DeriveActivity() only ever returns
                // non-nullopt when activityContext.CurrentRoutineGoal (and
                // therefore record->RoutineGoalState) was already set above.
                // Milestone 2.12F4A P3 fix (STATIC review): ControlMode
                // early gate (performance/early-rejection - ActionSystem::
                // Validate() is the actual mandatory safety boundary
                // regardless, see its own comment) - an ObserveOnly agent's
                // RoutineActivityState above is still tracked (pure state,
                // set unconditionally just above this point), but no
                // WORK/REST ActionRequest is ever built/dispatched for one.
                // An earlier version of this milestone left this specific
                // path ungated (routine MOVE_TO dispatch already was) -
                // ActionSystem::Validate() rejected it correctly either way,
                // but building/validating/logging a request that can never
                // be ALLOWED for an ObserveOnly agent is exactly the wasted
                // work this early gate exists to avoid.
                if (record->ControlMode == AgentControlMode::AIWorldControlled)
                {
                    ActionRequest activityRequest;
                    activityRequest.Actor = id;
                    activityRequest.Type = *derivedActivity == RoutineActivityType::Work ? ActionType::Work : ActionType::Rest;
                    activityRequest.SourceGoal = record->RoutineGoalState->Type;
                    activityRequest.GoalStartedAtMs = activity.StartedAtMs;

                    ActionPosition activityDestination;
                    activityDestination.MapId = record->RoutineGoalState->Target.MapId;
                    activityDestination.X = record->RoutineGoalState->Target.X;
                    activityDestination.Y = record->RoutineGoalState->Target.Y;
                    activityDestination.Z = record->RoutineGoalState->Target.Z;
                    activityRequest.Destination = activityDestination;

                    TC_LOG_DEBUG("ai.world", "AI action request agent={} type={} sourceGoal={} destination=({:.1f},{:.1f},{:.1f})",
                        record->Id.Value, ToString(activityRequest.Type), ToString(activityRequest.SourceGoal),
                        activityDestination.X, activityDestination.Y, activityDestination.Z);

                    ActionValidationContext activityValidationContext;
                    activityValidationContext.Materialized = activityContext.Materialized;
                    activityValidationContext.Alive = activityContext.Alive;
                    activityValidationContext.ControlMode = record->ControlMode;
                    activityValidationContext.ActiveGoalType = record->RoutineGoalState->Type;
                    activityValidationContext.ActiveGoalStartedAtMs = activity.StartedAtMs;
                    activityValidationContext.MapId = creature->GetMapId();
                    activityValidationContext.X = creature->GetPositionX();
                    activityValidationContext.Y = creature->GetPositionY();
                    activityValidationContext.Z = creature->GetPositionZ();
                    activityValidationContext.HasActiveMovement = activityContext.ActorMoving;

                    // 2.11E1 P3 fix: read independently from AgentRecord::
                    // RoutineActivityState (just set above), not echoed from
                    // the request being validated - see
                    // ActionValidationContext::ExpectedRoutineActivity.
                    activityValidationContext.ExpectedRoutineActivity = record->RoutineActivityState->Type;
                    activityValidationContext.RoutineActivityStartedAtMs = record->RoutineActivityState->StartedAtMs;

                    ActionValidationResult activityValidation = _actionSystem.Validate(activityRequest, activityValidationContext);

                    TC_LOG_DEBUG("ai.world", "AI action validation agent={} type={} result={} reason={}",
                        record->Id.Value, ToString(activityRequest.Type), activityValidation.Allowed ? "ALLOWED" : "REJECTED",
                        ToString(activityValidation.Reason));

                    if (activityValidation.Allowed)
                    {
                        ActionResult activityResult = *derivedActivity == RoutineActivityType::Work
                            ? _actionExecutor.ExecuteWork(activityRequest, *creature)
                            : _actionExecutor.ExecuteRest(activityRequest, *creature);

                        TC_LOG_DEBUG("ai.world", "AI action execution agent={} type={} status={} reason={}",
                            record->Id.Value, ToString(activityResult.Type), ToString(activityResult.Status), ToString(activityResult.Reason));

                        // 2.11E1 P3 fix: same "no engine completion callback,
                        // Started treated as immediately done, routed through
                        // HandleActionCompletion() for consistent logging"
                        // shape TryEat() already uses for Eat - the 2.11E2
                        // economy mutation below gates on this Succeeded/
                        // Performed completion, never on Started alone. No
                        // ActiveActionState was ever set for this action, so
                        // HandleActionCompletion()'s own reset() is a harmless
                        // no-op here.
                        if (activityResult.Status == ActionExecutionStatus::Started)
                        {
                            ActionCompletion activityCompletion;
                            activityCompletion.Actor = record->Id;
                            activityCompletion.Type = activityRequest.Type;
                            activityCompletion.SourceGoal = activityRequest.SourceGoal;
                            activityCompletion.GoalStartedAtMs = activityRequest.GoalStartedAtMs;
                            activityCompletion.Status = ActionCompletionStatus::Succeeded;
                            activityCompletion.Reason = ActionCompletionReason::Performed;
                            activityCompletion.CompletedAtMs = nowMs;

                            HandleActionCompletion(*record, activityCompletion);

                            // Milestone 2.11E2: the only economy mutation that
                            // exists yet, and its only gate - Work's own
                            // Succeeded/Performed completion, applied here and
                            // nowhere else. REST reaches this same call with
                            // Type == ActionType::Rest and earns nothing; a
                            // rejected/failed Work request never reaches this
                            // block at all (activityResult.Status ==
                            // ActionExecutionStatus::Started already gates it
                            // above).
                            if (activityCompletion.Type == ActionType::Work
                                && activityCompletion.Status == ActionCompletionStatus::Succeeded
                                && activityCompletion.Reason == ActionCompletionReason::Performed)
                            {
                                // 2.11E2 P2 fix: RoutineActivityState (and this
                                // whole transition block) is runtime-only,
                                // cleared on every dematerialize/restart - so
                                // "this is a fresh WORK attempt" is not by
                                // itself proof "this work window hasn't been
                                // paid yet". workWindowId identifies the
                                // current synthetic work window independent of
                                // any runtime state: dayStartMs is this
                                // synthetic day's own start (nowMs with its
                                // within-day remainder subtracted off, the same
                                // epoch RoutineSystem::DeriveGoal() already
                                // uses), offset by WorkStartMs so it changes
                                // exactly once per day/work-phase, not every
                                // tick. A rematerialize or restart landing back
                                // inside the same window recomputes the same
                                // id and is correctly skipped.
                                uint64 dayStartMs = nowMs - (nowMs % _routineScheduleConfig.DayLengthMs);
                                uint64 workWindowId = dayStartMs + _routineScheduleConfig.WorkStartMs;

                                if (record->EconomyState.LastRewardedWorkWindowId != workWindowId)
                                {
                                    uint32 workMoneyReward = _workMoneyReward;

                                    // Money and the idempotency marker are
                                    // applied together and persisted in the
                                    // same UPDATE - MutateEconomyAndPersist()
                                    // only ever does one SaveEconomyState() call
                                    // per invocation, and that call bumps
                                    // Version unconditionally itself (see
                                    // AgentPersistence::SaveEconomyState()),
                                    // so this site does not need to.
                                    MutateEconomyAndPersist(*record, [workMoneyReward, workWindowId](AgentEconomyState& economy)
                                    {
                                        economy.Money += workMoneyReward;
                                        economy.LastRewardedWorkWindowId = workWindowId;
                                    });

                                    TC_LOG_DEBUG("ai.world", "AI economy agent={} money={} food={} resource={} workWindowId={} version={}",
                                        record->Id.Value, record->EconomyState.Money, record->EconomyState.Food,
                                        record->EconomyState.Resource, workWindowId, record->EconomyState.Version);
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                // 2.11D P3 fix: a routine-owned MOVE_TO (IsRoutineSourceGoal)
                // just starting is still routine's own doing, not a
                // takeover - the earlier version classified this as
                // GOAL_OWNERSHIP simply because ActiveActionState had
                // already been set by the MOVE_TO block above this same
                // tick, misreporting the ordinary "phase flipped, commute
                // starting" case. Only an actual ActiveGoalState, or some
                // other (non-routine) ActiveActionState, is a genuine
                // takeover; a routine-owned action, or no action at all
                // (mid-transition/rejected move), reads as TRAVEL.
                bool takenOverByGoal = activityContext.HasActiveGoal
                    || (record->ActiveActionState && !IsRoutineSourceGoal(record->ActiveActionState->SourceGoal));
                char const* reason = takenOverByGoal ? "GOAL_OWNERSHIP" : "TRAVEL";

                TC_LOG_DEBUG("ai.world", "AI routine activity agent={} from={} to=NONE reason={}",
                    record->Id.Value, ToString(*previousActivity), reason);

                record->RoutineActivityState.reset();
            }
        }

        // Milestone 2.8G P2 fix: consume any pending Eat continuation only
        // now, after this tick's own goal selection above has already run
        // - not from inside HandleActionCompletion() at MOVE_TO arrival
        // time, which happens before GenerateCandidates()/UpdateActiveGoal()
        // and would let Eat commit a moment before a same-tick FLEE_DANGER
        // emergency gets the chance to interrupt GET_FOOD. One-shot:
        // cleared here whether or not it actually still matches the
        // (possibly just-changed) ActiveGoalState.
        if (record->PendingEat)
        {
            PendingEatContinuation pending = *record->PendingEat;
            record->PendingEat.reset();

            if (record->ActiveGoalState && record->ActiveGoalState->Type == pending.SourceGoal
                && record->ActiveGoalState->StartedAtMs == pending.GoalStartedAtMs)
            {
                TryEat(*record, *creature, pending, nowMs);
            }
        }

        // Milestone 2.8A/2.8B: propose, validate, and (on ALLOWED) execute
        // a FLEE ActionRequest the tick FLEE_DANGER is Activated or
        // Interrupted-into - not every tick it stays active. GET_FOOD gets
        // its own MOVE_TO block below (2.8E) - it does not map to any
        // action here.
        if ((selection.Transition == GoalTransition::Activated || selection.Transition == GoalTransition::Interrupted)
            && selection.Goal->Type == GoalType::FleeDanger)
        {
            // Resolved once here and reused for both the request's claimed
            // FleeFromGuid and the validation context's actual
            // FleeSourceGuid - trivially the same value in 2.8B since both
            // come from this one live lookup in this one synchronous pass,
            // but this is exactly the engine-authoritative source
            // ActionSystem::Validate() checks the request's honesty
            // against.
            Unit* fleeSource = creature->GetThreatManager().GetCurrentVictim();

            ActionRequest request;
            request.Actor = id;
            request.Type = ActionType::Flee;
            request.SourceGoal = selection.Goal->Type;
            request.GoalStartedAtMs = selection.Goal->StartedAtMs;
            request.FleeFromGuid = fleeSource ? fleeSource->GetGUID() : ObjectGuid::Empty;

            TC_LOG_DEBUG("ai.world", "AI action request agent={} type={} sourceGoal={} fleeFrom={}",
                record->Id.Value, ToString(request.Type), ToString(request.SourceGoal), request.FleeFromGuid.ToString());

            ActionValidationContext validationContext;
            validationContext.Materialized = record->WorldState == AgentWorldState::Materialized;
            validationContext.Alive = context.Alive;
            validationContext.ControlMode = record->ControlMode;
            validationContext.ActiveGoalType = record->ActiveGoalState->Type;
            validationContext.ActiveGoalStartedAtMs = record->ActiveGoalState->StartedAtMs;
            validationContext.FleeSourceGuid = request.FleeFromGuid;

            ActionValidationResult validation = _actionSystem.Validate(request, validationContext);

            TC_LOG_DEBUG("ai.world", "AI action validation agent={} type={} result={} reason={}",
                record->Id.Value, ToString(request.Type), validation.Allowed ? "ALLOWED" : "REJECTED",
                ToString(validation.Reason));

            // No fleeSource means Validate() already rejected with
            // NoFleeSource above, so this dereference is safe - Allowed is
            // never true without a resolved threat victim.
            if (validation.Allowed)
            {
                ActionResult executionResult = _actionExecutor.ExecuteFlee(request, *creature, *fleeSource);

                TC_LOG_DEBUG("ai.world", "AI action execution agent={} type={} status={} reason={} targetGuid={}",
                    record->Id.Value, ToString(executionResult.Type), ToString(executionResult.Status),
                    ToString(executionResult.Reason), request.FleeFromGuid.ToString());
            }
        }

        // Milestone 2.8E: resolve a food target and propose/validate/
        // execute a MOVE_TO ActionRequest only on GET_FOOD's own
        // Activated transition - a brand new goal attempt, not every tick
        // it stays active, and not Interrupted (GET_FOOD is Normal
        // priority - nothing lower exists to interrupt into it in the
        // current catalog, so it can only ever reach Activated). Answers
        // only "where should a hungry agent go" - no EAT, no target
        // ownership/economy, no automatic goal failure if nothing is
        // found (just an audit log).
        if (selection.Transition == GoalTransition::Activated && selection.Goal->Type == GoalType::GetFood)
        {
            std::optional<GoalTarget> foodTarget = _foodTargetResolver.Resolve(_foodTargetConfig);

            TC_LOG_DEBUG("ai.world", "AI goal target agent={} goal={} result={}",
                record->Id.Value, ToString(GoalType::GetFood), foodTarget ? "FOUND" : "NOT_FOUND");

            if (foodTarget)
            {
                ActionRequest moveRequest;
                moveRequest.Actor = id;
                moveRequest.Type = ActionType::MoveTo;
                moveRequest.SourceGoal = selection.Goal->Type;
                moveRequest.GoalStartedAtMs = selection.Goal->StartedAtMs;

                ActionPosition destination;
                destination.MapId = foodTarget->MapId;
                destination.X = foodTarget->X;
                destination.Y = foodTarget->Y;
                destination.Z = foodTarget->Z;
                moveRequest.Destination = destination;

                TC_LOG_DEBUG("ai.world", "AI action request agent={} type={} sourceGoal={} destination=({:.1f},{:.1f},{:.1f})",
                    record->Id.Value, ToString(moveRequest.Type), ToString(moveRequest.SourceGoal),
                    destination.X, destination.Y, destination.Z);

                ActionValidationContext moveContext;
                moveContext.Materialized = record->WorldState == AgentWorldState::Materialized;
                moveContext.Alive = context.Alive;
                moveContext.ControlMode = record->ControlMode;
                moveContext.ActiveGoalType = record->ActiveGoalState->Type;
                moveContext.ActiveGoalStartedAtMs = record->ActiveGoalState->StartedAtMs;
                moveContext.MapId = creature->GetMapId();
                moveContext.X = creature->GetPositionX();
                moveContext.Y = creature->GetPositionY();
                moveContext.Z = creature->GetPositionZ();
                moveContext.HasActiveMovement = creature->GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE) != nullptr;

                ActionValidationResult moveValidation = _actionSystem.Validate(moveRequest, moveContext);

                TC_LOG_DEBUG("ai.world", "AI action validation agent={} type={} result={} reason={}",
                    record->Id.Value, ToString(moveRequest.Type), moveValidation.Allowed ? "ALLOWED" : "REJECTED",
                    ToString(moveValidation.Reason));

                if (moveValidation.Allowed)
                {
                    ActionResult moveResult = _actionExecutor.ExecuteMoveTo(moveRequest, *creature);

                    TC_LOG_DEBUG("ai.world", "AI action execution agent={} type={} status={} reason={}",
                        record->Id.Value, ToString(moveResult.Type), ToString(moveResult.Status), ToString(moveResult.Reason));

                    // Milestone 2.8F: only after the executor actually
                    // returned Started, never before validation and never
                    // on a Failed result - ActiveActionState must only ever
                    // claim a movement that is genuinely running.
                    if (moveResult.Status == ActionExecutionStatus::Started)
                    {
                        ActiveAction action;
                        action.Type = moveRequest.Type;
                        action.SourceGoal = moveRequest.SourceGoal;
                        action.GoalStartedAtMs = moveRequest.GoalStartedAtMs;
                        action.StartedAtMs = nowMs;
                        action.Destination = moveRequest.Destination;
                        record->ActiveActionState = action;
                    }
                }
            }
        }
    }
}

// World thread only, drained from _actionEngineEventBus once per tick,
// right after _eventBus's own drain (see Update()). Milestone 2.8F: the
// event was resolved from a live Creature at publish time (a map-updater
// thread, potentially several ticks ago) but reaches here as a pure value
// - the agent could have unloaded, rebound to a different Creature, or had
// the goal that started this action released/succeeded/failed/interrupted
// before this ever runs. Every field is checked against the agent's actual
// current AgentRecord state before being trusted as a real arrival; a
// failed check just discards the event (debug-logged, not an error) - a
// stale/mismatched engine callback is an expected consequence of crossing
// an unsynchronized boundary, not a bug.
void AIWorldMgr::ProcessActionEngineEvent(ActionEngineEvent const& event)
{
    if (event.MovementType != POINT_MOTION_TYPE || event.MovementId != ActionExecutor::MovePointId)
    {
        TC_LOG_DEBUG("ai.world", "AI action engine event map={} spawn={} discarded: not an AIWorld MOVE_TO (movementType={} movementId={})",
            event.MapId, event.SpawnId, event.MovementType, event.MovementId);
        return;
    }

    AgentRecord* record = FindLiveAgentBySpawn(_registry, event.MapId, event.SpawnId);
    if (!record)
    {
        TC_LOG_DEBUG("ai.world", "AI action engine event map={} spawn={} discarded: no registered agent", event.MapId, event.SpawnId);
        return;
    }

    if (record->RuntimeGuid != event.RuntimeGuid)
    {
        TC_LOG_DEBUG("ai.world", "AI action engine event agent={} discarded: stale Creature instance (recordGuid={} eventGuid={})",
            record->Id.Value, record->RuntimeGuid.ToString(), event.RuntimeGuid.ToString());
        return;
    }

    if (!record->ActiveActionState || record->ActiveActionState->Type != ActionType::MoveTo)
    {
        TC_LOG_DEBUG("ai.world", "AI action engine event agent={} discarded: no active MOVE_TO to complete", record->Id.Value);
        return;
    }

    // Milestone 2.11C: ActiveActionState::SourceGoal identifies which of
    // the (2.12F2: now three) independent owners this action belongs to -
    // AgentRecord::ActiveGoalState for every GoalType except GoToWork/
    // GoHome/Regroup/Roam, ::RoutineGoalState for GoToWork/GoHome (see
    // IsRoutineSourceGoal()), ::GroupCoordinationGoalState for Regroup/Roam
    // (see IsCoordinationSourceGoal()).
    // RoutineGoal has no StartedAtMs of its own to cross-check (it is stateless,
    // recomputed fresh every tick - see RoutineGoal.h): Type still matching
    // is the routine-side equivalent of the GoalType+StartedAtMs match
    // below, sufficient here because AIWorldMgr::UpdateNeeds() always stops
    // and clears a routine-owned ActiveActionState (target change,
    // preemption by a gameplay goal, dematerialization, death) before a
    // different attempt of the same Type could ever start - never lets one
    // silently get superseded out from under this check the way it could
    // not otherwise notice.
    // Milestone 2.12F2: a third branch, alongside the Routine/ActiveGoal
    // ones the comment above already documents - GroupCoordinationGoalState
    // is this attempt's own owner for a Regroup/Roam-sourced action,
    // checked the same Type+StartedAtMs way ActiveGoalState already is
    // (see GroupCoordinationGoal.h for why this is ephemeral like
    // ActiveGoal, not stateless like RoutineGoal). Type == SourceGoal
    // already distinguishes Regroup from Roam correctly here - no separate
    // per-intent branch needed, since GroupCoordinationGoal::Type is set
    // to the SAME sourceGoal DispatchGroupMemberActionProposal() chose.
    bool ownedByCurrentAttempt;
    if (IsRoutineSourceGoal(record->ActiveActionState->SourceGoal))
        ownedByCurrentAttempt = record->RoutineGoalState && record->RoutineGoalState->Type == record->ActiveActionState->SourceGoal;
    else if (IsCoordinationSourceGoal(record->ActiveActionState->SourceGoal))
    {
        ownedByCurrentAttempt = record->GroupCoordinationGoalState
            && record->GroupCoordinationGoalState->Type == record->ActiveActionState->SourceGoal
            && record->GroupCoordinationGoalState->StartedAtMs == record->ActiveActionState->GoalStartedAtMs;

        // Milestone 2.12G3C2 P2 fix (STATIC review): HUNT additionally
        // carries target identity (ActiveAction::Target,
        // GroupCoordinationGoal::TargetGuid/TargetEntry) - Type+StartedAtMs
        // matching alone is not proof this completion belongs to the same
        // TARGET-directed attempt, only the same goal-attempt slot. A
        // no-op for Regroup/Roam, which carry no Target at all.
        if (ownedByCurrentAttempt && record->ActiveActionState->SourceGoal == GoalType::Hunt)
        {
            ownedByCurrentAttempt = record->ActiveActionState->Target
                && record->ActiveActionState->Target->Guid == record->GroupCoordinationGoalState->TargetGuid
                && record->ActiveActionState->Target->Entry == record->GroupCoordinationGoalState->TargetEntry;
        }
    }
    else
        ownedByCurrentAttempt = record->ActiveGoalState && record->ActiveGoalState->Type == record->ActiveActionState->SourceGoal
            && record->ActiveGoalState->StartedAtMs == record->ActiveActionState->GoalStartedAtMs;

    if (!ownedByCurrentAttempt)
    {
        TC_LOG_DEBUG("ai.world", "AI action engine event agent={} discarded: goal attempt has already ended", record->Id.Value);
        return;
    }

    ActionCompletion completion;
    completion.Actor = record->Id;
    completion.Type = record->ActiveActionState->Type;
    completion.SourceGoal = record->ActiveActionState->SourceGoal;
    completion.GoalStartedAtMs = record->ActiveActionState->GoalStartedAtMs;
    completion.CompletedAtMs = CurrentTimeMs();

    // Milestone 2.8F P2 fix: MovementInform() firing is not itself proof of
    // arrival - MoveSplineInit::MoveTo() accepts a PATHFIND_INCOMPLETE
    // path, which still finalizes (and still fires this callback) short of
    // the requested destination. Compare the actor's actual position at
    // callback time against what was actually requested before ever
    // reporting Succeeded.
    if (record->ActiveActionState->Destination
        && IsWithinArrivalTolerance(*record->ActiveActionState->Destination, event.X, event.Y, event.Z))
    {
        completion.Status = ActionCompletionStatus::Succeeded;
        completion.Reason = ActionCompletionReason::Arrived;
    }
    else
    {
        completion.Status = ActionCompletionStatus::Failed;
        completion.Reason = ActionCompletionReason::DestinationNotReached;
    }

    HandleActionCompletion(*record, completion);
}

// World thread only. Milestone 2.8F/2.8G/2.8G P2 fix: closes out
// ActiveActionState and logs - for a MOVE_TO that Succeeded/Arrived on a
// GET_FOOD attempt, also records the arrival as record.PendingEat rather
// than acting on it here. Eat is deliberately not proposed/validated/
// executed in this call: UpdateNeeds()'s own goal-selection pass
// (GenerateCandidates()/UpdateActiveGoal()) must get to run first, so a
// same-tick FLEE_DANGER emergency can still interrupt GET_FOOD before Eat
// ever commits to it - see PendingEatContinuation.h and TryEat(). Arriving
// at a target is not the same as the goal that wanted it being satisfied:
// this itself never touches NeedsState, never marks a goal Succeeded,
// never calls GoalSystem - only Eat's own success (via TryEat() ->
// NeedsSystem::SatisfyHunger()) may do that.
void AIWorldMgr::HandleActionCompletion(AgentRecord& record, ActionCompletion const& completion)
{
    uint64 startedAtMs = record.ActiveActionState ? record.ActiveActionState->StartedAtMs : completion.CompletedAtMs;
    std::optional<ActionPosition> completedDestination = record.ActiveActionState ? record.ActiveActionState->Destination : std::nullopt;

    record.ActiveActionState.reset();

    TC_LOG_DEBUG("ai.world", "AI action completion agent={} type={} status={} reason={} sourceGoal={} durationMs={}",
        record.Id.Value, ToString(completion.Type), ToString(completion.Status), ToString(completion.Reason),
        ToString(completion.SourceGoal), completion.CompletedAtMs - startedAtMs);

    if (completion.Type == ActionType::MoveTo && completion.Status == ActionCompletionStatus::Succeeded
        && completion.Reason == ActionCompletionReason::Arrived && completion.SourceGoal == GoalType::GetFood
        && completedDestination)
    {
        PendingEatContinuation pending;
        pending.Destination = *completedDestination;
        pending.SourceGoal = completion.SourceGoal;
        pending.GoalStartedAtMs = completion.GoalStartedAtMs;
        record.PendingEat = pending;
    }

    // Milestone 2.12F2: unlike RoutineGoalState (deliberately left alone
    // on completion - see GroupCoordinationGoal.h for why Coordination is
    // ephemeral instead), GroupCoordinationGoalState has nothing else that
    // ever re-derives or re-consumes it - it must be cleared here,
    // regardless of Status/Reason (Arrived, DestinationNotReached,
    // GoalInterrupted, ActorDematerialized, ActorDead, EngineStopped all
    // reach this same call), or a completed attempt's own identity would
    // linger and could be misread as still owning a future unrelated
    // ActiveActionState of the same Type. 2.12G2: IsCoordinationSourceGoal()
    // (Regroup or Roam), not GoalType::Regroup alone - a completed Roam
    // attempt must be released here exactly the same way, or a stale
    // GroupCoordinationGoalState would linger for it.
    //
    // Milestone 2.12G3D1 P2 fix (runtime finding): HUNT is the one
    // deliberate exception, and only for a genuine SUCCESSFUL Arrived
    // completion - reaching a HUNT target is not "done", it is this
    // member still owning the group's ongoing attempt with nothing
    // further to do YET (no combat phase exists in this milestone).
    // Releasing GroupCoordinationGoalState here the same way Regroup/Roam
    // do let the very next RunCoalitionCoordination() pass see no
    // in-flight attempt at all, freshly re-select the SAME still-eligible
    // target (nothing about arriving makes HuntIntentSystem stop
    // selecting it - there is no Roam-style arrival radius for HUNT), and
    // dispatch ANOTHER, effectively zero-distance MOVE_TO - repeating
    // every single coordination pass. Retaining the SAME (group, target,
    // StartedAtMs) identity instead means HasInFlightHuntAttempt()/
    // ResolveHuntIntentForGroup() keep recognizing this as the one
    // already-owned attempt (the pinned identity is reused, never
    // re-selected - see ResolveHuntIntentForGroup()'s own comment), and
    // DispatchHuntProposal()'s own existing GroupCoordinationGoalState
    // re-check (COORDINATION_GOAL) now correctly refuses to dispatch a
    // second MOVE_TO for an already-arrived member. Every OTHER HUNT
    // completion reason (Failed/DestinationNotReached, GoalInterrupted,
    // ActorDematerialized, ActorDead, EngineStopped) still releases
    // ownership exactly like Regroup/Roam - retaining it there would
    // leave a member that never actually reached its target permanently
    // stuck, with no path back to being re-dispatched. Target-invalid
    // reconciliation (ReconcileActiveHuntTargetsForGroup()), lifecycle
    // stops (StopInFlightGroupCoordination(), which resets
    // GroupCoordinationGoalState unconditionally regardless of whether
    // ActiveActionState still exists), and individual goal preemption (a
    // fresh Emergency/Normal ActiveGoalState is never blocked by a
    // retained GroupCoordinationGoalState - dispatch everywhere else in
    // this codebase only ever checks ActiveActionState, already reset
    // above, for busy-ness) all still apply unchanged to a member sitting
    // in this retained-ownership state.
    bool retainHuntOwnershipOnArrival = completion.SourceGoal == GoalType::Hunt
        && completion.Status == ActionCompletionStatus::Succeeded
        && completion.Reason == ActionCompletionReason::Arrived;

    if (IsCoordinationSourceGoal(completion.SourceGoal) && !retainHuntOwnershipOnArrival)
        record.GroupCoordinationGoalState.reset();

    // Milestone 2.12G3D2A: the retained state's own phase must be set
    // EXPLICITLY to AtTarget here, never left as whatever it already was
    // (Approaching, set when this same attempt was originally dispatched)
    // - see HuntPhase's own comment in GroupCoordinationGoal.h for why
    // this must never be inferred from ActiveActionState's own absence.
    if (retainHuntOwnershipOnArrival && record.GroupCoordinationGoalState)
    {
        record.GroupCoordinationGoalState->Phase = HuntPhase::AtTarget;

        // Diagnostic-only (STATIC review pattern established for
        // AIWorld.TestObserveActiveHuntAgentId): purely read-only, no
        // *Fired dependency - unlike CheckTestObserveActiveHunt()'s own
        // one-shot PASS, this must keep firing on every arrival for the
        // watched agent regardless of whether that earlier hook already
        // latched (arrival always happens well after the first Approaching
        // capture).
        if (_testObserveActiveHuntAgentId && record.Id == _testObserveActiveHuntAgentId)
            TC_LOG_INFO("ai.world", "AI HUNT approach runtime diagnostic: agent={} stage=ARRIVED result=AT_TARGET group={} target={} targetEntry={} startedAtMs={}",
                record.Id.Value, record.GroupCoordinationGoalState->SourceGroup.Value,
                record.GroupCoordinationGoalState->TargetGuid.ToString(), record.GroupCoordinationGoalState->TargetEntry,
                record.GroupCoordinationGoalState->StartedAtMs);
    }
}

// World thread only. Milestone 2.11E2 P3 fix: applies mutate, then persists
// - see this method's own header comment for why the Version bump is
// deliberately not done here (SaveEconomyState() does it unconditionally
// itself now, so this stays a plain two-step convenience rather than the
// thing anything actually depends on for correctness).
void AIWorldMgr::MutateEconomyAndPersist(AgentRecord& record, std::function<void(AgentEconomyState&)> const& mutate)
{
    mutate(record.EconomyState);

    _persistence.SaveEconomyState(record.Id, record.EconomyState);
}

// World thread only, called only from UpdateNeeds(), after this same
// tick's GenerateCandidates()/UpdateActiveGoal() pass has already run -
// never from HandleActionCompletion() itself. Milestone 2.8G P2 fix: the
// caller has already re-confirmed record.ActiveGoalState still matches
// pending's SourceGoal/GoalStartedAtMs (the same goal attempt that was
// still active when the MOVE_TO arrived) before calling this - if
// FLEE_DANGER pre-empted GET_FOOD in the meantime, this is never called at
// all. ActionSystem::Validate() re-checks the same identity independently
// (ActionValidationContext::ArrivedDestination/ArrivedSourceGoal/
// ArrivedGoalStartedAtMs) as the real safety boundary; this caller-side
// check only avoids building a request that Validate() would reject anyway.
void AIWorldMgr::TryEat(AgentRecord& record, Creature& creature, PendingEatContinuation const& pending, uint64 nowMs)
{
    ActionRequest eatRequest;
    eatRequest.Actor = record.Id;
    eatRequest.Type = ActionType::Eat;
    eatRequest.SourceGoal = pending.SourceGoal;
    eatRequest.GoalStartedAtMs = pending.GoalStartedAtMs;
    eatRequest.Destination = pending.Destination;

    TC_LOG_DEBUG("ai.world", "AI action request agent={} type={} sourceGoal={}",
        record.Id.Value, ToString(eatRequest.Type), ToString(eatRequest.SourceGoal));

    ActionValidationContext eatContext;
    eatContext.Materialized = record.WorldState == AgentWorldState::Materialized;
    eatContext.Alive = creature.IsAlive();
    eatContext.ControlMode = record.ControlMode;
    eatContext.InCombat = creature.IsInCombat();
    eatContext.ActiveGoalType = record.ActiveGoalState->Type;
    eatContext.ActiveGoalStartedAtMs = record.ActiveGoalState->StartedAtMs;
    eatContext.MapId = creature.GetMapId();
    eatContext.X = creature.GetPositionX();
    eatContext.Y = creature.GetPositionY();
    eatContext.Z = creature.GetPositionZ();
    eatContext.ArrivedDestination = pending.Destination;
    eatContext.ArrivedSourceGoal = pending.SourceGoal;
    eatContext.ArrivedGoalStartedAtMs = pending.GoalStartedAtMs;

    ActionValidationResult eatValidation = _actionSystem.Validate(eatRequest, eatContext);

    TC_LOG_DEBUG("ai.world", "AI action validation agent={} type={} result={} reason={}",
        record.Id.Value, ToString(eatRequest.Type), eatValidation.Allowed ? "ALLOWED" : "REJECTED",
        ToString(eatValidation.Reason));

    if (!eatValidation.Allowed)
        return;

    ActionResult eatResult = _actionExecutor.ExecuteEat(eatRequest, creature);

    TC_LOG_DEBUG("ai.world", "AI action execution agent={} type={} status={} reason={}",
        record.Id.Value, ToString(eatResult.Type), ToString(eatResult.Status), ToString(eatResult.Reason));

    if (eatResult.Status != ActionExecutionStatus::Started)
        return;

    // Eat has no engine completion callback to wait for - ExecuteEat()'s
    // emote is fire-and-forget, so a Started result is treated as
    // immediately Consumed, in this same call. Routed back through
    // HandleActionCompletion() for the same logging format rather than
    // duplicating the "AI action completion" log line - it is a safe
    // single-level call there, not a re-entrant one: completion.Type is
    // Eat, not MoveTo, so HandleActionCompletion()'s own PendingEat check
    // above never matches, and Eat never had an ActiveActionState to reset.
    ActionCompletion eatCompletion;
    eatCompletion.Actor = record.Id;
    eatCompletion.Type = ActionType::Eat;
    eatCompletion.SourceGoal = eatRequest.SourceGoal;
    eatCompletion.GoalStartedAtMs = eatRequest.GoalStartedAtMs;
    eatCompletion.Status = ActionCompletionStatus::Succeeded;
    eatCompletion.Reason = ActionCompletionReason::Consumed;
    eatCompletion.CompletedAtMs = nowMs;

    HandleActionCompletion(record, eatCompletion);

    // The only place Hunger is allowed to decrease, and only now - after
    // Eat itself reached Succeeded/Consumed, never as a side effect of
    // MOVE_TO's own arrival. GoalSystem is not called here: the next Needs
    // tick's own UpdateActiveGoal() will see Hunger < 0.60 and produce
    // GET_FOOD's Succeeded/NeedSatisfied transition through the existing,
    // unmodified retention-threshold logic.
    _needsSystem.SatisfyHunger(record.Needs);
}

// World thread only. Milestone 2.4A/2.4B/2.4C: log, then hand the
// Observation to ShortTermMemory. Still no decision context, no agent
// reaction - Remember() only turns a raw perception stream into a
// deduplicated, TTL'd memory record; nothing here acts on it yet.
void AIWorldMgr::ProcessObservation(Observation const& observation)
{
    char const* sourceEventType = observation.SourceEventType ? ToString(*observation.SourceEventType) : "NONE";

    TC_LOG_DEBUG("ai.world",
        "AI observation observer={} type={} sourceEvent={} sourceEventType={} channel={} distance={:.1f} los={} actorGuid={} actorAgent={} targetGuid={} targetEntry={} targetAgent={}",
        observation.Observer.Value, ToString(observation.Type), observation.SourceEventId, sourceEventType,
        ToString(observation.Channel), observation.Distance, observation.LineOfSight,
        observation.Actor.Guid.ToString(), observation.Actor.Agent.Value,
        observation.Target.Guid.ToString(), observation.Target.Entry, observation.Target.Agent.Value);

    float importance = MemoryImportance::Score(observation);

    // Return value unused - ShortTermMemory already logs its own
    // Added/Refreshed transition, the same way AgentRegistry and EventBus
    // log their own state changes rather than making the caller do it.
    _shortTermMemory.Remember(observation, _shortTermMemoryTtlMs, importance);

    // Everything goes into short-term memory; only importance >= threshold
    // gets promoted to long-term (and, only on an actual promotion - not a
    // refresh of an existing long-term memory - persisted). PlayerSeen/
    // CreatureSeen and most WorldEvent types never cross this bar; that's
    // the point.
    if (importance >= _longTermMemoryMinImportance)
    {
        if (std::optional<LongTermMemoryRecord> record = _longTermMemory.Remember(observation, importance))
        {
            TC_LOG_DEBUG("ai.world", "AI long-term memory promoted agent={} type={} importance={:.2f} sourceEvent={} sourceEventType={}",
                observation.Observer.Value, ToString(record->Type), record->Importance, record->SourceEventId, sourceEventType);

            _memoryPersistence.PersistLongTermMemory(*record);
        }
    }
}

void AIWorldMgr::Shutdown()
{
    // Unconditional and first, even if !_enabled: narrows (does not close -
    // see the _acceptEvents comment in AIWorldMgr.h) the window a map
    // worker could publish into a manager that's mid-teardown. Cheap and
    // idempotent either way.
    _acceptEvents.store(false, std::memory_order_release);

    if (!_enabled)
        return;

    TC_LOG_INFO("ai.world", "AIWorld shutting down");
    _enabled = false;
    TC_LOG_INFO("ai.world", "AIWorld stopped");
}
