/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENT_H
#define AIWORLD_NPCAGENT_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Needs/NeedsState.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

struct NpcAgentProfileContext
{
    std::string Name, Backstory, Personality;
};

struct NpcAgentSelf
{
    uint32 MapId = 0;
    float X = 0, Y = 0, Z = 0, Orientation = 0;
    uint32 Health = 0, MaxHealth = 0;
    bool Alive = false, InCombat = false;
    std::string CurrentAction, LastActionResult;
};

struct NpcAgentDialogue
{
    std::string Speaker, Text, Kind; // PLAYER, NPC or SELF
};

struct NpcAgentNearby
{
    uint32 Token = 0;
    std::string Name, Kind; // PLAYER or NPC
    float Distance = 0;
};

struct NpcAgentAction
{
    uint32 Token = 0;
    std::string Type, Description; // WAIT, MOVE_TO, EAT, REST, STOP or FLEE
};

struct NpcAgentContext
{
    NpcAgentProfileContext Profile;
    NeedsState Needs;
    NpcAgentSelf Self;
    std::vector<NpcAgentDialogue> RecentDialogue;
    std::vector<NpcAgentNearby> Nearby;
    std::vector<NpcAgentAction> Actions;
    std::vector<std::string> Emotes;
    std::vector<uint32> SpeechTargets;
};

struct NpcAgentRequest
{
    uint32 ProtocolVersion = 1;
    uint64 RequestId = 0;
    AgentId Agent;
    uint64 SnapshotSequence = 0;
    NpcAgentContext Context;
};

struct NpcAgentDecision
{
    uint32 ActionToken = 0;
    std::string Speech;
    uint32 SpeechTarget = 0;
    std::string Emote = "NONE";
};

struct NpcAgentResponse
{
    uint32 ProtocolVersion = 1;
    uint64 RequestId = 0;
    AgentId Agent;
    uint64 SnapshotSequence = 0;
    NpcAgentDecision Decision;
};

// Local request provenance, never serialized or accepted from the provider.
struct NpcAgentProvenance
{
    ObjectGuid RuntimeGuid;
    uint64 ContextRevision = 0;
};

#endif
