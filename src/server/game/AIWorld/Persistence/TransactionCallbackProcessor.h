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

#ifndef AIWORLD_TRANSACTIONCALLBACKPROCESSOR_H
#define AIWORLD_TRANSACTIONCALLBACKPROCESSOR_H

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"

// Milestone 2.12E2: TransactionCallback's own sibling to QueryCallbackProcessor
// (DatabaseEnvFwd.h's own alias for AsyncCallbackProcessor<QueryCallback>,
// used throughout the engine for e.g. World::_queryProcessor) - no
// equivalent alias exists upstream for TransactionCallback, so AIWorld
// defines its own here rather than repeating the template spelling at
// every call site.
//
// Every AgentGroupPersistence async write (CreateGroupAsync()/
// AddGroupMemberAsync()/RemoveGroupMemberAsync()/DeleteGroupAsync())
// returns a TransactionCallback that the caller must AddCallback() into a
// processor of this type and poll once per world tick via
// ProcessReadyCallbacks() - exactly the same "enqueue, poll every Update()"
// shape World::Update() already uses for _queryProcessor. A completion
// only ever runs from inside that ProcessReadyCallbacks() call, so it is
// always world-thread-only, the same as everything else in AIWorld -
// there is no other place a completion lambda can fire from.
using TransactionCallbackProcessor = AsyncCallbackProcessor<TransactionCallback>;

#endif // AIWORLD_TRANSACTIONCALLBACKPROCESSOR_H
