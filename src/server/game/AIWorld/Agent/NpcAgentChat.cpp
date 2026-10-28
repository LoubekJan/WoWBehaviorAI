/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "NpcAgentChat.h"
#include "NpcAgentDialogue.h"
#include "CellImpl.h"
#include "ChatPackets.h"
#include "Creature.h"
#include "Errors.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"

namespace
{
    bool ValidNamedSpeech(std::string const& authoredName, std::string const& text)
    {
        return ValidateNpcAgentPlainText(authoredName, NpcAgentDialogueLimits::MaxNameBytes,
            NpcAgentDialogueLimits::MaxNameCharacters) && ValidateNpcAgentSpeechText(text);
    }

    // Same builder/localizer contract as Trinity::CustomChatTextBuilder.
    // Only the chat packet's display name changes after native Initialize.
    class NamedNpcChatBuilder
    {
    public:
        NamedNpcChatBuilder(Creature const& creature, Unit const* target,
            std::string const& authoredName, std::string const& text)
            : _creature(creature), _target(target), _authoredName(authoredName), _text(text) { }

        void operator()(WorldPacket& data, LocaleConstant locale) const
        {
            WorldPackets::Chat::Chat packet;
            packet.Initialize(CHAT_MSG_MONSTER_SAY, LANG_UNIVERSAL, &_creature, _target, _text, 0, "", locale);
            ASSERT_WITH_SIDE_EFFECTS(NpcAgentChat::SetAuthoredSenderName(packet, _authoredName));
            packet.Write();
            data = packet.Move();
        }

    private:
        Creature const& _creature;
        Unit const* _target;
        std::string const& _authoredName;
        std::string const& _text;
    };
}

bool NpcAgentChat::SetAuthoredSenderName(WorldPackets::Chat::Chat& packet, std::string const& authoredName)
{
    if (packet.GetSize() || !packet.SenderGUID.IsCreatureOrVehicle()
        || (packet.SlashCmd != CHAT_MSG_MONSTER_SAY && packet.SlashCmd != CHAT_MSG_MONSTER_WHISPER)
        || (packet.SlashCmd == CHAT_MSG_MONSTER_WHISPER && !packet.TargetGUID.IsPlayer())
        || !ValidNamedSpeech(authoredName, packet.ChatText))
        return false;
    packet.SenderName = authoredName;
    return true;
}

bool SendNpcAgentChat(Creature& creature, Unit* target,
    std::string const& authoredName, std::string const& text, bool whisper)
{
    if (!creature.IsInWorld() || !creature.IsAlive() || !creature.GetGUID().IsCreatureOrVehicle()
        || !ValidNamedSpeech(authoredName, text))
        return false;
    if (target && (target == &creature || !target->IsInWorld() || !target->IsAlive()
        || !creature.IsInMap(target) || !creature.InSamePhase(target)
        || !creature.IsWithinDistInMap(target, 25.0f) || !creature.CanSeeOrDetect(target)
        || !target->CanSeeOrDetect(&creature) || !creature.IsWithinLOSInMap(target)))
        return false;

    if (whisper)
    {
        Player* player = target ? target->ToPlayer() : nullptr;
        if (!player || !player->GetSession())
            return false;
        WorldPackets::Chat::Chat packet;
        packet.Initialize(CHAT_MSG_MONSTER_WHISPER, LANG_UNIVERSAL, &creature, player, text,
            0, "", player->GetSession()->GetSessionDbLocaleIndex());
        if (!NpcAgentChat::SetAuthoredSenderName(packet, authoredName))
            return false;
        player->SendDirectMessage(packet.Write());
        return true;
    }

    NamedNpcChatBuilder builder(creature, target, authoredName, text);
    Trinity::LocalizedPacketDo<NamedNpcChatBuilder> localizer(builder);
    Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<NamedNpcChatBuilder>> worker(
        &creature, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY), localizer);
    Cell::VisitWorldObjects(&creature, worker, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));
    return true;
}
