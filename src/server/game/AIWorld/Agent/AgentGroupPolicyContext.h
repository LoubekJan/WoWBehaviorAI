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

#ifndef AIWORLD_AGENTGROUPPOLICYCONTEXT_H
#define AIWORLD_AGENTGROUPPOLICYCONTEXT_H

#include "AgentGroupOperationSource.h"
#include "AgentGroupPolicyConfig.h"

// Milestone 2.12E3A: everything AgentGroupPolicySystem's three methods need
// beyond the AgentGroupRecord/AgentId already passed alongside it - built
// fresh by the caller for each call, the same "no state, every dependency
// a parameter" shape AgentGroupPolicyConfig's own comment already
// describes. Config is a value copy, not a reference - this struct is
// meant to be constructed on the stack right before a CanJoin()/
// CanLeave()/ShouldDissolve() call and never outlives it, so a copy of a
// four-uint32 struct costs nothing and carries no dangling-reference risk
// the way holding AgentGroupPolicyConfig const& would if some future
// caller ever built one ahead of time instead.
//
// Source only matters to CanLeave() (see AgentGroupPolicySystem.h) -
// CanJoin()/ShouldDissolve() ignore it. Still passed uniformly to all
// three rather than only to CanLeave(), so call sites build one context
// per request and hand it to whichever policy method they need, without
// needing to know in advance which methods do and don't read Source.
struct AgentGroupPolicyContext
{
    AgentGroupPolicyConfig Config;
    AgentGroupOperationSource Source = AgentGroupOperationSource::Manual;
};

#endif // AIWORLD_AGENTGROUPPOLICYCONTEXT_H
