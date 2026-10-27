/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTRUNTIME_H
#define AIWORLD_NPCAGENTRUNTIME_H
#include "Action/ActionPosition.h"
#include "Inference/NpcAgent.h"
#include "NpcAgentDialogue.h"
#include <optional>
#include <string_view>
#include <unordered_map>

// World-thread owned. The model receives tokens, never these entity bindings.
struct NpcAgentActionBinding
{
    std::string Type;
    ActionPosition Destination;
    uint32 Depot = 0;
};
struct NpcAgentRecipientBinding
{
    ObjectGuid Guid;
    bool Player = false;
};
struct NpcAgentRuntime
{
    ObjectGuid Guid;
    uint64 Revision = 1, Sequence = 0, NextThinkAt = 0, RequestAt = 0;
    uint64 RequestId = 0, ActionAt = 0, ActionUntil = 0;
    uint64 Conversation = 0;
    uint32 Hop = 0, Failures = 0, MealDepot = 0;
    std::string Status = "READY", LastResult = "NONE", Active = "NONE";
    std::string LastChoice;
    std::optional<NpcAgentRequest> Pending;
    std::unordered_map<uint32, NpcAgentActionBinding> Actions;
    std::unordered_map<uint32, NpcAgentRecipientBinding> Recipients;
    NpcAgentDialogueHistory Dialogue;

    void InvalidatePending()
    {
        ++Revision;
        RequestId = RequestAt = 0;
        Pending.reset(); Actions.clear(); Recipients.clear();
    }
};

// A pending slot remains occupied when new input advances Revision. Its old
// reply must terminate that slot, but cannot act on the newer context or a
// replacement Creature. All identities here are local echoes, never wire GUIDs.
inline bool NpcAgentReplyIsFresh(NpcAgentRuntime const& runtime, uint64 requestId,
    ObjectGuid actualGuid, ObjectGuid echoedGuid, uint64 echoedRevision, uint64 nowMs, uint64 maxAgeMs)
{
    return requestId && runtime.Pending && runtime.RequestId == requestId &&
        runtime.Pending->RequestId == requestId && !actualGuid.IsEmpty() && !runtime.Guid.IsEmpty() &&
        !echoedGuid.IsEmpty() && actualGuid == runtime.Guid && echoedGuid == runtime.Guid &&
        runtime.Revision == echoedRevision && nowMs >= runtime.RequestAt &&
        nowMs - runtime.RequestAt <= maxAgeMs;
}

// A proposed animation is not a completed meal or rest. The caller supplies
// freshly checked goal/action ownership and the actual engine posture. Resource
// consumption and need effects stay with the world-thread completion handler.
inline bool NpcAgentActivityCanComplete(std::string_view kind, uint64 startedAtMs, uint64 nowMs,
    bool alive, bool inCombat, bool stopped, bool postureMatches, bool owned)
{
    if (!startedAtMs || nowMs < startedAtMs || !alive || inCombat || !stopped || !postureMatches || !owned)
        return false;
    uint64 minimumMs;
    if (kind == "EAT") minimumMs = 5000;
    else if (kind == "REST") minimumMs = 20000;
    else if (kind == "WAIT") minimumMs = 1000;
    else return false;
    return nowMs - startedAtMs >= minimumMs;
}

struct NpcAgentPlayerSpeech
{
    ObjectGuid Player, Target;
    uint32 Map = 0, Language = 0;
    std::string Text;
};
#endif
