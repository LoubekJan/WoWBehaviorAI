/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingResidentsV11.h"
#include "Agent/LivingResidentsV11Locations.h"
#include "Agent/LivingSupplyState.h"
#include "Agent/NpcAgentTemplateAliases.h"
#include <map>
#include <set>

namespace
{
    LivingSupply::Ledger ResidentFixture()
    {
        LivingSupply::Ledger value;
        for (uint32 id = 1; id <= 6; ++id) REQUIRE(value.ConfigureDepot(id));
        REQUIRE(value.ConfigureResidentsV11());
        for (uint32 id = 1; id <= 3; ++id) REQUIRE(value.ConfigureField(id, 1));
        for (uint64 spawn = 902000; spawn <= 902044; ++spawn)
        {
            auto job = LivingResidentsV11::Role(spawn);
            auto depot = LivingResidentsV11::Depot(spawn);
            if (job == LivingResidentsV11::Job::Farmer) REQUIRE(value.RegisterFarmer(spawn, depot, depot));
            else if (job == LivingResidentsV11::Job::Carrier) REQUIRE(value.RegisterCarrier(spawn, 5));
            else if (job == LivingResidentsV11::Job::Raider) REQUIRE(value.RegisterRaider(spawn, 6, 1, 2, true));
            else REQUIRE(value.RegisterActor(spawn, depot, 0, false, true));
        }
        return value;
    }
    void Produce(LivingSupply::Ledger& ledger, uint32 depot, uint32 portions)
    {
        uint64 farmer = 902000 + (depot-1)*4;
        for (uint32 part = 0; part < portions; ++part)
        {
            REQUIRE(ledger.HarvestField(farmer, 2*part+1, depot, 1, 1, true));
            REQUIRE(ledger.Deliver(farmer, 2*part+2, depot, 1, true));
        }
    }
}

TEST_CASE("V11 capability accepts exactly the newly reviewed 45 controlled residents", "[AIWorld][LivingResidentsV11]")
{
    using namespace LivingResidentsV11;
    std::map<Job, uint32> counts;
    for (uint64 spawn = First; spawn <= Last; ++spawn)
    {
        auto job = Role(spawn);
        ++counts[job];
        auto type = job == Job::Raider ? AgentType::Combatant : AgentType::Civilian;
        REQUIRE(Capability(spawn, 725, type, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(Capability(spawn, 0, type, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(Capability(spawn, 725, type, AgentControlMode::ObserveOnly, true));
        REQUIRE_FALSE(Capability(spawn, 725, type, AgentControlMode::AIWorldControlled, false));
        REQUIRE_FALSE(Capability(spawn, 725, AgentType::Guard, AgentControlMode::AIWorldControlled, true));
    }
    REQUIRE(counts[Job::Farmer] == 12);
    REQUIRE(counts[Job::Consumer] == 21);
    REQUIRE(counts[Job::Carrier] == 2);
    REQUIRE(counts[Job::Raider] == 10);
    for (uint64 foreign : {0ull, 901000ull, 901099ull, 901999ull, 902045ull})
        REQUIRE_FALSE(Capability(foreign, 725, AgentType::Civilian, AgentControlMode::AIWorldControlled, true));
    SimulationScope scope;
    scope.MapId = 725; scope.ZoneIds = {4988}; scope.Bounds = SimulationBounds{-532,532,1,1065};
    for (uint64 spawn = First; spawn <= Last; ++spawn) scope.SpawnIds.push_back(spawn);
    REQUIRE(ScopeValid(scope));
    scope.SpawnIds.back() = 901099; REQUIRE_FALSE(ScopeValid(scope));
    scope.SpawnIds.back() = Last; scope.Bounds.reset(); REQUIRE_FALSE(ScopeValid(scope));
}

TEST_CASE("V11 ledger is an explicit empty-ledger opt in without changing original field authorities", "[AIWorld][LivingResidentsV11]")
{
    LivingSupply::Ledger original;
    for (uint32 id = 1; id <= 6; ++id) REQUIRE(original.ConfigureDepot(id));
    REQUIRE_FALSE(original.ConfigureField(1, 1));
    REQUIRE(original.ConfigureField(4, 1));
    REQUIRE_FALSE(original.ConfigureResidentsV11());
    REQUIRE_FALSE(original.RegisterCarrier(902016, 5));
    REQUIRE_FALSE(original.RegisterRaider(902035, 6, 1, 2, true));
    auto residents = ResidentFixture();
    REQUIRE_FALSE(residents.ConfigureField(4, 1));
    REQUIRE_FALSE(residents.ConfigureResidentsV11());
    REQUIRE_FALSE(residents.RegisterRaider(902035, 3));
    REQUIRE_FALSE(residents.RegisterCarrier(902016));
    REQUIRE(residents.GetCounters().InitialFood == 0);
    REQUIRE(residents.DepotFood() == 0);
    REQUIRE(residents.Conserved());
    REQUIRE(residents.PotentialConserved());
}

TEST_CASE("V11 completed work transfers crop potential through cargo and the actual village depot", "[AIWorld][LivingResidentsV11]")
{
    auto ledger = ResidentFixture();
    REQUIRE_FALSE(ledger.HarvestField(902000, 1, 1, 1, 1, false));
    REQUIRE_FALSE(ledger.HarvestField(902000, 1, 2, 1, 1, true));
    REQUIRE_FALSE(ledger.HarvestField(902012, 1, 1, 1, 1, true));
    REQUIRE(ledger.HarvestField(902000, 1, 1, 1, 1, true));
    REQUIRE(ledger.GetCargo(902000) == 1);
    REQUIRE(ledger.GetDepot(1)->Stock == 0);
    REQUIRE_FALSE(ledger.Deliver(902000, 2, 1, 1, false));
    REQUIRE_FALSE(ledger.Deliver(902000, 2, 5, 1, true));
    REQUIRE(ledger.Deliver(902000, 2, 1, 1, true));
    REQUIRE(ledger.Deliver(902000, 2, 1, 1, true).Duplicate);
    REQUIRE(ledger.GetDepot(1)->Stock == 1);
    REQUIRE(ledger.GetCargo(902000) == 0);
    REQUIRE_FALSE(ledger.EatDepot(902012, 1, 1, 1, true));
    REQUIRE(ledger.EatDepot(902000, 3, 1, 1, true));
    REQUIRE(ledger.Conserved()); REQUIRE(ledger.PotentialConserved());
}

TEST_CASE("V11 haulers preserve village reserves and deliver only to the two dependent settlements", "[AIWorld][LivingResidentsV11]")
{
    auto ledger = ResidentFixture(); Produce(ledger, 1, 12);
    REQUIRE_FALSE(ledger.Load(902016, 1, 1, 6, false));
    REQUIRE_FALSE(ledger.Load(902016, 1, 4, 6, true));
    REQUIRE(ledger.Load(902016, 1, 1, 6, true));
    REQUIRE(ledger.Load(902016, 1, 1, 6, true).Duplicate);
    REQUIRE(ledger.GetDepot(1)->Stock == 6);
    REQUIRE(ledger.GetCargo(902016) == 6);
    REQUIRE_FALSE(ledger.Deliver(902016, 2, 6, 6, true));
    REQUIRE(ledger.Deliver(902016, 2, 4, 3, true));
    REQUIRE(ledger.Deliver(902016, 3, 5, 3, true));
    REQUIRE(ledger.EatDepot(902012, 1, 4, 1, true));
    REQUIRE(ledger.EatDepot(902016, 4, 4, 1, true));
    REQUIRE(ledger.EatDepot(902018, 1, 5, 1, true));
    REQUIRE_FALSE(ledger.Load(902018, 2, 1, 6, true));
    REQUIRE(ledger.Load(902017, 1, 1, 6, true).Portions == 2);
    REQUIRE(ledger.GetDepot(1)->Stock == 4);
    REQUIRE_FALSE(ledger.Load(902017, 2, 1, 1, true));
    REQUIRE(ledger.DropCargo(902017) == 2);
    REQUIRE(ledger.Conserved()); REQUIRE(ledger.PotentialConserved());
}

TEST_CASE("V11 theft debits a real village then credits camp only after owned delivery", "[AIWorld][LivingResidentsV11]")
{
    auto ledger = ResidentFixture(); Produce(ledger, 3, 8);
    REQUIRE_FALSE(ledger.Steal(902035, 1, 3, 2, false));
    REQUIRE_FALSE(ledger.Steal(902035, 1, 5, 2, true));
    REQUIRE_FALSE(ledger.Steal(902016, 1, 3, 2, true));
    REQUIRE(ledger.Steal(902035, 1, 3, 2, true));
    REQUIRE(ledger.Steal(902035, 1, 3, 2, true).Duplicate);
    REQUIRE(ledger.GetDepot(3)->Stock == 6);
    REQUIRE(ledger.GetDepot(6)->Stock == 0);
    REQUIRE(ledger.GetCargo(902035) == 2);
    REQUIRE_FALSE(ledger.Deliver(902035, 2, 5, 2, true));
    REQUIRE(ledger.Deliver(902035, 2, 6, 2, true));
    REQUIRE(ledger.EatDepot(902035, 3, 6, 1, true));
    REQUIRE(ledger.GetDepot(6)->Stock == 1);
    REQUIRE(ledger.Conserved()); REQUIRE(ledger.PotentialConserved());
}

TEST_CASE("V11 source rotation and demand allocation cover actual producers without inventing stock", "[AIWorld][LivingResidentsV11]")
{
    using namespace LivingResidentsV11;
    REQUIRE_FALSE(Source({0,0,0,0}, 0, false));
    REQUIRE(Source({10,10,10,10}, 0, false) == 1);
    REQUIRE(Source({10,10,10,10}, 1, false) == 2);
    REQUIRE(Source({10,10,10,10}, 3, false) == 1);
    REQUIRE_FALSE(Source({0,0,0,20}, 3, false));
    REQUIRE(Source({0,0,0,20}, 3, true) == 4);
    REQUIRE(DeliveryDepot(0,0) == 4);
    REQUIRE(DeliveryDepot(6,0) == 5);
    REQUIRE(DeliveryDepot(4,20) == 4);
    REQUIRE(DeliveryDepot(12,20) == 5);
    REQUIRE(DeliveryDepot(12,48) == 0);
}

TEST_CASE("Every v11 custom template resolves to its own source and rejects unbound clones", "[AIWorld][LivingResidentsV11][template]")
{
    NpcAgentTemplateAliases aliases;
    for (uint64 spawn = 902000; spawn <= 902044; ++spawn)
    {
        NpcAgentTemplateAlias value{spawn,725,NpcAgentTemplateEntry(spawn),spawn < 902035 ? 11328u : 38u,"Resident",""};
        NpcAgentTemplateBinding binding{spawn,725,value.Entry,true,true,true,true,true};
        REQUIRE(value.Entry == 9810000 + spawn-902000);
        REQUIRE(aliases.Add(value,binding));
        REQUIRE(aliases.Resolve(725,spawn,value.Entry) == value.BaseEntry);
        REQUIRE(aliases.ProfileFailure(725,spawn,value.Entry,"Resident") == nullptr);
        REQUIRE(aliases.ProfileFailure(725,spawn == 902044 ? 902000 : spawn+1,value.Entry,"Resident") != nullptr);
    }
    REQUIRE(aliases.Size() == 45);
    REQUIRE(NpcAgentTemplateEntry(902045) == 0);
    REQUIRE(NpcAgentTemplateEntry(901999) == 0);
    REQUIRE(NpcAgentTemplateEntry(901035) == 9800035);
}

TEST_CASE("V11 depot routes preserve proved short anchors for all supply and home-return combinations", "[AIWorld][LivingResidentsV11]")
{
    using namespace LivingResidentsV11;
    REQUIRE(Locations::Depots.size() == 6);
    REQUIRE(Locations::Fields.size() == 3);
    REQUIRE(Locations::Routes.size() == 16);
    for (uint32 from = 1; from <= 6; ++from)
        for (uint32 to = 1; to <= 6; ++to)
        {
            if (from == to) continue;
            auto path = TransitRoute(from, to, Locations::Routes);
            REQUIRE_FALSE(path.empty());
            auto const& origin = Locations::Depots[from-1].Point;
            auto const& destination = Locations::Depots[to-1].Point;
            REQUIRE(LivingReturnPolicy::Distance(path.front(), origin) <= .2f);
            REQUIRE(LivingReturnPolicy::Distance(path.back(), destination) <= .2f);
            for (std::size_t i = 1; i < path.size(); ++i)
            {
                REQUIRE(LivingReturnPolicy::Finite(path[i]));
                REQUIRE(path[i].MapId == 725);
                REQUIRE(LivingReturnPolicy::Distance(path[i-1], path[i]) <= 180.0f);
            }
        }
    // Interrupted camp/city trips also have a complete graph connection;
    // no long direct query is substituted when the nearest depot changes.
    REQUIRE_FALSE(TransitRoute(5, 6, Locations::Routes).empty());
    REQUIRE_FALSE(TransitRoute(6, 5, Locations::Routes).empty());
    REQUIRE(TransitRoute(1, 1, Locations::Routes).empty());
}
TEST_CASE("V11 retargeting retains the physical coast exit before joining a new depot route", "[AIWorld][LivingResidentsV11]")
{
    using namespace LivingResidentsV11;
    State state;
    state.TravelDepot = 4;
    state.TravelGoal = Locations::Depots[3].Point;
    state.Waypoints = TransitRoute(5, 4, Locations::Routes);
    REQUIRE(state.Waypoints.size() > 3);
    state.Waypoint = state.Waypoints.size()/2;
    auto oldRemaining = std::vector<ActionPosition>(state.Waypoints.begin()+state.Waypoint, state.Waypoints.end());
    // Near the distant coast, depot1 happens to be Euclidean-nearest. Its
    // route starts on a different side of the obstacle and is not an exit.
    RetargetJourney(state, Locations::Depots[5].Point, 6, 1, Locations::Routes);
    REQUIRE(state.Waypoint == 0);
    REQUIRE(state.TravelDepot == 6);
    REQUIRE(state.Waypoints.size() > oldRemaining.size());
    for (std::size_t i = 0; i < oldRemaining.size(); ++i)
        REQUIRE(LivingReturnPolicy::SamePosition(state.Waypoints[i], oldRemaining[i]));
    REQUIRE(LivingReturnPolicy::Distance(state.Waypoints.back(), Locations::Depots[5].Point) < .2f);
    for (std::size_t i = 1; i < state.Waypoints.size(); ++i)
        REQUIRE(LivingReturnPolicy::Distance(state.Waypoints[i-1],state.Waypoints[i]) <= 180.0f);
}
