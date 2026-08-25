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

#ifndef AIWORLD_STABLEAGENTHASH_H
#define AIWORLD_STABLEAGENTHASH_H

#include "Define.h"

// Milestone 2.10D P2 fix (runtime review): a well-mixed, deterministic
// 64-bit hash - deliberately NOT a raw AgentId::Value modulo, which would
// put consecutive AgentIds (1, 2, 3, ...) only a few milliseconds apart
// and defeat the entire point of phase-staggering them across the coarse
// tick interval (see AIWorldMgr::RunDecisionScheduler()'s
// Background/Abstract tier-entry handling - this is what
// StableAgentHash(id) % intervalMs is for). Uses the well-known
// MurmurHash3 64-bit finalizer (fmix64) purely for its avalanche property
// - every input bit affects every output bit, so nearby AgentIds land at
// unrelated phases - not for any cryptographic purpose. Pure, stateless,
// deterministic: the same AgentId always maps to the same phase.
inline uint64 StableAgentHash(uint64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

#endif // AIWORLD_STABLEAGENTHASH_H
