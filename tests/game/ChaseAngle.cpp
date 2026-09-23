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
#include "Agent/GroupMemberFormation.h"
#include "MovementDefines.h"
#include "Position.h"
#include <limits>

TEST_CASE("World-space pack chase slots stay reachable when the target turns", "[AIWorld][Movement][ChaseAngle]")
{
    using namespace GroupMemberFormation;
    std::vector<AgentId> const roster{ AgentId{ 1 }, AgentId{ 2 }, AgentId{ 3 }, AgentId{ 4 }, AgentId{ 5 } };
    for (AgentId member : roster)
    {
        auto slot = GetSlot(member, roster, MaxRadius);
        REQUIRE(slot.has_value());
        ChaseAngle const chase(slot->Angle, ChaseAngleTolerance);
        Position const attacker(10.0f + slot->X, 20.0f + slot->Y);

        // Include turns through zero and a half-turn toward a surviving wolf.
        // A slot remains valid after the front wolf dies and the player turns.
        for (float facing : { 0.0f, 0.001f, TwoPi / 4, TwoPi / 2, TwoPi * 0.75f, TwoPi - 0.001f, 0.0f, TwoPi / 6 })
        {
            Position const target(10.0f, 20.0f, 0.0f, facing);
            ChaseAngle const relative = chase.Resolve(target.GetOrientation(), ChaseAngleReference::World);
            REQUIRE(relative.IsAngleOkay(target.GetRelativeAngle(attacker)));
            REQUIRE(relative.Tolerance == chase.Tolerance);
            float const absolute = target.ToAbsoluteAngle(relative.RelativeAngle);
            REQUIRE(target.GetPositionX() + slot->Radius * std::cos(absolute) == Approx(attacker.GetPositionX()).margin(0.0001f));
            REQUIRE(target.GetPositionY() + slot->Radius * std::sin(absolute) == Approx(attacker.GetPositionY()).margin(0.0001f));
        }
    }
}

TEST_CASE("World-space chase still tracks translation and enforces its angular tolerance", "[AIWorld][Movement][ChaseAngle]")
{
    ChaseAngle const chase(0.0f, 0.15f);
    Position target(25.0f, -12.0f, 0.0f, float(M_PI));
    ChaseAngle const relative = chase.Resolve(target.GetOrientation(), ChaseAngleReference::World);
    float const absolute = target.ToAbsoluteAngle(relative.RelativeAngle);
    REQUIRE(target.GetPositionX() + 3.0f * std::cos(absolute) == Approx(28.0f));
    REQUIRE(target.GetPositionY() + 3.0f * std::sin(absolute) == Approx(-12.0f));
    REQUIRE(relative.IsAngleOkay(target.GetRelativeAngle(28.0f, -12.0f)));
    REQUIRE(!relative.IsAngleOkay(target.GetRelativeAngle(22.0f, -12.0f)));
    REQUIRE(relative.IsAngleOkay(target.ToRelativeAngle(0.14f)));
    REQUIRE(relative.IsAngleOkay(target.ToRelativeAngle(-0.14f)));
    REQUIRE(!relative.IsAngleOkay(target.ToRelativeAngle(0.16f)));
    REQUIRE(!relative.IsAngleOkay(target.ToRelativeAngle(-0.16f)));
}

TEST_CASE("Existing target-relative chase angles keep following target facing", "[Movement][ChaseAngle]")
{
    for (float bearing : { 0.0f, 0.7f, float(M_PI) })
    {
        ChaseAngle const chase(bearing, 0.15f);
        for (float facing : { 0.0f, float(M_PI / 2), float(M_PI), float(2 * M_PI - 0.001f) })
        {
            Position const target(0.0f, 0.0f, 0.0f, facing);
            ChaseAngle const relative = chase.Resolve(target.GetOrientation(), ChaseAngleReference::Target);
            REQUIRE(relative.RelativeAngle == chase.RelativeAngle);
            REQUIRE(relative.Tolerance == chase.Tolerance);
            REQUIRE(relative.IsAngleOkay(target.ToRelativeAngle(facing + bearing)));
            REQUIRE(!relative.IsAngleOkay(target.ToRelativeAngle(facing + bearing + 0.2f)));
        }
    }
}

TEST_CASE("Chase speed boost expires once and ordinary chases have no bonus", "[Movement][ChaseSpeed]")
{
    ChaseSpeedBoost ordinary;
    REQUIRE(ordinary.GetMultiplier() == 1.0f);
    REQUIRE(ordinary.GetRemainingMs() == 0);
    REQUIRE(!ordinary.Update(10000));

    ChaseSpeedBoost sprint(1.35f, 10000);
    REQUIRE(!sprint.Update(0));
    REQUIRE(!sprint.Update(9999));
    REQUIRE(sprint.GetRemainingMs() == 1);
    REQUIRE(sprint.GetMultiplier() == Approx(1.35f));
    // This edge tells the generator to relaunch a running spline even if
    // the target has not moved. It must never wrap or refill the timer.
    REQUIRE(sprint.Update(1));
    REQUIRE(sprint.GetMultiplier() == 1.0f);
    REQUIRE(sprint.GetRemainingMs() == 0);
    REQUIRE(!sprint.Update(std::numeric_limits<uint32>::max()));
    REQUIRE(sprint.GetMultiplier() == 1.0f);

    ChaseSpeedBoost delayedTick(1.35f, 10000);
    REQUIRE(delayedTick.Update(20000));
    REQUIRE(delayedTick.GetMultiplier() == 1.0f);
    REQUIRE(!delayedTick.Update(1));
}

TEST_CASE("Invalid chase speed boosts retain the normal movement speed", "[Movement][ChaseSpeed]")
{
    for (float multiplier : {0.0f, -1.0f, 1.0f, std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()})
    {
        ChaseSpeedBoost boost(multiplier, 10000);
        REQUIRE(boost.GetMultiplier() == 1.0f);
        REQUIRE(boost.GetRemainingMs() == 0);
        REQUIRE(!boost.Update(1000));
    }
    ChaseSpeedBoost noDuration(1.35f, 0);
    REQUIRE(noDuration.GetMultiplier() == 1.0f);
    REQUIRE(!noDuration.Update(1));
}
