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

void NeedsSystem::Update(NeedsState& state, NeedsUpdateContext const& context, uint32 elapsedMs, NeedsUpdateRates const& rates) const
{
    // Dead: HealthPressure pinned to critical, everything else (Hunger,
    // Fatigue, ResourcePressure, SafetyPressure) frozen at its last value -
    // a dead Creature does not keep getting hungrier/more tired/more
    // resource-strapped while it waits to respawn, and combat state stops
    // being meaningful once it's dead.
    if (!context.Alive)
    {
        state.HealthPressure = 1.0f;
        return;
    }

    state.HealthPressure = context.MaxHealth == 0
        ? 1.0f
        : 1.0f - std::clamp(float(context.Health) / float(context.MaxHealth), 0.0f, 1.0f);

    // 2.6B1's minimal live safety signal: in combat is maximally unsafe,
    // otherwise safe. No memory of recent danger yet - that's 2.6B2.
    state.SafetyPressure = context.InCombat ? 1.0f : 0.0f;

    float dtSeconds = float(elapsedMs) / 1000.0f;

    state.Hunger = std::clamp(state.Hunger + rates.HungerPerSecond * dtSeconds, 0.0f, 1.0f);
    state.Fatigue = std::clamp(state.Fatigue + rates.FatiguePerSecond * dtSeconds, 0.0f, 1.0f);
    state.ResourcePressure = std::clamp(state.ResourcePressure + rates.ResourcePressurePerSecond * dtSeconds, 0.0f, 1.0f);
}
