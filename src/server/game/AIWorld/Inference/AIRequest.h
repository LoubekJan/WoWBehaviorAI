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

#ifndef AIWORLD_AIREQUEST_H
#define AIWORLD_AIREQUEST_H

#include "DecisionRequest.h"
#include "Define.h"
#include <chrono>

enum class AIRequestType : uint8
{
    Health = 0,
    Decision = 1
};

// Plain data handed to AIClient::SubmitDecision(). Never carries a
// Creature*/Player*/Map* - AIClient and everything below it only ever
// sees values, never live game objects. Decision is only meaningful (and
// only sent) for Type == Decision - SubmitHealthCheck() takes no AIRequest
// at all. RequestId, Type, SubmittedAt, and Decision.RequestId/Version are
// all stamped internally by SubmitDecision() (any value the caller set is
// overwritten), so callers only need to fill in Decision.Context.
struct AIRequest
{
    uint64 RequestId = 0;
    AIRequestType Type = AIRequestType::Health;

    // Milestone 2.9D: world-thread timestamp captured immediately before
    // net::post() - purely internal transport metadata for the
    // ai.world.decision.queue_ms metric (time between SubmitDecision() and
    // DecisionSession actually starting on an asio worker thread), never
    // serialized into the /decision JSON body. Not HTTP latency - that's
    // AIResponse::LatencyMs, measured separately from where the session
    // itself starts.
    std::chrono::steady_clock::time_point SubmittedAt;

    DecisionRequest Decision;
};

#endif // AIWORLD_AIREQUEST_H
