/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingRaidRuntime.h"
#include "Agent/LivingRaidMemory.h"
#include "Agent/LivingRolePolicy.h"
#include "Action/ActionSystem.h"
#include <limits>

namespace
{
    using namespace LivingRaid;
    using LivingReturnPolicy::Distance;
    using LivingReturnPolicy::SamePosition;

    void Bounded(std::vector<ActionPosition> const& route, ActionPosition from, float limit = 14)
    {
        for (auto const& point : route)
        {
            REQUIRE(point.MapId == from.MapId);
            REQUIRE(LivingReturnPolicy::Finite(point));
            REQUIRE(Distance(from, point) <= limit + 0.0001f);
            from = point;
        }
    }
    bool Includes(std::vector<ActionPosition> const& route, ActionPosition const& point)
    {
        return std::any_of(route.begin(), route.end(), [&](ActionPosition const& other) { return SamePosition(point, other); });
    }
    ActionValidationContext RaidActionContext()
    {
        ActionValidationContext context;
        context.ControlMode = AgentControlMode::AIWorldControlled;
        context.Materialized = context.Alive = context.LivingRoleAllowed = true;
        context.MapId = context.Scope.MapId = 725;
        context.Scope.ZoneIds = {4988};
        context.LivingRoleZoneId = 4988;
        context.LivingRole = LivingRolePolicy::Role::Raider;
        context.ActiveGoalType = GoalType::LocalActivity;
        context.ActiveGoalStartedAtMs = 100;
        context.ExpectedAmbientActivity = LivingRolePolicy::Activity::Work;
        return context;
    }
}

TEST_CASE("Only the reviewed controlled raider and guard receive raid capabilities", "[AIWorld][LivingRaid]")
{
    REQUIRE(RaiderCapability(901055, 725, AgentType::Combatant, AgentControlMode::AIWorldControlled, true));
    REQUIRE(GuardCapability(901039, 725, AgentType::Guard, AgentControlMode::AIWorldControlled, true));
    for (uint64 spawn : {901034ull, 901038ull, 901039ull, 901054ull, 901055ull, 901056ull, 0ull})
    {
        REQUIRE(RaiderCapability(spawn, 725, AgentType::Combatant, AgentControlMode::AIWorldControlled, true) == (spawn == 901055));
        REQUIRE(GuardCapability(spawn, 725, AgentType::Guard, AgentControlMode::AIWorldControlled, true) == (spawn == 901039));
    }
    for (AgentType type : {AgentType::Civilian, AgentType::Guard, AgentType::Merchant, AgentType::Unclassified,
        AgentType::Combatant, AgentType::Predator, AgentType::Prey, AgentType(255)})
    {
        REQUIRE(RaiderCapability(901055, 725, type, AgentControlMode::AIWorldControlled, true) == (type == AgentType::Combatant));
        REQUIRE(GuardCapability(901039, 725, type, AgentControlMode::AIWorldControlled, true) == (type == AgentType::Guard));
    }
    for (auto control : {AgentControlMode::ObserveOnly, AgentControlMode(255)})
    {
        REQUIRE_FALSE(RaiderCapability(901055, 725, AgentType::Combatant, control, true));
        REQUIRE_FALSE(GuardCapability(901039, 725, AgentType::Guard, control, true));
    }
    for (uint32 map : {0u, 1u, 724u, 726u})
    {
        REQUIRE_FALSE(RaiderCapability(901055, map, AgentType::Combatant, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(GuardCapability(901039, map, AgentType::Guard, AgentControlMode::AIWorldControlled, true));
    }
    REQUIRE_FALSE(RaiderCapability(901055, 725, AgentType::Combatant, AgentControlMode::AIWorldControlled, false));
    REQUIRE_FALSE(GuardCapability(901039, 725, AgentType::Guard, AgentControlMode::AIWorldControlled, false));
}

TEST_CASE("A hungry raider fairly rotates all stocked village sources without targeting camps", "[AIWorld][LivingRaid]")
{
    std::array<uint32, 4> stocks{6, 30, 30, 30};
    uint32 previous = 0;
    for (unsigned round = 0; round < 3; ++round)
        for (uint32 expected : {1u, 4u, 5u, 6u})
        {
            auto selected = SelectSource(previous, true, stocks);
            REQUIRE(selected.has_value());
            REQUIRE(*selected == expected);
            previous = *selected;
        }
    // Empty/low villages are skipped, including one depleted since the
    // previous attempt; their eligibility returns when producers restock.
    stocks = {5, 6, 0, 30};
    REQUIRE(SelectSource(0, true, stocks) == 4);
    REQUIRE(SelectSource(4, true, stocks) == 6);
    REQUIRE(SelectSource(6, true, stocks) == 4);
    stocks[2] = 6;
    REQUIRE(SelectSource(4, true, stocks) == 5);
    stocks = {0, 0, 0, 5};
    REQUIRE_FALSE(SelectSource(6, true, stocks).has_value());
    // Legacy raids retain their exact south-only behavior even when the
    // caller supplies observations for other warehouse identifiers.
    stocks = {5, 30, 30, 30};
    REQUIRE_FALSE(SelectSource(0, false, stocks).has_value());
    stocks[0] = 6;
    REQUIRE(SelectSource(6, false, stocks) == 1);
}

TEST_CASE("Each village raid owns a bounded source-specific reversible corridor", "[AIWorld][LivingRaid]")
{
    for (auto const& spec : Sources)
    {
        REQUIRE(SourceFor(spec.Depot, true) == &spec);
        REQUIRE((SourceFor(spec.Depot, false) != nullptr) == (spec.Depot == 1));
        REQUIRE(spec.Count > 0);
        REQUIRE(spec.Count <= spec.Route.size());
        ActionPosition previous = Camp;
        for (uint32 leg = 0; leg < spec.Count; ++leg)
        {
            REQUIRE(LivingReturnPolicy::Finite(spec.Route[leg]));
            REQUIRE(spec.Route[leg].MapId == 725);
            REQUIRE(Distance(previous, spec.Route[leg]) <= 384);
            REQUIRE(std::hypot(spec.Route[leg].X-Camp.X, spec.Route[leg].Y-Camp.Y) <= RaiderHomeRadius);
            previous = spec.Route[leg];
        }
        for (uint32 leg = spec.Count-1; leg > 0; --leg)
            REQUIRE(Distance(spec.Route[leg], spec.Route[leg-1]) <= 384);
        REQUIRE(SamePosition(spec.Point(), spec.Route[spec.Count-1]));
        if (spec.Depot == 1) REQUIRE(SamePosition(spec.Point(), Source));
        else
        {
            auto field = LivingFields::Field(spec.Depot);
            REQUIRE(field != nullptr);
            REQUIRE(SamePosition(spec.Point(), field->DepotPoint));
            REQUIRE(ArrivalRadius(spec.Point()) == 1.9f);
        }
    }
    for (uint32 forbidden : {0u, 2u, 3u, 7u, 99u})
        REQUIRE_FALSE(SourceFor(forbidden, true));
    REQUIRE(ArrivalRadius(Source) == 3);
    REQUIRE(ArrivalRadius(Camp) == 3);
    REQUIRE(ArrivalRadius(LivingFields::Fields[0].WorkPoints[0]) == 3);
    auto shifted = LivingFields::Fields[0].DepotPoint;
    shifted.X += 0.01f;
    REQUIRE(ArrivalRadius(shifted) == 3);
}

TEST_CASE("The remembered left guard has a separate authorized approach to the right warehouse", "[AIWorld][LivingRaid]")
{
    auto route = SourceFor(6, true, 2);
    REQUIRE(route == &RightBypass);
    REQUIRE(route->RouteId == 2);
    REQUIRE(SamePosition(route->Point(), LivingFields::Fields[2].DepotPoint));
    REQUIRE_FALSE(SourceFor(6, false, 2));
    for (uint32 depot : {0u, 1u, 2u, 3u, 4u, 5u, 7u}) REQUIRE_FALSE(SourceFor(depot, true, 2));
    REQUIRE_FALSE(SourceFor(6, true, 3));
    ActionPosition leftGuard{725, 40.2458698357f, 872.5641450404f, .15f};
    auto minimum = [&](SourceSpec const& spec)
    {
        float result = std::numeric_limits<float>::max();
        ActionPosition previous = Camp;
        for (uint32 leg = 0; leg < spec.Count; ++leg)
        {
            auto const& point = spec.Route[leg];
            REQUIRE(Distance(previous, point) <= 384);
            REQUIRE(std::hypot(point.X-Camp.X, point.Y-Camp.Y) <= RaiderHomeRadius);
            result = std::min(result, LivingRaidMemory::SegmentDistance(leftGuard, previous, point));
            previous = point;
        }
        return result;
    };
    REQUIRE(minimum(*SourceFor(6, true)) < LivingRaidMemory::DangerRadius);
    REQUIRE(minimum(*route) > LivingRaidMemory::DangerRadius);
}

TEST_CASE("Dense straight raid travel coalesces into bounded three-dimensional execution legs", "[AIWorld][LivingRaid]")
{
    ActionPosition from{725, 0, 0, 0};
    std::vector<ActionPosition> dense;
    for (unsigned i = 1; i <= 30; ++i) dense.push_back({725, float(i)*2, 0, float(i)});
    auto merged = CoalesceStraightLegs(from, dense);
    REQUIRE(merged.size() == 5);
    REQUIRE(SamePosition(merged.back(), dense.back()));
    Bounded(merged, from);
    auto smaller = CoalesceStraightLegs(from, dense, 7);
    REQUIRE(smaller.size() == 10);
    REQUIRE(SamePosition(smaller.back(), dense.back()));
    Bounded(smaller, from, 7);
    auto larger = CoalesceStraightLegs(from, dense, LivingReturnPolicy::MaxStepLength);
    REQUIRE(larger.size() == 3);
    Bounded(larger, from, LivingReturnPolicy::MaxStepLength);
}

TEST_CASE("Guard commutes use the safe approach independently of their duty or meal owner", "[AIWorld][LivingRaid]")
{
    auto guid = ObjectGuid::Create<HighGuid::Unit>(1423, 901039);
    ActionPosition home{725, -396, 520, .15f};
    GuardTransit transit;
    auto first = transit.Select(guid, home, Source, true, true);
    REQUIRE(first.has_value());
    REQUIRE(SamePosition(*first, GuardVia));
    REQUIRE_FALSE(transit.PassedVia);
    auto second = transit.Select(guid, GuardVia, Source, true, true);
    REQUIRE(second.has_value());
    REQUIRE(SamePosition(*second, Source));
    REQUIRE(transit.PassedVia);
    ActionPosition approach{725, -391.2f, 490.9332f, .21656f};
    REQUIRE(SamePosition(*transit.Select(guid, approach, Source, true, true), Source));
    REQUIRE(transit.ObserveArrival(Source, true));
    REQUIRE_FALSE(transit.Goal.has_value());
    REQUIRE_FALSE(transit.PassedVia);
    // The meal's return and the duty's next departure are distinct trips,
    // even though both use the same consumer step planner and incarnation.
    REQUIRE(SamePosition(*transit.Select(guid, Source, home, true, true), GuardVia));
    REQUIRE(SamePosition(*transit.Select(guid, GuardVia, home, true, true), home));
    REQUIRE(transit.ObserveArrival(home, true));
    REQUIRE(SamePosition(*transit.Select(guid, home, Source, true, true), GuardVia));
}

TEST_CASE("Guard waypoint and final arrival require their actual floor and line of sight", "[AIWorld][LivingRaid]")
{
    auto guid = ObjectGuid::Create<HighGuid::Unit>(1423, 901039);
    GuardTransit transit;
    ActionPosition above = GuardVia; above.Z += 4;
    REQUIRE(SamePosition(*transit.Select(guid, above, Source, true, true), GuardVia));
    REQUIRE_FALSE(transit.PassedVia);
    REQUIRE(SamePosition(*transit.Select(guid, GuardVia, Source, true, false), GuardVia));
    REQUIRE_FALSE(transit.PassedVia);
    ActionPosition edge = GuardVia; edge.Z += 3;
    REQUIRE(SamePosition(*transit.Select(guid, edge, Source, true, true), Source));
    REQUIRE(transit.PassedVia);
    above = Source; above.Z += 4;
    REQUIRE_FALSE(transit.ObserveArrival(above, true));
    REQUIRE_FALSE(transit.ObserveArrival(Source, false));
    REQUIRE(transit.Goal.has_value());
    REQUIRE(transit.ObserveArrival(Source, true));
    REQUIRE_FALSE(transit.Goal.has_value());
}

TEST_CASE("Guard transit cannot reuse a passed waypoint after a changed goal life or interruption", "[AIWorld][LivingRaid]")
{
    auto guid = ObjectGuid::Create<HighGuid::Unit>(1423, 901039);
    auto nextLife = ObjectGuid::Create<HighGuid::Unit>(1423, 901040);
    ActionPosition home{725, -396, 520, .15f};
    ActionPosition between{725, -391.2f, 490.9332f, .21656f};
    GuardTransit transit;
    REQUIRE(SamePosition(*transit.Select(guid, GuardVia, Source, true, true), Source));
    SECTION("changed ultimate destination")
    {
        REQUIRE(SamePosition(*transit.Select(guid, between, home, true, true), GuardVia));
    }
    SECTION("changed incarnation")
    {
        REQUIRE(SamePosition(*transit.Select(nextLife, between, Source, true, true), GuardVia));
    }
    SECTION("same GUID respawn or emergency reset")
    {
        transit.Reset();
        REQUIRE(SamePosition(*transit.Select(guid, between, Source, true, true), GuardVia));
    }
    REQUIRE_FALSE(transit.PassedVia);
}

TEST_CASE("A guard already at its final destination is not sent on a waypoint detour", "[AIWorld][LivingRaid]")
{
    auto guid = ObjectGuid::Create<HighGuid::Unit>(1423, 901039);
    GuardTransit transit;
    REQUIRE(SamePosition(*transit.Select(guid, Source, Source, true, false), Source));
    REQUIRE_FALSE(transit.Goal.has_value());
    REQUIRE_FALSE(transit.PassedVia);
    ActionPosition home{725, -396, 520, .15f};
    REQUIRE(SamePosition(*transit.Select(guid, home, home, true, false), home));
    REQUIRE_FALSE(transit.Goal.has_value());
}

TEST_CASE("Guard transit rejects malformed context without retaining a passed waypoint", "[AIWorld][LivingRaid]")
{
    auto guid = ObjectGuid::Create<HighGuid::Unit>(1423, 901039);
    GuardTransit transit;
    REQUIRE(transit.Select(guid, GuardVia, Source, true, true).has_value());
    SECTION("empty incarnation") { REQUIRE_FALSE(transit.Select({}, GuardVia, Source, true, true).has_value()); }
    SECTION("foreign source map")
    {
        auto here = GuardVia; here.MapId = 0;
        REQUIRE_FALSE(transit.Select(guid, here, Source, true, true).has_value());
    }
    SECTION("foreign destination map")
    {
        auto destination = Source; destination.MapId = 0;
        REQUIRE_FALSE(transit.Select(guid, GuardVia, destination, true, true).has_value());
    }
    SECTION("nonfinite source")
    {
        auto here = GuardVia; here.Z = std::numeric_limits<float>::quiet_NaN();
        REQUIRE_FALSE(transit.Select(guid, here, Source, true, true).has_value());
    }
    SECTION("nonfinite destination")
    {
        auto destination = Source; destination.X = std::numeric_limits<float>::infinity();
        REQUIRE_FALSE(transit.Select(guid, GuardVia, destination, true, true).has_value());
    }
    REQUIRE_FALSE(transit.Goal.has_value());
    REQUIRE_FALSE(transit.PassedVia);
}

TEST_CASE("Raid leg coalescing retains L turns reversals and terrain crests", "[AIWorld][LivingRaid]")
{
    ActionPosition from{725, 0, 0, 0};
    SECTION("L turn")
    {
        std::vector<ActionPosition> route{{725, 4, 0, 0}, {725, 8, 0, 0}, {725, 8, 4, 0}, {725, 8, 8, 0}};
        auto merged = CoalesceStraightLegs(from, route);
        REQUIRE(merged.size() == 2);
        REQUIRE(SamePosition(merged.front(), route[1]));
        REQUIRE(SamePosition(merged.back(), route.back()));
        Bounded(merged, from);
    }
    SECTION("a reversal is not a straight forward continuation")
    {
        std::vector<ActionPosition> route{{725, 4, 0, 0}, {725, 8, 0, 0}, {725, 4, 0, 0}, from};
        auto merged = CoalesceStraightLegs(from, route);
        REQUIRE(merged.size() == 2);
        REQUIRE(SamePosition(merged.front(), route[1]));
        REQUIRE(SamePosition(merged.back(), from));
        Bounded(merged, from);
    }
    SECTION("height crest")
    {
        std::vector<ActionPosition> route{{725, 4, 0, 1}, {725, 8, 0, 2}, {725, 12, 0, 1}, {725, 16, 0, 0}};
        auto merged = CoalesceStraightLegs(from, route);
        REQUIRE(merged.size() == 2);
        REQUIRE(SamePosition(merged.front(), route[1]));
        REQUIRE(SamePosition(merged.back(), route.back()));
        Bounded(merged, from);
    }
    SECTION("even a shallow height crest remains a protected corner")
    {
        std::vector<ActionPosition> route{{725, 4, 0, 0.001f}, {725, 8, 0, 0}};
        auto merged = CoalesceStraightLegs(from, route);
        REQUIRE(merged.size() == 2);
        REQUIRE(SamePosition(merged.front(), route.front()));
    }
    SECTION("small successive bends do not accumulate into a smoothed curve")
    {
        std::vector<ActionPosition> route;
        for (unsigned i = 1; i <= 10; ++i) route.push_back({725, float(i), float(i*i)*0.0001f, 0});
        auto merged = CoalesceStraightLegs(from, route);
        REQUIRE(merged.size() > 5);
        REQUIRE(SamePosition(merged.back(), route.back()));
        Bounded(merged, from);
    }
}

TEST_CASE("Raid coalescing preserves the real source-to-guard navmesh bend", "[AIWorld][LivingRaid]")
{
    // Ground-only Detour probe of authored map725, source -> guard, 2026-10-27.
    // Intermediate interpolation emulates the native dense corridor points;
    // the original mesh corner must survive regardless of their density.
    std::vector<ActionPosition> path{{725, -372.4611511f, 492.6821594f, 0.39979494f},
        {725, -384.5333557f, 509.5998535f, 0.48322296f}, {725, -396, 520, 0.21655655f}};
    std::vector<ActionPosition> dense;
    for (unsigned leg = 1; leg < path.size(); ++leg)
    {
        auto const& a = path[leg-1]; auto const& b = path[leg];
        unsigned samples = unsigned(std::ceil(Distance(a, b) / 3));
        for (unsigned n = 1; n <= samples; ++n)
        {
            float t = float(n)/samples;
            dense.push_back(n == samples ? b : ActionPosition{725, a.X+(b.X-a.X)*t,
                a.Y+(b.Y-a.Y)*t, a.Z+(b.Z-a.Z)*t});
        }
    }
    auto merged = CoalesceStraightLegs(path.front(), dense);
    REQUIRE(merged.size() < dense.size());
    REQUIRE(Includes(merged, path[1]));
    REQUIRE(SamePosition(merged.back(), path.back()));
    Bounded(merged, path.front());
}

TEST_CASE("Raid coalescing fails closed on invalid coordinates maps or step limits", "[AIWorld][LivingRaid]")
{
    ActionPosition from{725, 0, 0, 0};
    std::vector<ActionPosition> route{{725, 4, 0, 0}, {725, 8, 0, 0}};
    REQUIRE(CoalesceStraightLegs(from, {}).empty());
    REQUIRE(CoalesceStraightLegs(from, {from, from}).empty());
    REQUIRE(CoalesceStraightLegs(from, route, 0).empty());
    REQUIRE(CoalesceStraightLegs(from, route, -1).empty());
    REQUIRE(CoalesceStraightLegs(from, route, 1).empty());
    REQUIRE(CoalesceStraightLegs(from, route, 29).empty());
    REQUIRE(CoalesceStraightLegs(from, route, std::numeric_limits<float>::infinity()).empty());
    REQUIRE(CoalesceStraightLegs(from, route, std::numeric_limits<float>::quiet_NaN()).empty());
    REQUIRE(CoalesceStraightLegs({725, std::numeric_limits<float>::quiet_NaN(), 0, 0}, route).empty());
    REQUIRE(CoalesceStraightLegs(from, {{725, 2, std::numeric_limits<float>::infinity(), 0}}).empty());
    REQUIRE(CoalesceStraightLegs(from, {{724, 2, 0, 0}}).empty());
    REQUIRE(CoalesceStraightLegs(from, {{725, 2, 0, 0}, {726, 4, 0, 0}}).empty());
    REQUIRE(CoalesceStraightLegs(from, {{725, 15, 0, 0}}).empty());
    REQUIRE(CoalesceStraightLegs(from, std::vector<ActionPosition>(129, {725, 1, 0, 0})).empty());
    auto duplicate = CoalesceStraightLegs(from, {from, route.front(), route.front(), route.back(), route.back()});
    REQUIRE(duplicate.size() == 1);
    REQUIRE(SamePosition(duplicate.back(), route.back()));
    // Corridor interpolation at float world coordinates can round a nominal
    // fourteen-yard original edge by one representable float step.
    ActionPosition rounded{725, std::nextafter(14.0f, 15.0f), 0, 0};
    auto original = CoalesceStraightLegs(from, {rounded});
    REQUIRE(original.size() == 1);
    REQUIRE(SamePosition(original.back(), rounded));
    Bounded(original, from);
}

TEST_CASE("Transient raid work does not grant ordinary combatants work or predator hunts", "[AIWorld][LivingRaid]")
{
    using namespace LivingRolePolicy;
    REQUIRE(Resolve(AgentType::Combatant, 116, false) == Role::Combatant);
    REQUIRE(Allows(Role::Raider, Activity::Work));
    REQUIRE_FALSE(Allows(Role::Combatant, Activity::Work));
    REQUIRE_FALSE(CanHuntNeutralPrey(Role::Raider, AgentType::Prey, true));
    ActionSystem actions;
    auto context = RaidActionContext();
    ActionRequest work;
    work.Actor = {RaiderSpawn}; work.Type = ActionType::Ambient; work.SourceGoal = GoalType::LocalActivity;
    work.GoalStartedAtMs = 100; work.AmbientActivity = Activity::Work;
    REQUIRE(actions.Validate(work, context).Allowed);
    context.LivingRole = Role::Combatant;
    REQUIRE_FALSE(actions.Validate(work, context).Allowed);
    context = RaidActionContext();
    context.ControlMode = AgentControlMode::ObserveOnly;
    REQUIRE_FALSE(actions.Validate(work, context).Allowed);
    context = RaidActionContext();
    context.InCombat = true;
    REQUIRE_FALSE(actions.Validate(work, context).Allowed);
    context = RaidActionContext();
    context.ActiveGoalType = GoalType::PredatorHunt;
    context.TargetResolved = context.TargetAlive = context.TargetAttackable = context.TargetIsRolePrey = true;
    context.TargetWithinAttackRange = context.TargetInLineOfSight = true;
    context.TargetMapId = 725;
    context.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(883, 1);
    context.TargetEntry = 883;
    ActionRequest hunt;
    hunt.Actor = {RaiderSpawn}; hunt.Type = ActionType::Attack; hunt.SourceGoal = GoalType::PredatorHunt;
    hunt.GoalStartedAtMs = 100; hunt.Target = ActionTargetRef{context.TargetGuid, context.TargetEntry};
    REQUIRE_FALSE(actions.Validate(hunt, context).Allowed);
    context.LivingRole = Role::Hunter;
    REQUIRE(actions.Validate(hunt, context).Allowed);
}
