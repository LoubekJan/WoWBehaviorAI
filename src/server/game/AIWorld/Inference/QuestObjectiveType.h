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

#ifndef AIWORLD_QUESTOBJECTIVETYPE_H
#define AIWORLD_QUESTOBJECTIVETYPE_H

#include "Define.h"

// Milestone 2.13A1 deliberately ships only KillCreature as the first
// vertical slice. Add another objective type only alongside its own
// authoritative 2.13B validator - never speculatively ahead of one.
//
// Invalid is deliberately the default. An uninitialized or partially
// parsed model response must never accidentally describe a valid task.
enum class QuestObjectiveType : uint8
{
    Invalid = 0,
    KillCreature = 1
};

inline char const* ToString(QuestObjectiveType type)
{
    switch (type)
    {
        case QuestObjectiveType::KillCreature:
            return "KILL_CREATURE";
        case QuestObjectiveType::Invalid:
        default:
            return "INVALID";
    }
}

#endif // AIWORLD_QUESTOBJECTIVETYPE_H
