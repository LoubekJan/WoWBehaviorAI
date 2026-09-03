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

#ifndef AIWORLD_DYNAMICTASKRESPONSE_H
#define AIWORLD_DYNAMICTASKRESPONSE_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "DynamicTaskProtocolVersion.h"
#include "QuestProposalDraft.h"

// Milestone 2.13A1: the parsed /dynamic-task response.
//
// Version/RequestId/Agent/SnapshotSequence are endpoint-envelope fields
// controlled/echoed by ai-server itself, not fields the LLM is asked to
// invent. The LLM produces only QuestProposalDraft.
struct DynamicTaskResponse
{
    DynamicTaskProtocolVersion Version =
        CurrentDynamicTaskProtocolVersion;

    uint64 RequestId = 0;
    AgentId Agent;
    uint64 SnapshotSequence = 0;

    QuestProposalDraft Proposal;
};

#endif // AIWORLD_DYNAMICTASKRESPONSE_H
