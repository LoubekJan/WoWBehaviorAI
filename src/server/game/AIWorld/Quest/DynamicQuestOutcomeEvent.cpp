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

#include "DynamicQuestOutcomeEvent.h"

namespace
{
    std::optional<WorldEventType> ToOutcomeEventType(DynamicQuestState state)
    {
        switch (state)
        {
            case DynamicQuestState::Completed: return WorldEventType::DynamicQuestCompleted;
            case DynamicQuestState::Failed:    return WorldEventType::DynamicQuestFailed;
            case DynamicQuestState::Expired:   return WorldEventType::DynamicQuestExpired;
            default:                           return std::nullopt;
        }
    }
}

std::optional<WorldEvent> BuildDynamicQuestOutcomeWorldEvent(
    DynamicQuestInstance const& instance,
    WorldEventLocation const& location,
    uint64 occurredAtMs)
{
    std::optional<WorldEventType> type = ToOutcomeEventType(instance.State);
    if (!type)
        return std::nullopt;

    WorldEvent event;
    event.EventId = 0;
    event.CorrelationId = instance.SourceCorrelationId;
    event.CauseEventId = instance.SourceEventId;
    event.OccurredAtMs = occurredAtMs;
    event.Type = *type;
    event.Location = location;

    event.Actor.Guid = instance.AcceptedByPlayerGuid;

    event.Target.Agent = instance.Giver;
    event.Target.Guid = instance.GiverRuntimeGuid;

    return event;
}
