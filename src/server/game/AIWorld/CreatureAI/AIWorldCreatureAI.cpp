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

#include "AIWorldCreatureAI.h"
#include "Action/ActionEngineEvent.h"
#include "AIWorldMgr.h"
#include "Creature.h"
#include "MotionMaster.h"

AIWorldCreatureAI::AIWorldCreatureAI(Creature* creature) : CreatureAI(creature)
{
    // MoveInLineOfSight()/AttackStart() are overridden unconditionally
    // above regardless of react state, but setting REACT_PASSIVE too keeps
    // every other react-state-gated engine reflex consistent with "this
    // Creature does not decide anything for itself" - the same belt-and-
    // suspenders convention PassiveAI/PossessedAI/NullCreatureAI already
    // follow (PassiveAI.cpp).
    me->SetReactState(REACT_PASSIVE);

    // Cancels whatever default DB movement (Random/Waypoint/Formation) this
    // spawn would otherwise be running under MOTION_SLOT_DEFAULT - the same
    // call ScriptedEscortAI uses to take over a creature's movement
    // (ScriptedEscortAI.cpp). One-shot, at takeover; AIWorld's own pipeline
    // is responsible for anything it wants this Creature to do afterwards -
    // nothing yet, as of 2.8A (see UpdateAI()).
    me->GetMotionMaster()->MoveIdle();
}

void AIWorldCreatureAI::MovementInform(uint32 type, uint32 id)
{
    ActionEngineEvent event;
    event.MapId = me->GetMapId();
    event.SpawnId = me->GetSpawnId();
    event.RuntimeGuid = me->GetGUID();
    event.MovementType = type;
    event.MovementId = id;
    event.X = me->GetPositionX();
    event.Y = me->GetPositionY();
    event.Z = me->GetPositionZ();

    sAIWorldMgr->PublishActionEngineEvent(event);
}
