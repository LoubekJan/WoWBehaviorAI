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

#ifndef AIWORLD_AIWORLDMGR_H
#define AIWORLD_AIWORLDMGR_H

#include "Define.h"
#include "Inference/AIClient.h"
#include <memory>

namespace Trinity::Asio { class IoContext; }

// Entry point for the AIWorld subsystem. Driven from the world update thread
// only (called after sMapMgr->Update() in World::Update()) - never spawns its
// own thread and never mutates Creature/Player/Map state. Fully inert unless
// AIWorld.Enable = 1.
class TC_GAME_API AIWorldMgr
{
    public:
        static AIWorldMgr* instance();

        void Initialize(Trinity::Asio::IoContext& ioContext);
        void Update(uint32 diff);
        void Shutdown();

        bool IsEnabled() const { return _enabled; }

    private:
        AIWorldMgr() = default;
        ~AIWorldMgr() = default;
        AIWorldMgr(AIWorldMgr const&) = delete;
        AIWorldMgr& operator=(AIWorldMgr const&) = delete;

        void CaptureTestAgentSnapshot();

        bool _enabled = false;

        uint32 _snapshotIntervalMs = 5000;
        uint32 _snapshotTimer = 0;

        uint32 _testMapId = 0;
        uint64 _testSpawnId = 0;

        uint64 _snapshotSequence = 0;

        // Owned for the process lifetime, deliberately not reset in
        // Shutdown(): by the time Shutdown() runs, the io_context it was
        // built on may already be stopped and its worker threads joined
        // (see Main.cpp), so there is no safe moment left to tear it down
        // early. Setting _enabled = false just stops new submissions.
        std::unique_ptr<AIClient> _aiClient;

        uint32 _healthIntervalMs = 10000;
        uint32 _healthTimer = 0;
};

#define sAIWorldMgr AIWorldMgr::instance()

#endif // AIWORLD_AIWORLDMGR_H
