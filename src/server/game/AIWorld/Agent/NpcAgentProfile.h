/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTPROFILE_H
#define AIWORLD_NPCAGENTPROFILE_H

#include "AgentId.h"
#include "Define.h"
#include <string>
#include <string_view>
#include <unordered_map>

struct NpcAgentProfile
{
    AgentId Agent;
    uint32 MapId = 0;
    uint64 SpawnId = 0;
    std::string Name;
    std::string Backstory;
    std::string Personality;
};

// Pure, strict six-field schema seam for tests and directory loading.
// No entry fallback: this profile belongs only to this stable individual.
TC_GAME_API bool ParseNpcAgentProfile(std::string_view json, NpcAgentProfile& profile);

class TC_GAME_API NpcAgentProfileCatalog
{
public:
    static constexpr std::size_t MaxProfiles = 128;
    static constexpr std::size_t MaxProfileFileBytes = 16384;

    // Replaces the complete catalog only after every .json file passes.
    // Any I/O/schema/identity error clears the catalog. Diagnostics never
    // include file contents, paths, or filesystem exception text.
    bool Load(std::string const& directory);
    NpcAgentProfile const* Get(AgentId id) const;
    std::size_t Size() const { return _profiles.size(); }
    std::string const& GetLastError() const { return _lastError; }

private:
    std::unordered_map<uint64, NpcAgentProfile> _profiles;
    std::string _lastError;
};

#endif // AIWORLD_NPCAGENTPROFILE_H
