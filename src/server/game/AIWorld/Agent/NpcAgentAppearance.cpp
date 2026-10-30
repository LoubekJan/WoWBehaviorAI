/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "NpcAgentAppearance.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

namespace
{
    constexpr std::array<std::string_view, std::size_t(NpcAppearanceSlot::Count)> Slots = {
        "head", "shoulder", "shirt", "chest", "waist", "legs", "feet", "wrist", "hands", "back", "tabard",
        "main_hand", "off_hand", "ranged"};

    bool SupportedRace(uint8 race)
    { return (race >= 1 && race <= 8) || race == 10 || race == 11; }

    class AppearanceParser
    {
    public:
        explicit AppearanceParser(std::string_view json) : _json(json) { }
        bool Parse(NpcAgentAppearance& result)
        {
            NpcAgentAppearance parsed;
            if (!Object(4, [&](std::string const& key)
            {
                uint64 value;
                if (key == "schema_version") return Number(value) && value == 1;
                if (key == "spawn_id") return Number(parsed.SpawnId) && parsed.SpawnId;
                if (key == "map_id")
                { if (!Number(value) || value > std::numeric_limits<uint32>::max()) return false; parsed.MapId = uint32(value); return true; }
                return key == "appearance" && Appearance(parsed);
            })) return false;
            Whitespace();
            if (_at != _json.size()) return false;
            result = std::move(parsed);
            return true;
        }
    private:
        void Whitespace()
        { while (_at < _json.size() && (_json[_at] == ' ' || _json[_at] == '\r' || _json[_at] == '\n' || _json[_at] == '\t')) ++_at; }
        bool Take(char c)
        { Whitespace(); if (_at == _json.size() || _json[_at] != c) return false; ++_at; return true; }
        bool Word(std::string& word)
        {
            if (!Take('"')) return false;
            std::size_t start = _at;
            while (_at < _json.size() && ((_json[_at] >= 'a' && _json[_at] <= 'z') || _json[_at] == '_')) ++_at;
            if (_at == start || _at - start > 32 || _at == _json.size() || _json[_at++] != '"') return false;
            word.assign(_json.substr(start, _at - start - 1));
            return true;
        }
        bool Number(uint64& value)
        {
            Whitespace();
            if (_at == _json.size() || _json[_at] < '0' || _json[_at] > '9') return false;
            bool zero = _json[_at] == '0';
            value = 0;
            std::size_t digits = 0;
            while (_at < _json.size() && _json[_at] >= '0' && _json[_at] <= '9')
            {
                uint64 digit = uint64(_json[_at++] - '0');
                if ((zero && digits) || value > (std::numeric_limits<uint64>::max() - digit) / 10) return false;
                value = value * 10 + digit;
                ++digits;
            }
            return true;
        }
        template<class Member> bool Object(std::size_t count, Member member)
        {
            if (!Take('{')) return false;
            std::unordered_set<std::string> seen;
            for (std::size_t i = 0; i < count; ++i)
            {
                if (i && !Take(',')) return false;
                std::string key;
                if (!Word(key) || !seen.insert(key).second || !Take(':') || !member(key)) return false;
            }
            return Take('}');
        }
        bool Appearance(NpcAgentAppearance& value)
        {
            if (!Object(9, [&](std::string const& key)
            {
                if (key == "equipment") return Equipment(value);
                uint8* field = nullptr;
                if (key == "race") field = &value.Race;
                else if (key == "gender") field = &value.Gender;
                else if (key == "class_id") field = &value.Class;
                else if (key == "skin") field = &value.Skin;
                else if (key == "face") field = &value.Face;
                else if (key == "hair_style") field = &value.HairStyle;
                else if (key == "hair_color") field = &value.HairColor;
                else if (key == "facial_hair") field = &value.FacialHair;
                uint64 number;
                if (!field || !Number(number) || number > 255) return false;
                *field = uint8(number); return true;
            })) return false;
            return SupportedRace(value.Race) && value.Gender <= 1 && value.Class == 1;
        }
        bool Equipment(NpcAgentAppearance& appearance)
        {
            if (!Take('[')) return false;
            if (Take(']')) return true;
            std::unordered_set<std::size_t> seen;
            for (std::size_t i = 0; i < Slots.size(); ++i)
            {
                if (i && !Take(',')) return false;
                std::string slot;
                NpcAppearanceItem item;
                if (!Object(3, [&](std::string const& key)
                {
                    if (key == "slot") return Word(slot);
                    uint64 number;
                    if ((key != "item_id" && key != "display_id") || !Number(number) || !number ||
                        number > std::numeric_limits<uint32>::max()) return false;
                    (key == "item_id" ? item.ItemId : item.DisplayId) = uint32(number);
                    return true;
                })) return false;
                std::size_t index = 0;
                while (index < Slots.size() && Slots[index] != slot) ++index;
                if (index == Slots.size() || !seen.insert(index).second) return false;
                appearance.Equipment[index] = item;
                if (Take(']')) return true;
            }
            return false;
        }
        std::string_view _json;
        std::size_t _at = 0;
    };
}

bool ParseNpcAgentAppearance(std::string_view json, NpcAgentAppearance& appearance)
{
    appearance = {};
    if (json.empty() || json.size() > NpcAgentAppearanceCatalog::MaxFileBytes) return false;
    return AppearanceParser(json).Parse(appearance);
}

bool NpcAppearanceSlotAccepts(NpcAppearanceSlot slot, uint32 type)
{
    switch (slot)
    {
        case NpcAppearanceSlot::Head: return type == 1;
        case NpcAppearanceSlot::Shoulder: return type == 3;
        case NpcAppearanceSlot::Shirt: return type == 4;
        case NpcAppearanceSlot::Chest: return type == 5 || type == 20;
        case NpcAppearanceSlot::Waist: return type == 6;
        case NpcAppearanceSlot::Legs: return type == 7;
        case NpcAppearanceSlot::Feet: return type == 8;
        case NpcAppearanceSlot::Wrist: return type == 9;
        case NpcAppearanceSlot::Hands: return type == 10;
        case NpcAppearanceSlot::Back: return type == 16;
        case NpcAppearanceSlot::Tabard: return type == 19;
        case NpcAppearanceSlot::MainHand: return type == 13 || type == 17 || type == 21;
        case NpcAppearanceSlot::OffHand: return type == 13 || type == 14 || type == 22 || type == 23;
        case NpcAppearanceSlot::Ranged: return type == 15 || type == 25 || type == 26 || type == 28;
        default: return false;
    }
}

bool NpcAgentAppearanceCatalog::Load(std::string const& directory)
{
    _appearances.clear();
    std::error_code error;
    bool exists = std::filesystem::exists(directory, error);
    if (error) return false;
    if (!exists) return true;
    auto entries = std::filesystem::directory_iterator(directory, error);
    if (error) return false;
    std::unordered_map<uint64, NpcAgentAppearance> loaded;
    std::size_t scanned = 0;
    for (auto const end = std::filesystem::directory_iterator(); entries != end; entries.increment(error))
    {
        if (error || ++scanned > MaxAppearances * 2) return false;
        if (entries->path().extension() != ".json") continue;
        auto status = entries->symlink_status(error);
        if (error || !std::filesystem::is_regular_file(status) || loaded.size() >= MaxAppearances) return false;
        auto size = entries->file_size(error);
        if (error || !size || size > MaxFileBytes) return false;
        std::ifstream input(entries->path(), std::ios::binary);
        if (!input) return false;
        std::string json(MaxFileBytes + 1, '\0');
        input.read(json.data(), std::streamsize(json.size()));
        std::size_t bytes = std::size_t(input.gcount());
        if (input.bad() || !bytes || bytes > MaxFileBytes) return false;
        json.resize(bytes);
        NpcAgentAppearance appearance;
        if (!ParseNpcAgentAppearance(json, appearance) || entries->path().stem().string() != std::to_string(appearance.SpawnId)) return false;
        if (!loaded.emplace(appearance.SpawnId, std::move(appearance)).second) return false;
    }
    if (error) return false;
    _appearances = std::move(loaded);
    return true;
}

NpcAgentAppearance const* NpcAgentAppearanceCatalog::Get(uint64 spawnId) const
{
    auto found = _appearances.find(spawnId);
    return found == _appearances.end() ? nullptr : &found->second;
}

// Keep the pure wire codec with the parser/catalog translation unit. Tests
// can link it without pulling the Creature/Player integration and its stores.
bool BuildNpcAgentMirrorImageData(WorldPacket& packet, ObjectGuid guid, uint32 baseDisplay,
    NpcAgentAppearance const& appearance)
{
    if (!guid.IsCreatureOrVehicle() || !baseDisplay || appearance.Gender > 1 || appearance.Class != 1 ||
        !((appearance.Race >= 1 && appearance.Race <= 8) || appearance.Race == 10 || appearance.Race == 11)) return false;
    packet.Initialize(SMSG_MIRRORIMAGE_DATA, 68);
    packet << guid << uint32(baseDisplay) << uint8(appearance.Race) << uint8(appearance.Gender) << uint8(appearance.Class);
    packet << uint8(appearance.Skin) << uint8(appearance.Face) << uint8(appearance.HairStyle)
           << uint8(appearance.HairColor) << uint8(appearance.FacialHair) << uint32(0); // no guild crest
    for (std::size_t slot = 0; slot < std::size_t(NpcAppearanceSlot::MainHand); ++slot)
        packet << uint32(appearance.Equipment[slot].DisplayId);
    return true;
}
