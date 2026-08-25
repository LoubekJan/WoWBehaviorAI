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

#include "AgentGroupPersistence.h"
#include "Agent/AgentGroupRegistry.h"
#include "Agent/AgentRegistry.h"
#include "DatabaseEnv.h"
#include "Log.h"

void AgentGroupPersistence::LoadGroupIdSequence()
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_GROUP_ID_SEQUENCE);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)
    {
        // Milestone 2.12E1 P2 fix (STATIC review, round 3): fail-closed -
        // _groupIdAllocatorValid stays false, so CreateGroup() refuses to
        // mint anything rather than guessing at _nextGroupId's
        // untrustworthy class-default.
        TC_LOG_ERROR("ai.world", "AgentGroupPersistence: ai_agent_group_id_sequence has no row (expected exactly one, id=1) - "
            "did the 2.12E1 P2 migration run? GroupId allocator left invalid; CreateGroup() will refuse to mint until this is fixed.");
        return;
    }

    uint64 nextGroupId = result->Fetch()[0].GetUInt64();

    // Milestone 2.12E1 P2 fix (STATIC review, round 4): a nonzero row alone
    // is not enough to trust - it must also exceed the highest group_id
    // actually present in ai_agent_groups right now, including a row
    // LoadGroups() itself would later reject (e.g. for an invalid kind) -
    // this query applies no such filter, so it sees exactly what physically
    // exists. Without this check, a sequence value that was never advanced
    // past an existing group's own id would let CreateGroup() mint a
    // "fresh" GroupId that collides with (and, via its own read-back,
    // could be misread as confirming) a real, unrelated existing group.
    CharacterDatabasePreparedStatement* maxStmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_GROUP_MAX_ID);
    PreparedQueryResult maxResult = CharacterDatabase.Query(maxStmt);
    uint64 maxExistingGroupId = maxResult ? maxResult->Fetch()[0].GetUInt64() : 0;

    if (nextGroupId == 0 || nextGroupId <= maxExistingGroupId)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupPersistence: ai_agent_group_id_sequence.next_group_id={} is not a trustworthy high-water mark "
            "(must be nonzero and greater than MAX(ai_agent_groups.group_id)={}) - GroupId allocator left invalid; CreateGroup() will refuse to mint until this is fixed.",
            nextGroupId, maxExistingGroupId);
        return;
    }

    _nextGroupId = nextGroupId;
    _groupIdAllocatorValid = true;

    TC_LOG_INFO("ai.world", "AI persistence loaded group id sequence, next group id={}", _nextGroupId);
}

uint32 AgentGroupPersistence::LoadGroups(AgentGroupRegistry& registry)
{
    uint32 loaded = 0;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_GROUPS);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)
    {
        TC_LOG_INFO("ai.world", "AI persistence loaded 0 agent groups");
        return 0;
    }

    do
    {
        Field* fields = result->Fetch();

        AgentGroupRecord record;
        record.Id = GroupId{ fields[0].GetUInt64() };

        // Hardening (STATIC review P3 fix): kind used to be a blind cast -
        // AgentGroupKind(fields[1].GetUInt8()) would silently accept any
        // uint8 the column happened to hold, including values with no
        // named enumerator, and ToString() would just print "UNKNOWN"
        // forever after. Fail-closed instead, the same discipline
        // AgentPersistence::LoadAgents() already holds ai_agents to: an
        // explicit switch over only the values AgentGroupKind actually
        // declares, everything else rejected (logged, skipped) rather than
        // silently reinterpreted or degraded.
        uint8 rawKind = fields[1].GetUInt8();
        switch (rawKind)
        {
            case uint8(AgentGroupKind::Loose):
                record.Kind = AgentGroupKind::Loose;
                break;
            case uint8(AgentGroupKind::Stable):
                record.Kind = AgentGroupKind::Stable;
                break;
            default:
                TC_LOG_ERROR("ai.world",
                    "AgentGroupPersistence: refusing to load group id={} with invalid kind={} (not LOOSE(0)/STABLE(1)), skipping",
                    record.Id.Value, rawKind);
                continue;
        }

        record.TerritoryMapId = fields[2].GetUInt32();
        record.TerritoryX = fields[3].GetFloat();
        record.TerritoryY = fields[4].GetFloat();
        record.TerritoryZ = fields[5].GetFloat();
        record.Resources = fields[6].GetFloat();
        record.Version = fields[7].GetUInt64();

        if (!registry.Add(record))
            continue;

        TC_LOG_INFO("ai.world", "AI agent group loaded id={} kind={} territoryMap={} resources={:.4f} version={}",
            record.Id.Value, ToString(record.Kind), record.TerritoryMapId, record.Resources, record.Version);

        ++loaded;
    } while (result->NextRow());

    TC_LOG_INFO("ai.world", "AI persistence loaded {} agent groups", loaded);
    return loaded;
}

uint32 AgentGroupPersistence::LoadGroupMembers(AgentGroupRegistry& groupRegistry, AgentRegistry const& agentRegistry)
{
    uint32 loaded = 0;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_GROUP_MEMBERS);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)
    {
        TC_LOG_INFO("ai.world", "AI persistence loaded 0 agent group members");
        return 0;
    }

    do
    {
        Field* fields = result->Fetch();

        GroupId groupId{ fields[0].GetUInt64() };
        AgentId memberId{ fields[1].GetUInt64() };
        uint64 joinedAtMs = fields[2].GetUInt64();

        // No FK to ai_agent_groups - an orphaned group_id is logged and
        // skipped, the same tolerance ai_long_term_memories' own orphan
        // handling already has. Never fatal to the rest of this load.
        AgentGroupRecord* group = groupRegistry.Find(groupId);
        if (!group)
        {
            TC_LOG_ERROR("ai.world", "AgentGroupPersistence: ai_agent_group_members row for group_id={} member_agent_id={} references an unregistered group, skipping",
                groupId.Value, memberId.Value);
            continue;
        }

        // Milestone 2.12D P2 fix (STATIC review): unlike the superseded
        // CreatureGroup model, a member is never a legitimate forward
        // reference here - it must already be a real, independent
        // AgentRecord by the time its group's membership loads (create/
        // register the individual agent first, then the membership edge
        // that names it, never the other way around).
        if (!agentRegistry.Find(memberId))
        {
            TC_LOG_ERROR("ai.world", "AgentGroupPersistence: ai_agent_group_members row for group_id={} member_agent_id={} references an unregistered agent, skipping",
                groupId.Value, memberId.Value);
            continue;
        }

        AgentGroupMembership membership;
        membership.Member = memberId;
        membership.JoinedAtMs = joinedAtMs;
        group->Members.push_back(membership);

        TC_LOG_INFO("ai.world", "AI agent group member loaded group={} member={} joinedAtMs={}", groupId.Value, memberId.Value, joinedAtMs);

        ++loaded;
    } while (result->NextRow());

    TC_LOG_INFO("ai.world", "AI persistence loaded {} agent group members", loaded);
    return loaded;
}

void AgentGroupPersistence::SaveGroupState(GroupId id, AgentGroupRecord& record)
{
    // Same reasoning as AgentPersistence::SaveEconomyState()'s own P3 fix:
    // unconditional, first thing, regardless of what the caller already
    // did.
    ++record.Version;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_AI_AGENT_GROUP);
    stmt->setUInt8(0, uint8(record.Kind));
    stmt->setUInt32(1, record.TerritoryMapId);
    stmt->setFloat(2, record.TerritoryX);
    stmt->setFloat(3, record.TerritoryY);
    stmt->setFloat(4, record.TerritoryZ);
    stmt->setFloat(5, record.Resources);
    stmt->setUInt64(6, record.Version);
    stmt->setUInt64(7, id.Value);

    // Bound again for the statement's own "AND version < ?" guard - see
    // AgentGroupRecord::Version and CHAR_UPD_AI_AGENT_GROUP's own comment
    // for why.
    stmt->setUInt64(8, record.Version);

    // Fire-and-forget by design - see the class comment. The world update
    // thread must never wait on this.
    CharacterDatabase.Execute(stmt);
}

GroupId AgentGroupPersistence::CreateGroup(AgentGroupKind kind, uint32 territoryMapId, float territoryX, float territoryY, float territoryZ, float resources)
{
    // Milestone 2.12E1 P2 fix (STATIC review, round 3): fail-closed - never
    // mint against an allocator LoadGroupIdSequence() could not confirm.
    if (!_groupIdAllocatorValid)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupPersistence::CreateGroup: refusing to mint a GroupId - the allocator is not valid "
            "(LoadGroupIdSequence() never confirmed ai_agent_group_id_sequence), group was not created");
        return GroupId{};
    }

    uint64 candidateId = _nextGroupId;
    uint64 reservedNext = candidateId + 1;

    // Milestone 2.12E1 P2 fix (STATIC review, round 3): the reservation
    // write is itself confirmed by read-back, exactly like every other
    // write in this class, before _nextGroupId is ever advanced in memory
    // or a group row is ever inserted - an earlier version trusted
    // DirectExecute() alone here, which could silently fail while the
    // runtime had already moved on, reintroducing the exact "restart
    // repeats the same id" bug this table exists to prevent. The write
    // itself is an absolute SET (not a relative increment), so retrying
    // this same reservation on a future call - which is exactly what
    // happens below if the read-back fails - is safe and idempotent.
    CharacterDatabasePreparedStatement* seqUpdateStmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_AI_AGENT_GROUP_ID_SEQUENCE);
    seqUpdateStmt->setUInt64(0, reservedNext);
    CharacterDatabase.DirectExecute(seqUpdateStmt);

    CharacterDatabasePreparedStatement* seqSelectStmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_GROUP_ID_SEQUENCE);
    PreparedQueryResult seqResult = CharacterDatabase.Query(seqSelectStmt);
    if (!seqResult || seqResult->Fetch()[0].GetUInt64() != reservedNext)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupPersistence::CreateGroup: reservation of group id={} could not be confirmed "
            "(ai_agent_group_id_sequence does not read back as {}), group was not created - _nextGroupId left unchanged for retry",
            candidateId, reservedNext);
        return GroupId{};
    }

    // Only now, with the reservation itself confirmed, does _nextGroupId
    // move and candidateId become real.
    _nextGroupId = reservedNext;
    GroupId newId{ candidateId };

    CharacterDatabasePreparedStatement* insertStmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_AI_AGENT_GROUP);
    insertStmt->setUInt64(0, newId.Value);
    insertStmt->setUInt8(1, uint8(kind));
    insertStmt->setUInt32(2, territoryMapId);
    insertStmt->setFloat(3, territoryX);
    insertStmt->setFloat(4, territoryY);
    insertStmt->setFloat(5, territoryZ);
    insertStmt->setFloat(6, resources);
    CharacterDatabase.DirectExecute(insertStmt);

    // DirectExecute() doesn't report success/failure, so the only way to
    // know whether the row actually landed is to read it back - the same
    // discipline AgentPersistence::CreateCreatureAgent() already holds
    // AgentId to via its own (map_id, spawn_id) binding. The reservation
    // above is already confirmed and _nextGroupId already advanced
    // regardless of what happens here - newId is never handed out again
    // either way.
    CharacterDatabasePreparedStatement* selectStmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_GROUP_BY_ID);
    selectStmt->setUInt64(0, newId.Value);
    PreparedQueryResult result = CharacterDatabase.Query(selectStmt);
    if (!result)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupPersistence: INSERT for group id={} did not produce a readable row, group was not created", newId.Value);
        return GroupId{};
    }

    return newId;
}

bool AgentGroupPersistence::AddGroupMember(GroupId groupId, AgentId memberId, uint64 joinedAtMs)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_AI_AGENT_GROUP_MEMBER);
    stmt->setUInt64(0, groupId.Value);
    stmt->setUInt64(1, memberId.Value);
    stmt->setUInt64(2, joinedAtMs);
    CharacterDatabase.DirectExecute(stmt);

    // Milestone 2.12E1 P2 fix (STATIC review, round 2): confirm the row
    // actually exists before the caller is allowed to trust it - see this
    // method's own header comment.
    CharacterDatabasePreparedStatement* selectStmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_GROUP_MEMBER);
    selectStmt->setUInt64(0, groupId.Value);
    selectStmt->setUInt64(1, memberId.Value);
    PreparedQueryResult result = CharacterDatabase.Query(selectStmt);
    if (!result)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupPersistence: INSERT for group_id={} member_agent_id={} did not produce a readable row, membership was not created",
            groupId.Value, memberId.Value);
        return false;
    }

    return true;
}

bool AgentGroupPersistence::RemoveGroupMember(GroupId groupId, AgentId memberId)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_AI_AGENT_GROUP_MEMBER);
    stmt->setUInt64(0, groupId.Value);
    stmt->setUInt64(1, memberId.Value);
    CharacterDatabase.DirectExecute(stmt);

    // Milestone 2.12E1 P2 fix (STATIC review, round 2): confirm the row is
    // actually gone before the caller is allowed to trust it - see this
    // method's own header comment. A row that is still readable here means
    // the DELETE did not (yet) take effect - reported as failure, not
    // silently treated as "already removed".
    CharacterDatabasePreparedStatement* selectStmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_GROUP_MEMBER);
    selectStmt->setUInt64(0, groupId.Value);
    selectStmt->setUInt64(1, memberId.Value);
    PreparedQueryResult result = CharacterDatabase.Query(selectStmt);
    if (result)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupPersistence: DELETE for group_id={} member_agent_id={} did not remove the row, membership still exists in the DB",
            groupId.Value, memberId.Value);
        return false;
    }

    return true;
}

bool AgentGroupPersistence::DeleteGroup(GroupId groupId)
{
    // Milestone 2.12E1 P2 fix (STATIC review, round 2): both DELETEs as
    // one CharacterDatabaseTransaction, committed synchronously and
    // atomically via DirectCommitTransaction() - an earlier version issued
    // these as two independent DirectExecute() calls, which could leave an
    // orphaned membership row if interrupted between the two. Membership
    // rows first, group row second within the transaction - not that it
    // matters for atomicity, but it keeps the statement order matching the
    // dependency order a reader would expect.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    CharacterDatabasePreparedStatement* membersStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_AI_AGENT_GROUP_MEMBERS_BY_GROUP);
    membersStmt->setUInt64(0, groupId.Value);
    trans->Append(membersStmt);

    CharacterDatabasePreparedStatement* groupStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_AI_AGENT_GROUP);
    groupStmt->setUInt64(0, groupId.Value);
    trans->Append(groupStmt);

    CharacterDatabase.DirectCommitTransaction(trans);

    // Confirm the group row is actually gone before the caller is allowed
    // to trust it - see this method's own header comment. Membership rows
    // are not checked separately: they were deleted in the same atomic
    // transaction as the group row, so this one confirmation covers both.
    CharacterDatabasePreparedStatement* selectStmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_GROUP_BY_ID);
    selectStmt->setUInt64(0, groupId.Value);
    PreparedQueryResult result = CharacterDatabase.Query(selectStmt);
    if (result)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupPersistence: DELETE transaction for group id={} did not remove the row, group still exists in the DB",
            groupId.Value);
        return false;
    }

    return true;
}
