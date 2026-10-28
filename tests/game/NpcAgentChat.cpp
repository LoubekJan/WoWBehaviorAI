/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/NpcAgentChat.h"
#include "ChatPackets.h"
#include "WorldPacket.h"

TEST_CASE("NPC authored chat names preserve the real wire identity and UTF8 name length", "[AIWorld][NpcAgent][Chat]")
{
    for (ChatMsg type : {CHAT_MSG_MONSTER_SAY, CHAT_MSG_MONSTER_WHISPER})
    {
        WorldPackets::Chat::Chat packet;
        packet.Initialize(type, LANG_UNIVERSAL, nullptr, nullptr, "Ahoj, sousede.");
        auto const sender = ObjectGuid::Create<HighGuid::Unit>(11328, 901035);
        auto const target = ObjectGuid::Create<HighGuid::Player>(42);
        packet.SenderGUID = sender;
        packet.TargetGUID = target;
        packet.SenderName = "Eastvale Peasant";
        packet.TargetName = "Player";
        REQUIRE(NpcAgentChat::SetAuthoredSenderName(packet, "Mat\xC4\x9Bj"));
        CHECK(packet.SenderGUID == sender);
        CHECK(packet.SenderGUID.GetEntry() == 11328);
        CHECK(packet.TargetGUID == target);
        CHECK(packet.TargetName == "Player");
        CHECK(packet.ChatText == "Ahoj, sousede.");
        WorldPacket wire(*packet.Write());
        CHECK(wire.GetOpcode() == SMSG_MESSAGECHAT);
        CHECK(wire.read<uint8>() == type);
        CHECK(wire.read<int32>() == LANG_UNIVERSAL);
        ObjectGuid wireSender, wireTarget;
        wire >> wireSender;
        CHECK(wireSender == sender);
        CHECK(wire.read<uint32>() == 0);
        CHECK(wire.read<uint32>() == 7); // six UTF-8 bytes plus terminator
        CHECK(wire.ReadCString() == "Mat\xC4\x9Bj");
        wire >> wireTarget;
        CHECK(wireTarget == target);
        CHECK(wire.read<uint32>() == 15);
        CHECK(wire.ReadCString() == "Ahoj, sousede.");
        CHECK(wire.read<uint8>() == CHAT_TAG_NONE);
        CHECK(wire.rpos() == wire.size());
        CHECK_FALSE(NpcAgentChat::SetAuthoredSenderName(packet, "Another name"));
    }
}

TEST_CASE("NPC named chat rejects unsafe names speech and non-NPC packet kinds before mutation", "[AIWorld][NpcAgent][Chat]")
{
    auto prepare = [](WorldPackets::Chat::Chat& packet)
    {
        packet.Initialize(CHAT_MSG_MONSTER_SAY, LANG_UNIVERSAL, nullptr, nullptr, "Ahoj.");
        packet.SenderGUID = ObjectGuid::Create<HighGuid::Unit>(11328, 901036);
        packet.SenderName = "Eastvale Peasant";
    };
    for (std::string const& name : {std::string{}, std::string("   "), std::string("Bela\nFake"),
        std::string("|cffffffffBela|r"), std::string("\xC0\xAF"), std::string(33, 'a')})
    {
        WorldPackets::Chat::Chat packet;
        prepare(packet);
        CHECK_FALSE(NpcAgentChat::SetAuthoredSenderName(packet, name));
        CHECK(packet.SenderName == "Eastvale Peasant");
        CHECK(packet.GetSize() == 0);
    }
    WorldPackets::Chat::Chat packet;
    prepare(packet);
    packet.ChatText = "|Hitem:1|hspoof|h";
    CHECK_FALSE(NpcAgentChat::SetAuthoredSenderName(packet, "Bela"));
    CHECK(packet.SenderName == "Eastvale Peasant");
    prepare(packet);
    packet.SlashCmd = CHAT_MSG_SAY;
    CHECK_FALSE(NpcAgentChat::SetAuthoredSenderName(packet, "Bela"));
    prepare(packet);
    packet.SenderGUID = ObjectGuid::Create<HighGuid::Player>(42);
    CHECK_FALSE(NpcAgentChat::SetAuthoredSenderName(packet, "Bela"));
    prepare(packet);
    packet.SlashCmd = CHAT_MSG_MONSTER_WHISPER;
    CHECK_FALSE(NpcAgentChat::SetAuthoredSenderName(packet, "Bela"));
}
