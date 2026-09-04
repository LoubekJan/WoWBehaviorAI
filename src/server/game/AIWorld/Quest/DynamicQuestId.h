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

#ifndef AIWORLD_DYNAMICQUESTID_H
#define AIWORLD_DYNAMICQUESTID_H

#include "Define.h"
#include <compare>

// Milestone 2.13C1: stable identity for one dynamic quest lifecycle
// instance - deliberately NOT a TrinityCore uint32 QuestId. TrinityCore's
// quest system expects a QuestId naming a static, DB-defined Quest
// template; a dynamic quest has neither. Keeping this its own 64-bit ID
// space means the lifecycle domain never has to pretend to be a real
// TrinityCore quest, and a future client-facing bridge (2.13C6) can map
// explicitly between the two ID spaces rather than this type silently
// aliasing one. Allocation (e.g. a monotonic counter) belongs to whatever
// owns a lifecycle registry, not to this pure value type.
struct DynamicQuestId
{
    uint64 Value = 0;

    explicit operator bool() const { return Value != 0; }
    auto operator<=>(DynamicQuestId const&) const = default;
};

#endif // AIWORLD_DYNAMICQUESTID_H
