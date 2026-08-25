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

#ifndef AIWORLD_ROUTINESCHEDULECONFIG_H
#define AIWORLD_ROUTINESCHEDULECONFIG_H

#include "Define.h"

// Milestone 2.11B: shared configuration, not per-agent state - same
// config-struct-passed-per-call pattern as NeedsUpdateRates/FoodTargetConfig.
// AIWorldMgr owns one instance, loaded once at Initialize() from
// AIWorld.Routine* and cross-validated there (0 <= WorkStartMs < WorkEndMs
// <= DayLengthMs) - RoutineSystem trusts this struct is already consistent
// and does not re-validate it.
//
// Deliberately a synthetic, config-driven cycle rather than either
// GameTime::GetSystemTime() (what the rest of AIWorld's scheduling already
// uses - but a real wall-clock hour would only flip GO_TO_WORK/GO_HOME once
// per real day, unusable for a runtime gate) or TrinityCore's in-game day/
// night cycle (ties routine to what players see, but still tens of minutes
// per cycle). nowMs % DayLengthMs is what RoutineSystem::DeriveGoal() turns
// into a phase - see its own comment.
struct RoutineScheduleConfig
{
    uint32 DayLengthMs = 1200000;
    uint32 WorkStartMs = 400000;
    uint32 WorkEndMs = 800000;
};

#endif // AIWORLD_ROUTINESCHEDULECONFIG_H
