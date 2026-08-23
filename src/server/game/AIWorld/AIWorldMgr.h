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
#include "Agent/AgentId.h"
#include "Agent/AgentRegistry.h"
#include "Define.h"
#include "Event/EventBus.h"
#include "Event/WorldEvent.h"
#include "Goal/FoodTargetResolver.h"
#include "Goal/GoalSystem.h"
#include "Inference/AIClient.h"
#include "Memory/LongTermMemory.h"
#include "Memory/MemoryRetrieval.h"
#include "Memory/ShortTermMemory.h"
#include "Needs/NeedsSystem.h"
#include "Perception/PerceptionSystem.h"
#include "Persistence/AgentPersistence.h"
#include "Persistence/MemoryPersistence.h"
#include <atomic>
#include <memory>

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

        void ProcessAgent(AgentId id);
        void CaptureAndSubmitSnapshot(AgentId id, AgentRecord& record, Creature& creature);
        void ProcessWorldEvent(WorldEvent& event);
        void ProcessObservation(Observation const& observation);
        void ScanNearbyEntities();
        void UpdateNeeds(uint32 elapsedMs);
        void ProcessActionEngineEvent(ActionEngineEvent const& event);
        void HandleActionCompletion(AgentRecord& record, ActionCompletion const& completion);
        void TryEat(AgentRecord& record, Creature& creature, PendingEatContinuation const& pending, uint64 nowMs);

        bool _enabled = false;

        uint32 _snapshotIntervalMs = 5000;
        uint32 _snapshotTimer = 0;

        // Registry of persistent agents - survives its Creature being
        // unloaded/reloaded; only ProcessAgent()'s Bind/UnbindCreature calls
        // change an agent's WorldState. Purely in-memory; rebuilt from
        // _persistence every startup.
        AgentRegistry _registry;

        // characters-DB authority for AgentId (Milestone 2.2A): _registry
        // never mints an id itself, only ever receives one already assigned
        // by this. Used exclusively during Initialize(), never per-tick.
        AgentPersistence _persistence;

        // Milestone 2.1's single test agent: config identifies the spawn to
        // load-or-create at Initialize() time, everything after that goes
        // through _registry via _testAgentId instead of touching map/spawn
        // directly.
        AgentId _testAgentId;

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
        // (faster, ~1s) cadence rather than piggybacking on
        // _snapshotIntervalMs - like ProcessWorldEvent()'s perception
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

        // Milestone 2.5C: deterministic relevance scoring over
        // _shortTermMemory/_longTermMemory, run once per CaptureAndSubmitSnapshot()
        // right after the AgentSnapshot is built. Logged only for now - nothing
        // consumes the result yet, AIRequest is untouched.
        MemoryRetrieval _memoryRetrieval;
        uint32 _memoryRetrievalTopN = 5;

        // Milestone 2.6A: deterministic per-agent NeedsState drift, run only
        // for Materialized agents (see UpdateNeeds()) on its own ~1s cadence,
        // independent of _snapshotIntervalMs - Needs is a simulation
        // subsystem, not part of the snapshot/decision cadence. Pure value
        // transform: NeedsSystem itself never touches AgentId/Creature/Map.
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
