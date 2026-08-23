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

#ifndef AIWORLD_ACTIONENGINEEVENT_H
#define AIWORLD_ACTIONENGINEEVENT_H

#include "Define.h"
#include "ObjectGuid.h"

// Milestone 2.8F: what AIWorldCreatureAI::MovementInform() is allowed to
// hand across threads - resolved from the live Creature at the moment of
// the callback (a map-updater thread, the same context FactorySelector::
// SelectAI() runs in), never a pointer. Deliberately not reused from
// WorldEvent/EventBus: an engine movement-completion callback is not a
// perception event, and giving it its own type keeps
// ActionEngineEventBus free to make assumptions (bounded queue, no
// correlation/cause chains) WorldEvent's contract doesn't promise.
struct ActionEngineEvent
{
    uint32 MapId = 0;
    uint64 SpawnId = 0;
    ObjectGuid RuntimeGuid;

    // MovementGeneratorType and PointMovementGenerator::GetId() at the
    // moment of the callback - AIWorldMgr::ProcessActionEngineEvent()
    // checks both against what it actually started before trusting this
    // as an arrival for a specific ActiveAction.
    uint32 MovementType = 0;
    uint32 MovementId = 0;
};

#endif // AIWORLD_ACTIONENGINEEVENT_H
