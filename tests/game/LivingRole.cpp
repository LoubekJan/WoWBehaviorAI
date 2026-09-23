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
    REQUIRE(farmer.Food == 4);
    REQUIRE(farmer.Resource == 0);
    REQUIRE(farmer.Money == 80);
    auto reloaded = farmer;
    REQUIRE(!ProduceWorkStock(reloaded, 250, 1000));
    REQUIRE(reloaded.Food == 4);
    REQUIRE(ProduceWorkStock(reloaded, 250, 2000));
    REQUIRE(reloaded.Food == 8);
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
    record.ResetLivingRoleActivity();
    REQUIRE(record.LivingRole.CompanionGuid.IsEmpty());
    REQUIRE(record.LivingRole.AlarmThreatGuid.IsEmpty());
    REQUIRE(record.LivingRole.DangerUntilMs == 0);
    REQUIRE(record.LivingRole.AlarmUntilMs == 0);
    REQUIRE(record.EconomyState.Food == 4);
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
