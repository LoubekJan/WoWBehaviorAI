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
#include "DecisionCadenceClass.h"
#include "DecisionScheduleState.h"
#include "Define.h"
#include <unordered_map>
#include <vector>

// Milestone 2.10A/2.10B: deterministic admission ranking - which of the
// given candidate agents get a decision request built and submitted this
// pass, and which are deferred purely for lack of capacity. Pure value
// transform, the same rule every other AIWorld "System" class follows: no
// AgentRecord*, Creature*, Map*, registry, or DB. The caller (AIWorldMgr)
// is responsible for candidates already being registered, Materialized,
// and classified (a live-Creature GetPlayerListInGrid() proximity check,
// see RunDecisionScheduler()) - this only ever judges due-ness, the
// per-agent AwaitingResponse guard, priority between the two cadence
// classes, and the capacity bound. It never resolves proximity itself.
class TC_GAME_API DecisionScheduler
{
    public:
        // Milestone 2.10B: one due candidate, plus the cadence class
        // AIWorldMgr already determined for it this pass - SelectDue()
        // itself never computes or re-checks proximity, only ranks by
        // what it is told.
        struct Candidate
        {
            AgentId Agent;
            DecisionCadenceClass CadenceClass;
        };

        struct SelectionResult
        {
            // Milestone 2.10B: carries CadenceClass too (not just
            // AgentId), since the caller needs it to pick the right
            // NextDecisionAtMs interval - Nearby vs Active - once a
            // request actually gets submitted for this agent.
            std::vector<Candidate> Admitted;
            std::vector<AgentId> SkippedCapacity;
        };

        // Deterministic ordering, in this fixed priority: Nearby before
        // Active, then NextDecisionAtMs ascending, then AgentId ascending
        // tie-break - reproducible, no random starvation between agents of
        // the same class. An agent with AwaitingResponse already true, or
        // whose NextDecisionAtMs is still in the future, is not a
        // candidate for either Admitted or SkippedCapacity at all - it
        // simply is not due yet. Everything else due is Admitted up to
        // maxAdmit, in that priority order; the remainder (if any) is
        // SkippedCapacity, left with its NextDecisionAtMs untouched by
        // this call so it stays at the front of the due set next pass
        // instead of being pushed back behind agents that already got a
        // turn - the caller is the one that actually advances
        // NextDecisionAtMs, only for what it goes on to submit. Nearby
        // always outranking Active, regardless of how overdue an Active
        // agent already is, is a deliberate, documented limitation for
        // this milestone (see RunDecisionScheduler()) - sustained Nearby
        // pressure at or above capacity could starve Active agents; a
        // later milestone's job to add real aging/fairness if that proves
        // to matter in practice.
        SelectionResult SelectDue(std::unordered_map<uint64, DecisionScheduleState> const& state,
            std::vector<Candidate> const& candidates, uint64 nowMs, uint32 maxAdmit) const;
};

#endif // AIWORLD_DECISIONSCHEDULER_H
