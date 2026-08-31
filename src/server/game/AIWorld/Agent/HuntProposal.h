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

#ifndef AIWORLD_HUNTPROPOSAL_H
#define AIWORLD_HUNTPROPOSAL_H

#include "AgentId.h"
#include "Define.h"
#include "GroupId.h"
#include "HuntTargetProvenance.h"

// Milestone 2.12G3A: the INDIVIDUAL member's own decomposition output a
// future HuntIntentProjector (2.12G3B, not this milestone) will produce
// from a group-level HuntIntent, mirroring GroupMemberActionProposal's own
// role for REGROUP/ROAM. A group never gets to hunt anything directly
// (see HuntIntent.h); this is the value that would cross that boundary -
// still no live Creature*/Map*/Unit*, still only a proposal, never an
// authorization. Producing an actual ActionRequest from this (SourceGoal
// = a future GoalType::Hunt), and re-confirming the member still exists,
// is still actually a member of SourceGroup, and does not already have a
// higher-priority individual reason to ignore it, is AIWorldMgr's own job
// once wired up - the same DispatchGroupMemberActionProposal() discipline
// REGROUP/ROAM already use, not something this DTO performs itself.
//
// Target is carried as the full HuntTargetProvenance value rather than
// flattened into loose MapId/X/Y/Z fields the way GroupMemberActionProposal
// flattens its REGROUP/ROAM point - deliberately, because a HUNT proposal's
// target is not just a point in space, it is a specific identified entity
// whose Alive/ObservedAtMs/TargetGuid all travel together as one
// inseparable fact. Splitting them into independent top-level fields would
// let a caller read, say, a fresh X/Y/Z alongside a stale Alive/ObservedAtMs
// (or vice versa) without any structural guarantee they came from the same
// observation - keeping HuntTargetProvenance as one named field closes that
// off entirely.
//
// This milestone defines the SHAPE of a "dead/unloaded/different-map/stale
// target fails closed" rule as a CONTRACT on Target's own fields (see
// HuntTargetProvenance.h) for 2.12G3C's future per-member validation to
// actually enforce - no such validation logic exists yet, and none of it
// runs from this DTO alone. Likewise, decomposing a HuntIntent into
// per-member HuntProposal values (who is actually assigned to this hunt,
// who already has individual ownership of a higher-priority fight) is
// entirely 2.12G3B/G3C's future job; this milestone only names the value
// their decomposition will produce, exactly as GroupMemberActionProposal
// did for AgentGroupIntentProjector ahead of 2.12F2's own existence.
//
// No physical attack, spell cast, or other TrinityCore combat execution is
// implied or performed by this type in any milestone up through G3C - that
// remains individual Emergency/Normal combat ownership's job, never a group
// HUNT proposal's.
struct HuntProposal
{
    GroupId SourceGroup;
    AgentId Member;
    HuntTargetProvenance Target;

    uint64 StartedAtMs = 0;
};

#endif // AIWORLD_HUNTPROPOSAL_H
