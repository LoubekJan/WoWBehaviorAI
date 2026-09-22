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

#ifdef AIWORLD_FORMATION_STANDALONE_TEST
#include <cstdlib>
#include <iostream>
#define TEST_CASE(name, tags) static void RunFormationTests()
#define REQUIRE(value) do { if (!(value)) { std::cerr << "Failed at line " << __LINE__ << '\n'; std::exit(1); } ++checks; } while (false)
static unsigned checks = 0;
#else
#include "tc_catch2.h"
#endif

#include "Action/ActionSystem.h"
#include "Agent/AgentGroupIntentProjector.h"
#include "Agent/AgentGroupIntentSystem.h"
#include "Agent/GroupMemberFormation.h"
#include <limits>

TEST_CASE("Group members settle in distinct stable positions and approach targets honestly", "[AIWorld][Formation]")
{
    using namespace GroupMemberFormation;
    for (uint64 count = 2; count <= 5; ++count)
    {
        std::vector<AgentId> roster;
        for (uint64 id = 1; id <= count; ++id)
            roster.push_back(AgentId{ id });
        for (AgentId id : roster)
        {
            auto slot = GetSlot(id, roster, MaxRadius);
            REQUIRE(slot.has_value());
            REQUIRE(slot->Radius <= MaxRadius);
            for (AgentId other : roster)
            {
                if (other <= id)
                    continue;
                auto neighbor = GetSlot(other, roster, MaxRadius);
                float dx = slot->X - neighbor->X;
                float dy = slot->Y - neighbor->Y;
                REQUIRE(std::sqrt(dx * dx + dy * dy) >= MemberSpacing - 0.001f);
            }
            auto reordered = roster;
            std::reverse(reordered.begin(), reordered.end());
            reordered.push_back(id); // Duplicate observations do not create new slots.
            auto stable = GetSlot(id, reordered, MaxRadius);
            REQUIRE(stable->X == slot->X && stable->Y == slot->Y && stable->Angle == slot->Angle);
        }
    }
    REQUIRE(!GetSlot(AgentId{ 9 }, std::vector<AgentId>{ AgentId{ 1 } }, MaxRadius));
    REQUIRE(!GetSlot(AgentId{ 1 }, std::vector<AgentId>{ AgentId{ 1 } }, std::numeric_limits<float>::quiet_NaN()));
    REQUIRE(GetSlot(AgentId{ 1 }, std::vector<AgentId>{ AgentId{ 1 } }, MaxRadius)->Radius == 0.0f);

    AgentGroupRecord group;
    group.Id = GroupId{ 7 };
    group.ProfileId = CoalitionFormationProfileId::WolfLoose;
    AgentGroupCoordinationProfile profile;
    profile.ProfileId = group.ProfileId;
    profile.RegroupRadius = 20.0f;
    profile.RoamEnabled = true;
    profile.RoamIntervalMs = 15000;
    profile.RoamDistance = 10.0f;
    profile.RoamArrivalRadius = 5.0f;
    profile.MemberFormationRadius = MaxRadius;
    std::vector<CoalitionMemberObservation> members;
    for (uint64 id = 1; id <= 5; ++id)
    {
        group.Members.push_back({ AgentId{ id }, 0 });
        members.push_back({ AgentId{ id }, true, true, 0, 40.0f, 40.0f, 0.0f });
    }
    AgentGroupIntentSystem intents;
    AgentGroupIntentProjector projector;
    for (uint64 phase = 0; phase < 9; ++phase)
    {
        auto intent = intents.Evaluate(group, profile, members, phase * 15000);
        auto moves = projector.Project(intent, profile, members);
        REQUIRE(moves.size() == members.size());
        for (auto const& move : moves)
            REQUIRE(std::hypot(move.X, move.Y) <= profile.RoamDistance + 0.001f);
    }
    auto intent = intents.Evaluate(group, profile, members, 15000);
    auto moves = projector.Project(intent, profile, members);
    for (std::size_t i = 0; i < members.size(); ++i)
    {
        members[i].X = moves[i].X;
        members[i].Y = moves[i].Y;
        // Terrain height is resolved by dispatch, not the planar slot selector.
        members[i].Z = float(i) * 1.5f;
    }
    REQUIRE(intents.Evaluate(group, profile, members, 15000).Type == AgentGroupIntentType::None);
    REQUIRE(projector.Project(intent, profile, members).empty());

    // Members stacked at the shared center must move even inside the old 5yd radius.
    members[0].X = members[1].X = intent.X;
    members[0].Y = members[1].Y = intent.Y;
    REQUIRE(intents.Evaluate(group, profile, members, 15000).Type == AgentGroupIntentType::Roam);
    REQUIRE(projector.Project(intent, profile, members).size() == 2);
    auto before = GetSlot(AgentId{ 2 }, members, MaxRadius);
    members[0].Materialized = false;
    auto after = GetSlot(AgentId{ 2 }, members, MaxRadius);
    REQUIRE(before->X == after->X && before->Y == after->Y);
    REQUIRE(projector.Project(intent, profile, members).size() == 1);

    auto regroup = intent;
    regroup.Type = AgentGroupIntentType::Regroup;
    regroup.X = regroup.Y = 0.0f;
    members[1].X = members[1].Y = 40.0f;
    auto regroupMoves = projector.Project(regroup, profile, members);
    REQUIRE(regroupMoves.size() == 1);
    REQUIRE(std::fabs(regroupMoves[0].Y - after->Y) < 0.001f);

    // Other profiles keep the original center-target behavior until opted in.
    profile.MemberFormationRadius = 0.0f;
    members[1].X = members[1].Y = 40.0f;
    auto unspaced = projector.Project(intent, profile, members);
    REQUIRE(!unspaced.empty());
    REQUIRE(unspaced[0].X == intent.X && unspaced[0].Y == intent.Y);

    ActionSystem actions;
    ActionValidationContext context;
    context.Materialized = context.Alive = true;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.ActiveGoalType = GoalType::Hunt;
    context.ActiveGoalStartedAtMs = 100;
    context.TargetResolved = context.TargetAlive = context.TargetAttackable = true;
    context.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(721, 1);
    context.TargetEntry = 721;
    context.TargetX = 10.0f;
    ActionRequest approach;
    approach.Type = ActionType::MoveTo;
    approach.SourceGoal = GoalType::Hunt;
    approach.GoalStartedAtMs = 100;
    approach.Target = ActionTargetRef{ context.TargetGuid, 721 };
    approach.Destination = ActionPosition{ 0, 12.0f, 0.0f, 0.0f };
    REQUIRE(!actions.Validate(approach, context).Allowed);
    context.HuntApproachDestination = approach.Destination;
    REQUIRE(actions.Validate(approach, context).Allowed);
    approach.Destination->Y = 0.1f;
    REQUIRE(!actions.Validate(approach, context).Allowed);
    approach.Destination->Y = 0.0f;
    context.HuntApproachDestination->X = approach.Destination->X = 20.0f;
    REQUIRE(!actions.Validate(approach, context).Allowed);
    context.HuntApproachDestination.reset();
    approach.Destination->X = context.TargetX;
    REQUIRE(actions.Validate(approach, context).Allowed);

    approach.Type = ActionType::Attack;
    approach.Destination.reset();
    context.TargetWithinAttackRange = context.TargetInLineOfSight = true;
    approach.ChaseAngleRadians = 1.0f;
    REQUIRE(actions.Validate(approach, context).Allowed);
    approach.ChaseAngleRadians = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(!actions.Validate(approach, context).Allowed);
    approach.ChaseAngleRadians = TwoPi;
    REQUIRE(!actions.Validate(approach, context).Allowed);
    approach.ChaseAngleRadians = -0.1f;
    REQUIRE(!actions.Validate(approach, context).Allowed);
    approach.ChaseAngleRadians = 1.0f;
    approach.Target = ActionTargetRef{ ObjectGuid::Create<HighGuid::Player>(1), 0 };
    REQUIRE(!actions.Validate(approach, context).Allowed);

    approach.SourceGoal = GoalType::Defend;
    context.ActiveGoalType = GoalType::Defend;
    context.TargetGuid = context.DefenseThreatGuid = approach.Target->Guid;
    context.TargetEntry = 0;
    REQUIRE(actions.Validate(approach, context).Allowed);
    approach.ChaseAngleRadians = std::numeric_limits<float>::infinity();
    REQUIRE(!actions.Validate(approach, context).Allowed);
}

#ifdef AIWORLD_FORMATION_STANDALONE_TEST
int main()
{
    RunFormationTests();
    std::cout << "GroupMemberFormation: " << checks << " checks passed\n";
}
#endif
