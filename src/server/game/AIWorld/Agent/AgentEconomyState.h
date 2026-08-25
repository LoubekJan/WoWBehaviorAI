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

#ifndef AIWORLD_AGENTECONOMYSTATE_H
#define AIWORLD_AGENTECONOMYSTATE_H

#include "Define.h"

// Milestone 2.11E2: a persistent stockpile, distinct from NeedsState::
// ResourcePressure (a 0.0-1.0 drive to act, drifting over time - see
// NeedsState.h) - this is an actual accumulated count, changed only by a
// specific ActionCompletion, never by a drift rate. Not optional, unlike
// HomeLocation/WorkLocation: every agent has one, defaulting to zero
// rather than "unset" - there is no meaningful absence of a stockpile the
// way there is of a home. Money is the only field any current mutation
// path (2.11E2's WORK completion) actually changes; Food/Resource exist
// and persist now so a later milestone's production doesn't need another
// schema migration, but nothing writes to them yet.
struct AgentEconomyState
{
    uint32 Money = 0;
    uint32 Food = 0;
    uint32 Resource = 0;
};

#endif // AIWORLD_AGENTECONOMYSTATE_H
