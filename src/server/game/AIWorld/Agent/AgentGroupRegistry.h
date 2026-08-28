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

#ifndef AIWORLD_AGENTGROUPREGISTRY_H
#define AIWORLD_AGENTGROUPREGISTRY_H

#include "AgentGroupKind.h"
#include "AgentGroupRecord.h"
#include "AgentId.h"
#include "Define.h"
#include "GroupId.h"
#include <map>
#include <vector>

// Milestone 2.12D (STATIC review P2 fix): owns every persistent AgentGroup's
// AgentGroupRecord for the process's lifetime - the group-side counterpart
// to AgentRegistry, deliberately its own class rather than another
// AgentType inside AgentRegistry. A group never binds to a live Creature
// and has no SpawnId/MapId identity of its own (see GroupId.h), so it has
// none of AgentRegistry's Creature-binding surface (BindCreature/
// UnbindCreature/FindBySpawn) - only identity storage and lookup.
//
// Purely in-memory: does not generate GroupIds and does not talk to any
// database. AgentGroupPersistence is the authority for minting and loading
// GroupIds - this class only ever receives already-assigned ones through
// Add(). Not thread-safe in general: mutating calls are world-thread-only,
// like AgentRegistry/AIWorldMgr itself.
//
// Milestone 2.12E4C2 P3 fix (STATIC review): _groups is an ordered
// std::map, not std::unordered_map - GetGroupsAfterUntil() needs a stable,
// GroupId-ascending iteration order it can resume from an arbitrary
// cursor via upper_bound() in O(log n + returned count), not O(n). Find()/
// Add()/Remove() go from O(1) to O(log n) as the tradeoff, acceptable at
// the group-count scale this subsystem targets (hundreds to low
// thousands of concurrently-existing AgentGroups, not millions) - see
// GetGroupsAfterUntil()'s own comment for the bounded-discovery problem
// this solves.
class TC_GAME_API AgentGroupRegistry
{
    public:
        // Adds an already-identified record (from AgentGroupPersistence).
        // Rejects and returns false for GroupId=0 or a duplicate GroupId.
        bool Add(AgentGroupRecord record);

        AgentGroupRecord* Find(GroupId id);
        AgentGroupRecord const* Find(GroupId id) const;

        // Milestone 2.12E1: erases the whole record - membership included,
        // since it lives inside AgentGroupRecord::Members, not a separate
        // container. Only ever called by AgentGroupLifecycleSystem::
        // DissolveGroup(), after AgentGroupPersistence::DeleteGroup() has
        // already removed the DB-side rows. Returns whether a record was
        // actually erased (false for an unknown GroupId - not an error,
        // the caller decides what that means).
        bool Remove(GroupId id);

        std::vector<GroupId> GetGroups() const;

        // Milestone 2.12E4C2 P2 fix, round 3 (STATIC review): the highest
        // currently-registered GroupId, or GroupId{} (0) if the registry
        // is empty - O(1) via _groups' own ordering (rbegin()). Exists to
        // let a caller snapshot a scan-cycle boundary (see
        // GetGroupsAfterUntil()'s own comment) without paying for a full
        // traversal just to find it.
        GroupId GetHighestGroupId() const;

        // Milestone 2.12E4C2 P3 fix, round 2 (STATIC review): bounded,
        // cursor-based discovery WITHIN one scan cycle - up to maxCount
        // GroupIds strictly greater than after AND less than or equal to
        // until, in ascending GroupId order, using _groups' own ordering
        // (upper_bound()) rather than materializing and filtering every
        // registered group the way GetGroups() does. Exists for
        // AIWorldMgr::RunCoalitionMaintenance()'s own scan (see
        // AIWorld.CoalitionMaintenanceScanMaxPerPass): rather than paying
        // O(all groups) every maintenance pass just to find which ones
        // are even candidates, a caller advances its own cursor
        // (after = the last GroupId this call returned) across repeated
        // calls/passes, seeing every group that existed at the START of
        // the current cycle (until = that cycle's own high-water mark,
        // from GetHighestGroupId() taken once when the cycle began) over
        // several bounded passes instead of all of them in one unbounded
        // one.
        //
        // until is what makes a scan cycle provably FINITE even under
        // continuous group creation - an earlier version had no upper
        // bound at all (every call simply returned "up to maxCount
        // entries past after", with nothing capping how far ahead new
        // GroupIds could keep appearing): if groups are created faster
        // than the scan can advance past them, there is always a higher
        // GroupId waiting past the cursor, the scan never reaches empty,
        // never wraps, and the earliest-created groups - the ones closest
        // to GroupId{} - starve indefinitely, never revisited. Capping
        // discovery at the cycle's own snapshot means new groups created
        // mid-cycle are simply deferred to the NEXT cycle (whose own
        // GetHighestGroupId() snapshot will include them) rather than
        // extending the current one.
        //
        // after = GroupId{} (0) starts from the beginning of a cycle -
        // every real GroupId is nonzero (see GroupId.h), so this never
        // ambiguously skips a real group 0. A cursor naming a GroupId that
        // has since been dissolved is harmless: upper_bound() only cares
        // about the VALUE, not whether a group with exactly that id still
        // exists, so it simply resumes from whatever survives immediately
        // after it - a dissolve between passes never permanently disrupts
        // the scan. Returns an empty vector once after is at or past
        // until (including the degenerate until = GroupId{} case, an
        // empty registry) or past the last entry _groups actually has -
        // this method never wraps a cycle around on its own; a caller
        // doing cursor-based wraparound (see AIWorldMgr::
        // RunCoalitionMaintenance()) is the one that decides when a cycle
        // has ended and starts a new one (a fresh GetHighestGroupId()
        // snapshot, cursor reset to GroupId{}).
        std::vector<GroupId> GetGroupsAfterUntil(GroupId after, GroupId until, uint32 maxCount) const;

        // Milestone 2.12E4R: true if member belongs to any registered
        // group of exactly this Kind - the group-domain question
        // AIWorldMgr's own automatic coalition formation/revalidation
        // needs (see CoalitionFormationSystem.h/AIWorldMgr::
        // RunCoalitionJoinStep()), pulled out of AIWorldMgr itself since
        // "does member belong to a Kind-X group" is a fact about
        // AgentGroupRegistry's own owned state, not about wolves or any
        // other specific formation profile. O(groups * members-per-group)
        // - a plain linear scan, the same complexity the AIWorldMgr-side
        // version this replaces already had for a single member; for
        // scanning an entire candidate list in one pass, prefer building a
        // one-off membership set from GetGroups()/Find() instead of
        // calling this once per candidate (see AIWorldMgr::
        // CollectMemberIdsOfKind()) - AgentGroupRegistry deliberately does
        // not keep a permanent member->group reverse index yet, since one
        // would have to be kept correctly in sync across every Join/Leave/
        // Dissolve completion, which is not this milestone's scope.
        bool IsMemberOfKind(AgentId member, AgentGroupKind kind) const;

    private:
        std::map<uint64, AgentGroupRecord> _groups;
};

#endif // AIWORLD_AGENTGROUPREGISTRY_H
