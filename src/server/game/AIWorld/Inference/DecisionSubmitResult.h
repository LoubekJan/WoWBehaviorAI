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

#ifndef AIWORLD_DECISIONSUBMITRESULT_H
#define AIWORLD_DECISIONSUBMITRESULT_H

#include "Agent/AgentId.h"
#include "Define.h"

// Milestone 2.9E/2.10A: whether AIClient::SubmitDecisions() actually handed
// one request off to the async transport, or skipped it because
// AIClient's own bounded DecisionsInFlight counter (Milestone 2.10A - see
// AIWorld.DecisionMaxInFlight) was already at capacity. A later request in
// the same batch can legitimately come back SkippedInFlight once the cap
// is reached, even for a perfectly well-formed request - see AIClient::
// SubmitDecisions(). This is a transport-level outcome only: a request the
// caller's own scheduler declines to even attempt (see DecisionScheduler::
// SelectionResult::SkippedCapacity) never reaches here at all - it is a
// scheduler-level decision, not a DecisionSubmitStatus value.
enum class DecisionSubmitStatus : uint8
{
    Submitted,
    SkippedInFlight
};

// Milestone 2.9E: per-agent outcome of one AIClient::SubmitDecisions()
// call. Deliberately carries AgentId rather than leaving correlation to
// vector position - Milestone 2.10A's bounded/per-agent admission policy
// can submit any subset of a batch, not necessarily a prefix, and a
// caller must still be able to tell input A/B/C/D apart from output
// A/C unambiguously. RequestId == 0 means "not submitted", the same
// convention AIClient::SubmitDecision()'s own return value already uses.
struct DecisionSubmitResult
{
    AgentId Agent;
    uint64 RequestId = 0;
    DecisionSubmitStatus Status = DecisionSubmitStatus::SkippedInFlight;
};

#endif // AIWORLD_DECISIONSUBMITRESULT_H
