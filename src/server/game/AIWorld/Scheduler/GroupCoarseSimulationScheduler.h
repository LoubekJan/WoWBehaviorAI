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

#ifndef AIWORLD_GROUPCOARSESIMULATIONSCHEDULER_H
#define AIWORLD_GROUPCOARSESIMULATIONSCHEDULER_H

#include "Agent/GroupId.h"
#include "Define.h"
#include "SimulationScheduleState.h"
#include <unordered_map>
#include <vector>

// Milestone 2.12D P2 fix (STATIC review): deterministic bounded admission
// for the group coarse tick - GroupId's own sibling to
// CoarseSimulationScheduler (AgentId-keyed, for Background agents), not a
// reuse of it. An earlier version of this same milestone ticked every due
// AgentGroup unconditionally, on the argument that group cardinality would
// stay small ("a handful of packs") - rejected by review: this codebase is
// moving toward dynamic LOOSE coalitions (packs that form and dissolve
// opportunistically), so group cardinality is not fixed the way a small
// set of scripted/STABLE groups would be, and a world with hundreds or
// thousands of transient coalitions ticking unconditionally every ~250ms
// scheduler pass is exactly the same class of problem
// CoarseSimulationScheduler was introduced in 2.10D to prevent for
// individual agents - both an unbounded per-pass work spike and every
// simultaneously-due group permanently phase-locking onto the same pass
// once they share a LastTickAtMs.
//
// Same shape as CoarseSimulationScheduler otherwise - never computes a due
// time itself, only ranks and bounds what the caller
// (AIWorldMgr::RunDecisionScheduler()) already resolved into
// SimulationScheduleState (a plain uint64-pair struct, reused unmodified
// here - see its own comment).
class TC_GAME_API GroupCoarseSimulationScheduler
{
    public:
        struct SelectionResult
        {
            std::vector<GroupId> Admitted;
            std::vector<GroupId> SkippedCapacity;
        };

        // Deterministic ordering: NextTickAtMs ascending (0 - "never set" -
        // is always immediately due and always sorts first), then GroupId
        // ascending as the tie-break. Admitted up to maxAdmit, in that
        // order; the remainder (if any) is SkippedCapacity, left with its
        // NextTickAtMs untouched by this call so it stays at the front of
        // the due set next pass instead of being starved behind groups
        // that already ticked - the caller is the one that actually
        // updates LastTickAtMs/NextTickAtMs, only for what it goes on to
        // tick.
        SelectionResult SelectDue(std::unordered_map<uint64, SimulationScheduleState> const& state,
            std::vector<GroupId> const& candidates, uint64 nowMs, uint32 maxAdmit) const;
};

#endif // AIWORLD_GROUPCOARSESIMULATIONSCHEDULER_H
