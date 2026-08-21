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
#include "Agent/AgentSnapshot.h"
#include "Config.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"

AIWorldMgr* AIWorldMgr::instance()
{
    static AIWorldMgr instance;
    return &instance;
}

void AIWorldMgr::Initialize()
{
    _enabled = sConfigMgr->GetBoolDefault("AIWorld.Enable", false);
    if (!_enabled)
    {
        TC_LOG_INFO("ai.world", "AIWorld disabled (AIWorld.Enable = 0)");
        return;
    }

    _snapshotIntervalMs = uint32(sConfigMgr->GetIntDefault("AIWorld.SnapshotIntervalMs", 5000));
    _testMapId = uint32(sConfigMgr->GetIntDefault("AIWorld.TestMapId", 0));
    _testSpawnId = uint64(sConfigMgr->GetIntDefault("AIWorld.TestSpawnId", 0));
    _snapshotTimer = 0;
    _snapshotSequence = 0;

    TC_LOG_INFO("ai.world", "AIWorld enabled");
    if (_testSpawnId)
        TC_LOG_INFO("ai.world", "test agent map={} spawn={} interval={}ms", _testMapId, _testSpawnId, _snapshotIntervalMs);
}

void AIWorldMgr::Update(uint32 diff)
{
    if (!_enabled || !_testSpawnId)
        return;

    _snapshotTimer += diff;
    if (_snapshotTimer < _snapshotIntervalMs)
        return;

    _snapshotTimer = 0;
    CaptureTestAgentSnapshot();
}

void AIWorldMgr::CaptureTestAgentSnapshot()
{
    Map* map = sMapMgr->FindBaseNonInstanceMap(_testMapId);
    if (!map)
    {
        TC_LOG_DEBUG("ai.world", "AIWorld snapshot skipped: map {} not currently loaded", _testMapId);
        return;
    }

    Creature* creature = map->GetCreatureBySpawnId(_testSpawnId);
    if (!creature)
    {
        TC_LOG_DEBUG("ai.world", "AIWorld snapshot skipped: spawn {} not currently loaded", _testSpawnId);
        return;
    }

    AgentSnapshot snapshot;
    snapshot.SpawnId = _testSpawnId;
    snapshot.Guid = creature->GetGUID();
    snapshot.Entry = creature->GetEntry();
    snapshot.MapId = creature->GetMapId();
    snapshot.X = creature->GetPositionX();
    snapshot.Y = creature->GetPositionY();
    snapshot.Z = creature->GetPositionZ();
    snapshot.Orientation = creature->GetOrientation();
    snapshot.Health = creature->GetHealth();
    snapshot.MaxHealth = creature->GetMaxHealth();
    snapshot.Alive = creature->IsAlive();
    snapshot.InCombat = creature->IsInCombat();
    snapshot.SnapshotSequence = ++_snapshotSequence;

    TC_LOG_INFO("ai.agent", "snapshot seq={} spawn={} entry={} hp={}/{} position=({:.1f}, {:.1f}, {:.1f}) combat={}",
        snapshot.SnapshotSequence, snapshot.SpawnId, snapshot.Entry,
        snapshot.Health, snapshot.MaxHealth,
        snapshot.X, snapshot.Y, snapshot.Z,
        snapshot.InCombat);
}

void AIWorldMgr::Shutdown()
{
    if (!_enabled)
        return;

    TC_LOG_INFO("ai.world", "AIWorld shutting down");
    _enabled = false;
    TC_LOG_INFO("ai.world", "AIWorld stopped");
}
