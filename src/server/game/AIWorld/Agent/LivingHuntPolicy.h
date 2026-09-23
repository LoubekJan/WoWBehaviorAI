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

#ifndef AIWORLD_LIVINGHUNTPOLICY_H
#define AIWORLD_LIVINGHUNTPOLICY_H

#include "Goal/GoalType.h"

namespace LivingHuntPolicy
{
    constexpr float SprintRunMultiplier = 1.35f;
    constexpr uint32 SprintDurationMs = 10000;
    constexpr float LeashDistance = 60.0f;

    // Individual predators validate a path to the prey, not to a formation
    // slot beside it. Keep that same destination policy in the executor.
    inline bool UsesFormationBearing(GoalType goal) { return goal != GoalType::PredatorHunt; }

    enum class MotionState : uint8 { Pursuing, InMeleeRange, TemporarilyBlocked, PathBlocked, VictimLost, ChaseMissing };

    inline MotionState EvaluateMotion(bool victimMatches, bool cannotReach, bool movementPrevented,
        bool ownedChase, bool inMeleeRange)
    {
        if (!victimMatches) return MotionState::VictimLost;
        // Root/stun must not be misreported as a bad path to this prey.
        if (movementPrevented) return MotionState::TemporarilyBlocked;
        if (cannotReach) return MotionState::PathBlocked;
        if (inMeleeRange) return MotionState::InMeleeRange;
        if (!ownedChase) return MotionState::ChaseMissing;
        return MotionState::Pursuing;
    }

    inline bool CanContinue(MotionState state)
    {
        return state == MotionState::Pursuing || state == MotionState::InMeleeRange || state == MotionState::TemporarilyBlocked;
    }

    inline char const* ToString(MotionState state)
    {
        switch (state)
        {
            case MotionState::Pursuing: return "HUNT_PURSUING";
            case MotionState::InMeleeRange: return "HUNT_IN_MELEE_RANGE";
            case MotionState::TemporarilyBlocked: return "HUNT_MOVEMENT_BLOCKED";
            case MotionState::PathBlocked: return "HUNT_PATH_BLOCKED";
            case MotionState::VictimLost: return "HUNT_VICTIM_LOST";
            case MotionState::ChaseMissing: return "HUNT_CHASE_MISSING";
            default: return "HUNT_UNKNOWN";
        }
    }
}

#endif
