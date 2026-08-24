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
#include "Agent/AgentSnapshot.h"
#include "Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "IoContext.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "Memory/MemoryImportance.h"
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

    int32 configuredIntervalMs = sConfigMgr->GetIntDefault("AIWorld.SnapshotIntervalMs", 5000);
    if (configuredIntervalMs < 100)
    {
        TC_LOG_WARN("ai.world", "AIWorld.SnapshotIntervalMs ({}) is invalid or too low, clamping to 100ms", configuredIntervalMs);
        configuredIntervalMs = 100;
    }
    _snapshotIntervalMs = uint32(configuredIntervalMs);
    _snapshotTimer = 0;

    uint32 testMapId = uint32(sConfigMgr->GetIntDefault("AIWorld.TestMapId", 0));
    uint64 testSpawnId = uint64(sConfigMgr->GetIntDefault("AIWorld.TestSpawnId", 0));

    std::string aiHost = sConfigMgr->GetStringDefault("AIWorld.AIHost", "ai-server");
    std::string aiPort = std::to_string(sConfigMgr->GetIntDefault("AIWorld.AIPort", 8000));

    int32 requestTimeoutMs = sConfigMgr->GetIntDefault("AIWorld.RequestTimeoutMs", 1000);
    if (requestTimeoutMs < 100)
    {
        TC_LOG_WARN("ai.world", "AIWorld.RequestTimeoutMs ({}) is invalid or too low, clamping to 100ms", requestTimeoutMs);
        requestTimeoutMs = 100;
    }

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

    _aiClient = std::make_unique<AIClient>(ioContext, aiHost, aiPort, uint32(requestTimeoutMs));

    TC_LOG_INFO("ai.world", "AIWorld enabled");

    // Rebuild _registry from characters.ai_agents before touching anything
    // spawn-specific below - every agent this produces is Abstract with an
    // empty RuntimeGuid regardless of what it was before shutdown.
    _persistence.LoadAgents(_registry);

    // Same idea as _registry, for long-term memory: rebuilt from the DB
    // every startup. Must come after LoadAgents() - it needs _registry
    // populated to skip an orphaned row's agent_id.
    _memoryPersistence.LoadLongTermMemories(_longTermMemory, _registry);

    // Does not require the spawn's Creature/grid to be loaded - the agent
    // exists in _registry as soon as this returns, Abstract until
    // ProcessAgent() finds a live Creature for it.
    if (testSpawnId)
    {
        if (AgentRecord* existing = _registry.FindBySpawn(testMapId, testSpawnId))
        {
            _testAgentId = existing->Id;
        }
        else
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
                    _testAgentId = newId;
                    TC_LOG_INFO("ai.world", "AI persistent agent created id={} type={} map={} spawn={}",
                        newId.Value, ToString(AgentType::Guard), testMapId, testSpawnId);
                }
            }
        }
    }

    TC_LOG_INFO("ai.world", "AI bridge target {}:{} (timeout={}ms, health interval={}ms)", aiHost, aiPort, requestTimeoutMs, _healthIntervalMs);

    // Last step: only from here on can PublishWorldEvent() actually enqueue
    // anything. Ordered after everything above so a map worker can't race
    // a WorldEvent into _eventBus before _registry/_aiClient exist.
    _acceptEvents.store(true, std::memory_order_release);
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

    if (_testAgentId)
    {
        _snapshotTimer += diff;
        if (_snapshotTimer >= _snapshotIntervalMs)
        {
            _snapshotTimer = 0;
            ProcessAgent(_testAgentId);
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

        if (!response.Success)
            continue; // AIClient already logged the failure/timeout

        AgentRecord* record = _registry.Find(response.Agent);
        if (!record)
        {
            TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} is no longer registered, discarding",
                response.RequestId, response.Agent.Value);
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
            continue;
        }

        if (!response.Decision)
        {
            TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} succeeded but carries no decision payload, discarding",
                response.RequestId, response.Agent.Value, response.SnapshotSequence);
            continue;
        }

        // Milestone 2.9A: ai-server now answers over the versioned
        // DecisionRequest/DecisionResponse protocol with a full
        // AgentContext behind it, but action is still always NONE from a
        // deterministic stub, and this is still just a log - the response
        // is deliberately NOT converted into an ActionRequest or executed.
        // That stays a later milestone's job, once Action validation can
        // decide whether ai-server's proposed action is ALLOWED.
        TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} action={} accepted (no-op)",
            response.RequestId, response.Agent.Value, response.SnapshotSequence, response.Decision->Action);
    }
}

// Bridges a registered agent to whatever its Creature is doing right now,
// without ever holding onto that Creature past this call: materializes it
// (Abstract -> Materialized) the tick it's found loaded, dematerializes it
// (Materialized -> Abstract) the tick it stops being found - the agent
// itself, and its SnapshotSequence, live in _registry across either
// transition. Never forces a grid to load.
//
// BindCreature() is called unconditionally (not just out of Abstract): a
// SpawnId identifies a TrinityCore spawn, not a runtime object, so the old
// Creature can despawn and a new one for the same spawn appear between two
// polls without WorldState ever passing through Abstract in between.
// BindCreature() is idempotent and only actually changes anything when the
// runtime GUID doesn't already match.
void AIWorldMgr::ProcessAgent(AgentId id)
{
    AgentRecord* record = _registry.Find(id);
    if (!record)
        return;

    Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
    Creature* creature = map ? map->GetCreatureBySpawnId(record->SpawnId) : nullptr;

    if (!creature)
    {
        if (record->WorldState == AgentWorldState::Materialized)
            _registry.UnbindCreature(id);
        return;
    }

    _registry.BindCreature(id, *creature);

    CaptureAndSubmitSnapshot(id, *record, *creature);
}

void AIWorldMgr::CaptureAndSubmitSnapshot(AgentId id, AgentRecord& record, Creature& creature)
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
    context.RelevantMemories = relevantMemories;
    context.AvailableActions = { ActionType::Flee, ActionType::MoveTo, ActionType::Eat };

    AIRequest request;
    request.Decision.Context = std::move(context);
    _aiClient->SubmitDecision(std::move(request));
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

    return _registry.FindBySpawn(mapId, spawnId) != nullptr;
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
        if (AgentRecord* agent = _registry.FindBySpawn(event.Location.MapId, event.Actor.SpawnId))
            event.Actor.Agent = agent->Id;
    }

    if (event.Target.SpawnId)
    {
        if (AgentRecord* agent = _registry.FindBySpawn(event.Location.MapId, event.Target.SpawnId))
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
        // record->WorldState, which is only as fresh as the last
        // ProcessAgent() snapshot poll (up to AIWorld.SnapshotIntervalMs
        // old). Gating on WorldState here would produce false-negative
        // perception for however long a grid can be loaded before the
        // next poll catches up: exactly the gap that must not exist going
        // into Memory, where it would show up as random, snapshot-timer-
        // dependent holes rather than a real absence of perception.
        Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
        Creature* observer = map ? map->GetCreatureBySpawnId(record->SpawnId) : nullptr;

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
// ProcessWorldEvent()'s perception loop, for the same reason: this runs
// faster than _snapshotIntervalMs, so trusting WorldState here would
// reintroduce the exact false-negative gap that was just closed there.
void AIWorldMgr::ScanNearbyEntities()
{
    for (AgentId id : _registry.GetAgents())
    {
        AgentRecord* record = _registry.Find(id);
        if (!record)
            continue;

        Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
        Creature* observer = map ? map->GetCreatureBySpawnId(record->SpawnId) : nullptr;

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
                if (AgentRecord* seenAgent = _registry.FindBySpawn(observation->Location.MapId, observation->Target.SpawnId))
                    observation->Target.Agent = seenAgent->Id;
            }

            ProcessObservation(*observation);
        }
    }
}

// World thread only, on its own ~1s cadence (_needsUpdateIntervalMs),
// independent of _snapshotIntervalMs - Milestone 2.6A/2.6B1/2.6B2. Only
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
        Creature* creature = map ? map->GetCreatureBySpawnId(record->SpawnId) : nullptr;

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

        // Same deterministic retrieval pipeline CaptureAndSubmitSnapshot()
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

    AgentRecord* record = _registry.FindBySpawn(event.MapId, event.SpawnId);
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

    if (!record->ActiveGoalState
        || record->ActiveGoalState->Type != record->ActiveActionState->SourceGoal
        || record->ActiveGoalState->StartedAtMs != record->ActiveActionState->GoalStartedAtMs)
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
