/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef AIWORLD_AGENTTELEMETRYSNAPSHOT_H
#define AIWORLD_AGENTTELEMETRYSNAPSHOT_H

#include "Agent/AgentSnapshot.h"
#include "Agent/AgentType.h"
#include "Action/ActionType.h"
#include "Action/ActionPosition.h"
#include "Agent/AgentEconomyState.h"
#include "Faction/WorldFactionId.h"
#include "Goal/GoalType.h"
#include "Needs/NeedsState.h"
#include "Scheduler/SimulationTier.h"
#include <array>
#include <optional>
#include <string>
#include <vector>

// Strings are owned copies: LivingRoleState and the command diagnostics use
// char const*, which must not be carried through the asynchronous boundary.
struct ReturnRecoveryTelemetry
{
    uint32 Failures = 0, TrailPoints = 0, Candidates = 0, PathType = 0;
    uint64 RetryMs = 0, StalledMs = 0;
    std::string Strategy, Failure;
    std::array<uint32, 7> Rejections{};
    float RequestedZ = 0.0f;
    std::optional<float> ResolvedZ;
};

struct LivingRoleTelemetry
{
    bool Enabled = false;
    bool ExtensionsEnabled = false;
    std::string Role, Status, Phase, Activity, Awareness, MovementPurpose;
    float Caution = 0.0f;
    uint64 StartedAtMs = 0;
    uint64 DecisionWaitMs = 0;
    uint64 DangerRemainingMs = 0;
    uint64 AlarmRemainingMs = 0;
    std::string HuntStatus, HuntEnd, AssistStatus;
    uint32 NearbyPrey = 0, AttackablePrey = 0, NearbyAllies = 0, AlliesInCombat = 0;
    uint64 CompanionSpawnId = 0, LastHuntTargetSpawnId = 0;
    std::optional<float> HuntTargetDistance, PreyRunSpeed, PreyMoveSpeed;
    std::optional<float> SprintMultiplier;
    std::optional<uint32> SprintRemainingMs;
    std::optional<ReturnRecoveryTelemetry> ReturnRecovery;
};

struct MovementTelemetry
{
    bool Moving = false, Blocked = false, CannotReachTarget = false, Evading = false;
    float RunSpeed = 0.0f, MoveSpeed = 0.0f, HomeDistance = 0.0f;
};

struct GroupTelemetry
{
    uint64 Id = 0;
    std::string Kind, Profile;
    uint32 MemberCount = 0;
    float Resources = 0.0f;
    ActionPosition Territory;
};

struct TargetTelemetry
{
    uint64 SpawnId = 0;
    uint32 Entry = 0;
    std::string Name;
    ActionPosition Position;
};

// A value-only observation captured on the world thread. No Creature, Map,
// registry, or other world-owned object crosses the asynchronous boundary.
struct AgentTelemetrySnapshot
{
    AgentId Agent;
    uint64 SpawnId = 0;
    uint32 Entry = 0;
    std::string Name;
    AgentType Type = AgentType::Unclassified;
    AgentControlMode ControlMode = AgentControlMode::ObserveOnly;
    WorldFactionId WorldFaction;
    std::optional<uint32> ReputationFactionId, FactionTemplateId;
    AgentWorldState WorldState = AgentWorldState::Abstract;
    SimulationTier Tier = SimulationTier::Background;

    // Always authoritative spawn coordinates. Live contains actual current
    // coordinates and stats only when a Creature was resolved during capture.
    uint32 MapId = 0;
    float SpawnX = 0.0f;
    float SpawnY = 0.0f;
    float SpawnZ = 0.0f;
    std::optional<AgentSnapshot> Live;

    NeedsState Needs;
    std::optional<GoalType> Goal;
    std::optional<float> GoalUtility;
    std::optional<GoalType> RoutineGoal;
    std::optional<ActionType> Action;
    std::optional<uint64> GroupId;

    // Persistent data remains available for background agents. All engine and
    // materialization-local observations below are absent when not current.
    AgentEconomyState Economy;
    std::optional<ActionPosition> Home, Work;
    std::vector<GroupTelemetry> Groups;
    std::optional<LivingRoleTelemetry> LivingRole;
    std::optional<MovementTelemetry> Movement;
    bool LivingWolf = false;
    std::optional<uint64> MealTargetSpawnId;
    std::optional<GoalType> EffectiveGoal, ActionSourceGoal, CoordinationGoal;
    std::string GoalOwner;
    std::optional<uint64> ActionStartedAtMs, CoordinationGroupId;
    std::string CoordinationPhase, RoutineActivity;
    std::optional<ActionPosition> Destination;
    std::optional<TargetTelemetry> Target;
};

#endif // AIWORLD_AGENTTELEMETRYSNAPSHOT_H
