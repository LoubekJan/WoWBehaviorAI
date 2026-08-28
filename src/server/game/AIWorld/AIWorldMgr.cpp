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
#include "Player.h"
#include "PointMovementGenerator.h"
#include <algorithm>
#include <chrono>
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

    // Milestone 2.12E4C2: deliberately larger than AIWorld.WolfGroupFormationRadius
    // - see CoalitionMaintenanceProfile.h's own LeaveRadius comment for the
    // hysteresis this gap exists to give. Clamped to never fall below
    // FormationRadius (a LeaveRadius smaller than FormationRadius would
    // defeat that hysteresis entirely - a member could form a coalition
    // and immediately be eligible to leave it again the very next
    // maintenance pass).
    float wolfGroupLeaveRadius = sConfigMgr->GetFloatDefault("AIWorld.WolfGroupLeaveRadius", 60.0f);
    if (wolfGroupLeaveRadius < _wolfGroupFormationRadius)
    {
        TC_LOG_WARN("ai.world", "AIWorld.WolfGroupLeaveRadius ({:.1f}) is lower than AIWorld.WolfGroupFormationRadius ({:.1f}), clamping to match",
            wolfGroupLeaveRadius, _wolfGroupFormationRadius);
        wolfGroupLeaveRadius = _wolfGroupFormationRadius;
    }
    _wolfGroupLeaveRadius = wolfGroupLeaveRadius;

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

    _maintenanceSchedule.clear();
    _maintenanceInFlight.clear();

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

    TC_LOG_INFO("ai.world", "AI wolf coalition maintenance configured enabled={} leaveRadius={:.1f} interval={}ms maxPerPass={}",
        _coalitionMaintenanceEnabled, _wolfGroupLeaveRadius, _coalitionMaintenanceIntervalMs, _coalitionMaintenanceMaxPerPass);

    // Milestone 2.12E4R P3 fix (STATIC review): the hard ceiling on how
    // many DIFFERENT profiles' formations can be in flight at once - see
    // _coalitionFormationMaxInFlight's own declaration comment. 1 (the
    // default) reproduces 2.12E4B's own "at most one automatic formation
    // in flight, period" behavior exactly, since only one profile exists
    // today.
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

    // Milestone 2.12E3B: off by default - see RunGroupPolicySmokeTest()'s
    // own comment. Read here (not inline at the call site below) purely to
    // sit next to the config values its pure half exercises.
    bool testGroupPolicy = sConfigMgr->GetBoolDefault("AIWorld.TestGroupPolicy", false);

    // Milestone 2.12E4C1: off by default - see RunCoalitionMaintenanceSmokeTest()'s
    // own comment. Read here for the same reason testGroupPolicy is: sits
    // next to the one other "pure smoke test" toggle Initialize() already
    // has.
    bool testCoalitionMaintenance = sConfigMgr->GetBoolDefault("AIWorld.TestCoalitionMaintenance", false);

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
        if (!_registry.FindBySpawn(testMapId, testSpawnId))
        {
            AgentId newId = _persistence.CreateCreatureAgent(AgentType::Guard, testMapId, testSpawnId);
            if (!newId)
            {
                // AgentPersistence already logged why. Nothing goes into
                // _registry without a real, DB-confirmed AgentId - the
                // whole point of this milestone is that the two never
                // disagree.
                TC_LOG_ERROR("ai.world", "AI persistent agent creation failed for map={} spawn={}, test agent disabled this run",
                    testMapId, testSpawnId);
            }
            else
            {
                AgentRecord record;
                record.Id = newId;
                record.Type = AgentType::Guard;
                record.MapId = testMapId;
                record.SpawnId = testSpawnId;
                record.WorldState = AgentWorldState::Abstract;

                if (_registry.Add(record))
                {
                    TC_LOG_INFO("ai.world", "AI persistent agent created id={} type={} map={} spawn={}",
                        newId.Value, ToString(AgentType::Guard), testMapId, testSpawnId);
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
    _groupLifecycleSystem.RequestDissolveGroup(groupId, _groupRegistry, _groupPersistence, _groupLifecyclePending,
        [this, groupId, onComplete = std::move(onComplete)](bool success)
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
        [onComplete = std::move(onComplete)](bool success)
        {
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
        [onComplete = std::move(onComplete)](bool success)
        {
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

void AIWorldMgr::RunCoalitionFormation(CoalitionFormationProfile const& profile)
{
    // 2.12E4R P3 fix (STATIC review): fail-closed, not fail-open - see
    // CoalitionFormationProfileId.h for why Invalid exists and why a
    // profile silently defaulting to WolfLoose was the wrong failure mode.
    if (profile.Id == CoalitionFormationProfileId::Invalid)
    {
        TC_LOG_ERROR("ai.world", "AIWorldMgr::RunCoalitionFormation: refusing to run for an Invalid profile id");
        return;
    }

    if (_formationInFlight.count(profile.Id))
        return;

    // 2.12E4R P3 fix (STATIC review): the global ceiling across ALL
    // profiles - see _coalitionFormationMaxInFlight's own declaration
    // comment. Checked after the per-profile check above so a profile
    // already in flight is rejected for that reason, not misreported as
    // budget-exhausted.
    if (uint32(_formationInFlight.size()) >= _coalitionFormationMaxInFlight)
    {
        TC_LOG_DEBUG("ai.world", "AI coalition formation: global in-flight budget ({}) reached, skipping profile={} this pass",
            _coalitionFormationMaxInFlight, ToString(profile.Id));
        return;
    }

    std::vector<CoalitionCandidate> candidates = CollectCoalitionCandidates();

    std::unordered_set<uint64> excludedMembers = CollectMemberIdsOfKind(profile.Kind);
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
        return;

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

void AIWorldMgr::RunCoalitionMaintenance(CoalitionMaintenanceProfile const& profile)
{
    uint64 nowMs = CurrentTimeMs();

    // Milestone 2.12E4C2 P2 fix (STATIC review): pre-filter to
    // profile.ProfileId - NOT group->Kind alone. Kind cannot tell two
    // profiles of the same Kind apart (only WolfLoose forms LOOSE groups
    // today, but nothing about Kind itself prevents a future second
    // LOOSE-forming profile from colliding the same way), and a manually/
    // admin-created LOOSE group (AgentGroupRecord::ProfileId == Invalid)
    // must never be silently swept into WolfLoose's own automatic
    // maintenance just because its Kind happens to match. A wrong-profile
    // group would also fail CoalitionMaintenanceSystem::Evaluate()'s own
    // ProfileId-mismatch guard, but only after consuming one of this
    // pass's bounded admission slots to find that out - filtering here
    // avoids that waste too.
    std::vector<GroupId> candidates;
    for (GroupId groupId : _groupRegistry.GetGroups())
    {
        AgentGroupRecord const* group = _groupRegistry.Find(groupId);
        if (group && group->ProfileId == profile.ProfileId)
            candidates.push_back(groupId);
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

    GroupCoarseSimulationScheduler::SelectionResult selection = _maintenanceScheduler.SelectDue(
        _maintenanceSchedule, candidates, nowMs, _coalitionMaintenanceMaxPerPass);

    for (GroupId groupId : selection.Admitted)
    {
        SimulationScheduleState& scheduleState = _maintenanceSchedule[groupId.Value];
        scheduleState.LastTickAtMs = nowMs;
        scheduleState.NextTickAtMs = nowMs + _coalitionMaintenanceIntervalMs;

        RunCoalitionMaintenanceForGroup(groupId, profile);
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

    // Milestone 2.12E4A/2.12E4B: entirely gated on AIWorld.WolfGroupAutoFormation
    // (off by default) - the timer itself does not even advance while
    // disabled, the same "no cost at all unless the feature is on"
    // treatment AIWorld.TestGroupPolicy's own one-shot smoke test gets at
    // Initialize(), just on a recurring cadence here instead.
    if (_wolfGroupAutoFormation)
    {
        _wolfGroupFormationTimer += diff;
        if (_wolfGroupFormationTimer >= _wolfGroupFormationIntervalMs)
        {
            _wolfGroupFormationTimer = 0;
            RunCoalitionFormation(_wolfLooseFormationProfile);
        }
    }

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
            RunCoalitionMaintenance(_wolfLooseMaintenanceProfile);
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
    return record != nullptr;
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

        // Milestone 2.11C: the routine's own MOVE_TO - only proposed while
        // no gameplay ActiveGoal exists at all (the preemption check above
        // already cleared a routine-owned action the moment one appeared,
        // so this never races GET_FOOD/FLEE_DANGER for the action slot).
        // GET_FOOD's own MOVE_TO/FLEE_DANGER's own Flee below never check
        // RoutineGoalState either way, so this has no effect on them.
        if (!record->ActiveGoalState && record->RoutineGoalState)
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
    // the two independent owners this action belongs to - AgentRecord::
    // ActiveGoalState for every GoalType except GoToWork/GoHome, ::
    // RoutineGoalState for those (see IsRoutineSourceGoal()). RoutineGoal
    // has no StartedAtMs of its own to cross-check (it is stateless,
    // recomputed fresh every tick - see RoutineGoal.h): Type still matching
    // is the routine-side equivalent of the GoalType+StartedAtMs match
    // below, sufficient here because AIWorldMgr::UpdateNeeds() always stops
    // and clears a routine-owned ActiveActionState (target change,
    // preemption by a gameplay goal, dematerialization, death) before a
    // different attempt of the same Type could ever start - never lets one
    // silently get superseded out from under this check the way it could
    // not otherwise notice.
    bool ownedByCurrentAttempt = IsRoutineSourceGoal(record->ActiveActionState->SourceGoal)
        ? (record->RoutineGoalState && record->RoutineGoalState->Type == record->ActiveActionState->SourceGoal)
        : (record->ActiveGoalState && record->ActiveGoalState->Type == record->ActiveActionState->SourceGoal
            && record->ActiveGoalState->StartedAtMs == record->ActiveActionState->GoalStartedAtMs);

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
