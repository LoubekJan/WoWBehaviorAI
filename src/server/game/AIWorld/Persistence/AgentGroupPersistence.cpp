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
        record.Kind = AgentGroupKind(fields[1].GetUInt8());
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
