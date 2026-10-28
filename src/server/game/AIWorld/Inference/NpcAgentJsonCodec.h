/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTJSONCODEC_H
#define AIWORLD_NPCAGENTJSONCODEC_H

#include "NpcAgent.h"
#include <cstddef>
#include <string_view>

constexpr std::size_t NpcAgentRequestBodyLimit = 32768;
constexpr std::size_t NpcAgentResponseBodyLimit = 8192;

// An empty result means the typed request violates the bounded wire contract.
std::string SerializeNpcAgentRequest(NpcAgentRequest const& request);
// Strict schema, escaped Unicode strings, no duplicate/extra/missing fields.
// Transactional: response is unchanged on rejection.
bool ParseNpcAgentResponse(std::string_view json, NpcAgentResponse& response);
// Checks exact envelope plus request-local action, speech recipient and emote grants.
bool MatchesNpcAgentResponse(NpcAgentRequest const& request, NpcAgentResponse const& response);

#endif
