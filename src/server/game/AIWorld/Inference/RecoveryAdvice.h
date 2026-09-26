/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_RECOVERYADVICE_H
#define AIWORLD_RECOVERYADVICE_H
#include "Agent/AgentId.h"
#include "Define.h"
#include <string>
#include <string_view>
#include <vector>

// The model chooses a request-local token. It cannot create coordinates,
// commands, rewards, or permissions. Geometry and bindings stay server-owned.
struct RecoveryAdviceOption
{
    uint32 Token = 0;
    std::string Strategy;
    float X = 0, Y = 0, Z = 0, Distance = 0, HomeGain = 0;
    uint32 NearbyPrey = 0, Visits = 0, Successes = 0;
};
struct RecoveryAdviceRequest
{
    uint64 RequestId = 0, Episode = 0;
    AgentId Agent;
    std::string Role, Problem, Failure;
    float Hunger = 0, HomeDistance = 0;
    uint32 Failures = 0;
    uint64 StalledMs = 0;
    std::vector<RecoveryAdviceOption> Options;
};
struct RecoveryAdviceResponse
{
    uint64 RequestId = 0, Episode = 0;
    AgentId Agent;
    uint32 Token = 0; // zero explicitly declines all offered options
};
std::string SerializeRecoveryAdvice(RecoveryAdviceRequest const& request);
bool ParseRecoveryAdvice(std::string_view json, RecoveryAdviceResponse& response);
bool MatchesRecoveryAdvice(RecoveryAdviceRequest const& request, RecoveryAdviceResponse const& response);
#endif
