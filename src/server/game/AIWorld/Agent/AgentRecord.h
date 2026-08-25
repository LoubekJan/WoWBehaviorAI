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
#include "AgentEconomyState.h"
#include "AgentId.h"
#include "AgentLocation.h"
#include "AgentType.h"
#include "CreatureGroupState.h"
#include "Define.h"
#include "Goal/ActiveGoal.h"
#include "Goal/RoutineActivity.h"
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
//
// Milestone 2.12A: for AgentType::CreatureGroup, SpawnId does NOT name a
// TrinityCore creature spawn the way it does for every other AgentType -
// a group never binds 1:1 to one Creature. It is instead an opaque,
// caller-assigned identifier, unique per MapId the same way a real
// spawn_id is, chosen by convention from a range reserved well above any
// real creature.guid in the world DB (see the 2.12A migration's own
// comment) - but that convention is not itself what keeps a CreatureGroup
// unbindable. Nothing at the DB level rules out a genuine collision with
// a real creature.guid, so a CreatureGroup is non-bindable BY
// CONSTRUCTION instead: every live-Creature-resolution and live-spawn-
// enrichment call site in AIWorldMgr.cpp goes through ResolveLiveCreature()/
// FindLiveAgentBySpawn() (2.12A P2 fix), which exclude AgentType::
// CreatureGroup unconditionally, regardless of what SpawnId value it holds
// or what it might collide with - and AgentRegistry::BindCreature() itself
// refuses the same way (2.12A P3 fix), so this holds for any future
// caller too, not only today's. MapId is the group's territory map - see
// CreatureGroupState.h. This is what lets a
// CreatureGroup agent reuse the exact same AgentRegistry/
// AgentPersistence (map_id, spawn_id) identity model as everything else,
// with no schema/registry change of its own - see SimulationTier.h's own
// DeriveSimulationTier() for why that keeps it correctly Abstract.
struct AgentRecord
{
    AgentId Id;
    AgentType Type = AgentType::Civilian;

    uint32 MapId = 0;
    uint64 SpawnId = 0;

    // Milestone 2.11A: persistent, optional - most agents (e.g. GUARD) have
    // neither. Behavior/capability layer (profession, daily cycle, ...) is
    // deliberately not modeled as its own AgentType - "has a home and a
    // workplace" is expressed purely by both being set here, regardless of
    // AgentType. Pa Maclure happens to be a CIVILIAN, but that is only his
    // identity, not what makes RoutineSystem (2.11B) treat him as routine-
    // eligible; nothing gates on AgentType::Civilian anywhere in that path.
    std::optional<AgentLocation> HomeLocation;
    std::optional<AgentLocation> WorkLocation;

    // Milestone 2.12A: persistent, optional - set only for an
    // AgentType::CreatureGroup aggregate agent (see CreatureGroupState.h
    // and this struct's own SpawnId comment above). Everything else
    // (individual Civilian/Guard/Merchant agents) leaves this empty, the
    // same optional-by-capability pattern HomeLocation/WorkLocation
    // already use. Load-only in 2.12A - nothing mutates it yet (no
    // hunger/combat/materialization-policy simulation exists for a group
    // until 2.12B), so there is no save path to wire up here either.
    std::optional<CreatureGroupState> GroupState;

    // Milestone 2.11E2: persistent, not optional - every agent has one
    // (defaulting to zero), unlike HomeLocation/WorkLocation. Only ever
    // mutated by AIWorldMgr::UpdateNeeds() on a WORK ActionCompletion
    // reaching Succeeded/Performed (see ActionCompletion.h) - never on
    // Started alone, and never for REST. Written back to ai_agents via
    // AgentPersistence::SaveEconomyState() (fire-and-forget async, the
    // same pattern MemoryPersistence uses) immediately after the in-memory
    // mutation, so it survives a restart without the world thread ever
    // blocking on the DB.
    AgentEconomyState EconomyState;

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

    // Milestone 2.11B/2.11C: the agent's current routine destination
    // (GoalType::GoToWork/GoHome), or empty if it has neither HomeLocation/
    // WorkLocation set or ActiveGoalState is currently Emergency (see
    // RoutineSystem::DeriveGoal()). Unlike ActiveGoalState this has no
    // hysteresis of its own - it is a pure function of current time,
    // recomputed fresh every Needs-cadence tick, not persisted across
    // restart. Stored here (rather than only ever a local in UpdateNeeds())
    // so it can be compared tick-over-tick before being reconciled into a
    // MOVE_TO ActionRequest - see AIWorldMgr::UpdateNeeds()'s 2.11C
    // arbitration comment for when that reconciliation is (and is not)
    // allowed to happen. This field only ever holds what routine currently
    // wants; whether that is actually safe to act on right now is decided
    // there, never here.
    std::optional<RoutineGoal> RoutineGoalState;

    // Milestone 2.11D: what the agent is actually doing right now at its
    // routine destination (Work/Rest), or empty - see
    // RoutineActivitySystem::DeriveActivity() for the reality checks this
    // requires (materialized, alive, no ActiveGoalState/ActiveActionState,
    // actually standing at RoutineGoalState's own target). Unlike
    // RoutineGoalState, StartedAtMs here is stable across ticks while the
    // same activity holds - only set fresh on an actual transition, not
    // recomputed from scratch every tick. Runtime-only, not persisted
    // across restart, the same as RoutineGoalState/ActiveActionState.
    std::optional<RoutineActivity> RoutineActivityState;

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
