/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingAdviceBudget.h"
#include <limits>

TEST_CASE("Planning advice queue skips a refreshed actor that never acquires its turn", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(99, 1000));
    REQUIRE_FALSE(budget.Acquire(1, 1000));
    REQUIRE_FALSE(budget.Acquire(2, 1000));
    bool acquired = false;
    for (uint64 now = 2000; now <= 10000; now += 1000)
    {
        // A remains eligible/dispatchable but another role decision prevents
        // it from reaching Acquire. Refresh must not indefinitely reserve it.
        budget.Refresh(1, now, true, false);
        budget.Refresh(2, now, true, false);
        if (budget.Acquire(2, now))
        {
            acquired = true;
            REQUIRE(now >= 3000); // the existing global interval still applies
            REQUIRE(budget.WaitMs(1, now) == now - 1000);
            break;
        }
    }
    REQUIRE(acquired);
}

TEST_CASE("Planning advice queue starts its bounded offer after cooldown without refresh renewal", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(99, 1000));
    REQUIRE_FALSE(budget.Acquire(1, 1000));
    REQUIRE_FALSE(budget.Acquire(2, 1000));
    for (uint64 now = 1100; now <= 7900; now += 100)
    {
        budget.Refresh(1, now, true, false);
        REQUIRE(budget.Ready(1, now) == (now >= 3000));
        REQUIRE_FALSE(budget.Ready(2, now));
        REQUIRE_FALSE(budget.Acquire(2, now));
    }
    REQUIRE(budget.Acquire(2, 8000));
    REQUIRE(budget.WaitMs(1, 8000) == 7000);
    REQUIRE_FALSE(budget.Acquire(1, 9999));
    REQUIRE(budget.Acquire(1, 10000));
}

TEST_CASE("Planning advice queue keeps an offered waiting actor ahead of new arrivals", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(99, 1000));
    REQUIRE_FALSE(budget.Acquire(1, 1000));
    REQUIRE_FALSE(budget.Acquire(2, 1000));
    REQUIRE(budget.Ready(1, 3000));
    REQUIRE_FALSE(budget.Acquire(3, 7999)); // new caller cannot replace the offer
    REQUIRE_FALSE(budget.Acquire(3, 8000)); // next existing waiter receives it
    REQUIRE(budget.Acquire(2, 8000));
    REQUIRE(budget.WaitMs(1, 8000) == 7000);
    REQUIRE(budget.Ready(3, 10000));
    REQUIRE_FALSE(budget.Acquire(1, 10000));
    REQUIRE(budget.Acquire(3, 10000));
}

TEST_CASE("Planning advice queue passes missed urgent offers to normal requesters", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(99, 1000));
    for (uint64 id = 1; id <= 4; ++id) REQUIRE_FALSE(budget.Acquire(id, 1000, true));
    REQUIRE_FALSE(budget.Acquire(5, 1000));
    REQUIRE_FALSE(budget.Acquire(6, 1000));
    bool acquired = false;
    for (uint64 now = 2000; now <= 18000; now += 1000)
    {
        for (uint64 id = 1; id <= 4; ++id) budget.Refresh(id, now, true, true);
        budget.Refresh(5, now, true, false);
        budget.Refresh(6, now, true, false);
        if (budget.Acquire(5, now))
        {
            REQUIRE(now == 18000); // three unused 5s urgent offers, then normal
            REQUIRE(budget.WaitMs(1, now) == 17000);
            acquired = true;
        }
    }
    REQUIRE(acquired);
    REQUIRE(budget.Ready(4, 20000));
    // The urgent offer may also be passed. Normal 6 remains live afterwards.
    for (uint64 now = 21000; now <= 35000; now += 1000)
    {
        for (uint64 id = 1; id <= 4; ++id) budget.Refresh(id, now, true, true);
        budget.Refresh(6, now, true, false);
    }
    REQUIRE(budget.Acquire(6, 35000));
}

TEST_CASE("Planning advice queue preserves successful three urgent one normal and two second rate", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(99, 1000));
    REQUIRE_FALSE(budget.Acquire(1, 1000));
    for (uint64 id = 2; id <= 5; ++id) REQUIRE_FALSE(budget.Acquire(id, 1000, true));
    REQUIRE(budget.Acquire(2, 3000, true));
    REQUIRE_FALSE(budget.Acquire(3, 4999, true));
    REQUIRE(budget.Acquire(3, 5000, true));
    REQUIRE(budget.Acquire(4, 7000, true));
    REQUIRE_FALSE(budget.Acquire(5, 9000, true));
    REQUIRE(budget.Acquire(1, 9000));
    REQUIRE(budget.Acquire(5, 11000, true));
}

TEST_CASE("Planning advice queue releases cancelled suspended and expired offers without erasing other ages", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(99, 1000));
    for (uint64 id = 1; id <= 4; ++id) REQUIRE_FALSE(budget.Acquire(id, 1000));
    REQUIRE(budget.Ready(1, 3000));
    budget.Refresh(1, 3001, true, false, false);
    REQUIRE(budget.Ready(2, 3001));
    budget.Cancel(2);
    REQUIRE(budget.Ready(3, 3001));
    budget.Refresh(3, 3001, false, false);
    REQUIRE(budget.Acquire(4, 3001));
    REQUIRE(budget.WaitMs(1, 3001) == 2001);
    budget.Refresh(1, 5001, true, false);
    REQUIRE(budget.Ready(1, 5001));
    REQUIRE_FALSE(budget.Ready(1, 35002)); // absent ticket, not refreshed
    REQUIRE(budget.Size() == 0);
}

TEST_CASE("Planning advice queue is bounded and a late ticket crosses a continuously refreshed crowd", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(9999, 1000));
    constexpr uint64 count = 1859;
    for (uint64 id = 1; id <= count; ++id) REQUIRE_FALSE(budget.Acquire(id, 1000));
    bool acquired = false;
    // Every predecessor stays dispatchable, but none acquires. Refresh the
    // whole crowd within membership TTL; the last actor must still proceed.
    for (uint64 now = 3000; now <= 3000 + (count - 1) * 5000; now += 5000)
    {
        if ((now - 3000) % 25000 == 0)
            for (uint64 id = 1; id <= count; ++id) budget.Refresh(id, now, true, false);
        if (budget.Acquire(count, now)) { acquired = true; break; }
    }
    REQUIRE(acquired);
    REQUIRE(budget.Size() == count - 1);
    REQUIRE(budget.WaitMs(1, 3000 + (count - 1) * 5000) == 2000 + (count - 1) * 5000);
    for (uint64 id = 1; id < count; ++id) budget.Cancel(id);
    REQUIRE(budget.Size() == 0);
    // Refusing new tickets at the cap neither grows memory nor skips waiters.
    REQUIRE(budget.Acquire(9999, 10000000));
    for (uint64 id = 1; id <= 2100; ++id) REQUIRE_FALSE(budget.Acquire(id, 10000000));
    REQUIRE(budget.Size() == 2048);
    REQUIRE_FALSE(budget.WaitMs(2100, 10000000));
    REQUIRE(budget.Acquire(1, 10002000));
}

TEST_CASE("Planning advice queue uses elapsed cooldown without uint64 deadline overflow", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    uint64 end = std::numeric_limits<uint64>::max();
    REQUIRE(budget.Acquire(1, end - 1000));
    REQUIRE_FALSE(budget.Acquire(2, end - 999));
    REQUIRE_FALSE(budget.Acquire(2, end));
    REQUIRE_FALSE(budget.Acquire(2, 0)); // a backwards clock cannot refund a slot
    REQUIRE_FALSE(budget.Ready(2, 1));
}

TEST_CASE("Planning advice queue also bounds urgent offers cancelled or suspended before acquisition", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(99, 1000));
    REQUIRE_FALSE(budget.Acquire(10, 1000));
    for (uint64 id = 1; id <= 4; ++id) REQUIRE_FALSE(budget.Acquire(id, 1000, true));
    REQUIRE(budget.Ready(1, 3000));
    budget.Cancel(1);
    REQUIRE(budget.Ready(2, 3000));
    budget.Refresh(2, 3000, true, true, false);
    REQUIRE(budget.Ready(3, 3000));
    budget.Refresh(3, 3000, false, true);
    REQUIRE(budget.Ready(10, 3000));
    REQUIRE(budget.WaitMs(10, 3000) == 2000);
    REQUIRE(budget.Acquire(10, 3000)); // Passing offers does not spend a slot.
    REQUIRE_FALSE(budget.Acquire(4, 4999, true));
    REQUIRE(budget.Acquire(4, 5000, true));
    REQUIRE(budget.WaitMs(2, 5000) == 4000); // suspended ticket kept its age
}
