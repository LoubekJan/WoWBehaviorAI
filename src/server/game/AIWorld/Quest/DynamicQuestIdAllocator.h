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

#ifndef AIWORLD_DYNAMICQUESTIDALLOCATOR_H
#define AIWORLD_DYNAMICQUESTIDALLOCATOR_H

#include "Define.h"
#include "DynamicQuestId.h"

// Milestone 2.13C2: the one place a DynamicQuestId is ever minted - see
// DynamicQuestId's own comment ("Allocation... belongs to whatever owns a
// lifecycle registry"). AIWorldMgr owns exactly one process-lifetime
// `uint64 _nextDynamicQuestId = 1` counter and calls this on every
// allocation attempt (AIWorldMgr::AllocateDynamicQuestId()); this
// function is pure so the counter's own fail-closed behavior is
// independently testable without a live AIWorldMgr.
//
// Advances counter and returns the id it named, UNLESS counter is
// already 0 (a prior call already reached exhaustion), in which case it
// returns DynamicQuestId{} (0) and leaves counter untouched - 0 is never
// handed out as a real id (see DynamicQuestId's own comment on why 0 must
// stay invalid) and is instead reused as the permanent "exhausted"
// sentinel. The last legal counter value (UINT64_MAX) is still handed
// out once, after which counter is explicitly set to 0 rather than left
// to a plain ++counter, which would silently wrap back to 1 and reissue
// an already-used id - ids are never reused within one process lifetime.
DynamicQuestId AdvanceDynamicQuestIdCounter(uint64& counter);

#endif // AIWORLD_DYNAMICQUESTIDALLOCATOR_H
