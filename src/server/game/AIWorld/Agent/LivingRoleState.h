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

#ifndef AIWORLD_LIVINGROLESTATE_H
#define AIWORLD_LIVINGROLESTATE_H

#include "LivingRolePolicy.h"
#include "Action/ActionPosition.h"
#include "ObjectGuid.h"
#include "Goal/GoalType.h"

// One materialization's local activity. Never persisted; no engine pointers.
struct LivingRoleState
{
    enum class Phase : uint8 { Idle, Moving, Acting, Hunting, Feeding, Defending, Fleeing, SeekingSafety, Investigating };
    ObjectGuid RuntimeGuid;
    ObjectGuid TargetGuid;
    Phase CurrentPhase = Phase::Idle;
    LivingRolePolicy::Activity Activity = LivingRolePolicy::Activity::None;
    GoalType SourceGoal = GoalType::LocalActivity;
    ActionPosition Destination;
    uint64 StartedAtMs = 0;
    uint64 NextDecisionAtMs = 0;
    uint64 NextSenseAtMs = 0;
    uint64 Cycle = 0;
    uint64 DefenseCooldownUntilMs = 0;
    float ChaseBearing = 0.0f;
    uint8 OwnedStandState = 0;
    char const* LastHuntStatus = "NOT_SCANNED";
    char const* LastHuntEnd = "NONE";
    ObjectGuid LastHuntTargetGuid;
    ObjectGuid UnreachablePreyGuid;
    uint64 UnreachablePreyUntilMs = 0;
    char const* LastAssistStatus = "NOT_SCANNED";
    uint32 NearbyPrey = 0;
    uint32 AttackablePrey = 0;
    uint32 NearbyAllies = 0;
    uint32 AlliesInCombat = 0;
    // Local observations expire; no entity pointers or cross-grid broadcasts.
    ObjectGuid AlarmThreatGuid;
    ObjectGuid CompanionGuid;
    ActionPosition DangerPosition;
    uint64 DangerUntilMs = 0;
    uint64 AlarmUntilMs = 0;
    uint64 NextAlarmAtMs = 0;
    uint64 NextInvestigationAtMs = 0;
    uint64 WorkWindowAtStart = 0;
    // Recovery survives local animations, but never a new materialization.
    uint32 ReturnFailures = 0;
    uint64 ReturnRetryAtMs = 0;
    char const* ReturnFailure = "NONE";
    ActionPosition MoveStart;
    bool StockMeal = false;
    uint64 ForageUntilMs = 0;
    uint64 NextForageAtMs = 0;
    uint32 ForageLeg = 0;
    bool HasForageWaypoint = false;
    ActionPosition ForageWaypoint;
    char const* Awareness = "QUIET";
    char const* MovementPurpose = "NONE";
};
#endif
