/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "ElwynnHuntPath.h"
#include "CompleteChasePath.h"
#include "GroundedPathSupport.h"
#include "Agent/LivingSurfaceCorridor.h"
#include "G3DPosition.hpp"
#include "Map.h"
#include "MoveSpline.h"
#include "PathGenerator.h"
#include "Unit.h"

bool Movement::PhysicalSplineSource(Unit const& owner, G3D::Vector3& source)
{
    bool const transport = owner.HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && !owner.GetTransGUID().IsEmpty();
    return SelectPhysicalSplineSource(PositionToVector3(&owner), transport,
        owner.movespline->Finalized(), owner.movespline->onTransport,
        [&]() -> G3D::Vector3 { return owner.movespline->ComputePosition(); }, source);
}

bool Movement::BuildElwynnHuntPath(Unit& owner, Unit& target, PathGenerator& path, PointsArray& points)
{
    points.clear();
    if (owner.GetMapId() != 0 || owner.GetMap() != target.GetMap()) return false;
    G3D::Vector3 origin;
    if (!PhysicalSplineSource(owner, origin)) return false;
    if (Creature* creature = owner.ToCreature(); creature && !target.isInAccessiblePlaceFor(creature)) return false;
    float const hitboxSum = owner.GetCombatReach() + target.GetCombatReach();
    bool const shorten = !owner.IsInDist(&target, owner.GetMeleeRange(&target));
    float x, y, z;
    if (shorten) target.GetPosition(x, y, z);
    else target.GetNearPoint(&owner, x, y, z, 0.0f, target.GetAbsoluteAngle(&owner));
    if (owner.IsHovering()) owner.UpdateAllowedPositionZ(x, y, z);
    PointsArray prepared;
    if (!PrepareCompleteChasePath(path, origin, G3D::Vector3(x, y, z),
        PositionToVector3(&target), CONTACT_DISTANCE + hitboxSum, shorten, points,
        [&](float px, float py, float pz)
        { return owner.GetMap()->GetZoneId(owner.GetPhaseMask(), px, py, pz) == 12; },
        [&](G3D::Vector3 const& a, G3D::Vector3 const& b)
        { return owner.GetMap()->isInLineOfSight(a.x, a.y, a.z + 0.5f, b.x, b.y, b.z + 0.5f,
            owner.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing); })) return false;
    if (owner.IsFlying()) return true;
    if (owner.CanEnterWater() && FullySwimmingPath(points,[&](G3D::Vector3 const& p)
        {
            auto liquid = owner.GetMap()->GetLiquidStatus(owner.GetPhaseMask(),p.x,p.y,p.z,
                {},nullptr,owner.GetCollisionHeight());
            return (liquid & (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK)) != 0;
        }))
        return true;
    // A mixed swimming/walking route must satisfy the ground proof too. An
    // unsupported transition is conservatively refused until it has its own
    // physical transition proof; a wet start cannot hide a dry floating end.
    prepared = std::move(points);
    float hover = owner.GetHoverOffset();
    return PrepareGroundedPath(prepared, origin, points,
        [&](G3D::Vector3 const& p) -> std::optional<float>
        {
            float lift = p.x == origin.x && p.y == origin.y && p.z == origin.z ? 0.3f : 0.8f;
            float height = owner.GetMap()->GetHeight(owner.GetPhaseMask(),p.x,p.y,p.z-hover+lift,true,3.0f);
            if (!std::isfinite(height) || height <= INVALID_HEIGHT) return std::nullopt;
            return height+hover;
        },
        [&](G3D::Vector3 const& a, G3D::Vector3 const& b)
        {
            return LivingSurfaceCorridor::BodyClear({0,a.x,a.y,a.z},{0,b.x,b.y,b.z},
                owner.GetBoundingRadius(),owner.GetCollisionHeight(),
                [&](ActionPosition const& from, ActionPosition const& to)
                { return owner.GetMap()->isInLineOfSight(from.X,from.Y,from.Z,to.X,to.Y,to.Z,
                    owner.GetPhaseMask(),LINEOFSIGHT_ALL_CHECKS,VMAP::ModelIgnoreFlags::Nothing); });
        },
        [&](G3D::Vector3 const& p)
        { return owner.GetMap()->GetZoneId(owner.GetPhaseMask(),p.x,p.y,p.z) == 12; },
        [&](G3D::Vector3 const& p) { return path.RecoveryTile(p); });
}
