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

#ifndef AIWORLD_HUNTTARGETPROVENANCE_H
#define AIWORLD_HUNTTARGETPROVENANCE_H

#include "Define.h"
#include "ObjectGuid.h"

// Milestone 2.12G3A: explicit, value-only identity/provenance for a HUNT
// target - what HuntIntent/HuntProposal (both this same milestone) name as
// "the thing this group wants to hunt", entirely as data, never a live
// Creature*/Player*/Unit*. A HUNT target is fundamentally unlike a
// REGROUP/ROAM one (AgentGroupIntent's own MapId/X/Y/Z, a fixed territory
// point that names no entity at all) - it names a specific, individually
// identified external entity, one this group does not own the way it owns
// its own members, so its identity has to travel as an honest snapshot,
// not an assumed-current fact.
//
// TargetGuid is a plain TrinityCore ObjectGuid - the target's own stable
// engine identity, whatever it actually is (ObjectGuid::GetTypeId()/
// IsCreature()/IsPlayer() etc. already carry that distinction; this
// struct deliberately does NOT re-derive or duplicate it into a separate
// "kind" field, which would just be a second, independently-maintained
// copy of a fact ObjectGuid already owns). Nothing here assumes the
// target is any particular kind of entity, an AIWorld-known Agent, or
// even still alive right now - see MapId/X/Y/Z/ObservedAtMs/Alive below
// for why.
//
// MapId/X/Y/Z/Alive are a SNAPSHOT, not a live fact - exactly the same
// "absence from the grid/last-known state must never be misread as a
// still-current one" discipline CoalitionMemberObservation.h already
// documents for a group's own members, applied here to an external
// target instead. ObservedAtMs is the snapshot's own clock: how long ago
// this provenance was actually true, for whoever consumes it (the
// milestone that actually validates staleness/target death/unload/map
// change against a FRESH re-observation - deliberately NOT this
// milestone, see this file's own class comment above for the full
// G3A/G3B/G3C/G3D split) to decide whether it is still trustworthy
// enough to act on. A HuntTargetProvenance is never itself re-validated
// in place; a caller either has a fresh one or does not.
//
// TargetEntry (2.12G3A follow-up, STATIC review): the target's own
// creature template entry - added so a generic HuntIntentSystem/
// HuntIntentProjector (2.12G3B) can decide "is this an allowed kind of
// prey?" purely from data (comparing TargetEntry against whatever a
// profile names as eligible), the same "never branch on which species
// this is" discipline AgentGroupIntentSystem already holds for REGROUP/
// ROAM - without this field, expressing a per-profile eligible-prey
// policy would have forced a species-specific branch (a WolfLoose-only
// "is this a rabbit?" check) directly into what is meant to stay a
// generic pipeline. 0 (the zero value/default, ObjectGuid's own
// TargetGuid.IsEmpty() already covers "no target at all") is never a
// real creature entry and must fail closed the same way an empty
// TargetGuid does.
//
// G3A-G3D HUNT is currently restricted to persistent non-instance/
// base-world targets. Instance-aware target identity is outside this
// milestone. A target outside the group's base-world map must fail closed.
struct HuntTargetProvenance
{
    ObjectGuid TargetGuid;
    uint32 TargetEntry = 0;

    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;

    // Whether the target was alive AT THE MOMENT this snapshot was taken -
    // never re-derived, never assumed to still hold by the time anything
    // downstream actually reads it.
    bool Alive = false;

    // When this snapshot was taken (the same CurrentTimeMs() clock every
    // other attempt/observation identity in this codebase already uses -
    // see GroupCoordinationGoal::StartedAtMs, ActiveAction::GoalStartedAtMs).
    uint64 ObservedAtMs = 0;
};

#endif // AIWORLD_HUNTTARGETPROVENANCE_H
