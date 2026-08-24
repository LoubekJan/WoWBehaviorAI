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
#include <optional>

// Milestone 2.9C: what world-thread question a DecisionResponse actually
// answers - captured by AIClient from the *request's* own
// AgentContext::Goal (never from anything ai-server sends back; ai-server
// never gets to claim what it was asked), the same "client's own echo, not
// the server's claim" pattern AIResponse::Agent/SnapshotSequence already
// use. SnapshotSequence alone doesn't prove a decision still answers a
// live question: Needs/ActiveGoal/GoalStartedAtMs/the actor's threat
// victim can all change between two snapshot captures without
// SnapshotSequence itself advancing (Needs/Goal update on their own ~1s
// cadence, independent of the snapshot/decision cadence). Anything that
// wants to translate a DecisionIntent into an ActionRequest must compare
// this against the agent's actual current ActiveGoalState first and
// discard outright on any mismatch - never silently retarget a stale
// decision onto whatever goal attempt happens to be active now. See
// AIWorldMgr::ValidateDecisionIntent().
struct DecisionProvenance
{
    std::optional<GoalType> Goal;
    uint64 GoalStartedAtMs = 0;
};

#endif // AIWORLD_DECISIONPROVENANCE_H
