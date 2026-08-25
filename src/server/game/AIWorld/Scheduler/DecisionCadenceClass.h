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

#ifndef AIWORLD_DECISIONCADENCECLASS_H
#define AIWORLD_DECISIONCADENCECLASS_H

#include "Define.h"

// Milestone 2.10B: which decision cadence/priority policy an agent gets
// this scheduler pass - Nearby (a real Player is within
// AIWorld.DecisionNearbyPlayerRange of the agent's live Creature right
// now) or Active (every other materialized agent). Deliberately not
// persisted anywhere - not part of AgentRecord, DecisionScheduleState, or
// the DB - recomputed fresh every pass from world-thread-resolved
// proximity (see AIWorldMgr::RunDecisionScheduler()), so a player walking
// up to (or away from) an agent changes its class the very next pass, with
// nothing to migrate and no restart required. This is not the roadmap's
// full ACTIVE/NEARBY/BACKGROUND/ABSTRACT simulation-tier system (that is a
// later milestone) - just two decision-cadence policies layered on top of
// today's single Materialized/Abstract WorldState.
enum class DecisionCadenceClass : uint8
{
    Nearby,
    Active
};

inline char const* ToString(DecisionCadenceClass cadenceClass)
{
    switch (cadenceClass)
    {
        case DecisionCadenceClass::Nearby: return "NEARBY";
        case DecisionCadenceClass::Active: return "ACTIVE";
        default:                           return "UNKNOWN";
    }
}

#endif // AIWORLD_DECISIONCADENCECLASS_H
