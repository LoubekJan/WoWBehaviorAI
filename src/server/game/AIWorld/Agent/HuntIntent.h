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

#ifndef AIWORLD_HUNTINTENT_H
#define AIWORLD_HUNTINTENT_H

#include "Define.h"
#include "GroupId.h"
#include "HuntTargetProvenance.h"

// Milestone 2.12G3A: the GROUP-level fact a future HuntIntentSystem::
// Evaluate() (2.12G3B, not this milestone) will produce - "this group
// currently wants to hunt this specific target" - named here now, ahead of
// that system's own existence, purely as a contract, the same precedent
// AgentGroupIntent.h itself set for AgentGroupIntentProjector back in
// 2.12F1. Like AgentGroupIntent, this names no live Creature*/Map*/Unit*
// and authorizes no movement or combat action by itself; only an
// individual Agent's own ActionRequest, validated by ActionSystem like any
// other, is ever allowed to reach TrinityCore (see HuntProposal.h for that
// boundary, and this file's own class comment on AIWorldMgr's dispatch
// discipline for the REGROUP/ROAM precedent HUNT will follow).
//
// HUNT is planned to occupy the SAME lowest-priority GroupCoordinationGoal
// tier REGROUP/ROAM already share (Emergency ActiveGoal > Normal
// ActiveGoal > RoutineGoal > Regroup/Roam/Hunt) once wired up - an
// individual member's own Emergency/Normal combat ownership (its own
// current fight, fleeing danger, etc.) always outranks a group HUNT
// proposal, exactly the way it already outranks REGROUP/ROAM today. This
// milestone does not wire that priority ordering in; it only names the
// DTOs the future system that does will use.
//
// StartedAtMs is this HUNT ATTEMPT's own identity - the same
// attempt-vs-target-freshness split GroupCoordinationGoal::StartedAtMs /
// ActiveActionState::GoalStartedAtMs already use for REGROUP/ROAM,
// generalized here for HUNT. It answers "is this still the same hunt
// attempt a caller previously observed", a DIFFERENT question from
// Target.ObservedAtMs ("is this target snapshot still fresh") - an
// attempt can remain the very same attempt across several re-observations
// of its target as the hunt progresses, so the two clocks are
// deliberately independent fields, not one shared timestamp.
//
// No system/projector/orchestration exists yet to produce or consume this
// type (that is 2.12G3B's job) - this milestone is DTO-only, per the
// project roadmap's own G3A/G3B/G3C/G3D split for HUNT.
struct HuntIntent
{
    GroupId Group;
    HuntTargetProvenance Target;

    uint64 StartedAtMs = 0;
};

#endif // AIWORLD_HUNTINTENT_H
