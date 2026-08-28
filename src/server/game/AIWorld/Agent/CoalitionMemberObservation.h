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

#ifndef AIWORLD_COALITIONMEMBEROBSERVATION_H
#define AIWORLD_COALITIONMEMBEROBSERVATION_H

#include "AgentId.h"
#include "Define.h"

// Milestone 2.12E4C1: one existing AgentGroup member's current world-thread
// observation, as a plain value - never a Creature*/Map*, the same "live
// TrinityCore objects stay local, only a DTO crosses into a pure system"
// boundary CoalitionCandidate.h already establishes for formation. Built by
// AIWorldMgr (2.12E4C2) for every AgentGroupMembership a group currently
// has, from whatever it can currently observe about that member's own
// AgentRecord/live Creature - NOT from a fresh spatial scan the way
// CollectCoalitionCandidates() is (maintenance walks an existing group's
// own membership looking for who no longer belongs; formation walks the
// whole registry looking for new candidates).
//
// Materialized/Alive default false, the fail-safe reading whenever there is
// no live Creature to observe at all (unloaded, or the member's own
// AgentRecord no longer resolves) - MapId/X/Y/Z are only meaningful when
// Materialized is true. CoalitionMaintenanceSystem::Evaluate() never
// proposes a Leave for a non-Materialized (or dead) member - see its own
// class comment for why absence from the grid must never be misread as a
// social departure.
struct CoalitionMemberObservation
{
    AgentId MemberId;
    bool Materialized = false;
    bool Alive = false;

    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

#endif // AIWORLD_COALITIONMEMBEROBSERVATION_H
