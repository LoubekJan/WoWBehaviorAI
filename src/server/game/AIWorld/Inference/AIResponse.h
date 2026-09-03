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

#ifndef AIWORLD_AIRESPONSE_H
#define AIWORLD_AIRESPONSE_H

#include "Agent/AgentId.h"
#include "AIRequest.h"
#include "DecisionProvenance.h"
#include "DecisionResponse.h"
#include "Define.h"
#include "DynamicTaskResponse.h"
#include "QuestRequestProvenance.h"
#include <optional>

// Delivered back to the world thread through AIClient::TryPopResponse().
// Success=false covers a network/HTTP failure, a timeout, and a non-2xx
// HTTP status alike; AIClient itself already logs which one happened at
// completion time. Agent/SnapshotSequence/Provenance are all the client's
// own echo of what was requested (used for the stale-response/provenance
// checks regardless of parse outcome), never anything ai-server sent
// back - Provenance specifically is set from the *request's* own
// AgentContext::Goal (see DecisionProvenance.h), not parsed from the
// response body. Decision is only populated for a Type == Decision
// response that both succeeded and parsed cleanly - see DecisionResponse
// for the actual decision content; it is never populated for Health.
struct AIResponse
{
    uint64 RequestId = 0;
    AIRequestType Type = AIRequestType::Health;
    AgentId Agent;
    uint64 SnapshotSequence = 0;
    DecisionProvenance Provenance;

    bool Success = false;
    uint32 StatusCode = 0;
    uint32 LatencyMs = 0;

    std::optional<DecisionResponse> Decision;

    // Milestone 2.13A3: the /dynamic-task counterpart to Provenance/
    // Decision above - only meaningful for Type == DynamicTask.
    // QuestProvenance is always the request's own echo (AIRequest::
    // QuestProvenance, copied through unconditionally, exactly like
    // Provenance above), so a caller can still see what request/target
    // bindings this response was supposed to answer even when
    // Success==false. DynamicTask itself is only populated once the HTTP
    // call succeeded, the body parsed, and the response envelope
    // (protocol_version/request_id/agent_id/snapshot_sequence) matched
    // what was actually sent - any mismatch there means Success=false and
    // DynamicTask stays empty, never a "best effort" partial fill.
    QuestRequestProvenance QuestProvenance;
    std::optional<DynamicTaskResponse> DynamicTask;
};

#endif // AIWORLD_AIRESPONSE_H
