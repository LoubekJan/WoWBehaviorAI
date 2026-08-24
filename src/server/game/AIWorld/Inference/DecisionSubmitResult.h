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

// Milestone 2.9E: whether AIClient::SubmitDecisions() actually handed one
// request off to the async transport, or skipped it. Today's single
// global DecisionInFlight guard (unchanged in 2.9E) means a later request
// in the same batch can legitimately come back SkippedInFlight even though
// nothing about it individually was wrong - see AIClient::SubmitDecisions().
enum class DecisionSubmitStatus : uint8
{
    Submitted,
    SkippedInFlight
};

// Milestone 2.9E: per-agent outcome of one AIClient::SubmitDecisions()
// call. Deliberately carries AgentId rather than leaving correlation to
// vector position - a future bounded/per-agent admission policy (2.10)
// could submit any subset of a batch, not necessarily a prefix, and a
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
