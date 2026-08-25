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

#ifndef AIWORLD_COARSESIMULATIONSCHEDULER_H
#define AIWORLD_COARSESIMULATIONSCHEDULER_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "SimulationScheduleState.h"
#include "SimulationTier.h"
#include <unordered_map>
#include <vector>

// Milestone 2.10D P2 fix: deterministic bounded admission for the coarse
// Background/Abstract tick - the same "pure value transform, caller
// resolves every live fact" rule DecisionScheduler already follows for the
// decision-eligible tiers. Without a bound, every due Background/Abstract
// agent would tick in the very same scheduler pass (thousands at once, at
// a shared 250ms poll cadence far finer than their own 60s/5min interval),
// and since they would all then share the exact same LastTickAtMs, they
// would stay permanently phase-locked afterwards too - ticking together
// forever instead of spreading out, exactly the "one global tick for every
// entity" outcome the whole scheduler/tier foundation exists to avoid. A
// bounded per-pass admission fixes both problems at once: only maxAdmit
// agents tick per pass, and draining an initial simultaneous backlog in
// deterministic AgentId order naturally staggers their LastTickAtMs across
// the drain window - which then persists as long-term desynchronization
// once the backlog clears, with no separate artificial stagger mechanism
// needed.
class TC_GAME_API CoarseSimulationScheduler
{
    public:
        // Milestone 2.10D: one due candidate, plus the SimulationTier
        // AIWorldMgr already determined for it this pass (Background or
        // Abstract - RunDecisionScheduler() never builds one of these for
        // a Materialized agent) - SelectDue() itself never computes or
        // re-checks tier, only ranks by what it is told.
        struct Candidate
        {
            AgentId Agent;
            SimulationTier Tier;
        };

        struct SelectionResult
        {
            std::vector<AgentId> Admitted;
            std::vector<AgentId> SkippedCapacity;
        };

        // Deterministic ordering: effective due time ascending - computed
        // fresh every call as LastTickAtMs + interval(candidate's current
        // Tier), using backgroundIntervalMs/abstractIntervalMs (mirroring
        // AIWorld.BackgroundSimulationIntervalMs/AbstractSimulationIntervalMs) -
        // then AgentId ascending as the tie-break. LastTickAtMs == 0
        // ("never ticked, or just entered this tier - see AIWorldMgr::
        // UpdateSimulationTier()'s epoch reset") is always immediately
        // due. Admitted up to maxAdmit, in that order; the remainder (if
        // any) is SkippedCapacity, left with its LastTickAtMs untouched by
        // this call so it stays at the front of the due set next pass
        // instead of being starved behind agents that already ticked - the
        // caller is the one that actually updates LastTickAtMs, only for
        // what it goes on to tick.
        SelectionResult SelectDue(std::unordered_map<uint64, SimulationScheduleState> const& state,
            std::vector<Candidate> const& candidates, uint64 nowMs, uint32 maxAdmit,
            uint32 backgroundIntervalMs, uint32 abstractIntervalMs) const;
};

#endif // AIWORLD_COARSESIMULATIONSCHEDULER_H
