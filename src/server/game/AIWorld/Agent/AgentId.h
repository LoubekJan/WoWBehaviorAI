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

#ifndef AIWORLD_AGENTID_H
#define AIWORLD_AGENTID_H

#include "Define.h"
#include <compare>

// Stable identity for a persistent agent - deliberately NOT a SpawnId. A
// SpawnId names a TrinityCore creature spawn; an AgentId names the
// long-lived AI entity currently (or not currently) bound to that spawn,
// and is meant to keep existing across the spawn's Creature being unloaded
// and reloaded. Assigned by AgentRegistry on registration. Only stable for
// this process's lifetime until Milestone 2.2 persists it to the database.
struct AgentId
{
    uint64 Value = 0;

    explicit operator bool() const { return Value != 0; }
    auto operator<=>(AgentId const&) const = default;
};

#endif // AIWORLD_AGENTID_H
