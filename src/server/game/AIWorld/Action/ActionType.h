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

#ifndef AIWORLD_ACTIONTYPE_H
#define AIWORLD_ACTIONTYPE_H

#include "Define.h"

// Milestone 2.8A/2.8D: Flee and MoveTo only - the full FOLLOW/ATTACK/TALK/
// TRADE/EAT/SLEEP/WORK/INVESTIGATE/REQUEST_HELP catalog (per the roadmap's
// 2.8 Bezpečné Action API) comes later, once these have cleared their own
// runtime gate. MoveTo is deliberately not wired to GoalSystem/GET_FOOD in
// 2.8D - it exists to prove the Action API isn't Flee-special, not to
// plan where an agent should go. Deliberately no GetFood -> Eat mapping
// either: the agent doesn't know where food is, whether it owns any, or
// whether it can reach it - that's planning (2.8E+), not something to
// skip past.
enum class ActionType : uint8
{
    Flee,
    MoveTo
};

inline char const* ToString(ActionType type)
{
    switch (type)
    {
        case ActionType::Flee:   return "FLEE";
        case ActionType::MoveTo: return "MOVE_TO";
        default:                 return "UNKNOWN";
    }
}

#endif // AIWORLD_ACTIONTYPE_H
