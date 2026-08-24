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

#ifndef AIWORLD_DECISIONINTENTTYPE_H
#define AIWORLD_DECISIONINTENTTYPE_H

#include "Define.h"

// Milestone 2.9B: the wire vocabulary for what a DecisionResponse proposes -
// deliberately its own enum, not ActionType reused with an extra value.
// Today's three non-None cases happen to line up 1:1 with ActionType, but
// keeping them separate types means a DecisionIntentType can never be
// passed somewhere an already-validated ActionType is expected without an
// explicit, visible translation step - exactly the boundary a later
// milestone's DecisionIntent -> ActionRequest translation must go through
// anyway, built from world-thread authoritative data, never from trusting
// ai-server's own claim (see DecisionIntent.h).
enum class DecisionIntentType : uint8
{
    None,
    Flee,
    MoveTo,
    Eat
};

inline char const* ToString(DecisionIntentType type)
{
    switch (type)
    {
        case DecisionIntentType::None:   return "NONE";
        case DecisionIntentType::Flee:   return "FLEE";
        case DecisionIntentType::MoveTo: return "MOVE_TO";
        case DecisionIntentType::Eat:    return "EAT";
        default:                         return "UNKNOWN";
    }
}

#endif // AIWORLD_DECISIONINTENTTYPE_H
