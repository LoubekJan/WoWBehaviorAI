/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTTELEMETRY_H
#define AIWORLD_NPCAGENTTELEMETRY_H

#include "Define.h"
#include <string>

// Owned public runtime observations only. The observer receives neither
// authored backstory/personality nor model configuration or credentials.
struct NpcAgentTelemetry
{
    std::string Name;
    std::string Status;
    std::string LastActionResult;
    std::string ActiveAction;
    uint64 RequestId = 0;
    uint64 NextThinkMs = 0; // remaining delay, not an absolute timestamp
    uint32 DialogueCount = 0;
    std::string LastSpeech;
};

#endif // AIWORLD_NPCAGENTTELEMETRY_H
