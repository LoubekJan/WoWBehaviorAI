/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Scheduler/AgentUpdateScheduler.h"
#include "Scheduler/PlanningWorkBudget.h"
#include <array>
#include <unordered_map>

namespace
{
    PlanningWorkBudget::TimePoint At(std::uint64_t microseconds)
    { return PlanningWorkBudget::TimePoint(std::chrono::microseconds(microseconds)); }
}

TEST_CASE("Planning budget stops at the deadline after an indivisible expensive query", "[AIWorld][PlanningWorkBudget]")
{
    PlanningWorkBudget budget(std::chrono::microseconds(2000), 16, At(1000));
    PlanningWorkBudget::Scope scope(budget);
    REQUIRE(PlanningWorkBudget::Available(At(1000)));
    auto work = PlanningWorkBudget::TryAcquire(At(1000));
    REQUIRE(bool(work));
    // A synchronous engine query cannot be interrupted, but its overrun must
    // prevent starting the next query in the same world frame.
    work.Finish(At(6000));
    REQUIRE_FALSE(PlanningWorkBudget::Available(At(6000)));
    REQUIRE_FALSE(bool(PlanningWorkBudget::TryAcquire(At(6000))));
    REQUIRE(budget.GetStatistics().Started == 1);
    REQUIRE(budget.GetStatistics().Deferred == 1);
    REQUIRE(budget.GetStatistics().OperationUs == 5000);
    REQUIRE(budget.GetStatistics().MaxOperationUs == 5000);
}

TEST_CASE("Planning budget count limits and availability checks do not spend candidates", "[AIWorld][PlanningWorkBudget]")
{
    PlanningWorkBudget budget(std::chrono::microseconds(2000), 3, At(0));
    PlanningWorkBudget::Scope scope(budget);
    for (unsigned i = 0; i < 20; ++i) REQUIRE(PlanningWorkBudget::Available(At(0)));
    for (unsigned i = 0; i < 3; ++i)
    {
        auto work = PlanningWorkBudget::TryAcquire(At(0));
        REQUIRE(bool(work));
        work.Finish(At(0));
    }
    REQUIRE_FALSE(PlanningWorkBudget::Available(At(0)));
    REQUIRE(budget.GetStatistics().Deferred == 0);
    PlanningWorkBudget::MarkDeferred();
    REQUIRE_FALSE(bool(PlanningWorkBudget::TryAcquire(At(0))));
    REQUIRE(budget.GetStatistics().Started == 3);
    REQUIRE(budget.GetStatistics().Deferred == 2);
}

TEST_CASE("Planning budget excludes maintenance time and rejects exact query-work expiry", "[AIWorld][PlanningWorkBudget]")
{
    PlanningWorkBudget budget(std::chrono::microseconds(2000), 16, At(1000));
    PlanningWorkBudget::Scope scope(budget);
    REQUIRE(PlanningWorkBudget::Available(At(999)));
    REQUIRE(PlanningWorkBudget::Available(At(3000000)));
    auto first = PlanningWorkBudget::TryAcquire(At(3000000));
    first.Finish(At(3001500));
    REQUIRE(PlanningWorkBudget::Available(At(9000000)));
    auto second = PlanningWorkBudget::TryAcquire(At(9000000));
    second.Finish(At(9000500));
    REQUIRE_FALSE(PlanningWorkBudget::Available(At(9000500)));
    REQUIRE_FALSE(bool(PlanningWorkBudget::TryAcquire(At(9000500))));
    REQUIRE(budget.GetStatistics().Started == 2);
    REQUIRE(budget.GetStatistics().OperationUs == 2000);
}

TEST_CASE("Planning budget measures each operation once when moved or completed early", "[AIWorld][PlanningWorkBudget]")
{
    PlanningWorkBudget budget(std::chrono::microseconds(2000), 16, At(0));
    PlanningWorkBudget::Scope scope(budget);
    auto first = PlanningWorkBudget::TryAcquire(At(0));
    auto moved = std::move(first);
    REQUIRE_FALSE(bool(first));
    REQUIRE(bool(moved));
    first.Finish(At(900));
    moved.Finish(At(300));
    moved.Finish(At(800));
    auto next = PlanningWorkBudget::TryAcquire(At(300));
    next.Finish(At(500));
    moved = std::move(next);
    moved.Finish(At(900));
    REQUIRE(budget.GetStatistics().Started == 2);
    REQUIRE(budget.GetStatistics().OperationUs == 500);
    REQUIRE(budget.GetStatistics().MaxOperationUs == 300);
}

TEST_CASE("Planning budget scopes restore the outer budget and leave probes outside needs unrestricted", "[AIWorld][PlanningWorkBudget]")
{
    REQUIRE(PlanningWorkBudget::Available(At(0)));
    REQUIRE(PlanningWorkBudget::StartedOperations() == 0);
    REQUIRE(bool(PlanningWorkBudget::TryAcquire(At(0))));
    PlanningWorkBudget outer(std::chrono::microseconds(2000), 1, At(0));
    PlanningWorkBudget inner(std::chrono::microseconds(2000), 1, At(0));
    {
        PlanningWorkBudget::Scope outerScope(outer);
        auto work = PlanningWorkBudget::TryAcquire(At(0));
        REQUIRE(bool(work));
        REQUIRE(PlanningWorkBudget::StartedOperations() == 1);
        work.Finish(At(100));
        REQUIRE_FALSE(PlanningWorkBudget::Available(At(100)));
        {
            PlanningWorkBudget::Scope innerScope(inner);
            auto nested = PlanningWorkBudget::TryAcquire(At(100));
            REQUIRE(bool(nested));
            nested.Finish(At(200));
        }
        REQUIRE_FALSE(PlanningWorkBudget::Available(At(200)));
    }
    REQUIRE(PlanningWorkBudget::Available(At(10000)));
    REQUIRE(PlanningWorkBudget::StartedOperations() == 0);
    REQUIRE(bool(PlanningWorkBudget::TryAcquire(At(10000))));
    REQUIRE(outer.GetStatistics().Started == 1);
    REQUIRE(inner.GetStatistics().Started == 1);
}

TEST_CASE("Planning resume reaches later candidates while overdue needs keep their elapsed time", "[AIWorld][PlanningWorkBudget]")
{
    AgentUpdateScheduler scheduler;
    std::vector<AgentId> agents;
    for (uint64 id = 1; id <= 32; ++id) agents.push_back(AgentId{id});
    scheduler.Sync(agents, 0, 1000, 0x4e454544);
    struct Search { unsigned Cursor = 0; std::vector<unsigned> Attempted; };
    std::unordered_map<uint64, Search> searches;
    std::unordered_map<uint64, uint64> elapsed, last;
    unsigned deferred = 0;

    // A five-second server stall leaves all agents overdue. Most candidates
    // are failures and only the ninth succeeds; replaying candidate zero on
    // every frame would never find it under this budget.
    for (uint64 now = 5000; now <= 20000; now += 10)
    {
        std::uint64_t usedUs = 0;
        PlanningWorkBudget budget(std::chrono::microseconds(2000), 16, At(now * 1000));
        PlanningWorkBudget::Scope scope(budget);
        for (unsigned processed = 0; processed < 128 && usedUs < 4000; ++processed)
        {
            auto due = scheduler.PopDue(now);
            if (!due) break;
            elapsed[due->Agent.Value] += due->ElapsedMs;
            last[due->Agent.Value] = now;
            usedUs += 100; // Needs/threat/movement maintenance still runs.
            auto& search = searches[due->Agent.Value];
            while (search.Cursor < 9)
            {
                auto work = PlanningWorkBudget::TryAcquire(At(now * 1000 + usedUs));
                if (!work) { ++deferred; break; }
                search.Attempted.push_back(search.Cursor++);
                usedUs += 600;
                work.Finish(At(now * 1000 + usedUs));
            }
        }
        REQUIRE(budget.GetStatistics().Started <= 4);
        // Needs time is independent of the query budget. The 4 ms needs
        // deadline can be overrun only by one atomic actor update here.
        REQUIRE(usedUs < 6500);
    }
    REQUIRE(deferred > 0);
    REQUIRE(searches.size() == agents.size());
    for (AgentId id : agents)
    {
        REQUIRE(searches[id.Value].Cursor == 9);
        REQUIRE(searches[id.Value].Attempted == std::vector<unsigned>{0, 1, 2, 3, 4, 5, 6, 7, 8});
        REQUIRE(elapsed[id.Value] == last[id.Value]);
        REQUIRE(last[id.Value] > 19000);
    }
}
