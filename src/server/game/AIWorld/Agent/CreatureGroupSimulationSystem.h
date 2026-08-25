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

#ifndef AIWORLD_CREATUREGROUPSIMULATIONSYSTEM_H
#define AIWORLD_CREATUREGROUPSIMULATIONSYSTEM_H

#include "CreatureGroupSimulationRates.h"
#include "CreatureGroupState.h"
#include "Define.h"

// Milestone 2.12B: the first real ABSTRACT background simulation -
// deliberately the same shape as NeedsSystem::Update(): a pure value
// transform advancing CreatureGroupState by an elapsed duration and a
// shared rates struct. No AgentId, Creature*, Map*, registry, or DB - the
// caller (AIWorldMgr::RunDecisionScheduler(), in its existing coarse-tick
// loop) resolves dtMs from SimulationScheduleState itself and calls this
// only for SimulationTier::Abstract + AgentType::CreatureGroup, never
// materializes anything, never touches ActionSystem/ActionExecutor, and
// persists the result itself afterward via AgentPersistence::
// SaveCreatureGroupState().
//
// Only Hunger/Resources move - Population/Territory stay exactly what
// 2.12A loaded them as. Widening what a coarse tick simulates (fear,
// territory drift, population change from starvation, ...) is later
// roadmap work, not this milestone's.
class TC_GAME_API CreatureGroupSimulationSystem
{
    public:
        void Update(CreatureGroupState& state, uint64 dtMs, CreatureGroupSimulationRates const& rates) const;
};

#endif // AIWORLD_CREATUREGROUPSIMULATIONSYSTEM_H
