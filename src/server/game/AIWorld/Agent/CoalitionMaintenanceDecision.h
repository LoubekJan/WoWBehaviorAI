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

#ifndef AIWORLD_COALITIONMAINTENANCEDECISION_H
#define AIWORLD_COALITIONMAINTENANCEDECISION_H

#include "AgentId.h"
#include "CoalitionMaintenanceDecisionType.h"
#include "Define.h"
#include "GroupId.h"

// Milestone 2.12E4C1: CoalitionMaintenanceSystem::Evaluate()'s output - see
// CoalitionMaintenanceDecisionType.h for what each Type means and why this
// is a proposal, never an authorization. Member is only meaningful for
// Type == LeaveMember (default-constructed/zero otherwise). Group is
// always set to the group Evaluate() was asked about, regardless of Type
// (including None), so a caller can log a "nothing to do" result without
// having to remember which group it asked about separately.
struct CoalitionMaintenanceDecision
{
    CoalitionMaintenanceDecisionType Type = CoalitionMaintenanceDecisionType::None;
    GroupId Group;
    AgentId Member;
};

#endif // AIWORLD_COALITIONMAINTENANCEDECISION_H
