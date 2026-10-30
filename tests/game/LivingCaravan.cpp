/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingCaravanRuntime.h"
#include <limits>
#include <set>

namespace
{
    using namespace LivingCaravan;
    ObjectGuid Life(uint32 counter) { return ObjectGuid::Create<HighGuid::Unit>(11328,counter); }
}

TEST_CASE("Caravans use only their two authored heterogeneous Stormwind pairs", "[AIWorld][LivingCaravan]")
{
    REQUIRE(Pairs.size() == 2);
    std::set<uint64> actors;
    std::set<uint32> sources;
    for (auto const& pair : Pairs)
    {
        REQUIRE(Pair(pair.Id) == &pair);
        REQUIRE(FindBySpawn(pair.CarrierSpawn) == &pair);
        REQUIRE(FindBySpawn(pair.EscortSpawn) == &pair);
        REQUIRE(MemberIndex(pair.CarrierSpawn) == 0);
        REQUIRE(MemberIndex(pair.EscortSpawn) == 1);
        REQUIRE(IsCarrier(pair.CarrierSpawn));
        REQUIRE_FALSE(IsCarrier(pair.EscortSpawn));
        REQUIRE(actors.insert(pair.CarrierSpawn).second);
        REQUIRE(actors.insert(pair.EscortSpawn).second);
        REQUIRE(CarrierCapability(pair.CarrierSpawn,MapId,pair.CarrierType,AgentControlMode::AIWorldControlled,true));
        REQUIRE(EscortCapability(pair.EscortSpawn,MapId,AgentType::Guard,AgentControlMode::AIWorldControlled,true));
        for (uint32 source : pair.Sources) REQUIRE(sources.insert(source).second);
        for (bool enabled : {false,true})
            REQUIRE_FALSE(CarrierCapability(pair.EscortSpawn,MapId,pair.CarrierType,AgentControlMode::AIWorldControlled,enabled));
        REQUIRE_FALSE(CarrierCapability(pair.CarrierSpawn,0,pair.CarrierType,AgentControlMode::AIWorldControlled,true));
        REQUIRE_FALSE(CarrierCapability(pair.CarrierSpawn,MapId,pair.CarrierType,AgentControlMode::ObserveOnly,true));
        REQUIRE_FALSE(CarrierCapability(pair.CarrierSpawn,MapId,AgentType::Combatant,AgentControlMode::AIWorldControlled,true));
        REQUIRE_FALSE(EscortCapability(pair.EscortSpawn,MapId,AgentType::Civilian,AgentControlMode::AIWorldControlled,true));
        REQUIRE_FALSE(EscortCapability(pair.EscortSpawn,MapId,AgentType::Guard,AgentControlMode::ObserveOnly,true));
    }
    REQUIRE(sources == std::set<uint32>{1,4,5,6});
    REQUIRE(actors == std::set<uint64>{901040,901041,901042,901043});
    REQUIRE_FALSE(Pair(0));
    REQUIRE_FALSE(FindBySpawn(901039));
    REQUIRE_FALSE(FindBySpawn(901044));
    REQUIRE_FALSE(MemberIndex(901044));
    REQUIRE(CityConsumer(901014)); REQUIRE(CityConsumer(901015));
    REQUIRE_FALSE(CityConsumer(901013));
    REQUIRE(CityConsumer(901014)->Depot == CityDepot);
    REQUIRE(CityCapacity == 120); REQUIRE(CityTarget == 60); REQUIRE(CityLow == 12);
}

TEST_CASE("Each caravan fairly alternates its two villages without remote stock knowledge", "[AIWorld][LivingCaravan]")
{
    for (auto const& pair : Pairs)
    {
        uint32 previous = 0;
        std::array<uint32,2> visits{};
        for (uint32 trip = 0; trip < 20; ++trip)
        {
            uint32 selected = SelectSource(pair,previous);
            REQUIRE(OwnSource(pair,selected));
            REQUIRE(selected != previous);
            REQUIRE(selected == pair.Sources[trip%2]);
            ++visits[trip%2]; previous = selected;
        }
        REQUIRE(visits[0] == visits[1]);
        REQUIRE_FALSE(OwnSource(pair,CityDepot));
        REQUIRE_FALSE(OwnSource(pair,2));
    }
}

TEST_CASE("All village routes have exact depot endpoints and bounded reversible hints", "[AIWorld][LivingCaravan]")
{
    for (uint32 source : {1u,4u,5u,6u})
    {
        REQUIRE(ValidRoute(source));
        auto const& route = Route(source);
        REQUIRE(route.front().SingleFile);
        REQUIRE(route.back().SingleFile);
        REQUIRE(LivingReturnPolicy::SamePosition(route.front().Point,CityPoint));
        REQUIRE(LivingReturnPolicy::SamePosition(route.back().Point,*SourcePoint(source)));
        for (auto const& pair : Pairs)
        {
            for (auto const& point : route)
            {
                auto carrier = Target(pair,pair.CarrierSpawn,point);
                auto escort = Target(pair,pair.EscortSpawn,point);
                REQUIRE(carrier.has_value()); REQUIRE(escort.has_value());
                REQUIRE(LivingReturnPolicy::Distance(point.Point,*carrier) <= FormationRadius+.001f);
                REQUIRE(LivingReturnPolicy::Distance(point.Point,*escort) <= FormationRadius+.001f);
                if (point.SingleFile)
                    REQUIRE(LivingReturnPolicy::SamePosition(*carrier,*escort));
                else REQUIRE(LivingReturnPolicy::Distance(*carrier,*escort) == Approx(3.0f).margin(.001f));
                REQUIRE_FALSE(Target(pair,901050,point).has_value());
            }
        }
        // Reversing the same ordered points preserves every bend and bridge
        // landing. No shortcut chord is manufactured by the return policy.
        for (size_t i = route.size()-1; i > 0; --i)
            REQUIRE(LivingReturnPolicy::Distance(route[i].Point,route[i-1].Point) <= 150.0f);
    }
    REQUIRE(Route(2).empty()); REQUIRE_FALSE(SourcePoint(2)); REQUIRE_FALSE(ValidRoute(2));
    REQUIRE(Route(3).empty()); REQUIRE_FALSE(SourcePoint(3));
    uint32 singleFileBridge = 0;
    for (auto const& waypoint : Route(1))
        if (waypoint.Point.X > -230 && waypoint.Point.X < -150 && waypoint.Point.Y > 830)
        { REQUIRE(waypoint.SingleFile); ++singleFileBridge; }
    REQUIRE(singleFileBridge == 3);
    auto invalid = Route(4)[1]; invalid.Point.X = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(Target(Pairs[0],Pairs[0].CarrierSpawn,invalid).has_value());
    invalid = Route(4)[1]; invalid.Point.MapId = 0;
    REQUIRE_FALSE(Target(Pairs[0],Pairs[0].CarrierSpawn,invalid).has_value());
}

TEST_CASE("Caravan arrivals require stopped physical feet on the same map and visible anchor", "[AIWorld][LivingCaravan]")
{
    auto here = CityPoint;
    REQUIRE(AtWaypoint(here,true,true,CityPoint));
    REQUIRE_FALSE(AtWaypoint(here,false,true,CityPoint));
    REQUIRE_FALSE(AtWaypoint(here,true,false,CityPoint));
    here.MapId = 0; REQUIRE_FALSE(AtWaypoint(here,true,true,CityPoint));
    here = CityPoint; here.Z += 4; REQUIRE_FALSE(AtWaypoint(here,true,true,CityPoint));
    here = CityPoint; here.X += 1.8f; REQUIRE(AtWaypoint(here,true,true,CityPoint));
    here.X += .2f; REQUIRE_FALSE(AtWaypoint(here,true,true,CityPoint));
    here = CityPoint; here.X = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(AtWaypoint(here,true,true,CityPoint));
    REQUIRE_FALSE(AtWaypoint(CityPoint,true,true,CityPoint,0));
    REQUIRE_FALSE(AtWaypoint(CityPoint,true,true,CityPoint,500));
}

TEST_CASE("Caravan trips bind both original incarnations and never reuse stale readiness", "[AIWorld][LivingCaravan]")
{
    State state;
    auto const& pair = Pairs[0];
    REQUIRE(Begin(state,pair,1000,1,Life(1),Life(2)));
    REQUIRE(state.SourceDepotId == 4);
    REQUIRE(BoundLives(state,{Life(1),Life(2)}));
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(3)}));
    REQUIRE_FALSE(BoundLives(state,{Life(2),Life(1)}));
    REQUIRE_FALSE(ReadyBarrier(state));
    state.Ready[0] = true; REQUIRE_FALSE(ReadyBarrier(state));
    state.Ready[1] = true; REQUIRE(ReadyBarrier(state));
    state.ReturningSolo = true; REQUIRE_FALSE(ReadyBarrier(state));
    state.ReturningSolo = false;
    SetPhase(state,Stage::ToVillage,2000,"READY");
    REQUIRE_FALSE(ReadyBarrier(state)); REQUIRE(state.LastProgressAtMs == 2000);
    state.CompletedTrips = 4;
    ResetForLife(state,3000,"ESCORT_DIED");
    REQUIRE(state.Current == Stage::Recover);
    REQUIRE(state.SourceDepotId == 4); REQUIRE(state.CompletedTrips == 4); REQUIRE(state.Trip == 1);
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)}));
    REQUIRE_FALSE(ReadyBarrier(state));
    REQUIRE_FALSE(Begin(state,pair,4000,1,Life(3),Life(4)));
    REQUIRE(Begin(state,pair,4000,2,Life(3),Life(4)));
    REQUIRE(state.SourceDepotId == 5); REQUIRE(state.SourceCursor == 1);
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)}));
    REQUIRE(BoundLives(state,{Life(3),Life(4)}));
}

TEST_CASE("Invalid caravan admission cannot mutate an established trip", "[AIWorld][LivingCaravan]")
{
    State state;
    REQUIRE(Begin(state,Pairs[1],1000,1,Life(1),Life(2)));
    auto before = state;
    auto counterfeit = Pairs[1]; counterfeit.Sources = {4,5};
    REQUIRE_FALSE(Begin(state,counterfeit,2000,2,Life(1),Life(2)));
    REQUIRE_FALSE(Begin(state,Pairs[1],2000,2,Life(1),Life(1)));
    REQUIRE_FALSE(Begin(state,Pairs[1],0,2,Life(1),Life(2)));
    REQUIRE_FALSE(Begin(state,Pairs[1],2000,0,Life(1),Life(2)));
    REQUIRE_FALSE(Begin(state,Pairs[1],2000,2,{},Life(2)));
    REQUIRE(state.Trip == before.Trip); REQUIRE(state.SourceDepotId == before.SourceDepotId);
    REQUIRE(state.Guids == before.Guids); REQUIRE(state.StartedAtMs == before.StartedAtMs);
}

TEST_CASE("An in-place respawn cannot reuse a caravan trip even with the same GUID", "[AIWorld][LivingCaravan]")
{
    State state;
    REQUIRE(Begin(state,Pairs[0],1000,1,Life(1),Life(2)));
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)},{1,1}));
    state.Lives = {4,7};
    REQUIRE(BoundLives(state,{Life(1),Life(2)},{4,7}));
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)},{5,7}));
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)},{4,8}));
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)},{0,7}));
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)},{4,0}));
    REQUIRE_FALSE(BoundLives(state,{Life(2),Life(1)},{4,7}));
    ResetForLife(state,2000,"NEW_LIFE");
    REQUIRE(state.Lives == std::array<uint64,2>{});
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)},{4,7}));
    REQUIRE(Begin(state,Pairs[0],3000,2,Life(1),Life(2)));
    REQUIRE(state.Lives == std::array<uint64,2>{});
    state.Lives = {5,7};
    REQUIRE(BoundLives(state,{Life(1),Life(2)},{5,7}));
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)},{4,7}));
}

TEST_CASE("Real waypoint progress resets bounded waits but cannot extend a trip indefinitely", "[AIWorld][LivingCaravan]")
{
    State state;
    REQUIRE(Begin(state,Pairs[1],1000,1,Life(1),Life(2)));
    REQUIRE_FALSE(TimedOut(state,1000+RallyTimeoutMs-1));
    REQUIRE(TimedOut(state,1000+RallyTimeoutMs));
    SetPhase(state,Stage::ToVillage,61000,"STARTED");
    REQUIRE_FALSE(TimedOut(state,61000+LegTimeoutMs-1));
    REQUIRE(TimedOut(state,61000+LegTimeoutMs));
    state.LastProgressAtMs = 1000+TripTimeoutMs-1;
    REQUIRE(TimedOut(state,1000+TripTimeoutMs));
    REQUIRE_FALSE(TimedOut(state,999));
    SetPhase(state,Stage::Rest,1000+TripTimeoutMs,"RESTING");
    REQUIRE_FALSE(TimedOut(state,1000+TripTimeoutMs+RestMs));
    REQUIRE(LoadMs == 5000); REQUIRE(UnloadMs == 5000); REQUIRE(RestMs == 45000);
    REQUIRE(CargoCapacity == 6); REQUIRE(Reserve == 4);
}

TEST_CASE("A death during the loaded city return releases the survivor without recruiting the respawn", "[AIWorld][LivingCaravan]")
{
    State state;
    REQUIRE(Begin(state,Pairs[0],1000,1,Life(1),Life(2)));
    state.Lives = {3,4};
    SetPhase(state,Stage::ToCity,2000,"FOOD_ON_BOARD");
    state.Waypoint = 3;
    state.Ready = {true,false};
    RefreshReturnMode(state,true,true);
    REQUIRE_FALSE(state.ReturningSolo);
    REQUIRE_FALSE(ReadyBarrier(state));
    RefreshReturnMode(state,true,false); // Escort died while the carrier was already returning.
    REQUIRE(state.ReturningSolo);
    REQUIRE(state.Ready[0]);
    REQUIRE(state.Current == Stage::ToCity);
    REQUIRE(state.Waypoint == 3); // Release coordination, never manufacture physical progress.
    REQUIRE(state.Guids[1] == Life(2));
    REQUIRE(state.Lives[1] == 4);
    REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)},{3,5})); // In-place respawn is a different escort.
    RefreshReturnMode(state,true,false);
    RefreshReturnMode(state,true,true);
    REQUIRE(state.ReturningSolo); // Even a later complete observation cannot rejoin this trip.
    REQUIRE(Begin(state,Pairs[0],3000,2,Life(1),Life(2)));
    REQUIRE_FALSE(state.ReturningSolo);
    REQUIRE(state.Current == Stage::Rally);
    REQUIRE(state.SourceDepotId == 5);
}

TEST_CASE("A surviving escort can return after carrier death and solo mode affects only returning trips", "[AIWorld][LivingCaravan]")
{
    State state;
    REQUIRE(Begin(state,Pairs[1],1000,1,Life(1),Life(2)));
    RefreshReturnMode(state,false,true);
    REQUIRE_FALSE(state.ReturningSolo); // Outbound loss still requires the runtime's safety transition.
    SetPhase(state,Stage::ToCity,2000,"CARRIER_DIED");
    state.Ready = {false,true};
    RefreshReturnMode(state,false,true);
    REQUIRE(state.ReturningSolo);
    REQUIRE(state.Ready[1]);
    REQUIRE(state.Current == Stage::ToCity);
    SetPhase(state,Stage::Rest,3000,"SURVIVOR_AT_CITY");
    RefreshReturnMode(state,true,true);
    REQUIRE(state.ReturningSolo);
    REQUIRE(Begin(state,Pairs[1],4000,2,Life(3),Life(2)));
    REQUIRE_FALSE(state.ReturningSolo);
    SetPhase(state,Stage::ToCity,5000,"PAIRED_RETURN");
    RefreshReturnMode(state,true,true);
    REQUIRE_FALSE(state.ReturningSolo);
}

TEST_CASE("The same original lives can start a fresh trip after a solo return and real city reunion", "[AIWorld][LivingCaravan]")
{
    State state;
    REQUIRE(Begin(state,Pairs[0],1000,1,Life(1),Life(2)));
    state.Lives = {3,4};
    SetPhase(state,Stage::ToCity,2000,"FOOD_ON_BOARD");
    RefreshReturnMode(state,true,false); // Temporary loss of binding, not a new life.
    REQUIRE(state.ReturningSolo);
    state.Ready = {true,false};
    REQUIRE_FALSE(ReadyForNextTrip(state));
    RefreshReturnMode(state,true,true);
    state.Ready = {true,true};
    REQUIRE(state.ReturningSolo);
    REQUIRE_FALSE(ReadyBarrier(state));
    REQUIRE_FALSE(ReadyForNextTrip(state)); // Reunion cannot rejoin the old return mid-route.
    SetPhase(state,Stage::Rest,3000,"DELIVERY_FINISHED");
    REQUIRE_FALSE(ReadyForNextTrip(state)); // Reobserve stopped city feet after the phase change.
    state.Ready = {true,true};
    REQUIRE(BoundLives(state,{Life(1),Life(2)},{3,4}));
    REQUIRE(state.ReturningSolo);
    REQUIRE_FALSE(ReadyBarrier(state));
    REQUIRE(ReadyForNextTrip(state));
    REQUIRE(Begin(state,Pairs[0],3000+RestMs,2,Life(1),Life(2)));
    REQUIRE_FALSE(state.ReturningSolo);
    REQUIRE(state.Current == Stage::Rally);
    REQUIRE(state.SourceDepotId == 5);
    REQUIRE_FALSE(ReadyForNextTrip(state));
    REQUIRE_FALSE(ReadyBarrier(state));
}

TEST_CASE("Next-trip admission requires a rested complete bound roster and cannot admit outbound replacements", "[AIWorld][LivingCaravan]")
{
    State state;
    REQUIRE(Begin(state,Pairs[1],1000,1,Life(1),Life(2)));
    state.Lives = {7,9};
    state.Ready = {true,true};
    for (Stage stage : {Stage::Rally,Stage::ToVillage,Stage::Loading,Stage::ToCity,Stage::Unloading,Stage::Recover})
    {
        state.Current = stage;
        REQUIRE_FALSE(ReadyForNextTrip(state));
    }
    state.Current = Stage::Rest;
    REQUIRE(ReadyForNextTrip(state));
    SECTION("missing or dead escort") { state.Ready[1] = false; }
    SECTION("missing or dead carrier") { state.Ready[0] = false; }
    SECTION("escort is a replacement incarnation")
    {
        REQUIRE_FALSE(BoundLives(state,{Life(1),Life(2)},{7,10}));
        state.Ready[1] = false; // Runtime readyAt rejects non-original incarnations.
    }
    SECTION("unbound escort") { state.Guids[1] = {}; }
    SECTION("unbound carrier") { state.Guids[0] = {}; }
    SECTION("missing escort lifetime") { state.Lives[1] = 0; }
    SECTION("missing carrier lifetime") { state.Lives[0] = 0; }
    SECTION("no established trip") { state.Trip = 0; }
    REQUIRE_FALSE(ReadyForNextTrip(state));
}
