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
#include "Agent/AgentSnapshot.h"
#include "Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "IoContext.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "Memory/MemoryImportance.h"
#include "Player.h"
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

        // Milestone 2C: still just a stub - action is always NONE and
        // nothing is applied to the game world yet.
        TC_LOG_DEBUG("ai.world", "AI decision id={} agent={} snapshot={} action={} accepted (no-op)",
            response.RequestId, response.Agent.Value, response.SnapshotSequence, response.Action);
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

    // Milestone 2.5C: deterministic retrieval over this agent's memories,
    // scoped to "now" and the snapshot's own position/map. Logged only -
    // nothing consumes _relevant yet, AIRequest below is unchanged.
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

    AIRequest request;
    request.Agent = id;
    request.SnapshotSequence = snapshot.SnapshotSequence;
    request.SpawnId = snapshot.SpawnId;
    request.Entry = snapshot.Entry;
    request.Health = snapshot.Health;
    request.MaxHealth = snapshot.MaxHealth;
    request.Alive = snapshot.Alive;
    request.InCombat = snapshot.InCombat;
    request.MapId = snapshot.MapId;
    request.X = snapshot.X;
    request.Y = snapshot.Y;
    request.Z = snapshot.Z;
    _aiClient->SubmitDecision(request);
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
// independent of _snapshotIntervalMs - Milestone 2.6A/2.6B1. Only
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
    for (AgentId id : _registry.GetAgents())
    {
        AgentRecord* record = _registry.Find(id);
        if (!record)
            continue;

        Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
        Creature* creature = map ? map->GetCreatureBySpawnId(record->SpawnId) : nullptr;

        if (!creature)
        {
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

        _needsSystem.Update(record->Needs, context, elapsedMs, _needsRates);

        TC_LOG_DEBUG("ai.world",
            "AI needs agent={} dt={}ms alive={} inCombat={} healthPressure={:.4f} hunger={:.4f} fatigue={:.4f} safetyPressure={:.4f} resourcePressure={:.4f}",
            record->Id.Value, elapsedMs, context.Alive, context.InCombat,
            record->Needs.HealthPressure, record->Needs.Hunger, record->Needs.Fatigue,
            record->Needs.SafetyPressure, record->Needs.ResourcePressure);
    }
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
