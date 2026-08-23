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

namespace
{
    // 2.6B2's danger whitelist: an inherently dangerous WorldEventType.
    // Deliberately excludes PlayerSeen/TradeCompleted/ItemStolen/
    // FoodShortage - those aren't physical danger (FoodShortage feeds
    // Hunger/ResourcePressure instead, ItemStolen isn't a threat to the
    // agent itself).
    bool IsDangerousEventType(WorldEventType type)
    {
        switch (type)
        {
            case WorldEventType::CreatureKilled:
            case WorldEventType::NPCInjured:
            case WorldEventType::LivestockKilled:
            case WorldEventType::WolfPackMoved:
            case WorldEventType::NPCDied:
                return true;
            default:
                return false;
        }
    }

    // 2.6C hysteresis gap: a Need must reach 0.80 to latch ACTIVE (emitting
    // one event) and must drop back below 0.60 before it can latch again -
    // without the gap, a value oscillating around a single threshold would
    // emit an event almost every ~1s Needs tick.
    constexpr float CriticalEnterThreshold = 0.80f;
    constexpr float CriticalResetThreshold = 0.60f;
}

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

    // Combat is always maximal danger, taking precedence over whatever
    // memory says; once combat itself ends, a recent dangerous memory can
    // still keep SafetyPressure above zero (2.6B2) until it ages out.
    state.SafetyPressure = context.InCombat
        ? 1.0f
        : std::clamp(context.MemorySafetyPressure, 0.0f, 1.0f);

    float dtSeconds = float(elapsedMs) / 1000.0f;

    state.Hunger = std::clamp(state.Hunger + rates.HungerPerSecond * dtSeconds, 0.0f, 1.0f);
    state.Fatigue = std::clamp(state.Fatigue + rates.FatiguePerSecond * dtSeconds, 0.0f, 1.0f);
    state.ResourcePressure = std::clamp(state.ResourcePressure + rates.ResourcePressurePerSecond * dtSeconds, 0.0f, 1.0f);
}

float NeedsSystem::EvaluateMemorySafety(std::vector<RetrievedMemory> const& memories, uint64 nowMs, uint32 recentWindowMs) const
{
    float pressure = 0.0f;

    for (RetrievedMemory const& memory : memories)
    {
        if (!memory.SourceEventType || !IsDangerousEventType(*memory.SourceEventType))
            continue;

        if (memory.LastObservedAtMs > nowMs)
            continue; // defensive: no future-dated memory should count

        uint64 ageMs = nowMs - memory.LastObservedAtMs;
        if (ageMs >= recentWindowMs)
            continue;

        float recency = 1.0f - float(ageMs) / float(recentWindowMs);
        pressure = std::max(pressure, memory.Importance * recency);
    }

    return std::clamp(pressure, 0.0f, 1.0f);
}

std::vector<NeedsThresholdEvent> NeedsSystem::EvaluateThresholds(NeedsState const& state, NeedsThresholdState& thresholds) const
{
    std::vector<NeedsThresholdEvent> events;

    if (!thresholds.HungerCriticalActive && state.Hunger >= CriticalEnterThreshold)
    {
        thresholds.HungerCriticalActive = true;
        events.push_back({ NeedsThresholdEventType::HungerCritical, state.Hunger });
    }
    else if (thresholds.HungerCriticalActive && state.Hunger < CriticalResetThreshold)
    {
        thresholds.HungerCriticalActive = false;
    }

    if (!thresholds.DangerHighActive && state.SafetyPressure >= CriticalEnterThreshold)
    {
        thresholds.DangerHighActive = true;
        events.push_back({ NeedsThresholdEventType::DangerHigh, state.SafetyPressure });
    }
    else if (thresholds.DangerHighActive && state.SafetyPressure < CriticalResetThreshold)
    {
        thresholds.DangerHighActive = false;
    }

    return events;
}

void NeedsSystem::SatisfyHunger(NeedsState& state) const
{
    state.Hunger = 0.0f;
}
