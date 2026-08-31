/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "HuntIntentSystem.h"
#include "AgentGroupRecord.h"
#include <cmath>
#include <unordered_map>

namespace
{
    bool IsRealGroupMember(AgentGroupRecord const& group, AgentId observer)
    {
        for (AgentGroupMembership const& membership : group.Members)
            if (membership.Member == observer)
                return true;

        return false;
    }

    bool SameObserverState(CoalitionMemberObservation const& a, CoalitionMemberObservation const& b)
    {
        return a.Materialized == b.Materialized && a.Alive == b.Alive && a.MapId == b.MapId &&
            a.X == b.X && a.Y == b.Y && a.Z == b.Z;
    }

    // 2.12G3B P2 fix (STATIC review): resolves every AgentId in members to
    // AT MOST one usable state - a MemberId with two or more disagreeing
    // entries is unresolvable and simply absent from the returned map,
    // rather than arbitrarily trusting whichever entry happened to come
    // first. Deterministic regardless of members' own order: for a given
    // MemberId, whether any two of its entries disagree does not depend on
    // which one is scanned first.
    std::unordered_map<uint64, CoalitionMemberObservation const*> ResolveObserverStates(std::vector<CoalitionMemberObservation> const& members)
    {
        std::unordered_map<uint64, CoalitionMemberObservation const*> resolved;
        std::unordered_map<uint64, bool> conflicted;

        for (CoalitionMemberObservation const& observation : members)
        {
            uint64 key = observation.MemberId.Value;
            auto it = resolved.find(key);
            if (it == resolved.end())
            {
                resolved.emplace(key, &observation);
                continue;
            }

            if (!SameObserverState(*it->second, observation))
                conflicted[key] = true;
        }

        for (auto const& [key, isConflicted] : conflicted)
            if (isConflicted)
                resolved.erase(key);

        return resolved;
    }

    bool SameObservationPayload(HuntTargetObservation const& a, HuntTargetObservation const& b)
    {
        return a.Distance == b.Distance && a.LineOfSight == b.LineOfSight &&
            a.Target.Alive == b.Target.Alive && a.Target.MapId == b.Target.MapId &&
            a.Target.TargetEntry == b.Target.TargetEntry &&
            a.Target.X == b.Target.X && a.Target.Y == b.Target.Y && a.Target.Z == b.Target.Z;
    }
}

std::optional<HuntIntent> HuntIntentSystem::Evaluate(AgentGroupRecord const& group, AgentGroupCoordinationProfile const& profile,
    std::vector<CoalitionMemberObservation> const& members, std::vector<HuntTargetObservation> const& targets, uint64 nowMs) const
{
    // Fail-closed profile checks - see this class's own header comment for
    // why each of these is checked independently rather than trusting that
    // a non-Invalid profileId already implies a matching Kind, or that a
    // matching Kind already implies a matching ProfileId (the same
    // discipline AgentGroupIntentSystem::Evaluate() already holds).
    if (profile.ProfileId == CoalitionFormationProfileId::Invalid)
        return std::nullopt;

    if (profile.Kind != group.Kind)
        return std::nullopt;

    if (profile.ProfileId != group.ProfileId)
        return std::nullopt;

    if (!profile.HuntEnabled)
        return std::nullopt;

    if (profile.HuntTargetCreatureEntry == 0)
        return std::nullopt;

    // 2.12G3B P2 fix (STATIC review): NaN/Inf must never reach the
    // per-observation Distance comparison below - NaN in particular would
    // break every comparison against it (NaN is neither <=, ==, nor > any
    // value), silently letting every Distance pass unrejected.
    if (!std::isfinite(profile.HuntAcquisitionRadius) || profile.HuntAcquisitionRadius <= 0.0f)
        return std::nullopt;

    if (profile.HuntObservationMaxAgeMs == 0)
        return std::nullopt;

    if (nowMs == 0)
        return std::nullopt;

    std::unordered_map<uint64, CoalitionMemberObservation const*> observerStates = ResolveObserverStates(members);

    // Stage 1: apply every per-observation fail-closed check, grouping
    // survivors by TargetGuid - see this class's own header comment for
    // the full rule order.
    std::unordered_map<uint64, std::vector<HuntTargetObservation const*>> observationsByTarget;

    for (HuntTargetObservation const& observation : targets)
    {
        if (!IsRealGroupMember(group, observation.Observer))
            continue;

        auto observerIt = observerStates.find(observation.Observer.Value);
        if (observerIt == observerStates.end())
            continue;

        CoalitionMemberObservation const* observerState = observerIt->second;
        if (!observerState->Materialized || !observerState->Alive)
            continue;

        if (observerState->MapId != group.TerritoryMapId)
            continue;

        HuntTargetProvenance const& target = observation.Target;

        if (target.TargetGuid.IsEmpty() || !target.TargetGuid.IsCreature())
            continue;

        // 2.12G3B P2 fix (STATIC review): TargetEntry must be provably the
        // same entry already encoded inside TargetGuid itself, never
        // trusted as an independent, freely-set field.
        if (target.TargetGuid.GetEntry() != target.TargetEntry)
            continue;

        if (target.TargetEntry != profile.HuntTargetCreatureEntry)
            continue;

        if (!target.Alive)
            continue;

        // 2.12G3A: HUNT is restricted to persistent non-instance/
        // base-world targets - a target off the group's own base-world
        // territory map fails closed here, never treated as reachable.
        if (target.MapId != group.TerritoryMapId)
            continue;

        // 2.12G3B P2 fix (STATIC review): not read by this class's own
        // selection math, but carried forward unchanged into the produced
        // HuntIntent/HuntProposal.
        if (!std::isfinite(target.X) || !std::isfinite(target.Y) || !std::isfinite(target.Z))
            continue;

        if (target.ObservedAtMs == 0 || target.ObservedAtMs > nowMs)
            continue;

        if (nowMs - target.ObservedAtMs > profile.HuntObservationMaxAgeMs)
            continue;

        if (!observation.LineOfSight)
            continue;

        if (!std::isfinite(observation.Distance) || observation.Distance < 0.0f || observation.Distance > profile.HuntAcquisitionRadius)
            continue;

        observationsByTarget[target.TargetGuid.GetRawValue()].push_back(&observation);
    }

    // Stage 2: within each TargetGuid's own group, detect conflicting
    // duplicates (same ObservedAtMs+Observer, disagreeing payload) and
    // exclude that whole target fail-closed if found; otherwise reduce to
    // the group's single freshest observation. See this class's own header
    // comment for why this replaces a plain freshest-wins fold.
    std::unordered_map<uint64, HuntTargetObservation const*> bestByTarget;

    for (auto const& [guidKey, bucket] : observationsByTarget)
    {
        bool ambiguous = false;

        for (std::size_t i = 0; i < bucket.size() && !ambiguous; ++i)
        {
            for (std::size_t j = i + 1; j < bucket.size(); ++j)
            {
                HuntTargetObservation const* a = bucket[i];
                HuntTargetObservation const* b = bucket[j];

                if (a->Target.ObservedAtMs != b->Target.ObservedAtMs || a->Observer != b->Observer)
                    continue;

                if (!SameObservationPayload(*a, *b))
                {
                    ambiguous = true;
                    break;
                }
            }
        }

        if (ambiguous)
            continue;

        HuntTargetObservation const* best = nullptr;
        for (HuntTargetObservation const* candidate : bucket)
        {
            if (!best)
            {
                best = candidate;
                continue;
            }

            bool candidateIsFresher = candidate->Target.ObservedAtMs > best->Target.ObservedAtMs;
            bool candidateIsSameAgeLowerObserver = candidate->Target.ObservedAtMs == best->Target.ObservedAtMs &&
                candidate->Observer.Value < best->Observer.Value;

            if (candidateIsFresher || candidateIsSameAgeLowerObserver)
                best = candidate;
        }

        bestByTarget.emplace(guidKey, best);
    }

    // Stage 3: across the surviving one-candidate-per-target set, the
    // nearest wins - tie-broken by the lower TargetGuid. Both reductions
    // above are strict total orders over their own tie-break field, so the
    // final result never depends on either input vector's order.
    HuntTargetObservation const* selected = nullptr;

    for (auto const& [guidKey, candidate] : bestByTarget)
    {
        if (!selected)
        {
            selected = candidate;
            continue;
        }

        bool candidateIsCloser = candidate->Distance < selected->Distance;
        bool candidateIsSameDistanceLowerGuid = candidate->Distance == selected->Distance &&
            candidate->Target.TargetGuid.GetRawValue() < selected->Target.TargetGuid.GetRawValue();

        if (candidateIsCloser || candidateIsSameDistanceLowerGuid)
            selected = candidate;
    }

    if (!selected)
        return std::nullopt;

    HuntIntent intent;
    intent.Group = group.Id;
    intent.Target = selected->Target;
    intent.StartedAtMs = nowMs;
    return intent;
}
