/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/NpcAgentAppearance.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "WorldPacket.h"

TEST_CASE("Authored humanoid appearance serializes the exact 3.3.5 mirror-image packet", "[AIWorld][NpcAgent][Appearance][Packet]")
{
    NpcAgentAppearance appearance;
    appearance.Race = 1; appearance.Gender = 1; appearance.Class = 1;
    appearance.Skin = 2; appearance.Face = 3; appearance.HairStyle = 4; appearance.HairColor = 5; appearance.FacialHair = 6;
    for (std::size_t slot = 0; slot < appearance.Equipment.size(); ++slot)
        appearance.Equipment[slot] = {uint32(2000 + slot), uint32(1000 + slot)};
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Unit>(11328, 42);
    WorldPacket packet;
    REQUIRE(BuildNpcAgentMirrorImageData(packet, guid, 50, appearance));
    CHECK(packet.GetOpcode() == SMSG_MIRRORIMAGE_DATA);
    CHECK(packet.size() == 68);
    ObjectGuid wireGuid;
    packet >> wireGuid;
    CHECK(wireGuid == guid);
    CHECK(wireGuid.GetEntry() == 11328);
    CHECK(packet.read<uint32>() == 50);
    CHECK(packet.read<uint8>() == 1);
    CHECK(packet.read<uint8>() == 1);
    CHECK(packet.read<uint8>() == 1);
    CHECK(packet.read<uint8>() == 2);
    CHECK(packet.read<uint8>() == 3);
    CHECK(packet.read<uint8>() == 4);
    CHECK(packet.read<uint8>() == 5);
    CHECK(packet.read<uint8>() == 6);
    CHECK(packet.read<uint32>() == 0);
    for (std::size_t slot = 0; slot < 11; ++slot)
        CHECK(packet.read<uint32>() == uint32(1000 + slot));
    CHECK(packet.rpos() == packet.size()); // hand weapons never enter clothing bytes
}

TEST_CASE("Mirror appearance packet rejects non-creatures and invalid humanoid headers", "[AIWorld][NpcAgent][Appearance][Packet]")
{
    NpcAgentAppearance appearance;
    WorldPacket packet;
    CHECK_FALSE(BuildNpcAgentMirrorImageData(packet, ObjectGuid::Create<HighGuid::Player>(42), 49, appearance));
    CHECK(packet.empty());
    auto guid = ObjectGuid::Create<HighGuid::Unit>(11328, 42);
    CHECK_FALSE(BuildNpcAgentMirrorImageData(packet, guid, 0, appearance));
    appearance.Race = 9;
    CHECK_FALSE(BuildNpcAgentMirrorImageData(packet, guid, 49, appearance));
    appearance.Race = 1; appearance.Gender = 2;
    CHECK_FALSE(BuildNpcAgentMirrorImageData(packet, guid, 49, appearance));
    CHECK(packet.empty());
}
