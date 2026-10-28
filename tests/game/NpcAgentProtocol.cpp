/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Inference/NpcAgentJsonCodec.h"
#include <limits>

namespace
{
    NpcAgentRequest Request()
    {
        NpcAgentRequest request;
        request.RequestId = 91;
        request.Agent = AgentId{42};
        request.SnapshotSequence = 7;
        request.Context.Profile = {"Alda", "A farmer's daughter.\nShe remembers the orchard.", "Patient"};
        request.Context.Self.CurrentAction = "IDLE";
        request.Context.Self.LastActionResult = "ARRIVED";
        request.Context.Actions = {{3, "MOVE_TO", "Walk toward the orchard"}, {4, "REST", "Sit down"}};
        request.Context.Nearby = {{8, "Visitor", "PLAYER", 4.0f}};
        request.Context.Emotes = {"NONE", "WAVE", "TALK"};
        request.Context.SpeechTargets = {8};
        return request;
    }

    std::string Body(std::string speech = "Hello", std::string action = "3", std::string target = "8", std::string emote = "WAVE")
    {
        return "{\"protocol_version\":1,\"request_id\":91,\"agent_id\":42,\"snapshot_sequence\":7,"
            "\"decision\":{\"action_token\":" + action + ",\"speech\":\"" + speech +
            "\",\"speech_target\":" + target + ",\"emote\":\"" + emote + "\"}}";
    }
}

TEST_CASE("NPC agent protocol escapes genuine lore and serializes bounded token grants", "[AIWorld][NpcAgent]")
{
    auto request = Request();
    request.Context.Profile.Backstory = "She said \"hello\" by the {orchard} \\ path.\n";
    request.Context.RecentDialogue = {{"Visitor", "Where is the inn?", "PLAYER"}};
    auto body = SerializeNpcAgentRequest(request);
    REQUIRE_FALSE(body.empty());
    CHECK(body.find("She said \\\"hello\\\" by the {orchard} \\\\ path.\\n") != std::string::npos);
    CHECK(body.find("\"speech_targets\":[8]") != std::string::npos);
    CHECK(body.find("\"recent_dialogue\":[{\"speaker\":\"Visitor\"") != std::string::npos);
    CHECK(body.find("RuntimeGuid") == std::string::npos);
}

TEST_CASE("NPC agent protocol rejects malformed or oversized typed requests", "[AIWorld][NpcAgent]")
{
    auto request = Request();
    SECTION("duplicate action token") { request.Context.Actions.push_back(request.Context.Actions.front()); }
    SECTION("zero nearby token") { request.Context.Nearby.front().Token = 0; }
    SECTION("unbound speech recipient") { request.Context.SpeechTargets.push_back(99); }
    SECTION("invalid need") { request.Context.Needs.Hunger = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("invalid position") { request.Context.Self.Z = std::numeric_limits<float>::infinity(); }
    SECTION("too much lore") { request.Context.Profile.Backstory.assign(8193, 'x'); }
    SECTION("encoded request body limit")
    {
        request.Context.Profile.Backstory.assign(8192, '"');
        request.Context.Profile.Personality.assign(2048, '"');
        request.Context.Actions.clear();
        for (uint32 i = 1; i <= 32; ++i)
            request.Context.Actions.push_back({i, "WAIT", std::string(256, '"')});
    }
    SECTION("too many dialogue entries") { request.Context.RecentDialogue.resize(13); }
    SECTION("unknown emote") { request.Context.Emotes.push_back("DANCE"); }
    SECTION("unknown action") { request.Context.Actions.front().Type = "TELEPORT"; }
    SECTION("invalid UTF-8") { request.Context.Profile.Name = std::string("\xC0\xAF", 2); }
    CHECK(SerializeNpcAgentRequest(request).empty());
}

TEST_CASE("NPC agent protocol offers FLEE only as an engine-owned action token", "[AIWorld][NpcAgent]")
{
    auto request = Request();
    request.Context.Actions = {{9, "FLEE", "Flee the current threat"}};
    auto body = SerializeNpcAgentRequest(request);
    REQUIRE_FALSE(body.empty());
    CHECK(body.find("\"type\":\"FLEE\"") != std::string::npos);
    NpcAgentResponse response;
    REQUIRE(ParseNpcAgentResponse(Body("", "9", "0", "NONE"), response));
    CHECK(MatchesNpcAgentResponse(request, response));
    request.Context.Actions.clear();
    CHECK_FALSE(MatchesNpcAgentResponse(request, response));
}

TEST_CASE("NPC agent response decodes escaped Unicode and matches only offered choices", "[AIWorld][NpcAgent]")
{
    NpcAgentResponse response;
    REQUIRE(ParseNpcAgentResponse(Body("Poj\\u010f sem. \\ud83d\\udc4b \\\"{hello}\\\""), response));
    CHECK(response.Decision.Speech == "Poj\xC4\x8F sem. \xF0\x9F\x91\x8B \"{hello}\"");
    CHECK(MatchesNpcAgentResponse(Request(), response));
    response.Decision.ActionToken = 99;
    CHECK_FALSE(MatchesNpcAgentResponse(Request(), response));
    response.Decision.ActionToken = 0; // explicit no-action answer
    CHECK(MatchesNpcAgentResponse(Request(), response));
    response.Decision.SpeechTarget = 99;
    CHECK_FALSE(MatchesNpcAgentResponse(Request(), response));
    response.Decision.SpeechTarget = 8;
    response.Decision.Emote = "LAUGH"; // known, but not offered
    CHECK_FALSE(MatchesNpcAgentResponse(Request(), response));
}

TEST_CASE("NPC agent response rejects ambiguous schemas without partially replacing output", "[AIWorld][NpcAgent]")
{
    auto json = Body();
    SECTION("duplicate envelope field") { json.insert(1, "\"agent_id\":42,"); }
    SECTION("escaped duplicate field") { json.insert(1, "\"agent\\u005fid\":42,"); }
    SECTION("extra envelope field") { json.insert(1, "\"debug\":1,"); }
    SECTION("missing required field") { json.erase(json.find("\"snapshot_sequence\":7,"), 22); }
    SECTION("extra decision field") { json.insert(json.find("\"action_token\""), "\"command\":\"move\","); }
    SECTION("wrong field type") { json = Body("Hello", "\"3\""); }
    SECTION("negative token") { json = Body("Hello", "-1"); }
    SECTION("fractional token") { json = Body("Hello", "3.0"); }
    SECTION("overflow token") { json = Body("Hello", "4294967296"); }
    SECTION("overflow identity") { json.replace(json.find("91"), 2, "18446744073709551616"); }
    SECTION("leading zero") { json = Body("Hello", "03"); }
    SECTION("unknown emote") { json = Body("Hello", "3", "8", "DANCE"); }
    SECTION("trailing garbage") { json += "{}"; }
    SECTION("trailing comma") { json.insert(json.size() - 2, ","); }
    SECTION("bad JSON whitespace") { json.insert(1, 1, '\v'); }
    SECTION("body too large") { json.append(NpcAgentResponseBodyLimit, ' '); }
    NpcAgentResponse response;
    response.RequestId = 555;
    response.Decision.Speech = "preserved";
    CHECK_FALSE(ParseNpcAgentResponse(json, response));
    CHECK(response.RequestId == 555);
    CHECK(response.Decision.Speech == "preserved");
}

TEST_CASE("NPC agent speech validates Unicode limits and rejects WoW markup and controls", "[AIWorld][NpcAgent]")
{
    std::string speech;
    SECTION("pipe markup") { speech = "Hello |cffffffffvisitor"; }
    SECTION("escaped pipe") { speech = "Hello \\u007cHitem:1"; }
    SECTION("newline") { speech = "Hello\\nvisitor"; }
    SECTION("null") { speech = "Hello\\u0000visitor"; }
    SECTION("C1 control") { speech = "Hello\\u0085visitor"; }
    SECTION("lone high surrogate") { speech = "\\ud83d"; }
    SECTION("lone low surrogate") { speech = "\\udc4b"; }
    SECTION("overlong UTF-8") { speech = std::string("\xC0\xAF", 2); }
    SECTION("raw surrogate UTF-8") { speech = std::string("\xED\xA0\x80", 3); }
    SECTION("too many characters") { speech.assign(257, 'a'); }
    SECTION("too many bytes") { for (unsigned i = 0; i < 193; ++i) speech += "\xF0\x9F\x91\x8B"; }
    NpcAgentResponse response;
    CHECK_FALSE(ParseNpcAgentResponse(Body(speech), response));
}

TEST_CASE("NPC agent response metadata is request local and zero target is nearby broadcast", "[AIWorld][NpcAgent]")
{
    NpcAgentResponse response;
    REQUIRE(ParseNpcAgentResponse(Body("Hello", "0", "0", "NONE"), response));
    auto request = Request();
    request.Context.SpeechTargets.clear();
    CHECK(MatchesNpcAgentResponse(request, response));
    SECTION("request id") { ++response.RequestId; }
    SECTION("agent") { ++response.Agent.Value; }
    SECTION("snapshot") { ++response.SnapshotSequence; }
    SECTION("version") { ++response.ProtocolVersion; }
    CHECK_FALSE(MatchesNpcAgentResponse(request, response));
}

TEST_CASE("NPC speech grants serialize an exact pending player turn independently of old chatter", "[AIWorld][NpcAgent]")
{
    auto request = Request();
    request.Context.RecentDialogue = {{"Other", "Old NPC chatter.", "NPC"}};
    request.Context.Speech = NpcAgentSpeechContext{"PLAYER_REPLY",
        NpcAgentSpeechTurn{std::numeric_limits<uint64>::max(), "Visitor", 8, "Where is \"the orchard\"?"}};
    auto json = SerializeNpcAgentRequest(request);
    REQUIRE_FALSE(json.empty());
    CHECK(json.find("\"speech\":{\"mode\":\"PLAYER_REPLY\",\"turn\":{\"id\":18446744073709551615") != std::string::npos);
    CHECK(json.find("\"recipient_token\":8,\"text\":\"Where is \\\"the orchard\\\"?\"") != std::string::npos);
    CHECK(json.find("DialoguePartnerGuid") == std::string::npos);
    CHECK(json.find("DialogueTurnId") == std::string::npos);
    request.Context.Speech.reset();
    CHECK(SerializeNpcAgentRequest(request).find("\"mode\"") == std::string::npos);
}

TEST_CASE("NPC speech grants bind reply kind name and offered token", "[AIWorld][NpcAgent]")
{
    auto request = Request();
    request.Context.Speech = NpcAgentSpeechContext{"PLAYER_REPLY", NpcAgentSpeechTurn{10, "Visitor", 8, "Hello?"}};
    REQUIRE_FALSE(SerializeNpcAgentRequest(request).empty());
    SECTION("no turn") { request.Context.Speech->Turn.reset(); }
    SECTION("zero id") { request.Context.Speech->Turn->Id = 0; }
    SECTION("unknown token") { request.Context.Speech->Turn->RecipientToken = 99; }
    SECTION("not offered") { request.Context.SpeechTargets.clear(); }
    SECTION("wrong name") { request.Context.Speech->Turn->Speaker = "Another player"; }
    SECTION("wrong kind") { request.Context.Nearby.front().Kind = "NPC"; }
    SECTION("empty speaker") { request.Context.Speech->Turn->Speaker.clear(); }
    SECTION("long speaker") { request.Context.Speech->Turn->Speaker.assign(33, 'n'); }
    SECTION("empty text") { request.Context.Speech->Turn->Text.clear(); }
    SECTION("unsafe text") { request.Context.Speech->Turn->Text = "hello\nforged"; }
    SECTION("oversized text") { request.Context.Speech->Turn->Text.assign(257, 't'); }
    SECTION("unknown mode") { request.Context.Speech->Mode = "ARBITRARY_CHAT"; }
    SECTION("unsolicited turn") { request.Context.Speech->Mode = "INITIATE"; }
    SECTION("silent turn") { request.Context.Speech->Mode = "SILENT"; }
    CHECK(SerializeNpcAgentRequest(request).empty());
}

TEST_CASE("NPC turnless speech modes and NPC reply retain structural movement validation", "[AIWorld][NpcAgent]")
{
    auto request = Request();
    for (std::string mode : {"SILENT", "INITIATE"})
    {
        request.Context.Speech = NpcAgentSpeechContext{mode, std::nullopt};
        auto json = SerializeNpcAgentRequest(request);
        REQUIRE_FALSE(json.empty());
        CHECK(json.find("\"turn\":null") != std::string::npos);
    }
    request.Context.Nearby.front().Kind = "NPC";
    request.Context.Speech = NpcAgentSpeechContext{"NPC_REPLY", NpcAgentSpeechTurn{11, "Visitor", 8, "A question."}};
    REQUIRE_FALSE(SerializeNpcAgentRequest(request).empty());
    request.Context.Speech = NpcAgentSpeechContext{"SILENT", std::nullopt};
    NpcAgentResponse response;
    REQUIRE(ParseNpcAgentResponse(Body("Unsolicited text", "3", "0", "NONE"), response));
    // Speech permission is applied independently by the controller: a denied
    // chat proposal must not discard the otherwise valid movement choice.
    CHECK(MatchesNpcAgentResponse(request, response));
    response.Decision.ActionToken = 99;
    CHECK_FALSE(MatchesNpcAgentResponse(request, response));
}
