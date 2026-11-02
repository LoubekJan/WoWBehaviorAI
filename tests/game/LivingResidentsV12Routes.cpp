/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingResidentsV11Locations.h"
#include "Agent/LivingResidentsV12Routes.h"
#include <set>

namespace
{
    float ResidentRouteLength(std::vector<ActionPosition> const& points)
    {
        float length = 0;
        for (std::size_t i = 1; i < points.size(); ++i)
            length += LivingReturnPolicy::Distance(points[i-1],points[i]);
        return length;
    }
    void SameResidentRoute(std::vector<ActionPosition> const& actual, std::vector<ActionPosition> const& expected)
    {
        REQUIRE(actual.size() == expected.size());
        for (std::size_t n = 0; n < expected.size(); ++n)
            REQUIRE(LivingReturnPolicy::SamePosition(actual[n],expected[n]));
    }
    bool ResidentRouteEquals(std::vector<ActionPosition> const& actual, std::vector<ActionPosition> const& expected)
    {
        return actual.size() == expected.size() && std::equal(actual.begin(),actual.end(),expected.begin(),
            [](ActionPosition const& a,ActionPosition const& b) { return LivingReturnPolicy::SamePosition(a,b); });
    }
}

TEST_CASE("The v12 catalog supplies every direct depot journey with bounded physically reviewed targets", "[AIWorld][LivingResidentsV12][BridgeRoutes]")
{
    using namespace LivingResidentsV11;
    REQUIRE(Locations::Routes.size() == 16); // Historical v11 catalog remains separate.
    REQUIRE(LivingResidentsV12::Routes.size() == 30);
    std::set<std::pair<uint32,uint32>> pairs;
    for (auto const& route : LivingResidentsV12::Routes)
    {
        REQUIRE(route.From >= 1);
        REQUIRE(route.From <= 6);
        REQUIRE(route.To >= 1);
        REQUIRE(route.To <= 6);
        REQUIRE(route.From != route.To);
        REQUIRE(pairs.emplace(route.From,route.To).second);
        REQUIRE(route.Points.size() >= 2);
        REQUIRE(route.Points.size() <= 64);
        REQUIRE(LivingReturnPolicy::SamePosition(route.Points.front(),Locations::Depots[route.From-1].Point));
        REQUIRE(LivingReturnPolicy::SamePosition(route.Points.back(),Locations::Depots[route.To-1].Point));
        for (std::size_t n = 0; n < route.Points.size(); ++n)
        {
            REQUIRE(LivingReturnPolicy::Finite(route.Points[n]));
            REQUIRE(route.Points[n].MapId == 725);
            if (n) REQUIRE(LivingReturnPolicy::Distance(route.Points[n-1],route.Points[n]) <= 160);
        }
        auto selected = TransitRoute(route.From,route.To,LivingResidentsV12::Routes);
        REQUIRE_FALSE(selected.empty());
        REQUIRE(LivingReturnPolicy::SamePosition(selected.front(),route.Points.front()));
        REQUIRE(LivingReturnPolicy::SamePosition(selected.back(),route.Points.back()));
        REQUIRE(ResidentRouteLength(selected) <= ResidentRouteLength(route.Points)+.02f);
        // A shorter composition is permitted only when its complete chains
        // are themselves in the reviewed catalog. No unproved splice or
        // shortcut may be introduced between arbitrary matching points.
        bool reviewed = ResidentRouteEquals(selected,route.Points);
        for (auto const& first : LivingResidentsV12::Routes)
            for (auto const& second : LivingResidentsV12::Routes)
                if (first.From == route.From && first.To == second.From && second.To == route.To)
                {
                    auto joined = first.Points;
                    joined.insert(joined.end(),second.Points.begin(),second.Points.end());
                    reviewed = reviewed || ResidentRouteEquals(selected,joined);
                }
        REQUIRE(reviewed);
    }
    REQUIRE(pairs.size() == 30);
    REQUIRE(TransitRoute(0,5,LivingResidentsV12::Routes).empty());
    REQUIRE(TransitRoute(4,4,LivingResidentsV12::Routes).empty());
}

TEST_CASE("The v12 southern carrier journey replaces the mandatory coastal detour without moving its depots", "[AIWorld][LivingResidentsV12][BridgeRoutes]")
{
    using namespace LivingResidentsV11;
    auto oldRoute = TransitRoute(4,5,Locations::Routes);
    auto bridgeRoute = TransitRoute(4,5,LivingResidentsV12::Routes);
    REQUIRE_FALSE(oldRoute.empty());
    REQUIRE_FALSE(bridgeRoute.empty());
    REQUIRE(ResidentRouteLength(oldRoute) > 1000);
    REQUIRE(ResidentRouteLength(bridgeRoute) < ResidentRouteLength(oldRoute));
    REQUIRE(std::any_of(oldRoute.begin(),oldRoute.end(),[](ActionPosition const& point) { return point.Y < 40; }));
    REQUIRE(std::none_of(bridgeRoute.begin(),bridgeRoute.end(),[](ActionPosition const& point) { return point.Y < 40; }));
    REQUIRE(LivingReturnPolicy::SamePosition(oldRoute.front(),bridgeRoute.front()));
    REQUIRE(LivingReturnPolicy::SamePosition(oldRoute.back(),bridgeRoute.back()));
    State journey;
    RetargetJourney(journey,Locations::Depots[4].Point,5,4,LivingResidentsV12::Routes);
    REQUIRE(journey.TravelDepot == 5);
    REQUIRE(journey.Waypoint == 0);
    REQUIRE(journey.Waypoints.size() == bridgeRoute.size()+1);
    for (std::size_t n = 0; n < bridgeRoute.size(); ++n)
        REQUIRE(LivingReturnPolicy::SamePosition(journey.Waypoints[n],bridgeRoute[n]));
    REQUIRE(LivingReturnPolicy::SamePosition(journey.Waypoints.back(),Locations::Depots[4].Point));
}

TEST_CASE("A v12 mid-bridge retarget retains its existing exit before joining the next direct depot journey", "[AIWorld][LivingResidentsV12][BridgeRoutes]")
{
    using namespace LivingResidentsV11;
    State journey;
    journey.TravelDepot = 5;
    journey.TravelGoal = Locations::Depots[4].Point;
    journey.Waypoints = TransitRoute(4,5,LivingResidentsV12::Routes);
    REQUIRE(journey.Waypoints.size() > 2);
    journey.Waypoint = journey.Waypoints.size()/2;
    auto remaining = std::vector<ActionPosition>(journey.Waypoints.begin()+journey.Waypoint,journey.Waypoints.end());
    auto onward = TransitRoute(5,6,LivingResidentsV12::Routes);
    // The caller may be Euclidean-nearest to depot1 while still physically
    // following another bridge exit. That proximity cannot splice its route.
    RetargetJourney(journey,Locations::Depots[5].Point,6,1,LivingResidentsV12::Routes);
    REQUIRE(journey.TravelDepot == 6);
    REQUIRE(journey.Waypoint == 0);
    REQUIRE(journey.Waypoints.size() == remaining.size()+onward.size()+1);
    for (std::size_t n = 0; n < remaining.size(); ++n)
        REQUIRE(LivingReturnPolicy::SamePosition(journey.Waypoints[n],remaining[n]));
    for (std::size_t n = 0; n < onward.size(); ++n)
        REQUIRE(LivingReturnPolicy::SamePosition(journey.Waypoints[remaining.size()+n],onward[n]));
    REQUIRE(LivingReturnPolicy::SamePosition(journey.Waypoints.back(),Locations::Depots[5].Point));
    for (std::size_t n = 1; n < journey.Waypoints.size(); ++n)
        REQUIRE(LivingReturnPolicy::Distance(journey.Waypoints[n-1],journey.Waypoints[n]) <= 160);
    State fresh;
    RetargetJourney(fresh,Locations::Depots[5].Point,6,5,LivingResidentsV12::Routes);
    std::vector<ActionPosition> freshRoute(fresh.Waypoints.begin(),fresh.Waypoints.end()-1);
    SameResidentRoute(freshRoute,onward);
}
