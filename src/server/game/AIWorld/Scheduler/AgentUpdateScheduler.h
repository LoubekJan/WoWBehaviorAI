/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef AIWORLD_AGENTUPDATESCHEDULER_H
#define AIWORLD_AGENTUPDATESCHEDULER_H

#include "Agent/AgentId.h"
#include "StableAgentHash.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

// World-thread schedule for periodic per-agent work. No engine pointers.
// The caller stops popping when its time/count budget is exhausted. Oldest
// deadlines stay first; processing a delayed tick never replays missed ticks
// or collapses all agents onto the same new deadline.
class AgentUpdateScheduler
{
public:
    struct Update
    {
        AgentId Agent;
        uint32 ElapsedMs;
        uint64 LateMs;
    };

    void Sync(std::vector<AgentId> const& agents, uint64 nowMs, uint32 intervalMs, uint64 salt)
    {
        intervalMs = std::max(uint32(1), intervalMs);
        std::unordered_map<uint64, State> states;
        states.reserve(agents.size());
        std::vector<Entry> entries;
        entries.reserve(agents.size());
        for (AgentId id : agents)
        {
            if (!id || states.count(id.Value)) continue;
            auto old = _states.find(id.Value);
            uint64 next = AddTime(nowMs, 1 + StableAgentHash(id.Value ^ salt) % intervalMs);
            State state{nowMs, next, next, 1};
            if (old != _states.end() && nowMs >= old->second.LastMs)
            {
                state.LastMs = old->second.LastMs;
                if (intervalMs == _intervalMs) state = old->second;
                else if (old->second.NextMs < old->second.PeriodicNextMs)
                    state.NextMs = std::min(state.NextMs, old->second.NextMs);
            }
            states.emplace(id.Value, state);
            entries.push_back({state.NextMs, id.Value, state.Generation});
        }
        _states = std::move(states);
        _due = Queue(Later{}, std::move(entries));
        _intervalMs = intervalMs;
    }

    // A validated engine event can request one early pass without moving the
    // periodic deadline or charging the same needs time twice. The caller
    // still drains work under its ordinary frame/count budgets.
    bool Wake(AgentId id, uint64 nowMs)
    {
        auto state = _states.find(id.Value);
        if (!id || state == _states.end() || nowMs <= state->second.LastMs || nowMs >= state->second.NextMs)
            return false;
        state->second.NextMs = nowMs;
        ++state->second.Generation;
        _due.push({nowMs, id.Value, state->second.Generation});
        return true;
    }

    std::optional<Update> PopDue(uint64 nowMs)
    {
        PruneStale();
        if (_due.empty() || _due.top().AtMs > nowMs) return std::nullopt;
        Entry entry = _due.top();
        _due.pop();
        return Advance(entry.Id, nowMs);
    }

    // Inspect a bounded cohort without advancing LastMs or its deadline.
    // Callers may reorder this cohort, then pop only work they actually run.
    std::vector<AgentId> DueAgents(uint64 nowMs, std::size_t limit = 128) const
    {
        PruneStale();
        Queue due = _due;
        std::vector<AgentId> result;
        result.reserve(std::min(limit, _states.size()));
        while (!due.empty() && due.top().AtMs <= nowMs && result.size() < limit)
        {
            Entry entry = due.top(); due.pop();
            auto state = _states.find(entry.Id);
            if (state != _states.end() && Matches(state->second, entry))
                result.push_back(AgentId{entry.Id});
        }
        return result;
    }

    std::optional<Update> PopDueAgent(AgentId id, uint64 nowMs)
    {
        auto state = _states.find(id.Value);
        if (!id || state == _states.end() || state->second.NextMs > nowMs || nowMs <= state->second.LastMs)
            return std::nullopt;
        // The old heap entry is left stale. Ordinary pops/peeks discard it;
        // Sync rebuilds the queue and therefore removes every stale entry.
        return Advance(id.Value, nowMs);
    }

    uint64 OldestLateMs(uint64 nowMs) const
    {
        PruneStale();
        return !_due.empty() && nowMs > _due.top().AtMs ? nowMs - _due.top().AtMs : 0;
    }

private:
    struct State { uint64 LastMs; uint64 NextMs; uint64 PeriodicNextMs; uint64 Generation; };
    struct Entry { uint64 AtMs; uint64 Id; uint64 Generation; };
    struct Later
    {
        bool operator()(Entry const& a, Entry const& b) const
        { return a.AtMs != b.AtMs ? a.AtMs > b.AtMs : a.Id > b.Id; }
    };
    using Queue = std::priority_queue<Entry, std::vector<Entry>, Later>;

    static uint64 AddTime(uint64 nowMs, uint64 delay)
    { return nowMs + std::min(delay, std::numeric_limits<uint64>::max() - nowMs); }
    static bool Matches(State const& state, Entry const& entry)
    {
        return state.NextMs == entry.AtMs && state.Generation == entry.Generation && state.NextMs > state.LastMs;
    }

    std::optional<Update> Advance(uint64 id, uint64 nowMs)
    {
        State& state = _states.at(id);
        Update update{AgentId{id}, uint32(std::min<uint64>(nowMs - state.LastMs,
            std::numeric_limits<uint32>::max())), nowMs - state.NextMs};
        state.LastMs = nowMs;
        if (nowMs >= state.PeriodicNextMs)
            state.PeriodicNextMs = AddTime(nowMs, _intervalMs - (nowMs - state.PeriodicNextMs) % _intervalMs);
        state.NextMs = state.PeriodicNextMs;
        ++state.Generation;
        _due.push({state.NextMs, id, state.Generation});
        return update;
    }
    void PruneStale() const
    {
        while (!_due.empty())
        {
            auto state = _states.find(_due.top().Id);
            if (state != _states.end() && Matches(state->second, _due.top())) break;
            _due.pop();
        }
    }
    std::unordered_map<uint64, State> _states;
    mutable Queue _due;
    uint32 _intervalMs = 1;
};

#endif
