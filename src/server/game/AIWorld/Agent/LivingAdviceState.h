/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGADVICESTATE_H
#define AIWORLD_LIVINGADVICESTATE_H
#include "Inference/RecoveryAdvice.h"
#include "LivingReturnPolicy.h"
#include "Action/RecoveryMovement.h"

struct LivingAdviceCandidate
{
    RecoveryAdviceOption Option;
    RecoveryMovement Move;
    LivingReturnPolicy::Diagnostics Diagnostics;
    bool Backtrack = false;
};
struct LivingAdviceState
{
    uint64 LifetimeAt = 0;
    uint64 PendingId = 0, RequestedAt = 0, CooldownUntil = 0, Episode = 0;
    bool Returning = false, Responded = false;
    ActionPosition Origin, Home;
    std::vector<LivingAdviceCandidate> Candidates;
    std::optional<uint32> Choice;
    uint32 Requests = 0, Selected = 0, Started = 0, Arrived = 0, HomeSuccess = 0,
        FoodSuccess = 0, Rejected = 0, Unavailable = 0, Reused = 0;
    std::string Status = "IDLE";
    std::optional<LivingAdviceCandidate> Active;
    ActionPosition ActiveOrigin;
    uint64 ActiveAt = 0;
    float ActiveHunger = 0;
    bool ActiveReturning = false, StepArrived = false;
    struct Memory { ActionPosition From; LivingAdviceCandidate Candidate; };
    std::vector<Memory> Successful;
    std::vector<ActionPosition> Searched;

    bool Fresh(uint64 now, ActionPosition const& here, ActionPosition const& home) const
    {
        return PendingId && now >= RequestedAt && now - RequestedAt <= 30000 &&
            here.MapId == Origin.MapId && LivingReturnPolicy::Distance(here, Origin) <= 2.0f &&
            RecoveryMovement::SamePoint(home, Home);
    }
    void ClearPending() { PendingId = 0; Responded = false; Choice.reset(); Candidates.clear(); }
    void RememberSuccess()
    {
        if (!Active) return;
        if (Successful.size() == 8) Successful.erase(Successful.begin());
        Successful.push_back({ActiveOrigin, *Active});
        Active.reset();
    }
};
#endif
