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
// see RunDecisionScheduler()) - this only ever judges due-ness (recomputed
// fresh every call from DecisionScheduleState::LastDecisionSubmittedAtMs
// and each candidate's current CadenceClass - see SelectDue()'s own
// comment), the per-agent AwaitingResponse guard, priority among due
// candidates, and the capacity bound. It never resolves proximity itself.
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
            std::vector<AgentId> Admitted;
            std::vector<AgentId> SkippedCapacity;
        };

        // Milestone 2.10B P2 fix: an agent's due-ness is never read
        // straight off a stored deadline - it is recomputed every call as
        // effectiveDueAtMs = LastDecisionSubmittedAtMs +
        // interval(candidate's CadenceClass this pass), using
        // nearbyIntervalMs/activeIntervalMs (mirroring AIWorld.
        // DecisionNearbyIntervalMs/DecisionActiveIntervalMs). A class
        // change therefore taps in within one scheduler poll in either
        // direction: ACTIVE -> NEARBY pulls the deadline earlier, NEARBY
        // -> ACTIVE pushes it later - never locked in at the class that
        // happened to be true at the last submission. LastDecisionSubmittedAtMs
        // == 0 ("never submitted/attempted") is always immediately due,
        // regardless of class.
        //
        // Deterministic ordering, in this fixed priority: effectiveDueAtMs
        // ascending first (an agent that has been overdue longer always
        // outranks one that only just became due, regardless of class -
        // this is what keeps an ACTIVE agent from starving under
        // sustained NEARBY contention: its own effectiveDueAtMs stays
        // fixed while it is skipped, so it only gets relatively more
        // overdue every pass it loses, while a repeatedly-admitted NEARBY
        // agent's effectiveDueAtMs keeps refreshing to "just now" - the
        // ACTIVE agent's priority strictly increases over time until it
        // wins a slot), then CadenceClass (Nearby before Active) only as
        // a tie-break between two candidates equally overdue, then
        // AgentId ascending as the final tie-break - reproducible, no
        // random starvation.
        //
        // An agent with AwaitingResponse already true, or not yet due, is
        // not a candidate for either Admitted or SkippedCapacity at all.
        // Everything else due is Admitted up to maxAdmit, in priority
        // order; the remainder (if any) is SkippedCapacity. Neither list
        // updates DecisionScheduleState itself - the caller is the one
        // that stamps LastDecisionSubmittedAtMs, only for what it actually
        // goes on to attempt/submit (see RunDecisionScheduler()).
        SelectionResult SelectDue(std::unordered_map<uint64, DecisionScheduleState> const& state,
            std::vector<Candidate> const& candidates, uint64 nowMs, uint32 maxAdmit,
            uint32 nearbyIntervalMs, uint32 activeIntervalMs) const;
};

#endif // AIWORLD_DECISIONSCHEDULER_H
