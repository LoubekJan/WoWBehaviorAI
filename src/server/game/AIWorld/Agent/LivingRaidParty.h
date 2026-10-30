/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGRAIDPARTY_H
#define AIWORLD_LIVINGRAIDPARTY_H

#include "Action/ActionPosition.h"
#include "CoalitionFormationSystem.h"
#include "GroupMemberFormation.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A sortie is a session-only coordination layer over independent agents.
// It neither registers persistent social groups nor owns movement, combat,
// cargo, memory, or home positions. Every live observation and action remains
// the caller's responsibility, including binding it to the original GUID.
namespace LivingRaidParty
{
    constexpr uint32 MapId = 725, CreatureEntry = 116;
    constexpr uint32 MinMembers = 2, MaxMembers = 5;
    constexpr float FormationRadius = 100.0f;
    constexpr uint64 AssemblyTimeoutMs = 45000, LagTimeoutMs = 120000, AttemptTimeoutMs = 900000;

    struct Candidate
    {
        CoalitionCandidate Actor;
        uint64 SpawnId = 0;
        bool Materialized = false, Alive = false, Bound = false;
        bool AtCamp = false, Idle = false, Ready = false;
    };

    inline bool Eligible(Candidate const& candidate)
    {
        auto const& actor = candidate.Actor;
        return candidate.SpawnId >= 901050 && candidate.SpawnId <= 901059 &&
            actor.Id.Value == candidate.SpawnId && actor.MapId == MapId &&
            actor.CreatureEntry == CreatureEntry && actor.WorldFaction == WorldFactions::DefiasBrotherhood &&
            std::isfinite(actor.X) && std::isfinite(actor.Y) && std::isfinite(actor.Z) &&
            candidate.Materialized && candidate.Alive && candidate.Bound &&
            candidate.AtCamp && candidate.Idle && candidate.Ready;
    }

    inline bool SameCandidate(Candidate const& a, Candidate const& b)
    {
        return a.Actor.Id == b.Actor.Id && a.Actor.MapId == b.Actor.MapId &&
            a.Actor.X == b.Actor.X && a.Actor.Y == b.Actor.Y && a.Actor.Z == b.Actor.Z &&
            a.Actor.CreatureEntry == b.Actor.CreatureEntry && a.Actor.WorldFaction == b.Actor.WorldFaction &&
            a.SpawnId == b.SpawnId && a.Materialized == b.Materialized && a.Alive == b.Alive &&
            a.Bound == b.Bound && a.AtCamp == b.AtCamp && a.Idle == b.Idle && a.Ready == b.Ready;
    }

    // Reuse the existing real entry/faction coalition proposal policy. Exact
    // duplicate observations are harmless; conflicting identities fail closed.
    // A sixth nearby candidate is left room in a second viable party instead
    // of creating a group of five and permanently excluding its lone peer.
    inline std::vector<std::vector<AgentId>> Form(std::vector<Candidate> const& candidates)
    {
        std::map<AgentId, Candidate const*> unique;
        for (auto const& candidate : candidates)
        {
            if (!candidate.Actor.Id) continue;
            auto [it, inserted] = unique.emplace(candidate.Actor.Id, &candidate);
            if (!inserted && it->second && !SameCandidate(*it->second, candidate)) it->second = nullptr;
        }
        std::vector<CoalitionCandidate> eligible;
        for (auto const& [id, candidate] : unique)
            if (candidate && Eligible(*candidate)) eligible.push_back(candidate->Actor);

        CoalitionFormationProfile profile;
        profile.Id = CoalitionFormationProfileId::DefiasLoose;
        profile.Kind = AgentGroupKind::Loose;
        profile.CreatureEntry = CreatureEntry;
        profile.RequiredWorldFaction = WorldFactions::DefiasBrotherhood;
        profile.MinMembers = MinMembers;
        profile.FormationRadius = FormationRadius;
        CoalitionFormationSystem formation;
        std::vector<std::vector<AgentId>> parties;
        while (eligible.size() >= MinMembers)
        {
            profile.MaxMembers = eligible.size() == MaxMembers + 1 ? MaxMembers - 1 : MaxMembers;
            auto proposal = formation.Propose(eligible, profile);
            if (!proposal) break;
            auto members = std::move(proposal->Members);
            std::sort(members.begin(), members.end());
            std::erase_if(eligible, [&](CoalitionCandidate const& actor)
                { return std::binary_search(members.begin(), members.end(), actor.Id); });
            parties.push_back(std::move(members));
        }
        return parties;
    }

    enum class Phase : uint8 { Assembling, Outbound, Raiding, Returning, Complete };
    enum class Decision : uint8 { Wait, Advance, Retreat, Complete };

    struct Party
    {
        uint64 Id = 0, AttemptId = 0, LeaderSpawn = 0;
        // Immutable for the lifetime of the sortie, including missing members.
        std::vector<AgentId> Members;
        uint32 SourceDepot = 0, RouteId = 1, Leg = 0;
        Phase Stage = Phase::Assembling;
        uint64 StartedAtMs = 0, PhaseStartedAtMs = 0, LastProgressAtMs = 0;
        std::string Reason;
    };

    struct MemberObservation
    {
        AgentId Member;
        bool Materialized = false, Alive = false, Bound = false;
        bool InCombat = false, AtAnchor = false, Complete = false;
    };

    struct Assessment
    {
        Decision Action = Decision::Wait;
        uint32 Present = 0, Arrived = 0, Completed = 0, Dead = 0, Unavailable = 0;
        bool Quorum = false, LeaderAvailable = false, TimedOut = false;
        char const* Reason = "WAITING_FOR_MEMBERS";
    };

    inline bool Elapsed(uint64 nowMs, uint64 sinceMs, uint64 limitMs)
    {
        return sinceMs && nowMs >= sinceMs && nowMs - sinceMs >= limitMs;
    }

    inline bool SameObservation(MemberObservation const& a, MemberObservation const& b)
    {
        return a.Member == b.Member && a.Materialized == b.Materialized && a.Alive == b.Alive &&
            a.Bound == b.Bound && a.InCombat == b.InCombat && a.AtAnchor == b.AtAnchor && a.Complete == b.Complete;
    }

    // Returning Complete releases coordination only. It never authorizes a
    // delivery, teleport, action cancellation, or a claim that an absent/dead
    // member got home. Those are per-actor outcomes checked by the caller.
    inline Assessment Assess(Party const& party, std::vector<MemberObservation> const& observations, uint64 nowMs)
    {
        Assessment result;
        auto roster = party.Members;
        std::sort(roster.begin(), roster.end());
        bool validRoster = roster.size() >= MinMembers && roster.size() <= MaxMembers &&
            roster.front() && std::adjacent_find(roster.begin(), roster.end()) == roster.end() &&
            std::binary_search(roster.begin(), roster.end(), AgentId{party.LeaderSpawn});
        if (!party.Id || !party.AttemptId || !validRoster ||
            uint8(party.Stage) > uint8(Phase::Complete))
        {
            result.Action = Decision::Retreat;
            result.Reason = "INVALID_PARTY";
            return result;
        }
        if (party.Stage == Phase::Complete)
        {
            result.Action = Decision::Complete;
            result.Reason = "PARTY_COMPLETE";
            return result;
        }

        bool inCombat = false;
        for (AgentId member : roster)
        {
            MemberObservation const* observed = nullptr;
            bool ambiguous = false;
            for (auto const& observation : observations)
            {
                if (observation.Member != member) continue;
                if (observed && !SameObservation(*observed, observation)) ambiguous = true;
                observed = &observation;
            }
            if (ambiguous || !observed || !observed->Materialized || !observed->Bound)
            {
                ++result.Unavailable;
                continue;
            }
            if (!observed->Alive)
            {
                ++result.Dead;
                continue;
            }
            ++result.Present;
            result.Arrived += observed->AtAnchor;
            result.Completed += observed->Complete;
            result.LeaderAvailable |= member.Value == party.LeaderSpawn;
            inCombat |= observed->InCombat;
        }
        result.Quorum = result.Arrived >= std::max(MinMembers, uint32(roster.size() / 2 + 1));

        uint64 progressAt = party.LastProgressAtMs ? party.LastProgressAtMs :
            (party.PhaseStartedAtMs ? party.PhaseStartedAtMs : party.StartedAtMs);
        bool attemptExpired = Elapsed(nowMs, party.StartedAtMs, AttemptTimeoutMs);
        bool lagExpired = Elapsed(nowMs, progressAt, LagTimeoutMs);
        if (party.Stage == Phase::Returning)
        {
            // Original missing/dead incarnations retain roster slots but do
            // not keep a returning party alive indefinitely. A new incarnation
            // is Bound=false and never participates in this old sortie.
            if (result.Completed == result.Present || attemptExpired || lagExpired)
            {
                result.Action = Decision::Complete;
                result.TimedOut = attemptExpired || lagExpired;
                result.Reason = result.TimedOut ? "RETURN_COORDINATION_EXPIRED" : "RETURNED_MEMBERS_COMPLETE";
            }
            else if (result.Arrived == result.Present)
            {
                result.Action = Decision::Advance;
                result.Reason = "RETURN_ANCHOR_REACHED";
            }
            return result;
        }
        if (inCombat || result.Dead)
        {
            result.Action = Decision::Retreat;
            result.Reason = inCombat ? "MEMBER_IN_COMBAT" : "MEMBER_DIED";
            return result;
        }
        bool assemblyExpired = party.Stage == Phase::Assembling &&
            Elapsed(nowMs, party.PhaseStartedAtMs ? party.PhaseStartedAtMs : party.StartedAtMs, AssemblyTimeoutMs);
        if (attemptExpired || assemblyExpired || lagExpired)
        {
            result.Action = Decision::Retreat;
            result.TimedOut = true;
            result.Reason = attemptExpired ? "ATTEMPT_EXPIRED" : (assemblyExpired ? "ASSEMBLY_EXPIRED" : "MEMBER_LAG_TIMEOUT");
            return result;
        }
        // A majority is observable, but never permission to leave a lagging
        // member behind. Advance only from a fully arrived live bound roster.
        if (result.Present == roster.size() && result.Arrived == roster.size())
        {
            result.Action = Decision::Advance;
            result.Reason = "ALL_MEMBERS_AT_ANCHOR";
        }
        return result;
    }

    inline std::optional<ActionPosition> FormationTarget(AgentId actor, std::vector<AgentId> const& roster,
        ActionPosition const& anchor, float radius = GroupMemberFormation::MaxRadius)
    {
        if (anchor.MapId != MapId || !std::isfinite(anchor.X) || !std::isfinite(anchor.Y) || !std::isfinite(anchor.Z))
            return std::nullopt;
        auto slot = GroupMemberFormation::GetSlot(actor, roster, radius);
        if (!slot) return std::nullopt;
        return ActionPosition{anchor.MapId, anchor.X + slot->X, anchor.Y + slot->Y, anchor.Z};
    }
}
#endif
