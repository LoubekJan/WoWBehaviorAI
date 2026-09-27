/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGADVICEBUDGET_H
#define AIWORLD_LIVINGADVICEBUDGET_H
#include "Define.h"
#include <algorithm>
#include <deque>

// World-thread admission, before path queries as well as HTTP. A caller must
// still be eligible when its turn arrives; absent/combat/dead callers expire.
class LivingAdviceBudget
{
    struct Waiting { uint64 Id, SeenAt; };
    std::deque<Waiting> _waiting;
    uint64 _nextAt = 0;
public:
    bool Acquire(uint64 id, uint64 now)
    {
        std::erase_if(_waiting, [now](auto const& w) { return now < w.SeenAt || now - w.SeenAt > 30000; });
        auto found = std::find_if(_waiting.begin(), _waiting.end(), [id](auto const& w) { return w.Id == id; });
        if (found != _waiting.end()) found->SeenAt = now;
        else if (_waiting.size() < 2048) _waiting.push_back({id, now});
        if (_waiting.empty() || _waiting.front().Id != id || now < _nextAt) return false;
        _waiting.pop_front();
        _nextAt = now + 2000;
        return true;
    }
};
#endif
