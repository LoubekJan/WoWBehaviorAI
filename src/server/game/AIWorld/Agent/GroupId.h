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

#ifndef AIWORLD_GROUPID_H
#define AIWORLD_GROUPID_H

#include "Define.h"
#include <compare>

// Milestone 2.12D: stable identity for an AgentGroup - deliberately its own
// type, not an AgentId. A STATIC review of the 2.12C rename (CreatureGroup
// -> AgentGroup) found that a group was still a fake AgentRecord sharing
// AgentId/(MapId, SpawnId) identity with real, individually-bound agents -
// exactly the "social layer over independent agents" invariant this whole
// rename was supposed to establish, undermined by identity still being
// shared. A group has no SpawnId, no Creature/Map binding, and no need for
// one: it is never anything AgentRegistry::FindBySpawn()/BindCreature()
// could resolve, so it does not belong in that identity space at all.
// Assigned by AgentGroupRegistry on registration/AgentGroupPersistence on
// load - see AgentId.h for the identical shape/reasoning this mirrors.
struct GroupId
{
    uint64 Value = 0;

    explicit operator bool() const { return Value != 0; }
    auto operator<=>(GroupId const&) const = default;
};

#endif // AIWORLD_GROUPID_H
