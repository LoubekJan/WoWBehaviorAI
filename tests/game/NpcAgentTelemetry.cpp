/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Telemetry/TelemetryJsonCodec.h"
#include <limits>

TEST_CASE("Character telemetry distinguishes reviewed behavior from actual native template identity", "[AIWorld][Telemetry][NpcAgent]")
{
    AgentTelemetrySnapshot row;
    row.Agent = AgentId{901014}; row.SpawnId = 901014;
    row.Entry = 11328; row.TemplateEntry = 9800014; row.Name = "Sasa";
    auto json = SerializeAgentTelemetry({row}, 1);
    CHECK(json.find("\"entry\":11328,\"template_entry\":9800014,\"name\":\"Sasa\"") != std::string::npos);
    row.TemplateEntry = 0;
    json = SerializeAgentTelemetry({row}, 1);
    CHECK(json.find("\"entry\":11328,\"template_entry\":11328") != std::string::npos);

    // Passive observation of an installed name never grants action ownership
    // or introduces optional LLM/economy state while the world is paused.
    row.TemplateEntry = 9800014;
    row.NativeCreature = true;
    row.ControlMode = AgentControlMode::ObserveOnly;
    json = SerializeAgentTelemetry({row}, 2);
    CHECK(json.find("\"template_entry\":9800014") != std::string::npos);
    CHECK(json.find("\"control_mode\":\"OBSERVE_ONLY\"") != std::string::npos);
    CHECK(json.find("\"needs\":null") != std::string::npos);
    CHECK(json.find("\"economy\":null") != std::string::npos);
    CHECK(json.find("\"llm_agent\":null") != std::string::npos);
}

TEST_CASE("NPC telemetry exports public runtime metadata with exact request IDs", "[AIWorld][Telemetry][NpcAgent]")
{
    AgentTelemetrySnapshot row;
    row.Agent = AgentId{901035};
    row.SpawnId = 901035;
    row.ControlMode = AgentControlMode::AIWorldControlled;
    row.Live.emplace();
    row.Live->Alive = true;
    row.NpcAgent.emplace();
    auto& npc = *row.NpcAgent;
    npc.Name = "NPC \"one\"";
    npc.Status = "THINKING";
    npc.LastActionResult = "ARRIVED";
    npc.ActiveAction = "MOVE_TO";
    npc.RequestId = std::numeric_limits<uint64>::max();
    npc.NextThinkMs = 2000;
    npc.DialogueCount = 12;
    npc.MemoryRecords = 96;
    npc.MemoryRecalled = 6;
    npc.LastSpeech = "Ahoj, \"sousede\".";
    auto json = SerializeAgentTelemetry({row}, 1000);
    CHECK(json.find("\"version\":4") != std::string::npos);
    CHECK(json.find("\"llm_agent\":{\"name\":\"NPC \\\"one\\\"\"") != std::string::npos);
    CHECK(json.find("\"status\":\"THINKING\"") != std::string::npos);
    CHECK(json.find("\"last_action_result\":\"ARRIVED\"") != std::string::npos);
    CHECK(json.find("\"active_action\":\"MOVE_TO\"") != std::string::npos);
    CHECK(json.find("\"request_id\":\"18446744073709551615\"") != std::string::npos);
    CHECK(json.find("\"next_think_ms\":2000") != std::string::npos);
    CHECK(json.find("\"dialogue_count\":12") != std::string::npos);
    CHECK(json.find("\"memory_records\":96") != std::string::npos);
    CHECK(json.find("\"memory_recalled\":6") != std::string::npos);
    CHECK(json.find("\"last_speech\":\"Ahoj, \\\"sousede\\\".\"") != std::string::npos);
    CHECK(json.find("backstory") == std::string::npos);
    CHECK(json.find("personality") == std::string::npos);
}

TEST_CASE("NPC memory telemetry defaults to empty counts without memory contents", "[AIWorld][Telemetry][NpcAgent]")
{
    AgentTelemetrySnapshot row;
    row.ControlMode = AgentControlMode::AIWorldControlled;
    row.Live.emplace();
    row.NpcAgent.emplace();
    auto json = SerializeAgentTelemetry({row}, 1);
    CHECK(json.find("\"memory_records\":0") != std::string::npos);
    CHECK(json.find("\"memory_recalled\":0") != std::string::npos);
    CHECK(json.find("\"memories\"") == std::string::npos);
    CHECK(json.find("\"relationships\"") == std::string::npos);
    CHECK(json.find("\"private_history\"") == std::string::npos);
}

TEST_CASE("NPC telemetry remains absent for legacy native and unloaded rows", "[AIWorld][Telemetry][NpcAgent]")
{
    AgentTelemetrySnapshot row;
    CHECK(SerializeAgentTelemetry({row}, 1).find("\"llm_agent\":null") != std::string::npos);
    row.NpcAgent.emplace();
    row.NpcAgent->Name = "Only live";
    CHECK(SerializeAgentTelemetry({row}, 2).find("\"llm_agent\":null") != std::string::npos);
    row.Live.emplace();
    CHECK(SerializeAgentTelemetry({row}, 3).find("\"llm_agent\":null") != std::string::npos);
    row.NativeCreature = true;
    CHECK(SerializeAgentTelemetry({row}, 3).find("\"llm_agent\":null") != std::string::npos);
    CHECK(SerializeAgentTelemetry({row}, 3).find("Only live") == std::string::npos);
}
