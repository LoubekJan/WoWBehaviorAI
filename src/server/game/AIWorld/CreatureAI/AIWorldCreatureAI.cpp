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
#include "Player.h"
#include "Quest/DynamicQuestGossipText.h"
#include "ScriptedGossip.h"

namespace
{
    // Milestone 2.13C4: purely a UI-freshness throttle - ballpark
    // "roughly once a second is often enough", not tied to any lifecycle
    // deadline the way AIWorld.DynamicQuestMaintenanceIntervalMs is, so
    // this stays a plain constant rather than a config key.
    constexpr uint32 DynamicQuestGossipFlagReconcileIntervalMs = 1000;
}

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

void AIWorldCreatureAI::UpdateAI(uint32 diff)
{
    if (me->GetVictim())
        DoMeleeAttackIfReady();

    // Milestone 2.13C4: see _dynamicQuestGossipFlagTimerMs's own comment
    // in AIWorldCreatureAI.h for why this is throttled rather than run
    // every tick.
    if (_dynamicQuestGossipFlagTimerMs <= diff)
    {
        sAIWorldMgr->ReconcileDynamicQuestGossipFlag(me);
        _dynamicQuestGossipFlagTimerMs = DynamicQuestGossipFlagReconcileIntervalMs;
    }
    else
        _dynamicQuestGossipFlagTimerMs -= diff;
}

bool AIWorldCreatureAI::OnGossipHello(Player* player)
{
    AIWorldMgr::DynamicQuestGossipContent content = sAIWorldMgr->GetDynamicQuestGossipContent(me, player);
    if (content.Kind == AIWorldMgr::DynamicQuestGossipContent::ContentKind::NoQuest)
        return false;

    ClearGossipMenuFor(player);

    AddGossipItemFor(player, GOSSIP_ICON_CHAT,
        FormatDynamicQuestProgressGossipLine(content.Title, content.Progress, content.RequiredCount),
        GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);

    if (!content.Description.empty())
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, content.Description, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);

    if (content.Kind == AIWorldMgr::DynamicQuestGossipContent::ContentKind::Offered)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Accept", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);

    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, me);
    return true;
}

bool AIWorldCreatureAI::OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId)
{
    uint32 action = GetGossipActionFor(player, gossipListId);
    CloseGossipMenuFor(player);

    if (action != GOSSIP_ACTION_INFO_DEF + 1)
        return true; // an informational line, or an action this milestone does not define

    // Milestone 2.13C4: re-resolved fresh rather than trusting
    // gossipListId/menuId to identify which DynamicQuestId was clicked -
    // see this method's own declaration comment in AIWorldCreatureAI.h.
    AIWorldMgr::DynamicQuestGossipContent content = sAIWorldMgr->GetDynamicQuestGossipContent(me, player);
    if (content.Kind == AIWorldMgr::DynamicQuestGossipContent::ContentKind::Offered)
        sAIWorldMgr->AcceptDynamicQuestForPlayer(content.Id, player->GetGUID(), sAIWorldMgr->GetCurrentTimeMs());

    return true;
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
