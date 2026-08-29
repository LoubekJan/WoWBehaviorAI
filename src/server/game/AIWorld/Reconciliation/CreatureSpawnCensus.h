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

#ifndef AIWORLD_CREATURESPAWNCENSUS_H
#define AIWORLD_CREATURESPAWNCENSUS_H

#include "CreatureSpawnIdentity.h"
#include "Define.h"
#include <vector>

// Milestone 2.12F4B: one-shot, startup-only enumeration of every eligible
// persistent non-instance world.creature spawn, as a pure value snapshot.
// Touches only ObjectMgr's already-resident in-memory creature spawn store
// (sObjectMgr->GetAllCreatureData() - populated once at boot by
// ObjectMgr::LoadCreatures(), long before AIWorldMgr::Initialize() ever
// runs, see World::SetInitialWorldSettings()) and sMapStore's static DBC
// map metadata - never a live Creature*/Map*, and no SQL query of its own.
// Safe to call regardless of which maps/grids are currently loaded - this
// is exactly what proves census does not depend on
// sMapMgr->FindBaseNonInstanceMap() (a live-map resolver AIWorldMgr's own
// runtime processing elsewhere legitimately uses, but which is unsafe as a
// census/scope filter - see AIWorld_Current_Roadmap.md's own Scope
// predicate section for why).
//
// Eligibility (2.12F4A2/2.12F4B Scope predicate, deterministic/static):
// MapEntry exists AND MapEntry::Instanceable() == false. Temporary
// summons are automatically excluded without any extra filtering here -
// they are never written to the `creature` table, so they never appear in
// ObjectMgr's own creature spawn store to begin with.
TC_GAME_API std::vector<CreatureSpawnIdentity> BuildCreatureSpawnCensus();

#endif // AIWORLD_CREATURESPAWNCENSUS_H
