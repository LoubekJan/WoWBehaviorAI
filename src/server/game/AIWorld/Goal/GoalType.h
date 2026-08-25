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

#ifndef AIWORLD_GOALTYPE_H
#define AIWORLD_GOALTYPE_H

#include "Define.h"

// Milestone 2.7A only generates candidates for the first two - the full
// SURVIVE/MAKE_MONEY/PROTECT_HOME/HELP_FAMILY/WORK/REST/INVESTIGATE/
// REQUEST_HELP catalog (per the roadmap's 2.7 Goal System) comes later,
// once GET_FOOD/FLEE_DANGER have cleared their own runtime gate.
//
// GoToWork/GoHome (2.11C) are a deliberate exception to that: they are
// never produced by GoalSystem::GenerateCandidates()/UpdateActiveGoal(),
// never appear in AgentRecord::ActiveGoalState, and have no Need behind
// them (see RoutineSystem.h for why routine is its own, separate, non-
// Need-driven decision from GoalSystem's). They exist in this enum purely
// so RoutineGoal::Type can double as ActionRequest::SourceGoal/
// ActiveAction::SourceGoal directly - reusing the exact same "AI proposes,
// ActionSystem validates, ActionExecutor executes" MOVE_TO pipeline
// GET_FOOD already goes through, rather than inventing a second, parallel
// action-identity type just for routine.
enum class GoalType : uint8
{
    GetFood,
    FleeDanger,
    GoToWork,
    GoHome
};

inline char const* ToString(GoalType type)
{
    switch (type)
    {
        case GoalType::GetFood:    return "GET_FOOD";
        case GoalType::FleeDanger: return "FLEE_DANGER";
        case GoalType::GoToWork:   return "GO_TO_WORK";
        case GoalType::GoHome:     return "GO_HOME";
        default:                   return "UNKNOWN";
    }
}

// Not urgency-ranking within Normal - just marks a candidate as the kind
// that must later be able to interrupt an already-active goal (per the
// roadmap's "přidat možnost cíl přerušit při nouzové situaci"). Selection/
// interruption logic itself is 2.7B - 2.7A only tags candidates with this.
enum class GoalPriority : uint8
{
    Normal,
    Emergency
};

inline char const* ToString(GoalPriority priority)
{
    switch (priority)
    {
        case GoalPriority::Normal:    return "NORMAL";
        case GoalPriority::Emergency: return "EMERGENCY";
        default:                      return "UNKNOWN";
    }
}

// What produced a GoalCandidate. Only Needs exists in 2.7A; a later
// milestone may add e.g. a directly-requested/perceived source without
// changing GoalCandidate's shape.
enum class GoalSource : uint8
{
    Needs
};

inline char const* ToString(GoalSource source)
{
    switch (source)
    {
        case GoalSource::Needs: return "NEEDS";
        default:                return "UNKNOWN";
    }
}

#endif // AIWORLD_GOALTYPE_H
