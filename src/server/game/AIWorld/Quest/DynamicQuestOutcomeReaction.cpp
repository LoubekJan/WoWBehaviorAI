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

#include "DynamicQuestOutcomeReaction.h"

namespace
{
    std::optional<DynamicQuestOutcomeReactionKind> ToReactionKind(WorldEventType type)
    {
        switch (type)
        {
            case WorldEventType::DynamicQuestCompleted: return DynamicQuestOutcomeReactionKind::Completed;
            case WorldEventType::DynamicQuestFailed:    return DynamicQuestOutcomeReactionKind::Failed;
            case WorldEventType::DynamicQuestExpired:   return DynamicQuestOutcomeReactionKind::Expired;
            default:                                    return std::nullopt;
        }
    }

    bool IsEligible(AgentId issuer, MemoryRecord const& memory, DynamicQuestOutcomeReactionKind& outKind)
    {
        if (memory.Owner != issuer)
            return false;

        if (memory.Type != ObservationType::WorldEvent)
            return false;

        if (memory.Target.Agent != issuer)
            return false;

        if (!memory.SourceEventType)
            return false;

        std::optional<DynamicQuestOutcomeReactionKind> kind = ToReactionKind(*memory.SourceEventType);
        if (!kind)
            return false;

        outKind = *kind;
        return true;
    }

    // Most recent by LastObservedAtMs; ties broken by the higher
    // SourceEventId (EventBus::Publish() assigns EventId monotonically -
    // see this file's own header comment).
    bool IsMoreRecent(MemoryRecord const& candidate, MemoryRecord const& current)
    {
        if (candidate.LastObservedAtMs != current.LastObservedAtMs)
            return candidate.LastObservedAtMs > current.LastObservedAtMs;

        return candidate.SourceEventId > current.SourceEventId;
    }
}

std::optional<DynamicQuestOutcomeReaction> SelectDynamicQuestOutcomeReaction(
    AgentId issuer, std::vector<MemoryRecord> const& memories)
{
    if (!issuer)
        return std::nullopt;

    MemoryRecord const* best = nullptr;
    DynamicQuestOutcomeReactionKind bestKind = DynamicQuestOutcomeReactionKind::Completed;

    for (MemoryRecord const& memory : memories)
    {
        DynamicQuestOutcomeReactionKind kind;
        if (!IsEligible(issuer, memory, kind))
            continue;

        if (!best || IsMoreRecent(memory, *best))
        {
            best = &memory;
            bestKind = kind;
        }
    }

    if (!best)
        return std::nullopt;

    DynamicQuestOutcomeReaction reaction;
    reaction.Kind = bestKind;
    reaction.SourceEventId = best->SourceEventId;
    reaction.CorrelationId = best->CorrelationId;
    reaction.ObservedAtMs = best->LastObservedAtMs;
    reaction.Channel = best->Channel;
    return reaction;
}
