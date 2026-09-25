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

#include "AIWorldMgr.h"
#include "Creature.h"
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include <algorithm>

void AIWorldMgr::CaptureTelemetry(Map* elwynnMap)
{
    uint64 nowMs = GetCurrentTimeMs();
    std::vector<AgentTelemetrySnapshot> snapshots;
    std::vector<AgentId> ids = _registry.GetAgents();
    snapshots.reserve(std::min(ids.size(), _telemetrySpawnIds.size()));
    for (AgentId id : ids)
    {
        AgentRecord const* record = _registry.Find(id);
        if (!record || record->MapId != 0 || record->ControlMode != AgentControlMode::AIWorldControlled ||
            _telemetrySpawnIds.find(record->SpawnId) == _telemetrySpawnIds.end())
            continue;

        CreatureData const* spawn = sObjectMgr->GetCreatureData(uint32(record->SpawnId));
        if (!spawn)
            continue;

        AgentTelemetrySnapshot item;
        item.Agent = id;
        item.SpawnId = record->SpawnId;
        item.Entry = spawn->id;
        if (CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(item.Entry))
            item.Name = creatureTemplate->Name;
        item.Type = record->Type;
        item.ControlMode = record->ControlMode;
        item.WorldFaction = record->WorldFaction;
        uint32 reputationFactionId;
        if (_worldFactionReputationCatalog.TryResolve(record->WorldFaction, reputationFactionId))
            item.ReputationFactionId = reputationFactionId;
        item.MapId = record->MapId;
        item.SpawnX = spawn->spawnPoint.GetPositionX();
        item.SpawnY = spawn->spawnPoint.GetPositionY();
        item.SpawnZ = spawn->spawnPoint.GetPositionZ();
        item.Needs = record->Needs;
        item.Economy = record->EconomyState;
        if (record->HomeLocation)
            item.Home = ActionPosition{ record->HomeLocation->MapId, record->HomeLocation->X, record->HomeLocation->Y, record->HomeLocation->Z };
        if (record->WorkLocation)
            item.Work = ActionPosition{ record->WorkLocation->MapId, record->WorkLocation->X, record->WorkLocation->Y, record->WorkLocation->Z };
        if (record->ActiveGoalState)
        {
            item.Goal = record->ActiveGoalState->Type;
            item.GoalUtility = record->ActiveGoalState->Utility;
        }
        if (record->RoutineGoalState)
            item.RoutineGoal = record->RoutineGoalState->Type;
        if (record->ActiveActionState)
        {
            item.Action = record->ActiveActionState->Type;
            item.ActionSourceGoal = record->ActiveActionState->SourceGoal;
            item.ActionStartedAtMs = record->ActiveActionState->StartedAtMs;
        }
        if (record->GroupCoordinationGoalState)
        {
            auto const& coordination = *record->GroupCoordinationGoalState;
            item.CoordinationGoal = coordination.Type;
            item.CoordinationGroupId = coordination.SourceGroup.Value;
            if (coordination.Type == GoalType::Hunt)
                item.CoordinationPhase = ToString(coordination.Phase);
        }
        if (record->RoutineActivityState)
            item.RoutineActivity = ToString(record->RoutineActivityState->Type);
        if (item.Goal) { item.EffectiveGoal = item.Goal; item.GoalOwner = "INDIVIDUAL"; }
        else if (item.RoutineGoal) { item.EffectiveGoal = item.RoutineGoal; item.GoalOwner = "ROUTINE"; }
        else if (item.CoordinationGoal) { item.EffectiveGoal = item.CoordinationGoal; item.GoalOwner = "GROUP"; }
        std::vector<GroupId> groups = _groupRegistry.GetGroupsOfMember(id);
        std::sort(groups.begin(), groups.end());
        if (!groups.empty())
            item.GroupId = groups.front().Value;
        for (GroupId groupId : groups)
            if (AgentGroupRecord const* group = _groupRegistry.Find(groupId))
            {
                GroupTelemetry membership;
                membership.Id = groupId.Value;
                membership.Kind = ToString(group->Kind);
                membership.Profile = ToString(group->ProfileId);
                membership.MemberCount = uint32(group->Members.size());
                membership.Resources = group->Resources;
                membership.Territory = { group->TerritoryMapId, group->TerritoryX, group->TerritoryY, group->TerritoryZ };
                item.Groups.push_back(std::move(membership));
            }

        // GetCreatureBySpawnId only checks already-loaded map state. This
        // capture never loads a grid to satisfy the viewer.
        if (Creature* creature = elwynnMap ? elwynnMap->GetCreatureBySpawnId(record->SpawnId) : nullptr)
        {
            AgentSnapshot live;
            live.Agent = id;
            live.SpawnId = record->SpawnId;
            live.Entry = creature->GetEntry();
            live.MapId = creature->GetMapId();
            live.X = creature->GetPositionX();
            live.Y = creature->GetPositionY();
            live.Z = creature->GetPositionZ();
            live.Orientation = creature->GetOrientation();
            live.Health = creature->GetHealth();
            live.MaxHealth = creature->GetMaxHealth();
            live.Alive = creature->IsAlive();
            live.InCombat = creature->IsInCombat();
            live.SnapshotSequence = record->SnapshotSequence;
            item.Live = live;
            item.FactionTemplateId = creature->GetFaction();
            item.WorldState = AgentWorldState::Materialized;
            auto tier = _agentSimulationTier.find(id.Value);
            item.Tier = tier != _agentSimulationTier.end() && tier->second != SimulationTier::Background
                ? tier->second : SimulationTier::Active;

            MovementTelemetry movement;
            movement.Moving = !creature->IsStopped();
            movement.Blocked = creature->HasUnitState(UNIT_STATE_NOT_MOVE) || creature->IsMovementPreventedByCasting();
            movement.CannotReachTarget = creature->CanNotReachTarget();
            movement.Evading = creature->IsInEvadeMode();
            movement.RunSpeed = creature->GetSpeed(MOVE_RUN);
            movement.MoveSpeed = movement.Moving ? creature->movespline->Velocity() : 0.0f;
            movement.HomeDistance = creature->GetExactDist2d(&creature->GetHomePosition());
            item.Movement = movement;

            // An old incarnation's role/action state is not current telemetry.
            if (record->WorldState == AgentWorldState::Materialized && record->RuntimeGuid == creature->GetGUID())
            {
                auto const info = DescribeLivingRole(*creature, *record);
                LivingRoleTelemetry role;
                role.Enabled = info.Enabled; role.ExtensionsEnabled = info.ExtensionsEnabled;
                role.Role = info.Role; role.Status = info.Status;
                role.Phase = info.Phase; role.Activity = info.Activity;
                role.Awareness = info.Awareness; role.MovementPurpose = info.MovementPurpose;
                role.Caution = info.Caution;
                role.StartedAtMs = record->LivingRole.StartedAtMs;
                role.DecisionWaitMs = record->LivingRole.NextDecisionAtMs > nowMs ? record->LivingRole.NextDecisionAtMs - nowMs : 0;
                role.DangerRemainingMs = record->LivingRole.DangerUntilMs > nowMs ? record->LivingRole.DangerUntilMs - nowMs : 0;
                role.AlarmRemainingMs = record->LivingRole.AlarmUntilMs > nowMs ? record->LivingRole.AlarmUntilMs - nowMs : 0;
                role.HuntStatus = info.HuntStatus ? info.HuntStatus : "";
                role.HuntEnd = info.HuntEnd;
                role.AssistStatus = info.AssistStatus ? info.AssistStatus : "";
                role.NearbyPrey = info.NearbyPrey; role.AttackablePrey = info.AttackablePrey;
                role.NearbyAllies = info.NearbyAllies; role.AlliesInCombat = info.AlliesInCombat;
                role.CompanionSpawnId = info.CompanionSpawnId;
                role.LastHuntTargetSpawnId = info.HuntTargetSpawnId;
                auto const& recovery = record->LivingRole;
                if (recovery.ReturningHome || recovery.ReturnFailures)
                {
                    ReturnRecoveryTelemetry diagnostic;
                    diagnostic.Failures = recovery.ReturnFailures;
                    diagnostic.TrailPoints = uint32(recovery.ReturnTrail.size());
                    diagnostic.RetryMs = recovery.ReturnRetryAtMs > nowMs ? recovery.ReturnRetryAtMs - nowMs : 0;
                    diagnostic.StalledMs = recovery.ReturnStalledSinceMs && nowMs >= recovery.ReturnStalledSinceMs ? nowMs - recovery.ReturnStalledSinceMs : 0;
                    diagnostic.Strategy = recovery.ReturnStrategy;
                    diagnostic.Failure = recovery.ReturnFailure;
                    diagnostic.Candidates = recovery.ReturnDiagnostics.Candidates;
                    diagnostic.PathType = recovery.ReturnDiagnostics.PathType;
                    diagnostic.Rejections = recovery.ReturnDiagnostics.Rejected;
                    diagnostic.RequestedZ = recovery.ReturnDiagnostics.RequestedZ;
                    diagnostic.ResolvedZ = recovery.ReturnDiagnostics.ResolvedZ;
                    role.ReturnRecovery = std::move(diagnostic);
                }
                if (info.HuntStatus)
                {
                    role.SprintMultiplier = info.SprintMultiplier;
                    role.SprintRemainingMs = info.SprintRemainingMs;
                    if (info.HuntTargetDistance >= 0.0f)
                    {
                        role.HuntTargetDistance = info.HuntTargetDistance;
                        role.PreyRunSpeed = info.PreyRunSpeed;
                        role.PreyMoveSpeed = info.PreyMoveSpeed;
                    }
                }
                item.LivingRole = std::move(role);
                item.LivingWolf = IsLivingWolf(*record);
                ObjectGuid targetGuid;
                if (record->ActiveActionState)
                {
                    item.Destination = record->ActiveActionState->Destination;
                    if (record->ActiveActionState->Target)
                        targetGuid = record->ActiveActionState->Target->Guid;
                }
                if (targetGuid.IsEmpty() && item.GoalOwner == "GROUP" && record->GroupCoordinationGoalState)
                    targetGuid = record->GroupCoordinationGoalState->TargetGuid;
                // Only already-loaded creatures are resolved; no player data is exported.
                if (Creature* target = ObjectAccessor::GetCreature(*creature, targetGuid))
                    item.Target = TargetTelemetry{ target->GetSpawnId(), target->GetEntry(), target->GetName(),
                        { target->GetMapId(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ() } };
                if (Creature* meal = ObjectAccessor::GetCreature(*creature, record->WolfMealTarget))
                    item.MealTargetSpawnId = meal->GetSpawnId();
            }
        }
        snapshots.push_back(std::move(item));
    }

    std::optional<MemoryPageTelemetry> memoryPage;
    if (auto request = _telemetryExporter->TakeMemoryRequest(); request &&
        std::any_of(snapshots.begin(), snapshots.end(), [&](auto const& item) { return item.Agent == request->Agent; }))
    {
        auto page = _longTermMemory.GetPage(request->Agent, request->Offset, TelemetryMemoryPageSize, request->Anchor);
        memoryPage.emplace();
        memoryPage->Request = *request;
        memoryPage->Total = page.Total;
        memoryPage->Anchor = page.Anchor;
        auto entityName = [](WorldEntityRef const& entity) -> std::string
        {
            // Historical NPC references only; no player lookup or runtime GUID export.
            if (!entity.Guid.IsEmpty() && entity.Guid.IsPlayer())
                return {};
            if (auto const* creatureTemplate = sObjectMgr->GetCreatureTemplate(entity.Entry))
                return creatureTemplate->Name;
            return {};
        };
        for (auto const& memory : page.Records)
            memoryPage->Records.push_back({memory, entityName(memory.Actor), entityName(memory.Target)});
    }
    _telemetryExporter->Submit(std::move(snapshots), nowMs, std::move(memoryPage));
}

