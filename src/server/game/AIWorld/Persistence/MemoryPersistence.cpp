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

#include "MemoryPersistence.h"
#include "Agent/AgentRegistry.h"
#include "DatabaseEnv.h"
#include "Memory/LongTermMemory.h"
#include "Log.h"

uint32 MemoryPersistence::LoadLongTermMemories(LongTermMemory& memory, AgentRegistry const& registry)
{
    uint32 loaded = 0;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_LONG_TERM_MEMORIES);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)
    {
        TC_LOG_INFO("ai.world", "AI persistence loaded 0 long-term memories");
        return 0;
    }

    do
    {
        Field* fields = result->Fetch();

        AgentId owner{ fields[1].GetUInt64() };
        uint64 persistentId = fields[0].GetUInt64();

        if (!registry.Find(owner))
        {
            TC_LOG_WARN("ai.world", "AI long-term memory persistentId={} references unknown agent={}, skipping (orphan)",
                persistentId, owner.Value);
            continue;
        }

        LongTermMemoryRecord record;
        record.PersistentId = persistentId;
        record.Owner = owner;

        record.Type = ObservationType(fields[2].GetUInt8());
        record.Importance = fields[3].GetFloat();

        record.SourceEventId = fields[4].GetUInt64();
        if (fields[5].GetBool())
            record.SourceEventType = WorldEventType(fields[6].GetUInt8());
        record.CorrelationId = fields[7].GetUInt64();

        record.SourceOccurredAtMs = fields[8].GetUInt64();
        record.FirstObservedAtMs = fields[9].GetUInt64();
        record.LastObservedAtMs = fields[10].GetUInt64();
        record.ObservationCount = fields[11].GetUInt32();

        record.Location.MapId = fields[12].GetUInt32();
        record.Location.X = fields[13].GetFloat();
        record.Location.Y = fields[14].GetFloat();
        record.Location.Z = fields[15].GetFloat();

        // Stored GUIDs are historical snapshots, not stable identity - see
        // the migration's own header comment. Reconstructed only so a
        // future consumer that genuinely needs the old runtime GUID (e.g.
        // display/debug) has it; AgentId/SpawnId/Entry remain the
        // reliable keys.
        record.Actor.Guid.SetRawValue(fields[16].GetUInt64());
        record.Actor.SpawnId = fields[17].GetUInt64();
        record.Actor.Entry = fields[18].GetUInt32();
        record.Actor.Agent = AgentId{ fields[19].GetUInt64() };

        record.Target.Guid.SetRawValue(fields[20].GetUInt64());
        record.Target.SpawnId = fields[21].GetUInt64();
        record.Target.Entry = fields[22].GetUInt32();
        record.Target.Agent = AgentId{ fields[23].GetUInt64() };

        record.Channel = PerceptionChannel(fields[24].GetUInt8());

        if (!memory.AddLoaded(record))
            continue;

        char const* sourceEventType = record.SourceEventType ? ToString(*record.SourceEventType) : "NONE";
        TC_LOG_INFO("ai.world", "AI long-term memory loaded persistentId={} agent={} type={} importance={:.2f} sourceEventType={}",
            record.PersistentId, owner.Value, ToString(record.Type), record.Importance, sourceEventType);

        ++loaded;
    } while (result->NextRow());

    TC_LOG_INFO("ai.world", "AI persistence loaded {} long-term memories", loaded);
    return loaded;
}

void MemoryPersistence::PersistLongTermMemory(LongTermMemoryRecord const& record)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_AI_LONG_TERM_MEMORY);

    stmt->setUInt64(0, record.Owner.Value);
    stmt->setUInt8(1, uint8(record.Type));
    stmt->setFloat(2, record.Importance);

    stmt->setUInt64(3, record.SourceEventId);
    stmt->setBool(4, record.SourceEventType.has_value());
    stmt->setUInt8(5, record.SourceEventType ? uint8(*record.SourceEventType) : 0);
    stmt->setUInt64(6, record.CorrelationId);

    stmt->setUInt64(7, record.SourceOccurredAtMs);
    stmt->setUInt64(8, record.FirstObservedAtMs);
    stmt->setUInt64(9, record.LastObservedAtMs);
    stmt->setUInt32(10, record.ObservationCount);

    stmt->setUInt32(11, record.Location.MapId);
    stmt->setFloat(12, record.Location.X);
    stmt->setFloat(13, record.Location.Y);
    stmt->setFloat(14, record.Location.Z);

    stmt->setUInt64(15, record.Actor.Guid.GetRawValue());
    stmt->setUInt64(16, record.Actor.SpawnId);
    stmt->setUInt32(17, record.Actor.Entry);
    stmt->setUInt64(18, record.Actor.Agent.Value);

    stmt->setUInt64(19, record.Target.Guid.GetRawValue());
    stmt->setUInt64(20, record.Target.SpawnId);
    stmt->setUInt32(21, record.Target.Entry);
    stmt->setUInt64(22, record.Target.Agent.Value);

    stmt->setUInt8(23, uint8(record.Channel));

    // Fire-and-forget by design - see the class comment. The world update
    // thread must never wait on this.
    CharacterDatabase.Execute(stmt);

    char const* sourceEventType = record.SourceEventType ? ToString(*record.SourceEventType) : "NONE";
    TC_LOG_DEBUG("ai.world", "AI long-term memory persistence queued agent={} type={} importance={:.2f} sourceEvent={} sourceEventType={}",
        record.Owner.Value, ToString(record.Type), record.Importance, record.SourceEventId, sourceEventType);
}
