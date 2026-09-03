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
#include "DynamicTaskRequest.h"
#include "QuestRequestProvenance.h"
#include <chrono>

enum class AIRequestType : uint8
{
    Health = 0,
    Decision = 1,
    DynamicTask = 2
};

// Plain data handed to AIClient::SubmitDecision()/SubmitDynamicTask().
// Never carries a Creature*/Player*/Map* - AIClient and everything below
// it only ever sees values, never live game objects. Decision is only
// meaningful (and only sent) for Type == Decision, DynamicTask/
// QuestProvenance only for Type == DynamicTask - SubmitHealthCheck() takes
// no AIRequest at all. RequestId, Type, SubmittedAt, and
// Decision.RequestId/Version (or, for DynamicTask,
// DynamicTask.RequestId/Version) are all stamped internally by the
// matching Submit*() call (any value the caller set is overwritten), so
// callers only need to fill in Decision.Context (or DynamicTask.Context
// plus QuestProvenance).
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

    // Milestone 2.13A3: the /dynamic-task counterpart to Decision above.
    DynamicTaskRequest DynamicTask;

    // Milestone 2.13A3: the world thread's own record of what this
    // DynamicTask request is actually about - RuntimeGuid, goal attempt,
    // source event identity and the Token -> ObjectGuid/Entry/MapId
    // bindings for every QuestContext::CandidateTargets entry the wire
    // request carries. Set by the caller alongside DynamicTask.Context
    // (SubmitDynamicTask() additionally copies Agent/SnapshotSequence from
    // DynamicTask.Context so both always agree - see AIClient.cpp).
    // Exactly the same "client's own echo, never server's claim" pattern
    // DecisionProvenance already uses - and, critically, NEVER serialized
    // to the wire: QuestRequestProvenance carries ObjectGuid, which
    // QuestContext deliberately never does (see QuestContext.h).
    QuestRequestProvenance QuestProvenance;
};

#endif // AIWORLD_AIREQUEST_H
