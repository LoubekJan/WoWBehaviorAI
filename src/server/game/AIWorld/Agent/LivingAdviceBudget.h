/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGADVICEBUDGET_H
#define AIWORLD_LIVINGADVICEBUDGET_H
#include "Define.h"
#include <algorithm>
#include <deque>
#include <optional>

// Refreshed at the world update cadence, independently of animation/decision
// delays. Admission still precedes both path queries and HTTP.
class LivingAdviceBudget
{
    struct Waiting { uint64 Id, Since, SeenAt; bool Returning, Dispatchable; };
    std::deque<Waiting> _waiting;
    uint64 _nextAt = 0, _sweptAt = 0;
    uint32 _urgentRun = 0;
    void Expire(uint64 now)
    {
        if (now >= _sweptAt && now - _sweptAt < 1000) return;
        _sweptAt = now;
        std::erase_if(_waiting, [now](auto const& w) { return now < w.SeenAt || now - w.SeenAt > 30000; });
    }
    auto Next() const
    {
        auto urgent = std::find_if(_waiting.begin(), _waiting.end(), [](auto const& w) { return w.Returning && w.Dispatchable; });
        auto normal = std::find_if(_waiting.begin(), _waiting.end(), [](auto const& w) { return !w.Returning && w.Dispatchable; });
        return urgent != _waiting.end() && (_urgentRun < 3 || normal == _waiting.end()) ? urgent : normal;
    }
public:
    void Cancel(uint64 id) { std::erase_if(_waiting, [id](auto const& w) { return w.Id == id; }); }
    void Refresh(uint64 id, uint64 now, bool eligible, bool returning, bool dispatchable = true)
    {
        Expire(now);
        if (!eligible) { Cancel(id); return; }
        auto found = std::find_if(_waiting.begin(), _waiting.end(), [id](auto const& w) { return w.Id == id; });
        if (found != _waiting.end()) { found->SeenAt = now; found->Returning = returning; found->Dispatchable = dispatchable; }
    }
    bool Ready(uint64 id, uint64 now) const
    {
        auto next = Next();
        return next != _waiting.end() && next->Id == id && now >= _nextAt;
    }
    bool Acquire(uint64 id, uint64 now, bool returning = false)
    {
        Refresh(id, now, true, returning);
        if (std::none_of(_waiting.begin(), _waiting.end(), [id](auto const& w) { return w.Id == id; }) && _waiting.size() < 2048)
            _waiting.push_back({id, now, now, returning, true});
        if (!Ready(id, now)) return false;
        _urgentRun = returning ? std::min(3u, _urgentRun + 1) : 0;
        Cancel(id);
        _nextAt = now + 2000;
        return true;
    }
    std::optional<uint64> WaitMs(uint64 id, uint64 now) const
    {
        auto found = std::find_if(_waiting.begin(), _waiting.end(), [id](auto const& w) { return w.Id == id; });
        if (found == _waiting.end()) return std::nullopt;
        return now >= found->Since ? now - found->Since : 0;
    }
    uint32 Size() const { return uint32(_waiting.size()); }
    bool Dispatchable(uint64 id) const
    { return std::any_of(_waiting.begin(), _waiting.end(), [id](auto const& w) { return w.Id == id && w.Dispatchable; }); }
};
#endif
