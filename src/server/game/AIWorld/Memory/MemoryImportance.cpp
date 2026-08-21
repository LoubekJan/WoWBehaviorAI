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

#include "MemoryImportance.h"

float MemoryImportance::Score(Observation const& observation)
{
    if (observation.Type == ObservationType::PlayerSeen)
        return 0.20f;

    if (observation.Type == ObservationType::CreatureSeen)
        return 0.10f;

    if (!observation.SourceEventType)
        return 0.30f;

    switch (*observation.SourceEventType)
    {
        case WorldEventType::NPCDied:          return 1.00f;
        case WorldEventType::LivestockKilled:  return 0.90f;
        case WorldEventType::CreatureKilled:   return 0.85f;
        case WorldEventType::ItemStolen:       return 0.80f;
        case WorldEventType::FoodShortage:     return 0.75f;
        case WorldEventType::NPCInjured:       return 0.70f;
        case WorldEventType::WolfPackMoved:    return 0.60f;
        case WorldEventType::TradeCompleted:   return 0.40f;
        case WorldEventType::PlayerSeen:       return 0.25f;
    }

    return 0.30f;
}
