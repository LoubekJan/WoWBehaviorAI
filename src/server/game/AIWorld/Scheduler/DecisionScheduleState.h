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

// Milestone 2.10A: per-agent, world-thread-only scheduling bookkeeping -
// deliberately not part of AgentRecord/AgentRegistry (see AIWorldMgr's own
// _decisionSchedule map keyed by AgentId) - this is admission/scheduling
// state, not simulation state, and AgentRegistry stays untouched.
//
// NextDecisionAtMs only ever advances when this agent is actually admitted
// and submitted (see AIWorldMgr::RunDecisionScheduler()) - an agent skipped
// for capacity keeps its old NextDecisionAtMs, so it naturally stays at the
// front of the next pass's deterministic ordering (NextDecisionAtMs
// ascending, AgentId ascending - see DecisionScheduler::SelectDue())
// instead of being pushed back behind agents that already got a turn. A
// freshly-seen agent (no entry yet) defaults to due immediately.
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
    uint64 NextDecisionAtMs = 0;
    bool AwaitingResponse = false;
};

#endif // AIWORLD_DECISIONSCHEDULESTATE_H
