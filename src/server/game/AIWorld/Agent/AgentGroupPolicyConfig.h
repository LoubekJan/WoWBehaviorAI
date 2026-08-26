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

#ifndef AIWORLD_AGENTGROUPPOLICYCONFIG_H
#define AIWORLD_AGENTGROUPPOLICYCONFIG_H

#include "Define.h"

// Milestone 2.12E3A: shared, already-validated policy configuration -
// AIWorldMgr loads and clamps this once at Initialize() from
// AIWorld.Loose/StableGroupMin/MaxMembers (the same "config struct built
// once, passed into a pure system per call" pattern _needsRates already
// uses for NeedsSystem/_foodTargetConfig for FoodTargetResolver, etc.).
// AgentGroupPolicySystem itself never reads sConfigMgr or owns tunable
// numbers of its own - it only ever sees whatever AgentGroupPolicyContext
// hands it, the same "caller resolves every live fact, system only
// combines them" discipline this whole codebase holds every pure value
// transform to.
struct AgentGroupPolicyConfig
{
    uint32 LooseMinMembers = 2;
    uint32 LooseMaxMembers = 5;

    // Milestone 2.12E3A: StableMinMembers is declared and loaded (see
    // AIWorld.StableGroupMinMembers) but not yet consulted by any rule -
    // AgentGroupPolicySystem::ShouldDissolve() never automatically
    // dissolves a Stable group regardless of size (see its own comment for
    // why). Kept here rather than added later so the config shape already
    // matches Loose's, ready for whatever future Stable rule needs it.
    uint32 StableMinMembers = 2;
    uint32 StableMaxMembers = 8;
};

#endif // AIWORLD_AGENTGROUPPOLICYCONFIG_H
