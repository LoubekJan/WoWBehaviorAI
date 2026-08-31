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

    CoalitionMemberObservation const* FindObserverState(std::vector<CoalitionMemberObservation> const& members, AgentId observer)
    {
        for (CoalitionMemberObservation const& observation : members)
            if (observation.MemberId == observer)
                return &observation;

        return nullptr;
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

    if (profile.HuntTargetCreatureEntry == 0 || profile.HuntAcquisitionRadius <= 0.0f || profile.HuntObservationMaxAgeMs == 0)
        return std::nullopt;

    if (nowMs == 0)
        return std::nullopt;

    // Reduction step 1: at most one surviving candidate per distinct
    // TargetGuid - the single freshest valid observation of it, tie-broken
    // by the lower observing AgentId. Keyed by ObjectGuid::GetRawValue()
    // (a plain uint64) rather than ObjectGuid itself purely so
    // std::unordered_map's default hash applies with no extra hasher.
    std::unordered_map<uint64, HuntTargetObservation const*> bestByTarget;

    for (HuntTargetObservation const& observation : targets)
    {
        if (!IsRealGroupMember(group, observation.Observer))
            continue;

        CoalitionMemberObservation const* observerState = FindObserverState(members, observation.Observer);
        if (!observerState || !observerState->Materialized || !observerState->Alive)
            continue;

        if (observerState->MapId != group.TerritoryMapId)
            continue;

        HuntTargetProvenance const& target = observation.Target;

        if (target.TargetGuid.IsEmpty())
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

        if (target.ObservedAtMs == 0 || target.ObservedAtMs > nowMs)
            continue;

        if (nowMs - target.ObservedAtMs > profile.HuntObservationMaxAgeMs)
            continue;

        if (!observation.LineOfSight)
            continue;

        if (observation.Distance > profile.HuntAcquisitionRadius)
            continue;

        uint64 guidKey = target.TargetGuid.GetRawValue();
        auto it = bestByTarget.find(guidKey);
        if (it == bestByTarget.end())
        {
            bestByTarget.emplace(guidKey, &observation);
            continue;
        }

        HuntTargetObservation const* current = it->second;
        bool observationIsFresher = target.ObservedAtMs > current->Target.ObservedAtMs;
        bool observationIsSameAgeLowerObserver = target.ObservedAtMs == current->Target.ObservedAtMs &&
            observation.Observer.Value < current->Observer.Value;

        if (observationIsFresher || observationIsSameAgeLowerObserver)
            it->second = &observation;
    }

    // Reduction step 2: across the surviving one-candidate-per-target set,
    // the nearest wins - tie-broken by the lower TargetGuid. Both
    // reductions are strict total orders over their own tie-break field,
    // so the final result never depends on either input vector's order.
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
