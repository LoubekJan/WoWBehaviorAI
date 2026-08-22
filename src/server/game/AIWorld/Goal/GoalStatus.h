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

#ifndef AIWORLD_GOALSTATUS_H
#define AIWORLD_GOALSTATUS_H

#include "Define.h"

// Milestone 2.7B2: a terminal outcome only - no Active entry here,
// because the mere existence of an ActiveGoal already means "active".
// A goal that's still running is represented by AgentRecord::ActiveGoalState
// being set, not by a GoalStatus value.
enum class GoalStatus : uint8
{
    Succeeded,
    Failed
};

inline char const* ToString(GoalStatus status)
{
    switch (status)
    {
        case GoalStatus::Succeeded: return "SUCCEEDED";
        case GoalStatus::Failed:    return "FAILED";
        default:                    return "UNKNOWN";
    }
}

enum class GoalCompletionReason : uint8
{
    NeedSatisfied,
    Timeout
};

inline char const* ToString(GoalCompletionReason reason)
{
    switch (reason)
    {
        case GoalCompletionReason::NeedSatisfied: return "NEED_SATISFIED";
        case GoalCompletionReason::Timeout:        return "TIMEOUT";
        default:                                   return "UNKNOWN";
    }
}

#endif // AIWORLD_GOALSTATUS_H
