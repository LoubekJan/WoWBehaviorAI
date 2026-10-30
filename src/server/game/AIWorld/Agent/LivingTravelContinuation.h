/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGTRAVELCONTINUATION_H
#define AIWORLD_LIVINGTRAVELCONTINUATION_H

#include "AgentRecord.h"
#include "Action/ActionCompletion.h"
#include "Action/RecoveryMovement.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>

// Pure ownership checks for waking a continuing local journey. The caller
// still proves live scope, incarnation and physical arrival before waking it;
// these checks never grant permission to move or to bypass planning admission.
namespace LivingTravelContinuation
{
    inline bool DirectedPurpose(char const* purpose)
    {
        if (!purpose) return false;
        std::string_view value(purpose);
        return value == "DELIVER_FOOD" || value == "GET_DEPOT_FOOD" || value == "RETURN_DEPOT" ||
            value == "TO_HUNTING_GROUND" || value == "SUPPLY_SEARCH" || value == "RETURN_CONSUMER_HOME" ||
            value == "RAID_SOURCE" || value == "RAID_CAMP" || value == "RAID_EMPTY_RETURN" ||
            value == "GUARD_HOME" || value == "GUARD_POST" || value == "RETURN_HOME" || value == "FORAGE_SEARCH" ||
            value == "FARM_FIELD" || value == "FARM_DEPOT" || value == "FARM_HOME" ||
            value == "CARAVAN_RALLY" || value == "CARAVAN_VILLAGE" || value == "CARAVAN_CITY";
    }

    namespace Detail
    {
        inline bool OwnedJourney(AgentRecord const& record)
        {
            auto const& state = record.LivingRole;
            return record.Id && record.WorldState == AgentWorldState::Materialized &&
                record.ControlMode == AgentControlMode::AIWorldControlled &&
                !record.RuntimeGuid.IsEmpty() && record.RuntimeGuid == state.RuntimeGuid &&
                LivingReturnPolicy::Finite(state.Destination) && record.MapId == state.Destination.MapId &&
                state.CurrentPhase == LivingRoleState::Phase::Moving && state.SourceGoal == GoalType::LocalActivity &&
                state.StartedAtMs && record.ActiveGoalState && record.ActiveGoalState->Type == GoalType::LocalActivity &&
                record.ActiveGoalState->StartedAtMs == state.StartedAtMs && DirectedPurpose(state.MovementPurpose);
        }
    }

    // Called before completion clears ActiveActionState, so a stale callback
    // cannot wake a different goal, a changed endpoint or a replacement action.
    inline bool CanWake(AgentRecord const& record, ActionCompletion const& completion)
    {
        if (!Detail::OwnedJourney(record) || !record.ActiveActionState) return false;
        auto const& state = record.LivingRole;
        auto const& action = *record.ActiveActionState;
        return completion.Actor == record.Id && completion.Type == ActionType::MoveTo &&
            completion.SourceGoal == GoalType::LocalActivity && completion.GoalStartedAtMs == state.StartedAtMs &&
            completion.Status == ActionCompletionStatus::Succeeded && completion.Reason == ActionCompletionReason::Arrived &&
            action.Type == ActionType::MoveTo && action.SourceGoal == GoalType::LocalActivity &&
            action.StartedAtMs == state.StartedAtMs && action.GoalStartedAtMs == state.StartedAtMs &&
            action.Destination && RecoveryMovement::SamePoint(*action.Destination, state.Destination);
    }

    // Only a verified completed action can set this transient pending flag.
    // A newly running action must never borrow the old arrival's readiness.
    inline bool Ready(AgentRecord const& record)
    {
        return Detail::OwnedJourney(record) && record.LivingRole.MoveArrivalPending && !record.ActiveActionState;
    }

    // Supply searches yield under the ordinary planning budget, then retry at
    // their requested deadline. The caller checks live scope, supply capability
    // and peaceful stationary motion; this only queues another bounded pass.
    inline std::optional<uint64> PlanningRetryAt(AgentRecord const& record, uint64 nowMs)
    {
        auto const& state = record.LivingRole;
        if (!record.Id || record.WorldState != AgentWorldState::Materialized ||
            record.ControlMode != AgentControlMode::AIWorldControlled || record.RuntimeGuid.IsEmpty() ||
            record.RuntimeGuid != state.RuntimeGuid || state.CurrentPhase != LivingRoleState::Phase::Idle ||
            !state.Planning.Deferred || !state.Planning.Reason || !state.Planning.Stage || !state.MovementPurpose ||
            std::string_view(state.Planning.Reason) != "WORK_BUDGET" ||
            std::string_view(state.Planning.Stage) != "SUPPLY_CORRIDOR" ||
            std::string_view(state.MovementPurpose) != "SUPPLY_PLANNING_DEFERRED" ||
            record.ActiveActionState || record.GroupCoordinationGoalState ||
            (record.ActiveGoalState && record.ActiveGoalState->Type != GoalType::LocalActivity) ||
            nowMs == std::numeric_limits<uint64>::max())
            return std::nullopt;
        return std::max(nowMs + 1, state.NextDecisionAtMs);
    }
}
#endif
