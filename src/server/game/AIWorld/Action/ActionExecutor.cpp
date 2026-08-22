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
