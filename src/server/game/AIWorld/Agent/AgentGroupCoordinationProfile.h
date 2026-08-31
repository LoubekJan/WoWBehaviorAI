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

#ifndef AIWORLD_AGENTGROUPCOORDINATIONPROFILE_H
#define AIWORLD_AGENTGROUPCOORDINATIONPROFILE_H

#include "AgentGroupKind.h"
#include "CoalitionFormationProfileId.h"
#include "Define.h"

// Milestone 2.12F1: everything AgentGroupIntentSystem::Evaluate() needs
// beyond the group/its members' own observations - the coordination-side
// counterpart to CoalitionFormationProfile/CoalitionMaintenanceProfile,
// the same "config struct built once by AIWorldMgr, passed in per call"
// pattern, and the same reuse of CoalitionFormationProfileId as the one
// identity a group's own persistent provenance (AgentGroupRecord::
// ProfileId) is checked against - see AgentGroupIntentSystem.h for why
// Kind/ProfileId are both validated the same fail-closed way
// CoalitionMaintenanceProfile already is.
//
// RegroupEnabled/RegroupRadius are the one coordination behavior this
// milestone defines - a profile that does not want automatic regrouping
// at all simply leaves RegroupEnabled false, the same "declare the shape,
// wire the rule only when it is actually needed" discipline this
// codebase already holds elsewhere (see AgentGroupKind.h's own
// Institutional-not-yet-added comment). WolfLoose is the one real
// profile that sets this today; a future GuardPatrol/BanditGroup/
// Caravan/CivilianGroup profile is just another value with its own
// RegroupRadius (or RegroupEnabled = false if that Kind of group never
// automatically regroups at all) - AgentGroupIntentSystem itself never
// special-cases which profile this is, and never will just from adding
// a new one here.
struct AgentGroupCoordinationProfile
{
    CoalitionFormationProfileId ProfileId = CoalitionFormationProfileId::Invalid;
    AgentGroupKind Kind = AgentGroupKind::Loose;

    bool RegroupEnabled = false;
    float RegroupRadius = 0.0f;

    // Milestone 2.12G2: same "declare the shape, wire the rule only when
    // it is actually needed" discipline as RegroupEnabled/RegroupRadius
    // above - a profile that has not opted into automatic territory
    // movement simply leaves RoamEnabled false.
    //
    // Milestone 2.12G2 P2 fix (STATIC review): RoamDistance is kept
    // within min(RegroupRadius, LeaveRadius) - NOT RegroupRadius alone.
    // RegroupRadius is deliberately independent Coordination-layer
    // config, with no enforced relationship to LeaveRadius (a valid
    // config can legally have RegroupRadius > LeaveRadius) - clamping
    // RoamDistance only against RegroupRadius could still legally send a
    // member past LeaveRadius, where CoalitionMaintenanceSystem's own
    // automatic Leave would remove it from the group entirely: Roam and
    // Maintenance fighting each other, not Roam and Regroup. See
    // AIWorldMgr::Initialize()'s own AIWorld.WolfGroupRoamDistance/
    // AIWorld.DefiasGroupRoamDistance clamp (computed once against
    // min(RegroupRadius, LeaveRadius), with a fail-closed disable of ROAM
    // entirely when that envelope leaves no room for a valid value) for
    // the actual enforcement.
    bool RoamEnabled = false;
    float RoamDistance = 0.0f;

    // How often (ms) the deterministic ROAM target itself changes - see
    // AgentGroupIntentSystem::Evaluate()'s own comment for the phase this
    // drives. Deliberately NOT how often RunCoalitionCoordination()
    // re-evaluates (that stays the shared AIWorld.GroupCoordinationIntervalMs,
    // unchanged since 2.12F2) - a group can be re-evaluated several times
    // while still roaming toward the same, still-current target.
    uint32 RoamIntervalMs = 0;

    // How close (yd) a member must already be to the current ROAM target
    // before AgentGroupIntentProjector::Project() stops proposing a move
    // for it - the Roam counterpart to RegroupRadius, but inverted:
    // RegroupRadius is a TRIGGER threshold ("far enough to bother"),
    // RoamArrivalRadius is a SATISFIED threshold ("close enough to
    // stop"). This, not any new per-group timer/attempt-identity state,
    // is what keeps a settled group standing still between roam phases
    // instead of re-issuing an identical MOVE_TO every coordination pass.
    float RoamArrivalRadius = 0.0f;

    // Milestone 2.12G3B: same "declare the shape, wire the rule only when
    // it is actually needed" discipline every other coordination behavior
    // in this struct already follows - a profile that has not opted into
    // automatic HUNT selection simply leaves HuntEnabled false.
    // HuntTargetCreatureEntry is deliberately the first, narrowest
    // possible eligibility policy (one allowed creature entry, not a set
    // or a species-kind enum) - HuntIntentSystem/HuntIntentProjector never
    // branch on WHICH profile this is (no WolfLoose/DefiasLoose special
    // case anywhere in their own logic), only on this already-resolved
    // generic value, the same way RegroupRadius/RoamDistance already let
    // REGROUP/ROAM stay entirely generic. HuntAcquisitionRadius bounds how
    // far a candidate target may be from the OBSERVING member (see
    // HuntTargetObservation::Distance) - a trigger threshold, the
    // HUNT counterpart to RegroupRadius. HuntObservationMaxAgeMs bounds how
    // old (nowMs - HuntTargetObservation::Target.ObservedAtMs) an
    // observation may be before it is too stale to select a hunt from -
    // both 0 (the zero/default value) fail closed, the same way
    // RoamIntervalMs == 0 already disables ROAM regardless of RoamEnabled.
    bool HuntEnabled = false;
    uint32 HuntTargetCreatureEntry = 0;
    float HuntAcquisitionRadius = 0.0f;
    uint32 HuntObservationMaxAgeMs = 0;
};

#endif // AIWORLD_AGENTGROUPCOORDINATIONPROFILE_H
