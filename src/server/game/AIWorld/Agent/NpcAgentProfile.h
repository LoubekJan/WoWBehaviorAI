/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTPROFILE_H
#define AIWORLD_NPCAGENTPROFILE_H

#include "AgentId.h"
#include "Define.h"
#include "NpcAgentCatalogLoading.h"
#include <functional>
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

// A profile is a request to use an existing agent, never authority to create
// one or to change its control/participation/scope. Pure seam shared by startup
// and regression tests for imported profiles targeting native civilians.
struct NpcAgentProfileBinding
{
    AgentId Agent;
    uint32 MapId = 0;
    uint64 SpawnId = 0;
    bool Controlled = false;
    bool InMembership = false;
    bool Participating = false;
    bool SupportedRole = false;
};

inline char const* NpcAgentProfileBindingFailure(NpcAgentProfile const& profile, NpcAgentProfileBinding const* binding)
{
    if (!binding) return "agent is not registered";
    if (profile.Agent != binding->Agent || profile.SpawnId != binding->SpawnId || profile.MapId != binding->MapId)
        return "agent identity or map mismatch";
    if (!binding->Controlled) return "agent is not AIWorld controlled";
    if (!binding->InMembership) return "agent is outside simulation membership";
    if (!binding->Participating) return "agent participation does not allow LLM control";
    if (!binding->SupportedRole) return "agent role has no supported action capabilities";
    return nullptr;
}

class TC_GAME_API NpcAgentProfileCatalog
{
public:
    static constexpr std::size_t MaxProfiles = 128;
    static constexpr std::size_t MaxProfileFileBytes = 16384;

    // Replaces the complete catalog only after every .json file passes.
    // Any I/O/schema/identity error clears the catalog. Diagnostics never
    // include file contents, paths, or filesystem exception text.
    bool Load(std::string const& directory);
    // Startup contains individual optional import errors. Only canonical
    // filenames and runtime-eligible identities enter the resulting catalog.
    using RuntimeValidator = std::function<char const*(NpcAgentProfile const&)>;
    NpcAgentCatalogIssues LoadRuntime(std::string const& directory, RuntimeValidator const& validator = {},
        std::size_t maxAccepted = MaxProfiles);
    NpcAgentProfile const* Get(AgentId id) const;
    std::size_t Size() const { return _profiles.size(); }
    std::string const& GetLastError() const { return _lastError; }

private:
    std::unordered_map<uint64, NpcAgentProfile> _profiles;
    std::string _lastError;
};

#endif // AIWORLD_NPCAGENTPROFILE_H
