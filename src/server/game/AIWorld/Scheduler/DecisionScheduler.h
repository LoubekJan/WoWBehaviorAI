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

#ifndef AIWORLD_DECISIONSCHEDULER_H
#define AIWORLD_DECISIONSCHEDULER_H

#include "Agent/AgentId.h"
#include "DecisionScheduleState.h"
#include "Define.h"
#include <unordered_map>
#include <vector>

// Milestone 2.10A: deterministic admission ranking - which of the given
// candidate agents get a decision request built and submitted this pass,
// and which are deferred purely for lack of capacity. Pure value
// transform, the same rule every other AIWorld "System" class follows: no
// AgentRecord*, Creature*, Map*, registry, or DB. The caller (AIWorldMgr)
// is responsible for candidates already being registered and Materialized
// (a cheap flag check against AgentRegistry) - this only ever judges
// due-ness, the per-agent AwaitingResponse guard, and the capacity bound.
class TC_GAME_API DecisionScheduler
{
    public:
        struct SelectionResult
        {
            std::vector<AgentId> Admitted;
            std::vector<AgentId> SkippedCapacity;
        };

        // Deterministic ordering: NextDecisionAtMs ascending, AgentId
        // ascending tie-break - reproducible, no random starvation. An
        // agent with AwaitingResponse already true, or whose
        // NextDecisionAtMs is still in the future, is not a candidate for
        // either Admitted or SkippedCapacity at all - it simply is not due
        // yet. Everything else due is Admitted up to maxAdmit, in order;
        // the remainder (if any) is SkippedCapacity, left with its
        // NextDecisionAtMs untouched by this call so it stays at the front
        // next pass instead of being starved behind agents that already
        // got a turn - the caller is the one that actually advances
        // NextDecisionAtMs, only for what it goes on to submit.
        SelectionResult SelectDue(std::unordered_map<uint64, DecisionScheduleState> const& state,
            std::vector<AgentId> const& candidates, uint64 nowMs, uint32 maxAdmit) const;
};

#endif // AIWORLD_DECISIONSCHEDULER_H
