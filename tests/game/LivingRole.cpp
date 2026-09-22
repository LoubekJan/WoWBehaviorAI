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
#include "Agent/AgentRecord.h"
#include "DBCStructure.h"
#include <limits>

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
