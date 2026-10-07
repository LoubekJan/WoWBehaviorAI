/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGADVICESTATE_H
#define AIWORLD_LIVINGADVICESTATE_H
#include "Inference/RecoveryAdvice.h"
#include "LivingReturnPolicy.h"
#include "Action/RecoveryMovement.h"
#include "LivingFoodMemory.h"
#include "LivingPlanningState.h"
#include "LivingAdviceAdmission.h"

struct LivingAdviceCandidate
{
    RecoveryAdviceOption Option;
    RecoveryMovement Move;
    LivingReturnPolicy::Diagnostics Diagnostics;
    bool Backtrack = false;
    bool FollowsCorridor = false;
    bool CorridorSeed = false;
    std::vector<ActionPosition> Continuation;
    uint32 ProofPhaseMask = 0, ProofCapabilities = 0;
    float ProofArrivalRadius = 0;
    bool HasProofContext = false;

    // A complete continuation belongs to the context which proved it. Its
    // first leg can be rechecked cheaply; changed danger/phase/bounds require
    // a fresh search, not reuse of the old full-home proof.
    bool ProofMatches(ActionPosition const& home, std::optional<ActionPosition> const& danger,
        uint32 phaseMask, uint32 capabilities, float radius, float arrivalRadius, float clearance) const
    {
        return HasProofContext && ProofPhaseMask == phaseMask && ProofCapabilities == capabilities && ProofArrivalRadius == arrivalRadius &&
            RecoveryMovement::SamePoint(Move.Home, home) && Move.HomeRadius == radius && Move.DangerRadius == clearance &&
            Move.Danger.has_value() == danger.has_value() && (!danger || RecoveryMovement::SamePoint(*Move.Danger, *danger));
    }
    void ConfigureCorridor(bool returning, std::vector<ActionPosition> const& current, bool surface)
    {
        FollowsCorridor = returning && CorridorSeed && !current.empty() &&
            LivingReturnPolicy::Distance(Move.Destination, current.front()) <= 1;
        Move.SurfaceCorridor = FollowsCorridor && surface;
    }
};

enum class LivingAdviceValidation : uint8 { Valid, Invalid, Deferred };

// One admitted model search can span many needs ticks. Its tested candidates
// are retained only while the exact actor/request context still matches.
struct LivingAdviceSearch
{
    enum class Stage : uint8 { Memory, Routes, Rejoin, Detours, Food, Local, Ready };
    struct Seed
    {
        ActionPosition Target;
        char const* Strategy = "DETOUR";
        bool RoutePoint = false, Rejoin = false, Toward = false;
        bool Ground = false, LocalGround = false, KnownFood = false, ObservedPrey = false, ForageLeg = false;
    };
    bool Active = false, Returning = false, SeedsReady = false;
    uint64 ProgressAt = 0, Lifetime = 0;
    uint32 PhaseMask = 0, Capabilities = 0;
    float Radius = 0, ArrivalRadius = 0, Clearance = 0;
    ActionPosition Origin, Home;
    std::optional<ActionPosition> Danger;
    Stage Current = Stage::Memory;
    std::size_t Next = 0, MemoryNext = 0;
    std::vector<Seed> Seeds;
    std::vector<LivingAdviceCandidate> Candidates;
    std::optional<ActionPosition> ResolvedTarget;
    std::optional<LivingAdviceCandidate> Trial;
    LivingReturnPolicy::HomeCorridorSearch Continuation;
    LivingReturnPolicy::RejoinSearch Probes;
    LivingPlanningCarePause CarePause;
    LivingPlanningCarePause BudgetPause;

    bool Matches(uint64 now, uint64 lifetime, ActionPosition const& here, ActionPosition const& home,
        std::optional<ActionPosition> const& danger, bool returning, uint32 phaseMask, uint32 capabilities,
        float radius, float arrivalRadius, float clearance) const
    {
        return Active && LivingPlanningFresh(now, ProgressAt, CarePause, BudgetPause) && Lifetime == lifetime &&
            RecoveryMovement::SamePoint(Origin, here) && RecoveryMovement::SamePoint(Home, home) &&
            Danger.has_value() == danger.has_value() && (!Danger || RecoveryMovement::SamePoint(*Danger, *danger)) &&
            Returning == returning && PhaseMask == phaseMask && Capabilities == capabilities &&
            Radius == radius && ArrivalRadius == arrivalRadius && Clearance == clearance;
    }
    void Advance(Stage next)
    {
        Current = next; Next = 0; Seeds.clear(); SeedsReady = false;
        ResolvedTarget.reset(); Trial.reset(); Continuation = {};
    }
    void NextSeed(uint64 now)
    { ++Next; ResolvedTarget.reset(); Trial.reset(); Continuation = {}; MarkProgress(now); }
    void MarkProgress(uint64 now) { ProgressAt = now; BudgetPause = {}; }
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
    LivingFoodMemory Food;
    LivingAdviceSearch Search;
    LivingPlanningCarePause ReplyBudgetPause;
    LivingAdviceAdmissionState Admission;

    bool Fresh(uint64 now, ActionPosition const& here, ActionPosition const& home) const
    {
        auto paused = Responded ? ReplyBudgetPause.PausedMs(now, RequestedAt, true) : std::optional<uint64>(0);
        return PendingId && paused && now >= RequestedAt && now - RequestedAt - *paused <= 30000 &&
            here.MapId == Origin.MapId && LivingReturnPolicy::Distance(here, Origin) <= 2.0f &&
            RecoveryMovement::SamePoint(home, Home);
    }
    void ClearPending()
    { PendingId = 0; Responded = false; Choice.reset(); Candidates.clear(); Search = {}; ReplyBudgetPause = {}; }
    LivingAdviceCandidate* ChosenCandidate()
    {
        if (!PendingId || !Responded || !Choice) return nullptr;
        for (auto& candidate : Candidates) if (candidate.Option.Token == *Choice) return &candidate;
        return nullptr;
    }
    std::optional<LivingAdviceCandidate> FinishChoice(LivingAdviceValidation validation)
    {
        auto candidate = ChosenCandidate();
        if (!candidate) { ClearPending(); Status = "DECLINED"; return std::nullopt; }
        if (validation == LivingAdviceValidation::Deferred)
        { Status = "PLANNING_DEFERRED"; return std::nullopt; }
        if (validation == LivingAdviceValidation::Invalid)
        { ++Rejected; ClearPending(); Status = "REVALIDATION_FAILED"; return std::nullopt; }
        auto selected = *candidate;
        ++Selected; ClearPending(); Status = "SELECTED";
        return selected;
    }
    void RememberSuccess()
    {
        if (!Active) return;
        if (Successful.size() == 8) Successful.erase(Successful.begin());
        Successful.push_back({ActiveOrigin, *Active});
        Active.reset();
    }
};
#endif
