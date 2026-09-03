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

#ifndef AIWORLD_QUESTCONTEXT_H
#define AIWORLD_QUESTCONTEXT_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Event/WorldEventType.h"
#include "QuestContractLimits.h"

#include <string>
#include <vector>

// Milestone 2.13A1: sanitized description of the world problem that caused
// dynamic-task generation to be considered.
//
// No WorldEntityRef: it contains raw engine/database identity that this
// boundary does not need.
struct QuestProblemContext
{
    WorldEventType Type = WorldEventType::CreatureKilled;

    uint32 ActorEntry = 0;
    uint32 TargetEntry = 0;
    uint32 MapId = 0;

    // Relative age is enough for the model. It does not need EventId,
    // CorrelationId, CauseEventId or an absolute internal timestamp.
    uint32 AgeMs = 0;
};

// Narrow summary of an event-backed memory selected by worldserver.
//
// A1 deliberately does not serialize MemoryId, SourceEventId or raw
// WorldEntityRef identity.
struct QuestRelevantEvent
{
    WorldEventType Type = WorldEventType::CreatureKilled;

    uint32 ActorEntry = 0;
    uint32 TargetEntry = 0;

    float Importance = 0.0f;
    float Relevance = 0.0f;
    uint32 AgeMs = 0;
};

// Target identity visible to the model.
//
// Token is request-local and meaningless outside this one request.
// The model never sees the authoritative engine identity this token
// represents - see QuestRequestProvenance::TargetBindings.
struct QuestTargetCandidate
{
    uint32 Token = 0;
    uint32 Entry = 0;

    // Server-resolved display text only. This is context, never an
    // authoritative identifier. Truncated to QuestContractMaxDisplayNameLength
    // before being placed on the wire.
    std::string DisplayName;

    uint32 MapId = 0;
    float DistanceYards = 0.0f;
    uint32 ObservationAgeMs = 0;
};

struct QuestContext
{
    AgentId Agent;
    uint64 SnapshotSequence = 0;

    QuestProblemContext Problem;

    // Bounded to at most QuestContractMaxRelevantEvents /
    // QuestContractMaxCandidateTargets entries before being placed on the
    // wire - never sent as an unbounded dump of world state.
    std::vector<QuestRelevantEvent> RelevantEvents;
    std::vector<QuestTargetCandidate> CandidateTargets;

    // The server's policy window for whatever QuestProposalDraft the
    // model returns for this request. See QuestContractLimits.h.
    QuestProposalLimits Limits;
};

#endif // AIWORLD_QUESTCONTEXT_H
