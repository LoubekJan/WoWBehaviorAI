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

#include "ActionExecutor.h"
#include "Creature.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "PointMovementGenerator.h"

namespace
{
    // Escort/waypoint scripts typically tag their own PointMovementGenerator
    // calls with small sequential ids (a waypoint index: 0, 1, 2, ...), so a
    // large, distinctive constant keeps AIWorld's own MoveTo generator
    // unambiguously identifiable via PointMovementGenerator::GetId(),
    // never colliding with an unrelated system's point movement that
    // happens to share the same POINT_MOTION_TYPE.
    constexpr uint32 AIWorldMovePointId = 0xA1700000;
}

ActionResult ActionExecutor::ExecuteFlee(ActionRequest const& request, Creature& actor, Unit& fleeSource) const
{
    ActionResult result;
    result.Actor = request.Actor;
    result.Type = request.Type;
    result.SourceGoal = request.SourceGoal;
    result.GoalStartedAtMs = request.GoalStartedAtMs;

    if (request.Type != ActionType::Flee)
    {
        result.Status = ActionExecutionStatus::Failed;
        result.Reason = ActionExecutionReason::UnsupportedAction;
        return result;
    }

    // Untimed (time=0): the flee ends when AIWorld's own goal lifecycle
    // (SafetyPressure dropping below retention, or the goal timing out)
    // says it should, via StopFlee() - not on a TrinityCore-owned timer
    // this class would then have to coordinate with.
    actor.GetMotionMaster()->MoveFleeing(&fleeSource);

    result.Status = ActionExecutionStatus::Started;
    result.Reason = ActionExecutionReason::None;
    return result;
}

void ActionExecutor::StopFlee(Creature& actor) const
{
    MotionMaster* motion = actor.GetMotionMaster();

    // StopMoving() halts whatever spline is currently running, unscoped by
    // generator type - if something else (knockback, a charge effect, ...)
    // has since taken over as the active movement generator, FLEE is no
    // longer running and StopMoving() must not touch that unrelated
    // movement. Only call it when FLEE was actually the one still active.
    bool wasActiveFlee = motion->GetCurrentMovementGeneratorType() == FLEEING_MOTION_TYPE;

    motion->Remove(FLEEING_MOTION_TYPE);

    if (wasActiveFlee)
        actor.StopMoving();
}

ActionResult ActionExecutor::ExecuteMoveTo(ActionRequest const& request, Creature& actor) const
{
    ActionResult result;
    result.Actor = request.Actor;
    result.Type = request.Type;
    result.SourceGoal = request.SourceGoal;
    result.GoalStartedAtMs = request.GoalStartedAtMs;

    if (request.Type != ActionType::MoveTo || !request.Destination)
    {
        result.Status = ActionExecutionStatus::Failed;
        result.Reason = ActionExecutionReason::UnsupportedAction;
        return result;
    }

    // Defensive mirror of ValidateMoveTo()'s HasActiveMovement check (2.8D
    // P2 fix): the world-thread caller should already have rejected this
    // request via ActionSystem::Validate() before ever reaching here, but
    // MOTION_SLOT_ACTIVE could in principle have changed between that
    // check and this call. MoveTo may only start from an idle actor -
    // adding into an already-occupied active slot could either replace a
    // movement this class doesn't own or queue silently behind it.
    if (actor.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE))
    {
        result.Status = ActionExecutionStatus::Failed;
        result.Reason = ActionExecutionReason::EngineRejected;
        return result;
    }

    actor.GetMotionMaster()->MovePoint(AIWorldMovePointId,
        request.Destination->X, request.Destination->Y, request.Destination->Z);

    result.Status = ActionExecutionStatus::Started;
    result.Reason = ActionExecutionReason::None;
    return result;
}

void ActionExecutor::StopMoveTo(Creature& actor) const
{
    MotionMaster* motion = actor.GetMotionMaster();

    // POINT_MOTION_TYPE alone isn't a safe removal key the way
    // FLEEING_MOTION_TYPE is for StopFlee() - scripts/escorts/formations
    // all use it too. Find the specific generator instance this class
    // created, identified by AIWorldMovePointId, and remove only that one.
    MovementGenerator* ours = motion->GetMovementGenerator([](MovementGenerator const* gen)
    {
        if (gen->GetMovementGeneratorType() != POINT_MOTION_TYPE)
            return false;

        auto const* point = dynamic_cast<PointMovementGenerator<Creature> const*>(gen);
        return point && point->GetId() == AIWorldMovePointId;
    });

    if (!ours)
        return;

    // Same reasoning as StopFlee(): PointMovementGenerator::DoFinalize()
    // only clears UNIT_STATE_ROAMING_MOVE, it does not itself stop the
    // in-progress spline, and StopMoving() is unscoped by generator
    // instance - only call it when our point movement was actually the
    // active one.
    bool wasActive = motion->GetCurrentMovementGenerator() == ours;

    motion->Remove(ours);

    if (wasActive)
        actor.StopMoving();
}
