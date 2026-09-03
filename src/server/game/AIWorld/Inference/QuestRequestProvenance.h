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

#ifndef AIWORLD_QUESTREQUESTPROVENANCE_H
#define AIWORLD_QUESTREQUESTPROVENANCE_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Event/WorldEventType.h"
#include "Goal/GoalType.h"
#include "ObjectGuid.h"

#include <optional>
#include <vector>

// One QuestContext::CandidateTargets entry's model-visible Token, bound
// back to the authoritative target it actually names. Never derived from
// anything ai-server sends back.
struct QuestTargetBinding
{
    uint32 Token = 0;

    ObjectGuid Guid;
    uint32 Entry = 0;
    uint32 MapId = 0;
};

// Milestone 2.13A1: request-side truth captured by worldserver.
//
// Never serialized to ai-server and never reconstructed from model output.
// This follows the same "client's own echo, never server's claim" rule
// already used by DecisionProvenance.
struct QuestRequestProvenance
{
    AgentId Agent;
    uint64 SnapshotSequence = 0;

    ObjectGuid RuntimeGuid;

    std::optional<GoalType> Goal;
    uint64 GoalStartedAtMs = 0;

    uint64 SourceEventId = 0;
    uint64 SourceCorrelationId = 0;
    WorldEventType SourceEventType = WorldEventType::CreatureKilled;
    uint64 SourceOccurredAtMs = 0;

    std::vector<QuestTargetBinding> TargetBindings;
};

#endif // AIWORLD_QUESTREQUESTPROVENANCE_H
