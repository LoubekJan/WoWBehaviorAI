/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "ElwynnHuntPath.h"
#include "CompleteChasePath.h"
#include "G3DPosition.hpp"
#include "Map.h"
#include "PathGenerator.h"
#include "Unit.h"

bool Movement::BuildElwynnHuntPath(Unit& owner, Unit& target, PathGenerator& path, PointsArray& points)
{
    points.clear();
    if (owner.GetMapId() != 0 || owner.GetMap() != target.GetMap()) return false;
    if (Creature* creature = owner.ToCreature(); creature && !target.isInAccessiblePlaceFor(creature)) return false;
    float const hitboxSum = owner.GetCombatReach() + target.GetCombatReach();
    bool const shorten = !owner.IsInDist(&target, owner.GetMeleeRange(&target));
    float x, y, z;
    if (shorten) target.GetPosition(x, y, z);
    else target.GetNearPoint(&owner, x, y, z, 0.0f, target.GetAbsoluteAngle(&owner));
    if (owner.IsHovering()) owner.UpdateAllowedPositionZ(x, y, z);
    return PrepareCompleteChasePath(path, PositionToVector3(&owner), G3D::Vector3(x, y, z),
        PositionToVector3(&target), CONTACT_DISTANCE + hitboxSum, shorten, points,
        [&](float px, float py, float pz)
        { return owner.GetMap()->GetZoneId(owner.GetPhaseMask(), px, py, pz) == 12; },
        [&](G3D::Vector3 const& a, G3D::Vector3 const& b)
        { return owner.GetMap()->isInLineOfSight(a.x, a.y, a.z + 0.5f, b.x, b.y, b.z + 0.5f,
            owner.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing); });
}
