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

#ifndef AIWORLD_DECISIONRESPONSE_H
#define AIWORLD_DECISIONRESPONSE_H

#include "Agent/AgentId.h"
#include "DecisionIntent.h"
#include "Define.h"
#include "ProtocolVersion.h"

// Milestone 2.9A/2.9B: the versioned /decision response body, parsed
// straight off ai-server's JSON - separate from AIResponse's
// transport-level Success/StatusCode/LatencyMs, which apply identically to
// Health and describe network/HTTP outcome rather than decision content.
// Agent/SnapshotSequence/Version here are ai-server's own echo, already
// checked against the request by AIClient's DecisionSession before this is
// ever constructed (see AIClient.cpp's "protocol mismatch" check) - by the
// time a caller sees one, it is known to answer the exact request that was
// sent. Milestone 2.9B replaced the original opaque Action string with a
// structured DecisionIntent - see DecisionIntent.h for why it still only
// carries a Type, and AIWorldMgr::Update() for why this is still only
// logged, never turned into an ActionRequest or executed.
struct DecisionResponse
{
    ProtocolVersion Version = CurrentProtocolVersion;
    uint64 RequestId = 0;
    AgentId Agent;
    uint64 SnapshotSequence = 0;
    DecisionIntent Intent;
};

#endif // AIWORLD_DECISIONRESPONSE_H
