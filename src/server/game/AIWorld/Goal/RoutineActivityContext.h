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

#ifndef AIWORLD_ROUTINEACTIVITYCONTEXT_H
#define AIWORLD_ROUTINEACTIVITYCONTEXT_H

#include "Define.h"
#include "GoalType.h"
#include <optional>

// Milestone 2.11D: the world-thread facts RoutineActivitySystem::
// DeriveActivity() is allowed to see, as plain values - the same "resolve
// live facts, hand over only this" pattern ActionValidationContext already
// uses for ActionSystem::Validate(). AIWorldMgr resolves all of this
// itself (it already has the live Creature/AgentRecord at the call site);
// RoutineActivitySystem never sees a Creature*, AgentRecord*, Map*, or the
// registry.
struct RoutineActivityContext
{
    // AgentRecord::RoutineGoalState->Type, or nullopt if the agent has no
    // RoutineGoalState at all right now (no HomeLocation/WorkLocation, or
    // an Emergency ActiveGoal is currently suppressing it - see
    // RoutineSystem::DeriveGoal()).
    std::optional<GoalType> CurrentRoutineGoal;

    bool Materialized = false;
    bool Alive = false;

    // AgentRecord::ActiveGoalState/::ActiveActionState both being truthy
    // or not - WORK/REST must yield to either the instant one exists, the
    // same single-owner rule 2.11C's arbitration already enforces for
    // routine movement.
    bool HasActiveGoal = false;
    bool HasActiveAction = false;

    // Same map as CurrentRoutineGoal's own target AND within
    // ArrivalToleranceYards of it (see ArrivalTolerance.h) - computed by
    // AIWorldMgr from the actor's actual live position, never trusted from
    // RoutineGoalState alone. An agent halfway through its commute must
    // never read as WORK/REST just because routine already wants it there.
    bool AtRoutineTarget = false;
};

#endif // AIWORLD_ROUTINEACTIVITYCONTEXT_H
