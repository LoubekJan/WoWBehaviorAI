/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGSURFACEDETOUR_H
#define AIWORLD_LIVINGSURFACEDETOUR_H

#include "Agent/LivingSurfaceCorridor.h"
#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace LivingSurfaceDetour
{
    using Status = LivingSurfaceCorridor::Status;
    constexpr float GridStep = 2.5f;
    constexpr float NarrowGridStep = 1.25f;
    constexpr float MaxRouteLength = 480.0f;
    constexpr unsigned MaxNodes = 512;
    constexpr unsigned MaxEdges = 2048;
    constexpr unsigned NoNode = std::numeric_limits<unsigned>::max();

    struct Node
    {
        ActionPosition Position;
        int X = 0, Y = 0;
        float Cost = 0, Remaining = 0;
        unsigned Parent = NoNode, HeapPosition = NoNode;
        bool Closed = false;
    };

    struct Search
    {
        Status State = Status::Pending;
        char const* Failure = "NOT_CHECKED";
        char const* LastEdgeFailure = "NOT_CHECKED";
        ActionPosition From, Home;
        float ArrivalRadius = 0, Limit = 0;
        float Spacing = GridStep;
        // These may only be lowered, before the first call, e.g. in a test.
        unsigned NodeLimit = MaxNodes, EdgeLimit = MaxEdges;
        unsigned StartedNodeLimit = 0, StartedEdgeLimit = 0;
        unsigned EdgeAttempts = 0, Expanded = 0;
        bool Started = false;
        std::vector<Node> Nodes;
        std::vector<unsigned> Open;
        std::unordered_map<std::uint64_t, unsigned> Index;
        // Only a complete home route is published here. Keep every checked
        // graph corner: grouping samples with Corridor::Legs would cut bends.
        std::vector<ActionPosition> Route;
        unsigned Current = NoNode, NextDirection = 0;
        unsigned EdgeFrom = NoNode, EdgeExisting = NoNode;
        int EdgeX = 0, EdgeY = 0;
        ActionPosition EdgeTarget;
        bool EdgeActive = false;
        LivingSurfaceCorridor::Search Edge;
    };

    inline bool Same(ActionPosition const& a, ActionPosition const& b)
    { return a.MapId == b.MapId && a.X == b.X && a.Y == b.Y && a.Z == b.Z; }

    inline float Remaining(ActionPosition const& point, ActionPosition const& home, float radius)
    { return std::max(0.0f, std::hypot(point.X-home.X, point.Y-home.Y)-radius); }

    inline std::uint64_t Key(int x, int y)
    { return (std::uint64_t(std::uint32_t(x)) << 32) | std::uint32_t(y); }

    inline bool Before(Search const& search, unsigned a, unsigned b)
    {
        auto const& left = search.Nodes[a]; auto const& right = search.Nodes[b];
        float leftTotal = left.Cost+left.Remaining, rightTotal = right.Cost+right.Remaining;
        if (leftTotal != rightTotal) return leftTotal < rightTotal;
        if (left.Remaining != right.Remaining) return left.Remaining < right.Remaining;
        return a < b;
    }

    inline void SwapOpen(Search& search, unsigned a, unsigned b)
    {
        std::swap(search.Open[a], search.Open[b]);
        search.Nodes[search.Open[a]].HeapPosition = a;
        search.Nodes[search.Open[b]].HeapPosition = b;
    }

    inline void Promote(Search& search, unsigned position)
    {
        while (position && Before(search, search.Open[position], search.Open[(position-1)/2]))
        { unsigned parent = (position-1)/2; SwapOpen(search, position, parent); position = parent; }
    }

    inline void Queue(Search& search, unsigned node)
    {
        auto& entry = search.Nodes[node];
        if (entry.HeapPosition == NoNode)
        { entry.HeapPosition = unsigned(search.Open.size()); search.Open.push_back(node); }
        Promote(search, entry.HeapPosition);
    }

    inline unsigned Pop(Search& search)
    {
        unsigned result = search.Open.front();
        search.Nodes[result].HeapPosition = NoNode;
        if (search.Open.size() == 1) { search.Open.pop_back(); return result; }
        search.Open.front() = search.Open.back(); search.Open.pop_back();
        search.Nodes[search.Open.front()].HeapPosition = 0;
        unsigned position = 0;
        while (position*2+1 < search.Open.size())
        {
            unsigned next = position*2+1;
            if (next+1 < search.Open.size() && Before(search, search.Open[next+1], search.Open[next])) ++next;
            if (!Before(search, search.Open[next], search.Open[position])) break;
            SwapOpen(search, position, next); position = next;
        }
        return result;
    }

    // Incremental A* over supported short edges, not a series of speculative
    // moves. It can temporarily head away from home but publishes nothing
    // until an entire route reaches the caller's physically supported goal.
    // Each call checks at most one edge and eight internal height samples,
    // or invokes one IsGoal callback. The caller must bound that callback's
    // own geometry work too. Every edge keeps a resumable Corridor cursor;
    // a zero budget invokes no callbacks.
    template<class HeightAt, class ClearSegment, class Contains, class IsGoal,
        class HasTile = LivingSurfaceCorridor::InstalledTiles>
    Status Advance(Search& search, ActionPosition const& from, ActionPosition const& home,
        float arrivalRadius, float limit, HeightAt&& heightAt, ClearSegment&& clearSegment,
        Contains&& contains, IsGoal&& isGoal, unsigned sampleBudget = 8, unsigned edgeBudget = 1,
        HasTile&& hasTile = {})
    {
        auto reject = [&](char const* reason)
        {
            search.Failure = reason; search.State = Status::Rejected; search.Route.clear();
            search.EdgeActive = false; return search.State;
        };
        if (search.Started && (!Same(search.From, from) || !Same(search.Home, home) ||
            search.ArrivalRadius != arrivalRadius || search.Limit != limit ||
            search.NodeLimit != search.StartedNodeLimit || search.EdgeLimit != search.StartedEdgeLimit))
            return reject("SURFACE_DETOUR_CONTEXT");
        if (search.State != Status::Pending || !sampleBudget || !edgeBudget) return search.State;
        sampleBudget = std::min(8u, sampleBudget);
        if (!search.Started)
        {
            if (!LivingSurfaceCorridor::Finite(from) || !LivingSurfaceCorridor::Finite(home) ||
                from.MapId != home.MapId || !std::isfinite(arrivalRadius) || arrivalRadius < 0 ||
                !std::isfinite(limit) || limit <= 0 || arrivalRadius > limit ||
                !search.NodeLimit || search.NodeLimit > MaxNodes || !search.EdgeLimit || search.EdgeLimit > MaxEdges ||
                std::hypot(from.X-home.X, from.Y-home.Y) > limit || !contains(from))
                return reject("SURFACE_DETOUR_BOUNDS");
            if (!hasTile(from)) return reject("SURFACE_MISSING_TILE");
            auto support = heightAt(from); --sampleBudget;
            if (!support || !std::isfinite(*support) || std::abs(*support-from.Z) > 1.0f)
                return reject("SURFACE_START_HEIGHT");
            if (Remaining(from, home, arrivalRadius) > MaxRouteLength)
                return reject("SURFACE_DETOUR_RANGE");
            search.Started = true; search.From = from; search.Home = home;
            search.ArrivalRadius = arrivalRadius; search.Limit = limit;
            search.StartedNodeLimit = search.NodeLimit; search.StartedEdgeLimit = search.EdgeLimit;
            search.Nodes.reserve(search.NodeLimit); search.Open.reserve(search.NodeLimit);
            search.Index.reserve(search.NodeLimit);
            search.Nodes.push_back({from, 0, 0, 0, Remaining(from, home, arrivalRadius)});
            search.Index.emplace(Key(0, 0), 0); Queue(search, 0); search.Failure = "NONE";
            if (!sampleBudget) return search.State;
        }

        if (!search.EdgeActive)
        {
            if (search.Current == NoNode || search.NextDirection == 8)
            {
                if (search.Open.empty())
                {
                    // A supported ledge may turn before the first coarse
                    // endpoint. Retry that isolated origin once at a shorter
                    // spacing; never skip collision/support checks or spend a
                    // fresh edge/node budget. Every resulting leg stays above
                    // the recovery executor's one-yard useful-step threshold.
                    if (search.Nodes.size() != 1 || search.Spacing != GridStep)
                        return reject("SURFACE_DETOUR_EXHAUSTED");
                    search.Spacing = NarrowGridStep;
                    search.Nodes.front().Closed = false;
                    Queue(search, 0);
                }
                search.Current = Pop(search); search.NextDirection = 0;
                auto& current = search.Nodes[search.Current]; current.Closed = true; ++search.Expanded;
                if (current.Remaining == 0 && isGoal(current.Position))
                {
                    for (unsigned node = search.Current; search.Nodes[node].Parent != NoNode; node = search.Nodes[node].Parent)
                        search.Route.push_back(search.Nodes[node].Position);
                    std::reverse(search.Route.begin(), search.Route.end());
                    search.State = Status::Complete; return search.State;
                }
                if (current.Remaining == 0) return search.State;
            }

            // Bookkeeping skips at most eight neighbours. Geometry work for
            // a new edge is always paid for by a subsequent bounded batch.
            constexpr std::array<std::array<int, 2>, 8> directions{{
                {{1,0}}, {{1,1}}, {{0,1}}, {{-1,1}}, {{-1,0}}, {{-1,-1}}, {{0,-1}}, {{1,-1}}}};
            while (search.NextDirection < directions.size())
            {
                auto direction = directions[search.NextDirection++];
                auto const& current = search.Nodes[search.Current];
                int x = current.X+direction[0], y = current.Y+direction[1];
                ActionPosition target{from.MapId, from.X+x*search.Spacing, from.Y+y*search.Spacing, current.Position.Z};
                if (std::hypot(target.X-from.X, target.Y-from.Y) > MaxRouteLength ||
                    std::hypot(target.X-home.X, target.Y-home.Y) > limit) continue;
                auto existing = search.Index.find(Key(x,y));
                unsigned index = existing == search.Index.end() ? NoNode : existing->second;
                float lowerCost = current.Cost+std::hypot(target.X-current.Position.X,target.Y-current.Position.Y);
                if (lowerCost > MaxRouteLength || (index != NoNode &&
                    (search.Nodes[index].Closed || lowerCost >= search.Nodes[index].Cost))) continue;
                if (search.EdgeAttempts == search.EdgeLimit) return reject("SURFACE_DETOUR_EDGE_LIMIT");
                ++search.EdgeAttempts;
                search.EdgeFrom = search.Current; search.EdgeExisting = index;
                search.EdgeX = x; search.EdgeY = y; search.EdgeTarget = target;
                search.Edge = {}; search.EdgeActive = true;
                break;
            }
            if (!search.EdgeActive) return search.State;
        }

        unsigned startSample = search.Edge.Started ? 0 : 1;
        auto state = LivingSurfaceCorridor::Advance(search.Edge, search.Nodes[search.EdgeFrom].Position,
            search.EdgeTarget, heightAt, clearSegment, contains, sampleBudget-startSample, hasTile, true);
        if (state == Status::Pending) return search.State;
        search.EdgeActive = false; search.LastEdgeFailure = search.Edge.Failure;
        if (state == Status::Rejected) return search.State;
        auto endpoint = search.Edge.Route.back();
        float cost = search.Nodes[search.EdgeFrom].Cost+search.Edge.Length;
        if (search.Edge.Length > 6.0f)
        { search.LastEdgeFailure = "SURFACE_DETOUR_EDGE_RANGE"; return search.State; }
        // Do not merge an edge into a previously discovered different floor.
        // Nodes already expanded are immutable, including their checked Z.
        if (cost > MaxRouteLength || (search.EdgeExisting != NoNode &&
            (cost >= search.Nodes[search.EdgeExisting].Cost ||
             std::abs(endpoint.Z-search.Nodes[search.EdgeExisting].Position.Z) > 1.0f))) return search.State;
        unsigned index = search.EdgeExisting;
        if (index == NoNode)
        {
            if (search.Nodes.size() == search.NodeLimit) return reject("SURFACE_DETOUR_NODE_LIMIT");
            index = unsigned(search.Nodes.size());
            search.Nodes.push_back({endpoint, search.EdgeX, search.EdgeY, cost, Remaining(endpoint,home,arrivalRadius), search.EdgeFrom});
            search.Index.emplace(Key(search.EdgeX,search.EdgeY), index);
        }
        else
        {
            auto& node = search.Nodes[index];
            node.Position = endpoint; node.Cost = cost; node.Parent = search.EdgeFrom;
        }
        Queue(search, index);
        return search.State;
    }
}
#endif
