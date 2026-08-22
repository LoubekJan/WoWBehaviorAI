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

#ifndef AIWORLD_ACTIVEGOAL_H
#define AIWORLD_ACTIVEGOAL_H

#include "Define.h"
#include "GoalType.h"

// Milestone 2.7B1/2.7B2: the one goal an agent is currently pursuing,
// selected deterministically from GoalCandidate[] and retained with its
// own hysteresis (see GoalSystem::UpdateActiveGoal()) so it doesn't flap
// every tick a Need dips a hair below its candidate threshold. No status
// field - a terminal outcome (GoalStatus, via GoalCompletion) ends this
// goal's existence rather than being a state it sits in; while this
// struct exists at all, the goal is active by definition. Pure value: no
// AgentId, Creature*, Map*, or DB.
struct ActiveGoal
{
    GoalType Type = GoalType::GetFood;
    GoalPriority Priority = GoalPriority::Normal;
    GoalSource Source = GoalSource::Needs;

    float Utility = 0.0f;

    uint64 StartedAtMs = 0;

    // Milestone 2.7B2: how long this goal attempt gets before it's
    // considered FAILED if its Need still hasn't dropped below the
    // retention threshold. Set once at Activate/Interrupt time from
    // GoalSystem's fixed per-GoalType default - no config knob yet.
    uint32 TimeoutMs = 0;
};

#endif // AIWORLD_ACTIVEGOAL_H
