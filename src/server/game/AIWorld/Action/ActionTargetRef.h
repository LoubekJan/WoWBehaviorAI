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

#ifndef AIWORLD_ACTIONTARGETREF_H
#define AIWORLD_ACTIONTARGETREF_H

#include "Define.h"
#include "ObjectGuid.h"

// Milestone 2.12G3C1: what an ActionRequest claims as its own external
// target, as a plain value - no Creature*/Unit*/WorldObject*, the same
// value-identity discipline ActionRequest::FleeFromGuid already holds for
// a Flee request's own claimed threat source. Deliberately a narrower
// shape than HuntTargetProvenance (no position/alive/observed-at fields)
// - those are ActionValidationContext's own job to carry as
// AIWorldMgr-resolved, authoritative facts (see ActionValidationContext::
// TargetResolved/TargetAlive/TargetAttackable/TargetGuid/TargetEntry/
// TargetMapId/TargetX/Y/Z), never something the request itself is trusted
// to assert. Guid/Entry together are ActionSystem::Validate()'s only way
// to prove request and context agree on WHICH target this is
// (TargetIdentityMismatch/TargetEntryMismatch) before it ever considers
// the request's own Destination geometrically valid.
struct ActionTargetRef
{
    ObjectGuid Guid;
    uint32 Entry = 0;
};

#endif // AIWORLD_ACTIONTARGETREF_H
