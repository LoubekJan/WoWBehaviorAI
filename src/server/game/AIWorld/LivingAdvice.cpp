/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Agent/LivingRecoveryPath.h"
#include "Agent/LivingForagePolicy.h"
#include "Creature.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Log.h"
#include <algorithm>

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
    auto& state = record.LivingRole;
    auto& advice = state.Advice;
    if (!_recoveryAdviceEnabled || !_recoveryAdviceAgents.contains(record.Id.Value) || !_aiClient ||
        !creature.IsAlive() || creature.IsInCombat() || creature.IsInEvadeMode() ||
        creature.GetMapId() != 0 || creature.GetZoneId() != 12 ||
        record.ControlMode != AgentControlMode::AIWorldControlled || record.RuntimeGuid != creature.GetGUID())
        return std::nullopt;
    ActionPosition here{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
    auto const& homePosition = creature.GetHomePosition();
    ActionPosition home{creature.GetMapId(), homePosition.GetPositionX(), homePosition.GetPositionY(), homePosition.GetPositionZ()};
    auto revalidate = [&](LivingAdviceCandidate& candidate)
    {
        candidate.Move.Danger = danger ? std::optional<ActionPosition>(*danger) : std::nullopt;
        candidate.Move.Home = home;
        candidate.Move.HomeRadius = returning ? state.ReturnHomeLimit : LivingForagePolicy::HomeRadius;
        if (state.ReturnRoute.Failed(here, candidate.Move.Destination)) return false;
        candidate.Backtrack = returning && state.ReturnRoute.Revisited(candidate.Move.Destination);
        if (candidate.Backtrack && !state.ReturnRoute.CanBacktrack(here, candidate.Move.Destination)) return false;
        Movement::PointsArray points;
        return LivingRecoveryPath::Build(creature, candidate.Move, points, &candidate.Diagnostics);
    };
    if (advice.PendingId)
    {
        if (!advice.Fresh(nowMs, here, home) || advice.Returning != returning)
        {
            ++advice.Rejected; advice.Status = "STALE"; advice.ClearPending();
            return std::nullopt;
        }
        if (!advice.Responded) return std::nullopt;
        auto found = std::find_if(advice.Candidates.begin(), advice.Candidates.end(),
            [&](auto const& c) { return advice.Choice && c.Option.Token == *advice.Choice; });
        std::optional<LivingAdviceCandidate> candidate;
        if (found != advice.Candidates.end()) candidate = *found;
        advice.ClearPending();
        if (!candidate) { advice.Status = "DECLINED"; return std::nullopt; }
        if (!revalidate(*candidate)) { ++advice.Rejected; advice.Status = "REVALIDATION_FAILED"; return std::nullopt; }
        ++advice.Selected; advice.Status = "SELECTED";
        return candidate;
    }
    bool stalled = returning ? state.ReturningHome && state.HomeProgressAtMs && nowMs >= state.HomeProgressAtMs + 30000 &&
        (state.ReturnFailures >= 3 || nowMs >= state.ReturnStartedAtMs + 60000) :
        state.HungrySinceMs && nowMs >= state.HungrySinceMs + 120000;
    if (!stalled || nowMs < advice.CooldownUntil || nowMs < _nextRecoveryAdviceAtMs) return std::nullopt;
    // Previously successful steps are still checked against current geometry,
    // danger, failed edges and visit budgets. Memory is per materialization.
    if (returning)
        for (auto const& memory : advice.Successful)
            if (memory.From.MapId == here.MapId && Distance(memory.From, here) <= 2.0f &&
                RecoveryMovement::SamePoint(memory.Candidate.Move.Home, home))
            {
                auto candidate = memory.Candidate;
                if (revalidate(candidate))
                {
                    advice.CooldownUntil = nowMs + 120000;
                    candidate.Option.Strategy = "KNOWN_SUCCESS"; candidate.Option.Successes = 1;
                    ++advice.Reused; advice.Status = "MEMORY_SELECTED";
                    return candidate;
                }
            }
    advice.CooldownUntil = nowMs + 120000;
    std::vector<LivingAdviceCandidate> candidates;
    float homeDistance = std::hypot(home.X-here.X, home.Y-here.Y);
    std::list<Creature*> nearby;
    creature.GetCreatureListWithEntryInGrid(nearby, 0, 30.0f);
    std::vector<ActionPosition> visiblePrey;
    for (auto* prey : nearby)
        if (prey->IsAlive() && _agentTypeCatalog.Resolve(prey->GetEntry()) == AgentType::Prey &&
            creature.IsValidAttackTarget(prey) && creature.IsWithinLOSInMap(prey))
            visiblePrey.push_back({here.MapId, prey->GetPositionX(), prey->GetPositionY(), prey->GetPositionZ()});
    auto add = [&](ActionPosition target, char const* strategy, bool routePoint = false, bool rejoin = false)
    {
        if (candidates.size() == 8 || !UsefulStep(here, target) || Distance(here, target) > 28.1f) return;
        if (!routePoint && !LivingRecoveryPath::InSwimmableWater(creature, target))
        {
            float height = creature.GetMap()->GetHeight(creature.GetPhaseMask(), target.X, target.Y, target.Z + 4, true);
            if (!std::isfinite(height) || height <= INVALID_HEIGHT || std::abs(height-target.Z) > 12) return;
            target.Z = height;
        }
        LivingAdviceCandidate candidate;
        candidate.Move = {target, home, returning ? state.ReturnHomeLimit : LivingForagePolicy::HomeRadius,
            danger ? std::optional<ActionPosition>(*danger) : std::nullopt, rejoin};
        candidate.Diagnostics.Candidates = 1;
        candidate.Diagnostics.RequestedZ = target.Z;
        candidate.Diagnostics.QueryDestination = target;
        Movement::PointsArray points;
        if (!LivingRecoveryPath::Build(creature, candidate.Move, points, &candidate.Diagnostics)) return;
        candidate.Move.QueryDestination = target;
        auto const& end = points.back();
        candidate.Move.Destination = {here.MapId, end.x, end.y, end.z};
        candidate.Diagnostics.ResolvedZ = end.z;
        if (!revalidate(candidate)) return;
        if (std::any_of(candidates.begin(), candidates.end(), [&](auto const& c)
            { return Distance(c.Move.Destination, candidate.Move.Destination) <= 2; })) return;
        auto& option = candidate.Option;
        option.Token = uint32(candidates.size()+1); option.Strategy = strategy;
        option.X = end.x-here.X; option.Y = end.y-here.Y; option.Z = end.z-here.Z;
        option.Distance = Distance(here, candidate.Move.Destination);
        option.HomeGain = homeDistance - std::hypot(home.X-end.x, home.Y-end.y);
        option.Visits = candidate.Backtrack ? 1 : 0;
        for (auto const& searched : advice.Searched)
            if (Distance(searched, candidate.Move.Destination) < 8) ++option.Visits;
        // Only already visible local prey contribute; no unloaded-grid or
        // omniscient target search. A hint is not permission to attack.
        for (auto const& prey : visiblePrey)
            if (std::hypot(prey.X-end.x, prey.Y-end.y) < 20) ++option.NearbyPrey;
        candidates.push_back(std::move(candidate));
    };
    if (returning)
    {
        if (!state.ReturnRoute.Planned.empty()) add(state.ReturnRoute.Planned.front(), "CORRIDOR", true);
        unsigned tried = 0;
        for (auto const& point : TrailSteps(state.ReturnTrail, here))
        { add(point, "TRAIL", true); if (++tried == 3) break; }
        unsigned rejoins = 0;
        for (auto point : LivingRecoveryPath::RejoinPositions(creature))
        { add(point, "NAV_REJOIN", true, true); if (++rejoins == 3) break; }
        for (auto const& point : Detours(here, home, ++state.ReturnSearchSequence)) add(point, "DETOUR");
    }
    else
        for (unsigned i = 0; i < 12 && candidates.size() < 8; ++i)
        {
            auto waypoint = LivingForagePolicy::Waypoint(home, record.Id.Value, state.ForageLeg+i);
            if (auto point = LivingForagePolicy::Step(here, waypoint, home)) add(*point, "FORAGE");
        }
    if (candidates.empty()) { advice.Status = "NO_VALID_OPTIONS"; return std::nullopt; }
    AIRequest request;
    auto& context = request.Recovery;
    context.Agent = record.Id; context.Episode = nowMs;
    context.Role = LivingRolePolicy::ToString(LivingRolePolicy::Resolve(record.Type, creature.GetEntry(), false));
    context.Problem = returning ? "RETURN_HOME" : "FIND_FOOD";
    context.Failure = returning ? state.ReturnFailure : state.LastHuntStatus;
    context.Failures = state.ReturnFailures; context.Hunger = record.Needs.Hunger;
    context.HomeDistance = homeDistance;
    context.StalledMs = nowMs - (returning ? state.HomeProgressAtMs : state.HungrySinceMs);
    for (auto const& candidate : candidates) context.Options.push_back(candidate.Option);
    uint64 id = _aiClient->SubmitRecovery(std::move(request));
    if (!id) { advice.Status = "CAPACITY_LIMIT"; advice.CooldownUntil = nowMs+5000; return std::nullopt; }
    advice.PendingId = id; advice.RequestedAt = nowMs; advice.Episode = nowMs;
    advice.Origin = here; advice.Home = home; advice.Returning = returning;
    advice.Responded = false; advice.Choice.reset(); advice.Candidates = std::move(candidates);
    ++advice.Requests; advice.Status = "PENDING";
    _nextRecoveryAdviceAtMs = nowMs+2000;
    TC_LOG_INFO("ai.world", "AI recovery agent={} request={} problem={} options={}", record.Id.Value, id,
        returning ? "RETURN_HOME" : "FIND_FOOD", advice.Candidates.size());
    return std::nullopt;
}
