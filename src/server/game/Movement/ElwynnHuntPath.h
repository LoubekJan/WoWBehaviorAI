/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef TRINITY_ELWYNNHUNTPATH_H
#define TRINITY_ELWYNNHUNTPATH_H

#include "MoveSplineInitArgs.h"

class Unit;
class PathGenerator;
namespace Movement
{
    // Default melee chase, without a formation angle or custom chase range.
    // Planning has no combat/movement side effects; chase execution uses this too.
    bool BuildElwynnHuntPath(Unit& owner, Unit& target, PathGenerator& path, PointsArray& points);
}
#endif
