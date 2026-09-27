/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Inference/RecoveryAdvice.h"
#include "Agent/LivingAdviceState.h"
#include "Agent/LivingAdviceBudget.h"
#include "Agent/LivingMovementWatchdog.h"
#include "Agent/LivingForagePolicy.h"
#include <limits>

TEST_CASE("All-NPC advice admission bounds work and gives waiting agents a turn", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(1, 1000));
    REQUIRE_FALSE(budget.Acquire(2, 1000));
    REQUIRE_FALSE(budget.Acquire(3, 1000));
    REQUIRE_FALSE(budget.Acquire(1, 3000)); // hot caller cannot jump the queue
    REQUIRE(budget.Acquire(2, 3000));
    REQUIRE_FALSE(budget.Acquire(3, 3000));
    REQUIRE(budget.Acquire(3, 5000));
    REQUIRE_FALSE(budget.Acquire(4, 5000));
    REQUIRE(budget.Acquire(4, 40001)); // absent head expires
}

TEST_CASE("A crowd of eligible NPCs shares the global advice budget", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    std::vector<uint64> admitted;
    for (uint64 now = 1000; now <= 255000; now += 1000)
        for (uint64 id = 1; id <= 128; ++id)
            if (std::find(admitted.begin(), admitted.end(), id) == admitted.end() && budget.Acquire(id, now))
                admitted.push_back(id);
    REQUIRE(admitted.size() == 128);
    for (size_t i = 0; i < admitted.size(); ++i) REQUIRE(admitted[i] == i + 1);
}

TEST_CASE("Role movement can follow long detours but stalls and loops terminate", "[AIWorld][RecoveryAdvice]")
{
    LivingMovementWatchdog move;
    move.Begin(1000, {0,0,0,0});
    for (uint64 t = 5000; t <= 120000; t += 5000)
        REQUIRE(move.Continue(1000+t, {0,float(t)/1000,0,0}, false));
    REQUIRE_FALSE(move.Continue(136000, {0,120,0,0}, false));
    REQUIRE(std::string(move.End) == "NO_PROGRESS");
    move.Begin(1000, {0,0,0,0});
    for (uint64 t = 5000; t < 180000; t += 5000)
        REQUIRE(move.Continue(1000+t, {0,float(t % 10000),0,0}, false));
    REQUIRE_FALSE(move.Continue(181000, {0,10,0,0}, false));
    REQUIRE(std::string(move.End) == "DURATION_LIMIT");
    move.Begin(1000, {0,0,0,0});
    REQUIRE_FALSE(move.Continue(9000, {0,10,0,0}, true));
    REQUIRE(std::string(move.End) == "ESCAPE_LIMIT");
}

TEST_CASE("Food memory distinguishes a meal from a search and forgets stale evidence", "[AIWorld][RecoveryAdvice]")
{
    LivingFoodMemory memory;
    ActionPosition home{0,0,0,0}, food{0,40,0,0};
    memory.Searched(food, 1000);
    REQUIRE_FALSE(memory.FoodHint(home, 80, 1000).has_value());
    REQUIRE(memory.Visits(food, 1000) == 1);
    memory.EmptyRound(); memory.EmptyRound(); memory.EmptyRound();
    REQUIRE(LivingForagePolicy::SearchRadius(memory.EmptyRounds) == 128);
    for (uint32 leg = 0; leg < 120; ++leg)
    {
        auto waypoint = LivingForagePolicy::Waypoint(home, 80992, leg, memory.EmptyRounds);
        REQUIRE(LivingReturnPolicy::Distance(home, waypoint) <= 128);
    }
    REQUIRE(memory.AdviceFailed() == 240000);
    REQUIRE(memory.AdviceFailed() == 480000);
    REQUIRE(memory.AdviceFailed() == 900000);
    REQUIRE(memory.AdviceFailed() == 900000);
    memory.Fed(food, 2000);
    REQUIRE(memory.FailedAdvice == 0);
    REQUIRE(memory.EmptyRounds == 0);
    REQUIRE(memory.FoodHint(home, 80, 2000).has_value());
    REQUIRE_FALSE(memory.FoodHint({1,0,0,0}, 80, 2000).has_value());
    REQUIRE_FALSE(memory.FoodHint(home, 30, 2000).has_value());
    memory.Searched(food, 3000);
    REQUIRE_FALSE(memory.FoodHint(home, 80, 4000).has_value());
    REQUIRE(memory.FoodHint(home, 80, 603000).has_value());
    REQUIRE_FALSE(memory.FoodHint(home, 80, 1802000).has_value());
    for (unsigned i = 0; i < 100; ++i)
    { memory.Searched({0,float(i)*10,0,0}, 2000000+i); memory.Fed({0,float(i)*10,100,0}, 2000000+i); }
    REQUIRE(memory.Searches.size() == 32);
    REQUIRE(memory.Meals.size() == 8);
}

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
