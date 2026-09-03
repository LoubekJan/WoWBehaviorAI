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

#ifndef AIWORLD_DYNAMICTASKCANDIDATE_H
#define AIWORLD_DYNAMICTASKCANDIDATE_H

#include "Define.h"
#include "QuestContext.h"
#include "QuestProposalDraft.h"
#include "QuestRequestProvenance.h"

// Milestone 2.13A3B: the end result of a dynamic-task response that has
// passed every provenance/staleness/target-binding check
// AIWorldMgr::HandleDynamicTaskResponse() runs - a purely inert value
// handoff to a future 2.13B authoritative validator. Nothing here
// authorizes gameplay: no Creature*/Player*/Unit*/Map*/Quest*, and
// AIWorldMgr::OnDynamicTaskCandidateAccepted() (the only place this is
// ever produced) does nothing with it but log - no ActionRequest, no
// ActionSystem/ActionExecutor call, no DB write, no reward, no world
// mutation. RequestContext/Provenance are exactly what the originating
// PendingDynamicTaskRequest held, so a future validator can re-derive
// everything about the request this draft answers without re-deriving
// live world state itself.
struct DynamicTaskCandidate
{
    uint64 RequestId = 0;
    uint64 AcceptedAtMs = 0;

    QuestContext RequestContext;
    QuestRequestProvenance Provenance;
    QuestProposalDraft Draft;
};

#endif // AIWORLD_DYNAMICTASKCANDIDATE_H
