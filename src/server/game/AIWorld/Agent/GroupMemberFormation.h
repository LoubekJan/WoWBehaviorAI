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

#ifndef AIWORLD_GROUPMEMBERFORMATION_H
#define AIWORLD_GROUPMEMBERFORMATION_H

#include "AgentGroupRecord.h"
#include "CoalitionMemberObservation.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace GroupMemberFormation
{
    constexpr float TwoPi = 6.283185307179586f;
    constexpr float MemberSpacing = 3.0f;
    constexpr float MaxRadius = 3.0f;
    constexpr float ArrivalRadius = 0.75f;
    constexpr float ChaseAngleTolerance = 0.15f;

    struct Slot
    {
        float Angle = 0.0f;
        float Radius = 0.0f;
        float X = 0.0f;
        float Y = 0.0f;
    };

    // Keep unloaded/dead members' places reserved. Reordering observations or
    // changing which member is visible must not reshuffle a settled formation.
    inline std::optional<Slot> GetSlot(AgentId actor, std::vector<AgentId> roster, float maxRadius)
    {
        if (!actor || !std::isfinite(maxRadius) || maxRadius <= 0.0f)
            return std::nullopt;
        roster.erase(std::remove(roster.begin(), roster.end(), AgentId{}), roster.end());
        std::sort(roster.begin(), roster.end());
        roster.erase(std::unique(roster.begin(), roster.end()), roster.end());
        auto member = std::lower_bound(roster.begin(), roster.end(), actor);
        if (member == roster.end() || *member != actor)
            return std::nullopt;
        if (roster.size() == 1)
            return Slot{};

        Slot slot;
        slot.Angle = TwoPi * float(member - roster.begin()) / float(roster.size());
        slot.Radius = std::min(maxRadius, MemberSpacing / (2.0f * std::sin(TwoPi / (2.0f * float(roster.size())))));
        slot.X = slot.Radius * std::cos(slot.Angle);
        slot.Y = slot.Radius * std::sin(slot.Angle);
        return slot;
    }

    inline std::optional<Slot> GetSlot(AgentId actor, std::vector<CoalitionMemberObservation> const& members, float maxRadius)
    {
        std::vector<AgentId> roster;
        for (auto const& member : members)
            roster.push_back(member.MemberId);
        return GetSlot(actor, std::move(roster), maxRadius);
    }

    inline std::optional<Slot> GetSlot(AgentId actor, AgentGroupRecord const& group, float maxRadius)
    {
        std::vector<AgentId> roster;
        for (auto const& member : group.Members)
            roster.push_back(member.Member);
        return GetSlot(actor, std::move(roster), maxRadius);
    }
}

#endif
