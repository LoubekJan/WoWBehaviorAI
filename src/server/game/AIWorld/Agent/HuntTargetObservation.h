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

#ifndef AIWORLD_HUNTTARGETOBSERVATION_H
#define AIWORLD_HUNTTARGETOBSERVATION_H

#include "AgentId.h"
#include "Define.h"
#include "HuntTargetProvenance.h"

// Milestone 2.12G3B: one existing group member's own current world-thread
// observation of ONE candidate HUNT target, as a plain value - the same
// "live TrinityCore objects stay local, only a DTO crosses into a pure
// system" boundary CoalitionMemberObservation.h already establishes for a
// group's own members, applied here to what a member currently sees.
// Built by whatever future caller resolves real Creature*/Unit* sight
// (not this milestone - HuntIntentSystem, below, only ever consumes
// already-built values like this one, it never resolves anything itself).
//
// Observer names WHO made this observation - a real, already-registered
// AgentId, never a fake/synthetic one - purely so HuntIntentSystem can
// confirm the observation actually came from a genuine member of the
// group it is evaluating (see HuntIntentSystem.h's own fail-closed rule
// order) without needing anything beyond this struct's own fields.
// Target is the full HuntTargetProvenance snapshot (identity + position +
// alive/observed-at provenance) this member saw. Distance/LineOfSight are
// this OBSERVATION's own additional facts - not part of the target's own
// identity/provenance (HuntTargetProvenance itself), because both are
// relative to the observing member, not to the target: two different
// members observing the exact same target at the exact same moment could
// legitimately report different Distance/LineOfSight values.
struct HuntTargetObservation
{
    AgentId Observer;
    HuntTargetProvenance Target;

    float Distance = 0.0f;
    bool LineOfSight = false;
};

#endif // AIWORLD_HUNTTARGETOBSERVATION_H
