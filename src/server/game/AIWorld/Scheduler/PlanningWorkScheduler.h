/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_PLANNINGWORKSCHEDULER_H
#define AIWORLD_PLANNINGWORKSCHEDULER_H

#include "Agent/AgentId.h"
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Persistent world-thread planning admission. Only the current due cohort can
// block admission. An older waiter which is not due never blocks ready work.
// After real work a continuing request returns to the tail, so repeated demand
// from the same early actor cannot monopolize the next frame's budget.
class PlanningWorkScheduler
{
    struct Waiting { uint64 Since, SeenAt, Sequence; };
public:
    void BeginFrame(uint64 nowMs, std::vector<AgentId> const& ready)
    {
        _nowMs = nowMs; _active = {};
        _ready.clear(); _ready.reserve(ready.size());
        for (AgentId id : ready) if (id) _ready.insert(id.Value);
    }
    void Request(AgentId id, uint64 nowMs)
    {
        if (!id) return;
        auto entry = _waiting.find(id.Value);
        if (entry == _waiting.end())
            _waiting.emplace(id.Value, Waiting{nowMs, nowMs, ++_sequence});
        else
        {
            if (nowMs < entry->second.SeenAt)
                entry->second = {nowMs, nowMs, ++_sequence};
            else entry->second.SeenAt = nowMs;
        }
    }
    void Cancel(AgentId id)
    {
        _waiting.erase(id.Value); _ready.erase(id.Value);
        if (_active == id) _active = {};
    }
    void SyncMembership(std::vector<AgentId> const& members)
    {
        std::unordered_set<uint64> live;
        live.reserve(members.size());
        for (AgentId id : members) if (id) live.insert(id.Value);
        std::erase_if(_waiting, [&](auto const& entry) { return !live.count(entry.first); });
        std::erase_if(_ready, [&](uint64 id) { return !live.count(id); });
        if (_active && !live.count(_active.Value)) _active = {};
    }
    void ExpireUnseen(uint64 nowMs, uint64 ttlMs = 30000)
    {
        std::erase_if(_waiting, [&](auto const& entry)
            { return nowMs < entry.second.SeenAt || nowMs - entry.second.SeenAt >= ttlMs; });
        if (_active && !_waiting.count(_active.Value)) _active = {};
    }
    uint64 Priority(AgentId id) const
    {
        auto entry = _waiting.find(id.Value);
        return entry != _waiting.end() ? entry->second.Sequence : std::numeric_limits<uint64>::max();
    }
    bool CanAdmit(AgentId id) const
    {
        if (_active) return _active == id;
        auto entry = _waiting.find(id.Value);
        if (!id || entry == _waiting.end() || !_ready.count(id.Value)) return false;
        for (uint64 other : _ready)
        {
            auto waiting = _waiting.find(other);
            if (waiting != _waiting.end() && waiting->second.Sequence < entry->second.Sequence) return false;
        }
        return true;
    }
    bool TryAdmit(AgentId id)
    {
        if (!CanAdmit(id)) return false;
        _active = id;
        return true;
    }
    // Every actually processed actor is unavailable for the rest of this due
    // cohort. A refused query preserves age; actual work starts a new wait.
    void EndActor(AgentId id, bool didWork)
    {
        _ready.erase(id.Value);
        if (_active == id) _active = {};
        auto entry = _waiting.find(id.Value);
        if (didWork && entry != _waiting.end())
            entry->second = {_nowMs, _nowMs, ++_sequence};
    }
    std::optional<uint64> WaitMs(AgentId id, uint64 nowMs) const
    {
        auto entry = _waiting.find(id.Value);
        if (entry == _waiting.end()) return std::nullopt;
        return nowMs >= entry->second.Since ? nowMs-entry->second.Since : 0;
    }
    bool HasRequest(AgentId id) const { return _waiting.count(id.Value) != 0; }
    std::size_t PendingCount() const { return _waiting.size(); }

private:
    std::unordered_map<uint64, Waiting> _waiting;
    std::unordered_set<uint64> _ready;
    AgentId _active;
    uint64 _sequence = 0, _nowMs = 0;
};

#endif
