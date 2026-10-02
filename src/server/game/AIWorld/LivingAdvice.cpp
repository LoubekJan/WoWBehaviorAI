/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Agent/LivingRecoveryPath.h"
#include "Agent/LivingForagePolicy.h"
#include "Scheduler/PlanningWorkBudget.h"
#include "Creature.h"
#include "Map.h"
#include "Log.h"
#include <algorithm>
#include <list>

void AIWorldMgr::HandleRecoveryAdvice(AIResponse const& response)
{
    AgentRecord* record = _registry.Find(response.Agent);
    if (!record) return;
    auto& advice = record->LivingRole.Advice;
    if (!advice.PendingId || advice.PendingId != response.RequestId) return;
    if (!response.Success || !response.Recovery || response.Recovery->Episode != advice.Episode ||
        record->RuntimeGuid != record->LivingRole.RuntimeGuid)
    {
        ++advice.Unavailable;
        advice.Status = response.StatusCode == 503 ? "MODEL_UNAVAILABLE" : "REQUEST_FAILED";
        advice.ClearPending();
        return;
    }
    advice.Choice = response.Recovery->Token;
    advice.Responded = true;
    advice.Status = "RESPONSE_READY";
}

std::optional<LivingAdviceCandidate> AIWorldMgr::TryLivingAdvice(AgentRecord& record, Creature& creature,
    uint64 nowMs, bool returning, ActionPosition const* danger)
{
    using namespace LivingReturnPolicy;
    using Stage = LivingAdviceSearch::Stage;
    using Seed = LivingAdviceSearch::Seed;
    constexpr std::size_t MaxOptions = 4;
    auto& state = record.LivingRole;
    auto& advice = state.Advice;
    auto& search = advice.Search;
    if (advice.Status == "PLANNING_DEFERRED") advice.Status = "IDLE";
    if (!_recoveryAdviceEnabled || !HasRecoveryAdvice(record.Id) || !_aiClient ||
        !creature.IsAlive() || creature.IsInCombat() || creature.IsInEvadeMode() ||
        creature.GetMapId() != 0 || creature.GetZoneId() != 12 ||
        record.ControlMode != AgentControlMode::AIWorldControlled || record.RuntimeGuid != creature.GetGUID())
    { search = {}; return std::nullopt; }
    float forageRadius = LivingForagePolicy::SearchRadius(advice.Food.EmptyRounds);
    ActionPosition here{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
    auto const& homePosition = creature.GetHomePosition();
    ActionPosition home{creature.GetMapId(), homePosition.GetPositionX(), homePosition.GetPositionY(), homePosition.GetPositionZ()};
    auto role = LivingRolePolicy::Resolve(record.Type, creature.GetEntry(), false);
    float arrivalRadius = ((role == LivingRolePolicy::Role::Service || creature.IsQuestGiver()) ?
        0.0f : LivingRolePolicy::RoamRadius(role)) + 2.0f;
    float radius = returning ? state.ReturnHomeLimit : forageRadius;
    float clearance = LivingRolePolicy::SafetyRadius(record.Id.Value);
    std::optional<ActionPosition> dangerPoint = danger ? std::optional<ActionPosition>(*danger) : std::nullopt;
    uint32 capabilities = (creature.CanWalk() ? 1u : 0u) | (creature.CanEnterWater() ? 2u : 0u);
    auto operationsBefore = PlanningWorkBudget::StartedOperations();
    auto defer = [&]() -> std::optional<LivingAdviceCandidate>
    {
        if (PlanningWorkBudget::StartedOperations() != operationsBefore) search.ProgressAt = nowMs;
        advice.Status = "PLANNING_DEFERRED";
        return std::nullopt;
    };
    auto configure = [&](LivingAdviceCandidate& candidate)
    {
        candidate.Move.Danger = dangerPoint; candidate.Move.DangerRadius = clearance;
        candidate.Move.Home = home; candidate.Move.HomeRadius = radius;
        candidate.ConfigureCorridor(returning, state.ReturnRoute.Planned, state.ReturnRoute.SurfaceCorridor);
        candidate.Backtrack = returning && !candidate.Move.Rejoin && !candidate.FollowsCorridor &&
            state.ReturnRoute.Revisited(candidate.Move.Destination);
    };
    // Submitted/remembered options have a complete continuation. Revalidate the
    // immediate movement; execution checks every subsequent corridor leg again.
    auto revalidate = [&](LivingAdviceCandidate& candidate)
    {
        if (!candidate.ProofMatches(home, dangerPoint, creature.GetPhaseMask(), capabilities, radius, arrivalRadius, clearance))
            return LivingAdviceValidation::Invalid;
        configure(candidate);
        if (state.ReturnRoute.Failed(here, candidate.Move.Destination)) return LivingAdviceValidation::Invalid;
        auto work = PlanningWorkBudget::TryAcquire();
        if (!work) return LivingAdviceValidation::Deferred;
        candidate.Diagnostics.Deferred = false;
        Movement::PointsArray points;
        if (!LivingRecoveryPath::Build(creature, candidate.Move, points, &candidate.Diagnostics))
            return candidate.Diagnostics.Deferred ? LivingAdviceValidation::Deferred : LivingAdviceValidation::Invalid;
        if (returning && !candidate.FollowsCorridor && candidate.Continuation.empty()) return LivingAdviceValidation::Invalid;
        if (returning && !state.ReturnRoute.Allows(here, candidate.Move.Destination,
            candidate.FollowsCorridor || !candidate.Continuation.empty(), candidate.Move.Rejoin)) return LivingAdviceValidation::Invalid;
        return LivingAdviceValidation::Valid;
    };
    if (advice.PendingId)
    {
        if (!advice.Fresh(nowMs, here, home) || advice.Returning != returning || advice.Candidates.empty() ||
            !advice.Candidates.front().ProofMatches(home, dangerPoint, creature.GetPhaseMask(), capabilities, radius, arrivalRadius, clearance))
        { ++advice.Rejected; advice.Status = "STALE"; advice.ClearPending(); return std::nullopt; }
        if (!advice.Responded) return std::nullopt;
        auto candidate = advice.ChosenCandidate();
        return advice.FinishChoice(candidate ? revalidate(*candidate) : LivingAdviceValidation::Invalid);
    }
    bool stalled = returning ? state.ReturningHome && state.HomeProgressAtMs && nowMs >= state.HomeProgressAtMs + 30000 &&
        (state.ReturnFailures >= 3 || nowMs >= state.ReturnStartedAtMs + 60000) :
        state.HungrySinceMs && nowMs >= state.HungrySinceMs + 120000;
    if (!stalled || nowMs < advice.CooldownUntil || (!returning && advice.Active))
    { search = {}; return std::nullopt; }
    if (!search.Matches(nowMs, advice.LifetimeAt, here, home, dangerPoint, returning,
        creature.GetPhaseMask(), capabilities, radius, arrivalRadius, clearance)) search = {};
    if (!search.Active)
    {
        if (!_recoveryAdviceBudget.Acquire(record.Id.Value, nowMs, returning))
        { advice.Status = "WAITING_TURN"; return std::nullopt; }
        search.Active = true; search.ProgressAt = nowMs; search.Lifetime = advice.LifetimeAt;
        search.Origin = here; search.Home = home; search.Danger = dangerPoint; search.Returning = returning;
        search.PhaseMask = creature.GetPhaseMask(); search.Capabilities = capabilities;
        search.Radius = radius; search.ArrivalRadius = arrivalRadius; search.Clearance = clearance;
        search.Current = returning ? Stage::Memory : Stage::Food;
    }
    auto observePrey = [&]()
    {
        std::list<Creature*> nearby;
        creature.GetCreatureListWithEntryInGrid(nearby, 0, 30.0f);
        std::vector<Creature*> prey;
        for (auto* target : nearby)
            if (target->IsAlive() && _agentTypeCatalog.Resolve(target->GetEntry()) == AgentType::Prey &&
                creature.IsValidAttackTarget(target)) prey.push_back(target);
        std::sort(prey.begin(), prey.end(), [&](auto* a, auto* b)
            { return creature.GetExactDist2d(a) < creature.GetExactDist2d(b); });
        if (prey.size() > 8) prey.resize(8);
        std::vector<ActionPosition> visible;
        for (auto* target : prey)
            if (creature.IsWithinLOSInMap(target))
                visible.push_back({here.MapId, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()});
        return visible;
    };
    auto addSeed = [&](ActionPosition const& point, char const* strategy, bool routePoint = false, bool rejoin = false)
    {
        Seed seed; seed.Target = point; seed.Strategy = strategy; seed.RoutePoint = routePoint; seed.Rejoin = rejoin;
        search.Seeds.push_back(seed);
    };
    auto rejectTrial = [&]()
    {
        if (!returning && search.Trial)
        {
            state.Forage.Navigation = search.Trial->Diagnostics.Navigation;
            if (state.Forage.Navigation.Failure == "NONE") state.Forage.Navigation.Failure = "RECOVERY_REJECTED";
            ++state.Forage.PathRejected;
        }
        search.NextSeed(nowMs);
    };
    while (search.Current != Stage::Ready)
    {
        if (search.Candidates.size() >= MaxOptions) { search.Advance(Stage::Ready); break; }
        if (search.Current == Stage::Memory)
        {
            while (search.MemoryNext < advice.Successful.size())
            {
                auto const& memory = advice.Successful[search.MemoryNext];
                if (memory.From.MapId == here.MapId && Distance(memory.From, here) <= 2.0f &&
                    RecoveryMovement::SamePoint(memory.Candidate.Move.Home, home))
                {
                    auto candidate = memory.Candidate;
                    auto validation = revalidate(candidate);
                    if (validation == LivingAdviceValidation::Deferred) return defer();
                    if (validation == LivingAdviceValidation::Valid)
                    {
                        advice.CooldownUntil = nowMs + 120000;
                        candidate.Option.Strategy = "KNOWN_SUCCESS"; candidate.Option.Successes = 1;
                        ++advice.Reused; advice.Status = "MEMORY_SELECTED"; search = {};
                        return candidate;
                    }
                }
                ++search.MemoryNext; search.ProgressAt = nowMs;
            }
            search.Advance(Stage::Routes);
        }
        if (!search.SeedsReady)
        {
            if (search.Current == Stage::Routes)
            {
                if (!state.ReturnRoute.Planned.empty()) addSeed(state.ReturnRoute.Planned.front(), "CORRIDOR", true);
                unsigned count = 0;
                for (auto const& point : TrailSteps(state.ReturnTrail, here))
                { addSeed(point, "TRAIL", true); if (++count == 3) break; }
            }
            else if (search.Current == Stage::Rejoin)
            {
                bool deferred = false;
                auto points = LivingRecoveryPath::RejoinPositions(creature, nullptr, &search.Probes, &deferred);
                if (deferred) return defer();
                unsigned count = 0;
                for (auto const& point : points) { addSeed(point, "NAV_REJOIN", true, true); if (++count == 3) break; }
            }
            else if (search.Current == Stage::Detours)
                for (auto const& point : Detours(here, home, ++state.ReturnSearchSequence)) addSeed(point, "DETOUR");
            else if (search.Current == Stage::Food)
            {
                auto work = PlanningWorkBudget::TryAcquire();
                if (!work) return defer();
                auto visible = observePrey();
                if (auto hint = advice.Food.FoodHint(home, forageRadius, nowMs))
                { Seed seed; seed.Target = *hint; seed.Strategy = "FORAGE"; seed.Toward = true; seed.KnownFood = true; search.Seeds.push_back(seed); }
                for (auto const& point : visible)
                { Seed seed; seed.Target = point; seed.Strategy = "FORAGE"; seed.Toward = true; seed.ObservedPrey = true; search.Seeds.push_back(seed); }
                for (unsigned i = 0; i < 12; ++i)
                {
                    Seed seed; seed.Target = LivingForagePolicy::Waypoint(home, record.Id.Value, state.ForageLeg+i, advice.Food.EmptyRounds);
                    seed.Strategy = "FORAGE"; seed.Toward = seed.Ground = seed.ForageLeg = true; search.Seeds.push_back(seed);
                }
            }
            else if (search.Current == Stage::Local)
                for (unsigned i = 0; i < 6; ++i)
                {
                    Seed seed; seed.Target = LivingForagePolicy::LocalWaypoint(here, record.Id.Value, state.ForageLeg+i);
                    seed.Strategy = "FORAGE"; seed.Toward = seed.Ground = seed.LocalGround = seed.ForageLeg = true; search.Seeds.push_back(seed);
                }
            search.SeedsReady = true;
        }
        while (search.Next < search.Seeds.size() && search.Candidates.size() < MaxOptions)
        {
            auto const& seed = search.Seeds[search.Next];
            if (!search.ResolvedTarget)
            {
                if (seed.Toward && !seed.ObservedPrey && advice.Food.Visits(seed.Target, nowMs))
                { search.NextSeed(nowMs); continue; }
                auto work = PlanningWorkBudget::TryAcquire();
                if (!work) return defer();
                if (seed.ForageLeg) ++state.ForageLeg;
                auto target = seed.Target;
                if (seed.Ground || (!seed.RoutePoint && !seed.Toward && !LivingRecoveryPath::InSwimmableWater(creature, target)))
                {
                    float height = seed.LocalGround ? creature.GetMapHeight(target.X, target.Y, here.Z) :
                        creature.GetMap()->GetHeight(creature.GetPhaseMask(), target.X, target.Y,
                            (seed.Toward ? here.Z : target.Z) + 4, true);
                    if (!std::isfinite(height) || height <= INVALID_HEIGHT || std::abs(height-(seed.Toward ? here.Z : target.Z)) > 12)
                    { search.NextSeed(nowMs); continue; }
                    target.Z = height + (seed.LocalGround ? creature.GetHoverOffset() : 0);
                }
                if (seed.Toward)
                {
                    state.Forage.SearchAtMs = nowMs; ++state.Forage.RouteAttempts;
                    auto point = LivingRecoveryPath::Toward(creature, target, home, forageRadius, &state.Forage.Navigation);
                    if (!point)
                    { ++state.Forage.PathRejected; advice.Food.Unreachable(seed.Target, nowMs); search.NextSeed(nowMs); continue; }
                    target = *point;
                }
                if (!UsefulStep(here, target) || Distance(here, target) > 28.1f)
                { search.NextSeed(nowMs); continue; }
                search.ResolvedTarget = target;
            }
            if (!search.Trial)
            {
                auto work = PlanningWorkBudget::TryAcquire();
                if (!work) return defer();
                LivingAdviceCandidate candidate;
                candidate.Option.Strategy = seed.Strategy;
                candidate.CorridorSeed = candidate.Option.Strategy == "CORRIDOR";
                candidate.Move = {*search.ResolvedTarget, home, radius, dangerPoint, seed.Rejoin};
                candidate.Move.QueryDestination = *search.ResolvedTarget; candidate.Move.DangerRadius = clearance;
                candidate.ProofPhaseMask = creature.GetPhaseMask(); candidate.ProofCapabilities = capabilities;
                candidate.ProofArrivalRadius = arrivalRadius;
                candidate.HasProofContext = true;
                candidate.Diagnostics.Candidates = 1; candidate.Diagnostics.RequestedZ = search.ResolvedTarget->Z;
                candidate.Diagnostics.QueryDestination = *search.ResolvedTarget;
                configure(candidate);
                Movement::PointsArray points;
                bool built = LivingRecoveryPath::Build(creature, candidate.Move, points, &candidate.Diagnostics);
                if (!built && candidate.Diagnostics.Deferred) return defer();
                if (!built || points.empty()) { search.Trial = std::move(candidate); rejectTrial(); continue; }
                auto const& end = points.back();
                candidate.Move.Destination = {here.MapId, end.x, end.y, end.z};
                candidate.Diagnostics.ResolvedZ = end.z; configure(candidate);
                search.Trial = std::move(candidate);
                if (state.ReturnRoute.Failed(here, search.Trial->Move.Destination)) { rejectTrial(); continue; }
                if (std::any_of(search.Candidates.begin(), search.Candidates.end(), [&](auto const& existing)
                    { return Distance(existing.Move.Destination, search.Trial->Move.Destination) <= 2; }))
                { search.NextSeed(nowMs); continue; }
            }
            auto& candidate = *search.Trial;
            if (returning && !candidate.FollowsCorridor)
            {
                Diagnostics continuationDiagnostics;
                candidate.Continuation = LivingRecoveryPath::HomeCorridor(creature, candidate.Move.Destination,
                    home, arrivalRadius, state.ReturnHomeLimit, danger, clearance, &continuationDiagnostics, &search.Continuation);
                candidate.Diagnostics.ContinuationPath = continuationDiagnostics.HomePath;
                if (continuationDiagnostics.Deferred) return defer();
                if (candidate.Continuation.empty()) { rejectTrial(); continue; }
            }
            if (returning && !state.ReturnRoute.Allows(here, candidate.Move.Destination,
                candidate.FollowsCorridor || !candidate.Continuation.empty(), candidate.Move.Rejoin))
            { rejectTrial(); continue; }
            auto& option = candidate.Option;
            option.Token = uint32(search.Candidates.size()+1);
            option.X = candidate.Move.Destination.X-here.X; option.Y = candidate.Move.Destination.Y-here.Y;
            option.Z = candidate.Move.Destination.Z-here.Z; option.Distance = Distance(here, candidate.Move.Destination);
            option.HomeGain = std::hypot(home.X-here.X, home.Y-here.Y) - std::hypot(home.X-candidate.Move.Destination.X, home.Y-candidate.Move.Destination.Y);
            option.Visits = (candidate.Backtrack ? 1 : 0) + advice.Food.Visits(candidate.Move.Destination, nowMs);
            option.Successes = seed.KnownFood ? 1 : 0;
            search.Candidates.push_back(std::move(candidate)); search.NextSeed(nowMs);
        }
        if (search.Candidates.size() >= MaxOptions) { search.Advance(Stage::Ready); break; }
        switch (search.Current)
        {
            case Stage::Routes: search.Advance(Stage::Rejoin); break;
            case Stage::Rejoin: search.Advance(Stage::Detours); break;
            case Stage::Detours: search.Advance(Stage::Ready); break;
            case Stage::Food: search.Advance(search.Candidates.empty() ? Stage::Local : Stage::Ready); break;
            case Stage::Local: search.Advance(Stage::Ready); break;
            default: break;
        }
    }
    if (!search.Candidates.empty())
    {
        auto work = PlanningWorkBudget::TryAcquire();
        if (!work) return defer();
        auto visible = observePrey(); // Refresh hints at submission after a long search.
        for (auto& candidate : search.Candidates)
        {
            candidate.Option.NearbyPrey = 0;
            for (auto const& prey : visible)
                if (std::hypot(prey.X-candidate.Move.Destination.X, prey.Y-candidate.Move.Destination.Y) < 20)
                    ++candidate.Option.NearbyPrey;
        }
        if (!returning) std::erase_if(search.Candidates, [](auto const& candidate)
            { return candidate.Option.Visits && !candidate.Option.NearbyPrey && !candidate.Option.Successes; });
    }
    if (search.Candidates.empty())
    {
        advice.Status = "NO_VALID_OPTIONS";
        advice.CooldownUntil = nowMs + (returning ? 120000 : advice.Food.AdviceFailed());
        search = {}; return std::nullopt;
    }
    AIRequest request;
    auto& context = request.Recovery;
    context.Agent = record.Id; context.Episode = nowMs;
    context.Role = LivingRolePolicy::ToString(role); context.Problem = returning ? "RETURN_HOME" : "FIND_FOOD";
    context.Failure = returning ? state.ReturnFailure : state.LastHuntStatus;
    context.Failures = returning ? state.ReturnFailures : advice.Food.FailedAdvice;
    context.Hunger = record.Needs.Hunger; context.HomeDistance = std::hypot(home.X-here.X, home.Y-here.Y);
    context.StalledMs = nowMs - (returning ? state.HomeProgressAtMs : state.HungrySinceMs);
    for (auto const& candidate : search.Candidates) context.Options.push_back(candidate.Option);
    uint64 id = _aiClient->SubmitRecovery(std::move(request));
    if (!id) { advice.Status = "CAPACITY_LIMIT"; advice.CooldownUntil = nowMs+5000; search = {}; return std::nullopt; }
    advice.PendingId = id; advice.RequestedAt = nowMs; advice.Episode = nowMs;
    advice.Origin = here; advice.Home = home; advice.Returning = returning; advice.CooldownUntil = nowMs+120000;
    advice.Responded = false; advice.Choice.reset(); advice.Candidates = std::move(search.Candidates); search = {};
    ++advice.Requests; advice.Status = "PENDING";
    TC_LOG_INFO("ai.world", "AI recovery agent={} request={} problem={} options={}", record.Id.Value, id,
        returning ? "RETURN_HOME" : "FIND_FOOD", advice.Candidates.size());
    return std::nullopt;
}
