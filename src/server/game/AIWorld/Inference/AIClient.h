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

#include "AIRequest.h"
#include "AIResponse.h"
#include "DecisionSubmitResult.h"
#include "Define.h"
#include <memory>
#include <string>
#include <vector>

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
        // Milestone 2.10A: maxDecisionsInFlight bounds how many /decision
        // requests SubmitDecision()/SubmitDecisions() will let be
        // simultaneously in flight - the hard global admission cap
        // AIWorldMgr's DecisionScheduler is designed around (see
        // AIWorld.DecisionMaxInFlight). Health checks are unaffected -
        // SubmitHealthCheck() keeps its own separate single-in-flight guard.
        AIClient(Trinity::Asio::IoContext& ioContext, std::string host, std::string port, uint32 requestTimeoutMs, uint32 maxDecisionsInFlight);
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

        // Builds and POSTs a versioned /decision request (Milestone 2.9A -
        // see DecisionRequest/AgentContext) from the given AgentContext.
        // RequestId, Type, Decision.RequestId, and Decision.Version are all
        // stamped internally (any value the caller set is overwritten), so
        // only Decision.Context needs to be filled in. Same non-blocking
        // contract as SubmitHealthCheck(); returns 0 - "skipped, no slot
        // available" - if maxDecisionsInFlight requests are already in
        // flight (Milestone 2.10A: a bounded counter, not a single bool -
        // multiple different agents' requests can be in flight at once, up
        // to that bound). Never blocks waiting for a slot to free up.
        uint64 SubmitDecision(AIRequest request);

        // Milestone 2.9E/2.10A: caller-facing batch entry point - still
        // just calls SubmitDecision() once per request in order, so
        // admission is still governed entirely by that same bounded
        // counter: a later request in the same batch can legitimately come
        // back SkippedInFlight once the cap is reached, even though
        // nothing about it individually was wrong. Deciding which agents
        // are even worth attempting (due-ness, per-agent duplicate guard,
        // deterministic ordering) is the caller's job - see AIWorldMgr's
        // DecisionScheduler - this only ever enforces the hard global cap.
        // Results are returned in request order, but callers must
        // correlate by DecisionSubmitResult::Agent, never by vector
        // position alone - see DecisionSubmitResult.h. Emits no metrics of
        // its own: SubmitDecision() already logs ai.world.decision.submit
        // once per request, and this must never double-count it.
        std::vector<DecisionSubmitResult> SubmitDecisions(std::vector<AIRequest> requests);

        // Non-blocking pop of one completed response. Returns false if none
        // is queued yet.
        bool TryPopResponse(AIResponse& response);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
};

#endif // AIWORLD_AICLIENT_H
