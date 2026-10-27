/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGRAIDRUNTIME_H
#define AIWORLD_LIVINGRAIDRUNTIME_H
#include "Action/ActionPosition.h"
#include "AgentType.h"
#include "Agent/LivingReturnPolicy.h"
#include "ObjectGuid.h"
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace LivingRaid
{
    constexpr uint64 RaiderSpawn = 901055, GuardSpawn = 901039;
    constexpr uint32 SourceDepot = 1, CampDepot = 3, Portions = 2, StartStock = 6;
    constexpr float RaiderHomeRadius = 900.0f;
    constexpr uint64 CooldownMs = 120000, DutyMs = 240000;
    inline constexpr ActionPosition Camp{725, 58.6867446881f, 971.2687759381f, 0.15f};
    inline constexpr ActionPosition Source{725, -372.4611532703f, 492.6821677284f, 0.15f};
    inline constexpr ActionPosition GuardPost = Source;
    // Reviewed transit anchors split the long journey into bounded searches.
    // Every actual leg is still grounded and checked against live collisions.
    inline constexpr std::array<ActionPosition, 9> ToSource{{
        {725, -80.0f, 840.0f, 0.15f},
        // Follow the northern crossing. A direct approach from the camp
        // toward the bridge can select deep water on a swim-capable mesh.
        {725, -160.0f, 860.0f, 0.15f},
        {725, -201.6000671387f, 861.0665283203f, -2.2201099396f},
        // Leave the west ramp before turning south; a diagonal departure
        // from the deck crosses its steep side instead of the landing.
        {725, -214.0f, 861.0665283203f, 0.15f},
        {725, -235.0f, 790.0f, -0.62f},
        {725, -235.0f, 725.0f, 0.15f},
        {725, -264.0f, 680.0f, 0.15f},
        {725, -340.0f, 580.0f, 0.15f},
        Source
    }};
    struct State
    {
        ObjectGuid Guid;
        uint64 AttemptId = 0, NextAtMs = 0, StartedAtMs = 0;
        uint64 Noticed = 0, Interceptions = 0, Thefts = 0, Delivered = 0, Prevented = 0;
        uint64 StolenAtStart = 0, LastNoticedAttempt = 0, LastInterceptionAttempt = 0;
        uint32 Leg = 0;
        bool Active = false, Returning = false, DutyAtPost = false;
        std::string Phase = "IDLE", Outcome = "MATERIALIZED";
    };

    inline bool RaiderCapability(uint64 spawn, uint32 map, AgentType type,
        AgentControlMode control, bool enabled)
    {
        return enabled && spawn == RaiderSpawn && map == 725 && type == AgentType::Combatant &&
            control == AgentControlMode::AIWorldControlled;
    }

    inline bool GuardCapability(uint64 spawn, uint32 map, AgentType type,
        AgentControlMode control, bool enabled)
    {
        return enabled && spawn == GuardSpawn && map == 725 && type == AgentType::Guard &&
            control == AgentControlMode::AIWorldControlled;
    }

    // Native paths may contain dense points along one perfectly straight
    // segment. Coalesce only that segment; never replace a bend or a crest
    // with a chord. The caller still proves the complete executed leg again.
    inline std::vector<ActionPosition> CoalesceStraightLegs(ActionPosition const& from,
        std::vector<ActionPosition> const& corridor, float maxLength = 14.0f)
    {
        using namespace LivingReturnPolicy;
        if (!Finite(from) || !std::isfinite(maxLength) || maxLength <= 1.0f ||
            maxLength > MaxStepLength || corridor.size() > 128)
            return {};
        ActionPosition previous = from;
        for (auto const& point : corridor)
        {
            if (!Finite(point) || point.MapId != from.MapId || Distance(previous, point) > maxLength + 0.0001f)
                return {};
            previous = point;
        }
        std::vector<ActionPosition> result;
        ActionPosition anchor = from, directionEnd = from, pending = from;
        bool havePending = false;
        for (auto const& point : corridor)
        {
            if (SamePosition(point, pending)) continue;
            if (!havePending)
            {
                directionEnd = pending = point;
                havePending = true;
                continue;
            }
            double ux = double(directionEnd.X)-anchor.X, uy = double(directionEnd.Y)-anchor.Y,
                uz = double(directionEnd.Z)-anchor.Z;
            double vx = double(point.X)-anchor.X, vy = double(point.Y)-anchor.Y, vz = double(point.Z)-anchor.Z;
            double cx = uy*vz-uz*vy, cy = uz*vx-ux*vz, cz = ux*vy-uy*vx;
            double squaredDirection = ux*ux+uy*uy+uz*uz;
            double forward = (double(point.X)-pending.X)*ux + (double(point.Y)-pending.Y)*uy +
                (double(point.Z)-pending.Z)*uz;
            // One ten-thousandth of a yard permits only coordinate rounding.
            // Compare against the fixed first ray, so repeated small bends
            // cannot accumulate into a smoothed curve.
            bool straight = forward > 0 && squaredDirection > 0 &&
                cx*cx+cy*cy+cz*cz <= 1.0e-8 * squaredDirection;
            if (!straight || Distance(anchor, point) > maxLength)
            {
                result.push_back(pending);
                anchor = pending;
                directionEnd = point;
            }
            pending = point;
        }
        if (havePending) result.push_back(pending);
        return result;
    }
}
#endif
