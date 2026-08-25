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

#ifndef AIWORLD_ROUTINEACTIVITY_H
#define AIWORLD_ROUTINEACTIVITY_H

#include "Define.h"
#include "RoutineActivityType.h"

// Milestone 2.11D: RoutineActivitySystem's only output - what an agent is
// doing right now at its routine destination. StartedAtMs is set once,
// only when AIWorldMgr::UpdateNeeds() actually transitions into this
// activity (Type changing, or NONE -> something), not recomputed every
// tick the same activity holds - unlike RoutineGoal, which has no state of
// its own at all, this does carry across ticks (so a later milestone can
// answer "how long has this agent been working") but is still never
// persisted across restart, same as RoutineGoalState/ActiveActionState.
// Pure value: no AgentId, Creature*, Map*, or DB.
struct RoutineActivity
{
    RoutineActivityType Type = RoutineActivityType::Rest;
    uint64 StartedAtMs = 0;
};

#endif // AIWORLD_ROUTINEACTIVITY_H
