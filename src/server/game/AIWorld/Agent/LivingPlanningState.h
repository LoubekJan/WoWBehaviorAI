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
    bool Fresh(uint64 now, uint64 progressAt, uint64 timeoutMs = 30000) const
    {
        if (Active || now < progressAt) return false;
        uint64 elapsed = now - progressAt;
        if (ProgressAnchor == progressAt)
        {
            if (now < EndedAt) return false;
            elapsed -= std::min(CompletedMs, elapsed);
        }
        return elapsed < timeoutMs;
    }
};

// One unfinished decision, not a persistent geometry cache. A budget yield
// retains the exact query context and the candidates already tested. Movement,
// a new home/danger/phase or a timeout starts a new search. Execution still
// validates the chosen route against the current world.
struct LivingPlanningContext
{
    bool Deferred = false;
    uint64 StartedAt = 0, ProgressAt = 0;
    ActionPosition Origin, Home;
    std::optional<ActionPosition> Danger;
    uint32 PhaseMask = 0, Capabilities = 0;
    uint32 ForageAttempts = 0;
    LivingPlanningCarePause CarePause;

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
        return Deferred && now >= StartedAt && CarePause.Fresh(now, ProgressAt) &&
            RecoveryMovement::SamePoint(Origin, here) && RouteContextMatches(home, danger, phaseMask, capabilities);
    }
    void Begin(uint64 now, ActionPosition const& here, ActionPosition const& home,
        std::optional<ActionPosition> danger, uint32 phaseMask, uint32 capabilities)
    {
        StartedAt = ProgressAt = now; Origin = here; Home = home; Danger = std::move(danger);
        PhaseMask = phaseMask; Capabilities = capabilities; Deferred = false;
        ForageAttempts = 0;
        CarePause = {};
    }
    bool PauseForCare(uint64 now)
    {
        if (now < StartedAt || (!CarePause.Active && !CarePause.Fresh(now, ProgressAt)) ||
            !CarePause.Begin(now, ProgressAt)) return false;
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
