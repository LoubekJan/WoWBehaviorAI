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

#include <algorithm>

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
    // Milestone 2.13C2 P2 fix (STATIC review): Find() is const-only - a
    // caller can never assign straight into a stored instance's own
    // State/Progress/etc through it, only through ApplyTransition().
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    DynamicQuestInstance const* found = registry.Find(DynamicQuestId{7});
    REQUIRE(found != nullptr);
    REQUIRE(found->Id == DynamicQuestId{7});
}

TEST_CASE("DynamicQuestRegistry::Find returns nullptr for an unknown id", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    REQUIRE(registry.Find(DynamicQuestId{999}) == nullptr);
}

TEST_CASE("DynamicQuestRegistry::ApplyTransition commits an accepted transition result", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    DynamicQuestInstance const* before = registry.Find(DynamicQuestId{7});
    REQUIRE(before->State == DynamicQuestState::Offered);
    REQUIRE(before->Revision == 0);

    DynamicQuestTransitionResult accept = AcceptDynamicQuest(*before, ObjectGuid::Create<HighGuid::Player>(uint32(1)), 10000);
    REQUIRE(accept.IsAccepted());
    REQUIRE(accept.SourceRevision == 0);
    REQUIRE(accept.Instance->Revision == 1);

    REQUIRE(registry.ApplyTransition(accept));

    DynamicQuestInstance const* after = registry.Find(DynamicQuestId{7});
    REQUIRE(after->State == DynamicQuestState::Active);
    REQUIRE(after->Revision == 1);
}

TEST_CASE("DynamicQuestRegistry::ApplyTransition rejects a stale commit computed from an already-superseded revision (Accept)", "[DynamicQuestRegistry]")
{
    // Milestone 2.13C2 P2 fix, round 2 (STATIC review): two independently
    // valid AcceptDynamicQuest() results, both computed from the SAME
    // Offered snapshot (simulating two players racing to accept the same
    // offer). Only the FIRST one committed may win - the second must be
    // rejected as stale, never silently re-binding an already-Active
    // quest to a different player.
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    DynamicQuestInstance const* snapshot = registry.Find(DynamicQuestId{7});
    ObjectGuid player1 = ObjectGuid::Create<HighGuid::Player>(uint32(1));
    ObjectGuid player2 = ObjectGuid::Create<HighGuid::Player>(uint32(2));

    DynamicQuestTransitionResult resultA = AcceptDynamicQuest(*snapshot, player1, 10000);
    DynamicQuestTransitionResult resultB = AcceptDynamicQuest(*snapshot, player2, 10000);
    REQUIRE(resultA.IsAccepted());
    REQUIRE(resultB.IsAccepted());
    REQUIRE(resultA.SourceRevision == resultB.SourceRevision); // both computed from the same snapshot

    REQUIRE(registry.ApplyTransition(resultA));
    REQUIRE_FALSE(registry.ApplyTransition(resultB)); // stale - stored revision already moved on

    DynamicQuestInstance const* stored = registry.Find(DynamicQuestId{7});
    REQUIRE(stored->State == DynamicQuestState::Active);
    REQUIRE(stored->AcceptedByPlayerGuid == player1); // resultA's own commit, never overwritten by resultB
}

TEST_CASE("DynamicQuestRegistry::ApplyTransition rejects a stale commit computed from an already-superseded revision (Progress)", "[DynamicQuestRegistry]")
{
    // Same race, for progress: two different authoritative events, both
    // applied against the SAME pre-progress snapshot. The second commit
    // must never silently overwrite ConsumedProgressEventIds and lose
    // the first event's own replay-guard entry.
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    ObjectGuid player = ObjectGuid::Create<HighGuid::Player>(uint32(1));
    DynamicQuestInstance const* offered = registry.Find(DynamicQuestId{7});
    DynamicQuestTransitionResult accept = AcceptDynamicQuest(*offered, player, 10000);
    REQUIRE(registry.ApplyTransition(accept));

    DynamicQuestInstance const* snapshot = registry.Find(DynamicQuestId{7});
    REQUIRE(snapshot->Progress == 0);

    DynamicQuestTransitionResult resultA = ApplyDynamicQuestProgress(*snapshot, player, 100, 10000);
    DynamicQuestTransitionResult resultB = ApplyDynamicQuestProgress(*snapshot, player, 200, 10000);
    REQUIRE(resultA.IsAccepted());
    REQUIRE(resultB.IsAccepted());
    REQUIRE(resultA.SourceRevision == resultB.SourceRevision);

    REQUIRE(registry.ApplyTransition(resultA));
    REQUIRE_FALSE(registry.ApplyTransition(resultB)); // stale

    DynamicQuestInstance const* stored = registry.Find(DynamicQuestId{7});
    REQUIRE(stored->Progress == 1);
    REQUIRE(stored->ConsumedProgressEventIds == std::vector<uint64>{100}); // event 100 preserved
    REQUIRE(std::find(stored->ConsumedProgressEventIds.begin(), stored->ConsumedProgressEventIds.end(), 200)
        == stored->ConsumedProgressEventIds.end()); // event 200 never silently lost/dropped nor applied
}

TEST_CASE("DynamicQuestRegistry::ApplyTransition rejects a rejected transition result", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    DynamicQuestTransitionResult rejected; // default-constructed: NotAttempted, no Instance
    REQUIRE_FALSE(registry.ApplyTransition(rejected));

    DynamicQuestInstance const* instance = registry.Find(DynamicQuestId{7});
    REQUIRE(instance->State == DynamicQuestState::Offered); // untouched
}

TEST_CASE("DynamicQuestRegistry::ApplyTransition rejects a result naming an id that was never added", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;

    DynamicQuestInstance orphan = MakeInstance(999);
    DynamicQuestTransitionResult accept = AcceptDynamicQuest(orphan, ObjectGuid::Create<HighGuid::Player>(uint32(1)), 10000);
    REQUIRE(accept.IsAccepted());

    REQUIRE_FALSE(registry.ApplyTransition(accept));
    REQUIRE(registry.Find(DynamicQuestId{999}) == nullptr);
    REQUIRE(registry.GetCount() == 0);
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
    REQUIRE(registry.GetHighestId() == DynamicQuestId{});
}

TEST_CASE("DynamicQuestRegistry::GetHighestId returns the highest registered id", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(5)));
    REQUIRE(registry.Add(MakeInstance(1)));
    REQUIRE(registry.Add(MakeInstance(9)));

    REQUIRE(registry.GetHighestId() == DynamicQuestId{9});

    REQUIRE(registry.Remove(DynamicQuestId{9}));
    REQUIRE(registry.GetHighestId() == DynamicQuestId{5});
}

TEST_CASE("DynamicQuestRegistry::GetIdsAfterUntil is bounded and cursor-resumable", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    for (uint64 idValue = 1; idValue <= 5; ++idValue)
        REQUIRE(registry.Add(MakeInstance(idValue)));

    DynamicQuestId until = registry.GetHighestId();
    REQUIRE(until == DynamicQuestId{5});

    std::vector<DynamicQuestId> firstPage = registry.GetIdsAfterUntil(DynamicQuestId{}, until, 2);
    REQUIRE(firstPage.size() == 2);
    REQUIRE(firstPage[0] == DynamicQuestId{1});
    REQUIRE(firstPage[1] == DynamicQuestId{2});

    std::vector<DynamicQuestId> secondPage = registry.GetIdsAfterUntil(firstPage.back(), until, 2);
    REQUIRE(secondPage.size() == 2);
    REQUIRE(secondPage[0] == DynamicQuestId{3});
    REQUIRE(secondPage[1] == DynamicQuestId{4});

    std::vector<DynamicQuestId> thirdPage = registry.GetIdsAfterUntil(secondPage.back(), until, 2);
    REQUIRE(thirdPage.size() == 1);
    REQUIRE(thirdPage[0] == DynamicQuestId{5});

    std::vector<DynamicQuestId> pastEnd = registry.GetIdsAfterUntil(thirdPage.back(), until, 2);
    REQUIRE(pastEnd.empty());
}

TEST_CASE("DynamicQuestRegistry::GetIdsAfterUntil never returns ids created after the until snapshot", "[DynamicQuestRegistry]")
{
    // The scan-cycle boundary itself - a quest added AFTER `until` was
    // snapshotted must not be discovered by a cycle already in progress,
    // the same starvation-prevention property AgentGroupRegistry::
    // GetGroupsAfterUntil() already guarantees.
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(1)));
    REQUIRE(registry.Add(MakeInstance(2)));

    DynamicQuestId until = registry.GetHighestId();
    REQUIRE(until == DynamicQuestId{2});

    REQUIRE(registry.Add(MakeInstance(3))); // created after the snapshot

    std::vector<DynamicQuestId> discovered = registry.GetIdsAfterUntil(DynamicQuestId{}, until, 100);
    REQUIRE(discovered.size() == 2);
    REQUIRE(discovered[0] == DynamicQuestId{1});
    REQUIRE(discovered[1] == DynamicQuestId{2});
}

TEST_CASE("DynamicQuestRegistry::GetIdsAfterUntil returns nothing for maxCount 0 or an already-exhausted range", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(1)));

    REQUIRE(registry.GetIdsAfterUntil(DynamicQuestId{}, DynamicQuestId{1}, 0).empty());
    REQUIRE(registry.GetIdsAfterUntil(DynamicQuestId{1}, DynamicQuestId{1}, 100).empty()); // after >= until
}
