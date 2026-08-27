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

#ifndef AIWORLD_COALITIONFORMATIONRESERVATIONKEY_H
#define AIWORLD_COALITIONFORMATIONRESERVATIONKEY_H

#include "AgentGroupKind.h"
#include "Define.h"
#include <functional>

// Milestone 2.12E4R P2 fix (STATIC review): the (member, Kind) key
// AIWorldMgr::_formationReservedMembers uses to close a cross-profile race
// the per-profile _formationInFlight guard cannot catch on its own. Two
// DIFFERENT CoalitionFormationProfiles of the SAME AgentGroupKind (today
// there is only one profile, WolfLoose, so this cannot happen yet - but
// nothing about _formationInFlight being keyed per-profile prevents a
// future second Loose-forming profile from running concurrently with it)
// could otherwise both see the same eligible member as "not yet a Kind
// member" - CollectMemberIdsOfKind() only reflects AgentGroupRegistry's
// own CONFIRMED membership, and neither profile's own RequestCreateGroup()/
// join chain has landed there yet while both are mid-flight - and both
// propose (and start joining) a group for it, landing the same member in
// two groups of the same Kind at once. Member is a plain AgentId::Value,
// not an AgentId itself - this key only ever lives inside an
// std::unordered_set, and AgentId has no std::hash specialization of its
// own (unlike a raw uint64).
struct CoalitionFormationReservationKey
{
    uint64 Member = 0;
    AgentGroupKind Kind = AgentGroupKind::Loose;

    bool operator==(CoalitionFormationReservationKey const& other) const
    {
        return Member == other.Member && Kind == other.Kind;
    }
};

struct CoalitionFormationReservationKeyHash
{
    std::size_t operator()(CoalitionFormationReservationKey const& key) const
    {
        return std::hash<uint64>()(key.Member) ^ (std::hash<uint8>()(uint8(key.Kind)) << 1);
    }
};

#endif // AIWORLD_COALITIONFORMATIONRESERVATIONKEY_H
