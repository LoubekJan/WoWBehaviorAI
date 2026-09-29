/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "tc_catch2.h"
#include "Scheduler/AgentUpdateScheduler.h"
#include "Agent/LivingRolePolicy.h"
#include <array>
#include <set>

TEST_CASE("Adjacent NPCs spread updates across a second without losing needs time", "[AIWorld][AgentUpdateScheduler]")
{
    AgentUpdateScheduler scheduler;
    std::vector<AgentId> agents;
    for (uint64 id = 79000; id < 80859; ++id) agents.push_back(AgentId{id});
    scheduler.Sync(agents, 1000, 1000, 0x4e454544);
    std::unordered_map<uint64, uint64> elapsed, last;
    std::array<unsigned, 10> firstSecond{};
    unsigned maxBatch = 0;
    for (uint64 now = 1010; now <= 11000; now += 10)
    {
        if (now % 1000 == 0) scheduler.Sync(agents, now, 1000, 0x4e454544);
        unsigned batch = 0;
        while (auto due = scheduler.PopDue(now))
        {
            REQUIRE(due->LateMs < 10);
            REQUIRE(due->ElapsedMs > 0);
            elapsed[due->Agent.Value] += due->ElapsedMs;
            last[due->Agent.Value] = now;
            ++batch;
            if (now <= 2000) ++firstSecond[(now - 1001) / 100];
        }
        maxBatch = std::max(maxBatch, batch);
    }
    REQUIRE(elapsed.size() == agents.size());
    REQUIRE(maxBatch < 64);
    for (unsigned count : firstSecond) REQUIRE(count > 100);
    for (AgentId id : agents)
    {
        REQUIRE(elapsed[id.Value] == last[id.Value] - 1000);
        REQUIRE(last[id.Value] > 10000);
    }
}

TEST_CASE("A delayed NPC pass preserves phases and bounded draining does not starve agents", "[AIWorld][AgentUpdateScheduler]")
{
    AgentUpdateScheduler scheduler;
    std::vector<AgentId> agents;
    for (uint64 id = 1; id <= 200; ++id) agents.push_back(AgentId{id});
    scheduler.Sync(agents, 0, 1000, 0);
    REQUIRE(scheduler.OldestLateMs(5000) >= 4000);
    std::set<uint64> seen;
    // Simulate a tiny per-frame budget after a five-second server stall.
    for (uint64 now = 5000; now < 5100; now += 10)
        for (unsigned n = 0; n < 20; ++n)
        {
            auto due = scheduler.PopDue(now);
            REQUIRE(due.has_value());
            REQUIRE(seen.insert(due->Agent.Value).second);
            REQUIRE(due->ElapsedMs == now);
        }
    REQUIRE(seen.size() == 200);
    // Missed updates are coalesced, not replayed 5 times per agent.
    unsigned restOfTick = 0;
    while (scheduler.PopDue(5090)) ++restOfTick;
    REQUIRE(restOfTick < 40);
    std::set<uint64> phases;
    for (uint64 now = 5100; now <= 6100; ++now)
        if (scheduler.PopDue(now)) phases.insert(now % 1000);
    REQUIRE(phases.size() > 100);
}

TEST_CASE("Refreshing registry membership preserves elapsed time and removes stale NPC work", "[AIWorld][AgentUpdateScheduler]")
{
    AgentUpdateScheduler scheduler;
    scheduler.Sync({AgentId{1}, AgentId{2}}, 1000, 1000, 0);
    scheduler.Sync({AgentId{2}, AgentId{3}, AgentId{3}, AgentId{}}, 2000, 1000, 0);
    auto due = scheduler.PopDue(2000);
    REQUIRE(due.has_value());
    REQUIRE(due->Agent == AgentId{2});
    REQUIRE(due->ElapsedMs == 1000);
    REQUIRE_FALSE(scheduler.PopDue(2000).has_value());
    std::set<uint64> seen;
    while (auto next = scheduler.PopDue(3000))
    {
        REQUIRE(next->ElapsedMs == 1000);
        seen.insert(next->Agent.Value);
    }
    REQUIRE(seen == std::set<uint64>{2, 3});
    scheduler.Sync({}, 3000, 1000, 0);
    REQUIRE_FALSE(scheduler.PopDue(10000).has_value());
    scheduler.Sync({AgentId{2}}, 10000, 0, 0);
    REQUIRE_FALSE(scheduler.PopDue(10000).has_value());
    REQUIRE(scheduler.PopDue(10001)->ElapsedMs == 1);
}

TEST_CASE("NPC peaceful pauses vary across neighbours and successive actions", "[AIWorld][AgentUpdateScheduler]")
{
    std::set<uint32> initial, pauses, cycles;
    for (uint64 id = 79000; id < 79100; ++id)
    {
        uint32 start = LivingRolePolicy::PauseMs(id, 0, 0, 14999);
        initial.insert(start / 1000);
        uint32 pause = LivingRolePolicy::PauseMs(id, 1, 4000, 8000);
        REQUIRE(pause >= 4000);
        REQUIRE(pause <= 8000);
        pauses.insert(pause / 1000);
        cycles.insert(LivingRolePolicy::PauseMs(79000, id, 4000, 8000));
    }
    REQUIRE(initial.size() >= 13);
    REQUIRE(pauses.size() >= 4);
    REQUIRE(cycles.size() > 90);
}
