/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGSUPPLYRUNTIME_H
#define AIWORLD_LIVINGSUPPLYRUNTIME_H

#include "LivingSupplyState.h"
#include "AgentType.h"
#include "Action/ActionPosition.h"
#include "ObjectGuid.h"
#include <array>

namespace LivingSupply
{
    struct HunterSpec { uint64 Spawn; uint32 Depot; ActionPosition Ground; };
    inline constexpr std::array<HunterSpec, 3> Hunters{{
        {901034, 1, {725, -260.0f, 340.0f, 0.3222731092f}},
        {901061, 2, {725, 245.0f, 1020.0f, 10.5024320642f}},
        {901062, 2, {725, 245.0f, 1020.0f, 10.5024320642f}}
    }};
    struct DepotSpec { uint32 Id; char const* Name; ActionPosition Point; };
    inline constexpr std::array<DepotSpec, 2> Depots{{
        {1, "South village", {725, -372.4611532703f, 492.6821677284f, 0.15f}},
        {2, "Gnoll camp", {725, 463.99765625f, 972.5791015625f, 13.0966855388f}}
    }};
    inline HunterSpec const* Hunter(uint64 spawn)
    {
        for (auto const& spec : Hunters) if (spec.Spawn == spawn) return &spec;
        return nullptr;
    }
    inline bool HunterCapability(uint64 spawn, uint32 map, AgentType type,
        AgentControlMode control, bool enabled)
    {
        return enabled && map == 725 && type == AgentType::Combatant &&
            control == AgentControlMode::AIWorldControlled && Hunter(spawn);
    }
    inline DepotSpec const* DepotLocation(uint32 id)
    {
        for (auto const& spec : Depots) if (spec.Id == id) return &spec;
        return nullptr;
    }
    inline bool Predator(uint64 spawn)
    { return spawn == 901076 || spawn == 901081 || spawn == 901086 || spawn == 901091; }
    inline bool Prey(uint64 spawn)
    { return spawn >= 901072 && spawn <= 901091 && !Predator(spawn); }

    struct ActorRuntime
    {
        ObjectGuid Guid;
        std::optional<Token> Claim;
        CarcassKey Corpse;
        uint64 EventSerial = 0, NextAtMs = 0;
        uint64 Harvested = 0, Delivered = 0, Consumed = 0, Lost = 0;
        uint32 SearchLeg = 0, EmptySearches = 0;
        bool Trip = false, AtGround = false, Live = false;
        char const* Phase = "IDLE";
        char const* LastResult = "READY";
    };
    struct SourceRuntime
    {
        ObjectGuid Guid;
        uint64 Life = 0;
        bool Alive = false;
    };
}
#endif
