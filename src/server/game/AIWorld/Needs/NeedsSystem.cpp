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

#include "NeedsSystem.h"
#include <algorithm>

void NeedsSystem::Update(NeedsState& state, uint32 elapsedMs, NeedsUpdateRates const& rates) const
{
    float dtSeconds = float(elapsedMs) / 1000.0f;

    state.Hunger = std::clamp(state.Hunger + rates.HungerPerSecond * dtSeconds, 0.0f, 1.0f);
    state.Fatigue = std::clamp(state.Fatigue + rates.FatiguePerSecond * dtSeconds, 0.0f, 1.0f);
    state.ResourcePressure = std::clamp(state.ResourcePressure + rates.ResourcePressurePerSecond * dtSeconds, 0.0f, 1.0f);

    // Not yet driven by anything in 2.6A - clamped defensively in case a
    // future writer (2.6B) puts them out of range.
    state.HealthPressure = std::clamp(state.HealthPressure, 0.0f, 1.0f);
    state.SafetyPressure = std::clamp(state.SafetyPressure, 0.0f, 1.0f);
}
