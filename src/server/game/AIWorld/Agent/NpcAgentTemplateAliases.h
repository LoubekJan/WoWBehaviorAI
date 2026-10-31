/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTTEMPLATEALIASES_H
#define AIWORLD_NPCAGENTTEMPLATEALIASES_H

#include "Define.h"
#include "NpcAgentDialogue.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

struct NpcAgentTemplateAlias
{
    uint64 SpawnId = 0;
    uint32 MapId = 0, Entry = 0, BaseEntry = 0;
    std::string Name, Title;
};

inline uint32 NpcAgentTemplateEntry(uint64 spawn)
{
    return spawn >= 901000 && spawn <= 901099 ? uint32(9800000 + spawn - 901000) : 0;
}

struct NpcAgentTemplateBinding
{
    uint64 AgentId = 0;
    uint32 MapId = 0, ActualEntry = 0;
    bool Controlled = false, InMembership = false, Participating = false;
    // The offline operator preflight verifies every native column/resource
    // against the reviewed base. Startup also verifies the native core fields.
    bool NativeTemplatesMatch = false, KnownSource = false;
};

inline char const* NpcAgentTemplateFailure(NpcAgentTemplateAlias const& alias, NpcAgentTemplateBinding const& binding)
{
    uint32 expected = NpcAgentTemplateEntry(alias.SpawnId);
    if (!expected || alias.MapId != 725 || alias.Entry != expected || !alias.BaseEntry ||
        alias.BaseEntry >= 9800000) return "invalid reserved character template identity";
    if (binding.AgentId != alias.SpawnId || binding.MapId != alias.MapId || binding.ActualEntry != alias.Entry)
        return "character template spawn binding mismatch";
    if (!binding.Controlled || !binding.InMembership || !binding.Participating)
        return "character template actor is not a participating controlled member";
    if (!binding.KnownSource || !binding.NativeTemplatesMatch) return "character template source or native fields mismatch";
    if (!ValidateNpcAgentPlainText(alias.Name, 80, 32) ||
        (!alias.Title.empty() && !ValidateNpcAgentPlainText(alias.Title, 80, 80)))
        return "invalid character template label";
    return nullptr;
}

class NpcAgentTemplateAliases
{
public:
    void Clear() { _aliases.clear(); }
    bool Add(NpcAgentTemplateAlias alias, NpcAgentTemplateBinding const& binding)
    {
        if (NpcAgentTemplateFailure(alias, binding)) return false;
        return _aliases.emplace(alias.SpawnId, std::move(alias)).second;
    }
    NpcAgentTemplateAlias const* Find(uint32 map, uint64 spawn, uint32 entry) const
    {
        auto it = _aliases.find(spawn);
        return it != _aliases.end() && it->second.MapId == map && it->second.Entry == entry ? &it->second : nullptr;
    }
    uint32 Resolve(uint32 map, uint64 spawn, uint32 entry) const
    {
        auto const* alias = Find(map, spawn, entry);
        return alias ? alias->BaseEntry : entry;
    }
    char const* ProfileFailure(uint32 map, uint64 spawn, uint32 entry, std::string_view name) const
    {
        auto const* alias = Find(map, spawn, entry);
        if (entry >= 9800000 && entry <= 9800099 && !alias) return "native character template binding is invalid";
        if (alias && alias->Name != name) return "card name differs from installed native template";
        return nullptr;
    }
    std::size_t Size() const { return _aliases.size(); }
    uint32 ClientCacheVersion(uint32 baseVersion) const
    {
        if (_aliases.empty()) return baseVersion;
        std::vector<uint64> ids;
        for (auto const& pair : _aliases) ids.push_back(pair.first);
        std::sort(ids.begin(), ids.end());
        uint32 hash = 2166136261u;
        auto byte = [&](uint8 value) { hash ^= value; hash *= 16777619u; };
        auto number = [&](uint64 value) { for (uint32 i = 0; i < 8; ++i) byte(uint8(value >> (8 * i))); };
        number(baseVersion);
        for (uint64 id : ids)
        {
            auto const& alias = _aliases.at(id);
            number(id); number(alias.Entry); number(alias.BaseEntry);
            for (uint8 value : alias.Name) byte(value);
            byte(0);
            for (uint8 value : alias.Title) byte(value);
            byte(0);
        }
        hash &= 0x7fffffffu;
        return hash ? hash : 1;
    }
private:
    std::unordered_map<uint64, NpcAgentTemplateAlias> _aliases;
};
#endif
