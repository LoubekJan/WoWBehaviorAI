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
            State state{nowMs, nowMs + 1 + StableAgentHash(id.Value ^ salt) % intervalMs};
            if (old != _states.end() && nowMs >= old->second.LastMs)
            {
                state.LastMs = old->second.LastMs;
                if (intervalMs == _intervalMs) state.NextMs = old->second.NextMs;
            }
            states.emplace(id.Value, state);
            entries.push_back({state.NextMs, id.Value});
        }
        _states = std::move(states);
        _due = Queue(Later{}, std::move(entries));
        _intervalMs = intervalMs;
    }

    std::optional<Update> PopDue(uint64 nowMs)
    {
        if (_due.empty() || _due.top().AtMs > nowMs) return std::nullopt;
        Entry entry = _due.top();
        _due.pop();
        State& state = _states.at(entry.Id);
        Update update{AgentId{entry.Id}, uint32(std::min<uint64>(nowMs - state.LastMs,
            std::numeric_limits<uint32>::max())), nowMs - entry.AtMs};
        state.LastMs = nowMs;
        state.NextMs = nowMs + _intervalMs - (nowMs - entry.AtMs) % _intervalMs;
        _due.push({state.NextMs, entry.Id});
        return update;
    }

    uint64 OldestLateMs(uint64 nowMs) const
    { return !_due.empty() && nowMs > _due.top().AtMs ? nowMs - _due.top().AtMs : 0; }

private:
    struct State { uint64 LastMs; uint64 NextMs; };
    struct Entry { uint64 AtMs; uint64 Id; };
    struct Later
    {
        bool operator()(Entry const& a, Entry const& b) const
        { return a.AtMs != b.AtMs ? a.AtMs > b.AtMs : a.Id > b.Id; }
    };
    using Queue = std::priority_queue<Entry, std::vector<Entry>, Later>;
    std::unordered_map<uint64, State> _states;
    Queue _due;
    uint32 _intervalMs = 1;
};

#endif
