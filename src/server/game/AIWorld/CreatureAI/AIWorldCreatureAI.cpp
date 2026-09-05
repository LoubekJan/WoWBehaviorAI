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
        ReconcileDynamicQuestGossipFlag();
        _dynamicQuestGossipFlagTimerMs = DynamicQuestGossipFlagReconcileIntervalMs;
    }
    else
        _dynamicQuestGossipFlagTimerMs -= diff;
}

// Milestone 2.13C4 P2 fix (STATIC review): owns the actual
// UNIT_NPC_FLAG_GOSSIP mutation and the _ownsDynamicQuestGossipFlag
// bookkeeping that makes it safe - see that member's own comment in
// AIWorldCreatureAI.h. AIWorldMgr::HasLiveDynamicQuestStateForGiver() only
// answers the read-only "should this be up" question; it never touches
// the flag itself.
void AIWorldCreatureAI::ReconcileDynamicQuestGossipFlag()
{
    bool shouldShow = sAIWorldMgr->HasLiveDynamicQuestStateForGiver(me);

    if (shouldShow)
    {
        if (!_ownsDynamicQuestGossipFlag && !me->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        {
            me->SetNpcFlag(UNIT_NPC_FLAG_GOSSIP);
            _ownsDynamicQuestGossipFlag = true;
        }
    }
    else if (_ownsDynamicQuestGossipFlag)
    {
        me->RemoveNpcFlag(UNIT_NPC_FLAG_GOSSIP);
        _ownsDynamicQuestGossipFlag = false;
    }
}

bool AIWorldCreatureAI::OnGossipHello(Player* player)
{
    AIWorldMgr::DynamicQuestGossipContent content = sAIWorldMgr->GetDynamicQuestGossipContent(me, player);
    if (content.Kind == AIWorldMgr::DynamicQuestGossipContent::ContentKind::NoQuest)
        return false;

    // Milestone 2.13C4 P2 fix (STATIC review): must not suppress this
    // Creature's own native gossip/vendor/trainer/quest-giver content -
    // build that FIRST, the exact same call TrinityCore's own default
    // path (WorldSession::HandleGossipHelloOpcode(), when this override
    // returns false) would make, then layer AIWorld's own dynamic-quest
    // rows on top of it before sending. Returning true below then only
    // ever means "this class already sent the (possibly native+AIWorld
    // merged) menu itself", never "suppress the native menu".
    player->PrepareGossipMenu(me, me->GetCreatureTemplate()->GossipMenuId, true);

    // Milestone 2.13C5: ReadyToTurnIn gets its own "objective complete"
    // line instead of a "Progress: 3/3" that would otherwise never
    // change again - see FormatDynamicQuestReadyToTurnInGossipLine()'s
    // own comment.
    std::string progressLine = content.Kind == AIWorldMgr::DynamicQuestGossipContent::ContentKind::ReadyToTurnIn
        ? FormatDynamicQuestReadyToTurnInGossipLine(content.Title)
        : FormatDynamicQuestProgressGossipLine(content.Title, content.Progress, content.RequiredCount);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, progressLine, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);

    if (!content.Description.empty())
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, content.Description, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);

    if (content.Kind == AIWorldMgr::DynamicQuestGossipContent::ContentKind::Offered)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Accept", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);
    else if (content.Kind == AIWorldMgr::DynamicQuestGossipContent::ContentKind::ReadyToTurnIn)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Turn in", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 2);

    player->SendPreparedGossip(me);
    return true;
}

bool AIWorldCreatureAI::OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId)
{
    uint32 action = GetGossipActionFor(player, gossipListId);

    // Milestone 2.13C4 P2 fix (STATIC review): the same native-menu
    // coexistence bug as OnGossipHello() above, just on the select side -
    // MiscHandler.cpp's HandleGossipSelectOptionOpcode() only calls
    // Player::OnGossipSelect() (the native vendor/trainer/DB-driven
    // gossip_menu_option handler) when this override itself returns
    // false. Unconditionally returning true here for every click would
    // silently swallow a click on a native row merged into this same
    // menu by OnGossipHello(). GOSSIP_ACTION_INFO_DEF/+1/+2 (Milestone
    // 2.13C5 added +2, "Turn in") are the only actions this class itself
    // ever hands out (see OnGossipHello() above) - anything else is
    // never ours to handle.
    if (action != GOSSIP_ACTION_INFO_DEF && action != GOSSIP_ACTION_INFO_DEF + 1 && action != GOSSIP_ACTION_INFO_DEF + 2)
        return false;

    CloseGossipMenuFor(player);

    if (action == GOSSIP_ACTION_INFO_DEF)
        return true; // the informational title/description line - nothing to do beyond closing

    // Milestone 2.13C4/2.13C5: re-resolved fresh rather than trusting
    // gossipListId/menuId to identify which DynamicQuestId was clicked -
    // see this method's own declaration comment in AIWorldCreatureAI.h.
    // A client can never influence which quest gets accepted/turned in
    // beyond "the one this giver is showing me right now".
    AIWorldMgr::DynamicQuestGossipContent content = sAIWorldMgr->GetDynamicQuestGossipContent(me, player);

    if (action == GOSSIP_ACTION_INFO_DEF + 1)
    {
        if (content.Kind == AIWorldMgr::DynamicQuestGossipContent::ContentKind::Offered)
            sAIWorldMgr->AcceptDynamicQuestForPlayer(content.Id, player->GetGUID(), sAIWorldMgr->GetCurrentTimeMs());
    }
    else // GOSSIP_ACTION_INFO_DEF + 2 - "Turn in"
    {
        if (content.Kind == AIWorldMgr::DynamicQuestGossipContent::ContentKind::ReadyToTurnIn)
            sAIWorldMgr->CompleteDynamicQuestForPlayer(content.Id, player->GetGUID(), sAIWorldMgr->GetCurrentTimeMs());
    }

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
