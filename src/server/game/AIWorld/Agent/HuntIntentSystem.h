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

#ifndef AIWORLD_HUNTINTENTSYSTEM_H
#define AIWORLD_HUNTINTENTSYSTEM_H

#include "AgentGroupCoordinationProfile.h"
#include "CoalitionMemberObservation.h"
#include "Define.h"
#include "HuntIntent.h"
#include "HuntTargetObservation.h"
#include <optional>
#include <vector>

struct AgentGroupRecord;

// Milestone 2.12G3B: "does this group currently want to hunt anything, as a
// group?" - a pure value transform, the same shape AgentGroupIntentSystem
// already is for REGROUP/ROAM: no DB, no AgentRegistry/AgentGroupRegistry
// mutation, no Creature*/Map*/Player*/ObjectAccessor lookup, no async code,
// nothing held as member state, and NO knowledge of any specific profile's
// own identity (WolfLoose, DefiasLoose, or any future profile) - this class
// never branches on CoalitionFormationProfileId anywhere in its own logic,
// only on the already-resolved generic AgentGroupCoordinationProfile fields
// a caller handed it. Turning a HuntIntent into per-member HuntProposal
// values is deliberately NOT this class's job either - see
// HuntIntentProjector.h for that boundary, mirroring AgentGroupIntent.h's
// own split from AgentGroupIntentProjector.
//
// Returns std::optional<HuntIntent> rather than a "Type == None" sentinel
// value the way AgentGroupIntent does - HuntIntent has no target-less
// variant that would make sense to default-construct (its Target field is
// a real HuntTargetProvenance, not an optional one), so "the group does
// not want to hunt anything right now" is expressed as nullopt instead of
// an intent whose fields a caller must remember not to read.
//
// Rules, evaluated in this order (the same fail-closed shape
// AgentGroupIntentSystem::Evaluate() already holds its own profile checks
// to, extended with HUNT's own target-eligibility checks):
//   0a. profile.ProfileId == Invalid -> nullopt.
//   0b. profile.Kind != group.Kind -> nullopt.
//   0c. profile.ProfileId != group.ProfileId -> nullopt.
//   0d. profile.HuntEnabled == false -> nullopt.
//   0e. profile.HuntTargetCreatureEntry == 0, or
//       profile.HuntAcquisitionRadius <= 0, or
//       profile.HuntObservationMaxAgeMs == 0 -> nullopt (an unconfigured
//       policy can select nothing, the same "zero means disabled" reading
//       RoamIntervalMs == 0 already gets for ROAM).
//   0f. nowMs == 0 -> nullopt (0 is never a real CurrentTimeMs() reading in
//       this codebase; treating it as one would make every observation
//       look infinitely stale/future-dated in an undefined way).
//   For each HuntTargetObservation in targets, reject (skip) unless ALL of:
//     1. observation.Observer actually names a member of group.Members -
//        an observation attributed to a non-member is never trusted,
//        regardless of how it was produced.
//     2. That member's own CoalitionMemberObservation (looked up in
//        members by AgentId) is Materialized, Alive, and on the same
//        MapId as group.TerritoryMapId - an unloaded, dead, or
//        different-map observer's sighting is never trusted either, the
//        same "absence from the grid must never be misread as a
//        coordination fact" discipline every other System class in this
//        codebase already holds to.
//     3. Target.TargetGuid is not empty.
//     4. Target.TargetEntry equals profile.HuntTargetCreatureEntry.
//     5. Target.Alive is true.
//     6. Target.MapId equals group.TerritoryMapId (2.12G3A: HUNT is
//        restricted to persistent non-instance/base-world targets - a
//        target outside the group's own base-world map fails closed here,
//        the same map-identity discipline REGROUP/ROAM already apply to
//        their own targets).
//     7. Target.ObservedAtMs is neither 0 nor greater than nowMs (a
//        future-dated observation is never trustworthy - it can only mean
//        a clock error or a fabricated value, never a genuine sighting).
//     8. nowMs - Target.ObservedAtMs does not exceed
//        profile.HuntObservationMaxAgeMs.
//     9. observation.LineOfSight is true.
//     10. observation.Distance does not exceed profile.HuntAcquisitionRadius.
//   If the exact same TargetGuid was validly observed by more than one
//   member, only its single FRESHEST observation (largest
//   Target.ObservedAtMs) is kept as that target's own candidate; a tie is
//   broken by the lower observation.Observer.Value - never by input order.
//   From the surviving one-candidate-per-target set, the final HuntIntent
//   targets whichever has the smallest Distance; a tie is broken by the
//   lower Target.TargetGuid - again never by input order. If no candidate
//   survives at all -> nullopt.
// Fully deterministic given the same group/profile/members/targets/nowMs:
// two calls with the same input always return the same result, and the
// result never depends on the order either the members or targets vector
// was given in.
class TC_GAME_API HuntIntentSystem
{
    public:
        std::optional<HuntIntent> Evaluate(AgentGroupRecord const& group, AgentGroupCoordinationProfile const& profile,
            std::vector<CoalitionMemberObservation> const& members, std::vector<HuntTargetObservation> const& targets,
            uint64 nowMs) const;
};

#endif // AIWORLD_HUNTINTENTSYSTEM_H
