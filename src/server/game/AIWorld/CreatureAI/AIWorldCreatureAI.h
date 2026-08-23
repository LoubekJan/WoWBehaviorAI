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

#ifndef AIWORLD_AIWORLDCREATUREAI_H
#define AIWORLD_AIWORLDCREATUREAI_H

#include "CreatureAI.h"

// Milestone 2.8A.5: the CreatureAI TrinityCore instantiates for a Creature
// whose (MapId, SpawnId) is a registered AIWorld agent - see
// AIWorldMgr::OwnsSpawn(), consulted from FactorySelector::SelectAI()
// (CreatureAISelector.cpp) ahead of the pet/scripted/AIName/Permissible
// selection paths, so an AIWorld agent always gets this regardless of what
// its spawn row's AIName/ScriptName happen to say.
//
// Deliberately near-empty: TrinityCore keeps owning damage, combat/threat
// bookkeeping, death/respawn, and pathfinding execution - nothing here
// touches any of that. This class only suppresses the DEFAULT AI's
// autonomous decisions (auto-aggro via MoveInLineOfSight, auto-chase +
// auto-melee via AttackStart, and whatever DB-driven Random/Waypoint
// movement the spawn would otherwise run) so that AIWorld's own
// Needs -> Goal -> ActionRequest -> ActionSystem pipeline is never racing
// against a second, TrinityCore-driven decision maker for the same
// Creature. It does not yet drive anything itself either - UpdateAI() is
// intentionally empty. 2.8B is where a Validated ActionRequest starts
// actually calling into GetMotionMaster()/Attack() (from here or wherever
// the executor ends up living) - until then this Creature simply does
// nothing on its own, by design.
class TC_GAME_API AIWorldCreatureAI : public CreatureAI
{
    public:
        explicit AIWorldCreatureAI(Creature* creature);

        // No autonomous decisions - AIWorld's Needs/Goal/Action pipeline
        // (driven entirely from AIWorldMgr::Update(), not from here) is
        // what decides anything this Creature does, and as of 2.8A that
        // pipeline is still audit-log-only.
        void UpdateAI(uint32 /*diff*/) override { }

        // Suppresses only the reflex of chasing/meleeing whatever this
        // Creature is attacking - TrinityCore's own combat/threat/damage
        // bookkeeping (JustEnteredCombat, JustDied, threat table, etc.)
        // still runs completely normally; nothing here touches any of it.
        void AttackStart(Unit* /*target*/) override { }

        // Suppresses CreatureAI::MoveInLineOfSight()'s default aggro
        // reflex (CreatureAI.cpp) unconditionally, the same way PassiveAI
        // does - this override runs regardless of react state, so it does
        // not depend on the constructor's SetReactState(REACT_PASSIVE)
        // call below to actually stop aggro.
        void MoveInLineOfSight(Unit* /*who*/) override { }

        // Milestone 2.8F: TrinityCore's authoritative "this movement
        // generator reached its own natural conclusion" callback -
        // PointMovementGenerator<Creature>::MovementInform() (called only
        // from MotionMaster::Update()'s natural-completion path, never
        // from an explicit Remove(), see MotionMaster.cpp's "Natural, and
        // only, call to MovementInform" comment) calls this with
        // (POINT_MOTION_TYPE, the point's own id) once a MOVE_TO arrives.
        // Runs on whatever thread TrinityCore calls it from (a map-updater
        // thread during Map::Update(), not necessarily the world thread) -
        // must not touch AgentRegistry or any AIWorld planning state
        // directly. Resolves only plain values from me and hands them to
        // AIWorldMgr::PublishActionEngineEvent(), the same
        // enqueue-and-defer-to-Drain() pattern PublishWorldEvent() already
        // uses for perception events.
        void MovementInform(uint32 type, uint32 id) override;
};

#endif // AIWORLD_AIWORLDCREATUREAI_H
