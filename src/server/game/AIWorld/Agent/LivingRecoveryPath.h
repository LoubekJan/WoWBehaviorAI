/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGRECOVERYPATH_H
#define AIWORLD_LIVINGRECOVERYPATH_H
#include "Action/RecoveryMovement.h"
#include "Agent/LivingReturnPolicy.h"
#include <G3D/Vector3.h>
#include "MoveSplineInitArgs.h"
class Creature;

namespace LivingRecoveryPath
{
    bool InSwimmableWater(Creature const& creature, ActionPosition const& point);
    bool Build(Creature& creature, RecoveryMovement const& request, Movement::PointsArray& points,
        LivingReturnPolicy::Diagnostics* diagnostics = nullptr);
    std::optional<ActionPosition> RejoinPosition(Creature& creature);
    std::vector<ActionPosition> RejoinPositions(Creature& creature);
}
#endif
