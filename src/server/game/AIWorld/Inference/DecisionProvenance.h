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

#ifndef AIWORLD_DECISIONPROVENANCE_H
#define AIWORLD_DECISIONPROVENANCE_H

#include "Define.h"
#include "Goal/GoalType.h"
#include "ObjectGuid.h"
#include <optional>

// Milestone 2.9C/2.9C P2 fix: what world-thread question a DecisionResponse
// actually answers - captured by AIClient from the *request's* own
// AgentContext (Goal/GoalStartedAtMs from Context.Goal, RuntimeGuid from
// Context.Self.Guid - never from anything ai-server sends back; ai-server
// never gets to claim what it was asked), the same "client's own echo, not
// the server's claim" pattern AIResponse::Agent/SnapshotSequence already
// use. SnapshotSequence alone doesn't prove a decision still answers a
// live question: Needs/ActiveGoal/GoalStartedAtMs can all change between
// two snapshot captures without SnapshotSequence itself advancing
// (Needs/Goal update on their own ~1s cadence, independent of the
// snapshot/decision cadence) - Goal/GoalStartedAtMs guard against that.
// RuntimeGuid guards a different drift: AgentRegistry::BindCreature()
// documents that a SpawnId identifies a spawn, not a runtime object - the
// Creature this request was actually built from can despawn and a new
// Creature for the same SpawnId (a different runtime incarnation, new
// ObjectGuid) can take over the same AgentRecord/WorldState==Materialized/
// even the same ActiveGoalState attempt, all without SnapshotSequence
// advancing either. A translator must compare Goal/GoalStartedAtMs against
// the agent's actual current ActiveGoalState AND RuntimeGuid against both
// the freshly-resolved live Creature's GUID and AgentRecord::RuntimeGuid,
// discarding outright on any mismatch - never silently retarget a stale
// decision onto whatever goal attempt or Creature incarnation happens to
// be current now. See AIWorldMgr::ValidateDecisionIntent(). Translating a
// DecisionIntent that names no specific target of its own (Flee doesn't
// carry a FleeFromGuid) still needs this: the actor's current threat
// victim is resolved fresh from the live Creature at translation time, not
// carried in Provenance, but that resolution is only trustworthy once
// RuntimeGuid proves it's still the same Creature the decision was asked
// about in the first place.
struct DecisionProvenance
{
    std::optional<GoalType> Goal;
    uint64 GoalStartedAtMs = 0;
    ObjectGuid RuntimeGuid;
};

#endif // AIWORLD_DECISIONPROVENANCE_H
