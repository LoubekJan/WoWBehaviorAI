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

#ifndef AIWORLD_AGENTGROUPSIMULATIONSYSTEM_H
#define AIWORLD_AGENTGROUPSIMULATIONSYSTEM_H

#include "AgentGroupRecord.h"
#include "AgentGroupSimulationRates.h"
#include "Define.h"

// Milestone 2.12B/2.12D: the group coarse-tick simulation - deliberately
// the same shape as NeedsSystem::Update(): a pure value transform advancing
// AgentGroupRecord by an elapsed duration and a shared rates struct. No
// AgentId, Creature*, Map*, registry, or DB - the caller
// (AIWorldMgr::RunDecisionScheduler(), in its own group coarse-tick loop -
// see GroupId.h for why this no longer shares AgentRecord's
// SimulationTier/AgentType machinery) resolves dtMs itself and persists
// the result afterward via AgentGroupPersistence::SaveGroupState(). Never
// materializes anything, never touches ActionSystem/ActionExecutor.
//
// Milestone 2.12D P2 fix (STATIC review): runs unconditionally every due
// group coarse tick now, regardless of whether any member happens to be
// materialized right now - see AgentGroupRecord.h for why Resources (a
// shared/environmental territory fact) pausing for member presence was
// itself a symptom of the aggregate-replaces-members model this rename
// was meant to remove. Only Resources moves - Territory stays exactly
// what was loaded. Widening what a coarse tick simulates (cohesion,
// territory pressure, shared group intent, ...) is later roadmap work.
class TC_GAME_API AgentGroupSimulationSystem
{
    public:
        void Update(AgentGroupRecord& record, uint64 dtMs, AgentGroupSimulationRates const& rates) const;
};

#endif // AIWORLD_AGENTGROUPSIMULATIONSYSTEM_H
