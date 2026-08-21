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

#ifndef AIWORLD_AICLIENT_H
#define AIWORLD_AICLIENT_H

#include "AIResponse.h"
#include "Define.h"
#include <memory>
#include <string>

namespace Trinity::Asio { class IoContext; }

// Async, fire-and-forget HTTP bridge to ai-server, built on the worldserver's
// shared io_context/thread pool (the same one WorldSocketMgr etc. already
// run on) rather than a dedicated thread.
//
// SubmitHealthCheck() and TryPopResponse() must only be called from the
// world update thread and never block: SubmitHealthCheck() posts the actual
// resolve/connect/write/read work onto the io_context and returns
// immediately; the eventual result (success, HTTP status, latency - or a
// timeout) is handed back through a lock-free queue that TryPopResponse()
// drains on a later world tick. The world update thread never waits on a
// socket, a future, or ai-server itself.
class TC_GAME_API AIClient
{
    public:
        AIClient(Trinity::Asio::IoContext& ioContext, std::string host, std::string port, uint32 requestTimeoutMs);
        ~AIClient();

        AIClient(AIClient const&) = delete;
        AIClient& operator=(AIClient const&) = delete;

        // Builds and submits a GET /health request. Returns the request id
        // immediately (assigned before the request is actually sent) so the
        // caller can correlate it with the "AI request id=... submitted" /
        // "AI response id=..." log lines. Returns 0 - never a valid request
        // id - if the previous health check hasn't completed yet, so a slow
        // ai-server (RequestTimeoutMs > HealthIntervalMs) can't pile up
        // concurrent in-flight requests.
        uint64 SubmitHealthCheck();

        // Non-blocking pop of one completed response. Returns false if none
        // is queued yet.
        bool TryPopResponse(AIResponse& response);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
};

#endif // AIWORLD_AICLIENT_H
