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
#include "Action/ActionSystem.h"
#include "Agent/LivingRolePolicy.h"
#include "Agent/LivingHuntPolicy.h"
#include "Agent/LivingReturnPolicy.h"
#include "Agent/LivingForagePolicy.h"
#include "Agent/GroupMemberFormation.h"
#include "Agent/AgentRecord.h"
#include "DBCStructure.h"
#include "MovementDefines.h"
#include "MovementPathBounds.h"
#include "Position.h"
#include <limits>
#include <string>

TEST_CASE("Defias social allies can assist despite mutually neutral native templates", "[AIWorld][LivingRole]")
{
    // FT 2300 is the accepted clone of native FT 7: same zero reaction
    // masks/lists, a different Faction id. Neutral does not mean friendly.
    FactionTemplateEntry thug{};
    thug.ID = 2300;
    thug.Faction = 1201;
    REQUIRE(!thug.IsFriendlyTo(thug));
    REQUIRE(!thug.IsHostileTo(thug));
    REQUIRE(LivingRolePolicy::CanAssistAlly(WorldFactions::DefiasBrotherhood,
        WorldFactions::DefiasBrotherhood, thug.IsHostileTo(thug)));
    REQUIRE(!LivingRolePolicy::CanAssistAlly(WorldFactions::Unaffiliated, WorldFactions::Unaffiliated, false));
    REQUIRE(!LivingRolePolicy::CanAssistAlly(WorldFactions::DefiasBrotherhood, WorldFactions::StormwindAlliance, false));
    REQUIRE(!LivingRolePolicy::CanAssistAlly(WorldFactions::DefiasBrotherhood, WorldFactions::DefiasBrotherhood, true));
}

TEST_CASE("Forest spiders can hunt neutral fauna without granting attacks on other roles", "[AIWorld][LivingRole]")
{
    using namespace LivingRolePolicy;
    // Native DBC rows 22 (Forest Spider) and 31 (Deer/Fawn/Rabbit).
    FactionTemplateEntry spider{22, 22, 0, 8, 0, 1, {0, 0, 0, 0}, {22, 0, 0, 0}};
    FactionTemplateEntry fauna{31, 28, 1024, 0, 0, 0, {973, 0, 0, 0}, {148, 28, 0, 0}};
    REQUIRE(!spider.IsHostileTo(fauna));
    REQUIRE(!fauna.IsHostileTo(spider));
    REQUIRE(!spider.IsFriendlyTo(fauna));
    REQUIRE(!fauna.IsFriendlyTo(spider));
    REQUIRE(CanHuntNeutralPrey(Role::Predator, AgentType::Prey, true));
    REQUIRE(!CanHuntNeutralPrey(Role::Predator, AgentType::Prey, false));
    for (AgentType type : {AgentType::Civilian, AgentType::Merchant, AgentType::Guard,
        AgentType::Predator, AgentType::Combatant, AgentType::Unclassified})
        REQUIRE(!CanHuntNeutralPrey(Role::Predator, type, true));
    for (Role role : {Role::None, Role::Service, Role::Prey, Role::Civilian, Role::Worker,
        Role::Traveler, Role::Guard, Role::Combatant})
        REQUIRE(!CanHuntNeutralPrey(role, AgentType::Prey, true));
}

TEST_CASE("Materialization cleanup discards role attempts without claiming newer work", "[AIWorld][LivingRole]")
{
    AgentRecord record;
    record.LivingRole.RuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(30, 1);
    record.LivingRole.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(721, 2);
    record.LivingRole.CurrentPhase = LivingRoleState::Phase::Hunting;
    record.LivingRole.SourceGoal = GoalType::PredatorHunt;
    record.LivingRole.StartedAtMs = 100;
    record.LivingRole.LastHuntStatus = "HUNT_PATH_BLOCKED";
    record.LivingRole.LastHuntEnd = "HUNT_PATH_BLOCKED";
    record.LivingRole.LastHuntTargetGuid = record.LivingRole.TargetGuid;
    record.LivingRole.UnreachablePreyGuid = record.LivingRole.TargetGuid;
    record.LivingRole.UnreachablePreyUntilMs = 30100;
    record.ActiveGoalState.emplace();
    record.ActiveGoalState->Type = GoalType::PredatorHunt;
    record.ActiveGoalState->StartedAtMs = 100;
    record.ActiveActionState.emplace();
    record.ActiveActionState->Type = ActionType::Attack;
    record.ActiveActionState->SourceGoal = GoalType::PredatorHunt;
    record.ActiveActionState->GoalStartedAtMs = 100;
    bool preserve = false;
    SECTION("the unloaded incarnation owns both attempts") { }
    SECTION("a newer attempt of the same goal must survive")
    {
        preserve = true;
        record.ActiveGoalState->StartedAtMs = record.ActiveActionState->GoalStartedAtMs = 101;
    }
    SECTION("a different goal at the same timestamp must survive")
    {
        preserve = true;
        record.ActiveGoalState->Type = record.ActiveActionState->SourceGoal = GoalType::Defend;
    }
    record.ResetLivingRoleActivity();
    REQUIRE(record.ActiveGoalState.has_value() == preserve);
    REQUIRE(record.ActiveActionState.has_value() == preserve);
    REQUIRE(record.LivingRole.RuntimeGuid.IsEmpty());
    REQUIRE(record.LivingRole.TargetGuid.IsEmpty());
    REQUIRE(record.LivingRole.CurrentPhase == LivingRoleState::Phase::Idle);
    REQUIRE(record.LivingRole.LastHuntTargetGuid.IsEmpty());
    REQUIRE(record.LivingRole.UnreachablePreyGuid.IsEmpty());
    REQUIRE(record.LivingRole.UnreachablePreyUntilMs == 0);
    REQUIRE(std::string(record.LivingRole.LastHuntEnd) == "NONE");
}

TEST_CASE("Living roles only cover controlled permanent Elwynn agents", "[AIWorld][LivingRole]")
{
    using namespace LivingRolePolicy;
    REQUIRE(InScope(true, AgentControlMode::AIWorldControlled, 0, 12, SpawnParticipationMode::FullAgent));
    REQUIRE(InScope(true, AgentControlMode::AIWorldControlled, 0, 12, SpawnParticipationMode::LightweightBackground));
    REQUIRE(!InScope(false, AgentControlMode::AIWorldControlled, 0, 12, SpawnParticipationMode::FullAgent));
    REQUIRE(!InScope(true, AgentControlMode::ObserveOnly, 0, 12, SpawnParticipationMode::FullAgent));
    REQUIRE(!InScope(true, AgentControlMode::AIWorldControlled, 1, 12, SpawnParticipationMode::FullAgent));
    REQUIRE(!InScope(true, AgentControlMode::AIWorldControlled, 0, 14, SpawnParticipationMode::FullAgent));
    REQUIRE(!InScope(true, AgentControlMode::AIWorldControlled, 0, 12, SpawnParticipationMode::VanillaOnly));
    REQUIRE(!InScope(true, AgentControlMode::AIWorldControlled, 0, 12, SpawnParticipationMode::Excluded));
    REQUIRE(!InScope(true, AgentControlMode::AIWorldControlled, 0, 12, SpawnParticipationMode(255)));
}

TEST_CASE("Living roles retain distinct ecology professions and service posts", "[AIWorld][LivingRole]")
{
    using namespace LivingRolePolicy;
    REQUIRE(Resolve(AgentType::Predator, 30, false) == Role::Predator);
    REQUIRE(Resolve(AgentType::Prey, 721, false) == Role::Prey);
    REQUIRE(Resolve(AgentType::Guard, 1423, false) == Role::Guard);
    REQUIRE(Resolve(AgentType::Combatant, 38, false) == Role::Combatant);
    REQUIRE(Resolve(AgentType::Civilian, 1975, false) == Role::Worker);
    REQUIRE(Resolve(AgentType::Civilian, 14390, false) == Role::Traveler);
    REQUIRE(Resolve(AgentType::Civilian, 123, false) == Role::Civilian);
    REQUIRE(Resolve(AgentType::Civilian, 123, true) == Role::Service);
    REQUIRE(Resolve(AgentType::Merchant, 123, false) == Role::Service);
    REQUIRE(Resolve(AgentType::Unclassified, 123, true) == Role::None);
    REQUIRE(Resolve(AgentType(255), 123, false) == Role::None);
    REQUIRE(Resolve(AgentType(255), 123, true) == Role::None);
    REQUIRE(!Allows(Role(255), Activity::Look));
    REQUIRE(IdleActivity(Role(255), 1, true, 1, 1) == Activity::None);
    REQUIRE(RoamRadius(Role::Service) == 0.0f);
    REQUIRE(IdleActivity(Role::Prey, 0, true, 0.7f, 0) == Activity::Graze);
    REQUIRE(IdleActivity(Role::Worker, 1, true, 0, 0) == Activity::Work);
    REQUIRE(IdleActivity(Role::Worker, 3, false, 0, 0) == Activity::Rest);
    REQUIRE(IdleActivity(Role::Service, 0, false, 0.7f, 1) == Activity::Eat);
    REQUIRE(!HelpsAllies(Role::Civilian));
    REQUIRE(!HelpsAllies(Role::Predator));
    REQUIRE(HelpsAllies(Role::Guard));
    REQUIRE(HelpsAllies(Role::Combatant));
    for (Role role : { Role::Predator, Role::Prey, Role::Guard, Role::Combatant, Role::Civilian, Role::Worker, Role::Traveler, Role::Service })
        for (uint64 phase = 0; phase < 6; ++phase)
            for (bool workHours : { false, true })
                REQUIRE(Allows(role, IdleActivity(role, phase, workHours, 0.2f, 0.1f)));
    REQUIRE(ShouldFlee(Role::Prey, 0.0f, false));
    REQUIRE(ShouldFlee(Role::Civilian, 0.0f, false));
    REQUIRE(!ShouldFlee(Role::Combatant, 0.2f, false));
    REQUIRE(ShouldFlee(Role::Combatant, 0.8f, false));
    REQUIRE(!ShouldFlee(Role::Guard, 0.7f, false));
    REQUIRE(ShouldFlee(Role::Guard, 0.9f, false));
    REQUIRE(ShouldFlee(Role::Combatant, 0.6f, true));
    REQUIRE(!ShouldFlee(Role::Combatant, 0.4f, true));
    REQUIRE(ShouldFlee(Role::Guard, std::numeric_limits<float>::quiet_NaN(), false));
}

TEST_CASE("Ambient actions require an authorized role safe state and matching activity", "[AIWorld][LivingRole]")
{
    using namespace LivingRolePolicy;
    ActionSystem actions;
    ActionRequest request;
    request.Type = ActionType::Ambient;
    request.SourceGoal = GoalType::LocalActivity;
    request.GoalStartedAtMs = 100;
    request.AmbientActivity = Activity::Graze;
    ActionValidationContext context;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.Materialized = context.Alive = context.LivingRoleAllowed = true;
    context.LivingRoleZoneId = 12;
    context.LivingRole = Role::Prey;
    context.ExpectedAmbientActivity = Activity::Graze;
    context.ActiveGoalType = GoalType::LocalActivity;
    context.ActiveGoalStartedAtMs = 100;
    REQUIRE(actions.Validate(request, context).Allowed);
    SECTION("ownership") { context.ControlMode = AgentControlMode::ObserveOnly; }
    SECTION("scope") { context.LivingRoleAllowed = false; }
    SECTION("outside Elwynn") { context.LivingRoleZoneId = 14; }
    SECTION("outside map zero") { context.MapId = 1; }
    SECTION("alive") { context.Alive = false; }
    SECTION("materialization") { context.Materialized = false; }
    SECTION("wrong role") { context.LivingRole = Role::Guard; }
    SECTION("unknown role") { context.LivingRole = Role(255); }
    SECTION("wrong activity") { context.ExpectedAmbientActivity = Activity::Work; }
    SECTION("combat interrupts") { context.InCombat = true; }
    SECTION("movement interrupts") { context.HasActiveMovement = true; }
    SECTION("stale attempt") { request.GoalStartedAtMs = 99; }
    SECTION("rest cannot replace an unrelated posture")
    {
        request.AmbientActivity = context.ExpectedAmbientActivity = Activity::Rest;
        context.WildlifeRestAllowed = false;
    }
    REQUIRE(!actions.Validate(request, context).Allowed);
}

TEST_CASE("New attackers reserve separate world bearings before reaching the target", "[AIWorld][LivingRole]")
{
    std::vector<float> occupied;
    for (unsigned count = 0; count < 5; ++count)
    {
        float bearing = LivingRolePolicy::FreeChaseBearing(0.0f, occupied);
        for (float other : occupied)
        {
            float dx = 3.0f * (std::cos(bearing) - std::cos(other));
            float dy = 3.0f * (std::sin(bearing) - std::sin(other));
            REQUIRE(std::hypot(dx, dy) > 2.0f);
        }
        REQUIRE(bearing >= 0.0f);
        REQUIRE(bearing < 6.283186f);
        occupied.push_back(bearing);
    }
}

TEST_CASE("Predators only hunt resolved classified attackable creature prey", "[AIWorld][LivingRole]")
{
    using namespace LivingRolePolicy;
    ActionSystem actions;
    ActionRequest request;
    request.Type = ActionType::Attack;
    request.SourceGoal = GoalType::PredatorHunt;
    request.GoalStartedAtMs = 100;
    request.Target = ActionTargetRef{ ObjectGuid::Create<HighGuid::Unit>(721, 1), 721 };
    ActionValidationContext context;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.Materialized = context.Alive = context.LivingRoleAllowed = true;
    context.LivingRoleZoneId = 12;
    context.LivingRole = Role::Predator;
    context.ActiveGoalType = GoalType::PredatorHunt;
    context.ActiveGoalStartedAtMs = 100;
    context.TargetResolved = context.TargetAlive = context.TargetAttackable = context.TargetIsRolePrey = true;
    context.TargetWithinAttackRange = context.TargetInLineOfSight = true;
    context.TargetGuid = request.Target->Guid;
    context.TargetEntry = 721;
    REQUIRE(actions.Validate(request, context).Allowed);
    SECTION("scope") { context.LivingRoleAllowed = false; }
    SECTION("outside Elwynn") { context.LivingRoleZoneId = 14; }
    SECTION("outside map zero") { context.MapId = 1; }
    SECTION("prey classification") { context.TargetIsRolePrey = false; }
    SECTION("wrong hunter role") { context.LivingRole = Role::Combatant; }
    SECTION("friendly prey") { context.TargetAttackable = false; }
    SECTION("dead prey") { context.TargetAlive = false; }
    SECTION("despawned prey") { context.TargetResolved = false; }
    SECTION("range") { context.TargetWithinAttackRange = false; }
    SECTION("line of sight") { context.TargetInLineOfSight = false; }
    SECTION("map") { context.TargetMapId = 1; }
    SECTION("unrelated movement") { context.HasActiveMovement = true; }
    SECTION("other victim") { context.ActorCurrentVictimGuid = ObjectGuid::Create<HighGuid::Unit>(721, 2); }
    SECTION("individual prey pursuit must not substitute a formation slot")
    {
        request.ChaseAngleRadians = 0.5f;
        REQUIRE(actions.Validate(request, context).Reason == ActionRejectReason::InvalidChaseAngle);
    }
    SECTION("players are never food")
    {
        request.Target = ActionTargetRef{ ObjectGuid::Create<HighGuid::Player>(1), 0 };
        context.TargetGuid = request.Target->Guid;
        context.TargetEntry = 0;
    }
    REQUIRE(!actions.Validate(request, context).Allowed);
}

TEST_CASE("Local movement cannot escape action range and combat gates", "[AIWorld][LivingRole]")
{
    ActionSystem actions;
    ActionRequest request;
    request.Type = ActionType::MoveTo;
    request.SourceGoal = GoalType::LocalActivity;
    request.Destination = ActionPosition{ 0, 12.0f, 0.0f, 0.0f };
    ActionValidationContext context;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.Materialized = context.Alive = context.LivingRoleAllowed = true;
    context.LivingRoleZoneId = 12;
    context.LivingRole = LivingRolePolicy::Role::Civilian;
    context.ActiveGoalType = GoalType::LocalActivity;
    REQUIRE(actions.Validate(request, context).Allowed);
    SECTION("combat") { context.InCombat = true; }
    SECTION("other movement") { context.HasActiveMovement = true; }
    SECTION("scope") { context.LivingRoleAllowed = false; }
    SECTION("outside Elwynn") { context.LivingRoleZoneId = 14; }
    SECTION("outside map zero") { context.MapId = 1; }
    SECTION("range") { request.Destination->X = 50.0f; }
    SECTION("finite") { request.Destination->X = std::numeric_limits<float>::quiet_NaN(); }
    REQUIRE(!actions.Validate(request, context).Allowed);
}

TEST_CASE("Return steps fit the path gate at large Elwynn coordinates", "[AIWorld][LivingRole][Return]")
{
    // Reproduce spawn 80700's home and the reported ~65-yard excursion.
    // The old exact 30-yard projection rounds to 30.000099 at this heading,
    // so CheckRolePath rejects the same unchanged destination on every retry.
    ActionPosition const home{ 0, -9588.36f, -12.2227f, 61.6865f };
    ActionPosition const stranded{ 0, -9523.34960938f, -8.81562901f, home.Z };
    float distance = std::hypot(home.X - stranded.X, home.Y - stranded.Y);
    float oldX = stranded.X + (home.X - stranded.X) * 30.0f / distance;
    float oldY = stranded.Y + (home.Y - stranded.Y) * 30.0f / distance;
    REQUIRE(Position(stranded.X, stranded.Y, stranded.Z).GetExactDist2d(oldX, oldY) > 30.0f);

    ActionSystem actions;
    ActionValidationContext context;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.Materialized = context.Alive = context.LivingRoleAllowed = true;
    context.LivingRoleZoneId = 12;
    context.LivingRole = LivingRolePolicy::Role::Predator;
    context.ActiveGoalType = GoalType::LocalActivity;
    // Sweep directions, including crossing zero and large negative positions.
    // Each proposed step must both fit the planner gate and be dispatchable.
    bool allStepsValid = true;
    for (int degrees = 0; degrees < 360; ++degrees)
    {
        float angle = degrees * GroupMemberFormation::TwoPi / 360.0f;
        ActionPosition from{ 0, home.X + 65.1f * std::cos(angle), home.Y + 65.1f * std::sin(angle), home.Z - 4.0f };
        auto step = LivingReturnPolicy::PathStep({from, home});
        if (!step) { allStepsValid = false; break; }
        context.X = from.X; context.Y = from.Y; context.Z = from.Z;
        ActionRequest request;
        request.Type = ActionType::MoveTo;
        request.SourceGoal = GoalType::LocalActivity;
        request.Destination = step;
        allStepsValid = allStepsValid && Position(from.X, from.Y, from.Z).GetExactDist2d(step->X, step->Y) < 30.0f &&
            std::hypot(step->X - home.X, step->Y - home.Y) < 65.1f && actions.Validate(request, context).Allowed;
    }
    REQUIRE(allStepsValid);
}

TEST_CASE("Returning follows bends and heights and terminates near home", "[AIWorld][LivingRole][Return]")
{
    using LivingReturnPolicy::PathStep;
    // Following this detour first moves sideways; a straight chord would
    // cut across the obstacle that caused the navigation bend.
    std::vector<ActionPosition> route{ {0, 0, 0, 0}, {0, 0, 20, 0}, {0, 60, 20, 0}, {0, 60, 0, 0} };
    auto step = PathStep(route);
    REQUIRE(step.has_value());
    REQUIRE(step->X == Approx(8.0f));
    REQUIRE(step->Y == Approx(20.0f));
    auto nearer = PathStep(route, 7.0f);
    REQUIRE(nearer.has_value());
    REQUIRE(nearer->X == 0.0f);
    REQUIRE(nearer->Y == Approx(7.0f));
    auto slope = PathStep({{0, 0, 0, 0}, {0, 30, 0, 40}});
    REQUIRE(slope.has_value());
    REQUIRE(slope->X == Approx(16.8f));
    REQUIRE(slope->Z == Approx(22.4f));
    auto home = PathStep({{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 3, 4, 2}});
    REQUIRE(home.has_value());
    REQUIRE(home->X == 3.0f);
    REQUIRE(home->Y == 4.0f);
    REQUIRE(home->Z == 2.0f);

    ActionPosition actor{0, -9523.26f, -12.2227f, 61.6865f};
    ActionPosition const origin{0, -9588.36f, -12.2227f, 61.6865f};
    for (int leg = 0; leg < 3; ++leg)
    {
        auto next = PathStep({actor, origin});
        REQUIRE(next.has_value());
        actor = *next;
    }
    REQUIRE(actor.X == origin.X);
    REQUIRE(actor.Y == origin.Y);
    REQUIRE(actor.Z == origin.Z);
}

TEST_CASE("Invalid or motionless return paths cannot authorize a step", "[AIWorld][LivingRole][Return]")
{
    using LivingReturnPolicy::PathStep;
    REQUIRE(!PathStep({}));
    REQUIRE(!PathStep({{0, 0, 0, 0}}));
    REQUIRE(!PathStep({{0, 0, 0, 0}, {0, 0, 0, 0}}));
    REQUIRE(!PathStep({{0, 0, 0, 0}, {1, 10, 0, 0}}));
    REQUIRE(!PathStep({{0, 0, 0, 0}, {0, 10, 0, std::numeric_limits<float>::quiet_NaN()}}));
    for (float budget : {0.0f, -1.0f, 30.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        REQUIRE(!PathStep({{0, 0, 0, 0}, {0, 60, 0, 0}}, budget));
}

TEST_CASE("Individual hunt motion detects engine failures without cancelling melee or roots", "[AIWorld][LivingRole]")
{
    using namespace LivingHuntPolicy;
    // A live victim alone is insufficient: the old planner retained the hunt
    // until timeout even when the chase reported an unreachable destination.
    REQUIRE(EvaluateMotion(true, true, false, true, false) == MotionState::PathBlocked);
    REQUIRE(!CanContinue(EvaluateMotion(true, true, false, true, false)));
    REQUIRE(EvaluateMotion(true, false, false, false, false) == MotionState::ChaseMissing);
    REQUIRE(!CanContinue(EvaluateMotion(true, false, false, false, false)));
    REQUIRE(EvaluateMotion(false, false, false, true, false) == MotionState::VictimLost);
    REQUIRE(!CanContinue(EvaluateMotion(false, false, false, true, false)));
    REQUIRE(EvaluateMotion(true, false, false, true, false) == MotionState::Pursuing);
    REQUIRE(CanContinue(EvaluateMotion(true, false, false, true, false)));
    // Stopping in reach is successful pursuit, not a stuck chase. Root/stun
    // does not blacklist otherwise valid prey, even with a stale path flag.
    REQUIRE(EvaluateMotion(true, false, false, false, true) == MotionState::InMeleeRange);
    REQUIRE(CanContinue(EvaluateMotion(true, false, false, false, true)));
    REQUIRE(EvaluateMotion(true, true, true, true, false) == MotionState::TemporarilyBlocked);
    REQUIRE(CanContinue(EvaluateMotion(true, true, true, true, false)));
    REQUIRE(std::string(ToString(MotionState::PathBlocked)) == "HUNT_PATH_BLOCKED");
    REQUIRE(std::string(ToString(MotionState::ChaseMissing)) == "HUNT_CHASE_MISSING");
}

TEST_CASE("Direct predator pursuit preserves formation slots for defense and wolf hunts", "[AIWorld][LivingRole]")
{
    REQUIRE(!LivingHuntPolicy::UsesFormationBearing(GoalType::PredatorHunt));
    REQUIRE(LivingHuntPolicy::UsesFormationBearing(GoalType::Defend));
    REQUIRE(LivingHuntPolicy::UsesFormationBearing(GoalType::Hunt));
    ActionSystem actions;
    ActionRequest request;
    request.Type = ActionType::Attack;
    request.SourceGoal = GoalType::Defend;
    request.Target = ActionTargetRef{ ObjectGuid::Create<HighGuid::Unit>(721, 1), 721 };
    request.ChaseAngleRadians = 0.5f;
    ActionValidationContext context;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.Materialized = context.Alive = true;
    context.ActiveGoalType = request.SourceGoal;
    context.TargetResolved = context.TargetAlive = context.TargetAttackable = true;
    context.TargetWithinAttackRange = context.TargetInLineOfSight = true;
    context.TargetGuid = context.DefenseThreatGuid = request.Target->Guid;
    context.TargetEntry = request.Target->Entry;
    REQUIRE(actions.Validate(request, context).Allowed);
    request.SourceGoal = GoalType::Hunt;
    context.ActiveGoalType = request.SourceGoal;
    REQUIRE(actions.Validate(request, context).Allowed);
}

TEST_CASE("Predator sprint can close an equal-speed gap within a finite territory", "[AIWorld][LivingRole]")
{
    // Straight unobstructed pursuit, both actors initially running at 7 yd/s.
    // The prey already has a 14-yard head start. This is a balance check,
    // not a replacement for testing pathfinding and combat in the world.
    auto catches = [](ChaseSpeedBoost boost, float leash, float preySpeed)
    {
        float hunter = 0.0f, prey = 14.0f;
        for (uint32 time = 0; time < 45000; time += 50)
        {
            boost.Update(50);
            hunter += 7.0f * boost.GetMultiplier() * 0.05f;
            prey += preySpeed * 0.05f;
            if (hunter > leash || prey - hunter > 30.0f)
                return false;
            if (prey - hunter <= 3.0f)
                return true;
        }
        return false;
    };
    using namespace LivingHuntPolicy;
    REQUIRE(!catches({}, LeashDistance, 7.0f));
    REQUIRE(!catches(ChaseSpeedBoost(SprintRunMultiplier, SprintDurationMs), 30.0f, 7.0f));
    REQUIRE(catches(ChaseSpeedBoost(SprintRunMultiplier, SprintDurationMs), LeashDistance, 7.0f));
    REQUIRE(!catches(ChaseSpeedBoost(SprintRunMultiplier, SprintDurationMs), LeashDistance, 10.0f));
}

TEST_CASE("Directed refuge movement requires a live danger and the independently approved path", "[AIWorld][LivingRole]")
{
    ActionSystem actions;
    ActionRequest request;
    request.Type = ActionType::MoveTo;
    request.SourceGoal = GoalType::SeekSafety;
    request.Destination = ActionPosition{ 0, 16, 0, 0 };
    request.Target = ActionTargetRef{ ObjectGuid::Create<HighGuid::Player>(1), 0 };
    request.FleeFromGuid = request.Target->Guid;
    ActionValidationContext context;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.Materialized = context.Alive = context.LivingRoleAllowed = context.LivingRoleExtensionsAllowed = true;
    context.LivingRoleZoneId = 12;
    context.LivingRole = LivingRolePolicy::Role::Prey;
    context.ActiveGoalType = request.SourceGoal;
    context.RoleMovementDestination = request.Destination;
    context.FleeSourceGuid = context.TargetGuid = request.Target->Guid;
    context.TargetResolved = context.TargetAlive = context.TargetWithinAttackRange = context.TargetInLineOfSight = true;
    // A critter need not be able to attack its attacker in order to escape.
    context.TargetAttackable = false;
    context.InCombat = true;
    REQUIRE(actions.Validate(request, context).Allowed);
    SECTION("feature off") { context.LivingRoleExtensionsAllowed = false; }
    SECTION("observe only") { context.ControlMode = AgentControlMode::ObserveOnly; }
    SECTION("outside Elwynn") { context.LivingRoleZoneId = 14; }
    SECTION("unapproved destination") { context.RoleMovementDestination.reset(); }
    SECTION("changed destination") { request.Destination->Y = 1; }
    SECTION("dead source") { context.TargetAlive = false; }
    SECTION("unloaded source") { context.TargetResolved = false; }
    SECTION("hidden source") { context.TargetInLineOfSight = false; }
    SECTION("distant source") { context.TargetWithinAttackRange = false; }
    SECTION("source changed maps") { context.TargetMapId = 1; }
    SECTION("forged source") { context.FleeSourceGuid.Clear(); }
    SECTION("different entity") { request.Target->Guid = ObjectGuid::Create<HighGuid::Player>(2); }
    SECTION("different entry") { request.Target->Entry = 30; }
    SECTION("unrelated movement") { context.HasActiveMovement = true; }
    SECTION("excessive range") { request.Destination->X = context.RoleMovementDestination->X = 50; }
    SECTION("NaN destination") { request.Destination->X = context.RoleMovementDestination->X = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("a refuge cannot grant an attack") { request.Type = ActionType::Attack; }
    REQUIRE(!actions.Validate(request, context).Allowed);
}

TEST_CASE("A local alarm authorizes only guard investigation and never a speculative attack", "[AIWorld][LivingRole]")
{
    ActionSystem actions;
    ActionRequest request;
    request.Type = ActionType::MoveTo;
    request.SourceGoal = GoalType::InvestigateDanger;
    request.Destination = ActionPosition{ 0, 18, 0, 0 };
    ActionValidationContext context;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.Materialized = context.Alive = context.LivingRoleAllowed = context.LivingRoleExtensionsAllowed = true;
    context.LivingRoleZoneId = 12;
    context.LivingRole = LivingRolePolicy::Role::Guard;
    context.ActiveGoalType = request.SourceGoal;
    context.RoleMovementDestination = request.Destination;
    context.FreshAllyAlarm = true;
    REQUIRE(actions.Validate(request, context).Allowed);
    SECTION("expired or unrelated alarm") { context.FreshAllyAlarm = false; }
    SECTION("civilian") { context.LivingRole = LivingRolePolicy::Role::Civilian; }
    SECTION("combatant") { context.LivingRole = LivingRolePolicy::Role::Combatant; }
    SECTION("busy guard") { context.InCombat = true; }
    SECTION("wrong destination") { request.Destination->X = 19; }
    SECTION("disabled") { context.LivingRoleExtensionsAllowed = false; }
    SECTION("no speculative attack") { request.Type = ActionType::Attack; }
    REQUIRE(!actions.Validate(request, context).Allowed);
}

TEST_CASE("Refuges and remembered danger reject routes through a threat", "[AIWorld][LivingRole]")
{
    using LivingRolePolicy::AvoidsDanger;
    // Both endpoints are outside danger, but the intervening path crosses it.
    REQUIRE(!AvoidsDanger(-12, 0, 12, 0, 0, 0, 8));
    REQUIRE(AvoidsDanger(-12, 10, 12, 10, 0, 0, 8));
    REQUIRE(AvoidsDanger(4, 0, 20, 0, 0, 0, 8));
    REQUIRE(!AvoidsDanger(4, 0, -20, 0, 0, 0, 8));
    REQUIRE(!AvoidsDanger(4, 0, 3, 0, 0, 0, 8));
    REQUIRE(!AvoidsDanger(4, 0, 4, 0, 0, 0, 8));
    REQUIRE(!AvoidsDanger(4, 0, std::numeric_limits<float>::infinity(), 0, 0, 0, 8));
    // Short navmesh segments must remain usable while moving outward.
    REQUIRE(AvoidsDanger(4, 0, 4.2f, 0, 0, 0, 8));
}

TEST_CASE("Personalities remain stable and herds do not mix arbitrary wildlife", "[AIWorld][LivingRole]")
{
    using namespace LivingRolePolicy;
    for (uint64 id = 79870; id < 79900; ++id)
    {
        REQUIRE(Caution(id) >= 0);
        REQUIRE(Caution(id) <= 1);
        REQUIRE(NoticeRadius(id) >= 9);
        REQUIRE(NoticeRadius(id) <= 14);
        REQUIRE(ShouldFlee(Role::Prey, 0, false, id));
        REQUIRE(!ShouldFlee(Role::Guard, 0.2f, false, id));
        REQUIRE(ShouldFlee(Role::Guard, 0.95f, false, id));
    }
    REQUIRE(Caution(79878) != Caution(79879));
    REQUIRE(SameHerd(883, 890));
    REQUIRE(SameHerd(890, 883));
    REQUIRE(SameHerd(721, 721));
    REQUIRE(!SameHerd(883, 113));
    REQUIRE(!SameHerd(2442, 721));
    REQUIRE(PreyScore(10, 20, 883, 30) < PreyScore(10, 100, 883, 30));
    REQUIRE(PreyScore(10, 100, 721, 30) < PreyScore(10, 100, 2442, 30));
    REQUIRE(PreyScore(1, 100, 2442, 30) < PreyScore(20, 100, 721, 30));
}

TEST_CASE("Completed work produces bounded persistent stocks once per work window", "[AIWorld][LivingRole]")
{
    using LivingRolePolicy::ProduceWorkStock;
    AgentEconomyState farmer;
    farmer.Money = 80;
    REQUIRE(ProduceWorkStock(farmer, 250, 1000));
    REQUIRE(farmer.Food == 6);
    REQUIRE(farmer.Resource == 0);
    REQUIRE(farmer.Money == 80);
    auto reloaded = farmer;
    REQUIRE(!ProduceWorkStock(reloaded, 250, 1000));
    REQUIRE(reloaded.Food == 6);
    REQUIRE(ProduceWorkStock(reloaded, 250, 2000));
    REQUIRE(reloaded.Food == 12);
    REQUIRE(!ProduceWorkStock(reloaded, 250, 1000));
    AgentEconomyState lumberjack;
    REQUIRE(ProduceWorkStock(lumberjack, 1975, 1000));
    REQUIRE(lumberjack.Resource == 2);
    REQUIRE(lumberjack.Food == 0);
    lumberjack.Resource = 19;
    REQUIRE(ProduceWorkStock(lumberjack, 1975, 2000));
    REQUIRE(lumberjack.Resource == 20);
    lumberjack.Resource = std::numeric_limits<uint32>::max();
    REQUIRE(ProduceWorkStock(lumberjack, 1975, 3000));
    REQUIRE(lumberjack.Resource == std::numeric_limits<uint32>::max());
    REQUIRE(!ProduceWorkStock(farmer, 30, 3000));
    REQUIRE(!ProduceWorkStock(farmer, 250, 0));
}

TEST_CASE("Role observations cannot survive a new materialization", "[AIWorld][LivingRole]")
{
    AgentRecord record;
    record.LivingRole.RuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(883, 1);
    record.LivingRole.CompanionGuid = ObjectGuid::Create<HighGuid::Unit>(890, 2);
    record.LivingRole.AlarmThreatGuid = ObjectGuid::Create<HighGuid::Unit>(30, 3);
    record.LivingRole.DangerUntilMs = record.LivingRole.AlarmUntilMs = 60000;
    record.LivingRole.CurrentPhase = LivingRoleState::Phase::SeekingSafety;
    record.EconomyState.Food = 4;
    record.LivingRole.ReturnFailures = 5;
    record.LivingRole.ReturnRetryAtMs = 60000;
    record.LivingRole.ReturnStalledSinceMs = 5000;
    record.LivingRole.ReturningHome = true;
    record.LivingRole.ReturnHomeLimit = 80.0f;
    record.LivingRole.ReturnTrail.push_back({0, 10, 20, 30});
    record.LivingRole.ReturnRoute.FollowingTrail = true;
    record.LivingRole.ReturnRoute.TrailTarget = ActionPosition{0, 10, 20, 30};
    record.LivingRole.ReturnRoute.Remember({0, 12, 20, 30});
    record.LivingRole.ReturnRoute.NextCareAtMs = 60000;
    record.LivingRole.ReturnDiagnostics.Candidates = 12;
    record.LivingRole.GatheringFood = true;
    record.LivingRole.ForageUntilMs = 120000;
    record.LivingRole.HasForageWaypoint = true;
    record.LivingRole.StockMeal = true;
    record.ResetLivingRoleActivity();
    REQUIRE(record.LivingRole.CompanionGuid.IsEmpty());
    REQUIRE(record.LivingRole.AlarmThreatGuid.IsEmpty());
    REQUIRE(record.LivingRole.DangerUntilMs == 0);
    REQUIRE(record.LivingRole.AlarmUntilMs == 0);
    REQUIRE(record.EconomyState.Food == 4);
    REQUIRE(record.LivingRole.ReturnFailures == 0);
    REQUIRE(record.LivingRole.ReturnRetryAtMs == 0);
    REQUIRE(record.LivingRole.ReturnStalledSinceMs == 0);
    REQUIRE(!record.LivingRole.ReturningHome);
    REQUIRE(record.LivingRole.ReturnHomeLimit == 0.0f);
    REQUIRE(record.LivingRole.ReturnTrail.empty());
    REQUIRE(!record.LivingRole.ReturnRoute.FollowingTrail);
    REQUIRE(!record.LivingRole.ReturnRoute.TrailTarget);
    REQUIRE(record.LivingRole.ReturnRoute.Visited.empty());
    REQUIRE(record.LivingRole.ReturnRoute.NextCareAtMs == 0);
    REQUIRE(record.LivingRole.ReturnDiagnostics.Candidates == 0);
    REQUIRE(!record.LivingRole.GatheringFood);
    REQUIRE(record.LivingRole.ForageUntilMs == 0);
    REQUIRE(!record.LivingRole.HasForageWaypoint);
    REQUIRE(!record.LivingRole.StockMeal);
}

TEST_CASE("Zone bounds reject an escape crossing outside between legal endpoints", "[AIWorld][LivingRole]")
{
    struct Point { float x, y, z; };
    auto inside = [](float x, float y, float) { return y >= 4 || x <= 2 || x >= 8; };
    std::vector<Point> shortcut{{0, 0, 0}, {10, 0, 0}};
    REQUIRE(inside(shortcut.front().x, shortcut.front().y, 0));
    REQUIRE(inside(shortcut.back().x, shortcut.back().y, 0));
    REQUIRE(!Movement::PathWithinBounds(shortcut, inside));
    std::vector<Point> around{{0, 0, 0}, {0, 5, 0}, {10, 5, 0}, {10, 0, 0}};
    REQUIRE(Movement::PathWithinBounds(around, inside));
    REQUIRE(!Movement::PathWithinBounds(std::vector<Point>{}, inside));
    REQUIRE(!Movement::PathWithinBounds(std::vector<Point>{{4, 0, 0}}, inside));
    // A bounds predicate need not inspect height. Reject every invalid axis
    // before passing a point to it, independently of hypot's NaN handling.
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
        for (unsigned axis = 0; axis < 3; ++axis)
            for (unsigned point = 0; point < 3; ++point)
            {
                CAPTURE(invalid, axis, point);
                std::vector<Point> path{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
                if (axis == 0) path[point].x = invalid;
                else if (axis == 1) path[point].y = invalid;
                else path[point].z = invalid;
                bool queriedInvalid = false;
                REQUIRE(!Movement::PathWithinBounds(path, [&](float x, float y, float z)
                {
                    queriedInvalid |= !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z);
                    return inside(x, y, z);
                }));
                REQUIRE(!queriedInvalid);
            }
    unsigned queries = 0;
    REQUIRE(!Movement::PathWithinBounds(std::vector<Point>{{0, 0, 0}, {100000, 0, 0}},
        [&](float, float, float) { ++queries; return true; }));
    REQUIRE(queries <= 513);
}

TEST_CASE("Blocked returns back off while permitting safe basic needs", "[AIWorld][LivingRole]")
{
    using namespace LivingRolePolicy;
    REQUIRE(LivingReturnPolicy::RetryDelayMs(1) == 5000);
    REQUIRE(LivingReturnPolicy::RetryDelayMs(2) > LivingReturnPolicy::RetryDelayMs(1));
    REQUIRE(LivingReturnPolicy::RetryDelayMs(1000) == 60000);
    // Replay the need values of the stranded boar/spider from the four-hour
    // recording: being unable to get home must not prohibit graze/rest.
    REQUIRE(RecoveryActivity(Role::Prey, 1, false) == Activity::Graze);
    REQUIRE(RecoveryActivity(Role::Predator, 1, false) == Activity::Rest);
    REQUIRE(RecoveryActivity(Role::Civilian, 1, false) == Activity::Eat);
    REQUIRE(RecoveryActivity(Role::Prey, 1, true) == Activity::Look);
    REQUIRE(RecoveryActivity(Role::Guard, 0, false) == Activity::Look);
    ActionSystem actions;
    for (Role role : {Role::Prey, Role::Predator, Role::Civilian, Role::Guard, Role::Worker, Role::Traveler, Role::Service})
    {
        ActionRequest request;
        request.Type = ActionType::Ambient; request.SourceGoal = GoalType::LocalActivity;
        request.AmbientActivity = RecoveryActivity(role, 1, false);
        ActionValidationContext context;
        context.ControlMode = AgentControlMode::AIWorldControlled;
        context.Materialized = context.Alive = context.LivingRoleAllowed = context.WildlifeRestAllowed = true;
        context.LivingRoleZoneId = 12; context.LivingRole = role;
        context.ActiveGoalType = request.SourceGoal; context.ExpectedAmbientActivity = request.AmbientActivity;
        REQUIRE(actions.Validate(request, context).Allowed);
        context.InCombat = true;
        REQUIRE(!actions.Validate(request, context).Allowed);
    }
}

TEST_CASE("Return retries search different bounded points including away from home", "[AIWorld][LivingRole]")
{
    ActionPosition from{0, -9606.48f, 218.8026f, 48.39812f};
    for (uint32 degree = 0; degree < 360; degree += 15)
    {
        float angle = float(degree) * GroupMemberFormation::TwoPi / 360;
        ActionPosition home{0, from.X + 47.1f * std::cos(angle), from.Y + 47.1f * std::sin(angle), from.Z};
        auto left = LivingReturnPolicy::Detours(from, home, 1);
        auto right = LivingReturnPolicy::Detours(from, home, 2);
        REQUIRE(left.size() == 8);
        REQUIRE(right.size() == 8);
        bool canWalkAround = false;
        for (auto const& point : left)
        {
            REQUIRE(std::hypot(point.X - from.X, point.Y - from.Y) <= 12.01f);
            REQUIRE(std::hypot(point.X - home.X, point.Y - home.Y) <= 47.1f + 12.01f);
            REQUIRE(point.MapId == from.MapId);
            canWalkAround |= std::hypot(point.X - home.X, point.Y - home.Y) > 47.1f;
            // Reordering the same four rejected points caused the long-run
            // regression. Every next-attempt point must really be different.
            for (auto const& other : right)
                REQUIRE(LivingReturnPolicy::Distance(point, other) > 0.1f);
        }
        REQUIRE(canWalkAround);
    }
    REQUIRE(LivingReturnPolicy::Detours(from, from, 1).empty());
    REQUIRE(LivingReturnPolicy::Detours(from, {1, 10, 20, 30}, 1).empty());
}

TEST_CASE("Observed return trail backtracks around a wall and does not record its own recovery loop", "[AIWorld][LivingRole]")
{
    using namespace LivingReturnPolicy;
    std::vector<ActionPosition> trail;
    for (auto p : std::vector<ActionPosition>{{0,0,0,1}, {0,0,8,1}, {0,10,8,1}, {0,10,0,1}})
        ObserveTrail(trail, p, false);
    auto steps = TrailSteps(trail, {0,10,0,1});
    REQUIRE(steps.front().Y == 8); // initially farther from home, around wall
    ObserveTrail(trail, steps.front(), true);
    REQUIRE(trail.size() == 3);
    steps = TrailSteps(trail, {0,10,8,1});
    REQUIRE(steps.front().X == 0);
    ObserveTrail(trail, {0,6,8,1}, true);
    REQUIRE(trail.size() == 3); // an intermediate return position is not a new outbound breadcrumb
    ObserveTrail(trail, {0,0,8,1}, true);
    REQUIRE(trail.size() == 2);
    REQUIRE(TrailSteps(trail, {0,0,8,1}).front().Y == 0);
    for (unsigned i=0; i<1000; ++i) ObserveTrail(trail, {0,float(i*4),0,1}, false);
    REQUIRE(trail.size() == MaxTrailPoints);
    REQUIRE(trail.front().X == 0);
    REQUIRE(TrailSteps(trail, {1,10,0,1}).empty());
    auto size = trail.size();
    ObserveTrail(trail, {0,0,0,std::numeric_limits<float>::quiet_NaN()}, false);
    REQUIRE(trail.size() == size);
}

TEST_CASE("Return validation rejects the recorded zero step after resolving its floor", "[AIWorld][LivingRole]")
{
    using namespace LivingReturnPolicy;
    // Spawn 80782 repeatedly chose this old breadcrumb: its proposed height
    // was distinct, but the engine resolved it to its current position.
    ActionPosition here{0, -9747.294f, -396.1371f, 52.74369f};
    ActionPosition proposed = here;
    proposed.Z = 55.43322f;
    REQUIRE(UsefulStep(here, proposed));
    ActionPosition resolved = proposed;
    resolved.Z = here.Z;
    REQUIRE_FALSE(UsefulStep(here, resolved));
    resolved.X += 3.5f;
    REQUIRE(UsefulStep(here, resolved));
    resolved.MapId = 1;
    REQUIRE_FALSE(UsefulStep(here, resolved));
    resolved = here;
    resolved.Z = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(UsefulStep(here, resolved));
}

TEST_CASE("Return trail consumes resolved arrivals and rejects walking back into its own loop", "[AIWorld][LivingRole]")
{
    using namespace LivingReturnPolicy;
    ActionPosition home{0, -30, 0, 52}, corner{0, -15, 8, 52}, original{0, -5, 8, 55}, start{0, 0, 0, 52};
    std::vector<ActionPosition> trail{home, corner, original, start};
    RouteMemory route;
    route.Remember(start);
    route.FollowingTrail = true;
    route.TrailTarget = original;
    route.TrailDestination = original;
    route.TrailDestination.Z = 52;
    route.ArriveOnTrail(start, trail);
    REQUIRE(trail.size() == 4); // selecting a step is not an arrival
    route.Remember(route.TrailDestination);
    route.ArriveOnTrail(route.TrailDestination, trail);
    REQUIRE(trail.size() == 2); // consume even though original height differs
    REQUIRE(!route.TrailTarget);
    REQUIRE(route.FollowingTrail);
    REQUIRE(route.Revisited(start)); // former HOME_PATH leg would undo progress
    auto next = TrailSteps(trail, route.TrailDestination);
    REQUIRE(!next.empty());
    REQUIRE(next.front().X == corner.X);
    REQUIRE_FALSE(route.Revisited(next.front()));
    // Memory stays bounded, rejects a nearby repeat and distinguishes floors.
    for (unsigned i = 0; i < 1000; ++i) route.Remember({0, float(i * 4), 0, 52});
    REQUIRE(route.Visited.size() == MaxTrailPoints);
    REQUIRE(route.Revisited({0, 3996.5f, 0, 52}));
    REQUIRE_FALSE(route.Revisited({0, 3996.5f, 0, 62}));
    REQUIRE_FALSE(route.Revisited({1, 3996.5f, 0, 52}));
}

TEST_CASE("Moving recovery leaves time for basic needs without waiting for a path failure", "[AIWorld][LivingRole]")
{
    LivingReturnPolicy::RouteMemory route;
    route.NextCareAtMs = 61000;
    REQUIRE_FALSE(route.NeedsPause(60000, 1, 0));
    // Successive real moves do not reset the opportunity to graze/rest.
    for (unsigned i = 0; i < 20; ++i) route.Remember({0, float(i * 4), 0, 52});
    REQUIRE(route.NeedsPause(61000, 1, 0));
    REQUIRE(route.NeedsPause(61000, 0, 1));
    REQUIRE_FALSE(route.NeedsPause(61000, 0.4f, 0.2f));
    route.NextCareAtMs = 121000;
    REQUIRE_FALSE(route.NeedsPause(66000, 1, 1));
}

TEST_CASE("Empty food is replenished by completed work without duplicate money or free meals", "[AIWorld][LivingRole]")
{
    using namespace LivingRolePolicy;
    AgentEconomyState farmer;
    farmer.Money = 39; farmer.Resource = 4; farmer.LastRewardedWorkWindowId = 1234;
    REQUIRE(CanGatherFood(Role::Worker, 250));
    REQUIRE_FALSE(CanGatherFood(Role::Worker, 1975));
    REQUIRE_FALSE(CanGatherFood(Role::Predator, 30));
    REQUIRE(GatherEmergencyFood(farmer));
    REQUIRE(farmer.Food == 2);
    REQUIRE_FALSE(GatherEmergencyFood(farmer));
    REQUIRE(farmer.Money == 39);
    REQUIRE(farmer.Resource == 4);
    REQUIRE(farmer.LastRewardedWorkWindowId == 1234);
    REQUIRE(ConsumeStockMeal(farmer));
    REQUIRE(ConsumeStockMeal(farmer));
    REQUIRE_FALSE(ConsumeStockMeal(farmer));
    REQUIRE(GatherEmergencyFood(farmer));
    // Twelve 20-minute work windows cover the supplied recording's 62 meals.
    farmer.Food = 0;
    for (uint64 day=1; day<=12; ++day)
    {
        REQUIRE(ProduceWorkStock(farmer, 250, 1234+day));
        for (unsigned meal=0; meal<(day%6 ? 5u : 6u); ++meal) REQUIRE(ConsumeStockMeal(farmer));
    }
    REQUIRE(farmer.Food > 0);
}

TEST_CASE("Curated meals consume real stock and leave an empty inventory unchanged", "[AIWorld][LivingRole]")
{
    using namespace LivingRolePolicy;
    AgentEconomyState farmer;
    farmer.Food = 20; farmer.Resource = 3; farmer.Money = 16;
    REQUIRE(CuratedSelfCare(Role::Worker, 1, 1, farmer.Food) == Activity::Eat);
    REQUIRE(ConsumeStockMeal(farmer));
    REQUIRE(farmer.Food == 19);
    REQUIRE(farmer.Resource == 3);
    REQUIRE(farmer.Money == 16);
    REQUIRE(CuratedSelfCare(Role::Worker, 0, 1, farmer.Food) == Activity::Rest);
    REQUIRE(CuratedSelfCare(Role::Worker, 0, 0, farmer.Food) == Activity::None);
    farmer.Food = 0;
    REQUIRE(!ConsumeStockMeal(farmer));
    REQUIRE(farmer.Food == 0);
    REQUIRE(CuratedSelfCare(Role::Worker, 1, 0, 0) == Activity::None);
    REQUIRE(CuratedSelfCare(Role::Predator, 1, 1, 20) == Activity::None);
    REQUIRE(CuratedSelfCare(Role::Service, 0, 1, 20) == Activity::None);
}

TEST_CASE("Foraging reaches wider search areas through bounded authorized local steps", "[AIWorld][LivingRole]")
{
    using namespace LivingForagePolicy;
    ActionPosition home{0, -9233.27f, 271.076f, 72.82477f};
    ActionSystem actions;
    ActionValidationContext context;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.Materialized = context.Alive = context.LivingRoleAllowed = true;
    context.LivingRoleZoneId = 12; context.LivingRole = LivingRolePolicy::Role::Predator;
    context.ActiveGoalType = GoalType::LocalActivity;
    for (uint32 leg = 0; leg < 12; ++leg)
    {
        auto waypoint = Waypoint(home, 79883, leg);
        ActionPosition from = home;
        unsigned steps = 0;
        while (std::hypot(from.X - waypoint.X, from.Y - waypoint.Y) > 3)
        {
            auto step = Step(from, waypoint, home);
            REQUIRE(step.has_value());
            REQUIRE(std::hypot(step->X - from.X, step->Y - from.Y) <= 20.01f);
            REQUIRE(std::hypot(step->X - home.X, step->Y - home.Y) < HomeRadius);
            ActionRequest request;
            request.Type = ActionType::MoveTo; request.SourceGoal = GoalType::LocalActivity; request.Destination = step;
            context.X = from.X; context.Y = from.Y; context.Z = from.Z;
            REQUIRE(actions.Validate(request, context).Allowed);
            from = *step;
            REQUIRE(++steps <= 4);
        }
        if (leg % 3 == 2) REQUIRE(std::hypot(from.X - home.X, from.Y - home.Y) > 60);
    }
    REQUIRE(!Step(home, {1, home.X + 10, home.Y, home.Z}, home));
    REQUIRE(!Step(home, home, home));
    REQUIRE(!Step(home, {0, std::numeric_limits<float>::infinity(), 0, 0}, home));
    ActionPosition outside{0, home.X + 90, home.Y, home.Z};
    REQUIRE(!Step(outside, {0, home.X + 120, home.Y, home.Z}, home));
}

TEST_CASE("A conversation can face only the nearby validated social partner", "[AIWorld][LivingRole]")
{
    ActionSystem actions;
    ActionRequest request;
    request.Type = ActionType::Ambient;
    request.SourceGoal = GoalType::LocalActivity;
    request.AmbientActivity = LivingRolePolicy::Activity::Talk;
    request.Target = ActionTargetRef{ ObjectGuid::Create<HighGuid::Unit>(250, 1), 250 };
    ActionValidationContext context;
    context.ControlMode = AgentControlMode::AIWorldControlled;
    context.Materialized = context.Alive = context.LivingRoleAllowed = true;
    context.LivingRoleZoneId = 12;
    context.LivingRole = LivingRolePolicy::Role::Worker;
    context.ActiveGoalType = request.SourceGoal;
    context.ExpectedAmbientActivity = request.AmbientActivity;
    context.TargetIsSocialPartner = context.TargetResolved = context.TargetAlive = true;
    context.TargetWithinAttackRange = context.TargetInLineOfSight = true;
    context.TargetGuid = request.Target->Guid;
    context.TargetEntry = request.Target->Entry;
    REQUIRE(actions.Validate(request, context).Allowed);
    SECTION("unrelated NPC") { context.TargetIsSocialPartner = false; }
    SECTION("target replaced") { context.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(250, 2); }
    SECTION("unloaded partner") { context.TargetResolved = false; }
    SECTION("dead partner") { context.TargetAlive = false; }
    SECTION("out of range") { context.TargetWithinAttackRange = false; }
    SECTION("behind a wall") { context.TargetInLineOfSight = false; }
    SECTION("different map") { context.TargetMapId = 1; }
    SECTION("emergency") { context.InCombat = true; }
    REQUIRE(!actions.Validate(request, context).Allowed);
}
