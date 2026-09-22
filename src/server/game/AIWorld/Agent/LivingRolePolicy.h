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

#ifndef AIWORLD_LIVINGROLEPOLICY_H
#define AIWORLD_LIVINGROLEPOLICY_H

#include "AgentType.h"
#include "Faction/WorldFactionId.h"
#include "Reconciliation/SpawnParticipationMode.h"
#include <algorithm>
#include <cmath>
#include <vector>

// Role policy for the permanent Elwynn population. No engine or database access.
namespace LivingRolePolicy
{
    enum class Role : uint8 { None, Predator, Prey, Guard, Combatant, Civilian, Worker, Traveler, Service };
    enum class Activity : uint8 { None, Roam, Look, Talk, Work, Graze, Eat, Rest };

    inline bool KnownRole(Role role)
    {
        switch (role)
        {
            case Role::Predator: case Role::Prey: case Role::Guard: case Role::Combatant:
            case Role::Civilian: case Role::Worker: case Role::Traveler: case Role::Service: return true;
            default: return false;
        }
    }

    inline bool InScope(bool enabled, AgentControlMode control, uint32 map, uint32 zone, SpawnParticipationMode participation)
    {
        return enabled && control == AgentControlMode::AIWorldControlled && map == 0 && zone == 12 &&
            (participation == SpawnParticipationMode::FullAgent || participation == SpawnParticipationMode::LightweightBackground);
    }

    inline Role Resolve(AgentType type, uint32 entry, bool service)
    {
        if (type != AgentType::Predator && type != AgentType::Prey && type != AgentType::Guard &&
            type != AgentType::Combatant && type != AgentType::Civilian && type != AgentType::Merchant)
            return Role::None;
        if (service || type == AgentType::Merchant)
            return Role::Service;
        switch (type)
        {
            case AgentType::Predator: return Role::Predator;
            case AgentType::Prey: return Role::Prey;
            case AgentType::Guard: return Role::Guard;
            case AgentType::Combatant: return Role::Combatant;
            case AgentType::Civilian:
                // Accepted WORKER_FARMER / TRAVELER entries from the Elwynn census.
                switch (entry)
                {
                    case 250: case 1975: case 10616: case 11260: case 11328: return Role::Worker;
                    case 14390: case 14393: return Role::Traveler;
                    default: return Role::Civilian;
                }
            default: return Role::None;
        }
    }

    inline bool Fighter(Role role) { return role == Role::Guard || role == Role::Combatant || role == Role::Predator; }
    inline bool Wildlife(Role role) { return role == Role::Predator || role == Role::Prey; }
    inline bool HelpsAllies(Role role) { return role == Role::Guard || role == Role::Combatant; }

    // Native neutral templates (notably Defias Thug's FT 7 clone) are not
    // friendly even to themselves. Social membership supplies the alliance;
    // an explicit native hostility still vetoes assistance.
    inline bool CanAssistAlly(WorldFactionId actor, WorldFactionId ally, bool nativeHostile)
    {
        return actor && actor == ally && !nativeHostile;
    }

    // Called only for an eligible controlled Elwynn hunter and wild prey.
    // Ecology may specialize neutrality; it never overrides friendship.
    inline bool CanHuntNeutralPrey(Role hunter, AgentType prey, bool mutuallyNeutral)
    {
        return hunter == Role::Predator && prey == AgentType::Prey && mutuallyNeutral;
    }

    inline float FreeChaseBearing(float approachBearing, std::vector<float> occupied)
    {
        constexpr float TwoPi = 6.283185307179586f;
        std::sort(occupied.begin(), occupied.end());
        float widest = 0.0f;
        for (std::size_t i = 0; i < occupied.size(); ++i)
        {
            float end = i + 1 < occupied.size() ? occupied[i + 1] : occupied[0] + TwoPi;
            if (end - occupied[i] > widest)
            {
                widest = end - occupied[i];
                approachBearing = std::fmod((end + occupied[i]) * 0.5f, TwoPi);
            }
        }
        return approachBearing;
    }

    inline bool ShouldFlee(Role role, float healthPressure, bool fleeing)
    {
        if (!Fighter(role) || !std::isfinite(healthPressure))
            return true;
        float threshold = role == Role::Guard ? 0.85f : 0.70f;
        return healthPressure >= threshold - (fleeing ? 0.20f : 0.0f);
    }

    inline float RoamRadius(Role role)
    {
        switch (role)
        {
            case Role::Predator: case Role::Traveler: return 12.0f;
            case Role::Combatant: return 10.0f;
            case Role::Guard: return 8.0f;
            case Role::Prey: return 6.0f;
            case Role::Civilian: case Role::Worker: return 4.0f;
            default: return 0.0f;
        }
    }

    inline bool Allows(Role role, Activity activity)
    {
        if (!KnownRole(role) || activity == Activity::None)
            return false;
        switch (activity)
        {
            case Activity::Roam: return RoamRadius(role) > 0.0f;
            case Activity::Look: return true;
            case Activity::Talk: return !Wildlife(role);
            case Activity::Work: return role == Role::Worker;
            case Activity::Graze: return role == Role::Prey;
            case Activity::Eat: return !Wildlife(role);
            case Activity::Rest: return role != Role::Service && role != Role::Guard;
            default: return false;
        }
    }

    inline Activity IdleActivity(Role role, uint64 phase, bool workHours, float hunger, float fatigue)
    {
        if (!KnownRole(role))
            return Activity::None;
        if (role == Role::Service)
            return hunger >= 0.65f ? Activity::Eat : (phase % 2 ? Activity::Talk : Activity::Look);
        if (role == Role::Prey && hunger >= 0.65f)
            return Activity::Graze;
        if (!Wildlife(role) && hunger >= 0.65f)
            return Activity::Eat;
        if (Allows(role, Activity::Rest) && (fatigue >= 0.8f || (!workHours && phase % 3 == 0)))
            return Activity::Rest;
        if (role == Role::Worker && workHours && phase % 3 != 0)
            return Activity::Work;
        if (role == Role::Prey && phase % 2)
            return Activity::Graze;
        if (!Wildlife(role) && role != Role::Guard && phase % 3 == 1)
            return Activity::Talk;
        return phase % 2 ? Activity::Look : Activity::Roam;
    }

    inline char const* ToString(Role role)
    {
        switch (role)
        {
            case Role::Predator: return "PREDATOR";
            case Role::Prey: return "PREY";
            case Role::Guard: return "GUARD";
            case Role::Combatant: return "COMBATANT";
            case Role::Civilian: return "CIVILIAN";
            case Role::Worker: return "WORKER";
            case Role::Traveler: return "TRAVELER";
            case Role::Service: return "SERVICE";
            default: return "NONE";
        }
    }

    inline char const* ToString(Activity activity)
    {
        switch (activity)
        {
            case Activity::Roam: return "ROAM";
            case Activity::Look: return "LOOK";
            case Activity::Talk: return "TALK";
            case Activity::Work: return "WORK";
            case Activity::Graze: return "GRAZE";
            case Activity::Eat: return "EAT";
            case Activity::Rest: return "REST";
            default: return "NONE";
        }
    }
}
#endif
