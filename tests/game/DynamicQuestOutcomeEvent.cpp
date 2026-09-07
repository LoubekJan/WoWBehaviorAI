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

#include "Quest/DynamicQuestOutcomeEvent.h"
#include "Quest/DynamicQuestLifecycle.h"
#include "Inference/QuestProposal.h"

namespace
{
    ObjectGuid PlayerGuid(uint32 lowGuid)
    {
        return ObjectGuid::Create<HighGuid::Player>(lowGuid);
    }

    QuestProposal MakeValidProposal()
    {
        QuestProposal proposal;
        proposal.Giver.Value = 42;
        proposal.GiverRuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);
        proposal.SourceEventId = 9001;
        proposal.SourceCorrelationId = 9000;
        proposal.SourceEventType = WorldEventType::CreatureKilled;
        proposal.Objective = QuestObjectiveType::KillCreature;
        proposal.TargetToken = 1;
        proposal.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(2002, 1);
        proposal.TargetEntry = 2002;
        proposal.TargetMapId = 0;
        proposal.RequiredCount = 1;
        proposal.MaxRangeYards = 40.0f;
        proposal.ExpiryMs = 200000;
        proposal.RewardMoneyCopper = 75;
        proposal.Title = "Cull the wolves";
        proposal.Description = "Thin the wolf pack near the road.";
        return proposal;
    }

    DynamicQuestInstance MakeOfferedInstance(QuestProposal const& proposal, uint64 nowMs = 10000)
    {
        DynamicQuestTransitionResult result = OfferDynamicQuest(DynamicQuestId{777}, proposal, nowMs);
        REQUIRE(result.IsAccepted());
        return *result.Instance;
    }

    DynamicQuestInstance MakeActiveInstance(QuestProposal const& proposal, ObjectGuid player, uint64 nowMs = 10000)
    {
        DynamicQuestInstance offered = MakeOfferedInstance(proposal, nowMs);
        DynamicQuestTransitionResult result = AcceptDynamicQuest(offered, player, nowMs);
        REQUIRE(result.IsAccepted());
        return *result.Instance;
    }

    DynamicQuestInstance MakeCompletedInstance(QuestProposal const& proposal, ObjectGuid player, uint64 nowMs = 10000)
    {
        DynamicQuestInstance active = MakeActiveInstance(proposal, player, nowMs);
        DynamicQuestTransitionResult progress = ApplyDynamicQuestProgress(active, player, 1, nowMs);
        REQUIRE(progress.IsAccepted());
        DynamicQuestTransitionResult completed = CompleteDynamicQuest(*progress.Instance, nowMs);
        REQUIRE(completed.IsAccepted());
        return *completed.Instance;
    }

    DynamicQuestInstance MakeFailedInstance(QuestProposal const& proposal, ObjectGuid player, uint64 nowMs = 10000)
    {
        DynamicQuestInstance active = MakeActiveInstance(proposal, player, nowMs);
        DynamicQuestTransitionResult failed = FailDynamicQuest(active, nowMs);
        REQUIRE(failed.IsAccepted());
        return *failed.Instance;
    }

    DynamicQuestInstance MakeExpiredFromOfferedInstance(QuestProposal const& proposal, uint64 offerNowMs, uint64 expireNowMs)
    {
        DynamicQuestInstance offered = MakeOfferedInstance(proposal, offerNowMs);
        DynamicQuestTransitionResult expired = ExpireDynamicQuest(offered, expireNowMs);
        REQUIRE(expired.IsAccepted());
        return *expired.Instance;
    }

    DynamicQuestInstance MakeExpiredFromActiveInstance(QuestProposal const& proposal, ObjectGuid player, uint64 offerNowMs, uint64 expireNowMs)
    {
        DynamicQuestInstance active = MakeActiveInstance(proposal, player, offerNowMs);
        DynamicQuestTransitionResult expired = ExpireDynamicQuest(active, expireNowMs);
        REQUIRE(expired.IsAccepted());
        return *expired.Instance;
    }

    WorldEventLocation MakeLocation()
    {
        WorldEventLocation location;
        location.MapId = 0;
        location.X = 1.0f;
        location.Y = 2.0f;
        location.Z = 3.0f;
        return location;
    }
}

TEST_CASE("BuildDynamicQuestOutcomeWorldEvent maps each terminal state to its own WorldEventType", "[DynamicQuestOutcomeEvent]")
{
    QuestProposal proposal = MakeValidProposal();
    ObjectGuid player = PlayerGuid(1);
    WorldEventLocation location = MakeLocation();

    SECTION("Completed")
    {
        DynamicQuestInstance instance = MakeCompletedInstance(proposal, player);
        std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, location, 20000);
        REQUIRE(event.has_value());
        REQUIRE(event->Type == WorldEventType::DynamicQuestCompleted);
    }

    SECTION("Failed")
    {
        DynamicQuestInstance instance = MakeFailedInstance(proposal, player);
        std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, location, 20000);
        REQUIRE(event.has_value());
        REQUIRE(event->Type == WorldEventType::DynamicQuestFailed);
    }

    SECTION("Expired")
    {
        DynamicQuestInstance instance = MakeExpiredFromActiveInstance(proposal, player, 10000, 999999999);
        std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, location, 20000);
        REQUIRE(event.has_value());
        REQUIRE(event->Type == WorldEventType::DynamicQuestExpired);
    }
}

TEST_CASE("BuildDynamicQuestOutcomeWorldEvent rejects non-terminal states", "[DynamicQuestOutcomeEvent]")
{
    QuestProposal proposal = MakeValidProposal();
    ObjectGuid player = PlayerGuid(1);
    WorldEventLocation location = MakeLocation();

    SECTION("Offered")
    {
        DynamicQuestInstance instance = MakeOfferedInstance(proposal);
        std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, location, 20000);
        REQUIRE_FALSE(event.has_value());
    }

    SECTION("Active")
    {
        DynamicQuestInstance instance = MakeActiveInstance(proposal, player);
        std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, location, 20000);
        REQUIRE_FALSE(event.has_value());
    }
}

TEST_CASE("BuildDynamicQuestOutcomeWorldEvent always leaves EventId at 0 - only EventBus::Publish() assigns one", "[DynamicQuestOutcomeEvent]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestInstance instance = MakeCompletedInstance(proposal, PlayerGuid(1));

    std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, MakeLocation(), 20000);
    REQUIRE(event.has_value());
    REQUIRE(event->EventId == 0);
}

TEST_CASE("BuildDynamicQuestOutcomeWorldEvent preserves SourceEventId/SourceCorrelationId all the way from provenance through proposal and the offered instance into CauseEventId/CorrelationId", "[DynamicQuestOutcomeEvent]")
{
    QuestProposal proposal = MakeValidProposal();
    REQUIRE(proposal.SourceEventId == 9001);
    REQUIRE(proposal.SourceCorrelationId == 9000);

    DynamicQuestInstance offered = MakeOfferedInstance(proposal);
    REQUIRE(offered.SourceEventId == proposal.SourceEventId);
    REQUIRE(offered.SourceCorrelationId == proposal.SourceCorrelationId);
    REQUIRE(offered.SourceEventType == proposal.SourceEventType);

    DynamicQuestInstance completed = MakeCompletedInstance(proposal, PlayerGuid(1));
    REQUIRE(completed.SourceEventId == proposal.SourceEventId);
    REQUIRE(completed.SourceCorrelationId == proposal.SourceCorrelationId);

    std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(completed, MakeLocation(), 20000);
    REQUIRE(event.has_value());
    REQUIRE(event->CauseEventId == proposal.SourceEventId);
    REQUIRE(event->CorrelationId == proposal.SourceCorrelationId);
}

TEST_CASE("BuildDynamicQuestOutcomeWorldEvent sets Actor to the accepting player only for Completed - the one outcome the player actually caused", "[DynamicQuestOutcomeEvent]")
{
    QuestProposal proposal = MakeValidProposal();
    ObjectGuid player = PlayerGuid(7);

    SECTION("Completed")
    {
        DynamicQuestInstance instance = MakeCompletedInstance(proposal, player);
        std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, MakeLocation(), 20000);
        REQUIRE(event.has_value());
        REQUIRE(event->Actor.Guid == player);
    }
}

TEST_CASE("BuildDynamicQuestOutcomeWorldEvent leaves Actor empty for Failed and Expired even when a player had accepted the quest", "[DynamicQuestOutcomeEvent]")
{
    // Failed/Expired both happen TO the quest (server force-fail/replay
    // containment, or deadline maintenance) - never something the
    // accepting player did, so Actor must never read as "the player"
    // for these, even though AcceptedByPlayerGuid is non-empty.
    QuestProposal proposal = MakeValidProposal();
    ObjectGuid player = PlayerGuid(7);

    SECTION("Failed")
    {
        DynamicQuestInstance instance = MakeFailedInstance(proposal, player);
        REQUIRE_FALSE(instance.AcceptedByPlayerGuid.IsEmpty());
        std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, MakeLocation(), 20000);
        REQUIRE(event.has_value());
        REQUIRE(event->Actor.Guid.IsEmpty());
    }

    SECTION("Expired from Active")
    {
        DynamicQuestInstance instance = MakeExpiredFromActiveInstance(proposal, player, 10000, 999999999);
        REQUIRE_FALSE(instance.AcceptedByPlayerGuid.IsEmpty());
        std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, MakeLocation(), 20000);
        REQUIRE(event.has_value());
        REQUIRE(event->Actor.Guid.IsEmpty());
    }
}

TEST_CASE("BuildDynamicQuestOutcomeWorldEvent leaves Actor empty for an Offered->Expired instance nobody ever accepted", "[DynamicQuestOutcomeEvent]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestInstance instance = MakeExpiredFromOfferedInstance(proposal, 10000, 999999999);
    REQUIRE(instance.AcceptedByPlayerGuid.IsEmpty());

    std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, MakeLocation(), 20000);
    REQUIRE(event.has_value());
    REQUIRE(event->Actor.Guid.IsEmpty());
}

TEST_CASE("BuildDynamicQuestOutcomeWorldEvent sets Target to the quest's giver, never an authorization by itself", "[DynamicQuestOutcomeEvent]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestInstance instance = MakeCompletedInstance(proposal, PlayerGuid(1));

    std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, MakeLocation(), 20000);
    REQUIRE(event.has_value());
    REQUIRE(event->Target.Agent.Value == proposal.Giver.Value);
    REQUIRE(event->Target.Guid == proposal.GiverRuntimeGuid);
}

TEST_CASE("BuildDynamicQuestOutcomeWorldEvent passes Location and OccurredAtMs through as given by the caller", "[DynamicQuestOutcomeEvent]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestInstance instance = MakeCompletedInstance(proposal, PlayerGuid(1));
    WorldEventLocation location = MakeLocation();

    std::optional<WorldEvent> event = BuildDynamicQuestOutcomeWorldEvent(instance, location, 54321);
    REQUIRE(event.has_value());
    REQUIRE(event->Location.MapId == location.MapId);
    REQUIRE(event->Location.X == location.X);
    REQUIRE(event->Location.Y == location.Y);
    REQUIRE(event->Location.Z == location.Z);
    REQUIRE(event->OccurredAtMs == 54321);
}
