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

#ifndef AIWORLD_CREATUREGROUPSTATE_H
#define AIWORLD_CREATUREGROUPSTATE_H

#include "Define.h"

// Milestone 2.12A: pure, persisted value-state for an AgentType::
// CreatureGroup aggregate agent (see AgentRecord::GroupState) - identity,
// not simulation. Deliberately minimal: Population and a single Territory
// center point are the only two facts this milestone needs to prove
// restart preserves group state, the same runtime gate 2.11A already
// established for an individual agent's HomeLocation/WorkLocation. No
// hunger/fear/combat/materialization-policy fields - that is 2.12B's
// coarse-simulation job, once this identity/persistence layer exists to
// build on; adding them now would be simulating a group this milestone
// explicitly does not run yet.
//
// Territory is a single point, not a radius/shape/patrol area - whatever
// shape a future materialization policy needs is that policy's own
// decision, not this identity model's. Its map is
// AgentRecord::MapId - a CreatureGroup's MapId means "where this group's
// territory is", the same role it already plays for an individual agent's
// spawn map, not duplicated here. Pure value: no Creature*/Map*/Unit* - a
// CreatureGroup never binds to a live Creature the way an individual
// agent does (see AgentRecord.h's own comment on SpawnId for this
// AgentType).
struct CreatureGroupState
{
    uint32 Population = 0;
    float TerritoryX = 0.0f;
    float TerritoryY = 0.0f;
    float TerritoryZ = 0.0f;
};

#endif // AIWORLD_CREATUREGROUPSTATE_H
