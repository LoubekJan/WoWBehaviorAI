/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGPLANNINGSTATE_H
#define AIWORLD_LIVINGPLANNINGSTATE_H
#include "Action/RecoveryMovement.h"
#include "LivingReturnPolicy.h"
#include "LivingForageGround.h"
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
    bool ForageAttemptStarted = false;
    LivingForageGroundSearch ForageGround;
    std::optional<ActionPosition> ForageStep;
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
        ForageAttemptStarted = false; ForageGround = {}; ForageStep.reset();
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

// A foreground home graph is allowed one wall-time slice for this return
// decision. Query progress, denied admission and stationary care never renew
// the slice. Expiry advances to other return strategies. A later decision may
// retain its bounded graph work when the physical request is still identical.
struct LivingReturnHomeDetourDeadline
{
    static constexpr uint64 WallLimitMs = 30000;
    bool Started = false, Expired = false;
    uint64 StartedAt = 0, LastAt = 0;

    bool Allow(uint64 now)
    {
        if (Expired) return false;
        if (!Started)
        { Started = true; StartedAt = LastAt = now; return true; }
        if (now < LastAt || now - StartedAt >= WallLimitMs)
        { Expired = true; return false; }
        LastAt = now;
        return true;
    }
};

struct LivingReturnSearch
{
    enum class Stage : uint8 { Corridor, Trail, Direct, Home, SurfaceDetour, Detour, Rejoin, Backtrack, Done };
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
    LivingReturnHomeDetourDeadline HomeDetourDeadline;

    bool HasPendingHomeDetour() const
    { return Home.HasContext && Home.Detour.Started && Home.Detour.State == LivingSurfaceCorridor::Status::Pending; }

    // Stationary recovery care ends the decision but not the return episode.
    // Keep only the graph and the old clock guard until the next decision
    // checks its full context. Moving, hunting and safety actions discard it.
    void InterruptForAction(bool stationaryReturnCare)
    {
        auto retained = stationaryReturnCare && HasPendingHomeDetour() ?
            std::move(Home) : LivingReturnPolicy::HomeCorridorSearch{};
        auto deadline = HomeDetourDeadline;
        *this = {};
        Home = std::move(retained);
        if (Home.HasContext) HomeDetourDeadline = deadline;
    }

    // A crowded planning scheduler may admit only a handful of graph edges
    // within a slice. Retrying those first edges forever cannot find a route.
    // Keep unfinished graph work across ordinary failed-return retries, while
    // starting the cheap alternatives and their deadlines afresh. Every
    // execution leg is still revalidated against the live world by the caller.
    bool RestartForDecision(LivingPlanningContext const& previous, uint64 now,
        ActionPosition const& here, ActionPosition const& home, std::optional<ActionPosition> const& danger,
        uint32 phaseMask, uint32 capabilities, bool returning)
    {
        bool retain = returning && HasPendingHomeDetour() &&
            now >= previous.StartedAt && now >= previous.ProgressAt &&
            (!HomeDetourDeadline.Started || now >= HomeDetourDeadline.LastAt) &&
            RecoveryMovement::SamePoint(previous.Origin, here) &&
            previous.RouteContextMatches(home, danger, phaseMask, capabilities) &&
            LivingReturnPolicy::SamePosition(Home.From, here) &&
            LivingReturnPolicy::SamePosition(Home.Home, home) &&
            Home.Danger.has_value() == danger.has_value() &&
            (!danger || LivingReturnPolicy::SamePosition(*Home.Danger, *danger));
        auto retained = retain ? std::move(Home) : LivingReturnPolicy::HomeCorridorSearch{};
        *this = {};
        Home = std::move(retained);
        return retain;
    }

    bool AllowHomeDetour(uint64 now) { return HomeDetourDeadline.Allow(now); }
    // Completed proofs may await execution validation past the search window.
    // The wall deadline limits unfinished graph work, not that retained proof.
    bool CanContinueHomeDetour(uint64 now)
    { return Home.Detour.State != LivingSurfaceCorridor::Status::Pending || AllowHomeDetour(now); }

    static constexpr Stage NextFallback(Stage stage)
    {
        switch (stage)
        {
            case Stage::Corridor: return Stage::Trail;
            case Stage::Trail: return Stage::Direct;
            case Stage::Direct: return Stage::Home;
            case Stage::Home: return Stage::SurfaceDetour;
            case Stage::SurfaceDetour: return Stage::Detour;
            case Stage::Detour: return Stage::Rejoin;
            case Stage::Rejoin: return Stage::Backtrack;
            case Stage::Backtrack: return Stage::Done;
            case Stage::Done: return Stage::Done;
        }
        return Stage::Done;
    }

    void Advance(Stage next)
    { Current = next; Next = 0; Trials.clear(); ConnectorReady = false; Connector.reset(); }

    // A stage owns its query cursor. Refused work retains that cursor, while a
    // completed route may explicitly return to Corridor for leg validation.
    // Ordinary unsuccessful stages follow one shared fallback order.
    template <typename TryStage, typename IsDeferred>
    std::optional<ActionPosition> Continue(TryStage&& tryStage, IsDeferred&& isDeferred)
    {
        while (Current != Stage::Done)
        {
            Stage stage = Current;
            if (auto step = tryStage(stage)) return step;
            if (isDeferred()) return std::nullopt;
            if (Current == stage) Advance(NextFallback(stage));
        }
        return std::nullopt;
    }
};

struct LivingHuntProbe
{
    ObjectGuid Prey;
    ActionPosition Position;
};
#endif
