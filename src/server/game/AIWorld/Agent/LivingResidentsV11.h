/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGRESIDENTSV11_H
#define AIWORLD_LIVINGRESIDENTSV11_H

#include "AgentType.h"
#include "Agent/LivingReturnPolicy.h"
#include "Simulation/SimulationScope.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// Explicit capabilities of the reviewed v11 population. None of the retired
// preview-v8 identities belongs to this catalog, and enabling this catalog
// never grants permissions to an arbitrary nearby creature.
namespace LivingResidentsV11
{
    enum class Job : uint8 { None, Farmer, Consumer, Carrier, Raider };
    enum class Activity : uint8 { None, Work, Deliver, Eat, Rest, Load, Steal };
    inline constexpr uint64 First = 902000, Last = 902044;
    inline Job Role(uint64 spawn)
    {
        if (spawn < First || spawn > Last) return Job::None;
        if (spawn <= 902011) return Job::Farmer;
        if (spawn == 902016 || spawn == 902017) return Job::Carrier;
        if (spawn >= 902035) return Job::Raider;
        return Job::Consumer;
    }
    inline uint32 Depot(uint64 spawn)
    {
        auto job = Role(spawn);
        return job == Job::Farmer ? uint32((spawn - First) / 4 + 1) :
            job == Job::Raider ? 6 : job == Job::None ? 0 : spawn <= 902015 ? 4 : 5;
    }
    inline bool Capability(uint64 spawn, uint32 map, AgentType type, AgentControlMode control, bool enabled)
    {
        auto job = Role(spawn);
        return enabled && job != Job::None && map == 725 && control == AgentControlMode::AIWorldControlled &&
            type == (job == Job::Raider ? AgentType::Combatant : AgentType::Civilian);
    }
    inline bool ScopeValid(SimulationScope const& scope)
    {
        if (!scope.Valid() || scope.MapId != 725 || !scope.Bounds ||
            scope.ZoneIds != std::vector<uint32>{4988} || scope.SpawnIds.size() != 45) return false;
        for (uint64 spawn = First; spawn <= Last; ++spawn)
            if (!scope.ContainsSpawn(spawn)) return false;
        return true;
    }
    inline uint32 WorkPoint(uint64 spawn, uint32 cycle)
    { return uint32((spawn - First) % 4) + 4 * (cycle % 3); }
    inline char const* RoleName(Job job)
    {
        switch (job)
        {
            case Job::Farmer: return "FARMER";
            case Job::Carrier: return "RESIDENT_CARRIER";
            case Job::Raider: return "RAIDER";
            case Job::Consumer: return "CONSUMER";
            default: return "NONE";
        }
    }
    inline uint32 Duration(Activity activity)
    { return activity == Activity::Work ? 15000 : activity == Activity::Rest ? 20000 : 5000; }
    struct DepotSpec { uint32 Id; char const* Name; ActionPosition Point; };
    struct FieldSpec { uint32 Id; std::array<ActionPosition, 12> WorkPoints; };
    struct RouteSpec { uint32 From, To; std::vector<ActionPosition> Points; };
    inline std::vector<ActionPosition> TransitRoute(uint32 from, uint32 to, std::vector<RouteSpec> const& routes)
    {
        if (!from || !to || from == to) return {};
        auto length = [](std::vector<ActionPosition> const& points)
        {
            float value = 0;
            for (std::size_t i = 1; i < points.size(); ++i)
                value += LivingReturnPolicy::Distance(points[i-1], points[i]);
            return value;
        };
        std::vector<ActionPosition> best;
        float distance = std::numeric_limits<float>::max();
        for (auto const& first : routes)
        {
            if (first.From != from) continue;
            if (first.To == to && length(first.Points) < distance)
            { best = first.Points; distance = length(first.Points); }
            for (auto const& second : routes)
            {
                if (second.From != first.To || second.To != to) continue;
                float candidate = length(first.Points) + length(second.Points);
                if (candidate >= distance) continue;
                best = first.Points;
                best.insert(best.end(), second.Points.begin(), second.Points.end());
                distance = candidate;
            }
        }
        return best;
    }
    // Two independently owned haulers supply the two settlements without a
    // field. A missing stock source is a wait, never permission to mint food.
    inline uint32 DeliveryDepot(uint32 south, uint32 city)
    {
        if (south < 4 && south <= city) return 4;
        if (city < 12) return 5;
        if (south < 12) return 4;
        return city < 48 ? 5 : 0;
    }
    inline std::optional<uint32> Source(std::array<uint32, 4> const& stocks, uint32 previous,
        bool raider)
    {
        uint32 count = raider ? 4 : 3;
        for (uint32 offset = 0; offset < count; ++offset)
        {
            uint32 id = (previous + offset) % count + 1;
            if (stocks[id - 1] >= (raider ? 6u : 10u)) return id;
        }
        return std::nullopt;
    }
    struct State
    {
        Activity Pending = Activity::None;
        ActionPosition ActionPoint, TravelGoal;
        uint64 ActionStartedAtMs = 0, NextWorkEmoteAtMs = 0, NextTripAtMs = 0;
        uint64 Attempts = 0, Thefts = 0, CompletedTrips = 0, Loaded = 0;
        uint32 SourceDepot = 0, PreviousSource = 0, DestinationDepot = 0, FieldCycle = 0, TravelDepot = 0;
        bool ReturningHome = false, RestAtHome = false, HomeViaCity = false;
        std::vector<ActionPosition> Waypoints;
        std::size_t Waypoint = 0;
    };
    inline void RetargetJourney(State& state, ActionPosition const& goal, uint32 depot,
        uint32 nearbyDepot, std::vector<RouteSpec> const& routes)
    {
        std::vector<ActionPosition> remaining;
        // Keep the already selected coast/bridge exit. Euclidean proximity
        // cannot attach a displaced mid-route traveller to another village.
        if (state.TravelDepot && state.Waypoint < state.Waypoints.size())
        {
            nearbyDepot = state.TravelDepot;
            remaining.assign(state.Waypoints.begin() + state.Waypoint, state.Waypoints.end());
        }
        state.TravelGoal = goal;
        state.TravelDepot = depot;
        state.Waypoint = 0;
        state.Waypoints = std::move(remaining);
        if (depot && nearbyDepot != depot)
        {
            auto continuation = TransitRoute(nearbyDepot, depot, routes);
            state.Waypoints.insert(state.Waypoints.end(), continuation.begin(), continuation.end());
        }
        state.Waypoints.push_back(goal);
    }
}
#endif
