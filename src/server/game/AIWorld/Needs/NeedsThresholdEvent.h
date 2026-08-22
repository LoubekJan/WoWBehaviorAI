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

#ifndef AIWORLD_NEEDSTHRESHOLDEVENT_H
#define AIWORLD_NEEDSTHRESHOLDEVENT_H

#include "Define.h"

// Milestone 2.6C: an agent's own internal Needs crossing a critical
// threshold. Deliberately NOT a WorldEventType/WorldEvent - a WorldEvent is
// a fact of the world other agents can witness via perception; an agent's
// hunger is not automatically observable by anyone else, so this never
// goes through EventBus/PerceptionSystem/Memory. Pure value, no
// AgentId/Creature/Map - AIWorldMgr attaches the AgentId itself when it
// logs/consumes this.
enum class NeedsThresholdEventType : uint8
{
    HungerCritical,
    DangerHigh
};

inline char const* ToString(NeedsThresholdEventType type)
{
    switch (type)
    {
        case NeedsThresholdEventType::HungerCritical: return "HUNGER_CRITICAL";
        case NeedsThresholdEventType::DangerHigh:      return "DANGER_HIGH";
        default:                                       return "UNKNOWN";
    }
}

struct NeedsThresholdEvent
{
    NeedsThresholdEventType Type;
    float Value = 0.0f;
};

#endif // AIWORLD_NEEDSTHRESHOLDEVENT_H
