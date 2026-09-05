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

#ifndef AIWORLD_DYNAMICQUESTINSTANCE_H
#define AIWORLD_DYNAMICQUESTINSTANCE_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "DynamicQuestId.h"
#include "DynamicQuestState.h"
#include "Inference/QuestObjectiveType.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

// Milestone 2.13C1: one dynamic quest lifecycle instance - a pure value
// object, never a Player*/Creature*/Map*. Nothing here is an
// authorization to mutate anything: exactly like QuestProposal (see its
// own comment), any consumer that acts on live-world state must first
// re-resolve and re-check it fresh, never trust a field below as still
// true.
//
// Only carries what the state machine in DynamicQuestLifecycle.h operates
// on, plus (Milestone 2.13C4) the player-facing display text this
// milestone's own gossip UI needs, plus (Milestone 2.13C5) the
// already-2.13B-validated reward this instance's own turn-in pays out.
struct DynamicQuestInstance
{
    DynamicQuestId Id;
    DynamicQuestState State = DynamicQuestState::Offered;

    // Milestone 2.13C4: the SAME model-originated, already-2.13B-validated
    // text QuestProposal carried - copied once at Offer() time, never
    // re-validated or re-derived here (DynamicQuestLifecycle.h has no
    // text-validation logic of its own; ValidateDynamicTaskCandidate()
    // already proved these bounded/control-character-free before this
    // instance ever existed). Still just player-facing display data -
    // nothing here authorizes gameplay.
    std::string Title;
    std::string Description;

    // The AI agent that gave this quest, and its runtime incarnation at
    // Offer() time - carried as plain values, never re-authorizing
    // anything by themselves. See QuestProposal's own Giver/
    // GiverRuntimeGuid comment.
    AgentId Giver;
    ObjectGuid GiverRuntimeGuid;

    QuestObjectiveType Objective = QuestObjectiveType::Invalid;
    ObjectGuid TargetGuid;
    uint32 TargetEntry = 0;
    uint32 TargetMapId = 0;

    // Milestone 2.13C5: the SAME already-2.13B-validated, server-capped
    // (AIWorld.DynamicTaskMaxRewardMoneyCopper) copper amount
    // QuestProposal carried - copied once at Offer() time, never
    // re-read from the model or recomputed afterward. Paid out exactly
    // once, by AIWorldMgr::CompleteDynamicQuestForPlayer() alone, via
    // Player::ModifyMoney() - nothing here authorizes that payment by
    // itself, same as every other field on this pure value object.
    uint32 RewardMoneyCopper = 0;

    // Saturating: DynamicQuestLifecycle::ApplyDynamicQuestProgress() never
    // lets Progress exceed RequiredCount.
    uint32 RequiredCount = 0;
    uint32 Progress = 0;

    uint64 CreatedAtMs = 0;

    // The single, canonical expiry boundary this whole lifecycle
    // domain uses - see DynamicQuestLifecycle::IsDynamicQuestExpired()'s
    // own comment. Computed with a saturating add at Offer() time, never
    // a raw CreatedAtMs + ExpiryMs that could wrap around.
    uint64 ExpiresAtMs = 0;

    // Empty until AcceptDynamicQuest() succeeds (Offered -> Active), which
    // binds it permanently for the rest of this instance's lifetime -
    // there is no re-assignment API in this milestone.
    ObjectGuid AcceptedByPlayerGuid;

    // Authoritative progress-event replay guard: the identity (e.g. a
    // WorldEvent::EventId) of every event that has already contributed
    // progress, so the same authoritative kill can never be counted
    // twice. Invariant enforced by ApplyDynamicQuestProgress() itself
    // (see its own comment): size() <= RequiredCount always - a
    // genuinely new event arriving once Progress == RequiredCount is
    // rejected (ProgressAlreadyComplete) and never appended here, so
    // this list, and the cost of checking it, both stay bounded.
    std::vector<uint64> ConsumedProgressEventIds;
};

#endif // AIWORLD_DYNAMICQUESTINSTANCE_H
