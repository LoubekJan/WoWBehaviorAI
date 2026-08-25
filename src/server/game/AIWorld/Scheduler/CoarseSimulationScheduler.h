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
#include <unordered_map>
#include <vector>

// Milestone 2.10D P2 fixes: deterministic bounded admission for the coarse
// Background tick - the same "pure value transform, caller resolves every
// live fact" rule DecisionScheduler already follows for the
// decision-eligible tiers. Without a bound, every due Background agent
// would tick in the very same scheduler pass (thousands at once, at a
// shared 250ms poll cadence far finer than their own 60s interval).
//
// Milestone 2.12D: AgentType::AgentGroup (formerly the other tier this
// class bounded) no longer exists - a group is not an AgentRecord/
// AgentId-keyed candidate any more (see GroupId.h), and its own coarse
// tick (AIWorldMgr::RunDecisionScheduler()'s group loop) deliberately does
// not reuse this class at all, since the cardinality concern this class
// exists for (potentially thousands of individual agents) does not apply
// to groups (a handful of packs, not thousands of creatures).
//
// Unlike DecisionScheduler, this class never computes a due time itself -
// SimulationScheduleState::NextTickAtMs is already the authoritative due
// time by the time a candidate reaches here, set by the caller
// (AIWorldMgr::RunDecisionScheduler()) either as a one-time phase-offset
// on tier entry or as a plain "+interval" after each real tick (see
// SimulationScheduleState.h/StableAgentHash.h for why entry needs a phase
// offset and a live per-pass recompute would not preserve one). This class
// only ever ranks and bounds what it is handed.
class TC_GAME_API CoarseSimulationScheduler
{
    public:
        struct SelectionResult
        {
            std::vector<AgentId> Admitted;
            std::vector<AgentId> SkippedCapacity;
        };

        // Deterministic ordering: NextTickAtMs ascending (0 - "never set",
        // reachable only defensively, see SimulationScheduleState.h - is
        // always immediately due and always sorts first), then AgentId
        // ascending as the tie-break. Admitted up to maxAdmit, in that
        // order; the remainder (if any) is SkippedCapacity, left with its
        // NextTickAtMs untouched by this call so it stays at the front of
        // the due set next pass instead of being starved behind agents
        // that already ticked - the caller is the one that actually
        // updates LastTickAtMs/NextTickAtMs, only for what it goes on to
        // tick.
        SelectionResult SelectDue(std::unordered_map<uint64, SimulationScheduleState> const& state,
            std::vector<AgentId> const& candidates, uint64 nowMs, uint32 maxAdmit) const;
};

#endif // AIWORLD_COARSESIMULATIONSCHEDULER_H
