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

#ifndef AIWORLD_SIMULATIONSCHEDULESTATE_H
#define AIWORLD_SIMULATIONSCHEDULESTATE_H

#include "Define.h"

// Milestone 2.10D/2.10D P2 fix: per-agent, world-thread-only bookkeeping
// for the coarse Background/Abstract simulation tick - deliberately not
// part of AgentRecord/AgentRegistry, the same reasoning as
// DecisionScheduleState.h and AIWorldMgr's own _agentSimulationTier: this
// is scheduling bookkeeping, not gameplay state. 2.10D's tick itself does
// nothing but log - no Needs/goal/memory change, so there is nothing here
// for a tier transition to disturb either way.
//
// One field, reused for both Background and Abstract - which interval
// (AIWorld.BackgroundSimulationIntervalMs/AbstractSimulationIntervalMs)
// applies is decided fresh each check from the agent's current
// SimulationTier, not stored here - the same "recompute from current
// classification, never lock in the interval that was true last time"
// principle DecisionScheduleState/DecisionScheduler already apply to
// Nearby/Active (see their own 2.10B P2 fix comments).
//
// LastTickAtMs == 0 means "never ticked" and is always immediately due -
// but AIWorldMgr::UpdateSimulationTier() also resets it to the current
// time (not 0) the moment an agent (re-)enters Background/Abstract, from
// any other tier or from having no tier recorded yet. Without that reset,
// an agent that spent time Materialized (Active/Nearby) in between two
// Background stretches would compute its next due time - and, once a
// future milestone attaches real background simulation here, whatever
// that simulation actually does - against a timestamp from before the
// materialized stretch even started, silently including time that was
// already being simulated for real by the live Materialized path. The
// reset makes the coarse-tick "epoch" start fresh every time this agent
// actually becomes Background/Abstract, so LastTickAtMs (and therefore any
// due-time or dt computed from it) never reaches back across a
// materialized stretch, or across the moment this agent was first ever
// observed.
struct SimulationScheduleState
{
    uint64 LastTickAtMs = 0;
};

#endif // AIWORLD_SIMULATIONSCHEDULESTATE_H
