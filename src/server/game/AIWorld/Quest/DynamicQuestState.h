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

#ifndef AIWORLD_DYNAMICQUESTSTATE_H
#define AIWORLD_DYNAMICQUESTSTATE_H

#include "Define.h"

// Milestone 2.13C1: the dynamic quest lifecycle. Every legal transition:
//   Offered -> Active     (DynamicQuestLifecycle::AcceptDynamicQuest)
//   Offered -> Expired    (DynamicQuestLifecycle::ExpireDynamicQuest)
//   Active  -> Completed  (DynamicQuestLifecycle::CompleteDynamicQuest)
//   Active  -> Failed     (DynamicQuestLifecycle::FailDynamicQuest)
//   Active  -> Expired    (DynamicQuestLifecycle::ExpireDynamicQuest)
// Completed/Failed/Expired are terminal - no transition ever leaves them;
// every further transition attempt is fail-closed rejected with
// DynamicQuestRejectReason::AlreadyTerminal.
//
// Offered -> Failed is deliberately NOT modeled: this milestone defines
// no explicit server-cancellation trigger, so that edge stays unreachable
// until a later milestone defines one on purpose.
enum class DynamicQuestState : uint8
{
    Offered = 0,
    Active,
    Completed,
    Failed,
    Expired
};

char const* ToString(DynamicQuestState state);

#endif // AIWORLD_DYNAMICQUESTSTATE_H
