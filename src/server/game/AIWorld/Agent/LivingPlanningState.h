/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGPLANNINGSTATE_H
#define AIWORLD_LIVINGPLANNINGSTATE_H
#include "Action/RecoveryMovement.h"
#include "LivingReturnPolicy.h"
#include "ObjectGuid.h"

// Only an explicit stationary care action suspends the inactivity clock.
// ProgressAt remains the time of real query work. A later query starts a new
// accounting epoch, so old care cannot extend that new query's deadline.
struct LivingPlanningCarePause
{
    bool Active = false;
    uint64 StartedAt = 0, EndedAt = 0, ProgressAnchor = 0, CompletedMs = 0;

    bool Begin(uint64 now, uint64 progressAt)
    {
        if (now < progressAt) return false;
        if (Active) return ProgressAnchor == progressAt && now >= StartedAt;
        if (ProgressAnchor != progressAt)
        { CompletedMs = EndedAt = 0; ProgressAnchor = progressAt; }
        if (now < EndedAt) return false;
        StartedAt = now; Active = true;
        return true;
    }
    bool End(uint64 now, uint64 progressAt)
    {
        if (!Active) return true;
        if (now < StartedAt || now < progressAt)
        { *this = {}; return false; }
        if (ProgressAnchor == progressAt) CompletedMs += now - StartedAt;
        else { CompletedMs = 0; ProgressAnchor = progressAt; }
        EndedAt = now; Active = false;
        return true;
    }
    std::optional<uint64> PausedMs(uint64 now, uint64 progressAt, bool allowActive = false) const
    {
        if ((Active && !allowActive) || now < progressAt) return std::nullopt;
        uint64 paused = 0;
        if (ProgressAnchor == progressAt)
        {
            if (now < EndedAt || (Active && now < StartedAt)) return std::nullopt;
            paused = CompletedMs;
            if (Active) paused += now - StartedAt;
        }
        return std::min(paused, now - progressAt);
    }
    bool Fresh(uint64 now, uint64 progressAt, uint64 timeoutMs = 30000) const
    {
        auto paused = PausedMs(now, progressAt);
        return paused && now - progressAt - *paused < timeoutMs;
    }
};

// Waiting for a cooperative planning turn is not query inactivity. Do not
// invent progress timestamps: keep real work and administrative waits separate.
inline bool LivingPlanningFresh(uint64 now, uint64 progressAt, LivingPlanningCarePause const& care,
    LivingPlanningCarePause const& waiting, uint64 timeoutMs = 30000)
{
    auto careMs = care.PausedMs(now, progressAt);
    auto waitMs = waiting.PausedMs(now, progressAt, true);
    if (!careMs || !waitMs) return false;
    uint64 elapsed = now - progressAt;
    uint64 paused = std::min(*careMs, elapsed) + std::min(*waitMs, elapsed - std::min(*careMs, elapsed));
    return elapsed - paused < timeoutMs;
}

// One unfinished decision, not a persistent geometry cache. A budget yield
// retains the exact query context and the candidates already tested. Movement,
// a new home/danger/phase or a timeout starts a new search. Execution still
// validates the chosen route against the current world.
struct LivingPlanningContext
{
    bool Deferred = false;
    uint64 StartedAt = 0, ProgressAt = 0;
    uint64 DeferredAt = 0;
    uint32 Resets = 0;
    char const* Reason = "NONE";
    char const* Stage = "NONE";
    ActionPosition Origin, Home;
    std::optional<ActionPosition> Danger;
    uint32 PhaseMask = 0, Capabilities = 0;
    uint32 ForageAttempts = 0, RefugeAttempts = 0;
    bool HuntScanned = false, CohesionChecked = false;
    LivingPlanningCarePause CarePause;
    LivingPlanningCarePause BudgetPause;

    bool RouteContextMatches(ActionPosition const& home, std::optional<ActionPosition> const& danger,
        uint32 phaseMask, uint32 capabilities) const
    {
        return RecoveryMovement::SamePoint(Home, home) && Danger.has_value() == danger.has_value() &&
            (!Danger || RecoveryMovement::SamePoint(*Danger, *danger)) &&
            PhaseMask == phaseMask && Capabilities == capabilities;
    }
    bool Resume(uint64 now, ActionPosition const& here, ActionPosition const& home,
        std::optional<ActionPosition> const& danger, uint32 phaseMask, uint32 capabilities) const
    {
        return Deferred && now >= StartedAt && LivingPlanningFresh(now, ProgressAt, CarePause, BudgetPause) &&
            RecoveryMovement::SamePoint(Origin, here) && RouteContextMatches(home, danger, phaseMask, capabilities);
    }
    void Begin(uint64 now, ActionPosition const& here, ActionPosition const& home,
        std::optional<ActionPosition> danger, uint32 phaseMask, uint32 capabilities)
    {
        if (StartedAt && Deferred) ++Resets;
        StartedAt = ProgressAt = now; Origin = here; Home = home; Danger = std::move(danger);
        PhaseMask = phaseMask; Capabilities = capabilities; Deferred = false;
        DeferredAt = 0; Reason = "NONE"; Stage = "DECISION";
        ForageAttempts = RefugeAttempts = 0; HuntScanned = CohesionChecked = false;
        CarePause = {};
        BudgetPause = {};
    }
    void MarkDeferred(uint64 now, char const* reason, char const* stage)
    {
        if (!DeferredAt) DeferredAt = now;
        Deferred = true; Reason = reason; Stage = stage;
        BudgetPause.Begin(now, ProgressAt);
    }
    void ClearWait()
    {
        Deferred = false; DeferredAt = 0; Reason = "NONE";
        BudgetPause = {};
    }
    void MarkProgress(uint64 now)
    {
        ProgressAt = now;
        ClearWait();
    }
    void ResumeWork(uint64 now)
    {
        BudgetPause.End(now, ProgressAt);
        Deferred = false;
    }
    bool PauseForCare(uint64 now)
    {
        if (now < StartedAt || (!CarePause.Active && !LivingPlanningFresh(now, ProgressAt, CarePause, BudgetPause)) ||
            !CarePause.Begin(now, ProgressAt)) return false;
        BudgetPause.End(now, ProgressAt);
        Deferred = true;
        return true;
    }
    bool ResumeAfterCare(uint64 now)
    {
        if (now < StartedAt || !CarePause.End(now, ProgressAt))
        { CarePause = {}; Deferred = false; return false; }
        return true;
    }
};

struct LivingReturnSearch
{
    enum class Stage : uint8 { Corridor, Home, Trail, Direct, Detour, Rejoin, Backtrack, Done };
    struct Trial { ActionPosition Destination; bool RoutePoint = false; };
    Stage Current = Stage::Corridor;
    bool Started = false;
    std::size_t Next = 0;
    std::vector<Trial> Trials;
    LivingReturnPolicy::HomeCorridorSearch Home, Continuation;
    LivingReturnPolicy::RejoinSearch Probes;
    bool ConnectorReady = false;
    std::optional<ActionPosition> Connector;
    std::optional<ActionPosition> Backtrack, BacktrackTrail;
    LivingReturnPolicy::Diagnostics BacktrackDiagnostics;

    void Advance(Stage next)
    { Current = next; Next = 0; Trials.clear(); ConnectorReady = false; Connector.reset(); }
};

struct LivingHuntProbe
{
    ObjectGuid Prey;
    ActionPosition Position;
};
#endif
