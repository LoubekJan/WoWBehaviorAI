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

#ifndef AIWORLD_NEEDSSYSTEM_H
#define AIWORLD_NEEDSSYSTEM_H

#include "Define.h"
#include "Memory/RetrievedMemory.h"
#include "NeedsState.h"
#include <vector>

// Per-second drift rates. Deliberately not part of NeedsState itself - the
// rates are shared configuration, not per-agent state.
struct NeedsUpdateRates
{
    float HungerPerSecond = 0.0002f;
    float FatiguePerSecond = 0.0001f;
    float ResourcePressurePerSecond = 0.00005f;
};

// Milestone 2.6B1/2.6B2: the live-world facts NeedsSystem is allowed to
// see, as plain values - not a Creature*. AIWorldMgr builds Health/
// MaxHealth/Alive/InCombat from the same live Creature it already resolved
// for the Materialized/Abstract check, and MemorySafetyPressure from
// EvaluateMemorySafety() below - the pointer itself never crosses into
// NeedsSystem.
struct NeedsUpdateContext
{
    uint32 Health = 0;
    uint32 MaxHealth = 0;
    bool Alive = false;
    bool InCombat = false;

    float MemorySafetyPressure = 0.0f;
};

// Milestone 2.6A/2.6B1/2.6B2: a pure value transform, deliberately holding
// no AgentId, Creature*, Map*, or MemoryRecord - Update() only knows how to
// advance a NeedsState by an elapsed duration and a NeedsUpdateContext.
// HealthPressure/SafetyPressure are 2.6B1's live health-ratio / in-combat
// signals, with SafetyPressure falling back to recent-memory-driven danger
// (2.6B2, see EvaluateMemorySafety()) once combat itself has ended.
class TC_GAME_API NeedsSystem
{
    public:
        void Update(NeedsState& state, NeedsUpdateContext const& context, uint32 elapsedMs, NeedsUpdateRates const& rates) const;

        // Milestone 2.6B2: deterministic, memory-relevance-independent
        // danger pressure - not the same thing as RetrievedMemory::Relevance,
        // whose freshness deliberately decays over days for recall, not
        // over seconds for "how dangerous does this feel right now". Only
        // memories whose SourceEventType is inherently dangerous
        // (CreatureKilled/NPCInjured/LivestockKilled/WolfPackMoved/NPCDied)
        // contribute; everything else (PlayerSeen, TradeCompleted, ...) is
        // ignored here regardless of importance/relevance. A memory older
        // than recentWindowMs contributes nothing, so a long-persisted
        // long-term memory from days ago never keeps an agent permanently
        // "in danger". Pure value in, pure value out - no Creature*/Map*/
        // registry/DB.
        float EvaluateMemorySafety(std::vector<RetrievedMemory> const& memories, uint64 nowMs, uint32 recentWindowMs) const;
};

#endif // AIWORLD_NEEDSSYSTEM_H
