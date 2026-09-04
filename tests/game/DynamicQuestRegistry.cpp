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

#include "Quest/DynamicQuestRegistry.h"

namespace
{
    DynamicQuestInstance MakeInstance(uint64 idValue, uint32 requiredCount = 3)
    {
        DynamicQuestInstance instance;
        instance.Id = DynamicQuestId{idValue};
        instance.State = DynamicQuestState::Offered;
        instance.Giver.Value = 42;
        instance.RequiredCount = requiredCount;
        instance.CreatedAtMs = 10000;
        instance.ExpiresAtMs = 210000;
        return instance;
    }
}

TEST_CASE("DynamicQuestRegistry::Add accepts a valid instance", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(1)));
    REQUIRE(registry.GetCount() == 1);
}

TEST_CASE("DynamicQuestRegistry::Add rejects DynamicQuestId{0}", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE_FALSE(registry.Add(MakeInstance(0)));
    REQUIRE(registry.GetCount() == 0);
}

TEST_CASE("DynamicQuestRegistry::Add rejects a duplicate id and leaves the original untouched", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(1, 3)));

    DynamicQuestInstance duplicate = MakeInstance(1, 99); // same id, different payload
    REQUIRE_FALSE(registry.Add(duplicate));
    REQUIRE(registry.GetCount() == 1);

    DynamicQuestInstance const* found = registry.Find(DynamicQuestId{1});
    REQUIRE(found != nullptr);
    REQUIRE(found->RequiredCount == 3); // original, not overwritten by the duplicate
}

TEST_CASE("DynamicQuestRegistry::Find returns the matching instance", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    DynamicQuestInstance* found = registry.Find(DynamicQuestId{7});
    REQUIRE(found != nullptr);
    REQUIRE(found->Id == DynamicQuestId{7});
}

TEST_CASE("DynamicQuestRegistry::Find returns nullptr for an unknown id", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    REQUIRE(registry.Find(DynamicQuestId{999}) == nullptr);
}

TEST_CASE("DynamicQuestRegistry::Find const overload returns the matching instance", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    DynamicQuestRegistry const& constRegistry = registry;
    DynamicQuestInstance const* found = constRegistry.Find(DynamicQuestId{7});
    REQUIRE(found != nullptr);
    REQUIRE(constRegistry.Find(DynamicQuestId{999}) == nullptr);
}

TEST_CASE("DynamicQuestRegistry::Remove erases an existing instance and returns true", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(1)));

    REQUIRE(registry.Remove(DynamicQuestId{1}));
    REQUIRE(registry.Find(DynamicQuestId{1}) == nullptr);
    REQUIRE(registry.GetCount() == 0);
}

TEST_CASE("DynamicQuestRegistry::Remove returns false for an unknown id", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(1)));

    REQUIRE_FALSE(registry.Remove(DynamicQuestId{999}));
    REQUIRE(registry.GetCount() == 1); // unaffected
}

TEST_CASE("Removing one quest does not affect any other", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(1)));
    REQUIRE(registry.Add(MakeInstance(2)));
    REQUIRE(registry.Add(MakeInstance(3)));

    REQUIRE(registry.Remove(DynamicQuestId{2}));

    REQUIRE(registry.Find(DynamicQuestId{1}) != nullptr);
    REQUIRE(registry.Find(DynamicQuestId{2}) == nullptr);
    REQUIRE(registry.Find(DynamicQuestId{3}) != nullptr);
    REQUIRE(registry.GetCount() == 2);
}

TEST_CASE("A fresh registry starts empty", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.GetCount() == 0);
    REQUIRE(registry.Find(DynamicQuestId{1}) == nullptr);
}
