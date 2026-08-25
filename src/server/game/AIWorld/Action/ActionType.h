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

// Milestone 2.8A/2.8D/2.8E/2.8G/2.11E1: Flee, MoveTo, Eat, Work, and Rest -
// the full FOLLOW/ATTACK/TALK/TRADE/INVESTIGATE/REQUEST_HELP catalog (per
// the roadmap's 2.8 Bezpečné Action API) comes later, once these have
// cleared their own runtime gate. GET_FOOD (2.8E) resolves a target and
// moves there via MoveTo; MOVE_TO arrival (2.8F) only means the actor is
// now standing at the target, so AIWorldMgr::TryEat() (2.8G, run only
// after this tick's own goal-selection pass - see
// PendingEatContinuation.h) proposes Eat as a second, separately-validated
// action from that same arrived position - arriving at food is not the
// same as having eaten it. NeedsState's Hunger only ever decreases via
// NeedsSystem::SatisfyHunger(), called after Eat itself reaches
// Succeeded/Consumed - never as a side effect of MOVE_TO's own completion
// handling.
//
// Work/Rest (2.11E1) follow the same "MOVE_TO gets you there, a second
// action is what you do once there" shape as GetFood/Eat, but are
// deliberately one-shot: unlike Eat (which AIWorldMgr proposes exactly
// once per MOVE_TO arrival, the same way), Work/Rest are proposed exactly
// once per AgentRecord::RoutineActivityState transition (see
// AIWorldMgr::UpdateNeeds()'s 2.11D/2.11E1 activity block) - never given
// their own ActiveActionState, since RoutineActivitySystem's own
// HasActiveAction check would otherwise immediately suppress the activity
// that just started it.
enum class ActionType : uint8
{
    Flee,
    MoveTo,
    Eat,
    Work,
    Rest
};

inline char const* ToString(ActionType type)
{
    switch (type)
    {
        case ActionType::Flee:   return "FLEE";
        case ActionType::MoveTo: return "MOVE_TO";
        case ActionType::Eat:    return "EAT";
        case ActionType::Work:   return "WORK";
        case ActionType::Rest:   return "REST";
        default:                 return "UNKNOWN";
    }
}

#endif // AIWORLD_ACTIONTYPE_H
