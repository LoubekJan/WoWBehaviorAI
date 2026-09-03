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

#ifndef AIWORLD_QUESTPROPOSAL_H
#define AIWORLD_QUESTPROPOSAL_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Event/WorldEventType.h"
#include "ObjectGuid.h"
#include "QuestObjectiveType.h"

#include <string>

// Milestone 2.13B: the validated, server-only proposal
// QuestProposalDraft's own comment refers to - the real thing an untrusted
// draft becomes once ValidateDynamicTaskCandidate() accepts it. Every
// field here has already been checked against the server's CURRENT
// policy and a fresh live-world re-derivation.
//
// Draft-derived fields (Title/Description/Objective/RequiredCount/
// MaxRangeYards/ExpiryMs/RewardMoneyCopper) remain model-originated, but
// have passed the server's authoritative validation. Giver/target
// identities (Giver/GiverRuntimeGuid/TargetGuid/TargetEntry/TargetMapId)
// are server-owned and were never exposed to the model. QuestProposal
// itself is an internal server-only value and is never serialized to
// ai-server.
//
// Still not an authorization to mutate anything. This milestone only ever
// logs a QuestProposal (never its Title/Description text, never a GUID)
// and immediately discards it - no storage, no queue. A future 2.13C
// consumer must not treat any field below as still true by the time it
// runs: it owes itself a fresh live re-resolution of Giver/Target before
// acting on anything here, exactly like this milestone owed one to
// 2.13A3B's own acceptance.
struct QuestProposal
{
    AgentId Giver;
    ObjectGuid GiverRuntimeGuid;

    uint64 SourceEventId = 0;
    WorldEventType SourceEventType = WorldEventType::CreatureKilled;

    QuestObjectiveType Objective = QuestObjectiveType::Invalid;

    uint32 TargetToken = 0;
    ObjectGuid TargetGuid;
    uint32 TargetEntry = 0;
    uint32 TargetMapId = 0;

    uint32 RequiredCount = 0;
    float MaxRangeYards = 0.0f;
    uint32 ExpiryMs = 0;
    uint32 RewardMoneyCopper = 0;

    std::string Title;
    std::string Description;
};

#endif // AIWORLD_QUESTPROPOSAL_H
