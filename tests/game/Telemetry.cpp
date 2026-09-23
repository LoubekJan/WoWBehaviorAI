/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "tc_catch2.h"
#include "Telemetry/TelemetryJsonCodec.h"
#include "Memory/LongTermMemory.h"
#include <limits>

TEST_CASE("Observer exports owned living state and exact stockpiles", "[AIWorld][Telemetry]")
{
    AgentTelemetrySnapshot agent;
    agent.Agent = AgentId{80542};
    agent.SpawnId = 80542;
    agent.Name = "NPC \"quote\"\\line\n";
    agent.Live.emplace();
    agent.Live->Alive = true;
    agent.Economy.Money = std::numeric_limits<uint64>::max();
    agent.Economy.Food = 2;
    agent.Economy.Resource = 3;
    agent.LivingRole.emplace();
    std::string diagnostic = "HUNT_CHASE_MISSING";
    agent.LivingRole->HuntStatus = diagnostic;
    diagnostic = "CHANGED_AFTER_CAPTURE";
    agent.LivingRole->SprintMultiplier = 1.35f;
    agent.LivingRole->SprintRemainingMs = 500;
    agent.EffectiveGoal = GoalType::Hunt;
    agent.GoalOwner = "GROUP";
    agent.CoordinationPhase = "ENGAGING";
    agent.Groups.push_back({17, "LOOSE", "WOLF_LOOSE", 4, 0.5f, {0, -9900, 40, 30}});
    agent.Groups.push_back({18, "STABLE", "INVALID", 2, 0.2f, {0, -9900, 40, 30}});
    std::string json = SerializeAgentTelemetry({agent}, 1234);
    CHECK(json.find("\"version\":3") != std::string::npos);
    CHECK(json.find("NPC \\\"quote\\\"\\\\line\\n") != std::string::npos);
    CHECK(json.find("\"money\":\"18446744073709551615\"") != std::string::npos);
    CHECK(json.find("HUNT_CHASE_MISSING") != std::string::npos);
    CHECK(json.find("CHANGED_AFTER_CAPTURE") == std::string::npos);
    CHECK(json.find("\"sprint_remaining_ms\":500") != std::string::npos);
    CHECK(json.find("\"effective_goal\":\"HUNT\"") != std::string::npos);
    CHECK(json.find("\"id\":17") != std::string::npos);
    CHECK(json.find("\"id\":18") != std::string::npos);
}

TEST_CASE("Observer never labels retained engine observations as background live data", "[AIWorld][Telemetry]")
{
    AgentTelemetrySnapshot background;
    background.LivingRole.emplace();
    background.Movement.emplace();
    background.Target.emplace();
    background.Destination.emplace();
    background.LivingWolf = true;
    background.MealTargetSpawnId = 10;
    background.Economy.Food = 7;
    std::string json = SerializeAgentTelemetry({background}, 100);
    for (char const* name : {"living_role", "movement", "target", "destination", "meal_target_spawn_id"})
        CHECK(json.find(std::string("\"") + name + "\":null") != std::string::npos);
    CHECK(json.find("\"living_wolf\":false") != std::string::npos);
    CHECK(json.find("\"food\":7") != std::string::npos);
    CHECK(json.find("\"source\":\"spawn\"") != std::string::npos);
    CHECK(SerializeAgentTelemetry({}, 42) == "{\"version\":3,\"captured_at_ms\":42,\"agents\":[],\"memory_page\":null}");
}

TEST_CASE("Observer memory reads are bounded and anchor pages against new insertions", "[AIWorld][Telemetry]")
{
    LongTermMemory memory;
    for (uint64 i = 1; i <= 3; ++i)
    {
        LongTermMemoryRecord record;
        record.Owner = AgentId{7}; record.PersistentId = i;
        REQUIRE(memory.AddLoaded(record));
    }
    auto first = memory.GetPage(AgentId{7}, 0, 2);
    REQUIRE(first.Records.size() == 2);
    CHECK(first.Total == 3);
    CHECK(first.Anchor == 3);
    CHECK(first.Records[0].PersistentId == 3);
    CHECK(first.Records[1].PersistentId == 2);
    LongTermMemoryRecord newest;
    newest.Owner = AgentId{7}; newest.PersistentId = 4;
    REQUIRE(memory.AddLoaded(newest));
    auto next = memory.GetPage(AgentId{7}, 2, 2, uint32(first.Anchor));
    REQUIRE(next.Records.size() == 1);
    CHECK(next.Records[0].PersistentId == 1);
    CHECK(next.Total == 4);
    CHECK(next.Anchor == 3);
    CHECK(memory.GetPage(AgentId{7}, 0, 2).Records[0].PersistentId == 4);
    CHECK(memory.GetPage(AgentId{8}, 0, 2).Records.empty());
    CHECK(memory.GetPage(AgentId{7}, std::numeric_limits<uint32>::max(), 2).Records.empty());
    CHECK(memory.GetPage(AgentId{7}, 0, 0).Records.empty());
}

TEST_CASE("Observer accepts only a precise memory read request", "[AIWorld][Telemetry]")
{
    std::string prefix(32, 'a');
    auto request = ParseMemoryPageRequest(prefix + ":80542:25:100");
    REQUIRE(request.has_value());
    CHECK(request->Agent == AgentId{80542});
    CHECK(request->Offset == 25);
    CHECK(request->Anchor == 100);
    for (std::string const& suffix : {":0:0:0", ":1:-1:0", ":1:0:4294967296", ":1:0:0:0", ":1:0:0x", ":1:0:", ":18446744073709551616:0:0"})
        CHECK_FALSE(ParseMemoryPageRequest(prefix + suffix).has_value());
    CHECK_FALSE(ParseMemoryPageRequest("restart worldserver").has_value());
    CHECK_FALSE(ParseMemoryPageRequest(std::string(32, 'z') + ":1:0:0").has_value());
}

TEST_CASE("Observer serializes historical long-term memories independently of live positions", "[AIWorld][Telemetry]")
{
    MemoryPageTelemetry page;
    page.Request = {std::string(32, 'a'), AgentId{7}, 0, 0};
    page.Total = page.Anchor = 1;
    MemoryTelemetryRecord item;
    item.Memory.PersistentId = std::numeric_limits<uint64>::max();
    item.Memory.SourceEventType = WorldEventType::CreatureKilled;
    item.Memory.Importance = 0.9f;
    item.Memory.ObservationCount = 4;
    item.Memory.Actor.Agent = AgentId{7};
    item.ActorName = "NPC \"one\"";
    item.Memory.Target.Guid = ObjectGuid::Create<HighGuid::Player>(42);
    item.Memory.Channel = PerceptionChannel::Rumor;
    page.Records.push_back(item);
    auto json = SerializeAgentTelemetry({}, 1234, page);
    CHECK(json.find("\"persistent_id\":\"18446744073709551615\"") != std::string::npos);
    CHECK(json.find("\"event_type\":\"CREATURE_KILLED\"") != std::string::npos);
    CHECK(json.find("\"channel\":\"RUMOR\"") != std::string::npos);
    CHECK(json.find("\"observation_count\":4") != std::string::npos);
    CHECK(json.find("\"kind\":\"PLAYER\"") != std::string::npos);
    CHECK(json.find("\"guid\"") == std::string::npos);
    CHECK(json.find("NPC \\\"one\\\"") != std::string::npos);
}

