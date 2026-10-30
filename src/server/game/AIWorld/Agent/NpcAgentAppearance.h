/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTAPPEARANCE_H
#define AIWORLD_NPCAGENTAPPEARANCE_H

#include "Define.h"
#include "NpcAgentCatalogLoading.h"
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

class Creature;
class ObjectGuid;
class WorldPacket;

// The first eleven slots are the exact 3.3.5 SMSG_MIRRORIMAGE_DATA order.
enum class NpcAppearanceSlot : uint8
{
    Head, Shoulder, Shirt, Chest, Waist, Legs, Feet, Wrist, Hands, Back, Tabard,
    MainHand, OffHand, Ranged, Count
};

struct NpcAppearanceItem
{
    uint32 ItemId = 0;
    uint32 DisplayId = 0;
};

struct NpcAgentAppearance
{
    uint64 SpawnId = 0;
    uint32 MapId = 0;
    uint8 Race = 1, Gender = 0, Class = 1;
    uint8 Skin = 0, Face = 0, HairStyle = 0, HairColor = 0, FacialHair = 0;
    std::array<NpcAppearanceItem, std::size_t(NpcAppearanceSlot::Count)> Equipment{};
};

TC_GAME_API bool ParseNpcAgentAppearance(std::string_view json, NpcAgentAppearance& appearance);
TC_GAME_API bool NpcAppearanceSlotAccepts(NpcAppearanceSlot slot, uint32 inventoryType);

class TC_GAME_API NpcAgentAppearanceCatalog
{
public:
    static constexpr std::size_t MaxAppearances = 128; // same bounded catalog capacity as individual profiles
    static constexpr std::size_t MaxFileBytes = 8192;
    // An absent/empty optional directory is valid. Any malformed authored file
    // clears the entire appearance catalog; no partial appearance is applied.
    bool Load(std::string const& directory);
    using RuntimeValidator = std::function<char const*(NpcAgentAppearance const&)>;
    NpcAgentCatalogIssues LoadRuntime(std::string const& directory, RuntimeValidator const& validator = {});
    void Clear() { _appearances.clear(); }
    NpcAgentAppearance const* Get(uint64 spawnId) const;
    auto const& Entries() const { return _appearances; }
    std::size_t Size() const { return _appearances.size(); }
private:
    std::unordered_map<uint64, NpcAgentAppearance> _appearances;
};

// Native DBC checks happen after all server data stores are loaded. They check
// the same customization tuples as character creation, and actual item/display
// and inventory-slot mappings; no client-supplied or model-supplied IDs enter here.
TC_GAME_API bool ValidateNativeNpcAgentAppearance(NpcAgentAppearance const& appearance);
TC_GAME_API uint32 NativeNpcAgentAppearanceDisplay(NpcAgentAppearance const& appearance);
TC_GAME_API bool ApplyNpcAgentAppearance(Creature& creature, NpcAgentAppearance const& appearance);
// Packet seam: preserves the real individual GUID and uses only the eleven
// clothing displays. Weapons use UNIT_VIRTUAL_ITEM_SLOT_ID with item IDs.
TC_GAME_API bool BuildNpcAgentMirrorImageData(WorldPacket& packet, ObjectGuid guid,
    uint32 baseDisplay, NpcAgentAppearance const& appearance);

#endif // AIWORLD_NPCAGENTAPPEARANCE_H
