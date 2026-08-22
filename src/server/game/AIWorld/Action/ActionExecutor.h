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

#ifndef AIWORLD_ACTIONEXECUTOR_H
#define AIWORLD_ACTIONEXECUTOR_H

#include "ActionRequest.h"
#include "ActionResult.h"
#include "Define.h"

class Creature;
class Unit;

// Milestone 2.8B/2.8C/2.8D: the one place in AIWorld allowed to touch TrinityCore's
// engine API to actually make a Creature do something - the third step of
// AI proposes (ActionRequest) / ActionSystem validates / ActionExecutor
// executes. World thread only, called only after ActionSystem::Validate()
// has already returned Allowed == true for this exact request; never holds
// onto a live Creature&/Unit& past the call, and takes them by reference
// specifically so nothing here could accidentally store one - the caller
// (AIWorldMgr) owns their lifetime and passes them in fresh every call.
//
// Deliberately does not touch combat/threat state: no CombatStop(),
// AttackStop(), or ClearAllThreat() - TrinityCore's combat/threat/damage
// bookkeeping keeps running normally while an agent flees (2.6B2's
// SafetyPressure and this agent's own FleeDanger goal both depend on that
// staying true), and death is left entirely to TrinityCore's own
// MotionMaster lifecycle.
class TC_GAME_API ActionExecutor
{
    public:
        // Starts an untimed flee from fleeSource via
        // GetMotionMaster()->MoveFleeing() - TrinityCore's own pathfinding
        // and collision-aware movement, not anything AIWorld computes
        // itself. Returns Failed/UnsupportedAction (and does nothing) if
        // request.Type is not Flee - defensive only; every current call
        // site already validated this before calling.
        ActionResult ExecuteFlee(ActionRequest const& request, Creature& actor, Unit& fleeSource) const;

        // Ends a flee started by ExecuteFlee() - removes only the
        // FLEEING_MOTION_TYPE generator (never MotionMaster::Clear(),
        // which could later delete an unrelated Action's movement), and
        // only halts the actor's current spline (StopMoving()) if FLEE was
        // actually still the active movement generator when this is
        // called - if something else (knockback, a charge effect, ...) had
        // already taken over as active, StopMoving() must not touch that
        // unrelated movement just because our own FLEE also happened to
        // end around the same time. Called once the goal that started the
        // flee reaches a terminal outcome (Succeeded/Failed) - not for
        // death, which TrinityCore's own MotionMaster lifecycle already
        // handles without help.
        void StopFlee(Creature& actor) const;

        // Milestone 2.8D: starts MoveTo via GetMotionMaster()->MovePoint()
        // - TrinityCore's own pathfinding, not anything AIWorld computes
        // itself. Tags the generator with a fixed AIWorld-owned point id
        // (see ActionExecutor.cpp) so StopMoveTo() can find and remove
        // exactly this movement later, never any other system's unrelated
        // MovePoint() call - unlike FLEEING_MOTION_TYPE for ExecuteFlee(),
        // POINT_MOTION_TYPE is used throughout the engine (scripts,
        // escorts, formations, ...), not exclusively by AIWorld. Returns
        // Failed/UnsupportedAction (and does nothing) if request.Type is
        // not MoveTo or request.Destination is empty - defensive only;
        // every current call site already validated this before calling.
        ActionResult ExecuteMoveTo(ActionRequest const& request, Creature& actor) const;

        // Ends a MoveTo started by ExecuteMoveTo() - finds and removes
        // only the specific point-movement generator instance
        // ExecuteMoveTo() created (matched by its AIWorld-owned point id,
        // not just POINT_MOTION_TYPE), and only halts the actor's current
        // spline (StopMoving()) if that generator was actually still the
        // active one - same reasoning as StopFlee(). A no-op if the point
        // movement already finished naturally (TrinityCore's own
        // MotionMaster already removed it once the actor arrived) or was
        // never running. Not wired to an automatic trigger in 2.8D - MoveTo
        // isn't driven by any goal whose completion would call this yet;
        // that wiring is 2.8E's, the same way StopFlee() only started
        // actually firing once 2.7B2 gave FLEE_DANGER a real completion
        // event to hook into.
        void StopMoveTo(Creature& actor) const;
};

#endif // AIWORLD_ACTIONEXECUTOR_H
