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
#include "AgentGroupMembership.h"
#include "AgentGroupRecord.h"
#include "AgentId.h"
#include "Define.h"
#include "GroupId.h"
#include <map>
#include <unordered_map>
#include <unordered_set>
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
        // Milestone 2.12F2 P3 fix (STATIC review): also indexes any Members
        // the record already carries into _memberGroups (see AddMember()'s
        // own comment) - in practice always empty at creation time (every
        // real caller adds members afterward, one at a time, through
        // AddMember()), but Add() stays correct for a record that already
        // carries some regardless, rather than silently under-indexing one.
        bool Add(AgentGroupRecord record);

        AgentGroupRecord* Find(GroupId id);
        AgentGroupRecord const* Find(GroupId id) const;

        // Milestone 2.12E1: erases the whole record - membership included,
        // since it lives inside AgentGroupRecord::Members, not a separate
        // container. Only ever called by AgentGroupLifecycleSystem::
        // DissolveGroup(), after AgentGroupPersistence::DeleteGroup() has
        // already removed the DB-side rows. Returns whether a record was
        // actually erased (false for an unknown GroupId - not an error,
        // the caller decides what that means). Milestone 2.12F2 P3 fix
        // (STATIC review): also removes every one of that group's own
        // members from _memberGroups (see AddMember()'s own comment) -
        // every membership this group ever held is gone along with it.
        bool Remove(GroupId id);

        // Milestone 2.12F2 P3 fix (STATIC review): the one place
        // AgentGroupRecord::Members is ever mutated to ADD a member -
        // AgentGroupLifecycleSystem::RequestJoinGroup()'s own confirmed-join
        // completion and AgentGroupPersistence::LoadGroupMembers() both go
        // through this now, instead of pushing onto group->Members
        // directly, so _memberGroups (the reverse AgentId -> GroupId[]
        // index below) can never drift out of sync with the forward
        // Members list it mirrors. Returns false, touching nothing, for an
        // unknown groupId OR a membership.Member that is already one of
        // this group's own Members (2.12F2 P3 fix, round 2, STATIC
        // review: an earlier version trusted every caller to have already
        // checked this itself, e.g. AgentGroupLifecycleSystem::
        // RequestJoinGroup() already does - but a duplicate slipping
        // through here would forward-add a second, indistinguishable
        // Members entry while the reverse _memberGroups side silently
        // stayed a one-element set, which RemoveMember() would then
        // desynchronize permanently: it erases only the first forward
        // entry but the WHOLE reverse one, so the member ends up still
        // forward-listed yet reverse-invisible to GetGroupsOfMember()/
        // IsMemberOfKind(). As the authoritative membership-mutation
        // boundary, this invariant belongs here, not only in whichever
        // caller happens to check it today).
        bool AddMember(GroupId groupId, AgentGroupMembership const& membership);

        // Milestone 2.12F2 P3 fix (STATIC review): the one place a member
        // is ever REMOVED - AgentGroupLifecycleSystem::RequestLeaveGroup()'s
        // own confirmed-leave completion goes through this now instead of
        // erasing from group->Members directly, for the same reverse-index
        // consistency reason AddMember() exists. Returns false, touching
        // nothing, for an unknown groupId or a memberId that is not
        // currently one of its members - the same idempotent/fail-safe
        // shape RequestLeaveGroup() itself already documents for calling
        // it twice in a row.
        bool RemoveMember(GroupId groupId, AgentId member);

        // Milestone 2.12F2 P3 fix (STATIC review): which groups member
        // currently belongs to, right now - O(1) average to find the
        // member's own entry in _memberGroups, O(k) to copy it out where k
        // is however many groups that specific member is in (in practice
        // almost always 0 or 1; nothing in this codebase enforces an upper
        // bound on it, hence returning every one found rather than
        // assuming at most one). Exists so a caller that already knows
        // WHICH member it cares about (e.g. AIWorldMgr::
        // RunCoalitionCoordination()'s own cross-group overlap check) never
        // has to scan the whole registry just to answer "which groups is
        // this one agent in" - see IsMemberOfKind()'s own comment for the
        // O(all groups) alternative this replaces for that specific use.
        // An unknown/never-a-member AgentId returns an empty vector, not an
        // error - not being in any group is an entirely ordinary state for
        // most agents.
        std::vector<GroupId> GetGroupsOfMember(AgentId member) const;

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
        // other specific formation profile.
        //
        // Milestone 2.12F2 P3 fix (STATIC review): now backed by
        // _memberGroups/GetGroupsOfMember() - O(k) where k is however many
        // groups member is actually in (almost always 0 or 1), not the
        // O(all groups) linear scan an earlier version did. That earlier
        // version's own comment reasoned AgentGroupRegistry deliberately
        // kept no permanent member->group reverse index, since one would
        // have to stay correctly in sync across every Join/Leave/Dissolve
        // completion - AddMember()/RemoveMember()/Remove() are now that
        // single, authoritative sync point (see each one's own comment),
        // so that reasoning no longer holds; this method simply uses the
        // index that now exists rather than re-deriving its own separate
        // scan. For scanning an entire candidate list in one pass, prefer
        // building a one-off membership set from GetGroups()/Find() instead
        // of calling this once per candidate anyway (see AIWorldMgr::
        // CollectMemberIdsOfKind()) - not because this got slower, but
        // because that pattern still does strictly less total work when
        // the candidate list itself is large.
        bool IsMemberOfKind(AgentId member, AgentGroupKind kind) const;

    private:
        // Milestone 2.12F2 P3 fix (STATIC review): AgentId::Value -> the
        // GroupId::Value of every group that AgentId currently belongs to -
        // generic membership infrastructure (any future caller that needs
        // "which groups is this agent in" reuses this, not a Regroup- or
        // coordination-specific index), kept in sync exclusively by
        // AddMember()/RemoveMember()/Add()/Remove() - nothing outside this
        // class ever touches it directly, the same way nothing outside
        // this class mutates _groups directly either. A plain
        // std::unordered_set per member (not a single GroupId) because
        // nothing in this codebase's own policy (AgentGroupPolicySystem::
        // CanJoin() checks only duplicate membership WITHIN one group, see
        // its own comment) actually prevents one agent from being a member
        // of more than one group at once - see AIWorldMgr::
        // RunCoalitionCoordination()'s own cross-group overlap check, which
        // exists specifically because that case is possible.
        std::map<uint64, AgentGroupRecord> _groups;
        std::unordered_map<uint64, std::unordered_set<uint64>> _memberGroups;
};

#endif // AIWORLD_AGENTGROUPREGISTRY_H
