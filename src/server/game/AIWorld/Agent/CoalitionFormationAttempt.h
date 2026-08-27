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

#ifndef AIWORLD_COALITIONFORMATIONATTEMPT_H
#define AIWORLD_COALITIONFORMATIONATTEMPT_H

#include "CoalitionFormationProfileId.h"
#include "CoalitionProposal.h"
#include "Define.h"
#include "GroupId.h"

// Milestone 2.12E4R: AIWorldMgr::RunCoalitionJoinStep()'s own runtime saga
// state - carries a CoalitionProposal that has already become a real
// GroupId (RequestCreateGroup() already confirmed) through the
// RequestJoinGroupWithPolicy() chain that joins Proposal.Members one at a
// time, NextMemberIndex naming how far the chain has gotten. Deliberately
// NOT persistent: this only exists on the world thread's own call stack/
// lambda captures for the lifetime of one formation attempt, the same way
// the plain (GroupId, members, index) parameters
// RunWolfCoalitionJoinStep() (2.12E4A/B) used to pass individually were
// never persisted either - bundling them into one value here is purely
// about keeping RunCoalitionJoinStep()/AbortCoalitionFormation()'s own
// signatures generic (profile-agnostic) rather than adding new state of
// any kind. See AIWorldMgr::RunCoalitionFormation()'s own comment for why
// a crash/restart mid-chain is not yet given special recovery handling
// (known, deferred P3).
struct CoalitionFormationAttempt
{
    // Milestone 2.12E4R P3 fix (STATIC review): defaults to Invalid, not
    // WolfLoose - see CoalitionFormationProfileId.h for why. Always
    // overwritten with the real profile's Id before this struct is ever
    // used - see RunCoalitionFormation()'s only construction site.
    CoalitionFormationProfileId ProfileId = CoalitionFormationProfileId::Invalid;
    CoalitionProposal Proposal;
    GroupId Group;
    std::size_t NextMemberIndex = 0;
};

#endif // AIWORLD_COALITIONFORMATIONATTEMPT_H
