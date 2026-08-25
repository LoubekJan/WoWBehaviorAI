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

#include "AgentGroupSimulationSystem.h"
#include <algorithm>

bool AgentGroupSimulationSystem::Update(AgentGroupRecord& record, uint64 dtMs, AgentGroupSimulationRates const& rates) const
{
    float dtSeconds = float(dtMs) / 1000.0f;

    // Same clamp(value + rate * dtSeconds, 0, 1) shape NeedsSystem::Update()
    // already uses for Hunger/Fatigue/ResourcePressure - Resources falls,
    // so its rate is subtracted.
    float previousResources = record.Resources;
    record.Resources = std::clamp(record.Resources - rates.ResourcesPerSecond * dtSeconds, 0.0f, 1.0f);

    // std::clamp() of an already-clamped value returns the identical
    // float, bit-for-bit - safe to compare with != here, this is not a
    // "floats drift" comparison. See this class's own header comment for
    // why the caller trusts this return value rather than re-deriving
    // "did anything change" itself.
    return record.Resources != previousResources;
}
