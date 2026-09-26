/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Inference/RecoveryAdvice.h"
#include "Agent/LivingAdviceState.h"
#include <limits>

TEST_CASE("Recovery response cannot invent geometry or alter the response schema", "[AIWorld][RecoveryAdvice]")
{
    RecoveryAdviceResponse response;
    REQUIRE(ParseRecoveryAdvice(R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":2})", response));
    REQUIRE(response.Token == 2);
    REQUIRE(response.Agent.Value == 80447);
    for (auto json : {
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":2,"x":1})",
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":true})",
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":2.0})",
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":2,"choice":0})",
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":9})",
        R"({"protocol_version":1,"request_id":18446744073709551616,"agent_id":80447,"episode":123,"choice":2})",
        R"({"protocol_version":2,"request_id":9,"agent_id":80447,"episode":123,"choice":2})"})
        REQUIRE_FALSE(ParseRecoveryAdvice(json, response));
    REQUIRE(ParseRecoveryAdvice(R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":0})", response));
    REQUIRE(response.Token == 0);
}

TEST_CASE("Recovery advice is stale after displacement home changes or deadline", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceState state;
    state.PendingId = 9; state.RequestedAt = 1000;
    state.Origin = {0, 10, 10, 10}; state.Home = {0, 0, 0, 0};
    REQUIRE(state.Fresh(31000, state.Origin, state.Home));
    REQUIRE_FALSE(state.Fresh(31001, state.Origin, state.Home));
    REQUIRE_FALSE(state.Fresh(999, state.Origin, state.Home));
    REQUIRE_FALSE(state.Fresh(2000, {0, 13, 10, 10}, state.Home));
    REQUIRE_FALSE(state.Fresh(2000, {1, 10, 10, 10}, state.Home));
    REQUIRE_FALSE(state.Fresh(2000, state.Origin, {0, 1, 0, 0}));
    state.ClearPending();
    REQUIRE_FALSE(state.Fresh(2000, state.Origin, state.Home));
}

TEST_CASE("Recovery choice belongs to the exact request agent episode and offered tokens", "[AIWorld][RecoveryAdvice]")
{
    RecoveryAdviceRequest request;
    request.RequestId = 9; request.Agent = AgentId{80447}; request.Episode = 123;
    request.Options = {{2, "TRAIL"}};
    RecoveryAdviceResponse response{9, 123, AgentId{80447}, 2};
    REQUIRE(MatchesRecoveryAdvice(request, response));
    auto other = response; other.RequestId++;
    REQUIRE_FALSE(MatchesRecoveryAdvice(request, other));
    other = response; other.Agent.Value++;
    REQUIRE_FALSE(MatchesRecoveryAdvice(request, other));
    other = response; other.Episode++;
    REQUIRE_FALSE(MatchesRecoveryAdvice(request, other));
    other = response; other.Token = 1;
    REQUIRE_FALSE(MatchesRecoveryAdvice(request, other));
    other.Token = 0;
    REQUIRE(MatchesRecoveryAdvice(request, other));
}

TEST_CASE("Successful recovery memory is bounded and consumes only the active step", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceState state;
    state.RememberSuccess();
    REQUIRE(state.Successful.empty());
    for (unsigned i = 0; i < 10; ++i)
    {
        state.Active = LivingAdviceCandidate{};
        state.ActiveOrigin = {0, float(i), 0, 0};
        state.RememberSuccess();
        REQUIRE_FALSE(state.Active.has_value());
    }
    REQUIRE(state.Successful.size() == 8);
    REQUIRE(state.Successful.front().From.X == 2);
    REQUIRE(state.Successful.back().From.X == 9);
}

TEST_CASE("Return corridor preserves corners and failed execution edges", "[AIWorld][RecoveryAdvice]")
{
    using namespace LivingReturnPolicy;
    auto plan = Corridor({{0,0,0,0}, {0,0,28,0}, {0,28,28,0}, {0,28,0,0}});
    REQUIRE(plan.size() == 6);
    REQUIRE(plan[1].Y == 28);
    REQUIRE(plan[1].X == 0);
    REQUIRE(plan.back().Y == 0);
    RouteMemory memory;
    memory.Planned = plan;
    memory.Advance({0,0,14,0});
    REQUIRE(memory.Planned.front().Y == 28);
    memory.Reject({0,0,14,0}, {0,0,28,0});
    REQUIRE(memory.Planned.empty());
    REQUIRE(memory.Failed({0,0,15,0}, {0,0,28,0}));
    REQUIRE_FALSE(memory.Failed({0,0,28,0}, {0,0,14,0}));
    REQUIRE_FALSE(memory.Failed({0,5,14,0}, {0,0,28,0}));
    REQUIRE(Corridor({{0,0,0,0}, {0,1000,0,0}}).empty());
    REQUIRE(Corridor({{0,0,0,0}, {1,5,0,0}}).empty());
    REQUIRE(HomeLimit(30) == 96);
    REQUIRE(HomeLimit(80) == 144);
}

TEST_CASE("Return rejection distinguishes a wall from unsupported terrain", "[AIWorld][RecoveryAdvice]")
{
    char const* reason = nullptr;
    auto clear = [](auto const&, auto const&) { return true; };
    auto flat = [](auto const&) -> std::optional<float> { return 0.0f; };
    using namespace LivingReturnPolicy;
    REQUIRE(SurfaceConnector({0,0,0,0}, {0,4,0,0}, flat, clear, &reason).size() == 9);
    REQUIRE(std::string(reason) == "NONE");
    REQUIRE(SurfaceConnector({0,0,0,0}, {0,4,0,0}, flat, [](auto const&, auto const&) { return false; }, &reason).empty());
    REQUIRE(std::string(reason) == "CONNECTOR_OBSTACLE");
    REQUIRE(SurfaceConnector({0,0,0,2}, {0,4,0,0}, flat, clear, &reason).empty());
    REQUIRE(std::string(reason) == "CONNECTOR_START_HEIGHT");
}

TEST_CASE("Normalized return keeps the exact query endpoint in authorization", "[AIWorld][RecoveryAdvice]")
{
    RecoveryMovement request{{0,1.1f,0,0}, {0,20,0,0}, 96, {}, false};
    request.QueryDestination = {0,2,0,0};
    auto changed = request;
    REQUIRE(request == changed);
    changed.QueryDestination->X += 1;
    REQUIRE_FALSE(request == changed);
    REQUIRE(LivingReturnPolicy::UsefulStep({0,0,0,0}, request.Destination));
    request.Destination = {0,0.2f,0,0};
    REQUIRE_FALSE(LivingReturnPolicy::UsefulStep({0,0,0,0}, request.Destination));
}
