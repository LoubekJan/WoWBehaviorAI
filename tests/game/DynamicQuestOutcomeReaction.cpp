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

#include "tc_catch2.h"

#include "Quest/DynamicQuestOutcomeReaction.h"

namespace
{
    AgentId MakeAgentId(uint64 value)
    {
        AgentId id;
        id.Value = value;
        return id;
    }

    MemoryRecord MakeDynamicQuestOutcomeMemory(AgentId owner, AgentId targetAgent, WorldEventType sourceType,
        uint64 sourceEventId, uint64 lastObservedAtMs, PerceptionChannel channel = PerceptionChannel::Sight)
    {
        MemoryRecord memory;
        memory.Id = sourceEventId; // arbitrary but distinct
        memory.Owner = owner;
        memory.Type = ObservationType::WorldEvent;
        memory.SourceEventId = sourceEventId;
        memory.CorrelationId = sourceEventId + 1000;
        memory.SourceOccurredAtMs = lastObservedAtMs - 1;
        memory.SourceEventType = sourceType;
        memory.FirstObservedAtMs = lastObservedAtMs;
        memory.LastObservedAtMs = lastObservedAtMs;
        memory.ExpiresAtMs = lastObservedAtMs + 60000;
        memory.ObservationCount = 1;
        memory.Target.Agent = targetAgent;
        memory.Channel = channel;
        return memory;
    }
}

TEST_CASE("SelectDynamicQuestOutcomeReaction maps each outcome WorldEventType to its own Kind", "[DynamicQuestOutcomeReaction]")
{
    AgentId issuer = MakeAgentId(42);

    SECTION("Completed")
    {
        std::vector<MemoryRecord> memories{ MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestCompleted, 1, 1000) };
        std::optional<DynamicQuestOutcomeReaction> reaction = SelectDynamicQuestOutcomeReaction(issuer, memories);
        REQUIRE(reaction.has_value());
        REQUIRE(reaction->Kind == DynamicQuestOutcomeReactionKind::Completed);
    }

    SECTION("Failed")
    {
        std::vector<MemoryRecord> memories{ MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestFailed, 1, 1000) };
        std::optional<DynamicQuestOutcomeReaction> reaction = SelectDynamicQuestOutcomeReaction(issuer, memories);
        REQUIRE(reaction.has_value());
        REQUIRE(reaction->Kind == DynamicQuestOutcomeReactionKind::Failed);
    }

    SECTION("Expired")
    {
        std::vector<MemoryRecord> memories{ MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestExpired, 1, 1000) };
        std::optional<DynamicQuestOutcomeReaction> reaction = SelectDynamicQuestOutcomeReaction(issuer, memories);
        REQUIRE(reaction.has_value());
        REQUIRE(reaction->Kind == DynamicQuestOutcomeReactionKind::Expired);
    }
}

TEST_CASE("SelectDynamicQuestOutcomeReaction copies SourceEventId/CorrelationId/ObservedAtMs/Channel from the winning memory", "[DynamicQuestOutcomeReaction]")
{
    AgentId issuer = MakeAgentId(42);

    SECTION("Sight")
    {
        MemoryRecord memory = MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestCompleted, 777, 5000, PerceptionChannel::Sight);
        std::vector<MemoryRecord> memories{ memory };
        std::optional<DynamicQuestOutcomeReaction> reaction = SelectDynamicQuestOutcomeReaction(issuer, memories);
        REQUIRE(reaction.has_value());
        REQUIRE(reaction->SourceEventId == memory.SourceEventId);
        REQUIRE(reaction->CorrelationId == memory.CorrelationId);
        REQUIRE(reaction->ObservedAtMs == memory.LastObservedAtMs);
        REQUIRE(reaction->Channel == PerceptionChannel::Sight);
    }

    SECTION("Rumor")
    {
        MemoryRecord memory = MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestExpired, 778, 5000, PerceptionChannel::Rumor);
        std::vector<MemoryRecord> memories{ memory };
        std::optional<DynamicQuestOutcomeReaction> reaction = SelectDynamicQuestOutcomeReaction(issuer, memories);
        REQUIRE(reaction.has_value());
        REQUIRE(reaction->SourceEventId == memory.SourceEventId);
        REQUIRE(reaction->CorrelationId == memory.CorrelationId);
        REQUIRE(reaction->ObservedAtMs == memory.LastObservedAtMs);
        REQUIRE(reaction->Channel == PerceptionChannel::Rumor);
    }
}

TEST_CASE("SelectDynamicQuestOutcomeReaction rejects a memory whose Target.Agent is not this issuer", "[DynamicQuestOutcomeReaction]")
{
    AgentId issuer = MakeAgentId(42);
    AgentId someoneElse = MakeAgentId(43);

    std::vector<MemoryRecord> memories{ MakeDynamicQuestOutcomeMemory(issuer, someoneElse, WorldEventType::DynamicQuestCompleted, 1, 1000) };
    REQUIRE_FALSE(SelectDynamicQuestOutcomeReaction(issuer, memories).has_value());
}

TEST_CASE("SelectDynamicQuestOutcomeReaction rejects a memory owned by a different agent", "[DynamicQuestOutcomeReaction]")
{
    AgentId issuer = MakeAgentId(42);
    AgentId someoneElse = MakeAgentId(43);

    // Owned by someone else, even though it names `issuer` as its Target.Agent -
    // GetActiveForAgent(issuer, ...) would never actually return this in
    // practice, but the selector must not rely on the caller alone.
    std::vector<MemoryRecord> memories{ MakeDynamicQuestOutcomeMemory(someoneElse, issuer, WorldEventType::DynamicQuestCompleted, 1, 1000) };
    REQUIRE_FALSE(SelectDynamicQuestOutcomeReaction(issuer, memories).has_value());
}

TEST_CASE("SelectDynamicQuestOutcomeReaction rejects a non-WorldEvent memory", "[DynamicQuestOutcomeReaction]")
{
    AgentId issuer = MakeAgentId(42);

    MemoryRecord memory = MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestCompleted, 1, 1000);
    memory.Type = ObservationType::CreatureSeen;

    std::vector<MemoryRecord> memories{ memory };
    REQUIRE_FALSE(SelectDynamicQuestOutcomeReaction(issuer, memories).has_value());
}

TEST_CASE("SelectDynamicQuestOutcomeReaction rejects a memory with no SourceEventType", "[DynamicQuestOutcomeReaction]")
{
    AgentId issuer = MakeAgentId(42);

    MemoryRecord memory = MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestCompleted, 1, 1000);
    memory.SourceEventType.reset();

    std::vector<MemoryRecord> memories{ memory };
    REQUIRE_FALSE(SelectDynamicQuestOutcomeReaction(issuer, memories).has_value());
}

TEST_CASE("SelectDynamicQuestOutcomeReaction rejects a memory whose SourceEventType is an unrelated WorldEventType", "[DynamicQuestOutcomeReaction]")
{
    AgentId issuer = MakeAgentId(42);

    MemoryRecord memory = MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::CreatureKilled, 1, 1000);

    std::vector<MemoryRecord> memories{ memory };
    REQUIRE_FALSE(SelectDynamicQuestOutcomeReaction(issuer, memories).has_value());
}

TEST_CASE("SelectDynamicQuestOutcomeReaction rejects an empty issuer", "[DynamicQuestOutcomeReaction]")
{
    std::vector<MemoryRecord> memories{ MakeDynamicQuestOutcomeMemory(AgentId{}, AgentId{}, WorldEventType::DynamicQuestCompleted, 1, 1000) };
    REQUIRE_FALSE(SelectDynamicQuestOutcomeReaction(AgentId{}, memories).has_value());
}

TEST_CASE("SelectDynamicQuestOutcomeReaction deterministically picks the most recent eligible memory", "[DynamicQuestOutcomeReaction]")
{
    AgentId issuer = MakeAgentId(42);

    SECTION("distinct LastObservedAtMs - the later one wins")
    {
        std::vector<MemoryRecord> memories{
            MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestCompleted, 1, 1000),
            MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestExpired, 2, 5000),
            MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestFailed, 3, 3000),
        };

        std::optional<DynamicQuestOutcomeReaction> reaction = SelectDynamicQuestOutcomeReaction(issuer, memories);
        REQUIRE(reaction.has_value());
        REQUIRE(reaction->Kind == DynamicQuestOutcomeReactionKind::Expired);
        REQUIRE(reaction->SourceEventId == 2);
    }

    SECTION("tied LastObservedAtMs - the higher SourceEventId wins, never iteration order")
    {
        std::vector<MemoryRecord> memories{
            MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestCompleted, 5, 1000),
            MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestFailed, 9, 1000),
            MakeDynamicQuestOutcomeMemory(issuer, issuer, WorldEventType::DynamicQuestExpired, 7, 1000),
        };

        std::optional<DynamicQuestOutcomeReaction> reaction = SelectDynamicQuestOutcomeReaction(issuer, memories);
        REQUIRE(reaction.has_value());
        REQUIRE(reaction->Kind == DynamicQuestOutcomeReactionKind::Failed);
        REQUIRE(reaction->SourceEventId == 9);
    }
}
