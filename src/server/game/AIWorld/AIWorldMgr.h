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
#include "Agent/AgentGroupCoordinationProfile.h"
#include "Agent/AgentGroupIntentProjector.h"
#include "Agent/AgentGroupIntentSystem.h"
#include "Agent/AgentGroupLifecycleSystem.h"
#include "Agent/AgentGroupOperationSource.h"
#include "Agent/AgentGroupPolicyConfig.h"
#include "Agent/AgentGroupPolicySystem.h"
#include "Agent/AgentGroupRegistry.h"
#include "Agent/AgentGroupSimulationSystem.h"
#include "Agent/AgentId.h"
#include "Agent/AgentRegistry.h"
#include "Agent/CoalitionCandidate.h"
#include "Agent/CoalitionFormationAttempt.h"
#include "Agent/CoalitionFormationProfile.h"
#include "Agent/CoalitionFormationReservationKey.h"
#include "Agent/CoalitionFormationSystem.h"
#include "Agent/CoalitionMaintenanceSystem.h"
#include "Agent/GroupId.h"
#include "Agent/GroupMemberActionProposal.h"
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
#include <unordered_set>
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
        //
        // Milestone 2.12F4A: true only for AgentControlMode::AIWorldControlled
        // - an AgentRecord existing is no longer sufficient by itself (see
        // AgentControlMode's own comment, AgentType.h). An ObserveOnly
        // agent falls through to TrinityCore's normal AI selection exactly
        // as if it had no AgentRecord at all - the read stays safe from a
        // map-updater thread since ControlMode is only ever set at load
        // time in this milestone, never mutated concurrently with it.
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

        // Milestone 2.12E4R test hook: one-shot, gated behind
        // AIWorld.TestDissolveGroupId (default 0 = disabled) - runs first
        // among Initialize()'s own manual test actions, so an operator can
        // clear a specific leftover AgentGroup out of the way before
        // AIWorld.WolfGroupAutoFormation's own timer (which cannot fire
        // until Update() first runs, strictly after Initialize() returns)
        // ever gets a chance to see its former members as still grouped.
        // Goes through RequestDissolveGroupWithPolicy(..., Manual) - the
        // same policy-gated path any other deliberately-authorized
        // individual request already uses - never
        // AgentGroupRegistry::Remove() or a raw DB DELETE. See its own
        // .cpp comment for the full "fresh formation" regression this
        // makes repeatable.
        void RunTestDissolveGroup(GroupId groupId);

        // Milestone 2.12F3 test hook: the runtime-proof counterpart to
        // RunTestDissolveGroup() - gated behind
        // AIWorld.TestDissolveOnActiveRegroupGroupId (default 0 = disabled,
        // also set true - "already fired" - at Initialize() if the
        // configured GroupId does not resolve, see Initialize()'s own
        // comment) and _testDissolveOnActiveRegroupFired, called from
        // Update() only right after a RunCoalitionCoordination() pass
        // actually runs (2.12F3 P3 fix, round 2, STATIC review - not every
        // tick the way an earlier version did; a Regroup is only ever
        // freshly dispatched from inside that same pass, so there is
        // nothing new to find in between two passes), since (unlike
        // RunTestDissolveGroup()'s own leftover-group cleanup) this hook
        // has to WAIT for a runtime condition that cannot exist yet at
        // Initialize() time: some member of the configured group actually
        // mid-REGROUP, proven by the full ownership provenance tuple
        // (2.12F3 P3 fix, round 2, STATIC review - not just
        // ActiveActionState::SourceGoal/GroupCoordinationGoalState::SourceGroup
        // alone, which does not by itself prove a MOVE_TO is really
        // running): ActiveActionState::Type == MoveTo,
        // ActiveActionState::SourceGoal == Regroup,
        // GroupCoordinationGoalState::Type == Regroup,
        // GroupCoordinationGoalState::SourceGroup naming this exact group,
        // the two attempts' own StartedAtMs/GoalStartedAtMs identity
        // matching, the member Materialized with a live, transiently-
        // resolved Creature, and HasOwnMoveToGenerator() actually true on
        // it - see this method's own definition for the full reasoning.
        // Once observed, requests a Manual dissolve through
        // RequestDissolveGroupWithPolicy() - the same entry point
        // RunTestDissolveGroup() itself uses, never AgentGroupRegistry::
        // Remove(), never a raw StopMoveTo() called directly from this
        // hook, never a raw DB DELETE - so this proves AIWorldMgr::
        // RequestDissolveGroup()'s own StopGroupCoordinationForMember()
        // call (2.12F2 P2 fix, STATIC review) actually stops a real
        // in-flight Regroup end to end, through the exact same production
        // path a real dissolve-while-regrouping would take. Sets
        // _testDissolveOnActiveRegroupFired BEFORE submitting the dissolve
        // request, unconditionally - fires at most once ever, the same
        // guarantee every other AIWorld.Test* hook in this file gives. Its
        // own completion logs CONFIRMED, not PASSED (2.12F3 P3 fix, STATIC
        // review) - a successful dissolve alone does not prove it actually
        // stopped a still-running REGROUP, only that the dissolve itself
        // committed; the triggering member's REGROUP could naturally have
        // already ended on its own before the async dissolve confirmed.
        // Also self-disables (logs, sets _testDissolveOnActiveRegroupFired)
        // if the configured group no longer resolves at all (2.12F3 P3 fix,
        // round 3, STATIC review) - the group can legitimately dissolve
        // through the normal lifecycle path before this hook ever observes
        // an active REGROUP, and Initialize()'s own existence check only
        // ever catches a target already gone at startup, not one that
        // vanishes later; without this, a vanished target would otherwise
        // be polled once per coordination pass forever.
        void CheckTestDissolveOnActiveRegroup();

        // Milestone 2.12E4C2 P2 fix (STATIC review): one-shot, gated
        // behind AIWorld.AdoptGroupId (default 0 = disabled) - the
        // controlled adoption path for a group whose persistent
        // AgentGroupRecord::ProfileId (added this same milestone, backfilled
        // to Invalid for every pre-existing row) does not yet reflect which
        // CoalitionFormationProfile actually governs it. Deliberately NOT
        // automatic: this milestone's own migration backfills every
        // pre-existing group to ProfileId::Invalid rather than guessing,
        // since silently relabeling an existing Loose group as WolfLoose
        // (or any other profile) with no operator confirmation is exactly
        // the kind of false-provenance assignment this whole ProfileId
        // field exists to prevent. Recovering a specific, already-known-
        // legitimate pre-existing group is instead this explicit, human-
        // triggered call - the same "no raw SQL/direct registry mutation,
        // go through the existing persistence API" discipline
        // RunTestDissolveGroup() already holds, applied to a metadata
        // correction instead of a lifecycle operation. Refuses anything but
        // a recognized, non-Invalid CoalitionFormationProfileId (an
        // explicit allow-list, not merely "!= Invalid" - a stray/garbage
        // config value must be refused here, not deferred to the next
        // restart's own LoadGroups() fail-closed switch to catch) or an
        // unresolvable groupId.
        //
        // 2.12E4C2 P2 fix, round 2 (STATIC review): submits
        // AgentGroupPersistence::AdoptGroupProfileAsync() (a confirmed
        // TransactionCallback, enqueued into _groupLifecyclePending like
        // every other AgentGroup persistence write) rather than mutating
        // AgentGroupRecord::ProfileId immediately and calling the existing
        // fire-and-forget SaveGroupState() - an earlier version did exactly
        // that and logged "adopted" before the DB write was even known to
        // have landed, so a failed async UPDATE could leave
        // RunCoalitionMaintenance() treating an only-in-RAM-adopted group
        // as WolfLoose (able to Leave/Dissolve its members) while the DB
        // itself still read Invalid, reverting silently on the next
        // restart. The in-memory AgentGroupRecord::ProfileId is now only
        // ever mutated inside that completion, once success is confirmed -
        // "adopted" is never logged, and the group is never treated as
        // adopted by anything else, before that.
        //
        // 2.12E4C2 P2 fix, round 3 (STATIC review): marks groupId in
        // _groupProfileAdoptionInFlight for the duration of the async
        // write - see that member's own declaration comment for the
        // cross-write race this closes against the group coarse tick's
        // own SaveGroupState() (a second, uncoordinated writer of the same
        // profile_id column that could otherwise commit either before or
        // after this one and silently undo it).
        void RunGroupProfileAdoption(GroupId groupId, CoalitionFormationProfileId profileId);

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

        // Milestone 2.12E4C1: manual proof of CoalitionMaintenanceSystem's
        // own rules, entirely pure - synthetic AgentGroupRecord/
        // CoalitionMemberObservation values built on the stack, fed
        // straight to a stack-local CoalitionMaintenanceSystem::Evaluate(),
        // no registry/DB/AIWorldMgr member state touched at all (unlike
        // RunGroupPolicySmokeTest(), this has no integration half yet -
        // 2.12E4C1 deliberately adds no lifecycle orchestration, see
        // CoalitionMaintenanceSystem.h's own class comment). Exercises
        // every rule CoalitionMaintenanceSystem.cpp implements: a
        // materialized/alive/same-map member past LeaveRadius proposes
        // LeaveMember; one well within it proposes None; an unloaded or
        // dead member is never a Leave candidate regardless of its last
        // known position; a group already below MinMembers proposes
        // DissolveGroup outright; and - the one case that matters most -
        // Evaluate() proposes LeaveMember for a Stable group's own
        // far-away member exactly as readily as for a Loose one, proving
        // this class does not (and must not) special-case Stable itself.
        // Always runs when this method is called at all (see
        // AIWorld.TestCoalitionMaintenance).
        void RunCoalitionMaintenanceSmokeTest() const;

        // Milestone 2.12F1: manual proof of AgentGroupIntentSystem's own
        // rules, entirely pure - synthetic AgentGroupRecord/
        // CoalitionMemberObservation/AgentGroupCoordinationProfile values
        // built on the stack, fed straight to a stack-local
        // AgentGroupIntentSystem::Evaluate(), no registry/DB/AIWorldMgr
        // member state touched at all (the same "pure layer first, zero
        // orchestration" scoping CoalitionMaintenanceSystem's own
        // RunCoalitionMaintenanceSmokeTest() already established for
        // 2.12E4C1 - 2.12F1 deliberately adds no AgentGroupIntentProjector/
        // ActionSystem wiring yet, see AgentGroupIntentSystem.h's own
        // class comment). Exercises every rule AgentGroupIntentSystem.cpp
        // implements: a materialized/alive/same-map member past
        // RegroupRadius proposes Regroup; one well within it proposes
        // None; an unloaded or dead member is never a Regroup trigger
        // regardless of its last known position; RegroupEnabled=false
        // proposes None even for an otherwise-textbook dispersed member;
        // and an Invalid profile, a Kind mismatch, or a ProfileId
        // mismatch all fail closed to None, the same three-way guard
        // CoalitionMaintenanceSystem::Evaluate() already holds. Always
        // runs when this method is called at all (see
        // AIWorld.TestGroupIntent).
        void RunGroupIntentSmokeTest() const;

        // Milestone 2.12F2: manual proof of AgentGroupIntentProjector's own
        // rules, entirely pure - a synthetic AgentGroupIntent/
        // AgentGroupCoordinationProfile/CoalitionMemberObservation values
        // built on the stack, fed straight to a stack-local
        // AgentGroupIntentProjector::Project(), no registry/DB/AIWorldMgr
        // member state touched at all (the same "pure layer first" scoping
        // RunGroupIntentSmokeTest() itself already established for
        // AgentGroupIntentSystem - this milestone deliberately adds no
        // ActionSystem/orchestration wiring yet, see
        // AgentGroupIntentProjector.h's own class comment). Exercises every
        // rule AgentGroupIntentProjector.cpp implements: a materialized/
        // alive/same-map member past RegroupRadius gets a proposal
        // targeting the intent's own point; one well within it gets none;
        // an unloaded or dead member never gets one regardless of its last
        // known position; a different-map member never gets one either;
        // and intent.Type == None produces no proposals for anyone,
        // regardless of how dispersed the members are. Always runs when
        // this method is called at all (see AIWorld.TestGroupIntentProjector).
        void RunGroupIntentProjectorSmokeTest() const;

        // Milestone 2.12F4A: manual proof only, gated behind
        // AIWorld.TestControlMode (default 0 = disabled) - runs at most once,
        // from Initialize(), only against a real, already-registered AgentId
        // (never a fake/ghost AgentRecord). Proves ActionSystem::Validate()'s
        // own mandatory, authoritative ControlMode gate: the exact same
        // otherwise-fully-valid MOVE_TO ActionRequest/context is validated
        // twice, with only ActionValidationContext::ControlMode changed
        // between the two calls (ObserveOnly, then AIWorldControlled) - so
        // any difference in outcome can only be attributed to that gate,
        // never to some other check incidentally differing too. Entirely
        // pure - no live Creature/Map/grid access, since the gate under
        // test is a plain-value check inside ActionSystem itself.
        void RunControlModeSmokeTest(AgentId testAgentId) const;

        // Milestone 2.12F4B: bidirectional global reconciliation between
        // eligible persistent non-instance world.creature spawns and
        // ai_agents - gated behind AIWorld.EnableSpawnReconciliation
        // (default false, 2.12F4B P2 fix STATIC review - see Initialize()'s
        // own comment for why this stays opt-in until 2.12F4C's scale
        // hardening lands), called right after _persistence.LoadAgents(
        // _registry) and before anything (group/memory loaders, the
        // AIWorld.TestSpawnId fixture) consumes _registry, so every
        // downstream startup step sees the final, reconciled state. The
        // diff is built against ai_agents' own PHYSICAL content
        // (_persistence.LoadAllBindings(), 2.12F4B P2 fix STATIC review)
        // rather than _registry, so a row LoadAgents() itself already
        // quarantined (AgentId != SpawnId, 2.12F4A2) is still known to
        // occupy its (map_id, spawn_id) binding AND its agent_id, and is
        // never treated as Missing - nor is a census spawn whose intended
        // new AgentId collides with a different quarantined row's own
        // agent_id (AGENT_ID_COLLISION, see SpawnReconciliationPlan.h).
        // Every physical row not eligible for the census is also checked
        // against BuildAllKnownCreatureSpawnIds() before being treated as
        // ORPHANED, so an existing-but-out-of-scope spawn (e.g. instance/
        // raid) is left untouched rather than misclassified as orphaned
        // and quarantined. MISSING census entries get a freshly created
        // AgentRecord (AgentId = SpawnId, ControlMode = ObserveOnly -
        // reconciliation never mass-grants AIWorldControlled), inserted
        // via a genuine chunked multi-row INSERT (AgentPersistence::
        // CreateCreatureAgentsBatch()), not one statement per row.
        // ORPHANED and CONFLICTED (spawn still eligible, but the row's own
        // stored MapId disagrees with world.creature) bindings are both
        // removed from _registry only - never an aggressive ai_agents
        // DELETE/auto-repair, see RunSpawnReconciliation()'s own .cpp
        // comment. Bounded/administered (runs once at startup, never
        // recurring per-tick) - scale hardening for a fully-reconciled,
        // thousands-of-agents population is 2.12F4C's own job, not this
        // one's.
        void RunSpawnReconciliation();

        // Milestone 2.12E4R (generalized from 2.12E4A's
        // CollectWolfCoalitionCandidates()): builds the value-only
        // candidate list CoalitionFormationSystem::Propose() needs - one
        // entry per currently registered agent that is Materialized and
        // alive, with NO further eligibility filtering at all (STATIC
        // review's own "WORLD OBSERVATION / FORMATION PROFILE split"
        // guidance): this method has no idea which CreatureEntry, Kind, or
        // group membership any particular CoalitionFormationProfile cares
        // about - that is CoalitionFormationSystem::Propose() and
        // RunCoalitionFormation()'s own job (see CollectMemberIdsOfKind()
        // for the membership half). Never mutates anything - no
        // BindCreature/UnbindCreature call, no registry/DB write. A
        // candidate's position/MapId/CreatureEntry come from its live
        // Creature, resolved fresh this call, never a stored/stale
        // AgentRecord field - the same "the live Creature is the ground
        // truth for where an agent actually is" discipline
        // UpdateNeeds()/RunDecisionScheduler() already follow. Never
        // force-loads a grid: an agent whose Creature does not currently
        // resolve is simply absent from the returned list, the same as if
        // it did not exist for this pass at all - an unload must never be
        // misread as a formation-relevant fact.
        std::vector<CoalitionCandidate> CollectCoalitionCandidates();

        // Milestone 2.12E4R (STATIC review): replaces the old wolf-only
        // IsMemberOfAnyLooseGroup() - builds a one-off O(1)-lookup set of
        // every AgentId currently a member of ANY group of exactly kind,
        // by walking _groupRegistry.GetGroups()/Find() exactly once per
        // RunCoalitionFormation() call, rather than re-walking every group
        // (and every one of its members) for every single candidate the
        // way the method this replaces did - the performance debt that
        // review flagged. AgentGroupRegistry::IsMemberOfKind() is
        // deliberately NOT called once per candidate here for that same
        // reason; IsMemberOfKind() itself stays reserved for a single,
        // one-off re-check (see RunCoalitionJoinStep()), where building a
        // whole snapshot for one member would be wasted work.
        std::unordered_set<uint64> CollectMemberIdsOfKind(AgentGroupKind kind) const;

        // Milestone 2.12E4R (generalized from 2.12E4A/B's
        // RunWolfCoalitionFormation()): AIWorld.WolfGroupFormationIntervalMs-
        // cadenced pass, only ever called while AIWorld.WolfGroupAutoFormation
        // is enabled. Refuses profile.Id == Invalid outright (2.12E4R P3
        // fix, STATIC review - see CoalitionFormationProfileId.h), refuses
        // if profile.Id is already in _formationInFlight (at most one
        // attempt in flight PER PROFILE - see its own declaration comment),
        // and refuses if _formationInFlight.size() has already reached
        // _coalitionFormationMaxInFlight (2.12E4R P3 fix, STATIC review:
        // the per-profile bound alone has no ceiling on how many DIFFERENT
        // profiles' formations can all be in flight at once - see
        // _coalitionFormationMaxInFlight's own declaration comment).
        //
        // Builds this pass's candidate list (CollectCoalitionCandidates()),
        // excludes every candidate already a CONFIRMED member of a
        // profile.Kind group (CollectMemberIdsOfKind()) AND every candidate
        // already RESERVED for a profile.Kind formation still in flight
        // (_formationReservedMembers - 2.12E4R P2 fix, STATIC review, see
        // its own declaration comment for the cross-profile race this
        // closes), and asks _coalitionFormationSystem.Propose() for a
        // deterministic CoalitionProposal. No proposal this pass is not an
        // error - it just means nothing eligible was close enough right
        // now, and nothing is reserved or added to _formationInFlight for
        // it.
        //
        // On an actual proposal: reserves every Proposal.Members entry
        // (CoalitionFormationReservationKey{member, profile.Kind}) BEFORE
        // submitting RequestCreateGroup() - this is what actually closes
        // the cross-profile race, not the join-time re-check alone (that
        // re-check only ever sees CONFIRMED AgentGroupRegistry state, which
        // is exactly the window two same-Kind profiles' own in-flight
        // CreateGroup/Join chains do not yet occupy) - then turns the
        // proposal into a real AgentGroup via RequestCreateGroup() followed
        // by a RequestJoinGroupWithPolicy(AutomaticPolicy) chain (see
        // RunCoalitionJoinStep()). Reservations are released
        // (ReleaseCoalitionFormationReservations()) at every terminal
        // outcome - a failed CreateGroup here, a fully successful join
        // chain, or a cleaned-up aborted one - never left dangling.
        //
        // Only one profile exists today (AIWorldMgr's own
        // _wolfLooseFormationProfile, built once at Initialize()), but
        // nothing here reads AIWorld.Wolf* config directly - a second
        // profile is just another CoalitionFormationProfile value and call
        // site.
        void RunCoalitionFormation(CoalitionFormationProfile const& profile);

        // Milestone 2.12E4R (generalized from 2.12E4B's
        // RunWolfCoalitionJoinStep()): joins
        // attempt.Proposal.Members[attempt.NextMemberIndex] with
        // AgentGroupOperationSource::AutomaticPolicy, then recurses with
        // NextMemberIndex+1 - once NextMemberIndex reaches
        // attempt.Proposal.Members.size(), formation is complete, logged
        // PASSED, attempt.Proposal's own reservations are released
        // (ReleaseCoalitionFormationReservations() - 2.12E4R P2 fix, now
        // redundant with real AgentGroupRegistry membership but released
        // for symmetry with every other terminal outcome), and
        // attempt.ProfileId is erased from _formationInFlight. The same
        // "chain each step from the previous one's own completion" shape
        // RunGroupLifecycleSmokeTestJoinStep() already uses, for the same
        // reason (AgentGroupLifecycleSystem allows at most one in-flight
        // operation per GroupId - see its own header comment). Any join
        // failure aborts the whole formation and best-effort dissolves the
        // group already created for it (AbortCoalitionFormation()) rather
        // than leaving a partially joined automatic coalition behind.
        //
        // 2.12E4A/B P2 fix (STATIC review), preserved unchanged by this
        // generalization: re-checks the member still resolves in
        // _registry AND is still not a member of any attempt.Proposal.Kind
        // group (AgentGroupRegistry::IsMemberOfKind()), immediately before
        // issuing its own join - the exact eligibility
        // CollectCoalitionCandidates()/CollectMemberIdsOfKind() already
        // checked once when the proposal was built, but that snapshot can
        // go stale by the time a later step in this chain actually runs
        // (the async CreateGroup round trip, and every join before this
        // one, all take real time - a manual/lifecycle/other-policy change
        // to this member's own membership could land in between).
        // RequestJoinGroupWithPolicy()'s own CanJoin() only ever validates
        // against THIS groupId - AgentGroupPolicySystem deliberately has no
        // global one-group invariant (see its own class comment) - so it
        // cannot catch a member that is already in some OTHER group of the
        // same Kind; only this re-check (plus RunCoalitionFormation()'s own
        // reservation, for the cross-PROFILE case specifically) can. A
        // failure here aborts and cleans up the same way a rejected/failed
        // join itself does.
        void RunCoalitionJoinStep(CoalitionFormationAttempt attempt);

        // Milestone 2.12E4R (generalized from 2.12E4B's
        // RunWolfCoalitionFormationAbort()): logs the formation attempt as
        // FAILED (naming the member that could not join) and fires a
        // fire-and-forget RequestDissolveGroup() best-effort cleanup for
        // attempt.Group - the same reasoning
        // RunGroupLifecycleSmokeTestAbort() already documents for the
        // manual lifecycle smoke test. This is cleanup of an already
        // policy-approved formation operation (CanJoin() already said
        // Allowed for every member that got this far), never a way to
        // bypass automatic policy. attempt.Proposal's own reservations are
        // released (ReleaseCoalitionFormationReservations()) and
        // attempt.ProfileId is erased from _formationInFlight only once
        // this cleanup dissolve itself completes (success or failure),
        // never synchronously inside this method - see
        // _formationInFlight's own declaration comment.
        void AbortCoalitionFormation(CoalitionFormationAttempt const& attempt, AgentId failedMember);

        // Milestone 2.12E4R P2 fix (STATIC review): erases
        // CoalitionFormationReservationKey{member.Value, proposal.Kind} for
        // every proposal.Members entry from _formationReservedMembers - the
        // one place every RunCoalitionFormation()/RunCoalitionJoinStep()/
        // AbortCoalitionFormation() terminal outcome releases its own
        // reservations, so there is exactly one place that has to get the
        // "release everything this proposal reserved" logic right.
        void ReleaseCoalitionFormationReservations(CoalitionProposal const& proposal);

        // Milestone 2.12E4C2: builds one CoalitionMemberObservation per
        // group.Members entry - the maintenance-side counterpart to
        // CollectCoalitionCandidates(), walking an EXISTING group's own
        // membership rather than scanning the whole registry for new
        // candidates. Never mutates anything, never force-loads a grid -
        // a member whose Creature does not currently resolve (or whose
        // AgentRecord is not even Materialized) simply gets a default
        // (Materialized=false) observation, never skipped outright, so
        // CoalitionMaintenanceSystem::Evaluate() sees one observation per
        // membership edge, present or not.
        std::vector<CoalitionMemberObservation> CollectCoalitionMemberObservations(AgentGroupRecord const& group) const;

        // Milestone 2.12E4C2: AIWorld.CoalitionMaintenanceIntervalMs-
        // cadenced pass, only ever called while AIWorld.CoalitionMaintenance
        // is enabled - a SEPARATE gate from AIWorld.WolfGroupAutoFormation
        // (2.12E4C2 P2 fix, STATIC review - see _coalitionMaintenanceEnabled's
        // own declaration comment for why formation and maintenance must
        // not share one on/off switch).
        //
        // Milestone 2.12E4C2 P2 fix, round 2 (STATIC review): takes NO
        // profile parameter any more - an earlier version was itself a
        // per-profile function called once per configured profile, each
        // with its own _maintenanceScanCursor... except there was only
        // ever ONE cursor (a single AIWorldMgr member), shared across
        // every call. With only WolfLoose configured this happened to
        // work; the moment a second profile existed, both would share and
        // mutate that same cursor (profile A's own pass advances it past
        // groups 1-100, profile B's own pass - sharing that SAME
        // post-A cursor position - then only ever sees 101-200, never
        // 1-100, and the next pass both wrap together from wherever B
        // left it) - a group whose GroupId never happens to fall in
        // whichever slice its own profile's call ends up scanning is
        // starved forever, the same class of cross-profile bug already
        // fixed once for formation's own member reservations (see
        // CoalitionFormationReservationKey.h). Discovery is now a SINGLE
        // GLOBAL bounded scan across ALL groups regardless of profile,
        // with per-group profile resolution happening AFTER discovery -
        // see the two stages below - so total discovery work stays
        // bounded by one scan bound regardless of how many profiles exist,
        // rather than growing as profileCount * ScanMaxPerPass.
        //
        // Two independently bounded stages, deliberately not one:
        //   1. DISCOVERY: AgentGroupRegistry::GetGroupsAfterUntil(_maintenanceScanCursor,
        //      _maintenanceScanCycleHighWater, AIWorld.CoalitionMaintenanceScanMaxPerPass)
        //      - 2.12E4C2 P3 fix. An earlier version built its candidate
        //      list from GetGroups(), which materializes and filters
        //      EVERY registered group every single pass regardless of how
        //      few of them ever get admitted - O(all groups) recurring
        //      world-thread work, exactly the class of problem
        //      GroupCoarseSimulationScheduler was already introduced to
        //      prevent for admission, just not yet for discovery.
        //
        //      2.12E4C2 P2 fix, round 3 (STATIC review): an interim
        //      version bounded discovery per PASS (GetGroupsAfter(), no
        //      upper bound) but not per SCAN CYCLE - GroupIds are
        //      monotonically increasing, so under continuous group
        //      creation (new groups always minted with a HIGHER id than
        //      any that exist yet) the scan could always find a higher
        //      GroupId waiting past the cursor and would never reach
        //      empty, never wrap, so the earliest-created groups (closest
        //      to GroupId{}) would never be revisited - starved
        //      indefinitely, not merely delayed. _maintenanceScanCycleHighWater
        //      (see its own declaration comment) fixes this: it snapshots
        //      GetHighestGroupId() once at the START of each scan cycle,
        //      and the whole cycle only ever scans up to that snapshot -
        //      a group created mid-cycle is deferred to the NEXT cycle's
        //      own snapshot, never chased by the one already running, so
        //      every cycle is provably finite regardless of creation
        //      rate. The pass immediately after one that reaches the
        //      cycle's own high-water mark gets an EMPTY result and
        //      starts a fresh cycle (new snapshot, cursor reset to
        //      GroupId{}) - a tail pass that returns a non-empty but
        //      under-capacity batch does NOT itself backfill from the
        //      start to top up to ScanMaxPerPass, it simply leaves some
        //      of that pass's own budget unused. Every group that existed
        //      at the start of a cycle is still seen exactly once by the
        //      end of that cycle, spread over several bounded passes
        //      instead of scanned in one unbounded one.
        //   2. Per discovered group, ResolveMaintenanceProfile(group->ProfileId)
        //      - NOT a check against one fixed profile.ProfileId parameter
        //      any more, since this function no longer receives one. A
        //      group whose ProfileId does not resolve (Invalid, or a
        //      profile this process does not know how to maintain) is
        //      simply not a maintenance candidate this pass - see that
        //      method's own comment for why a manually/admin-created group
        //      must never be swept in just because some profile's Kind
        //      happens to match its own. The resolved candidates are then
        //      handed to GroupCoarseSimulationScheduler
        //      (_maintenanceScheduler/_maintenanceSchedule - its own
        //      dedicated instance/schedule map, entirely separate from
        //      _groupCoarseSimulationScheduler/_groupSimulationSchedule's
        //      own resource-drift tick) for the same phase-offset, bounded,
        //      deterministic per-pass admission the group coarse tick
        //      already uses, capped at AIWorld.CoalitionMaintenanceMaxPerPass
        //      - globally, across every profile mixed together in this
        //      pass's own discovered batch, not per profile.
        // Every admitted group is handed to RunCoalitionMaintenanceForGroup()
        // together with its own already-resolved CoalitionMaintenanceProfile.
        void RunCoalitionMaintenance();

        // Milestone 2.12E4C2 P2 fix, round 2 (STATIC review): the one
        // place that maps a persistent AgentGroupRecord::ProfileId to the
        // CoalitionMaintenanceProfile that governs it - std::nullopt for
        // Invalid or any profileId this process has no maintenance rules
        // configured for. Today only WolfLoose resolves (to
        // _wolfLooseMaintenanceProfile); a future second profile is just
        // another case added here, not a change to RunCoalitionMaintenance()'s
        // own discovery/admission shape.
        std::optional<CoalitionMaintenanceProfile> ResolveMaintenanceProfile(CoalitionFormationProfileId profileId) const;

        // Milestone 2.12E4C2: one group this pass's maintenance scheduler
        // admitted, together with the CoalitionMaintenanceProfile
        // RunCoalitionMaintenance() already resolved for it via
        // ResolveMaintenanceProfile() (2.12E4C2 P2 fix, round 2 - a single
        // pass's own admitted batch can now mix groups governed by
        // different profiles, so profile is no longer necessarily the
        // same value across every call in one pass). Refuses to start if
        // groupId is already in
        // _maintenanceInFlight (see its own declaration comment for the
        // repeated-request spam this guards against) or no longer resolves
        // in _groupRegistry. Builds this group's own observations
        // (CollectCoalitionMemberObservations()) and asks
        // _coalitionMaintenanceSystem.Evaluate() for a deterministic
        // CoalitionMaintenanceDecision:
        //   - LeaveMember: marks groupId in flight, then
        //     RequestLeaveGroupWithPolicy(..., AutomaticPolicy) - the same
        //     policy-gated path RunCoalitionJoinStep() already uses for
        //     joins, so a Stable group's own member is protected the exact
        //     same way an automatic formation join would be rejected for
        //     one (see AgentGroupPolicySystem::CanLeave()). On a confirmed
        //     leave, chains into RunCoalitionMaintenanceAfterLeave() -
        //     never re-uses the AgentGroupRecord* this pass already
        //     resolved, always re-resolves fresh from _groupRegistry once
        //     the leave actually confirms.
        //   - DissolveGroup: marks groupId in flight, then straight into
        //     RunCoalitionMaintenanceDissolveGroup().
        //   - None: nothing to do, groupId is never marked in flight for
        //     it.
        void RunCoalitionMaintenanceForGroup(GroupId groupId, CoalitionMaintenanceProfile const& profile);

        // Milestone 2.12E4C2: runs once a LeaveMember request this same
        // maintenance pass already confirmed - re-resolves groupId fresh
        // from _groupRegistry (never trusts the AgentGroupRecord*/snapshot
        // RunCoalitionMaintenanceForGroup() itself resolved before the
        // leave; that reference is stale by the time this runs, possibly
        // several ticks later - the same "a completion never trusts
        // request-time validity" discipline AgentGroupLifecycleSystem.h's
        // own header comment already documents). An unknown groupId
        // (dissolved some other way in the meantime) just clears
        // _maintenanceInFlight and returns. Otherwise: Kind == Loose and
        // now below profile.MinMembers chains into
        // RunCoalitionMaintenanceDissolveGroup() (still going through
        // policy, so Stable is never reachable here in the first place -
        // Kind is re-read from the freshly-resolved group, not assumed
        // from profile); anything else clears _maintenanceInFlight - the
        // leave alone was the whole maintenance action needed this pass.
        void RunCoalitionMaintenanceAfterLeave(GroupId groupId, CoalitionMaintenanceProfile profile);

        // Milestone 2.12E4C2: RequestDissolveGroupWithPolicy(...,
        // AutomaticPolicy) for groupId - reached either directly from a
        // DissolveGroup decision (a group already below minimum,
        // independent of any single member's own distance) or from
        // RunCoalitionMaintenanceAfterLeave() (a group that just dropped
        // below minimum as a result of its own confirmed leave). Going
        // through policy here (never a raw RequestDissolveGroup()) is what
        // keeps a Stable group protected - AgentGroupPolicySystem::
        // ShouldDissolve() already refuses Stable unconditionally, the
        // same protection RunCoalitionMaintenanceForGroup()'s own
        // LeaveMember path gets from CanLeave(). Always clears
        // _maintenanceInFlight once this dissolve attempt itself completes
        // (success or failure) - the one place every
        // RunCoalitionMaintenanceForGroup()/RunCoalitionMaintenanceAfterLeave()
        // path that reaches a dissolve funnels through, so there is
        // exactly one place that has to get that release right.
        void RunCoalitionMaintenanceDissolveGroup(GroupId groupId);

        // Milestone 2.12F2: the one place that maps a persistent
        // AgentGroupRecord::ProfileId to the AgentGroupCoordinationProfile
        // that governs it - std::nullopt for Invalid or any profileId this
        // process has no coordination rules configured for. Today only
        // WolfLoose resolves (to _wolfLooseCoordinationProfile); a future
        // second profile is just another case added here, the same
        // "profile identity is data, not a new code path" discipline
        // ResolveMaintenanceProfile() already established - see this
        // milestone's own architectural mandate: a second group TYPE must
        // never require a new orchestration method, only a new profile
        // value.
        std::optional<AgentGroupCoordinationProfile> ResolveCoordinationProfile(CoalitionFormationProfileId profileId) const;

        // Milestone 2.12F2: the bounded orchestration pass - AgentGroup ->
        // generic observations -> generic intent (_agentGroupIntentSystem)
        // -> per-member proposals (_agentGroupIntentProjector) -> individual
        // ActionRequest (DispatchGroupMemberActionProposal()). Reuses the
        // exact two-stage bounded-discovery shape RunCoalitionMaintenance()
        // already established (AgentGroupRegistry::GetGroupsAfterUntil()/
        // GetHighestGroupId(), AIWorld.GroupCoordinationScanMaxPerPass) -
        // its own dedicated cursor/high-water pair
        // (_coordinationScanCursor/_coordinationScanCycleHighWater),
        // entirely separate from _maintenanceScanCursor/
        // _maintenanceScanCycleHighWater. A shared cursor would reproduce
        // the exact cross-scan starvation _maintenanceScanCursor's own
        // 2.12E4C2 P2 fix already closed for cross-PROFILE sharing within
        // one concern - two entirely different concerns (maintenance,
        // coordination) scanning the same registry must never share one
        // cursor either, for the same reason.
        //
        // Deliberately no second GroupCoarseSimulationScheduler-backed
        // admission tier the way RunCoalitionMaintenance() has - dispatching
        // a MOVE_TO proposal is a cheap, synchronous, in-memory operation
        // (no async DB write the way an automatic leave/dissolve is), so
        // there is nothing here that needs a separate admission bound
        // beyond the scan bound itself.
        //
        // Milestone 2.12F2 P2 fix (STATIC review): a discovered group whose
        // GroupId currently has a Join/Leave/Dissolve in flight
        // (_groupLifecycleSystem.HasPendingOperation()) is skipped entirely
        // this pass, before ResolveCoordinationProfile()/observations/intent
        // are even attempted. AgentGroupLifecycleSystem only ever mutates
        // AgentGroupRecord::Members inside a Request*'s own confirmed
        // completion (see its own header comment) - so a group with an
        // operation still in flight has membership that is ABOUT to change
        // but has not yet, and evaluating intent/proposing movement against
        // that stale snapshot could dispatch a fresh Regroup for a member
        // whose Leave is already committed to the DB and about to remove
        // them. Failing closed here (skip the whole group, not just the
        // one member already known to be leaving - Dissolve affects every
        // member, and this call has no cheap way to tell which case it is
        // without re-deriving lifecycle's own bookkeeping) is simpler and
        // safer than trying to filter the one affected member out: the
        // group is re-evaluated fresh, with fresh membership, the very next
        // pass once the operation resolves either way.
        //
        // Milestone 2.12F2 P2 fix, round 2 (STATIC review): overlap
        // arbitration is checked per-proposal against how many
        // RegroupEnabled groups that proposal's own Member currently
        // belongs to (AgentGroupRegistry::GetGroupsOfMember(), 2.12F2 P3
        // fix, round 2 - see that method's own comment), not against how
        // many proposals this pass's own bounded discovery batch happened
        // to produce. A first version of this fix collected proposals per
        // pass and counted duplicates only within that same batch - STATIC
        // review correctly identified that as provably incomplete: with
        // AIWorld.GroupCoordinationScanMaxPerPass smaller than the
        // registry, two overlapping groups can simply never be discovered
        // in the same pass, so a batch-local count silently degrades back
        // to "whichever group's own pass runs first wins", the exact
        // hidden discovery-order priority this mechanism exists to remove.
        // A second version fixed THAT by scanning the whole registry once
        // per call up front - STATIC review correctly identified THAT as
        // reintroducing the exact O(all groups) recurring world-thread
        // work class already eliminated from maintenance discovery (see
        // this method's own bounded-discovery paragraph above). Checking
        // per-PROPOSAL via GetGroupsOfMember() instead means this
        // arbitration step's total cost scales with how many proposals
        // this pass actually produced (typically small, and already
        // bounded by AIWorld.GroupCoordinationScanMaxPerPass indirectly via
        // discovered group count), never with total registry size. Nothing
        // in AgentGroupPolicySystem::CanJoin() enforces that one agent can
        // only ever be a real member of ONE coordination-enabled group at a
        // time (it only checks duplicate membership WITHIN the target group
        // and that group's own capacity) - so two different groups can, in
        // principle, both legitimately claim the same AgentId. Any AgentId
        // that belongs to more than one group whose own resolved profile
        // has RegroupEnabled gets NO proposal dispatched for it, from any
        // of them, for as long as that overlap persists - this generic
        // layer has no basis to pick a winner between two coordination
        // profiles it knows nothing else about; leaving the member standing
        // still (re-evaluated fresh next pass) is the only choice that does
        // not invent an implicit, undocumented arbitration rule. Logged at
        // DEBUG, at most once per conflicted AgentId per call - see this
        // method's own definition for why (2.12F2 P3 fix, STATIC review: an
        // earlier version logged a WARN per proposal per pass, turning a
        // persistent, expected arbitration state into recurring log spam).
        //
        // Milestone 2.12F2 P2 fix, round 4 (STATIC review): this arbitration
        // only ever protects a NEW dispatch - it says nothing about an
        // ALREADY in-flight Regroup that a confirmed Join, some time after
        // that dispatch, newly makes ambiguous. See
        // ReconcileGroupCoordinationForMember() (called from
        // RequestJoinGroupWithPolicy()'s own confirmed-join completion, not
        // from here) for the counterpart that closes that gap; both share
        // the exact same CountRegroupEnabledMemberships() definition of
        // "ambiguous" so the two can never silently disagree.
        //
        // Per resolved, non-pending group: CollectCoalitionMemberObservations(),
        // _agentGroupIntentSystem.Evaluate(), and - only if the result is
        // AgentGroupIntentType::Regroup - _agentGroupIntentProjector.
        // Project(), whose proposals are appended to this pass's own
        // combined batch.
        void RunCoalitionCoordination();

        // Milestone 2.12F2: the one place a GroupMemberActionProposal
        // (AgentGroupIntentProjector's own output - a group-level intent
        // already decomposed to one individual member, but still only a
        // PROPOSAL, not an authorization - see GroupMemberActionProposal.h)
        // is turned into a real, individually-validated MOVE_TO
        // ActionRequest (SourceGoal = GoalType::Regroup). Full revalidation,
        // never trusting anything RunCoalitionCoordination() itself already
        // resolved earlier this same pass (a proposal can be several
        // members old by the time this specific one runs):
        //   - proposal.SourceIntent == AgentGroupIntentType::Regroup (2.12F2
        //     P3 fix, STATIC review - fail-closed the same way
        //     AgentGroupIntentProjector::Project() itself now is; today the
        //     only way this can ever be false is proposal being a default-
        //     constructed/malformed value, since Project() itself never
        //     produces anything else, but this must never silently start a
        //     MOVE_TO for a future intent type this dispatcher does not yet
        //     know how to honestly source-tag).
        //   - proposal.Member still resolves in _registry.
        //   - proposal.SourceGroup still resolves in _groupRegistry, AND
        //     proposal.Member is still actually one of its Members (a leave
        //     that lands between Project() and this call must not dispatch
        //     a movement for a member who already left).
        //   - the member is Materialized with a live, resolvable Creature,
        //     and Alive.
        //   - no higher-priority individual reason to ignore this: neither
        //     ActiveGoalState nor RoutineGoalState set (Regroup is the
        //     LOWEST of the three tiers - see AIWorldMgr::UpdateNeeds()'s
        //     own arbitration comment), and no ActiveActionState already
        //     running (a member already mid-action, of ANY SourceGoal, is
        //     left alone - this pass never preempts anything; only
        //     UpdateNeeds()'s own COORDINATION_PREEMPTED_BY_GOAL/routine-
        //     preemption blocks ever interrupt an in-flight Regroup, never
        //     the reverse).
        //   - proposal.X/Y/Z is within ActionSystem::CoordinationMoveToRangeYards()
        //     of the member's own actual current position (2.12F2 P3 fix,
        //     round 2, STATIC review - explicit "unreachable coordination
        //     member" semantics). AgentGroupIntentProjector::Project() only
        //     ever compares distance against profile.RegroupRadius, a
        //     TRIGGER threshold, never against this execution-layer bound -
        //     by design, so the pure projector never needs to know an
        //     ActionSystem constant. This is deliberately the ONLY place
        //     that enforces reachability at all (2.12F2 P2 fix, round 3,
        //     STATIC review): AIWorld.WolfGroupFormationRadius/
        //     AIWorld.WolfGroupLeaveRadius are NOT clamped against this
        //     bound - Formation/Maintenance is its own capability with its
        //     own enable flag, independent of Coordination's, and must not
        //     have its own policy silently narrowed by a completely
        //     different, possibly-disabled capability's own execution
        //     limit (see AIWorld.WolfGroupLeaveRadius's own Initialize()
        //     comment). AIWorld.WolfGroupRegroupRadius alone is still
        //     clamped at Initialize(), since that IS a Coordination-layer
        //     trigger threshold. So a member can legitimately remain a
        //     group member, or even trigger a Regroup intent, from farther
        //     away than this dispatcher can actually reach - that gap is
        //     expected, not a misconfiguration, and this check turns it
        //     into a clean, named "nothing to do" rather than rebuilding
        //     and re-rejecting an identical ActionRequest as
        //     DestinationTooFar every pass, forever.
        // On every rejection above, this simply returns - no log spam for
        // what is an expected, frequent outcome (most members most passes
        // are not eligible), the same restraint RunCoalitionMaintenanceForGroup()
        // already shows for its own None decision.
        //
        // Sets record->GroupCoordinationGoalState BEFORE calling
        // ActionSystem::Validate() (the same order UpdateNeeds()'s own
        // RoutineGoal MOVE_TO dispatch already uses, so Validate()'s
        // ActiveGoalType/ActiveGoalStartedAtMs context can honestly name
        // this attempt), and clears it again immediately if Validate()
        // rejects or ExecuteMoveTo() fails to actually start - a
        // GroupCoordinationGoalState must never survive naming an attempt
        // that never actually became a running ActiveActionState (see
        // GroupCoordinationGoal.h for why this differs from RoutineGoalState,
        // which is stateless and needs no such rollback).
        void DispatchGroupMemberActionProposal(GroupMemberActionProposal const& proposal);

        // Milestone 2.12F2 P2 fix, round 4 (STATIC review): how many
        // currently-registered groups member belongs to whose own resolved
        // profile has RegroupEnabled - the one shared definition of
        // "coordination-ambiguous membership" both RunCoalitionCoordination()'s
        // own overlap arbitration and ReconcileGroupCoordinationForMember()
        // now use, via AgentGroupRegistry::GetGroupsOfMember() (O(k) where k
        // is however many groups member is actually in, almost always 0 or
        // 1 - see that method's own comment).
        uint32 CountRegroupEnabledMemberships(AgentId member) const;

        // Milestone 2.12F2 P2 fix, round 4 (STATIC review): the shared
        // mechanics both StopGroupCoordinationForMember() and
        // ReconcileGroupCoordinationForMember() need - captures
        // record.GroupCoordinationGoalState's own SourceGroup (2.12F2 P3
        // fix, round 3, STATIC review - folding both former callers' own
        // separate bodies into this one shared helper had silently dropped
        // this GroupId from the stop log; captured here, before the reset
        // below, since there is nowhere left to read it from afterward),
        // then unconditionally clears record.GroupCoordinationGoalState.
        // Only if record.ActiveActionState is actually the matching
        // in-flight Regroup (never assumed - see this method's own body
        // comment for why) does it go on to stop the underlying engine
        // movement (if a live Creature still resolves), logging that
        // captured SourceGroup alongside it, and clear ActiveActionState
        // too. reason is a literal describing WHY this particular caller
        // stopped it, logged the same way - callers never share one
        // generic reason string.
        void StopInFlightGroupCoordination(AgentRecord& record, char const* reason);

        // Milestone 2.12F2 P2 fix (STATIC review): the one place an
        // in-flight Regroup attempt (AgentRecord::GroupCoordinationGoalState/
        // ActiveActionState) is stopped because its OWNING GROUP changed
        // underneath it, not because a higher-priority individual goal
        // preempted it (see UpdateNeeds()'s own COORDINATION_PREEMPTED_BY_GOAL
        // block for that other case). GroupCoordinationGoal.h's own class
        // comment previously documented this as a deliberate non-goal ("a
        // dissolve/leave that happens while this attempt is already moving
        // simply lets the movement run to its own natural conclusion") -
        // STATIC review correctly identified that as a real bug, not a
        // deliberate simplification: a confirmed Leave/Dissolve for
        // groupId must stop any member still actively moving toward that
        // now-former group's own territory, or the movement completes as a
        // stale group-owned action with no group behind it any more.
        //
        // A no-op if memberId has no GroupCoordinationGoalState at all, or
        // one whose SourceGroup names a different group (an agent can only
        // ever have one in flight at a time - see DispatchGroupMemberActionProposal()'s
        // own ActiveActionState check). Called from RequestDissolveGroup()'s
        // own confirmed-dissolve completion (for every FORMER member,
        // captured before the dissolve was ever submitted - see that
        // method's own comment for why membership cannot have changed in
        // the meantime) and from RequestLeaveGroupWithPolicy()'s own
        // confirmed-leave completion (for the one member who just left).
        void StopGroupCoordinationForMember(AgentId memberId, GroupId groupId);

        // Milestone 2.12F2 P2 fix, round 4 (STATIC review): the Join-side
        // counterpart to StopGroupCoordinationForMember() - a confirmed
        // Join, unlike a confirmed Leave/Dissolve, can newly CREATE
        // coordination ambiguity for memberId (a second RegroupEnabled
        // group now claims it - see CountRegroupEnabledMemberships()) while
        // a Regroup dispatched before this join, back when membership was
        // still unambiguous, may still be actively running. Without this,
        // RunCoalitionCoordination()'s own overlap-arbitration rule would
        // only ever apply to NEW dispatches - an already in-flight action
        // would keep running to its own natural conclusion purely because
        // it started before the join confirmed, an implicit "whoever got
        // there first keeps it" priority the arbitration rule exists
        // specifically to remove. Deliberately generic - this only ever
        // asks AgentGroupRegistry/CountRegroupEnabledMemberships() "is this
        // membership still unambiguous", never anything about WolfLoose or
        // any other specific profile - so a future second RegroupEnabled
        // profile needs no change here.
        //
        // A no-op if memberId has no GroupCoordinationGoalState at all (the
        // overwhelmingly common case - most joins never race an in-flight
        // Regroup) or if CountRegroupEnabledMemberships() still resolves to
        // at most 1 (the join did not actually create ambiguity - e.g. the
        // newly-joined group's own profile has RegroupEnabled false).
        // Called only from RequestJoinGroupWithPolicy()'s own confirmed-join
        // completion - deliberately not placed inside
        // AgentGroupLifecycleSystem itself, which has (and must keep) no
        // knowledge of AgentGroupCoordinationProfile/RegroupEnabled; this is
        // AIWorldMgr's own post-confirmation orchestration, the same layer
        // StopGroupCoordinationForMember() already lives at.
        void ReconcileGroupCoordinationForMember(AgentId memberId);

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

        // Milestone 2.12E4C2 P2 fix, round 3 (STATIC review): GroupId::Value
        // entries with a RunGroupProfileAdoption() write currently
        // in flight - the group coarse tick's own resource-drift loop
        // below (RunDecisionScheduler()) skips a group entirely (Update()/
        // SaveGroupState() both, schedule state left untouched so it stays
        // due and is retried the very next pass) while its GroupId is in
        // this set. Without this, AgentGroupPersistence::AdoptGroupProfileAsync()'s
        // own targeted "SET profile_id = ? WHERE group_id = ?" and
        // SaveGroupState()'s own whole-row "SET ..., profile_id = ?,
        // version = ? WHERE group_id = ? AND version < ?" (built from
        // whatever AgentGroupRecord::ProfileId the RAM copy still held
        // BEFORE the adoption's own completion updates it) can commit in
        // either order - both are individually valid UPDATEs the DB
        // reports as successful, but if SaveGroupState()'s commits second,
        // it silently overwrites the just-adopted profile_id back to its
        // old (pre-adoption) value, and the adoption's own already-logged
        // "PASSED" (RAM now says adopted) no longer matches what the DB
        // holds - exactly the persistence/provenance divergence the
        // confirmed-write fix (round 2) was meant to close, reintroduced
        // by a second, uncoordinated write path to the same column.
        // Inserted right before RunGroupProfileAdoption() submits its own
        // AdoptGroupProfileAsync() call, erased unconditionally at the top
        // of that call's own completion (success or failure) - the same
        // "always cleared first, regardless of outcome" shape
        // AgentGroupLifecycleSystem::_pendingGroupOperations already uses.
        std::unordered_set<uint64> _groupProfileAdoptionInFlight;

        // Milestone 2.12B/2.12D: the only simulation the group coarse tick
        // runs - called from RunDecisionScheduler()'s own group coarse-tick
        // loop, unconditionally for every due AgentGroup (2.12D P2 fix:
        // no longer gated on whether any member happens to be materialized
        // right now - see AgentGroupRecord.h). No config of its own beyond
        // _agentGroupSimulationRates.
        AgentGroupSimulationSystem _agentGroupSimulationSystem;
        AgentGroupSimulationRates _agentGroupSimulationRates;

        // Milestone 2.12E4R (generalized from 2.12E4A's
        // _wolfCoalitionFormationSystem): pure formation candidate-
        // selection system - see its own class comment for why this is a
        // separate question from AgentGroupPolicySystem's "is this
        // socially allowed?" (a formation proposal still has to pass
        // CanJoin() for every member, same as any other automatic join -
        // see RunCoalitionJoinStep()). Stateless and profile-agnostic,
        // called only from RunCoalitionFormation().
        CoalitionFormationSystem _coalitionFormationSystem;

        // Milestone 2.12E4R: AIWorldMgr's own one existing
        // CoalitionFormationProfile, built once at Initialize() from
        // AIWorld.WolfGroupCreatureEntry/FormationRadius and
        // _groupPolicyConfig.LooseMinMembers/LooseMaxMembers - see
        // CoalitionFormationProfile.h for why Min/MaxMembers are never
        // independently-tunable here. Passed to RunCoalitionFormation()
        // from Update()'s own AIWorld.WolfGroupFormationIntervalMs timer.
        // A second profile would be another member exactly like this one,
        // built the same way, with its own timer/call site - nothing
        // about CoalitionFormationSystem/RunCoalitionFormation() itself
        // would need to change.
        CoalitionFormationProfile _wolfLooseFormationProfile;

        // Milestone 2.12E4A: AIWorld.WolfGroupAutoFormation - off by
        // default, see RunCoalitionFormation()'s own comment.
        bool _wolfGroupAutoFormation = false;

        // Milestone 2.12E4A: AIWorld.WolfGroupCreatureEntry - the one
        // creature_template entry _wolfLooseFormationProfile is built
        // with. A single fixed entry, not a list/category - deliberately
        // narrow for this milestone's own vertical slice, the same "prove
        // it for one concrete case first" scoping the manual group
        // lifecycle/policy smoke tests already followed.
        uint32 _wolfGroupCreatureEntry = 1423;

        // Milestone 2.12E4A: AIWorld.WolfGroupFormationIntervalMs -
        // Update()'s own cadence for RunCoalitionFormation(_wolfLooseFormationProfile),
        // deliberately its own timer rather than piggybacking on any
        // existing one (a formation scan is neither a decision nor a
        // coarse simulation tick).
        uint32 _wolfGroupFormationIntervalMs = 5000;
        uint32 _wolfGroupFormationTimer = 0;

        // Milestone 2.12E4A: AIWorld.WolfGroupFormationRadius -
        // _wolfLooseFormationProfile's own FormationRadius.
        float _wolfGroupFormationRadius = 30.0f;

        // Milestone 2.12E4R (STATIC review, generalized from 2.12E4B's
        // bool _wolfFormationInFlight): the CoalitionFormationProfileIds
        // with a formation attempt currently in flight - a per-PROFILE
        // bound now, not a single global boolean, so a future second
        // profile (e.g. a bandit or caravan formation) never has its own
        // formation blocked by an unrelated one already in progress for
        // WolfLoose. Still at most one in-flight attempt PER profile,
        // exactly the same guarantee 2.12E4B's own roadmap message asked
        // for: without it, two RunCoalitionFormation() passes for the
        // SAME profile landing before the first's CreateGroup/Join chain
        // even completes could both propose (and then create a group for)
        // the same still-ungrouped candidates, since neither pass's
        // candidate list reflects a join that has not landed in
        // _groupRegistry yet. profile.Id is inserted (and, when no
        // proposal exists, never inserted) at the top of
        // RunCoalitionFormation(); erased once RunCoalitionJoinStep()
        // reaches NextMemberIndex==Proposal.Members.size() (full success),
        // or once AbortCoalitionFormation()'s own best-effort dissolve
        // attempt completes (success or failure) - never synchronously
        // inside AbortCoalitionFormation() itself, so a new formation pass
        // for that same profile cannot start until any cleanup dissolve
        // for a failed attempt has also finished.
        //
        // Deliberately per-profile ONLY, not a cross-profile guarantee by
        // itself - see _formationReservedMembers below for the P2 STATIC
        // review fix that closes the gap this alone leaves for two
        // DIFFERENT profiles of the same AgentGroupKind, and
        // _coalitionFormationMaxInFlight for the P3 fix bounding how many
        // DIFFERENT profiles can have an entry here at once.
        std::unordered_set<CoalitionFormationProfileId> _formationInFlight;

        // Milestone 2.12E4R P2 fix (STATIC review): every (member, Kind)
        // reserved by a formation attempt currently building its
        // CreateGroup/Join chain - see CoalitionFormationReservationKey.h
        // for the exact cross-profile race this closes, and
        // RunCoalitionFormation()/RunCoalitionJoinStep()/
        // AbortCoalitionFormation()/ReleaseCoalitionFormationReservations()
        // for where entries are inserted and erased. With only one profile
        // (WolfLoose) existing today this can never actually collide with
        // itself - _formationInFlight above already prevents that - but it
        // is what makes a second, concurrently-running same-Kind profile
        // safe once one exists, without requiring _formationInFlight to
        // become a single cross-profile lock again.
        std::unordered_set<CoalitionFormationReservationKey, CoalitionFormationReservationKeyHash> _formationReservedMembers;

        // Milestone 2.12E4R P3 fix (STATIC review): AIWorld.CoalitionFormationMaxInFlight
        // - the hard ceiling on _formationInFlight.size() RunCoalitionFormation()
        // admits against, regardless of how many distinct profiles exist.
        // _formationInFlight alone bounds each profile to at most one
        // attempt, but places no bound on the number of DIFFERENT profiles
        // that can all be mid-formation at the same time - with only one
        // profile today that is moot, but a future profile registry
        // (WolfLoose, BanditLoose, Caravan, ...) would otherwise let the
        // number of concurrent automatic-formation async DB sagas grow
        // with the number of profiles, unbounded. Defaults to 1 - with
        // today's single profile, this reproduces the exact "at most one
        // automatic formation in flight, period" behavior 2.12E4B's own
        // bool _wolfFormationInFlight already gave.
        uint32 _coalitionFormationMaxInFlight = 1;

        // Milestone 2.12E4C1/C2: pure decision system - see its own class
        // comment. Stateless, called only from RunCoalitionMaintenance().
        CoalitionMaintenanceSystem _coalitionMaintenanceSystem;

        // Milestone 2.12E4C2: AIWorldMgr's own one existing
        // CoalitionMaintenanceProfile, paired with
        // _wolfLooseFormationProfile above (same ProfileId/Kind) - built
        // once at Initialize() from AIWorld.WolfGroupLeaveRadius and
        // _groupPolicyConfig.LooseMinMembers.
        CoalitionMaintenanceProfile _wolfLooseMaintenanceProfile;

        // Milestone 2.12E4C2 P2 fix (STATIC review): AIWorld.CoalitionMaintenance
        // - a SEPARATE enable gate from _wolfGroupAutoFormation, off by
        // default. An earlier version shared _wolfGroupAutoFormation's own
        // gate for both formation AND maintenance, on the argument that
        // "maintenance has nothing to maintain until formation has created
        // something" - wrong: a group can persist across a restart, or be
        // created manually/by an admin tool, entirely independent of
        // whether automatic formation is currently enabled. An operator
        // who sets AIWorld.WolfGroupAutoFormation = 0 to stop NEW groups
        // from forming must not also silently freeze every EXISTING
        // group's automatic leave/dissolve maintenance - creation and
        // maintenance are two different lifecycle capabilities. See
        // RunCoalitionMaintenance()'s own call site in Update().
        bool _coalitionMaintenanceEnabled = false;

        // Milestone 2.12E4C2: AIWorld.WolfGroupLeaveRadius -
        // _wolfLooseMaintenanceProfile's own LeaveRadius, clamped at
        // Initialize() to never fall below _wolfGroupFormationRadius (see
        // Initialize()'s own comment for why a smaller LeaveRadius would
        // defeat the formation/leave hysteresis entirely).
        float _wolfGroupLeaveRadius = 60.0f;

        // Milestone 2.12E4C2: RunCoalitionMaintenance()'s own cadence,
        // deliberately its own timer (a maintenance pass is neither a
        // formation pass nor a group coarse simulation tick). Milestone
        // 2.12E4C2 P2 fix (STATIC review): only advances while
        // _coalitionMaintenanceEnabled is true - a SEPARATE gate from
        // _wolfGroupAutoFormation, not shared with it, see
        // _coalitionMaintenanceEnabled's own declaration comment for why.
        uint32 _coalitionMaintenanceIntervalMs = 5000;
        uint32 _coalitionMaintenanceTimer = 0;

        // Milestone 2.12E4C2: RunCoalitionMaintenance()'s own bounded,
        // deterministic per-pass admission - GroupCoarseSimulationScheduler's
        // own dedicated instance/schedule map for the maintenance pass,
        // entirely separate from _groupCoarseSimulationScheduler/
        // _groupSimulationSchedule's own resource-drift tick (different
        // cadence, different candidate filter, different meaning of "due"
        // - sharing either the instance or the schedule map across both
        // concerns would let one pass's admission accounting bleed into
        // the other's). See GroupCoarseSimulationScheduler.h for why an
        // unbounded per-pass scan is rejected for the exact same reason
        // here as it already is for the group coarse tick.
        //
        // Milestone 2.12E4C2 P3 fix (STATIC review): erased on every
        // confirmed dissolve (see RequestDissolveGroup()'s own completion),
        // the same GroupId-keyed-bookkeeping-never-recycled reasoning
        // _groupSimulationSchedule's own erase there already documents -
        // GroupIds are never reused (see GroupId.h), so a schedule entry
        // left behind after its group is gone would grow this map without
        // bound across a long-running world's dynamic create/dissolve
        // history instead of tracking only currently-existing groups.
        GroupCoarseSimulationScheduler _maintenanceScheduler;
        std::unordered_map<uint64, SimulationScheduleState> _maintenanceSchedule;
        uint32 _coalitionMaintenanceMaxPerPass = 20;

        // Milestone 2.12E4C2 P3 fix (STATIC review): AIWorld.CoalitionMaintenanceScanMaxPerPass
        // - the bound on DISCOVERY (AgentGroupRegistry::GetGroupsAfterUntil()),
        // separate from _coalitionMaintenanceMaxPerPass's own bound on
        // actual per-group WORK (SelectDue() admission) - see
        // RunCoalitionMaintenance()'s own comment for why these are two
        // independently bounded stages, not one. Typically larger than
        // _coalitionMaintenanceMaxPerPass (discovery only filters/reads,
        // work is comparatively expensive), but nothing enforces that
        // relationship - a smaller scan bound just means admission rarely
        // reaches its own ceiling, which is harmless, not incorrect.
        uint32 _coalitionMaintenanceScanMaxPerPass = 100;

        // Milestone 2.12E4C2 P3 fix (STATIC review): RunCoalitionMaintenance()'s
        // own discovery cursor - the last GroupId a GetGroupsAfterUntil()
        // call returned, so the NEXT pass resumes immediately after it
        // rather than re-scanning from the beginning every time. GroupId{}
        // (0) both starts a fresh scan cycle and marks "the previous pass
        // reached the end of the CURRENT cycle's own high-water mark" -
        // every real GroupId is nonzero (see GroupId.h), so 0 is never
        // ambiguous with an actual group. A dissolve between passes never
        // permanently disrupts this: GetGroupsAfterUntil() resumes from
        // whatever GroupId value survives immediately after the cursor,
        // regardless of whether the cursor's own former group still
        // exists - see that method's own comment.
        //
        // Milestone 2.12E4C2 P2 fix, round 2 (STATIC review): deliberately
        // a SINGLE GLOBAL cursor, not one per CoalitionMaintenanceProfile -
        // RunCoalitionMaintenance() itself no longer takes a profile
        // parameter (discovery happens once, across every profile
        // together, then each discovered group's own profile is resolved
        // - see that method's own comment for the cross-profile
        // starvation a per-profile cursor would otherwise cause, and why
        // that would also multiply total discovery work by the number of
        // configured profiles).
        GroupId _maintenanceScanCursor;

        // Milestone 2.12E4C2 P2 fix, round 3 (STATIC review): the highest
        // GroupId (AgentGroupRegistry::GetHighestGroupId()) that existed
        // at the moment the CURRENT scan cycle began - RunCoalitionMaintenance()
        // never discovers a group with a higher GroupId than this within
        // the same cycle, even if one is created mid-cycle. Recomputed
        // fresh (from whatever the registry actually holds right then)
        // every time _maintenanceScanCursor is at GroupId{} - both the
        // very first call ever and every subsequent cycle start after a
        // wrap use the exact same "cursor is 0, so take a fresh snapshot"
        // path, no separate bootstrap needed. Without this, GroupIds being
        // monotonically increasing means a registry growing faster than
        // the scan can advance past it would always have a higher GroupId
        // waiting past the cursor - the scan would never reach empty,
        // never wrap, and the earliest-created groups would never be
        // revisited at all, not merely delayed. See
        // AgentGroupRegistry::GetGroupsAfterUntil()'s own comment for the
        // full reasoning.
        GroupId _maintenanceScanCycleHighWater;

        // Milestone 2.12E4C2: GroupId::Value entries with a maintenance
        // Leave/Dissolve chain currently in flight - inserted by
        // RunCoalitionMaintenanceForGroup() before its own
        // RequestLeaveGroupWithPolicy()/RequestDissolveGroupWithPolicy()
        // call, erased only once that chain's own terminal outcome is
        // fully resolved (RunCoalitionMaintenanceAfterLeave() when no
        // dissolve follows, or RunCoalitionMaintenanceDissolveGroup()'s own
        // completion otherwise - see each method's own comment). Exists so
        // this orchestrator never deliberately re-issues a request for a
        // group it already knows has one in flight - AgentGroupLifecycleSystem's
        // own per-GroupId pending-operation guard (see its header comment)
        // is a second, independent line of defense against the same class
        // of race, not a substitute for this one: that guard only ever
        // rejects an overlapping request AFTER it has already been built
        // and submitted, where this avoids building/logging/submitting a
        // redundant one at all - the concrete case this closes is a group
        // still being processed from a previous maintenance pass (a Leave
        // -> Dissolve chain spanning several ticks) getting re-selected
        // and re-evaluated by a later pass before the first chain has
        // finished.
        std::unordered_set<uint64> _maintenanceInFlight;

        // Milestone 2.12F1/2.12F2: pure decision systems - see their own
        // class comments. Stateless, called only from
        // RunCoalitionCoordination().
        AgentGroupIntentSystem _agentGroupIntentSystem;
        AgentGroupIntentProjector _agentGroupIntentProjector;

        // Milestone 2.12F2: AIWorldMgr's own one existing
        // AgentGroupCoordinationProfile, paired with
        // _wolfLooseFormationProfile/_wolfLooseMaintenanceProfile above
        // (same ProfileId/Kind) - built once at Initialize() from
        // AIWorld.WolfGroupRegroupEnabled/AIWorld.WolfGroupRegroupRadius.
        AgentGroupCoordinationProfile _wolfLooseCoordinationProfile;

        // Milestone 2.12F2: gated on its OWN AIWorld.GroupCoordination flag,
        // deliberately NOT _wolfGroupAutoFormation or
        // _coalitionMaintenanceEnabled - the same "each capability gets its
        // own gate" discipline _coalitionMaintenanceEnabled's own
        // declaration comment already established, applied to a third,
        // independent capability (an operator stopping automatic
        // maintenance must not also silently freeze coordination movement
        // of groups that already exist, and vice versa).
        bool _groupCoordinationEnabled = false;

        // Milestone 2.12F2: RunCoalitionCoordination()'s own cadence,
        // deliberately its own timer - a coordination pass is neither a
        // formation pass, a maintenance pass, nor a group coarse simulation
        // tick.
        uint32 _groupCoordinationIntervalMs = 5000;
        uint32 _groupCoordinationTimer = 0;

        // Milestone 2.12F2: the bound on RunCoalitionCoordination()'s own
        // DISCOVERY (AgentGroupRegistry::GetGroupsAfterUntil()) -
        // AIWorld.GroupCoordinationScanMaxPerPass. No separate admission
        // bound exists the way _coalitionMaintenanceMaxPerPass is separate
        // from _coalitionMaintenanceScanMaxPerPass - see
        // RunCoalitionCoordination()'s own comment for why a dispatched
        // MOVE_TO proposal needs no second bounded tier.
        uint32 _groupCoordinationScanMaxPerPass = 100;

        // Milestone 2.12F2: RunCoalitionCoordination()'s own discovery
        // cursor/scan-cycle high-water mark - the exact same two-field
        // pattern _maintenanceScanCursor/_maintenanceScanCycleHighWater
        // already established (see their own declaration comments for the
        // full reasoning), just a dedicated pair for this concern so
        // coordination discovery and maintenance discovery never share, and
        // therefore never race on, one cursor.
        GroupId _coordinationScanCursor;
        GroupId _coordinationScanCycleHighWater;

        // Milestone 2.12F3 test hook: AIWorld.TestDissolveOnActiveRegroupGroupId
        // - see CheckTestDissolveOnActiveRegroup()'s own comment. GroupId{}
        // (0) means disabled, reloaded fresh every Initialize() (parsed
        // fail-closed there against a negative config value - 2.12F3 P3
        // fix, STATIC review - before ever being stored here). Unlike
        // _coordinationScanCursor/_coordinationScanCycleHighWater above,
        // this is not scan state - it is simply which single group (if
        // any) this test hook is watching.
        GroupId _testDissolveOnActiveRegroupGroupId;

        // Milestone 2.12F3 test hook: doubles as this hook's own enabled/
        // disabled latch, not only "already fired successfully" - besides
        // being set once CheckTestDissolveOnActiveRegroup() has submitted
        // its one and only dissolve request, Initialize() itself also sets
        // this true up front (2.12F3 P3 fix, STATIC review) if
        // _testDissolveOnActiveRegroupGroupId does not resolve in
        // _groupRegistry right after LoadGroups()/LoadGroupMembers() - a
        // configured GroupId that will never exist this process's
        // lifetime must disable the hook outright, not leave it silently
        // polling forever once per coordination pass. CheckTestDissolveOnActiveRegroup()
        // itself sets this the same way (2.12F3 P3 fix, round 3, STATIC
        // review) if the group later stops resolving at all - a group can
        // legitimately dissolve through the normal lifecycle path before
        // this hook ever observes an active REGROUP, which the
        // Initialize()-time existence check above cannot catch. Either way, never
        // fires a second time for the rest of this process's lifetime
        // (even if the group somehow still resolves and still has a
        // member mid-REGROUP for some other reason afterward) - the same
        // at-most-once guarantee _formationInFlight/_maintenanceInFlight
        // give their own in-flight work, just for a single test group
        // rather than a set of them since only one groupId is ever
        // configured at a time. Reset to false every Initialize(), before
        // the existence check above can set it back to true.
        bool _testDissolveOnActiveRegroupFired = false;

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
