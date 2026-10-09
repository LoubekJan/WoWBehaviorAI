/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef TRINITY_ELWYNNHUNTPATH_H
#define TRINITY_ELWYNNHUNTPATH_H

#include "MoveSplineInitArgs.h"
#include "Simulation/SimulationScope.h"

class Unit;
class PathGenerator;
namespace Movement
{
    // The world-space origin Launch will actually use. Refuses active
    // transports and does not change the actor, map or running spline.
    bool PhysicalSplineSource(Unit const& owner, G3D::Vector3& source);

    // Default melee chase, without a formation angle or custom chase range.
    // Planning has no combat/movement side effects; chase execution uses this
    // too. Dry walking routes are grounded after shortening, including every
    // executed half-yard control, so an interrupted hunt retains support.
    bool BuildElwynnHuntPath(Unit& owner, Unit& target, PathGenerator& path, PointsArray& points);
    bool BuildSimulationHuntPath(Unit& owner, Unit& target, PathGenerator& path, PointsArray& points,
        SimulationScope const& scope);
}
#endif
