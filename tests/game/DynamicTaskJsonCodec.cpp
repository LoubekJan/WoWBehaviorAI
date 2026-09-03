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

#include "DynamicTaskJsonCodec.h"
#include "QuestContractLimits.h"

#include <string>

namespace
{
    // A well-formed /dynamic-task response body, exactly the shape
    // ai-server's own DynamicTaskResponse pydantic model emits.
    std::string ValidResponseJson()
    {
        return "{\"protocol_version\":1,\"request_id\":123,\"agent_id\":42,\"snapshot_sequence\":77,"
               "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":3,"
               "\"max_range_yards\":50.5,\"expiry_ms\":300000,\"reward_money_copper\":100,"
               "\"title\":\"Cull the wolves\",\"description\":\"Thin the wolf pack near the road.\"}}";
    }

    // Poisons `response` with sentinel values so a test can assert a
    // failed ParseDynamicTaskResponse() call left it completely
    // untouched, never a partial fill.
    DynamicTaskResponse PoisonedResponse()
    {
        DynamicTaskResponse response;
        response.RequestId = 999999;
        response.Agent.Value = 999999;
        response.SnapshotSequence = 999999;
        response.Proposal.Title = "UNTOUCHED";
        response.Proposal.Description = "UNTOUCHED";
        return response;
    }
}

TEST_CASE("ParseDynamicTaskResponse valid response", "[DynamicTaskJsonCodec]")
{
    DynamicTaskResponse response;
    REQUIRE(ParseDynamicTaskResponse(ValidResponseJson(), response));

    REQUIRE(response.Version == DynamicTaskProtocolVersion::V1);
    REQUIRE(response.RequestId == 123);
    REQUIRE(response.Agent.Value == 42);
    REQUIRE(response.SnapshotSequence == 77);
    REQUIRE(response.Proposal.Objective == QuestObjectiveType::KillCreature);
    REQUIRE(response.Proposal.TargetToken == 1);
    REQUIRE(response.Proposal.RequiredCount == 3);
    // 50.5 is exactly representable in both double and float (32+16+2+0.5),
    // so this is a safe exact comparison, not one that needs an epsilon.
    REQUIRE(response.Proposal.MaxRangeYards == 50.5f);
    REQUIRE(response.Proposal.ExpiryMs == 300000);
    REQUIRE(response.Proposal.RewardMoneyCopper == 100);
    REQUIRE(response.Proposal.Title == "Cull the wolves");
    REQUIRE(response.Proposal.Description == "Thin the wolf pack near the road.");
}

TEST_CASE("ParseDynamicTaskResponse escaped title/description round-trip", "[DynamicTaskJsonCodec]")
{
    // Quotes, a backslash, and literal braces inside the untrusted text -
    // exactly the shape a naive brace-counting/quote-oblivious parser
    // would mis-parse (see DynamicTaskJsonCodec.h's own rationale for why
    // /decision's FindObjectField()/FindStringField() aren't reused here).
    std::string json =
        "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
        "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
        "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
        "\"title\":\"Kill the \\\"Feral\\\" Wolves\","
        "\"description\":\"Path: C:\\\\wolves\\\\{den} and {more}\"}}";

    DynamicTaskResponse response;
    REQUIRE(ParseDynamicTaskResponse(json, response));
    REQUIRE(response.Proposal.Title == "Kill the \"Feral\" Wolves");
    REQUIRE(response.Proposal.Description == "Path: C:\\wolves\\{den} and {more}");
}

TEST_CASE("ParseDynamicTaskResponse rejects malformed JSON", "[DynamicTaskJsonCodec]")
{
    SECTION("missing root braces and commas")
    {
        // The reviewer's own example: every field individually looks
        // "findable" by a naive key/value search, but this is not legal
        // JSON at all - no root object, no separators.
        std::string json =
            "\"protocol_version\":1\n"
            "\"request_id\":5\n"
            "\"agent_id\":10\n"
            "\"snapshot_sequence\":20\n"
            "\"proposal\":{\n"
            "  \"objective\":\"KILL_CREATURE\"\n"
            "  \"target_token\":1\n"
            "  \"required_count\":1\n"
            "  \"max_range_yards\":30\n"
            "  \"expiry_ms\":1000\n"
            "  \"reward_money_copper\":0\n"
            "  \"title\":\"test\"\n"
            "  \"description\":\"test\"\n"
            "}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
        REQUIRE(response.RequestId == 999999);
        REQUIRE(response.Proposal.Title == "UNTOUCHED");
    }

    SECTION("missing comma between fields")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
        REQUIRE(response.RequestId == 999999);
    }

    SECTION("trailing garbage after an otherwise-valid document")
    {
        std::string json = ValidResponseJson() + "GARBAGE";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
        REQUIRE(response.RequestId == 999999);
    }

    SECTION("root value is not an object")
    {
        std::string json = "[1,2,3]";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("duplicate key within the proposal object")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"objective\":\"KILL_CREATURE\","
            "\"target_token\":1,\"required_count\":1,\"max_range_yards\":10,\"expiry_ms\":1000,"
            "\"reward_money_copper\":0,\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("empty body")
    {
        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(std::string(), response));
    }
}

TEST_CASE("ParseDynamicTaskResponse rejects required fields nested under another root field", "[DynamicTaskJsonCodec]")
{
    // Every required key is "findable" somewhere in the document, but
    // none of them are direct members of the root object - the root's
    // only direct member is "wrapper". A substring-search parser that
    // doesn't track object nesting would accept this; a real
    // direct-member parser must not.
    std::string json =
        "{\"wrapper\":{\"protocol_version\":1,\"request_id\":123,\"agent_id\":42,\"snapshot_sequence\":77,"
        "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":3,"
        "\"max_range_yards\":50,\"expiry_ms\":1000,\"reward_money_copper\":0,"
        "\"title\":\"x\",\"description\":\"x\"}}}";

    DynamicTaskResponse response = PoisonedResponse();
    REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    REQUIRE(response.RequestId == 999999);
}

TEST_CASE("ParseDynamicTaskResponse rejects unknown root and proposal fields", "[DynamicTaskJsonCodec]")
{
    SECTION("unknown root field")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"},"
            "\"unexpected\":\"nope\"}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("unknown proposal field")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\",\"unexpected\":\"nope\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("missing root field")
    {
        std::string json =
            "{\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("missing proposal field")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("root fields in a different order still parse correctly (JSON objects are unordered)")
    {
        std::string json =
            "{\"proposal\":{\"description\":\"d\",\"title\":\"t\",\"reward_money_copper\":0,"
            "\"expiry_ms\":1000,\"max_range_yards\":10,\"required_count\":1,\"target_token\":1,"
            "\"objective\":\"KILL_CREATURE\"},"
            "\"snapshot_sequence\":77,\"agent_id\":42,\"request_id\":123,\"protocol_version\":1}";

        DynamicTaskResponse response;
        REQUIRE(ParseDynamicTaskResponse(json, response));
        REQUIRE(response.RequestId == 123);
        REQUIRE(response.Agent.Value == 42);
        REQUIRE(response.SnapshotSequence == 77);
        REQUIRE(response.Proposal.Title == "t");
    }
}

TEST_CASE("ParseDynamicTaskResponse rejects non-JSON whitespace between tokens", "[DynamicTaskJsonCodec]")
{
    // JSON's insignificant whitespace is exactly space/tab/LF/CR
    // (RFC 8259 section 2) - vertical tab and form feed are not
    // whitespace as far as JSON is concerned, even though std::isspace()
    // in the "C" locale accepts both.
    SECTION("vertical tab between tokens")
    {
        std::string json =
            "{\"protocol_version\":1,\x0B\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("form feed between tokens")
    {
        std::string json =
            "{\"protocol_version\":1,\x0C\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("the legal whitespace set (space/tab/LF/CR) is still accepted")
    {
        std::string json =
            "{\t\"protocol_version\":1,\n\"request_id\":1,\r\n\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"} }";

        DynamicTaskResponse response;
        REQUIRE(ParseDynamicTaskResponse(json, response));
    }
}

TEST_CASE("ParseDynamicTaskResponse rejects an unsupported objective", "[DynamicTaskJsonCodec]")
{
    std::string json =
        "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
        "\"proposal\":{\"objective\":\"SPAWN_NPC\",\"target_token\":1,\"required_count\":1,"
        "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
        "\"title\":\"t\",\"description\":\"d\"}}";

    DynamicTaskResponse response = PoisonedResponse();
    REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
}

TEST_CASE("ParseDynamicTaskResponse rejects the literal in-memory default INVALID objective", "[DynamicTaskJsonCodec]")
{
    // QuestObjectiveType::Invalid is the in-memory default an
    // uninitialized/partially-parsed draft would have (see
    // QuestObjectiveType.h) - the wire contract must never accept the
    // literal string "INVALID" as if it were a real, supported value.
    std::string json =
        "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
        "\"proposal\":{\"objective\":\"INVALID\",\"target_token\":1,\"required_count\":1,"
        "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
        "\"title\":\"t\",\"description\":\"d\"}}";

    DynamicTaskResponse response = PoisonedResponse();
    REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
}

TEST_CASE("ParseDynamicTaskResponse rejects uint32 overflow", "[DynamicTaskJsonCodec]")
{
    std::string json =
        "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
        "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":4294967296,\"required_count\":1,"
        "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
        "\"title\":\"t\",\"description\":\"d\"}}";

    DynamicTaskResponse response = PoisonedResponse();
    REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
}

TEST_CASE("ParseDynamicTaskResponse rejects a float that overflows to infinity when narrowed", "[DynamicTaskJsonCodec]")
{
    // 1e100 is a perfectly finite double, but static_cast<float>(1e100)
    // overflows float's finite range - the exact narrowing bug this test
    // guards against.
    std::string json =
        "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
        "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
        "\"max_range_yards\":1e100,\"expiry_ms\":1000,\"reward_money_copper\":0,"
        "\"title\":\"t\",\"description\":\"d\"}}";

    DynamicTaskResponse response = PoisonedResponse();
    REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
}

TEST_CASE("ParseDynamicTaskResponse rejects NaN/Infinity literals", "[DynamicTaskJsonCodec]")
{
    SECTION("NaN")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":NaN,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("Infinity")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":Infinity,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }
}

TEST_CASE("ParseDynamicTaskResponse enforces title/description length caps", "[DynamicTaskJsonCodec]")
{
    SECTION("title exactly at the cap is accepted")
    {
        std::string title(QuestContractMaxTitleLength, 'x');
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"" + title + "\",\"description\":\"d\"}}";

        DynamicTaskResponse response;
        REQUIRE(ParseDynamicTaskResponse(json, response));
        REQUIRE(response.Proposal.Title.size() == QuestContractMaxTitleLength);
    }

    SECTION("title one over the cap is rejected")
    {
        std::string title(QuestContractMaxTitleLength + 1, 'x');
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"" + title + "\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("description one over the cap is rejected")
    {
        std::string description(QuestContractMaxDescriptionLength + 1, 'x');
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"" + description + "\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }
}

TEST_CASE("ParseDynamicTaskResponse rejects degenerate proposal values", "[DynamicTaskJsonCodec]")
{
    SECTION("required_count == 0")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":0,"
            "\"max_range_yards\":10,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("max_range_yards == 0")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":0,\"expiry_ms\":1000,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }

    SECTION("expiry_ms == 0")
    {
        std::string json =
            "{\"protocol_version\":1,\"request_id\":1,\"agent_id\":1,\"snapshot_sequence\":1,"
            "\"proposal\":{\"objective\":\"KILL_CREATURE\",\"target_token\":1,\"required_count\":1,"
            "\"max_range_yards\":10,\"expiry_ms\":0,\"reward_money_copper\":0,"
            "\"title\":\"t\",\"description\":\"d\"}}";

        DynamicTaskResponse response = PoisonedResponse();
        REQUIRE_FALSE(ParseDynamicTaskResponse(json, response));
    }
}

TEST_CASE("SerializeDynamicTaskRequest escapes DisplayName", "[DynamicTaskJsonCodec]")
{
    DynamicTaskRequest request;
    request.Version = DynamicTaskProtocolVersion::V1;
    request.RequestId = 7;
    request.Context.Agent.Value = 42;
    request.Context.SnapshotSequence = 5;
    request.Context.Problem.Type = WorldEventType::CreatureKilled;

    QuestTargetCandidate candidate;
    candidate.Token = 1;
    candidate.Entry = 2002;
    candidate.DisplayName = "Kill the \"Feral\" Wolves\\Pack";
    request.Context.CandidateTargets.push_back(candidate);

    std::string json = SerializeDynamicTaskRequest(request);

    // The raw byte sequence \" must appear (escaped quote), never a bare
    // unescaped '"' in the middle of the string value, and the literal
    // backslash must itself be escaped too.
    REQUIRE(json.find("\"display_name\":\"Kill the \\\"Feral\\\" Wolves\\\\Pack\"") != std::string::npos);

    // And it must round-trip back through the parser-side primitives
    // correctly - reusing ParseDynamicTaskResponse's own machinery isn't
    // possible here (this is a request, not a response), so this just
    // confirms the emitted document is itself valid, escaped JSON by
    // checking the exact expected escaped substring above is present and
    // no bare unescaped quote splits it.
    REQUIRE(json.find("Kill the \"Feral\"") == std::string::npos);
}
