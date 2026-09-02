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
#include "ObjectGuid.h"

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
//
// Milestone 2.12F4A: deliberately still knows nothing about AgentRecord,
// AgentRegistry, or AgentControlMode - the ObserveOnly/AIWorldControlled
// ownership gate belongs entirely to ActionSystem::Validate() (mandatory,
// authoritative, checked before this class is ever reached), not to a
// second ownership check duplicated here. By the time any ExecuteX() call
// below runs, Validate() already returned Allowed == true for
// AIWorldControlled - this class's own contract ("Validate() already
// returned Allowed") already implies ControlMode was already checked, so
// it never needs to ask again.
class TC_GAME_API ActionExecutor
{
    public:
        // Milestone 2.8F: exposed (not just an ActionExecutor.cpp-local
        // anonymous-namespace constant, as it was through 2.8D/2.8E) so
        // AIWorldMgr::ProcessActionEngineEvent() can check an
        // ActionEngineEvent's MovementId against the exact id ExecuteMoveTo()
        // tagged its own PointMovementGenerator with, before ever trusting
        // it as a real arrival. See ExecuteMoveTo()'s comment for why a
        // fixed, distinctive id matters here.
        static constexpr uint32 MovePointId = 0xA1700000;

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
        // itself. Tags the generator with MovePointId so StopMoveTo() can
        // find and remove exactly this movement later, never any other
        // system's unrelated MovePoint() call - unlike FLEEING_MOTION_TYPE
        // for ExecuteFlee(), POINT_MOTION_TYPE is used throughout the
        // engine (scripts, escorts, formations, ...), not exclusively by
        // AIWorld. 2.8F's AIWorldCreatureAI::MovementInform() reports the
        // same id back on natural arrival - see
        // AIWorldMgr::ProcessActionEngineEvent(). Returns
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

        // Milestone 2.8G: purely cosmetic - HandleEmoteCommand() only
        // broadcasts a one-shot animation packet (no UpdateField, no
        // server-side state, nothing to undo later), since the current
        // food target is still just a deterministic test source, not a
        // real consumable item/GameObject. No inventory, no DB. Never
        // touches NeedsState/Hunger - this class doesn't know AgentRecord
        // exists; the caller applies NeedsSystem::SatisfyHunger() only
        // after this (and the resulting ActionCompletion) reach Succeeded/
        // Consumed. Has no engine completion callback to wait for, so the
        // caller treats a Started result as immediately Consumed - no
        // ActiveActionState is ever created for Eat. Returns Failed/
        // UnsupportedAction (and does nothing) if request.Type is not Eat
        // - defensive only; every current call site already validated
        // this before calling.
        ActionResult ExecuteEat(ActionRequest const& request, Creature& actor) const;

        // Milestone 2.11E1: same "purely cosmetic one-shot emote" shape as
        // ExecuteEat() - no UpdateField, no server-side state, nothing to
        // undo later, no ActiveActionState. Returns Failed/UnsupportedAction
        // (and does nothing) if request.Type is not Work/Rest respectively
        // - defensive only; every current call site already validated this
        // before calling.
        ActionResult ExecuteWork(ActionRequest const& request, Creature& actor) const;
        ActionResult ExecuteRest(ActionRequest const& request, Creature& actor) const;

        // Milestone 2.12G3D: starts melee combat via Unit::Attack() +
        // GetMotionMaster()->MoveChase() - TrinityCore's own combat/threat
        // bookkeeping and chase pathfinding, not anything AIWorld computes
        // itself. Deliberately NOT actor.AI()->AttackStart(target) -
        // AIWorldCreatureAI's own AttackStart() override is an intentional
        // no-op (see its own comment: it exists specifically to suppress
        // TrinityCore's OWN reflexive combat starts, e.g. via
        // MoveInLineOfSight-driven aggro, which this same class already
        // suppresses) - routing through it here would silently do nothing.
        // This calls the exact same underlying primitives
        // UnitAI::AttackStart()'s own default implementation does
        // (Unit::Attack(victim, true) + MoveChase(victim)), just directly,
        // the same way ExecuteMoveTo() already bypasses actor.AI() and
        // calls GetMotionMaster() directly rather than going through
        // AttackStart's own suppressed override or any other AI()
        // indirection. Returns Failed/EngineRejected if Unit::Attack()
        // itself refuses (dead/evading/already this exact victim with
        // nothing to change - see Unit::Attack()'s own rules), and
        // Failed/UnsupportedAction (doing nothing) if request.Type is not
        // Attack - defensive only; every current call site already
        // validated this before calling. No ActiveActionState bookkeeping
        // here - that is the caller's (AIWorldMgr::DispatchHuntAttack())
        // own responsibility, the same boundary every other ExecuteX()
        // already holds to.
        ActionResult ExecuteAttack(ActionRequest const& request, Creature& actor, Unit& target) const;

        // Milestone 2.12G3D P2 fix (STATIC review): ends combat started by
        // ExecuteAttack() - ownedTargetGuid is the HUNT attempt's own
        // pinned target, and this method touches NOTHING at all unless the
        // actor's OWN CURRENT victim (Unit::GetVictim()) still IS that
        // exact target. An earlier version called this unconditionally
        // whenever a HUNT attempt's own stop path fired, which had two
        // bugs: (1) it used a plain Unit::CombatStop(), which also calls
        // RemoveAllAttackers() - clearing every OTHER unit's own attacker
        // relationship against this actor too, silently forcing unrelated
        // attackers to stop fighting it; (2) it ran even when the stored
        // HUNT action named target A but the actor's own live victim had
        // already become some different B (engine/bookkeeping desync),
        // which would then stop B's fight instead of - or as well as -
        // ending A's, neither of which this HUNT attempt has any business
        // touching. Now: a targeted Unit::AttackStop() only (never
        // CombatStop()/RemoveAllAttackers() - this ends THIS actor's own
        // attack relationship with ownedTargetGuid, nothing broader), plus
        // removing the CHASE_MOTION_TYPE generator MoveChase() started
        // (AttackStop() itself does not touch MotionMaster) - the same
        // "find and remove only the generator we started, only halt the
        // active spline if it was actually still the one running"
        // discipline StopMoveTo()/StopFlee() already apply to their own
        // generator types. A no-op (does nothing, including to
        // MotionMaster) if the actor's current victim is not
        // ownedTargetGuid - there is nothing of THIS attempt's own left to
        // stop.
        void StopAttack(Creature& actor, ObjectGuid ownedTargetGuid) const;
};

#endif // AIWORLD_ACTIONEXECUTOR_H
