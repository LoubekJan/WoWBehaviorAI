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

#ifndef AIWORLD_DECISIONREQUEST_H
#define AIWORLD_DECISIONREQUEST_H

#include "AgentContext.h"
#include "Define.h"
#include "ProtocolVersion.h"

// Milestone 2.9A: the versioned /decision request body - a ProtocolVersion
// plus one agent's full AgentContext, and nothing else. Self-contained on
// purpose (RequestId mirrors the owning AIRequest's RequestId, both
// stamped together by AIClient::SubmitDecision()) so this struct - and the
// JSON it serializes to - stays meaningful on its own instead of only
// inside AIRequest's transport envelope.
struct DecisionRequest
{
    ProtocolVersion Version = CurrentProtocolVersion;
    uint64 RequestId = 0;
    AgentContext Context;
};

#endif // AIWORLD_DECISIONREQUEST_H
