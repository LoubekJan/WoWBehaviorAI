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

#ifndef AIWORLD_ARRIVALTOLERANCE_H
#define AIWORLD_ARRIVALTOLERANCE_H

#include "ActionPosition.h"
#include "Define.h"

// Milestone 2.8F/2.8G: the single shared "close enough" tolerance for two
// otherwise-unrelated checks that both mean the same thing - is the actor
// actually standing at a given point - AIWorldMgr::ProcessActionEngineEvent()/
// its reconciliation check (was the MOVE_TO's actual arrival close enough
// to Destination to count as ARRIVED, given PathGenerator can return an
// incomplete path) and ActionSystem::ValidateEat() (is the actor close
// enough to the food target to eat from it). Not a tuned gameplay value -
// wide enough to absorb minor pathfinding/positional slop, tight enough to
// still catch a PATHFIND_INCOMPLETE result that stopped meaningfully short
// of the actual destination.
constexpr float ArrivalToleranceYards = 3.0f;

TC_GAME_API bool IsWithinArrivalTolerance(ActionPosition const& destination, float x, float y, float z);

#endif // AIWORLD_ARRIVALTOLERANCE_H
