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

#ifndef AIWORLD_QUESTCONTRACTLIMITS_H
#define AIWORLD_QUESTCONTRACTLIMITS_H

#include "Define.h"

// Milestone 2.13A1: explicit bounds for the /dynamic-task wire contract.
//
// Every collection or string QuestContext/QuestProposalDraft carries must
// be capped by one of the structural constants below, and every numeric
// field the model is asked to choose for a QuestProposalDraft must stay
// inside the matching QuestProposalLimits value that QuestContext hands
// it. None of this makes a QuestProposalDraft authoritative - it only
// bounds the wire shape and tells the model what the server's policy
// window is for this one request. 2.13B re-validates every field against
// the server's own policy again, independently of what the model was
// told or what it actually sent back; a request-scoped copy of policy is
// not proof a draft obeys it.

// Structural caps on QuestContext's own collections/strings - these guard
// the wire format itself (parse cost, payload size), not gameplay policy.
constexpr uint32 QuestContractMaxRelevantEvents = 8;
constexpr uint32 QuestContractMaxCandidateTargets = 16;
constexpr uint32 QuestContractMaxDisplayNameLength = 64;

// Structural caps on QuestProposalDraft's own untrusted text fields.
constexpr uint32 QuestContractMaxTitleLength = 80;
constexpr uint32 QuestContractMaxDescriptionLength = 400;

// Server-declared policy window for one QuestProposalDraft, carried on
// QuestContext so the model knows what it may propose before it ever
// drafts anything. Authoritative re-validation of the actual draft
// against server policy remains 2.13B's job, not this struct's.
struct QuestProposalLimits
{
    uint32 MaxRequiredCount = 0;
    float MaxRangeYards = 0.0f;
    uint32 MaxExpiryMs = 0;
    uint32 MaxRewardMoneyCopper = 0;
};

#endif // AIWORLD_QUESTCONTRACTLIMITS_H
