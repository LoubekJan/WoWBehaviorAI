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

#include "Perception/PerceptionSystem.h"

// Milestone 2.13C6C: PerceptionSystem::ObserveDirectedEvent() is the one
// PerceptionSystem method that takes no live Creature*/Player* at all -
// unlike ObserveEvent()/ObserveNearbyPlayer()/ObserveNearbyCreature()
// (which need a live TrinityCore Creature/Player and are therefore not
// unit-testable in this sandbox), it is pure value logic over an AgentId
// and a WorldEvent, so it gets direct Catch2 coverage here.

namespace
{
    AgentId MakeAgentId(uint64 value)
    {
        AgentId id;
        id.Value = value;
        return id;
    }

    WorldEvent MakeDynamicQuestOutcomeEvent(WorldEventType type, AgentId targetAgent)
    {
        WorldEvent event;
        event.EventId = 555;
        event.CorrelationId = 9000;
        event.CauseEventId = 9001;
        event.OccurredAtMs = 12345;
        event.Type = type;

        event.Location.MapId = 0;
        event.Location.X = -9464.0f;
        event.Location.Y = 64.0f;
        event.Location.Z = 56.0f;

        event.Actor.Guid = ObjectGuid::Create<HighGuid::Player>(uint32(7));

        event.Target.Agent = targetAgent;
        event.Target.Guid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);

        return event;
    }
}

TEST_CASE("PerceptionSystem::ObserveDirectedEvent delivers a directed Rumor Observation for each dynamic quest outcome type", "[PerceptionSystem]")
{
    PerceptionSystem perception;
    AgentId issuer = MakeAgentId(42);

    auto verify = [&](WorldEventType type)
    {
        WorldEvent event = MakeDynamicQuestOutcomeEvent(type, issuer);

        std::optional<Observation> observation = perception.ObserveDirectedEvent(issuer, event);
        REQUIRE(observation.has_value());

        REQUIRE(observation->Observer.Value == issuer.Value);
        REQUIRE(observation->Type == ObservationType::WorldEvent);
        REQUIRE(observation->SourceEventId == event.EventId);
        REQUIRE(observation->CorrelationId == event.CorrelationId);
        REQUIRE(observation->SourceOccurredAtMs == event.OccurredAtMs);
        REQUIRE(observation->SourceEventType.has_value());
        REQUIRE(*observation->SourceEventType == type);

        REQUIRE(observation->Location.MapId == event.Location.MapId);
        REQUIRE(observation->Location.X == event.Location.X);
        REQUIRE(observation->Location.Y == event.Location.Y);
        REQUIRE(observation->Location.Z == event.Location.Z);

        REQUIRE(observation->Actor.Guid == event.Actor.Guid);
        REQUIRE(observation->Target.Agent.Value == event.Target.Agent.Value);
        REQUIRE(observation->Target.Guid == event.Target.Guid);

        REQUIRE(observation->Channel == PerceptionChannel::Rumor);
        REQUIRE(observation->Distance == 0.0f);
        REQUIRE_FALSE(observation->LineOfSight);
    };

    SECTION("DynamicQuestCompleted") { verify(WorldEventType::DynamicQuestCompleted); }
    SECTION("DynamicQuestFailed")    { verify(WorldEventType::DynamicQuestFailed); }
    SECTION("DynamicQuestExpired")   { verify(WorldEventType::DynamicQuestExpired); }
}

TEST_CASE("PerceptionSystem::ObserveDirectedEvent rejects an event that does not name this observer as its Target.Agent", "[PerceptionSystem]")
{
    PerceptionSystem perception;
    AgentId issuer = MakeAgentId(42);
    AgentId someoneElse = MakeAgentId(43);

    WorldEvent event = MakeDynamicQuestOutcomeEvent(WorldEventType::DynamicQuestCompleted, someoneElse);

    std::optional<Observation> observation = perception.ObserveDirectedEvent(issuer, event);
    REQUIRE_FALSE(observation.has_value());
}

TEST_CASE("PerceptionSystem::ObserveDirectedEvent rejects an event whose Target.Agent is empty", "[PerceptionSystem]")
{
    PerceptionSystem perception;
    AgentId issuer = MakeAgentId(42);

    WorldEvent event = MakeDynamicQuestOutcomeEvent(WorldEventType::DynamicQuestExpired, AgentId{});

    std::optional<Observation> observation = perception.ObserveDirectedEvent(issuer, event);
    REQUIRE_FALSE(observation.has_value());
}

TEST_CASE("PerceptionSystem::ObserveDirectedEvent rejects an empty/default observerId even if it happened to match Target.Agent", "[PerceptionSystem]")
{
    PerceptionSystem perception;
    WorldEvent event = MakeDynamicQuestOutcomeEvent(WorldEventType::DynamicQuestFailed, AgentId{});

    std::optional<Observation> observation = perception.ObserveDirectedEvent(AgentId{}, event);
    REQUIRE_FALSE(observation.has_value());
}
