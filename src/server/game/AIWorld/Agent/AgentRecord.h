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

#ifndef AIWORLD_AGENTRECORD_H
#define AIWORLD_AGENTRECORD_H

#include "Action/ActiveAction.h"
#include "Action/PendingEatContinuation.h"
#include "AgentId.h"
#include "AgentLocation.h"
#include "AgentType.h"
#include "Define.h"
#include "Goal/ActiveGoal.h"
#include "Goal/RoutineGoal.h"
#include "Needs/NeedsState.h"
#include "Needs/NeedsThresholdState.h"
#include "ObjectGuid.h"
#include <optional>

// One persistent agent's registry-owned state. SpawnId/MapId is the stable
// binding to a TrinityCore creature spawn; RuntimeGuid is only the current
// Creature this agent happens to be bound to right now (meaningful only
// while WorldState == Materialized) - never the agent's identity, and never
// a substitute for AgentId. Deliberately holds no Creature*/Map*.
struct AgentRecord
{
    AgentId Id;
    AgentType Type = AgentType::Civilian;

    uint32 MapId = 0;
    uint64 SpawnId = 0;

    // Milestone 2.11A: persistent, optional - most agents (e.g. GUARD) have
    // neither. Behavior/capability layer (profession, daily cycle, ...) is
    // deliberately not modeled as its own AgentType; a CIVILIAN with both
    // set is how "this agent has a home and a workplace" is expressed for
    // now. Not yet read by any goal/action/scheduler - 2.11A only persists
    // and loads these, nothing acts on them.
    std::optional<AgentLocation> HomeLocation;
    std::optional<AgentLocation> WorkLocation;

    ObjectGuid RuntimeGuid;
    AgentWorldState WorldState = AgentWorldState::Abstract;

    uint64 SnapshotSequence = 0;

    // Milestone 2.6A: owned here, not by NeedsSystem - AgentRecord is
    // already the registry-owned home for an agent's runtime state, and
    // Needs must survive Creature unload/reload the same way everything
    // else here does. Frozen (not updated) while WorldState == Abstract.
    NeedsState Needs;

    // Milestone 2.6C: edge-trigger latch for NeedsThresholdEvent, owned
    // alongside Needs for the same reason - must survive Creature
    // unload/reload so a threshold doesn't spuriously re-fire on the next
    // materialize.
    NeedsThresholdState NeedsThresholds;

    // Milestone 2.7B1: the agent's single currently-selected goal, or
    // empty if none is active. Owned here for the same reason as Needs -
    // must survive Creature unload/reload within this process. Not
    // persisted across restart yet.
    std::optional<ActiveGoal> ActiveGoalState;

    // Milestone 2.11B: the agent's current routine destination (GO_TO_WORK/
    // GO_HOME), or empty if it has neither HomeLocation/WorkLocation set or
    // ActiveGoalState is currently Emergency (see RoutineSystem::
    // DeriveGoal()). Unlike ActiveGoalState this has no hysteresis of its
    // own - it is a pure function of current time, recomputed fresh every
    // Needs-cadence tick, not persisted across restart. Stored here (rather
    // than only ever a local in UpdateNeeds()) so a later milestone can
    // compare it tick-over-tick before reconciling it into a MOVE_TO
    // ActionRequest; 2.11B itself never acts on it.
    std::optional<RoutineGoal> RoutineGoalState;

    // Milestone 2.8F: the Action currently actually running in TrinityCore
    // for this agent's ActiveGoalState, if any. Set only after
    // ActionExecutor::ExecuteX() has actually returned Started - every
    // path that ends the underlying engine movement (interrupt, goal
    // completion, dematerialization, natural arrival) must also clear
    // this. Not persisted across restart.
    std::optional<ActiveAction> ActiveActionState;

    // Milestone 2.8G P2 fix: a MOVE_TO's arrival at a GET_FOOD target,
    // set by HandleActionCompletion() but deliberately not acted on until
    // UpdateNeeds()'s own goal-selection pass has run this same tick - see
    // PendingEatContinuation.h for why. One-shot; consumed (whether or not
    // it actually produces an Eat) the same tick it is first seen set. Not
    // persisted across restart.
    std::optional<PendingEatContinuation> PendingEat;
};

#endif // AIWORLD_AGENTRECORD_H
