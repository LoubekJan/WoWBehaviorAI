/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingRaidParty.h"
#include <limits>
#include <set>

namespace
{
    using namespace LivingRaidParty;

    Candidate Ready(uint64 spawn, float x = 0.0f)
    {
        Candidate candidate;
        candidate.Actor = {AgentId{spawn}, MapId, x, 0.0f, 0.0f, CreatureEntry, WorldFactions::DefiasBrotherhood};
        candidate.SpawnId = spawn;
        candidate.Materialized = candidate.Alive = candidate.Bound = candidate.AtCamp = candidate.Idle = candidate.Ready = true;
        return candidate;
    }

    std::vector<Candidate> Candidates(uint32 count = 10)
    {
        std::vector<Candidate> result;
        for (uint64 spawn = 901050; spawn < 901050 + count; ++spawn) result.push_back(Ready(spawn, float(spawn - 901050)));
        return result;
    }

    std::set<AgentId> UniqueMembers(std::vector<std::vector<AgentId>> const& parties)
    {
        std::set<AgentId> result;
        for (auto const& party : parties)
        {
            REQUIRE(party.size() >= MinMembers);
            REQUIRE(party.size() <= MaxMembers);
            for (AgentId member : party) REQUIRE(result.insert(member).second);
        }
        return result;
    }

    Party Sortie(uint32 count = 5)
    {
        Party party;
        party.Id = party.AttemptId = 1;
        party.LeaderSpawn = 901050;
        party.StartedAtMs = party.PhaseStartedAtMs = party.LastProgressAtMs = 1000;
        party.Stage = Phase::Outbound;
        for (uint64 spawn = 901050; spawn < 901050 + count; ++spawn) party.Members.push_back(AgentId{spawn});
        return party;
    }

    std::vector<MemberObservation> Members(Party const& party, bool arrived = true)
    {
        std::vector<MemberObservation> result;
        for (AgentId member : party.Members) result.push_back({member, true, true, true, false, arrived, false});
        return result;
    }
}

TEST_CASE("All ten reviewed Defias form deterministic separate session parties", "[AIWorld][LivingRaidParty]")
{
    auto candidates = Candidates();
    auto parties = Form(candidates);
    REQUIRE(parties.size() == 2);
    REQUIRE(parties[0].size() == 5);
    REQUIRE(parties[1].size() == 5);
    REQUIRE(UniqueMembers(parties).size() == 10);
    std::reverse(candidates.begin(), candidates.end());
    REQUIRE(Form(candidates) == parties);
    for (uint32 count : {2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        REQUIRE(UniqueMembers(Form(Candidates(count))).size() == count);
    REQUIRE(Form(Candidates(1)).empty());
}

TEST_CASE("Raid party readiness respects genuine identity and observed capability", "[AIWorld][LivingRaidParty]")
{
    REQUIRE(Eligible(Ready(901050)));
    for (uint64 spawn : {0ull, 901049ull, 901060ull, 9010550ull}) REQUIRE_FALSE(Eligible(Ready(spawn)));
    auto bad = Ready(901050);
    bad.Actor.Id = AgentId{901051};
    REQUIRE_FALSE(Eligible(bad));
    bad = Ready(901050); bad.Actor.MapId = 0;
    REQUIRE_FALSE(Eligible(bad));
    bad = Ready(901050); bad.Actor.CreatureEntry = 38;
    REQUIRE_FALSE(Eligible(bad));
    bad = Ready(901050); bad.Actor.WorldFaction = WorldFactions::StormwindAlliance;
    REQUIRE_FALSE(Eligible(bad));
    for (bool Candidate::* flag : {&Candidate::Materialized, &Candidate::Alive, &Candidate::Bound,
        &Candidate::AtCamp, &Candidate::Idle, &Candidate::Ready})
    {
        bad = Ready(901050); bad.*flag = false;
        REQUIRE_FALSE(Eligible(bad));
    }
    for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()})
        for (float CoalitionCandidate::* coordinate : {&CoalitionCandidate::X, &CoalitionCandidate::Y, &CoalitionCandidate::Z})
        {
            bad = Ready(901050); bad.Actor.*coordinate = value;
            REQUIRE_FALSE(Eligible(bad));
            REQUIRE(Form({bad, Ready(901051)}).empty());
        }
}

TEST_CASE("Duplicate identities and distant observations cannot inflate a party", "[AIWorld][LivingRaidParty]")
{
    auto candidates = Candidates();
    auto duplicates = candidates;
    candidates.insert(candidates.end(), duplicates.begin(), duplicates.end());
    REQUIRE(UniqueMembers(Form(candidates)).size() == 10);
    auto conflict = Ready(901050); conflict.Bound = false;
    candidates.push_back(conflict);
    auto members = UniqueMembers(Form(candidates));
    REQUIRE(members.size() == 9);
    REQUIRE_FALSE(members.contains(AgentId{901050}));
    std::reverse(candidates.begin(), candidates.end());
    REQUIRE(UniqueMembers(Form(candidates)) == members);
    auto distant = Candidates();
    distant.back().Actor.X = 1000;
    members = UniqueMembers(Form(distant));
    REQUIRE(members.size() == 9);
    REQUIRE_FALSE(members.contains(AgentId{901059}));
    REQUIRE(Form({Ready(901050, 0), Ready(901051, 100.001f)}).empty());
    REQUIRE(Form({Ready(901050, 0), Ready(901051, 100)}).size() == 1);
}

TEST_CASE("Outbound parties advance together and retain a bounded wait for a lagging member", "[AIWorld][LivingRaidParty]")
{
    auto party = Sortie();
    auto members = Members(party);
    auto assessment = Assess(party, members, 2000);
    REQUIRE(assessment.Action == Decision::Advance);
    REQUIRE(assessment.Present == 5);
    REQUIRE(assessment.Arrived == 5);
    REQUIRE(assessment.Quorum);
    REQUIRE(assessment.LeaderAvailable);
    members.back().AtAnchor = false;
    assessment = Assess(party, members, 2000);
    REQUIRE(assessment.Action == Decision::Wait);
    REQUIRE(assessment.Quorum);
    REQUIRE(Assess(party, members, 1000 + LagTimeoutMs - 1).Action == Decision::Wait);
    assessment = Assess(party, members, 1000 + LagTimeoutMs);
    REQUIRE(assessment.Action == Decision::Retreat);
    REQUIRE(assessment.TimedOut);
    for (uint32 i = 2; i < members.size(); ++i) members[i].AtAnchor = false;
    REQUIRE_FALSE(Assess(party, members, 2000).Quorum);
    REQUIRE(party.Members.size() == 5);
}

TEST_CASE("Party combat and actual original incarnation death request a retreat", "[AIWorld][LivingRaidParty]")
{
    auto party = Sortie();
    auto members = Members(party);
    members.back().InCombat = true;
    REQUIRE(Assess(party, members, 2000).Action == Decision::Retreat);
    members.back().InCombat = false;
    members.back().Alive = false;
    auto assessment = Assess(party, members, 2000);
    REQUIRE(assessment.Action == Decision::Retreat);
    REQUIRE(assessment.Dead == 1);
    REQUIRE(assessment.Present == 4);
    members.back().Materialized = false;
    assessment = Assess(party, members, 2000);
    REQUIRE(assessment.Action == Decision::Wait);
    REQUIRE(assessment.Dead == 0);
    REQUIRE(assessment.Unavailable == 1);
    members = Members(party);
    members.back().Bound = false;
    REQUIRE(Assess(party, members, 2000).Action == Decision::Wait);
    REQUIRE(Assess(party, members, 2000).Present == 4);
}

TEST_CASE("Foreign and contradictory observations cannot authorize party advancement", "[AIWorld][LivingRaidParty]")
{
    auto party = Sortie();
    auto members = Members(party);
    members.push_back({AgentId{42}, true, false, true, true, false, false});
    REQUIRE(Assess(party, members, 2000).Action == Decision::Advance);
    REQUIRE(Assess(party, members, 2000).Dead == 0);
    members.push_back(members.front());
    REQUIRE(Assess(party, members, 2000).Present == 5);
    auto contradictory = members.front(); contradictory.AtAnchor = false;
    members.push_back(contradictory);
    auto assessment = Assess(party, members, 2000);
    REQUIRE(assessment.Action == Decision::Wait);
    REQUIRE(assessment.Present == 4);
    REQUIRE_FALSE(assessment.LeaderAvailable);
    std::reverse(members.begin(), members.end());
    REQUIRE(Assess(party, members, 2000).Action == assessment.Action);
}

TEST_CASE("Returning parties release coordination without fabricating absent member deliveries", "[AIWorld][LivingRaidParty]")
{
    auto party = Sortie();
    auto roster = party.Members;
    party.Stage = Phase::Returning;
    auto members = Members(party);
    members[0].Alive = false;
    members[1].Materialized = false;
    members[2].Bound = false; // a respawn/new incarnation cannot join an old trip
    members[3].Complete = members[4].Complete = true;
    auto assessment = Assess(party, members, 2000);
    REQUIRE(assessment.Action == Decision::Complete);
    REQUIRE(assessment.Present == 2);
    REQUIRE(assessment.Completed == 2);
    REQUIRE(assessment.Dead == 1);
    REQUIRE(assessment.Unavailable == 2);
    REQUIRE(party.Members == roster);
    members[3].Complete = members[4].Complete = false;
    REQUIRE(Assess(party, members, 2000).Action == Decision::Advance);
    members[4].AtAnchor = false;
    REQUIRE(Assess(party, members, 2000).Action == Decision::Wait);
    assessment = Assess(party, members, 1000 + LagTimeoutMs);
    REQUIRE(assessment.Action == Decision::Complete);
    REQUIRE(assessment.TimedOut);
    REQUIRE(assessment.Completed == 0);
    assessment = Assess(party, {}, 2000);
    REQUIRE(assessment.Action == Decision::Complete);
    REQUIRE(assessment.Completed == 0);
}

TEST_CASE("Assembly and attempt deadlines are bounded and safe across time regression", "[AIWorld][LivingRaidParty]")
{
    auto party = Sortie();
    auto members = Members(party, false);
    party.Stage = Phase::Assembling;
    REQUIRE(Assess(party, members, 1000 + AssemblyTimeoutMs - 1).Action == Decision::Wait);
    REQUIRE(Assess(party, members, 1000 + AssemblyTimeoutMs).Action == Decision::Retreat);
    party.Stage = Phase::Outbound;
    party.LastProgressAtMs = 1000 + AttemptTimeoutMs;
    REQUIRE(Assess(party, members, 1000 + AttemptTimeoutMs).Action == Decision::Retreat);
    party.StartedAtMs = party.PhaseStartedAtMs = party.LastProgressAtMs = 10000;
    REQUIRE_FALSE(Assess(party, members, 9000).TimedOut);
    REQUIRE(Assess(party, members, 9000).Action == Decision::Wait);
    party.StartedAtMs = party.PhaseStartedAtMs = party.LastProgressAtMs = 0;
    REQUIRE_FALSE(Assess(party, members, std::numeric_limits<uint64>::max()).TimedOut);
}

TEST_CASE("Invalid party identities and rosters fail closed", "[AIWorld][LivingRaidParty]")
{
    auto party = Sortie();
    auto members = Members(party);
    party.Members.push_back(party.Members.front());
    REQUIRE(Assess(party, members, 2000).Action == Decision::Retreat);
    party = Sortie(); party.LeaderSpawn = 42;
    REQUIRE(Assess(party, members, 2000).Action == Decision::Retreat);
    party = Sortie(); party.Id = 0;
    REQUIRE(Assess(party, members, 2000).Action == Decision::Retreat);
    party = Sortie(); party.AttemptId = 0;
    REQUIRE(Assess(party, members, 2000).Action == Decision::Retreat);
    party = Sortie(); party.Stage = Phase(255);
    REQUIRE(Assess(party, members, 2000).Action == Decision::Retreat);
    party = Sortie(); party.Members.clear();
    REQUIRE(Assess(party, members, 2000).Action == Decision::Retreat);
}

TEST_CASE("Raid formation targets reuse stable existing group slots without changing the anchor", "[AIWorld][LivingRaidParty]")
{
    auto party = Sortie();
    ActionPosition anchor{MapId, 10, 20, 30};
    auto target = FormationTarget(party.Members.front(), party.Members, anchor);
    auto slot = GroupMemberFormation::GetSlot(party.Members.front(), party.Members, GroupMemberFormation::MaxRadius);
    REQUIRE(target.has_value());
    REQUIRE(slot.has_value());
    REQUIRE(target->X == anchor.X + slot->X);
    REQUIRE(target->Y == anchor.Y + slot->Y);
    REQUIRE(target->Z == anchor.Z);
    REQUIRE(anchor.X == 10);
    std::reverse(party.Members.begin(), party.Members.end());
    auto reordered = FormationTarget(AgentId{901050}, party.Members, anchor);
    REQUIRE(reordered->X == target->X);
    REQUIRE(reordered->Y == target->Y);
    REQUIRE_FALSE(FormationTarget(AgentId{42}, party.Members, anchor).has_value());
    REQUIRE_FALSE(FormationTarget(AgentId{901050}, party.Members, anchor, 0).has_value());
    anchor.Z = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(FormationTarget(AgentId{901050}, party.Members, anchor).has_value());
}
