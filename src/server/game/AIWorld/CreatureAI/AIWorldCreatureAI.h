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
// Creature.
//
// Milestone 2.12G3D P1 fix (STATIC review): UpdateAI() is no longer
// unconditionally empty. ActionExecutor::ExecuteAttack() (2.12G3D) calls
// Unit::Attack() + MoveChase() directly - which sets the victim/chase
// relationship, but does NOT itself land any melee damage. Real damage
// only ever comes from UnitAI::DoMeleeAttackIfReady() ->
// Unit::AttackerStateUpdate(), which nothing was ever calling: TrinityCore's
// own default AI would normally call this every UpdateAI() tick once
// AttackStart() set a victim, but THIS class's own AttackStart() override
// is a deliberate no-op (see its own comment) specifically to suppress
// TrinityCore's autonomous target SELECTION - it was never meant to also
// suppress landing damage against a target AIWorld itself already
// authorized via a validated ATTACK ActionRequest. This override does not
// select, chase, or engage anything new - it only executes melee swings
// against whatever victim already exists (Unit::GetVictim(), set solely
// by ExecuteAttack()'s own Unit::Attack() call), the same "AI proposes via
// AIWorldMgr, this class only ever carries out an already-authorized
// action" boundary AttackStart()/MoveInLineOfSight() themselves preserve
// by staying suppressed.
class TC_GAME_API AIWorldCreatureAI : public CreatureAI
{
    public:
        explicit AIWorldCreatureAI(Creature* creature);

        // Milestone 2.12G3D P1 fix (STATIC review): the ONE piece of
        // per-tick engine work this class now does - see this class's own
        // header comment for why. Still no target selection, no chasing,
        // no combat decision-making: GetVictim() only reads whatever
        // Unit::Attack() (ExecuteAttack()) already set, and
        // DoMeleeAttackIfReady() itself only swings at the actor's own
        // CURRENT victim on its own weapon timer - it cannot redirect to a
        // different target. AIWorld's Needs/Goal/Action pipeline (driven
        // entirely from AIWorldMgr::Update(), not from here) still decides
        // WHETHER/WHOM to attack; this only carries out melee swings once
        // that decision already landed.
        //
        // Milestone 2.12G3D P2 fix, round 2 (STATIC review): defined in
        // AIWorldCreatureAI.cpp, not inline here - CreatureAI.h (the base
        // this class includes) only forward-declares Creature, so an
        // inline body dereferencing me (a Creature*) made this header not
        // self-compilable outside a precompiled-header build; the .cpp
        // already includes Creature.h for MovementInform()'s own use.
        void UpdateAI(uint32 diff) override;

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

        // Milestone 2.13C4: read-only against DynamicQuestRegistry state -
        // decides what (if anything) to show from
        // AIWorldMgr::GetDynamicQuestGossipContent(me, player). Returns
        // false immediately (letting TrinityCore's own default
        // WorldSession::HandleGossipHelloOpcode() path run untouched) when
        // that query finds nothing AIWorld-specific for this player at
        // this giver.
        //
        // Milestone 2.13C4 P2 fix (STATIC review): when there IS AIWorld
        // content, this must still never suppress the Creature's own
        // native gossip/vendor/trainer/quest-giver menu - the earlier
        // version cleared the menu and sent only AIWorld's own rows,
        // silently hiding a vendor/trainer/quest-giver NPC's real content
        // for as long as it also had a live dynamic quest. Now calls
        // player->PrepareGossipMenu(me, ..., true) itself FIRST (the same
        // call the native path would make) to build the native menu, then
        // layers AIWorld's own rows on top before player->
        // SendPreparedGossip(me) sends the merged result - returning true
        // here means "this class already sent the full menu itself",
        // never "suppress the native one".
        bool OnGossipHello(Player* player) override;

        // Milestone 2.13C4: the only path from a client click to
        // AIWorldMgr::AcceptDynamicQuestForPlayer() - never mutates
        // DynamicQuestRegistry state itself. Re-resolves the gossip
        // content fresh (via the same GetDynamicQuestGossipContent() call
        // OnGossipHello() uses) rather than trusting gossipListId/menuId
        // to identify which DynamicQuestId was clicked, so a client can
        // never influence which quest gets accepted beyond "the one this
        // giver is currently offering me right now".
        //
        // Milestone 2.13C4 P2 fix (STATIC review): only ever claims (and
        // returns true for) a click whose action is one of the two values
        // OnGossipHello() itself hands out - anything else is a click on
        // this same Creature's native gossip/vendor/trainer menu (merged
        // in by OnGossipHello() above) and must fall through to
        // TrinityCore's own Player::OnGossipSelect(), which
        // MiscHandler.cpp's HandleGossipSelectOptionOpcode() only calls
        // once this override returns false.
        bool OnGossipSelect(Player* player, uint32 menuId, uint32 gossipListId) override;

    private:
        // Milestone 2.13C4 P2 fix (STATIC review): the actual
        // UNIT_NPC_FLAG_GOSSIP add/remove, using
        // AIWorldMgr::HasLiveDynamicQuestStateForGiver() as its only
        // input - see _ownsDynamicQuestGossipFlag's own comment below for
        // why the mutation and its ownership bookkeeping had to move here
        // rather than staying in AIWorldMgr.
        void ReconcileDynamicQuestGossipFlag();

        // Milestone 2.13C4: throttles the
        // AIWorldMgr::HasLiveDynamicQuestStateForGiver() poll from
        // UpdateAI() - that decision only needs to track truth within
        // roughly a second, not every tick. Starts at 0 so a freshly
        // materialized agent's flag is correct from its very first
        // UpdateAI() rather than waiting a full interval.
        uint32 _dynamicQuestGossipFlagTimerMs = 0;

        // Milestone 2.13C4 P2 fix (STATIC review): explicit ownership of
        // the UNIT_NPC_FLAG_GOSSIP overlay this class itself may add -
        // true only once THIS class has actually called me->SetNpcFlag()
        // for it, never merely because the flag happens to be set (it may
        // be a native DB flag, or something else's runtime flag this
        // class must never touch). The earlier design decided whether it
        // was safe to remove purely from the CreatureTemplate's native
        // flag, which cannot distinguish "AIWorld added this" from
        // "something else added this after AIWorld last looked" - this
        // bit closes that gap by construction: RemoveNpcFlag() is only
        // ever called when this is true, and only this class ever sets or
        // clears it (see UpdateAI()'s own reconciliation below).
        bool _ownsDynamicQuestGossipFlag = false;
};

#endif // AIWORLD_AIWORLDCREATUREAI_H
