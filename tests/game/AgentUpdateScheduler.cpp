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

TEST_CASE("An early movement wake preserves the periodic phase and charges elapsed needs time once", "[AIWorld][AgentUpdateScheduler]")
{
    AgentUpdateScheduler scheduler;
    AgentId actor{901034};
    uint64 start = 1000, salt = 0x4e454544;
    uint32 interval = 1000;
    uint64 periodic = start + 1 + StableAgentHash(actor.Value ^ salt) % interval;
    scheduler.Sync({actor}, start, interval, salt);
    REQUIRE(periodic > start + 2);
    uint64 early = start + (periodic - start) / 2;
    REQUIRE(scheduler.Wake(actor, early));
    REQUIRE(scheduler.DueAgents(early) == std::vector<AgentId>{actor});
    auto woke = scheduler.PopDue(early);
    REQUIRE(woke.has_value());
    REQUIRE(woke->ElapsedMs == early - start);
    REQUIRE(woke->LateMs == 0);
    REQUIRE_FALSE(scheduler.PopDue(early).has_value());
    REQUIRE_FALSE(scheduler.PopDue(periodic - 1).has_value());
    auto regular = scheduler.PopDue(periodic);
    REQUIRE(regular.has_value());
    REQUIRE(regular->ElapsedMs == periodic - early);
    REQUIRE(uint64(woke->ElapsedMs) + regular->ElapsedMs == periodic - start);
    REQUIRE_FALSE(scheduler.PopDue(periodic).has_value());
    REQUIRE_FALSE(scheduler.PopDue(periodic + interval - 1).has_value());
    REQUIRE(scheduler.PopDue(periodic + interval)->ElapsedMs == interval);
}

TEST_CASE("Repeated wakes and stale heap entries cannot duplicate an NPC update", "[AIWorld][AgentUpdateScheduler]")
{
    AgentUpdateScheduler scheduler;
    AgentId actor{901034};
    uint64 periodic = 1001 + StableAgentHash(actor.Value ^ 0x4e454544) % 1000;
    scheduler.Sync({actor}, 1000, 1000, 0x4e454544);
    REQUIRE(scheduler.Wake(actor, 1001));
    REQUIRE_FALSE(scheduler.Wake(actor, 1001));
    REQUIRE_FALSE(scheduler.Wake(actor, 1002));
    REQUIRE(scheduler.DueAgents(periodic) == std::vector<AgentId>{actor});
    auto delayedWake = scheduler.PopDueAgent(actor, periodic - 1);
    REQUIRE(delayedWake.has_value());
    REQUIRE(delayedWake->ElapsedMs == periodic - 1001);
    // The old periodic heap entry now has the same time as the current one.
    // Its generation must not expose the NPC twice to a bounded cohort.
    REQUIRE(scheduler.DueAgents(periodic) == std::vector<AgentId>{actor});
    REQUIRE(scheduler.PopDueAgent(actor, periodic).has_value());
    REQUIRE(scheduler.DueAgents(periodic).empty());
    REQUIRE_FALSE(scheduler.PopDue(periodic).has_value());
    REQUIRE_FALSE(scheduler.Wake(actor, periodic));
    REQUIRE_FALSE(scheduler.Wake(actor, periodic - 1));
    REQUIRE(scheduler.Wake(actor, periodic + 1));
    REQUIRE(scheduler.PopDue(periodic + 1)->ElapsedMs == 1);
    REQUIRE_FALSE(scheduler.PopDue(periodic + 1).has_value());
}

TEST_CASE("An overdue periodic update is never delayed or replayed by a movement wake", "[AIWorld][AgentUpdateScheduler]")
{
    AgentUpdateScheduler scheduler;
    AgentId actor{901034};
    uint64 periodic = 1001 + StableAgentHash(actor.Value ^ 0x4e454544) % 1000;
    scheduler.Sync({actor}, 1000, 1000, 0x4e454544);
    uint64 delayed = periodic + 2500;
    REQUIRE_FALSE(scheduler.Wake(actor, delayed));
    REQUIRE(scheduler.OldestLateMs(delayed) == 2500);
    auto due = scheduler.PopDue(delayed);
    REQUIRE(due.has_value());
    REQUIRE(due->ElapsedMs == delayed - 1000);
    REQUIRE(due->LateMs == 2500);
    REQUIRE_FALSE(scheduler.PopDue(delayed).has_value());
    REQUIRE_FALSE(scheduler.PopDue(periodic + 2999).has_value());
    auto next = scheduler.PopDue(periodic + 3000);
    REQUIRE(next.has_value());
    REQUIRE(next->ElapsedMs == 500);
}

TEST_CASE("Registry refresh retains pending wakes only for the same registered membership", "[AIWorld][AgentUpdateScheduler]")
{
    AgentUpdateScheduler scheduler;
    AgentId first{901034}, removed{901035}, added{901036};
    uint64 periodic = 1001 + StableAgentHash(first.Value ^ 0x4e454544) % 1000;
    scheduler.Sync({first, removed}, 1000, 1000, 0x4e454544);
    REQUIRE(scheduler.Wake(first, 1001));
    REQUIRE(scheduler.Wake(removed, 1001));
    scheduler.Sync({first, added, added, AgentId{}}, 1001, 1000, 0x4e454544);
    REQUIRE(scheduler.DueAgents(1001) == std::vector<AgentId>{first});
    REQUIRE(scheduler.PopDue(1001)->Agent == first);
    REQUIRE_FALSE(scheduler.PopDue(1001).has_value());
    REQUIRE_FALSE(scheduler.Wake(removed, 1002));
    REQUIRE_FALSE(scheduler.Wake({}, 1002));
    auto regular = scheduler.PopDueAgent(first, periodic);
    REQUIRE(regular.has_value());
    REQUIRE(regular->ElapsedMs == periodic - 1001);
    scheduler.Sync({}, periodic, 1000, 0x4e454544);
    REQUIRE(scheduler.DueAgents(periodic + 10000).empty());
    REQUIRE_FALSE(scheduler.PopDue(periodic + 10000).has_value());
    scheduler.Sync({first}, periodic + 10000, 1000, 0x4e454544);
    REQUIRE_FALSE(scheduler.Wake(first, periodic + 10000));
    REQUIRE_FALSE(scheduler.PopDue(periodic + 10000).has_value());
}

TEST_CASE("An interval change retains an already requested early wake", "[AIWorld][AgentUpdateScheduler]")
{
    AgentUpdateScheduler scheduler;
    AgentId actor{901034};
    scheduler.Sync({actor}, 1000, 1000, 0x4e454544);
    REQUIRE(scheduler.Wake(actor, 1001));
    scheduler.Sync({actor}, 1001, 2000, 0x4e454544);
    auto woke = scheduler.PopDue(1001);
    REQUIRE(woke.has_value());
    REQUIRE(woke->ElapsedMs == 1);
    uint64 next = 1002 + StableAgentHash(actor.Value ^ 0x4e454544) % 2000;
    REQUIRE_FALSE(scheduler.PopDue(next - 1).has_value());
    REQUIRE(scheduler.PopDue(next)->ElapsedMs == next - 1001);
}

TEST_CASE("Many early wakes conserve actual elapsed time across delayed bounded draining", "[AIWorld][AgentUpdateScheduler]")
{
    AgentUpdateScheduler scheduler;
    std::vector<AgentId> actors{AgentId{901034}, AgentId{901035}, AgentId{901039}};
    std::unordered_map<uint64, uint64> elapsed, last;
    scheduler.Sync(actors, 1000, 1000, 0x4e454544);
    for (uint64 now = 1010; now <= 6000; now += 10)
    {
        if (now % 100 == 0)
            for (AgentId actor : actors) scheduler.Wake(actor, now);
        if (now % 1000 == 0) scheduler.Sync(actors, now, 1000, 0x4e454544);
        // One actor per frame. Wake does not bypass this admission limit.
        auto due = scheduler.PopDue(now);
        if (!due) continue;
        REQUIRE(due->ElapsedMs > 0);
        elapsed[due->Agent.Value] += due->ElapsedMs;
        last[due->Agent.Value] = now;
    }
    for (AgentId actor : actors)
    {
        REQUIRE(elapsed[actor.Value] == last[actor.Value] - 1000);
        REQUIRE(last[actor.Value] >= 5900);
    }
}
