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

#ifndef AIWORLD_DYNAMICTASKREQUEST_H
#define AIWORLD_DYNAMICTASKREQUEST_H

#include "Define.h"
#include "DynamicTaskProtocolVersion.h"
#include "QuestContext.h"

// Milestone 2.13A1: the future /dynamic-task wire request.
//
// RequestId will eventually be stamped by the transport exactly like
// AIClient stamps DecisionRequest::RequestId today.
struct DynamicTaskRequest
{
    DynamicTaskProtocolVersion Version =
        CurrentDynamicTaskProtocolVersion;

    uint64 RequestId = 0;

    QuestContext Context;
};

#endif // AIWORLD_DYNAMICTASKREQUEST_H
