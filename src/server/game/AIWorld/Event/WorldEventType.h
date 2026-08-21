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

#ifndef AIWORLD_WORLDEVENTTYPE_H
#define AIWORLD_WORLDEVENTTYPE_H

#include "Define.h"

// Milestone 2.3A only actually produces CreatureKilled. The rest are
// declared now (per the roadmap) so WorldEvent/EventBus don't need to
// change shape again when a later milestone starts producing them.
enum class WorldEventType : uint8
{
    CreatureKilled,
    NPCInjured,
    PlayerSeen,
    ItemStolen,
    TradeCompleted,
    LivestockKilled,
    WolfPackMoved,
    FoodShortage,
    NPCDied
};

inline char const* ToString(WorldEventType type)
{
    switch (type)
    {
        case WorldEventType::CreatureKilled: return "CREATURE_KILLED";
        case WorldEventType::NPCInjured:     return "NPC_INJURED";
        case WorldEventType::PlayerSeen:     return "PLAYER_SEEN";
        case WorldEventType::ItemStolen:     return "ITEM_STOLEN";
        case WorldEventType::TradeCompleted: return "TRADE_COMPLETED";
        case WorldEventType::LivestockKilled:return "LIVESTOCK_KILLED";
        case WorldEventType::WolfPackMoved:  return "WOLF_PACK_MOVED";
        case WorldEventType::FoodShortage:   return "FOOD_SHORTAGE";
        case WorldEventType::NPCDied:        return "NPC_DIED";
        default:                             return "UNKNOWN";
    }
}

#endif // AIWORLD_WORLDEVENTTYPE_H
