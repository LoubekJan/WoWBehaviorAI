/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Scheduler/AgentUpdateScheduler.h"
#include "Scheduler/PlanningWorkBudget.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <set>

namespace
{
    PlanningWorkBudget::TimePoint QueryTime(uint64 microseconds)
    { return PlanningWorkBudget::TimePoint(std::chrono::microseconds(microseconds)); }
}

TEST_CASE("Planning budget oldest ready request wins and refusal preserves its age", "[AIWorld][PlanningWorkScheduler]")
{
    PlanningWorkScheduler planner;
    planner.Request(AgentId{1}, 10);
    planner.Request(AgentId{2}, 20);
    planner.Request(AgentId{3}, 30);
    auto first = planner.Priority(AgentId{1});
    planner.Request(AgentId{1}, 99);
    REQUIRE(planner.Priority(AgentId{1}) == first);
    REQUIRE(planner.WaitMs(AgentId{1}, 100) == 90);
    // The oldest request is absent from this needs cohort and cannot block it.
    planner.BeginFrame(100, {AgentId{3}, AgentId{2}});
    REQUIRE_FALSE(planner.CanAdmit(AgentId{3}));
    REQUIRE(planner.TryAdmit(AgentId{2}));
    REQUIRE(planner.TryAdmit(AgentId{2}));
    REQUIRE_FALSE(planner.CanAdmit(AgentId{3}));
    planner.EndActor(AgentId{2}, true);
    REQUIRE(planner.CanAdmit(AgentId{3}));
    planner.EndActor(AgentId{3}, false);
    REQUIRE(planner.WaitMs(AgentId{3}, 100) == 70);
    REQUIRE(planner.WaitMs(AgentId{2}, 100) == 0);
    REQUIRE(planner.Priority(AgentId{3}) < planner.Priority(AgentId{2}));
    planner.BeginFrame(110, {AgentId{2}, AgentId{3}, AgentId{1}});
    REQUIRE(planner.CanAdmit(AgentId{1}));
    REQUIRE_FALSE(planner.CanAdmit(AgentId{3}));
}

TEST_CASE("Planning budget cancellation membership and stale waits cannot block another actor", "[AIWorld][PlanningWorkScheduler]")
{
    PlanningWorkScheduler planner;
    for (uint64 id = 1; id <= 4; ++id) planner.Request(AgentId{id}, id);
    planner.BeginFrame(100, {AgentId{1}, AgentId{2}, AgentId{3}, AgentId{4}});
    REQUIRE(planner.TryAdmit(AgentId{1}));
    planner.Cancel(AgentId{1});
    REQUIRE_FALSE(planner.HasRequest(AgentId{1}));
    REQUIRE(planner.CanAdmit(AgentId{2}));
    planner.SyncMembership({AgentId{3}, AgentId{4}, AgentId{4}, AgentId{}});
    REQUIRE(planner.PendingCount() == 2);
    REQUIRE(planner.CanAdmit(AgentId{3}));
    planner.Request(AgentId{4}, 29999);
    planner.ExpireUnseen(30003);
    REQUIRE_FALSE(planner.HasRequest(AgentId{3}));
    REQUIRE(planner.CanAdmit(AgentId{4}));
    planner.Request(AgentId{4}, 5); // Clock rollback starts a fresh wait.
    REQUIRE(planner.WaitMs(AgentId{4}, 5) == 0);
    planner.ExpireUnseen(4);
    REQUIRE(planner.PendingCount() == 0);
    REQUIRE_FALSE(planner.CanAdmit(AgentId{4}));
    REQUIRE(planner.Priority(AgentId{}) == std::numeric_limits<uint64>::max());
}

TEST_CASE("Planning resume temporary care removes readiness without forgetting a waiting search", "[AIWorld][PlanningWorkScheduler]")
{
    PlanningWorkScheduler planner;
    planner.Request(AgentId{1}, 0);
    planner.Request(AgentId{2}, 1);
    auto oldPriority = planner.Priority(AgentId{1});
    for (uint64 now = 1000; now <= 20000; now += 1000)
    {
        planner.BeginFrame(now, {AgentId{2}});
        REQUIRE(planner.TryAdmit(AgentId{2}));
        planner.EndActor(AgentId{2}, true);
        // Refresh only the still-live paused demand, never Cancel it.
        planner.Request(AgentId{1}, now);
    }
    REQUIRE(planner.Priority(AgentId{1}) == oldPriority);
    REQUIRE(planner.WaitMs(AgentId{1}, 20000) == 20000);
    planner.BeginFrame(21000, {AgentId{1}, AgentId{2}});
    REQUIRE(planner.CanAdmit(AgentId{1}));
    REQUIRE_FALSE(planner.CanAdmit(AgentId{2}));
}

TEST_CASE("Planning budget nested leaf permits share actor admission count and time", "[AIWorld][PlanningWorkScheduler]")
{
    PlanningWorkScheduler planner;
    planner.Request(AgentId{1}, 0);
    planner.Request(AgentId{2}, 0);
    planner.BeginFrame(1000, {AgentId{1}, AgentId{2}});
    PlanningWorkBudget budget(std::chrono::microseconds(2000), 16);
    PlanningWorkBudget::Scope scope(budget);
    {
        PlanningWorkBudget::ActorScope actor(planner, AgentId{1}, std::chrono::microseconds(500), 1);
        auto outer = PlanningWorkBudget::TryAcquire(QueryTime(1000));
        REQUIRE(bool(outer));
        auto inner = PlanningWorkBudget::TryAcquire(QueryTime(1100));
        REQUIRE(bool(inner));
        auto leaf = PlanningWorkBudget::TryAcquire(QueryTime(1200));
        REQUIRE(bool(leaf));
        leaf.Finish(QueryTime(1400));
        inner.Finish(QueryTime(1500));
        outer.Finish(QueryTime(1600));
        REQUIRE(actor.StartedOperations() == 1);
        REQUIRE(actor.OperationUs() == 600);
        REQUIRE_FALSE(PlanningWorkBudget::Available(QueryTime(1600)));
        REQUIRE_FALSE(bool(PlanningWorkBudget::TryAcquire(QueryTime(1600))));
    }
    {
        PlanningWorkBudget::ActorScope actor(planner, AgentId{2});
        auto next = PlanningWorkBudget::TryAcquire(QueryTime(9000000));
        REQUIRE(bool(next));
        next.Finish(QueryTime(9000100));
    }
    REQUIRE(budget.GetStatistics().Started == 2);
    REQUIRE(budget.GetStatistics().OperationUs == 700);
    REQUIRE(budget.GetStatistics().MaxOperationUs == 600);
    REQUIRE(planner.Priority(AgentId{1}) < planner.Priority(AgentId{2}));
}

TEST_CASE("Planning budget bounded actor slice prevents a cheap persistent search consuming the frame", "[AIWorld][PlanningWorkScheduler]")
{
    PlanningWorkScheduler planner;
    planner.Request(AgentId{1}, 0);
    planner.Request(AgentId{2}, 0);
    planner.BeginFrame(1000, {AgentId{1}, AgentId{2}});
    PlanningWorkBudget budget(std::chrono::microseconds(2000), 16);
    PlanningWorkBudget::Scope scope(budget);
    {
        PlanningWorkBudget::ActorScope actor(planner, AgentId{1});
        for (unsigned n = 0; n < 4; ++n)
        {
            auto permit = PlanningWorkBudget::TryAcquire(QueryTime(n));
            REQUIRE(bool(permit)); permit.Finish(QueryTime(n + 1));
        }
        REQUIRE_FALSE(PlanningWorkBudget::Available());
    }
    {
        PlanningWorkBudget::ActorScope actor(planner, AgentId{2});
        auto permit = PlanningWorkBudget::TryAcquire(QueryTime(4));
        REQUIRE(bool(permit)); permit.Finish(QueryTime(5));
    }
    REQUIRE(budget.GetStatistics().Started == 5);
    REQUIRE(budget.GetStatistics().OperationUs == 5);
}

TEST_CASE("Planning resume due cohort peeks do not reschedule unprocessed needs", "[AIWorld][PlanningWorkScheduler]")
{
    AgentUpdateScheduler needs;
    std::vector<AgentId> members;
    for (uint64 id = 1; id <= 200; ++id) members.push_back(AgentId{id});
    needs.Sync(members, 0, 1000, 42);
    auto cohort = needs.DueAgents(5000);
    REQUIRE(cohort.size() == 128);
    REQUIRE(needs.DueAgents(5000) == cohort);
    auto beforeLate = needs.OldestLateMs(5000);
    AgentId chosen = cohort.back();
    auto update = needs.PopDueAgent(chosen, 5000);
    REQUIRE(update.has_value());
    REQUIRE(update->ElapsedMs == 5000);
    REQUIRE_FALSE(needs.PopDueAgent(chosen, 5000).has_value());
    REQUIRE(needs.OldestLateMs(5000) == beforeLate);
    REQUIRE(needs.DueAgents(5000).front() == cohort.front());
    auto oldest = needs.PopDue(5100);
    REQUIRE(oldest->Agent == cohort.front());
    REQUIRE(oldest->ElapsedMs == 5100);
    std::set<uint64> remaining;
    while (auto next = needs.PopDue(5100)) REQUIRE(remaining.insert(next->Agent.Value).second);
    // A chosen actor may be due again when its original phase has passed;
    // its old lazy heap entry must not replay the earlier 5-second update.
    REQUIRE(remaining.size() >= 198);
    REQUIRE(needs.DueAgents(5100).empty());
    needs.Sync({chosen}, 5100, 1000, 42);
    REQUIRE_FALSE(needs.PopDueAgent(AgentId{9999}, 9000).has_value());
    auto next = needs.PopDue(9000);
    REQUIRE(next->Agent == chosen);
    REQUIRE(next->ElapsedMs == uint32(9000 - (remaining.count(chosen.Value) ? 5100 : 5000)));
}

TEST_CASE("Planning resume continuous mixed 1859 demand is fair with due cohorts and variable maintenance", "[AIWorld][PlanningWorkScheduler]")
{
    for (unsigned profile : {0u, 1u, 2u})
    {
        bool mixed = profile != 0;
        bool suspendChurn = profile == 2;
        AgentUpdateScheduler needs;
        PlanningWorkScheduler planner;
        std::vector<AgentId> allMembers, members;
        for (uint64 id = 1; id <= 1859; ++id) allMembers.push_back(AgentId{id});
        members = allMembers;
        needs.Sync(members, 0, 1000, 0x4e454544);
        struct State { uint64 Grants = 0, Visits = 0, LastWork = 0, Elapsed = 0, LastNeeds = 0, Cursor = 0,
            Epoch = 0, UnavailableSinceWork = 0; bool Present = true; };
        std::vector<State> states(1860);
        uint64 maxGap = 0, maxEligibleGap = 0, maxNeedsLate = 0, queries = 0, refused = 0;
        // Demand remains live for the entire three minutes: finishing a
        // candidate group immediately creates another 9-23 candidate search.
        for (uint64 now = 20; now <= 180000; now += 20)
        {
            if (now % 1000 == 0)
            {
                if (suspendChurn)
                {
                    members.clear();
                    for (AgentId id : allMembers)
                    {
                        State& state = states[id.Value];
                        bool present = id.Value % 41 != 0 || now % 31000 >= 5000;
                        if (present && !state.Present) { state.Elapsed = 0; state.Epoch = now; }
                        state.Present = present;
                        if (present) members.push_back(id);
                    }
                    planner.SyncMembership(members);
                }
                needs.Sync(members, now, 1000, 0x4e454544);
            }
            auto cohort = needs.DueAgents(now);
            auto eligible = [&](AgentId id)
            {
                return !mixed || id.Value % 13 != 0 || now % (suspendChurn ? 7000 : 60000) >= 1500;
            };
            for (AgentId id : allMembers)
                if (!states[id.Value].Present || !eligible(id)) states[id.Value].UnavailableSinceWork += 20;
            std::vector<AgentId> ready;
            for (AgentId id : cohort)
            {
                if (eligible(id)) { planner.Request(id, now); ready.push_back(id); }
                else if (suspendChurn && planner.HasRequest(id)) planner.Request(id, now);
                else planner.Cancel(id);
            }
            std::stable_sort(cohort.begin(), cohort.end(), [&](AgentId a, AgentId b)
                { return (eligible(a) ? planner.Priority(a) : std::numeric_limits<uint64>::max()) <
                         (eligible(b) ? planner.Priority(b) : std::numeric_limits<uint64>::max()); });
            planner.BeginFrame(now, ready);
            uint64 usedUs = 0;
            PlanningWorkBudget budget(std::chrono::microseconds(2000), 16);
            PlanningWorkBudget::Scope scope(budget);
            for (AgentId id : cohort)
            {
                if (usedUs >= 4000) break;
                auto due = needs.PopDueAgent(id, now);
                REQUIRE(due.has_value());
                State& state = states[id.Value];
                ++state.Visits; state.Elapsed += due->ElapsedMs; state.LastNeeds = now;
                maxNeedsLate = std::max(maxNeedsLate, due->LateMs);
                usedUs += mixed ? 20 + (id.Value * 17 + now / 20) % 80 : 20;
                PlanningWorkBudget::ActorScope actor(planner, id);
                if (!eligible(id)) continue; // Combat/care maintenance still receives needs time.
                for (;;)
                {
                    auto work = PlanningWorkBudget::TryAcquire(QueryTime(now * 1000 + usedUs));
                    if (!work) { ++refused; break; }
                    uint64 gap = state.LastWork ? now - state.LastWork : now;
                    maxGap = std::max(maxGap, gap);
                    maxEligibleGap = std::max(maxEligibleGap, gap - std::min(gap, state.UnavailableSinceWork));
                    state.UnavailableSinceWork = 0;
                    state.LastWork = now; ++state.Grants; ++queries;
                    state.Cursor = (state.Cursor + 1) % (9 + id.Value % 15);
                    // Rare indivisible long queries may overrun a slice;
                    // every following top-level query must then be refused.
                    uint64 cost = !mixed ? 1 : id.Value % 311 == 0 ? 2500 : id.Value % 97 == 0 ? 600 : 25 + id.Value % 106;
                    usedUs += cost;
                    work.Finish(QueryTime(now * 1000 + usedUs));
                }
            }
            REQUIRE(budget.GetStatistics().Started <= 16);
            REQUIRE(usedUs < 8000); // One actor's indivisible work can cross the 4 ms deadline.
        }
        uint64 minimum = std::numeric_limits<uint64>::max();
        for (AgentId id : allMembers)
        {
            State const& state = states[id.Value];
            CAPTURE(profile, id.Value, state.Grants, state.Visits, state.LastWork, maxGap);
            REQUIRE(state.Grants >= 8);
            REQUIRE(state.Visits >= 80);
            REQUIRE(state.Elapsed == state.LastNeeds - state.Epoch);
            REQUIRE(state.LastNeeds > 175000);
            REQUIRE(state.LastWork > 150000);
            minimum = std::min(minimum, state.Grants);
        }
        REQUIRE(maxEligibleGap < 30000);
        REQUIRE(maxGap < (suspendChurn ? 40000 : 30000));
        REQUIRE(maxNeedsLate < 10000);
        REQUIRE(refused > queries / 4);
        std::cout << "Planning stress profile=" << profile << " agents=1859 durationMs=180000 queries=" << queries
                  << " minimumGrants=" << minimum << " maxServiceGapMs=" << maxGap
                  << " maxEligibleServiceGapMs=" << maxEligibleGap
                  << " maxNeedsLateMs=" << maxNeedsLate << " refused=" << refused << '\n';
        planner.SyncMembership({});
        needs.Sync({}, 180000, 1000, 0x4e454544);
        REQUIRE(planner.PendingCount() == 0);
        REQUIRE(needs.DueAgents(200000).empty());
    }
}
