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

#ifndef AIWORLD_DYNAMICQUESTOUTCOMEEVENT_H
#define AIWORLD_DYNAMICQUESTOUTCOMEEVENT_H

#include "Define.h"
#include "DynamicQuestInstance.h"
#include "Event/WorldEvent.h"

#include <optional>

// Milestone 2.13C6A: pure, value-only builder that turns a terminal
// DynamicQuestInstance (Completed/Failed/Expired) into the typed WorldEvent
// the roadmap's causal loop requires - see AIWorld_Current_Roadmap.md's own
// 2.13C6 section. No Creature*/Player*/Map*, no EventBus, no AIWorldMgr:
// this milestone is the contract alone. AIWorldMgr publishing this through
// a real EventBus, from a real registry transition, is 2.13C6B's job, not
// this file's.
//
// Returns nullopt for Offered/Active - there is no outcome event for a
// quest that has not yet reached a terminal state.
//
// The returned WorldEvent always has EventId == 0: exactly like every
// other WorldEvent producer in this codebase, only EventBus::Publish()
// ever assigns one (see WorldEvent.h's own comment) - this builder is not
// itself a publication.
//
// CauseEventId/CorrelationId are taken from instance.SourceEventId/
// SourceCorrelationId (see DynamicQuestInstance.h's own comment) - the
// dynamic quest's outcome is a caused-by event, chained back to the
// original WorldEvent whose provenance created the quest in the first
// place, never a new root correlation of its own.
//
// Actor.Guid follows WorldEvent's existing Actor = did-it / Target =
// had-it-done-to-them convention: it is instance.AcceptedByPlayerGuid
// ONLY for Completed, the one outcome the accepting player genuinely
// caused. Failed and Expired both happen TO the quest, not because of
// anything the player did - Expired comes from deadline maintenance,
// Failed from server-side force-fail/replay-containment recovery (see
// AIWorldMgr) - so Actor stays empty for both, never the accepting
// player's GUID. Do not widen this to "the accepted player is
// responsible for every outcome" - a future explicit player-abandon
// transition would need its own real failure-cause semantics, not an
// inference from AcceptedByPlayerGuid alone. Target.Agent/Target.Guid
// are instance.Giver/instance.GiverRuntimeGuid - pure historical
// value/provenance data, exactly as DynamicQuestInstance's own comment
// describes them elsewhere, never an authorization by themselves.
//
// location is supplied by the caller as a plain value so this builder
// itself never needs a live Creature*/Player*/Map* to run - the caller
// (2.13C6B, in AIWorldMgr) is responsible for resolving whatever location
// value makes sense for its own real transition point.
std::optional<WorldEvent> BuildDynamicQuestOutcomeWorldEvent(
    DynamicQuestInstance const& instance,
    WorldEventLocation const& location,
    uint64 occurredAtMs);

#endif // AIWORLD_DYNAMICQUESTOUTCOMEEVENT_H
