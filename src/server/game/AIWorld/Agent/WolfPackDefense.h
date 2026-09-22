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

#ifndef AIWORLD_WOLFPACKDEFENSE_H
#define AIWORLD_WOLFPACKDEFENSE_H

#include "AgentGroupRecord.h"
#include "ObjectGuid.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

// World-thread snapshots of current combat, never inferred from faction alone.
struct WolfPackThreatObservation
{
    AgentId Member;
    ObjectGuid MemberGuid;
    ObjectGuid ThreatGuid;
    uint32 MemberMapId = 0;
    uint32 ThreatMapId = 0;
    bool MemberControlled = false;
    bool MemberAlive = false;
    bool EngagedWithThreat = false;
    bool ThreatAlive = false;
    bool ThreatAttackable = false;
    bool MemberVisible = false;
    bool ThreatVisible = false;
    float MemberDistance = std::numeric_limits<float>::infinity();
    float ThreatDistance = std::numeric_limits<float>::infinity();
};

namespace WolfPackDefense
{
    constexpr float AssistRadius = 30.0f;

    inline std::optional<WolfPackThreatObservation> SelectThreat(AgentId actor, uint32 mapId,
        AgentGroupRecord const& group, std::vector<WolfPackThreatObservation> const& observations, uint32 preyEntry)
    {
        auto isMember = [&group](AgentId id)
        {
            return id && std::any_of(group.Members.begin(), group.Members.end(),
                [id](AgentGroupMembership const& member) { return member.Member == id; });
        };
        if (!actor || !group.Id || group.Kind != AgentGroupKind::Loose ||
            group.ProfileId != CoalitionFormationProfileId::WolfLoose ||
            group.TerritoryMapId != mapId || !isMember(actor))
            return std::nullopt;

        std::optional<WolfPackThreatObservation> best;
        for (WolfPackThreatObservation const& candidate : observations)
        {
            if (candidate.Member == actor || !isMember(candidate.Member) || !candidate.MemberGuid.IsCreature() ||
                !candidate.MemberControlled || !candidate.MemberAlive || !candidate.EngagedWithThreat ||
                !candidate.ThreatGuid.IsUnit() || !candidate.ThreatAlive || !candidate.ThreatAttackable ||
                candidate.MemberMapId != mapId || candidate.ThreatMapId != mapId ||
                !candidate.MemberVisible || !candidate.ThreatVisible)
                continue;

            // A prey animal fighting back must not turn a normal hunt into defense.
            if (candidate.ThreatGuid.IsCreature() && candidate.ThreatGuid.GetEntry() == preyEntry)
                continue;
            if (!std::isfinite(candidate.MemberDistance) || candidate.MemberDistance < 0.0f ||
                candidate.MemberDistance > AssistRadius || !std::isfinite(candidate.ThreatDistance) ||
                candidate.ThreatDistance < 0.0f || candidate.ThreatDistance > AssistRadius)
                continue;

            bool targetsPackMember = std::any_of(observations.begin(), observations.end(),
                [&](WolfPackThreatObservation const& member)
                { return isMember(member.Member) && member.MemberGuid == candidate.ThreatGuid; });
            if (targetsPackMember)
                continue;

            if (!best || candidate.ThreatDistance < best->ThreatDistance ||
                (candidate.ThreatDistance == best->ThreatDistance && candidate.Member < best->Member))
                best = candidate;
        }
        return best;
    }
}

#endif
