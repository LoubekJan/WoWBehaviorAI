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

#include "Quest/DynamicQuestIdAllocator.h"

#include <limits>
#include <unordered_set>

TEST_CASE("AdvanceDynamicQuestIdCounter never hands out the first id as 0", "[DynamicQuestIdAllocator]")
{
    uint64 counter = 1;
    DynamicQuestId id = AdvanceDynamicQuestIdCounter(counter);
    REQUIRE(id == DynamicQuestId{1});
    REQUIRE(bool(id));
}

TEST_CASE("AdvanceDynamicQuestIdCounter is strictly monotonic", "[DynamicQuestIdAllocator]")
{
    uint64 counter = 1;
    DynamicQuestId first = AdvanceDynamicQuestIdCounter(counter);
    DynamicQuestId second = AdvanceDynamicQuestIdCounter(counter);
    DynamicQuestId third = AdvanceDynamicQuestIdCounter(counter);

    REQUIRE(first == DynamicQuestId{1});
    REQUIRE(second == DynamicQuestId{2});
    REQUIRE(third == DynamicQuestId{3});
}

TEST_CASE("AdvanceDynamicQuestIdCounter never reissues a previously-allocated id", "[DynamicQuestIdAllocator]")
{
    // Not just monotonic within one short sequence - genuinely no
    // repeats across many allocations, independent of anything a
    // DynamicQuestRegistry does with those ids afterward (e.g. Remove()
    // never feeds back into this counter).
    uint64 counter = 1;
    std::unordered_set<uint64> seen;
    for (int i = 0; i < 1000; ++i)
    {
        DynamicQuestId id = AdvanceDynamicQuestIdCounter(counter);
        REQUIRE(bool(id));
        REQUIRE(seen.insert(id.Value).second); // true only if genuinely new
    }
}

TEST_CASE("AdvanceDynamicQuestIdCounter hands out UINT64_MAX exactly once, then fails closed", "[DynamicQuestIdAllocator]")
{
    uint64 counter = std::numeric_limits<uint64>::max();

    DynamicQuestId last = AdvanceDynamicQuestIdCounter(counter);
    REQUIRE(last == DynamicQuestId{std::numeric_limits<uint64>::max()});
    REQUIRE(counter == 0); // explicitly marked exhausted, not wrapped to 1

    DynamicQuestId exhausted = AdvanceDynamicQuestIdCounter(counter);
    REQUIRE_FALSE(bool(exhausted));
    REQUIRE(exhausted == DynamicQuestId{});
    REQUIRE(counter == 0); // stays exhausted forever

    // Calling again changes nothing further.
    DynamicQuestId stillExhausted = AdvanceDynamicQuestIdCounter(counter);
    REQUIRE_FALSE(bool(stillExhausted));
    REQUIRE(counter == 0);
}

TEST_CASE("AdvanceDynamicQuestIdCounter on an already-exhausted counter returns DynamicQuestId{0} without touching it", "[DynamicQuestIdAllocator]")
{
    uint64 counter = 0;
    DynamicQuestId id = AdvanceDynamicQuestIdCounter(counter);
    REQUIRE(id == DynamicQuestId{});
    REQUIRE(counter == 0);
}
