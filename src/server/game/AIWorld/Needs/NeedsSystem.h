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

#ifndef AIWORLD_NEEDSSYSTEM_H
#define AIWORLD_NEEDSSYSTEM_H

#include "Define.h"
#include "NeedsState.h"

// Per-second drift rates. Deliberately not part of NeedsState itself - the
// rates are shared configuration, not per-agent state.
struct NeedsUpdateRates
{
    float HungerPerSecond = 0.0002f;
    float FatiguePerSecond = 0.0001f;
    float ResourcePressurePerSecond = 0.00005f;
};

// Milestone 2.6A: a pure value transform, deliberately holding no AgentId,
// Creature*, Map*, or MemoryRecord - Update() only knows how to advance a
// NeedsState by an elapsed duration. HealthPressure/SafetyPressure are left
// untouched (still clamped) until 2.6B wires them to real world/memory
// context.
class TC_GAME_API NeedsSystem
{
    public:
        void Update(NeedsState& state, uint32 elapsedMs, NeedsUpdateRates const& rates) const;
};

#endif // AIWORLD_NEEDSSYSTEM_H
