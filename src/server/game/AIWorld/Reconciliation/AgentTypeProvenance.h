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

#ifndef AIWORLD_AGENTTYPEPROVENANCE_H
#define AIWORLD_AGENTTYPEPROVENANCE_H

#include "Agent/AgentType.h"
#include "Define.h"

// Milestone 2.12F4B: deterministic, reproducible AgentType classification
// from a world.creature spawn's own npcflag - pure function, no DB/live
// pointers. Only one rule exists so far: creature.npcflag's own vendor bit
// (UNIT_NPC_FLAG_VENDOR, see UnitDefines.h) is unambiguous and maps to
// Merchant. Every other case - including "is this a guard", which has no
// single reliable deterministic signal in spawn data alone yet - returns
// AgentType::Unclassified rather than guessing; a bulk bootstrap must
// never silently mislabel thousands of spawns as Civilian just because
// that enumerator happens to be 0. Extend this function's own rule set
// (not its callers) when a new deterministic signal is found.
TC_GAME_API AgentType DeriveCreatureAgentType(uint32 npcFlags);

#endif // AIWORLD_AGENTTYPEPROVENANCE_H
