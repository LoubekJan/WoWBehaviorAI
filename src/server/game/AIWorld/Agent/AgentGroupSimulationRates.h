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

#ifndef AIWORLD_AGENTGROUPSIMULATIONRATES_H
#define AIWORLD_AGENTGROUPSIMULATIONRATES_H

#include "Define.h"

// Milestone 2.12B/2.12D: per-second drift rate for AgentGroupSimulationSystem::
// Update() - shared configuration, not per-group state, the same
// config-struct-passed-per-call pattern NeedsUpdateRates already uses for
// NeedsSystem::Update(). Not a tuned gameplay value - chosen only to be
// deterministically observable over the group coarse-tick interval
// (AIWorld.GroupSimulationIntervalMs), the same "first experiment" scope
// NeedsSystem's own defaults started from.
//
// Milestone 2.12D P2 fix (STATIC review): HungerPerSecond is gone along
// with AgentGroupRecord's own Hunger field - see its comment for why a
// group-level Hunger modeled the exact aggregate-replaces-members shape
// this rename was meant to remove.
struct AgentGroupSimulationRates
{
    float ResourcesPerSecond = 0.0005f;
};

#endif // AIWORLD_AGENTGROUPSIMULATIONRATES_H
