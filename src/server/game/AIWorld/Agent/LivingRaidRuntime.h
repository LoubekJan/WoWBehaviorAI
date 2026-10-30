/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGRAIDRUNTIME_H
#define AIWORLD_LIVINGRAIDRUNTIME_H
#include "Action/ActionPosition.h"
#include "AgentType.h"
#include "Agent/LivingReturnPolicy.h"
#include "Agent/LivingFieldRuntime.h"
#include "Agent/LivingSupplyRuntime.h"
#include "Agent/LivingRaidParty.h"
#include "ObjectGuid.h"
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

namespace LivingRaid
{
    constexpr uint64 RaiderSpawn = 901055, GuardSpawn = 901039;
    constexpr uint64 FirstRaider = 901050, LastRaider = 901059;
    inline constexpr std::array<LivingSupply::ConsumerSpec, 6> CampResidents{{
        {901044, 3, AgentType::Guard}, {901045, 3, AgentType::Guard},
        {901046, 3, AgentType::Guard}, {901047, 3, AgentType::Guard},
        {901048, 3, AgentType::Civilian}, {901049, 3, AgentType::Civilian}
    }};
    inline LivingSupply::ConsumerSpec const* CampResident(uint64 spawn)
    {
        for (auto const& resident : CampResidents) if (resident.Spawn == spawn) return &resident;
        return nullptr;
    }
    inline bool CampResidentCapability(uint64 spawn, uint32 map, AgentType type, AgentControlMode control, bool enabled)
    {
        auto resident = CampResident(spawn);
        return enabled && map == 725 && control == AgentControlMode::AIWorldControlled && resident && resident->Type == type;
    }
    constexpr uint32 SourceDepot = 1, CampDepot = 3, Portions = 2, StartStock = 6;
    constexpr float RaiderHomeRadius = 900.0f;
    constexpr uint64 CooldownMs = 120000, DutyMs = 240000;
    inline constexpr ActionPosition Camp{725, 58.6867446881f, 971.2687759381f, 0.15f};
    inline constexpr ActionPosition Source{725, -372.4611532703f, 492.6821677284f, 0.15f};
    inline constexpr ActionPosition GuardPost = Source;
    // Approach the village depot around the authored buildings, using the
    // ordinary worker's proven ground corridor. This does not change home.
    inline constexpr ActionPosition GuardVia{725, -407.3137084990f, 499.3137084990f, 0.15f};
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
    struct SourceSpec
    {
        uint32 Depot;
        std::array<ActionPosition, 9> Route;
        uint32 Count;
        uint32 RouteId = 1;
        ActionPosition const& Point() const { return Route[Count-1]; }
    };
    // Source-specific hints bound every planner query. They do not prove
    // movement: each actual leg still uses the live floor/body/LOS checks.
    inline constexpr std::array<SourceSpec, 4> Sources{{
        {1, ToSource, 9},
        {4, {{{725, 61.066650390625f, 969.0665283203125f, .15f},
            {725, 160, 868, .15f}, {725, 260, 766, 2.0f},
            {725, 362.66656494140625f, 661.333251953125f, .15f}, LivingFields::Fields[0].DepotPoint}}, 5},
        {5, {{{725, 53.599983215332031f, 964.2664794921875f, .15f}, LivingFields::Fields[1].DepotPoint}}, 2},
        {6, {{{725, 53.599983215332031f, 964.2664794921875f, .15f},
            {725, 58, 820, .15f}, {725, 63.999980926513672f, 661.333251953125f, .15f},
            {725, 85.33331298828125f, 597.33331298828125f, .15f},
            {725, 95.999969482421875f, 539.73333740234375f, .48f},
            {725, 106.66663360595703f, 426.66653442382812f, .15f}, LivingFields::Fields[2].DepotPoint}}, 7}
    }};
    // A separately probed eastern approach reaches the right village without
    // walking through the left village. Runtime still proves every real leg.
    inline constexpr SourceSpec RightBypass{6, {{{725, 120, 930, .15f},
        {725, 120, 820, .15f}, {725, 63.999980926513672f, 661.333251953125f, .15f},
        {725, 85.33331298828125f, 597.33331298828125f, .15f},
        {725, 95.999969482421875f, 539.73333740234375f, .48f},
        {725, 106.66663360595703f, 426.66653442382812f, .15f}, LivingFields::Fields[2].DepotPoint}}, 7, 2};
    inline SourceSpec const* SourceFor(uint32 depot, bool fieldsEnabled, uint32 routeId = 1)
    {
        if (fieldsEnabled && depot == RightBypass.Depot && routeId == RightBypass.RouteId) return &RightBypass;
        for (auto const& spec : Sources)
            if (spec.Depot == depot && spec.RouteId == routeId && (fieldsEnabled || depot == SourceDepot)) return &spec;
        return nullptr;
    }
    inline std::optional<uint32> SelectSource(uint32 previousDepot, bool fieldsEnabled,
        std::array<uint32, 4> const& stocks)
    {
        // Rotate among stocked villages. Nearer or faster-producing villages
        // cannot permanently starve another eligible warehouse of attempts.
        size_t start = 0, count = fieldsEnabled ? Sources.size() : 1;
        for (size_t i = 0; i < count; ++i)
            if (Sources[i].Depot == previousDepot) start = (i+1)%count;
        for (size_t offset = 0; offset < count; ++offset)
        {
            size_t index = (start+offset)%count;
            if (stocks[index] >= StartStock) return Sources[index].Depot;
        }
        return std::nullopt;
    }
    inline float ArrivalRadius(ActionPosition const& destination)
    {
        // Match the farmers' tighter approach at these building-side stocks
        // so the thief's next departure does not start on a collision edge.
        for (auto const& field : LivingFields::Fields)
            if (LivingReturnPolicy::SamePosition(destination, field.DepotPoint)) return 1.9f;
        return 3.0f;
    }
    struct GuardTransit
    {
        ObjectGuid Guid;
        std::optional<ActionPosition> Goal;
        bool PassedVia = false;

        static bool Near(ActionPosition const& here, ActionPosition const& point)
        {
            return LivingReturnPolicy::Finite(here) && LivingReturnPolicy::Finite(point) &&
                here.MapId == point.MapId && LivingReturnPolicy::Distance(here, point) <= 3.0f;
        }
        void Reset() { *this = {}; }
        bool ObserveArrival(ActionPosition const& here, bool goalInLineOfSight)
        {
            if (!Goal || !goalInLineOfSight || !Near(here, *Goal)) return false;
            Reset();
            return true;
        }
        std::optional<ActionPosition> Select(ObjectGuid guid, ActionPosition const& here,
            ActionPosition const& goal, bool goalInLineOfSight, bool viaInLineOfSight)
        {
            if (guid.IsEmpty() || !LivingReturnPolicy::Finite(here) || !LivingReturnPolicy::Finite(goal) ||
                here.MapId != GuardVia.MapId || goal.MapId != here.MapId)
            {
                Reset();
                return std::nullopt;
            }
            if (Guid != guid || !Goal || !LivingReturnPolicy::SamePosition(*Goal, goal))
            {
                Reset();
                Guid = guid;
                Goal = goal;
            }
            // A guard already at its real destination must not be sent away
            // just to visit a route hint. Completing a trip clears its cursor.
            if (ObserveArrival(here, goalInLineOfSight)) return goal;
            if (viaInLineOfSight && Near(here, GuardVia)) PassedVia = true;
            return PassedVia ? goal : GuardVia;
        }
    };

    struct PartyRuntime
    {
        LivingRaidParty::Party Coordination;
        std::unordered_map<uint64, ObjectGuid> Lives;
        uint64 RallyReadyAtMs = 0;
    };

    struct State
    {
        ObjectGuid Guid;
        uint64 PartyId = 0, TargetSpawnId = 0, CampReadyAtMs = 0;
        uint64 AttemptId = 0, NextAtMs = 0, StartedAtMs = 0;
        uint64 Noticed = 0, Interceptions = 0, Thefts = 0, Delivered = 0, Prevented = 0;
        uint64 StolenAtStart = 0, LastNoticedAttempt = 0, LastInterceptionAttempt = 0;
        uint32 Leg = 0;
        // The target changes only when a new attempt begins. Keep it through
        // interruption/return so theft receipts and the reverse route agree.
        uint32 SelectedSourceDepot = SourceDepot, PreviousSourceDepot = 0;
        uint32 SelectedRouteId = 1;
        uint64 LastStockObservationAtMs = 0, LastSurveyAtMs = 0, LastGuardObservationAtMs = 0;
        float MemoryRisk = 0;
        std::string PlanningReason = "NOT_EVALUATED";
        bool Active = false, Returning = false, DutyAtPost = false, CombatNoted = false;
        std::string Phase = "IDLE", Outcome = "MATERIALIZED";
        GuardTransit Transit;
    };

    inline bool RaiderCapability(uint64 spawn, uint32 map, AgentType type,
        AgentControlMode control, bool enabled, bool allDefias = false)
    {
        return enabled && (spawn == RaiderSpawn || (allDefias && spawn >= FirstRaider && spawn <= LastRaider)) && map == 725 && type == AgentType::Combatant &&
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
