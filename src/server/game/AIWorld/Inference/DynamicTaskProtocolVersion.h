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

#ifndef AIWORLD_DYNAMICTASKPROTOCOLVERSION_H
#define AIWORLD_DYNAMICTASKPROTOCOLVERSION_H

#include "Define.h"

// Milestone 2.13A1: wire-schema version for the future /dynamic-task
// request/response boundary.
//
// Deliberately separate from ProtocolVersion, which belongs to /decision.
// A wire-shape change in one endpoint must never force an unrelated
// protocol bump in the other endpoint.
enum class DynamicTaskProtocolVersion : uint32
{
    V1 = 1
};

constexpr DynamicTaskProtocolVersion CurrentDynamicTaskProtocolVersion =
    DynamicTaskProtocolVersion::V1;

inline uint32 ToUnderlying(DynamicTaskProtocolVersion version)
{
    return static_cast<uint32>(version);
}

inline constexpr char DynamicTaskEndpoint[] = "/dynamic-task";

#endif // AIWORLD_DYNAMICTASKPROTOCOLVERSION_H
