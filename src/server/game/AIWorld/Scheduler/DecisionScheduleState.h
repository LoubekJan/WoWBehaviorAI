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

#ifndef AIWORLD_DECISIONSCHEDULESTATE_H
#define AIWORLD_DECISIONSCHEDULESTATE_H

#include "Define.h"

// Milestone 2.10A/2.10B P2 fix: per-agent, world-thread-only scheduling
// bookkeeping - deliberately not part of AgentRecord/AgentRegistry (see
// AIWorldMgr's own _decisionSchedule map keyed by AgentId) - this is
// admission/scheduling state, not simulation state, and AgentRegistry
// stays untouched.
//
// LastDecisionSubmittedAtMs replaces 2.10A/B's original NextDecisionAtMs:
// storing a locked-in absolute deadline meant it was computed once, from
// whatever DecisionCadenceClass happened to be true at the moment of that
// submission, and stayed wrong until the next submission - an agent that
// went ACTIVE -> NEARBY right after being admitted would still wait out
// its old, much longer ACTIVE-cadence deadline. Storing only the
// timestamp instead lets DecisionScheduler::SelectDue() recompute
// "effective due" fresh every pass as LastDecisionSubmittedAtMs +
// interval(this pass's CadenceClass) - so a class change taps in within
// one scheduler poll in either direction, faster (ACTIVE -> NEARBY) or
// slower (NEARBY -> ACTIVE). 0 means "never submitted (or never
// attempted)" - always immediately due, regardless of class. Stamped on
// every admitted attempt, not just a successful AIClient submission (see
// RunDecisionScheduler()) - a locally-failed resolution attempt (the
// agent turned out not to be materialized after all) still needs to move
// this forward, or the agent would be re-attempted again the very next
// (much shorter, 2.10B) scheduler pass instead of waiting a normal
// interval.
//
// AwaitingResponse is the per-agent duplicate guard: never true for more
// than one concurrently in-flight decision request for the same agent at
// once. Set only when AIClient::SubmitDecisions() actually reports
// DecisionSubmitStatus::Submitted for this agent; cleared only when
// AIWorldMgr::Update() drains a Decision-type AIResponse naming this
// agent - success, failure, or discard alike, since AIClient guarantees
// exactly one terminal response per submitted request (RequestTimeoutMs
// bounds how long that can take).
struct DecisionScheduleState
{
    uint64 LastDecisionSubmittedAtMs = 0;
    bool AwaitingResponse = false;
};

#endif // AIWORLD_DECISIONSCHEDULESTATE_H
