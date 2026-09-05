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

#include "Quest/DynamicQuestLifecycle.h"
#include "Inference/QuestProposal.h"

#include <limits>

namespace
{
    QuestProposal MakeValidProposal()
    {
        QuestProposal proposal;
        proposal.Giver.Value = 42;
        proposal.GiverRuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);
        proposal.SourceEventId = 9001;
        proposal.SourceEventType = WorldEventType::CreatureKilled;
        proposal.Objective = QuestObjectiveType::KillCreature;
        proposal.TargetToken = 1;
        proposal.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(2002, 1);
        proposal.TargetEntry = 2002;
        proposal.TargetMapId = 0;
        proposal.RequiredCount = 3;
        proposal.MaxRangeYards = 40.0f;
        proposal.ExpiryMs = 200000;
        proposal.RewardMoneyCopper = 0;
        proposal.Title = "Cull the wolves";
        proposal.Description = "Thin the wolf pack near the road.";
        return proposal;
    }

    ObjectGuid PlayerGuid(uint32 lowGuid)
    {
        return ObjectGuid::Create<HighGuid::Player>(lowGuid);
    }

    DynamicQuestInstance MakeOfferedInstance(uint64 nowMs = 10000)
    {
        DynamicQuestTransitionResult result = OfferDynamicQuest(DynamicQuestId{777}, MakeValidProposal(), nowMs);
        REQUIRE(result.IsAccepted());
        return *result.Instance;
    }

    DynamicQuestInstance MakeActiveInstance(ObjectGuid player = PlayerGuid(1), uint64 nowMs = 10000)
    {
        DynamicQuestInstance offered = MakeOfferedInstance(nowMs);
        DynamicQuestTransitionResult result = AcceptDynamicQuest(offered, player, nowMs);
        REQUIRE(result.IsAccepted());
        return *result.Instance;
    }
}

TEST_CASE("OfferDynamicQuest builds a fully-populated Offered instance", "[DynamicQuestLifecycle]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestTransitionResult offerResult = OfferDynamicQuest(DynamicQuestId{777}, proposal, 10000);
    REQUIRE(offerResult.IsAccepted());
    DynamicQuestInstance const& instance = *offerResult.Instance;

    REQUIRE(instance.Id == DynamicQuestId{777});
    REQUIRE(instance.State == DynamicQuestState::Offered);
    REQUIRE(instance.Giver.Value == proposal.Giver.Value);
    REQUIRE(instance.GiverRuntimeGuid == proposal.GiverRuntimeGuid);
    REQUIRE(instance.Objective == proposal.Objective);
    REQUIRE(instance.TargetGuid == proposal.TargetGuid);
    REQUIRE(instance.TargetEntry == proposal.TargetEntry);
    REQUIRE(instance.TargetMapId == proposal.TargetMapId);
    REQUIRE(instance.RequiredCount == proposal.RequiredCount);
    REQUIRE(instance.Progress == 0);
    REQUIRE(instance.CreatedAtMs == 10000);
    REQUIRE(instance.ExpiresAtMs == 10000 + proposal.ExpiryMs);
    REQUIRE(instance.AcceptedByPlayerGuid.IsEmpty());
    REQUIRE(instance.ConsumedProgressEventIds.empty());
}

TEST_CASE("OfferDynamicQuest rejects DynamicQuestId{0}", "[DynamicQuestLifecycle]")
{
    // DynamicQuestId{0} is that type's own invalid/default value (see
    // its own comment) - a genuine lifecycle instance must never carry
    // it, so the allocator-side caller can rely on this boundary
    // rejecting a bug rather than silently accepting id 0.
    DynamicQuestTransitionResult result = OfferDynamicQuest(DynamicQuestId{0}, MakeValidProposal(), 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::InvalidQuestId);
    REQUIRE_FALSE(result.Instance.has_value());
}

TEST_CASE("OfferDynamicQuest computes ExpiresAtMs with a saturating add, never wrapping around", "[DynamicQuestLifecycle]")
{
    QuestProposal proposal = MakeValidProposal();
    proposal.ExpiryMs = std::numeric_limits<uint32>::max();

    uint64 nowMs = std::numeric_limits<uint64>::max() - 10;
    DynamicQuestTransitionResult offerResult = OfferDynamicQuest(DynamicQuestId{1}, proposal, nowMs);
    REQUIRE(offerResult.IsAccepted());
    DynamicQuestInstance const& instance = *offerResult.Instance;

    REQUIRE(instance.ExpiresAtMs == std::numeric_limits<uint64>::max());
    REQUIRE(instance.ExpiresAtMs >= nowMs); // never wrapped below "now"
}

// ---------------------------------------------------------------------
// Legal transitions
// ---------------------------------------------------------------------

TEST_CASE("AcceptDynamicQuest: Offered -> Active", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance offered = MakeOfferedInstance();
    ObjectGuid player = PlayerGuid(1);

    DynamicQuestTransitionResult result = AcceptDynamicQuest(offered, player, 10000);
    REQUIRE(result.IsAccepted());
    REQUIRE(result.Instance->State == DynamicQuestState::Active);
    REQUIRE(result.Instance->AcceptedByPlayerGuid == player);

    // The original value is untouched by a successful transition too -
    // every function returns a NEW instance, never mutates in place.
    REQUIRE(offered.State == DynamicQuestState::Offered);
    REQUIRE(offered.AcceptedByPlayerGuid.IsEmpty());
}

TEST_CASE("ApplyDynamicQuestProgress increments Progress on an Active instance", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance active = MakeActiveInstance(player);

    DynamicQuestTransitionResult result = ApplyDynamicQuestProgress(active, player, 501, 10000);
    REQUIRE(result.IsAccepted());
    REQUIRE(result.Instance->State == DynamicQuestState::Active);
    REQUIRE(result.Instance->Progress == 1);
    REQUIRE(result.Instance->ConsumedProgressEventIds == std::vector<uint64>{501});
}

TEST_CASE("CompleteDynamicQuest: Active -> Completed once Progress == RequiredCount", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance instance = MakeActiveInstance(player);

    for (uint64 eventId = 1; eventId <= instance.RequiredCount; ++eventId)
    {
        DynamicQuestTransitionResult progress = ApplyDynamicQuestProgress(instance, player, eventId, 10000);
        REQUIRE(progress.IsAccepted());
        instance = *progress.Instance;
    }
    REQUIRE(instance.Progress == instance.RequiredCount);

    DynamicQuestTransitionResult result = CompleteDynamicQuest(instance, 10000);
    REQUIRE(result.IsAccepted());
    REQUIRE(result.Instance->State == DynamicQuestState::Completed);
}

TEST_CASE("FailDynamicQuest: Active -> Failed", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance active = MakeActiveInstance();

    DynamicQuestTransitionResult result = FailDynamicQuest(active, 10000);
    REQUIRE(result.IsAccepted());
    REQUIRE(result.Instance->State == DynamicQuestState::Failed);
}

TEST_CASE("ExpireDynamicQuest: Offered -> Expired once past the deadline", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance offered = MakeOfferedInstance(10000);

    DynamicQuestTransitionResult result = ExpireDynamicQuest(offered, offered.ExpiresAtMs);
    REQUIRE(result.IsAccepted());
    REQUIRE(result.Instance->State == DynamicQuestState::Expired);
}

TEST_CASE("ExpireDynamicQuest: Active -> Expired once past the deadline", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance active = MakeActiveInstance();

    DynamicQuestTransitionResult result = ExpireDynamicQuest(active, active.ExpiresAtMs);
    REQUIRE(result.IsAccepted());
    REQUIRE(result.Instance->State == DynamicQuestState::Expired);
}

// ---------------------------------------------------------------------
// Illegal transitions / state-machine edges
// ---------------------------------------------------------------------

TEST_CASE("Offered -> Failed is not modeled: FailDynamicQuest rejects an Offered instance", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance offered = MakeOfferedInstance();

    DynamicQuestTransitionResult result = FailDynamicQuest(offered, 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::InvalidTransition);
}

TEST_CASE("CompleteDynamicQuest rejects an Offered instance", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance offered = MakeOfferedInstance();

    DynamicQuestTransitionResult result = CompleteDynamicQuest(offered, 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::InvalidTransition);
}

TEST_CASE("ApplyDynamicQuestProgress rejects an Offered instance", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance offered = MakeOfferedInstance();

    DynamicQuestTransitionResult result = ApplyDynamicQuestProgress(offered, PlayerGuid(1), 501, 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::InvalidTransition);
}

TEST_CASE("AcceptDynamicQuest rejects an already-Active instance (wrong-player accept)", "[DynamicQuestLifecycle]")
{
    // The quest was already accepted by player 1; a second accept
    // attempt by a DIFFERENT player must not silently steal or re-bind
    // it - the state machine rejects on state alone.
    DynamicQuestInstance active = MakeActiveInstance(PlayerGuid(1));

    DynamicQuestTransitionResult result = AcceptDynamicQuest(active, PlayerGuid(2), 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::InvalidTransition);
    REQUIRE(active.AcceptedByPlayerGuid == PlayerGuid(1)); // unchanged
}

TEST_CASE("AcceptDynamicQuest rejects an empty player identity", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance offered = MakeOfferedInstance();

    DynamicQuestTransitionResult result = AcceptDynamicQuest(offered, ObjectGuid::Empty, 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::InvalidPlayer);
}

TEST_CASE("AcceptDynamicQuest rejects a non-player GUID", "[DynamicQuestLifecycle]")
{
    // A non-empty GUID of the wrong entity type (e.g. a creature) must be
    // rejected exactly like an empty one - only ObjectGuid::IsPlayer()
    // qualifies as an authoritative player identity.
    DynamicQuestInstance offered = MakeOfferedInstance();
    ObjectGuid creatureGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);

    DynamicQuestTransitionResult result = AcceptDynamicQuest(offered, creatureGuid, 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::InvalidPlayer);
}

TEST_CASE("ApplyDynamicQuestProgress rejects a wrong-player caller", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance active = MakeActiveInstance(PlayerGuid(1));

    DynamicQuestTransitionResult result = ApplyDynamicQuestProgress(active, PlayerGuid(2), 501, 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::PlayerMismatch);
    REQUIRE(active.Progress == 0); // unchanged
}

TEST_CASE("ApplyDynamicQuestProgress rejects a zero progress-event identity", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance active = MakeActiveInstance(player);

    DynamicQuestTransitionResult result = ApplyDynamicQuestProgress(active, player, 0, 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::InvalidProgressEvent);
}

TEST_CASE("CompleteDynamicQuest rejects incomplete progress", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance active = MakeActiveInstance(player);

    DynamicQuestTransitionResult progress = ApplyDynamicQuestProgress(active, player, 501, 10000);
    REQUIRE(progress.IsAccepted());
    REQUIRE(progress.Instance->Progress < progress.Instance->RequiredCount);

    DynamicQuestTransitionResult result = CompleteDynamicQuest(*progress.Instance, 10000);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::ProgressIncomplete);
}

TEST_CASE("ExpireDynamicQuest rejects a not-yet-expired instance", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance offered = MakeOfferedInstance(10000);

    DynamicQuestTransitionResult result = ExpireDynamicQuest(offered, offered.ExpiresAtMs - 1);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::NotYetExpired);
}

// ---------------------------------------------------------------------
// Every terminal state rejects every further transition
// ---------------------------------------------------------------------

TEST_CASE("Every transition on a Completed instance is rejected as AlreadyTerminal", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance instance = MakeActiveInstance(player);
    for (uint64 eventId = 1; eventId <= instance.RequiredCount; ++eventId)
        instance = *ApplyDynamicQuestProgress(instance, player, eventId, 10000).Instance;
    instance = *CompleteDynamicQuest(instance, 10000).Instance;
    REQUIRE(instance.State == DynamicQuestState::Completed);

    REQUIRE(AcceptDynamicQuest(instance, player, 10000).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(ApplyDynamicQuestProgress(instance, player, 999, 10000).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(CompleteDynamicQuest(instance, 10000).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(FailDynamicQuest(instance, 10000).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(ExpireDynamicQuest(instance, instance.ExpiresAtMs).Reason == DynamicQuestRejectReason::AlreadyTerminal);
}

TEST_CASE("Every transition on a Failed instance is rejected as AlreadyTerminal", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance instance = MakeActiveInstance(player);
    instance = *FailDynamicQuest(instance, 10000).Instance;
    REQUIRE(instance.State == DynamicQuestState::Failed);

    REQUIRE(AcceptDynamicQuest(instance, player, 10000).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(ApplyDynamicQuestProgress(instance, player, 999, 10000).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(CompleteDynamicQuest(instance, 10000).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(FailDynamicQuest(instance, 10000).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(ExpireDynamicQuest(instance, instance.ExpiresAtMs).Reason == DynamicQuestRejectReason::AlreadyTerminal);
}

TEST_CASE("Every transition on an Expired instance is rejected as AlreadyTerminal", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance instance = MakeOfferedInstance(10000);
    instance = *ExpireDynamicQuest(instance, instance.ExpiresAtMs).Instance;
    REQUIRE(instance.State == DynamicQuestState::Expired);

    REQUIRE(AcceptDynamicQuest(instance, player, instance.ExpiresAtMs).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(ApplyDynamicQuestProgress(instance, player, 999, instance.ExpiresAtMs).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(CompleteDynamicQuest(instance, instance.ExpiresAtMs).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(FailDynamicQuest(instance, instance.ExpiresAtMs).Reason == DynamicQuestRejectReason::AlreadyTerminal);
    REQUIRE(ExpireDynamicQuest(instance, instance.ExpiresAtMs).Reason == DynamicQuestRejectReason::AlreadyTerminal);
}

TEST_CASE("Repeated Complete/Fail/Expire on the same terminal instance stays rejected", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance instance = MakeActiveInstance(player);
    instance = *FailDynamicQuest(instance, 10000).Instance;

    DynamicQuestTransitionResult second = FailDynamicQuest(instance, 10000);
    REQUIRE_FALSE(second.IsAccepted());
    REQUIRE(second.Reason == DynamicQuestRejectReason::AlreadyTerminal);

    DynamicQuestTransitionResult third = FailDynamicQuest(instance, 10000);
    REQUIRE_FALSE(third.IsAccepted());
    REQUIRE(third.Reason == DynamicQuestRejectReason::AlreadyTerminal);
}

// ---------------------------------------------------------------------
// Saturating, replay-guarded progress
// ---------------------------------------------------------------------

TEST_CASE("Progress saturates at RequiredCount and never exceeds it", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance instance = MakeActiveInstance(player);
    REQUIRE(instance.RequiredCount == 3);

    // 0 events applied yet.
    REQUIRE(instance.Progress == 0);

    // 1 event.
    instance = *ApplyDynamicQuestProgress(instance, player, 1, 10000).Instance;
    REQUIRE(instance.Progress == 1);

    // N events (N == RequiredCount).
    instance = *ApplyDynamicQuestProgress(instance, player, 2, 10000).Instance;
    instance = *ApplyDynamicQuestProgress(instance, player, 3, 10000).Instance;
    REQUIRE(instance.Progress == instance.RequiredCount);

    // N+1: a genuinely new (non-duplicate) event arriving once already
    // saturated is rejected outright - it is never appended to
    // ConsumedProgressEventIds, so that list (and the cost of checking
    // it) stays bounded by RequiredCount instead of growing without limit.
    DynamicQuestTransitionResult overshoot = ApplyDynamicQuestProgress(instance, player, 4, 10000);
    REQUIRE_FALSE(overshoot.IsAccepted());
    REQUIRE(overshoot.Reason == DynamicQuestRejectReason::ProgressAlreadyComplete);
    REQUIRE(instance.Progress == instance.RequiredCount); // unchanged
    REQUIRE(instance.ConsumedProgressEventIds.size() == instance.RequiredCount);
}

TEST_CASE("The same progress-event identity can never be counted twice", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance instance = MakeActiveInstance(player);

    DynamicQuestTransitionResult first = ApplyDynamicQuestProgress(instance, player, 501, 10000);
    REQUIRE(first.IsAccepted());
    REQUIRE(first.Instance->Progress == 1);

    DynamicQuestTransitionResult replay = ApplyDynamicQuestProgress(*first.Instance, player, 501, 10000);
    REQUIRE_FALSE(replay.IsAccepted());
    REQUIRE(replay.Reason == DynamicQuestRejectReason::DuplicateProgressEvent);
}

TEST_CASE("A duplicate progress-event identity is still rejected once Progress is already saturated", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance instance = MakeActiveInstance(player);
    for (uint64 eventId = 1; eventId <= instance.RequiredCount; ++eventId)
        instance = *ApplyDynamicQuestProgress(instance, player, eventId, 10000).Instance;
    REQUIRE(instance.Progress == instance.RequiredCount);

    // Event id 2 already contributed - replaying it must still be
    // recognized and rejected, not silently ignored as "already capped".
    DynamicQuestTransitionResult replay = ApplyDynamicQuestProgress(instance, player, 2, 10000);
    REQUIRE_FALSE(replay.IsAccepted());
    REQUIRE(replay.Reason == DynamicQuestRejectReason::DuplicateProgressEvent);
}

// ---------------------------------------------------------------------
// Expiry boundary
// ---------------------------------------------------------------------

TEST_CASE("IsDynamicQuestExpired follows the single now >= ExpiresAt rule at the boundary", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance instance = MakeOfferedInstance(10000);

    REQUIRE_FALSE(IsDynamicQuestExpired(instance, instance.ExpiresAtMs - 1)); // just before
    REQUIRE(IsDynamicQuestExpired(instance, instance.ExpiresAtMs));           // exactly at
    REQUIRE(IsDynamicQuestExpired(instance, instance.ExpiresAtMs + 1));       // just after
}

TEST_CASE("AcceptDynamicQuest rejects an Offered instance whose deadline has already passed", "[DynamicQuestLifecycle]")
{
    // State still literally says Offered (nobody called ExpireDynamicQuest
    // yet) but the time boundary must still fail closed.
    DynamicQuestInstance offered = MakeOfferedInstance(10000);

    DynamicQuestTransitionResult result = AcceptDynamicQuest(offered, PlayerGuid(1), offered.ExpiresAtMs);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::AlreadyExpired);
}

TEST_CASE("ApplyDynamicQuestProgress rejects an Active instance whose deadline has already passed", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance active = MakeActiveInstance(player, 10000);

    DynamicQuestTransitionResult result = ApplyDynamicQuestProgress(active, player, 501, active.ExpiresAtMs);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::AlreadyExpired);
}

TEST_CASE("CompleteDynamicQuest rejects a fully-progressed Active instance whose deadline has already passed", "[DynamicQuestLifecycle]")
{
    ObjectGuid player = PlayerGuid(1);
    DynamicQuestInstance instance = MakeActiveInstance(player, 10000);
    for (uint64 eventId = 1; eventId <= instance.RequiredCount; ++eventId)
        instance = *ApplyDynamicQuestProgress(instance, player, eventId, 10000).Instance;
    REQUIRE(instance.Progress == instance.RequiredCount);

    DynamicQuestTransitionResult result = CompleteDynamicQuest(instance, instance.ExpiresAtMs);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::AlreadyExpired);
}

TEST_CASE("FailDynamicQuest rejects an Active instance whose deadline has already passed", "[DynamicQuestLifecycle]")
{
    DynamicQuestInstance active = MakeActiveInstance(PlayerGuid(1), 10000);

    DynamicQuestTransitionResult result = FailDynamicQuest(active, active.ExpiresAtMs);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE(result.Reason == DynamicQuestRejectReason::AlreadyExpired);
}

// ---------------------------------------------------------------------
// ToString coverage
// ---------------------------------------------------------------------

TEST_CASE("ToString(DynamicQuestState) covers every enumerator", "[DynamicQuestLifecycle]")
{
    REQUIRE(std::string(ToString(DynamicQuestState::Offered)) == "OFFERED");
    REQUIRE(std::string(ToString(DynamicQuestState::Active)) == "ACTIVE");
    REQUIRE(std::string(ToString(DynamicQuestState::Completed)) == "COMPLETED");
    REQUIRE(std::string(ToString(DynamicQuestState::Failed)) == "FAILED");
    REQUIRE(std::string(ToString(DynamicQuestState::Expired)) == "EXPIRED");
}

TEST_CASE("ToString(DynamicQuestRejectReason) covers every enumerator", "[DynamicQuestLifecycle]")
{
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::NotAttempted)) == "NOT_ATTEMPTED");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::None)) == "NONE");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::AlreadyTerminal)) == "ALREADY_TERMINAL");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::InvalidTransition)) == "INVALID_TRANSITION");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::InvalidQuestId)) == "INVALID_QUEST_ID");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::QuestNotFound)) == "QUEST_NOT_FOUND");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::InvalidPlayer)) == "INVALID_PLAYER");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::PlayerMismatch)) == "PLAYER_MISMATCH");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::AlreadyExpired)) == "ALREADY_EXPIRED");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::NotYetExpired)) == "NOT_YET_EXPIRED");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::ProgressIncomplete)) == "PROGRESS_INCOMPLETE");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::ProgressAlreadyComplete)) == "PROGRESS_ALREADY_COMPLETE");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::InvalidProgressEvent)) == "INVALID_PROGRESS_EVENT");
    REQUIRE(std::string(ToString(DynamicQuestRejectReason::DuplicateProgressEvent)) == "DUPLICATE_PROGRESS_EVENT");
}

TEST_CASE("DynamicQuestTransitionResult defaults to rejected, never to an accepted None", "[DynamicQuestLifecycle]")
{
    DynamicQuestTransitionResult result;
    REQUIRE(result.Reason == DynamicQuestRejectReason::NotAttempted);
    REQUIRE(result.Reason != DynamicQuestRejectReason::None);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE_FALSE(result.Instance.has_value());
}
