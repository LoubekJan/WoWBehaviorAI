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

#ifndef AIWORLD_QUESTPROPOSALDRAFT_H
#define AIWORLD_QUESTPROPOSALDRAFT_H

#include "Define.h"
#include "QuestObjectiveType.h"

#include <string>

// Milestone 2.13A1: untrusted structured model output.
//
// Named Draft, not QuestProposal, on purpose - this is a raw, unvalidated
// model suggestion. A real QuestProposal is the 2.13B authoritative
// validator's output, not this. Nothing in this DTO authorizes gameplay:
// every field must pass that validator before a real QuestProposal may
// exist. In particular there is no field through which the model could
// propose SQL, a spell cast, a spawn/delete, a script or any other
// execution payload - only declarative data.
struct QuestProposalDraft
{
    QuestObjectiveType Objective = QuestObjectiveType::Invalid;

    // Request-local token from QuestContext::CandidateTargets.
    // Never a GUID, spawn id or database id.
    uint32 TargetToken = 0;

    uint32 RequiredCount = 0;

    // Proposed maximum locality/range constraint. 2.13B must reject
    // non-finite, non-positive and out-of-policy values.
    float MaxRangeYards = 0.0f;

    uint32 ExpiryMs = 0;
    uint32 RewardMoneyCopper = 0;

    // Untrusted player-facing text. 2.13B must impose byte/character
    // limits and reject unwanted control/formatting characters before
    // anything becomes visible to a player.
    std::string Title;
    std::string Description;
};

#endif // AIWORLD_QUESTPROPOSALDRAFT_H
