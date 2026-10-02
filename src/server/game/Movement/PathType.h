/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef TRINITY_PATHTYPE_H
#define TRINITY_PATHTYPE_H

enum PathType
{
    PATHFIND_BLANK             = 0x00, // not built yet
    PATHFIND_NORMAL            = 0x01, // complete navigation path
    PATHFIND_SHORTCUT          = 0x02, // direct path, possibly through geometry
    PATHFIND_INCOMPLETE        = 0x04, // partial route toward the target
    PATHFIND_NOPATH            = 0x08, // no valid path or a query error
    PATHFIND_NOT_USING_PATH    = 0x10, // flying, swimming or missing mmaps
    PATHFIND_SHORT             = 0x20, // truncated at the path length limit
    PATHFIND_FARFROMPOLY_START  = 0x40, // start too far from a polygon
    PATHFIND_FARFROMPOLY_END    = 0x80, // destination too far from a polygon
    PATHFIND_FARFROMPOLY        = PATHFIND_FARFROMPOLY_START | PATHFIND_FARFROMPOLY_END
};

namespace Movement
{
    inline bool CompleteNavmeshPath(PathType type)
    {
        return (type & PATHFIND_NORMAL) && !(type & (PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE |
            PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORT | PATHFIND_FARFROMPOLY));
    }
}
#endif
