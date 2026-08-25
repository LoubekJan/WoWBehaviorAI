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

#ifndef AIWORLD_AGENTECONOMYSTATE_H
#define AIWORLD_AGENTECONOMYSTATE_H

#include "Define.h"

// Milestone 2.11E2: a persistent stockpile, distinct from NeedsState::
// ResourcePressure (a 0.0-1.0 drive to act, drifting over time - see
// NeedsState.h) - this is an actual accumulated count, changed only by a
// specific ActionCompletion, never by a drift rate. Not optional, unlike
// HomeLocation/WorkLocation: every agent has one, defaulting to zero
// rather than "unset" - there is no meaningful absence of a stockpile the
// way there is of a home. Money is the only field any current mutation
// path (2.11E2's WORK completion) actually changes; Food/Resource exist
// and persist now so a later milestone's production doesn't need another
// schema migration, but nothing writes to them yet.
struct AgentEconomyState
{
    uint32 Money = 0;
    uint32 Food = 0;
    uint32 Resource = 0;

    // Milestone 2.11E2 P2 fix: idempotency key for the WORK reward -
    // AgentRecord::RoutineActivityState (and its StartedAtMs) is runtime-
    // only and gets cleared on every dematerialize/restart, so it cannot
    // by itself tell "already paid for this work window" apart from "a
    // brand new WORK attempt" once the agent rematerializes still inside
    // the same synthetic work window (see AIWorldMgr::UpdateNeeds() for
    // how the window id is derived from RoutineScheduleConfig, independent
    // of any runtime state). Persisted alongside Money in the same
    // SaveEconomyState() call - both change together, in one UPDATE, or
    // neither does; a reward that persisted Money but not this marker
    // (or vice versa) would let a restart either repeat or silently lose
    // the payment.
    uint64 LastRewardedWorkWindowId = 0;

    // Milestone 2.11E2 P3 fix: a monotonic write counter, incremented in
    // memory every time AIWorldMgr is about to persist this state (see
    // UpdateNeeds()'s WORK reward block) and written together with the
    // rest of the row. SaveEconomyState()'s UPDATE only applies when the
    // stored economy_version is still below the one it is writing - the
    // async write queue does not itself guarantee execution order across
    // CharacterDatabase's own async worker threads, so without this an
    // older snapshot could in principle finish after a newer one and
    // silently revert it.
    // Harmless today (at most one SaveEconomyState() call per work window,
    // see LastRewardedWorkWindowId, so two writes for the same agent never
    // actually race in practice) - this is what keeps it safe once a
    // future milestone adds more frequent/overlapping economy mutations.
    uint64 Version = 0;
};

#endif // AIWORLD_AGENTECONOMYSTATE_H
