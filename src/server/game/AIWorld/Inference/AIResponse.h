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
#include "DecisionResponse.h"
#include "Define.h"
#include <optional>

// Delivered back to the world thread through AIClient::TryPopResponse().
// Success=false covers a network/HTTP failure, a timeout, and a non-2xx
// HTTP status alike; AIClient itself already logs which one happened at
// completion time. Agent/SnapshotSequence are the client's own echo of
// what was requested (used for the stale-response check regardless of
// parse outcome), not anything ai-server sent back. Decision is only
// populated for a Type == Decision response that both succeeded and
// parsed cleanly - see DecisionResponse for the actual decision content;
// it is never populated for Health.
struct AIResponse
{
    uint64 RequestId = 0;
    AIRequestType Type = AIRequestType::Health;
    AgentId Agent;
    uint64 SnapshotSequence = 0;

    bool Success = false;
    uint32 StatusCode = 0;
    uint32 LatencyMs = 0;

    std::optional<DecisionResponse> Decision;
};

#endif // AIWORLD_AIRESPONSE_H
