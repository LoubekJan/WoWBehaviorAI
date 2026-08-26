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

#ifndef AIWORLD_AIWORLDMGR_H
#define AIWORLD_AIWORLDMGR_H

#include "Action/ActionCompletion.h"
#include "Action/ActionEngineEventBus.h"
#include "Action/ActionExecutor.h"
#include "Action/ActionSystem.h"
#include "Action/PendingEatContinuation.h"
#include "Agent/AgentGroupLifecycleSystem.h"
#include "Agent/AgentGroupOperationSource.h"
#include "Agent/AgentGroupPolicyConfig.h"
#include "Agent/AgentGroupPolicySystem.h"
#include "Agent/AgentGroupRegistry.h"
#include "Agent/AgentGroupSimulationSystem.h"
#include "Agent/AgentId.h"
#include "Agent/AgentRegistry.h"
#include "Agent/GroupId.h"
#include "Define.h"
#include "Event/EventBus.h"
#include "Event/WorldEvent.h"
#include "Goal/FoodTargetResolver.h"
#include "Goal/GoalSystem.h"
#include "Goal/RoutineActivitySystem.h"
#include "Goal/RoutineSystem.h"
#include "Inference/AIClient.h"
#include "Memory/LongTermMemory.h"
#include "Memory/MemoryRetrieval.h"
#include "Memory/ShortTermMemory.h"
#include "Needs/NeedsSystem.h"
#include "Perception/PerceptionSystem.h"
#include "Persistence/AgentGroupPersistence.h"
#include "Persistence/AgentPersistence.h"
#include "Persistence/MemoryPersistence.h"
#include "Persistence/TransactionCallbackProcessor.h"
#include "Scheduler/CoarseSimulationScheduler.h"
#include "Scheduler/DecisionScheduler.h"
#include "Scheduler/GroupCoarseSimulationScheduler.h"
#include "Scheduler/SimulationScheduleState.h"
#include "Scheduler/SimulationTier.h"
#include "Scheduler/StableAgentHash.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Trinity::Asio { class IoContext; }
class Creature;

// Entry point for the AIWorld subsystem. Driven from the world update thread
// only (called after sMapMgr->Update() in World::Update()) - never spawns its
// own thread and never mutates Creature/Player/Map state. Fully inert unless
// AIWorld.Enable = 1.
class TC_GAME_API AIWorldMgr
{
    public:
        static AIWorldMgr* instance();

        void Initialize(Trinity::Asio::IoContext& ioContext);
        void Update(uint32 diff);
        void Shutdown();

        bool IsEnabled() const { return _enabled; }

        // Safe to call from ANY thread that can observe a game-world fact
        // (a map/combat worker, not just the world thread). Does nothing
        // but a relaxed-ish atomic check and a mutex-guarded enqueue - never
        // looks up a Creature/AgentRecord, never calls ai-server, never
        // mutates world state. See EventBus for the actual thread-safety
        // story.
        void PublishWorldEvent(WorldEvent event);

        // Milestone 2.8A.5: consulted from FactorySelector::SelectAI()
        // (CreatureAISelector.cpp), ahead of pet/scripted/AIName/
        // Permissible-based AI selection, to decide whether a Creature
        // gets AIWorldCreatureAI instead of its normal default AI. Safe to
        // call from a map-updater thread during grid loading even though
        // AgentRegistry's mutating calls are world-thread-only:
        // MapManager::Update() (MapManager.cpp) blocks on m_updater.wait()
        // before returning, and AIWorldMgr::Update() (which is the only
        // thing that ever mutates _registry once Initialize() has run) is
        // called strictly after sMapMgr->Update() returns in World::Update()
        // - so no map-updater thread executing FactorySelector::SelectAI()
        // is ever concurrent with anything that mutates _registry.
        // Multiple map-updater threads calling this concurrently with each
        // other, for different maps, is fine too: this and everything it
        // calls (AgentRegistry::FindBySpawn() const) are read-only. Never
        // touches Creature/Map, never calls ai-server.
        bool OwnsSpawn(uint32 mapId, uint64 spawnId) const;

        // Milestone 2.8F: safe to call from ANY thread TrinityCore itself
        // calls AIWorldCreatureAI::MovementInform() from (a map-updater
        // thread during Map::Update(), not necessarily the world thread).
        // Same enqueue-only contract as PublishWorldEvent(): never touches
        // Creature/AgentRegistry, never calls ai-server, never mutates
        // world state. See ActionEngineEventBus for the thread-safety
        // story.
        void PublishActionEngineEvent(ActionEngineEvent event);

    private:
        AIWorldMgr() = default;
        ~AIWorldMgr() = default;
        AIWorldMgr(AIWorldMgr const&) = delete;
        AIWorldMgr& operator=(AIWorldMgr const&) = delete;

        std::optional<AIRequest> ProcessAgent(AgentId id);
        AIRequest CaptureAgentContext(AgentId id, AgentRecord& record, Creature& creature);
        void ProcessWorldEvent(WorldEvent& event);
        void ProcessObservation(Observation const& observation);
        void ScanNearbyEntities();
        void UpdateNeeds(uint32 elapsedMs);
        void ProcessActionEngineEvent(ActionEngineEvent const& event);
        void HandleActionCompletion(AgentRecord& record, ActionCompletion const& completion);
        void TryEat(AgentRecord& record, Creature& creature, PendingEatContinuation const& pending, uint64 nowMs);

        // Milestone 2.11E2 P3 fix: a convenience for the common "apply one
        // mutation, then persist" shape every economy change so far has
        // (see UpdateNeeds()'s WORK reward block) - not itself the source
        // of the Version-bump guarantee. That guarantee now lives in
        // AgentPersistence::SaveEconomyState() itself (it increments
        // Version unconditionally, first thing, regardless of caller) -
        // see its own comment for why a convention at this layer alone was
        // not enough: anyone could still call SaveEconomyState() directly
        // and bypass this method entirely.
        void MutateEconomyAndPersist(AgentRecord& record, std::function<void(AgentEconomyState&)> const& mutate);
        void ValidateDecisionIntent(AgentId id, AgentRecord const& record, AIResponse const& response);
        std::vector<DecisionSubmitResult> SubmitDecisionContexts(std::vector<AIRequest> requests);
        void RunDecisionScheduler();
        bool UpdateSimulationTier(AgentId id, SimulationTier tier);

        // Milestone 2.12E2/2.12E3B P2 fix (STATIC review): the raw,
        // policy-UNGATED dissolve path - thin wrapper around
        // _groupLifecycleSystem.RequestDissolveGroup(), except it also
        // erases _groupSimulationSchedule's entry for groupId once the
        // dissolve is confirmed (2.12E2 hardening: an earlier version left
        // that entry behind on every dissolve, growing
        // _groupSimulationSchedule without bound across repeated
        // create/dissolve cycles - RequestDissolveGroup() itself has no
        // access to that map, it is AIWorldMgr-only scheduling bookkeeping,
        // so this is the one place that can close the gap). onComplete
        // fires with the same success/failure AgentGroupLifecycleSystem::
        // RequestDissolveGroup() itself would have reported.
        //
        // NOT the canonical dissolve entry point any more (STATIC review:
        // an earlier version of this comment claimed it was meant for
        // "any automatic/policy-driven caller too" - wrong, since this
        // method has no AgentGroupOperationSource and asks
        // AgentGroupPolicySystem nothing, an automatic caller going
        // through this directly could dissolve a Stable group outright,
        // exactly what StableGroupProtected exists to prevent for
        // CanLeave()). Reserved for callers that are themselves already
        // the policy decision - today's manual smoke test/cleanup paths,
        // which are unconditionally allowed the same way a Manual request
        // always is. Any automatic/policy-driven caller (2.12E4+) MUST use
        // RequestDissolveGroupWithPolicy() below instead.
        void RequestDissolveGroup(GroupId groupId, std::function<void(bool)> onComplete);

        // Milestone 2.12E3B P2 fix (STATIC review): the canonical,
        // policy-gated dissolve entry point - the Dissolve counterpart to
        // RequestJoinGroupWithPolicy()/RequestLeaveGroupWithPolicy() below,
        // closing the gap those two already covered but
        // RequestDissolveGroup() above never did. source ==
        // AgentGroupOperationSource::Manual always proceeds straight to
        // RequestDissolveGroup(), the same "a deliberately-authorized
        // individual request is never policy-blocked" rule CanLeave()
        // already gives Manual. source == AutomaticPolicy asks
        // _groupPolicySystem.ShouldDissolve() first - which already
        // encodes both halves of the rule this milestone's own roadmap
        // message asked for (AutomaticPolicy + Stable -> always false,
        // i.e. rejected; AutomaticPolicy + Loose -> only when actually
        // below AIWorld.LooseGroupMinMembers) without this method needing
        // to duplicate that Kind-branch itself - only calls through to
        // RequestDissolveGroup() if that returns true. An unknown groupId
        // or a rejected decision calls onComplete(false) synchronously and
        // never reaches AgentGroupLifecycleSystem/the DB at all, the same
        // contract RequestJoinGroupWithPolicy()/
        // RequestLeaveGroupWithPolicy() already hold.
        void RequestDissolveGroupWithPolicy(GroupId groupId, AgentGroupOperationSource source, std::function<void(bool)> onComplete);

        // Milestone 2.12E2: manual proof that AgentGroupLifecycleSystem's
        // async Request* API never touches Creature/WorldState and never
        // blocks the world thread - runs once from Initialize(), only when
        // all three AIWorld.TestGroupMemberAgentId1/2/3 resolve to
        // already-registered agents. Never creates an agent itself (see
        // this method's own .cpp comment for why an earlier version doing
        // so was wrong). Kicks off RequestCreateGroup(), whose completion
        // chains into RunGroupLifecycleSmokeTestJoinStep() to join the
        // three members one at a time (each join only issued from the
        // previous one's own completion - see AgentGroupLifecycleSystem.h
        // for why a synchronous loop can no longer do this), which in turn
        // chains into a leave and a dissolve. Every step's outcome is
        // checked; a failure anywhere logs FAILED and attempts a
        // best-effort RequestDissolveGroup() cleanup via
        // RunGroupLifecycleSmokeTestAbort() rather than leaving a partially
        // joined test group behind.
        void RunGroupLifecycleSmokeTest(AgentId memberId1, AgentId memberId2, AgentId memberId3);

        // Milestone 2.12E2: joins memberIds[index], then recurses into
        // index+1 - once index reaches memberIds.size(), leaves the last
        // member and dissolves the group instead. See
        // RunGroupLifecycleSmokeTest()'s own comment for why this is a
        // chain of completions rather than a loop.
        void RunGroupLifecycleSmokeTestJoinStep(GroupId groupId, std::array<AgentId, 3> memberIds, std::size_t index);

        // Milestone 2.12E2: logs the smoke test as FAILED (naming which
        // step and member failed), then fires a fire-and-forget
        // RequestDissolveGroup() best-effort cleanup for groupId so a
        // mid-sequence failure (e.g. the second Join succeeds but the
        // third fails) does not leave a partially joined test group
        // sitting in the DB/registry forever. The cleanup's own outcome is
        // only logged, never chained into anything further - this is
        // best-effort, not a guarantee.
        void RunGroupLifecycleSmokeTestAbort(GroupId groupId, char const* step, AgentId memberId);

        // Milestone 2.12E3B: the canonical way ANY part of AIWorldMgr asks
        // to join a group - resolves groupId in _groupRegistry, asks
        // _groupPolicySystem.CanJoin() (source is always relevant here,
        // even though CanJoin() itself never reads it - kept in the
        // signature for symmetry with RequestLeaveGroupWithPolicy() and so
        // every call site names who is asking, matching
        // AgentGroupOperationSource.h's own comment on why that matters
        // for logging even where a rule does not yet branch on it), and
        // only calls through to _groupLifecycleSystem.RequestJoinGroup()
        // if the answer is Allowed. An unknown groupId or a rejected
        // decision calls onComplete(false, decision) synchronously and
        // never reaches AgentGroupLifecycleSystem/the DB at all - see
        // AgentGroupPolicySystem.h's own class comment for why this gate
        // sits strictly upstream of lifecycle, never inside it.
        //
        // Milestone 2.12E3B P3 fix (STATIC review): onComplete carries the
        // AgentGroupPolicyDecision alongside success, not just a bare bool
        // - decision != Allowed means policy itself rejected the request
        // (success is always false in that case, and nothing downstream of
        // policy was ever touched); decision == Allowed means the request
        // reached AgentGroupLifecycleSystem, and success reflects whatever
        // that layer/the DB ultimately reported. Without this, a caller
        // (a test, or future policy/AI code reacting to a rejection)
        // cannot tell "policy said no" apart from "policy said yes but the
        // DB write failed" from the bool alone - see
        // RunGroupPolicySmokeTest()'s own integration half for exactly the
        // assertion this makes possible that a bare bool could not
        // (STABLE_GROUP_PROTECTED specifically, not just "some rejection
        // happened, for some reason, somewhere"). InvalidOperation is used
        // for the one case that never reaches AgentGroupPolicySystem at
        // all - an unknown groupId, see AgentGroupPolicyDecision.h's own
        // comment on why that value was reserved rather than invented here.
        void RequestJoinGroupWithPolicy(GroupId groupId, AgentId memberId, uint64 joinedAtMs, AgentGroupOperationSource source,
            std::function<void(bool success, AgentGroupPolicyDecision decision)> onComplete);

        // Milestone 2.12E3B: same shape as RequestJoinGroupWithPolicy(),
        // asking _groupPolicySystem.CanLeave() instead - this is the one
        // that actually matters for Stable protection, since CanLeave()
        // rejects (StableGroupProtected) an AutomaticPolicy leave for a
        // Stable group but allows a Manual one for either kind. An unknown
        // groupId or a rejected decision calls onComplete(false, decision)
        // synchronously and never reaches AgentGroupLifecycleSystem/the DB
        // - see RequestJoinGroupWithPolicy()'s own comment (2.12E3B P3 fix)
        // for why decision is reported alongside success.
        void RequestLeaveGroupWithPolicy(GroupId groupId, AgentId memberId, AgentGroupOperationSource source,
            std::function<void(bool success, AgentGroupPolicyDecision decision)> onComplete);

        // Milestone 2.12E3B: manual proof of AgentGroupPolicySystem's own
        // rules, in two parts. Part one is pure - synthetic
        // AgentGroupRecord values built on the stack, fed straight to
        // _groupPolicySystem.CanJoin()/CanLeave()/ShouldDissolve(), no
        // registry/DB touched at all, exercising every rule
        // AgentGroupPolicySystem.cpp implements (capacity, Manual-vs-
        // AutomaticPolicy leave, Loose-below-minimum dissolution, Stable
        // protection). Always runs when this method is called at all (see
        // AIWorld.TestGroupPolicy). Part two only runs if testMemberId is
        // set and resolves in _registry - creates a real Stable test
        // group, joins testMemberId, then proves the policy gate actually
        // protects RequestLeaveGroupWithPolicy(): an AutomaticPolicy leave
        // must be rejected with membership left untouched, a Manual leave
        // for the same member must then succeed, and the test group is
        // dissolved (manually, via RequestDissolveGroup()) either way
        // before finishing, so it never lingers as a permanent DB fixture.
        void RunGroupPolicySmokeTest(AgentId testMemberId);

        bool _enabled = false;

        // Milestone 2.10A/2.10B: how often RunDecisionScheduler() itself
        // runs - not every tick (GetAgents() allocates, so this avoids
        // paying that cost every single world tick the way
        // _needsUpdateTimer/_nearbyPerceptionTimer already don't either),
        // and deliberately faster than either per-agent decision interval
        // below (AIWorld.DecisionSchedulerIntervalMs, default 250ms) - see
        // RunDecisionScheduler()'s own comment for why scheduler-poll
        // cadence and per-agent decision cadence are two different things.
        uint32 _decisionSchedulerIntervalMs = 250;
        uint32 _decisionSchedulerTimer = 0;

        // Milestone 2.10B: per-agent decision interval, chosen fresh each
        // pass by DecisionCadenceClass (see RunDecisionScheduler()) rather
        // than one interval for every agent - Nearby (a real Player within
        // _decisionNearbyPlayerRange right now) gets faster decisions than
        // Active (everyone else materialized).
        uint32 _decisionNearbyIntervalMs = 1000;
        uint32 _decisionActiveIntervalMs = 5000;
        float _decisionNearbyPlayerRange = 60.0f;

        // Registry of persistent agents - survives its Creature being
        // unloaded/reloaded; only ProcessAgent()'s Bind/UnbindCreature calls
        // change an agent's WorldState. Purely in-memory; rebuilt from
        // _persistence every startup.
        AgentRegistry _registry;

        // characters-DB authority for AgentId (Milestone 2.2A): _registry
        // never mints an id itself, only ever receives one already assigned
        // by this. Used exclusively during Initialize(), never per-tick.
        AgentPersistence _persistence;

        // Milestone 2.12D (STATIC review P2 fix): registry of persistent
        // AgentGroups - deliberately its own registry/GroupId identity
        // space, not part of _registry/AgentId any more (see GroupId.h for
        // why the old shared-identity model was wrong). Purely in-memory;
        // rebuilt from _groupPersistence every startup, after _registry
        // itself (LoadGroupMembers() needs both already populated).
        AgentGroupRegistry _groupRegistry;

        // characters-DB authority for GroupId - the group-side counterpart
        // to _persistence, see AgentGroupPersistence's own class comment
        // for why this is a separate class rather than folded into
        // AgentPersistence.
        AgentGroupPersistence _groupPersistence;

        // Milestone 2.12E1: the single owner of AgentGroup create/join/
        // leave/dissolve - see its own class comment. Stateless (every
        // dependency is a parameter), so this is just a stable place to
        // call through rather than constructing one per call; no different
        // in spirit from _agentGroupSimulationSystem below.
        AgentGroupLifecycleSystem _groupLifecycleSystem;

        // Milestone 2.12E2: every AgentGroupPersistence async write's
        // TransactionCallback lands here (via _groupLifecycleSystem's own
        // Request* methods) and is polled once per world tick in Update()
        // - the same "enqueue, poll every Update()" shape
        // World::_queryProcessor already uses for QueryCallback. A
        // completion only ever runs from inside that poll, so it is always
        // world-thread-only.
        TransactionCallbackProcessor _groupLifecyclePending;

        // Milestone 2.12E3A/2.12E3B: "is this socially allowed?" - see its
        // own class comment for why this is a separate question from
        // everything _groupLifecycleSystem already answers. Pure/stateless
        // like _groupLifecycleSystem, called only from
        // RequestJoinGroupWithPolicy()/RequestLeaveGroupWithPolicy() below
        // (and the smoke test) - _groupLifecycleSystem's own Request*
        // methods are never called directly by anything else in this file
        // any more once 2.12E3B's own automatic-policy callers exist,
        // though 2.12E2's own lifecycle-only smoke test still does, since
        // it is deliberately testing the seam one layer below this one.
        AgentGroupPolicySystem _groupPolicySystem;

        // Milestone 2.12E3B: loaded and clamped once at Initialize() from
        // AIWorld.Loose/StableGroupMin/MaxMembers - see
        // AgentGroupPolicyConfig.h for why _groupPolicySystem itself never
        // reads config directly. Copied into a fresh AgentGroupPolicyContext
        // for every CanJoin()/CanLeave()/ShouldDissolve() call rather than
        // referenced, the same reasoning AgentGroupPolicyContext.h's own
        // comment gives.
        AgentGroupPolicyConfig _groupPolicyConfig;

        // Milestone 2.10A: deterministic admission ranking over every
        // registered+Materialized agent - see DecisionScheduler.h. Pure
        // value transform, like _needsSystem/_goalSystem/_actionSystem;
        // RunDecisionScheduler() is the one place that resolves live
        // Creatures/AgentRecords and drives it.
        DecisionScheduler _decisionScheduler;

        // Per-agent scheduling bookkeeping (LastDecisionSubmittedAtMs/
        // AwaitingResponse) for _decisionScheduler - deliberately not part
        // of AgentRecord/AgentRegistry, see DecisionScheduleState.h. Keyed
        // by AgentId::Value.
        std::unordered_map<uint64, DecisionScheduleState> _decisionSchedule;

        // Milestone 2.10C: last SimulationTier observed for each registered
        // agent - deliberately not part of AgentRecord/AgentRegistry either
        // (a tier transition must never disturb AgentId/Needs/memories/
        // goal state, only ever log against it), purely so
        // UpdateSimulationTier() can tell a real transition apart from an
        // unchanged tier. Keyed by AgentId::Value; an agent with no entry
        // yet is logged as an initial assignment, not a "from" transition.
        std::unordered_map<uint64, SimulationTier> _agentSimulationTier;

        // Milestone 2.10D: coarse Background tick cadence
        // (AIWorld.BackgroundSimulationIntervalMs, default 60s) - not a
        // decision cadence, deliberately much coarser. See
        // RunDecisionScheduler()'s own comment for why this stays a pure
        // observability scaffold in 2.10D - no live Creature/Map lookup,
        // no /decision, no ActionExecutor, no gameplay state change.
        uint32 _backgroundSimulationIntervalMs = 60000;

        // Milestone 2.12D P2 fix (STATIC review): the group coarse-tick
        // cadence (AIWorld.GroupSimulationIntervalMs, default 5min) -
        // renamed from _abstractSimulationIntervalMs now that groups no
        // longer go through SimulationTier::Abstract at all (that tier is
        // gone - see SimulationTier.h). Governs how often
        // RunDecisionScheduler()'s own group coarse-tick loop below
        // simulates each AgentGroup.
        uint32 _groupSimulationIntervalMs = 300000;

        // Milestone 2.10D P2 fix: deterministic bounded admission for the
        // Background coarse tick, the exact same reason _decisionScheduler/
        // _decisionMaxInFlight exist for the decision-eligible tiers - see
        // CoarseSimulationScheduler.h for why an unbounded coarse tick
        // would both spike world-thread work and permanently phase-lock
        // every Background agent onto the same tick pass. AgentId-only -
        // the group coarse tick below has its own GroupId-keyed sibling
        // instead, see _groupCoarseSimulationScheduler.
        CoarseSimulationScheduler _coarseSimulationScheduler;
        uint32 _coarseSimulationMaxPerPass = 50;

        // Per-agent bookkeeping for the Background coarse tick above -
        // deliberately not part of AgentRecord/AgentRegistry, see
        // SimulationScheduleState.h. Keyed by AgentId::Value; never
        // touched for a Materialized (Active/Nearby) agent.
        std::unordered_map<uint64, SimulationScheduleState> _simulationSchedule;

        // Milestone 2.12D P2 fix (STATIC review): the group coarse tick's
        // own bounded admission, GroupId's sibling to
        // _coarseSimulationScheduler/_coarseSimulationMaxPerPass above - an
        // earlier version of this same milestone ticked every due
        // AgentGroup unconditionally on the assumption that group
        // cardinality would stay small; rejected by review once dynamic
        // LOOSE coalitions (which do not have a fixed, small population
        // the way scripted/STABLE groups do) entered the picture. See
        // GroupCoarseSimulationScheduler.h for the full reasoning.
        GroupCoarseSimulationScheduler _groupCoarseSimulationScheduler;
        uint32 _groupSimulationMaxPerPass = 50;

        // Per-group bookkeeping for the group coarse tick above -
        // deliberately not part of AgentGroupRecord/AgentGroupRegistry, see
        // SimulationScheduleState.h (its own comment on why it is reused
        // unmodified here). Keyed by GroupId::Value.
        std::unordered_map<uint64, SimulationScheduleState> _groupSimulationSchedule;

        // Milestone 2.12B/2.12D: the only simulation the group coarse tick
        // runs - called from RunDecisionScheduler()'s own group coarse-tick
        // loop, unconditionally for every due AgentGroup (2.12D P2 fix:
        // no longer gated on whether any member happens to be materialized
        // right now - see AgentGroupRecord.h). No config of its own beyond
        // _agentGroupSimulationRates.
        AgentGroupSimulationSystem _agentGroupSimulationSystem;
        AgentGroupSimulationRates _agentGroupSimulationRates;

        // AIWorld.DecisionMaxInFlight - the hard global cap RunDecisionScheduler()
        // admits against and AIClient itself separately enforces (defense in
        // depth: even a scheduler bug can't get more than this many
        // concurrent /decision requests out of AIClient).
        uint32 _decisionMaxInFlight = 4;

        // Owned for the process lifetime, deliberately not reset in
        // Shutdown(): by the time Shutdown() runs, the io_context it was
        // built on may already be stopped and its worker threads joined
        // (see Main.cpp), so there is no safe moment left to tear it down
        // early. Setting _enabled = false just stops new submissions.
        std::unique_ptr<AIClient> _aiClient;

        uint32 _healthIntervalMs = 10000;
        uint32 _healthTimer = 0;

        // Cross-thread ingress for WorldEvents (see EventBus) - map/combat
        // workers publish into it concurrently with the world thread
        // draining it once per tick. _acceptEvents is the only thing
        // PublishWorldEvent() reads before touching _eventBus; it has to be
        // atomic because, unlike every other member here, it's read from
        // threads other than the world thread. Set true at the end of
        // Initialize(), false at the very first line of Shutdown(), to
        // narrow the window a worker could publish into a half-constructed
        // or tearing-down manager - not a hard barrier: a worker can still
        // read true, get preempted before Publish() takes _eventBus's
        // mutex, and only reach it after Shutdown() has already flipped
        // the flag. Harmless today (EventBus is a plain process-lifetime
        // member and Publish() never touches anything else on AIWorldMgr),
        // but don't treat this as a real teardown barrier once one is
        // needed - that'll want an EventBus::Close() serialized on its own
        // mutex instead.
        EventBus _eventBus;
        std::atomic<bool> _acceptEvents{ false };

        // World-thread-only: turns a witnessed WorldEvent (or, since
        // Milestone 2.4B/2.4C, a nearby Player/Creature - see
        // ScanNearbyEntities()) into an Observation per Materialized agent
        // (range + LOS). Never called from a map/combat worker - only
        // after this manager has already resolved the observer's live
        // Creature itself.
        PerceptionSystem _perception;
        uint32 _perceptionSightRange = 40;

        // Milestone 2.4B/2.4C: periodic PlayerSeen/CreatureSeen
        // perception, independent of any WorldEvent. Deliberately its own
        // (faster, ~1s) cadence rather than piggybacking on the decision
        // scheduler's own cadence - like ProcessWorldEvent()'s perception
        // loop, ScanNearbyEntities() treats live Creature existence as the
        // authority for whether an agent can perceive anything, not
        // record->WorldState.
        uint32 _nearbyPerceptionIntervalMs = 1000;
        uint32 _nearbyPerceptionTimer = 0;

        // Milestone 2.5A/2.5B1: deduplicated, TTL'd summary of every
        // Observation ProcessObservation() sees, weighted by
        // MemoryImportance::Score(). Not persisted, not read by any
        // decision context yet.
        ShortTermMemory _shortTermMemory;
        uint32 _shortTermMemoryTtlMs = 30000;

        // Expiry doesn't need to run every tick - its own ~1s cadence,
        // independent of every other timer here.
        uint32 _memoryMaintenanceTimer = 0;

        // Milestone 2.5B: every Observation goes into _shortTermMemory
        // unconditionally, but only importance >= _longTermMemoryMinImportance
        // gets promoted into _longTermMemory and (on an actual promotion,
        // not a refresh of an existing one) queued for async persistence
        // via _memoryPersistence. Rebuilt from characters.
        // ai_long_term_memories every startup, the same way _registry is
        // rebuilt from _persistence.
        LongTermMemory _longTermMemory;
        MemoryPersistence _memoryPersistence;
        float _longTermMemoryMinImportance = 0.75f;

        // Milestone 2.5C/2.9A: deterministic relevance scoring over
        // _shortTermMemory/_longTermMemory, run once per CaptureAgentContext()
        // right after the AgentSnapshot is built - its Top-N result feeds
        // AgentContext::RelevantMemories.
        MemoryRetrieval _memoryRetrieval;
        uint32 _memoryRetrievalTopN = 5;

        // Milestone 2.6A: deterministic per-agent NeedsState drift, run only
        // for Materialized agents (see UpdateNeeds()) on its own ~1s cadence,
        // independent of the decision scheduler's cadence - Needs is a
        // simulation subsystem, not part of the snapshot/decision cadence.
        // Pure value transform: NeedsSystem itself never touches AgentId/
        // Creature/Map.
        NeedsSystem _needsSystem;
        NeedsUpdateRates _needsRates;
        uint32 _needsUpdateIntervalMs = 1000;
        uint32 _needsUpdateTimer = 0;

        // Milestone 2.7A: deterministic, level-triggered goal candidate
        // generation from NeedsState, run right after UpdateNeeds() so it
        // always sees this tick's freshly-updated Needs. Logged only - no
        // ActiveGoal, no selection, no Action API (that's 2.7B/2.8). Pure
        // value transform: GoalSystem itself never touches AgentId/
        // Creature/Map/AgentRecord/DB.
        GoalSystem _goalSystem;

        // Milestone 2.8A/2.8B: the safety boundary between "AI proposes"
        // and "TrinityCore executes" - AIWorldMgr builds an ActionRequest
        // only on FLEE_DANGER's ACTIVATED/INTERRUPTED transition (see
        // UpdateNeeds()) and asks Validate() whether it's currently
        // ALLOWED. Debug-logged either way.
        ActionSystem _actionSystem;

        // Milestone 2.8B: the only member allowed to actually touch
        // TrinityCore's engine API on AIWorld's behalf, and only after
        // _actionSystem has already returned Allowed == true for the exact
        // request being executed.
        ActionExecutor _actionExecutor;

        // Milestone 2.8E: answers "where should a hungry agent go" for
        // GET_FOOD's ACTIVATED transition (see UpdateNeeds()) - superseding
        // 2.8D's generic, goal-agnostic MOVE_TO test trigger, now that a
        // real Goal -> target -> Action pipeline exists. _foodTargetConfig
        // is shared configuration (AIWorld.TestFoodTarget*, loaded once at
        // Initialize()), passed into Resolve() per call - the same
        // pattern _needsRates already uses for NeedsSystem. Pure value
        // transform: FoodTargetResolver itself never touches AgentId/
        // Creature/Map/DB.
        FoodTargetResolver _foodTargetResolver;
        FoodTargetConfig _foodTargetConfig;

        // Milestone 2.11B: run alongside GoalSystem in UpdateNeeds() (same
        // Needs-cadence tick, right after ActiveGoalState is finalized for
        // it), independent of GoalSystem's own Needs-driven selection - see
        // RoutineSystem.h for why routine is deliberately not folded into
        // GoalCandidate/ActiveGoal. _routineScheduleConfig is shared
        // configuration (AIWorld.Routine*, loaded and cross-validated once
        // at Initialize()), passed into DeriveGoal() per call, the same
        // pattern _foodTargetConfig already uses for FoodTargetResolver.
        RoutineSystem _routineSystem;
        RoutineScheduleConfig _routineScheduleConfig;

        // Milestone 2.11D: run in UpdateNeeds() right after RoutineGoalState/
        // its own MOVE_TO handling, from facts AIWorldMgr resolves itself
        // (materialized/alive/live position vs. RoutineGoalState's target) -
        // see RoutineActivitySystem.h for why this is a separate class from
        // RoutineSystem rather than a third responsibility folded into it.
        // No config of its own; ArrivalToleranceYards (already shared with
        // MOVE_TO arrival/Eat) is reused, not duplicated.
        RoutineActivitySystem _routineActivitySystem;

        // Milestone 2.11E2: how much AgentRecord::EconomyState::Money a WORK
        // ActionCompletion reaching Succeeded/Performed adds - see
        // UpdateNeeds()'s activity block. Loaded once at Initialize() from
        // AIWorld.WorkMoneyReward.
        uint32 _workMoneyReward = 1;

        // Milestone 2.8F: cross-thread ingress for ActionEngineEvents (see
        // ActionEngineEventBus) - AIWorldCreatureAI::MovementInform()
        // publishes into it, potentially from a map-updater thread, the
        // world thread drains it once per tick in Update(), right after
        // _eventBus's own Drain(). Gated by the same _acceptEvents flag as
        // PublishWorldEvent(), for the same reason.
        ActionEngineEventBus _actionEngineEventBus;
};

#define sAIWorldMgr AIWorldMgr::instance()

#endif // AIWORLD_AIWORLDMGR_H
