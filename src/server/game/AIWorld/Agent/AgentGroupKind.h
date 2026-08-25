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

#ifndef AIWORLD_AGENTGROUPKIND_H
#define AIWORLD_AGENTGROUPKIND_H

#include "Define.h"

// Milestone 2.12C: what kind of social bond an AgentGroup represents -
// purely declarative in this milestone, since no lifecycle (create/join/
// leave/dissolve, that is later roadmap work) exists yet to actually
// enforce either policy. A wolf pack that forms opportunistically and
// drifts apart is Loose; a scripted/persistent group (a family, a guard
// patrol, a deliberately-authored pack) whose membership only changes on
// an explicit event is Stable. Institutional (a faction, a guild) is
// deliberately not added yet - two kinds is enough to prove the model,
// per the roadmap's own "netbych to teď nepřeháněl".
enum class AgentGroupKind : uint8
{
    Loose,
    Stable
};

inline char const* ToString(AgentGroupKind kind)
{
    switch (kind)
    {
        case AgentGroupKind::Loose:  return "LOOSE";
        case AgentGroupKind::Stable: return "STABLE";
        default:                     return "UNKNOWN";
    }
}

#endif // AIWORLD_AGENTGROUPKIND_H
