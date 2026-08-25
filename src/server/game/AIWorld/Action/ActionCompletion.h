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

#ifndef AIWORLD_ACTIONCOMPLETION_H
#define AIWORLD_ACTIONCOMPLETION_H

#include "ActionType.h"
#include "Agent/AgentId.h"
#include "Define.h"
#include "Goal/GoalType.h"

// Milestone 2.8F: how an ActiveAction stopped running. Distinct from
// ActionResult (what ExecuteX() did the instant it was called) and
// GoalCompletion (why the goal that requested the action ended) - this is
// specifically "what became of the engine movement itself". Succeeded
// only ever means the movement reached its own natural conclusion
// (Arrived for MoveTo); it says nothing about whether the goal that
// wanted it is now satisfied - arriving at a food target is not the same
// as having eaten.
enum class ActionCompletionStatus : uint8
{
    Succeeded,
    Failed,
    Cancelled
};

inline char const* ToString(ActionCompletionStatus status)
{
    switch (status)
    {
        case ActionCompletionStatus::Succeeded: return "SUCCEEDED";
        case ActionCompletionStatus::Failed:    return "FAILED";
        case ActionCompletionStatus::Cancelled: return "CANCELLED";
        default:                                return "UNKNOWN";
    }
}

enum class ActionCompletionReason : uint8
{
    // The engine-owned movement generator reached its own natural
    // conclusion (MovementInform, 2.8F) AND the actor's position at that
    // moment was actually within arrival tolerance of the destination -
    // MoveTo's only Succeeded reason. MovementInform() firing alone is not
    // sufficient: MoveSplineInit::MoveTo() accepts a PATHFIND_INCOMPLETE
    // path (which finalizes, and still fires this callback, short of the
    // requested destination).
    Arrived,
    // MovementInform() fired, but the actor's position wasn't within
    // arrival tolerance of the destination - almost certainly a
    // PATHFIND_INCOMPLETE path. Distinct from EngineStopped: this is a
    // callback AIWorld actually received and checked, not a silently
    // vanished generator.
    DestinationNotReached,
    // An emergency goal (FleeDanger) interrupted the goal that owned this
    // action.
    GoalInterrupted,
    // The goal that owned this action reached its own terminal outcome
    // (Succeeded/Failed) before the action itself did.
    GoalCompleted,
    // The actor stopped being Materialized while this action was running.
    ActorDematerialized,
    // The actor died while this action was running - TrinityCore's own
    // death handling (MotionMaster::StopOnDeath()) already stops the
    // movement itself; this only reflects that in ActiveActionState.
    ActorDead,
    // Reconciliation (2.8F P2 fix, UpdateNeeds()): AIWorld's own
    // MovePointId generator is no longer present in MOTION_SLOT_ACTIVE,
    // but no ActionEngineEvent for it was ever processed - most likely a
    // dropped event (ActionEngineEventBus's bounded queue overflowed), but
    // covers any other way the engine's movement state could stop matching
    // ActiveActionState without an event ever arriving. Reconciliation
    // still checks position the same way Arrived/DestinationNotReached do;
    // this is only the reason used when the check runs without an event.
    EngineStopped,
    // Milestone 2.8G: Eat's only Succeeded reason. Eat has no engine
    // completion callback to wait for - ExecuteEat()'s emote is fire-and-
    // forget, so AIWorldMgr::TryEat() treats a Started ActionResult as
    // immediately Consumed, in the same call, with no ActiveActionState
    // ever created for it.
    Consumed,
    // Milestone 2.11E1 P3 fix: Work/Rest's only Succeeded reason - the same
    // "no engine completion callback, Started treated as immediately done,
    // in the same call, no ActiveActionState" shape as Consumed, just not
    // literally eating. Kept distinct from Consumed rather than reused, so
    // a future 2.11E2 economy mutation can gate specifically on this
    // reason without also matching Eat completions.
    Performed
};

inline char const* ToString(ActionCompletionReason reason)
{
    switch (reason)
    {
        case ActionCompletionReason::Arrived:               return "ARRIVED";
        case ActionCompletionReason::DestinationNotReached: return "DESTINATION_NOT_REACHED";
        case ActionCompletionReason::GoalInterrupted:       return "GOAL_INTERRUPTED";
        case ActionCompletionReason::GoalCompleted:         return "GOAL_COMPLETED";
        case ActionCompletionReason::ActorDematerialized:   return "ACTOR_DEMATERIALIZED";
        case ActionCompletionReason::ActorDead:             return "ACTOR_DEAD";
        case ActionCompletionReason::EngineStopped:         return "ENGINE_STOPPED";
        case ActionCompletionReason::Consumed:              return "CONSUMED";
        case ActionCompletionReason::Performed:             return "PERFORMED";
        default:                                            return "UNKNOWN";
    }
}

// Milestone 2.8F: pure value - no Creature*/Unit*/Map*. Produced only by
// AIWorldMgr on the world thread, after validating an ActionEngineEvent
// (for Arrived) or at one of the existing cancellation call sites (for
// everything else).
struct ActionCompletion
{
    AgentId Actor;
    ActionType Type = ActionType::MoveTo;
    GoalType SourceGoal = GoalType::GetFood;
    uint64 GoalStartedAtMs = 0;

    ActionCompletionStatus Status = ActionCompletionStatus::Failed;
    ActionCompletionReason Reason = ActionCompletionReason::EngineStopped;
    uint64 CompletedAtMs = 0;
};

#endif // AIWORLD_ACTIONCOMPLETION_H
