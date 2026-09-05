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
    // Milestone 2.13C2 P2 fix, round 1 (STATIC review): Find() is
    // const-only - a caller can never assign straight into a stored
    // instance's own State/Progress/etc through it, only through
    // Accept()/ApplyProgress()/Complete()/Fail()/Expire() below.
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

TEST_CASE("DynamicQuestRegistry::Accept commits Offered -> Active against its own stored instance", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    ObjectGuid player = ObjectGuid::Create<HighGuid::Player>(uint32(1));
    DynamicQuestTransitionResult result = registry.Accept(DynamicQuestId{7}, player, 10000);
    REQUIRE(result.IsAccepted());

    DynamicQuestInstance const* stored = registry.Find(DynamicQuestId{7});
    REQUIRE(stored->State == DynamicQuestState::Active);
    REQUIRE(stored->AcceptedByPlayerGuid == player);
}

TEST_CASE("DynamicQuestRegistry::Accept rejects an unknown id as QuestNotFound", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;

    DynamicQuestTransitionResult result = registry.Accept(DynamicQuestId{999}, ObjectGuid::Create<HighGuid::Player>(uint32(1)), 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::QuestNotFound);
}

TEST_CASE("DynamicQuestRegistry::Accept a second time on an already-Active quest is rejected, not a silent re-bind", "[DynamicQuestRegistry]")
{
    // The direct analogue of the earlier "two Accept results racing
    // against the same snapshot" scenario - but there is no longer any
    // way to even construct that race, since Accept() itself always
    // reads its OWN current stored instance immediately before deciding.
    // A second call simply sees the now-Active instance and is rejected
    // by AcceptDynamicQuest()'s own InvalidTransition rule.
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    ObjectGuid player1 = ObjectGuid::Create<HighGuid::Player>(uint32(1));
    ObjectGuid player2 = ObjectGuid::Create<HighGuid::Player>(uint32(2));

    REQUIRE(registry.Accept(DynamicQuestId{7}, player1, 10000).IsAccepted());
    DynamicQuestTransitionResult second = registry.Accept(DynamicQuestId{7}, player2, 10000);
    REQUIRE_FALSE(second.IsAccepted());
    REQUIRE(second.Reason == DynamicQuestRejectReason::InvalidTransition);

    DynamicQuestInstance const* stored = registry.Find(DynamicQuestId{7});
    REQUIRE(stored->AcceptedByPlayerGuid == player1); // never re-bound to player2
}

TEST_CASE("DynamicQuestRegistry::ApplyProgress commits progress against its own stored instance", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));
    ObjectGuid player = ObjectGuid::Create<HighGuid::Player>(uint32(1));
    REQUIRE(registry.Accept(DynamicQuestId{7}, player, 10000).IsAccepted());

    DynamicQuestTransitionResult first = registry.ApplyProgress(DynamicQuestId{7}, player, 100, 10000);
    REQUIRE(first.IsAccepted());

    DynamicQuestTransitionResult replay = registry.ApplyProgress(DynamicQuestId{7}, player, 100, 10000);
    REQUIRE_FALSE(replay.IsAccepted());
    REQUIRE(replay.Reason == DynamicQuestRejectReason::DuplicateProgressEvent);

    DynamicQuestTransitionResult second = registry.ApplyProgress(DynamicQuestId{7}, player, 200, 10000);
    REQUIRE(second.IsAccepted());

    DynamicQuestInstance const* stored = registry.Find(DynamicQuestId{7});
    REQUIRE(stored->Progress == 2);
    REQUIRE(stored->ConsumedProgressEventIds == std::vector<uint64>{100, 200});
}

TEST_CASE("DynamicQuestRegistry cannot be tricked into committing a transition computed from a fabricated source instance", "[DynamicQuestRegistry]")
{
    // Milestone 2.13C2 P2 fix, round 3 (STATIC review): the exact bypass
    // an earlier ApplyTransition(DynamicQuestTransitionResult const&)
    // design could not rule out - a caller builds its own
    // DynamicQuestInstance claiming to already be Active with
    // Progress == RequiredCount (even reusing the real stored Id), feeds
    // it directly to CompleteDynamicQuest() to legitimately obtain an
    // IsAccepted() result, and would previously have been able to commit
    // that fake result over a stored instance that is still genuinely
    // Offered. There is now no public entry point that accepts a
    // caller-supplied DynamicQuestInstance/DynamicQuestTransitionResult
    // at all - Complete() (and Accept()/ApplyProgress()/Fail()/Expire())
    // only ever operate on the id it looks up internally, so this
    // fabricated result has nothing to be committed THROUGH; it is
    // simply a value sitting unused in this test.
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7, 3)));

    DynamicQuestInstance const* stored = registry.Find(DynamicQuestId{7});
    REQUIRE(stored->State == DynamicQuestState::Offered);

    DynamicQuestInstance fabricated = MakeInstance(7, 3);
    fabricated.State = DynamicQuestState::Active;
    fabricated.Progress = 3;
    fabricated.AcceptedByPlayerGuid = ObjectGuid::Create<HighGuid::Player>(uint32(1));

    DynamicQuestTransitionResult fakeComplete = CompleteDynamicQuest(fabricated, 10000);
    REQUIRE(fakeComplete.IsAccepted()); // a genuinely valid C1 result - just not one anything can commit

    // The only way to actually attempt completing dynamic quest id 7 is
    // through the registry's own Complete(), which reads its own stored
    // (still Offered) instance - it can never see `fabricated` or
    // `fakeComplete` at all.
    DynamicQuestTransitionResult result = registry.Complete(DynamicQuestId{7}, 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::InvalidTransition); // Offered, not Active

    DynamicQuestInstance const* afterward = registry.Find(DynamicQuestId{7});
    REQUIRE(afterward->State == DynamicQuestState::Offered); // completely untouched
}

TEST_CASE("DynamicQuestRegistry::Complete/Fail/Expire reject an unknown id as QuestNotFound", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;

    REQUIRE(registry.Complete(DynamicQuestId{999}, 10000).Reason == DynamicQuestRejectReason::QuestNotFound);
    REQUIRE(registry.Fail(DynamicQuestId{999}, 10000).Reason == DynamicQuestRejectReason::QuestNotFound);
    REQUIRE(registry.Expire(DynamicQuestId{999}, 10000).Reason == DynamicQuestRejectReason::QuestNotFound);
    REQUIRE(registry.ApplyProgress(DynamicQuestId{999}, ObjectGuid::Create<HighGuid::Player>(uint32(1)), 100, 10000).Reason == DynamicQuestRejectReason::QuestNotFound);
}

TEST_CASE("DynamicQuestRegistry::Expire commits Offered -> Expired once past the deadline", "[DynamicQuestRegistry]")
{
    DynamicQuestRegistry registry;
    REQUIRE(registry.Add(MakeInstance(7)));

    DynamicQuestInstance const* offered = registry.Find(DynamicQuestId{7});
    uint64 expiresAtMs = offered->ExpiresAtMs;

    DynamicQuestTransitionResult tooEarly = registry.Expire(DynamicQuestId{7}, expiresAtMs - 1);
    REQUIRE_FALSE(tooEarly.IsAccepted());
    REQUIRE(tooEarly.Reason == DynamicQuestRejectReason::NotYetExpired);

    DynamicQuestTransitionResult result = registry.Expire(DynamicQuestId{7}, expiresAtMs);
    REQUIRE(result.IsAccepted());
    REQUIRE(registry.Find(DynamicQuestId{7})->State == DynamicQuestState::Expired);
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
