/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTCHAT_H
#define AIWORLD_NPCAGENTCHAT_H

#include "Define.h"
#include <string>

class Creature;
class Unit;
namespace WorldPackets::Chat { class Chat; }

namespace NpcAgentChat
{
    // Packet-only seam: preserves real sender/target GUIDs, locale, text and
    // type. Must run before serialization. Invalid input leaves it unchanged.
    TC_GAME_API bool SetAuthoredSenderName(WorldPackets::Chat::Chat& packet, std::string const& authoredName);
}

// World-thread only. Public speech keeps Unit::Talk's range/phase/locale
// distribution; whisper requires a current visible Player recipient.
// Success means the valid native broadcast/direct-send was submitted, not
// a client acknowledgement. Public NPC speech can exist without observers.
// Creature identity, template name, entry and database state are untouched.
TC_GAME_API bool SendNpcAgentChat(Creature& creature, Unit* target,
    std::string const& authoredName, std::string const& text, bool whisper);

#endif // AIWORLD_NPCAGENTCHAT_H
