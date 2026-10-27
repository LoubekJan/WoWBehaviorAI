/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

class aiworld_npc_chat : public PlayerScript
{
public:
    aiworld_npc_chat() : PlayerScript("aiworld_npc_chat") { }

    void OnChat(Player* player, uint32 type, uint32 language, std::string& message) override
    {
        // Normal /say keeps its original delivery. The manager owns all
        // opt-in, selection, live identity, phase and distance checks.
        if (type == CHAT_MSG_SAY && player && player->GetSelectedUnit())
            sAIWorldMgr->HandleNpcPlayerSpeech(player, message, language);
    }
};

void AddSC_aiworld_npc_chat()
{
    new aiworld_npc_chat();
}
