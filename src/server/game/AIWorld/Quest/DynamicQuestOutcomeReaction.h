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

#ifndef AIWORLD_DYNAMICQUESTOUTCOMEREACTION_H
#define AIWORLD_DYNAMICQUESTOUTCOMEREACTION_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Memory/MemoryRecord.h"
#include "Perception/PerceptionChannel.h"

#include <optional>
#include <vector>

// Milestone 2.13C6D: what a dynamic quest issuer's OWN memory says its
// last terminal quest outcome was - the visible-issuer-impact half of
// the roadmap's WorldEvent -> Perception -> Memory -> issuer causal loop
// (2.13C6A/B already proved the first two arrows; this proves the last
// one, entirely by reading Memory, never DynamicQuestRegistry - a
// terminal instance is already Remove()d from that registry by the time
// any of this could run).
enum class DynamicQuestOutcomeReactionKind : uint8
{
    Completed,
    Failed,
    Expired
};

struct DynamicQuestOutcomeReaction
{
    DynamicQuestOutcomeReactionKind Kind = DynamicQuestOutcomeReactionKind::Completed;

    uint64 SourceEventId = 0;
    uint64 CorrelationId = 0;
    uint64 ObservedAtMs = 0;

    PerceptionChannel Channel = PerceptionChannel::Sight;
};

// Pure selector over an already-fetched, per-agent memory slice (the
// caller is responsible for having called
// ShortTermMemory::GetActiveForAgent(issuer, nowMs) - see that method's
// own comment; this function never touches ShortTermMemory itself, never
// touches Creature/Player/Map, never calls ai-server). Considers only a
// memory where ALL of the following hold:
//   - Owner == issuer
//   - Type == ObservationType::WorldEvent
//   - Target.Agent == issuer (the event names this agent as its issuer -
//     see BuildDynamicQuestOutcomeWorldEvent()'s own Target.Agent/
//     Target.Guid comment)
//   - SourceEventType is DynamicQuestCompleted/Failed/Expired
// Every other memory (a different owner, a different event's issuer, a
// non-WorldEvent observation, or an unrelated/missing SourceEventType) is
// ignored outright - this must never surface an unrelated memory as a
// dynamic quest outcome reaction.
//
// When more than one eligible memory exists (e.g. two different quests
// this same agent issued both resolved recently), the most recent one by
// LastObservedAtMs wins; a tie is broken deterministically by the higher
// SourceEventId (EventBus::Publish() assigns EventId monotonically, so
// among two memories observed in the same millisecond the higher id is
// the more recently published one) - never an unspecified/iteration-
// order-dependent pick.
//
// nullopt if issuer is empty (AgentId{}) or no eligible memory exists.
std::optional<DynamicQuestOutcomeReaction> SelectDynamicQuestOutcomeReaction(
    AgentId issuer, std::vector<MemoryRecord> const& memories);

#endif // AIWORLD_DYNAMICQUESTOUTCOMEREACTION_H
