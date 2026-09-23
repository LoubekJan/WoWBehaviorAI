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
    CHECK(json.find("\"version\":2") != std::string::npos);
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
    CHECK(SerializeAgentTelemetry({}, 42) == "{\"version\":2,\"captured_at_ms\":42,\"agents\":[]}");
}

