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

#ifndef AIWORLD_FOODTARGETRESOLVER_H
#define AIWORLD_FOODTARGETRESOLVER_H

#include "Define.h"
#include "GoalTarget.h"
#include <optional>

// Milestone 2.8E: shared configuration, not per-agent state - the same
// config-struct-passed-per-call pattern NeedsUpdateRates already uses for
// NeedsSystem::Update(). AIWorldMgr owns one instance, loaded once at
// Initialize() from AIWorld.TestFoodTarget* - no config knob is read
// anywhere else. Enabled is a separate flag rather than treating some
// MapId/X/Y/Z combination as a "not configured" sentinel: MapId 0 is a
// real map, so there is no coordinate tuple that unambiguously means
// "nothing set".
struct FoodTargetConfig
{
    bool Enabled = false;
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

// Milestone 2.8E: answers only "where should a hungry agent go", nothing
// else - no inventory, no vendor/ownership, no EAT, no DB query, no
// pathfinding (that's TrinityCore's, once ActionExecutor::ExecuteMoveTo()
// runs). The only target source is a single fixed, config-driven world
// point - deliberately not per-agent or nearest-anything yet, so this
// milestone proves the Goal -> target -> Action boundary without also
// having to solve food economy. Pure value transform: no AgentId,
// Creature*, Map*, or DB.
class TC_GAME_API FoodTargetResolver
{
    public:
        std::optional<GoalTarget> Resolve(FoodTargetConfig const& config) const;
};

#endif // AIWORLD_FOODTARGETRESOLVER_H
