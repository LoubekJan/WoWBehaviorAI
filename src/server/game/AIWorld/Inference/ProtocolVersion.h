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

#ifndef AIWORLD_PROTOCOLVERSION_H
#define AIWORLD_PROTOCOLVERSION_H

#include "Define.h"

// Wire-schema version for the /decision request/response bodies
// (Milestone 2.9A) - a distinct concern from AgentContext::Self's
// SnapshotSequence: SnapshotSequence says which snapshot of one agent a
// decision answers, ProtocolVersion says which shape of DecisionRequest/
// DecisionResponse ai-server and worldserver agree on. Bump this (and add
// a case to ToString()) whenever DecisionRequest/DecisionResponse's wire
// shape changes in a way an older/newer peer can't just ignore.
enum class ProtocolVersion : uint32
{
    V1 = 1,

    // Milestone 2.9B: DecisionResponse's Action string became a nested
    // {"decision":{"type":...}} object (see DecisionIntent) - a V1 peer's
    // parser (which only ever looked for a top-level "action" string)
    // can't just ignore this shape change, so it's a new version rather
    // than a silent extension of V1.
    V2 = 2
};

constexpr ProtocolVersion CurrentProtocolVersion = ProtocolVersion::V2;

inline uint32 ToUnderlying(ProtocolVersion version)
{
    return static_cast<uint32>(version);
}

#endif // AIWORLD_PROTOCOLVERSION_H
