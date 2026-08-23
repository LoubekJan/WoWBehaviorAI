/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AIWORLD_GOALTARGET_H
#define AIWORLD_GOALTARGET_H

#include "Define.h"
#include "ObjectGuid.h"

// Milestone 2.8E: a resolved target for an ActiveGoal - "where should the
// agent go", not itself an ActionRequest. Kept separate from
// ActionPosition/ActionRequest.Destination on purpose: this is the
// output of goal-target planning (FoodTargetResolver), a Goal-layer
// concern, not an Action-layer one - AIWorldMgr is what turns a
// GoalTarget into a MOVE_TO ActionRequest, not GoalSystem or
// ActionSystem themselves. Pure value: no WorldObject*/Creature*/
// GameObject*. Guid is empty for a fixed world point (2.8E's only target
// source); a resolver that targets a specific live object later would
// set it.
struct GoalTarget
{
    ObjectGuid Guid;
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

#endif // AIWORLD_GOALTARGET_H
