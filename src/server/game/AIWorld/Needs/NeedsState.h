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

#ifndef AIWORLD_NEEDSSTATE_H
#define AIWORLD_NEEDSSTATE_H

// An agent's needs, all on the same 0.0-1.0 "pressure" scale: 0.0 is no
// pressure to act, 1.0 is critical. This is a uniform-direction convention,
// not a raw stat - unlike Health, higher Hunger/Fatigue is worse, so it is
// deliberately named Hunger/Fatigue rather than something like "Satiety"
// that would flip the direction. Milestone 2.6A drifts Hunger/Fatigue/
// ResourcePressure over time; 2.6B1 derives HealthPressure from live HP
// ratio and SafetyPressure from live InCombat state. Recent-memory-driven
// safety (danger persisting after combat ends) is 2.6B2.
struct NeedsState
{
    float HealthPressure = 0.0f;
    float Hunger = 0.0f;
    float Fatigue = 0.0f;
    float SafetyPressure = 0.0f;
    float ResourcePressure = 0.0f;
};

#endif // AIWORLD_NEEDSSTATE_H
