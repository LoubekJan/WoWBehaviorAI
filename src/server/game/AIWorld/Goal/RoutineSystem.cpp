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

#include "RoutineSystem.h"

std::optional<RoutineGoal> RoutineSystem::DeriveGoal(std::optional<ActiveGoal> const& currentGoal,
    std::optional<AgentLocation> const& home, std::optional<AgentLocation> const& work,
    uint64 nowMs, RoutineScheduleConfig const& config) const
{
    if (!home || !work)
        return std::nullopt;

    // Same rule GoalSystem::UpdateActiveGoal() already enforces the other
    // direction (a Normal candidate can never interrupt an active
    // Emergency goal) - here it means an active FLEE_DANGER fully
    // suppresses routine output rather than racing it for the agent's
    // attention. AIWorldMgr still owns deciding what (if anything) acts on
    // that suppression; this only stops offering a routine destination
    // while it holds.
    if (currentGoal && currentGoal->Priority == GoalPriority::Emergency)
        return std::nullopt;

    // config.DayLengthMs is validated > 0 by AIWorldMgr::Initialize()
    // before this is ever called - trusted, not re-checked here (same
    // convention as FoodTargetResolver trusting FoodTargetConfig).
    uint32 elapsedInDayMs = uint32(nowMs % config.DayLengthMs);
    bool isWorkHours = elapsedInDayMs >= config.WorkStartMs && elapsedInDayMs < config.WorkEndMs;

    AgentLocation const& source = isWorkHours ? *work : *home;

    RoutineGoal goal;
    goal.Type = isWorkHours ? GoalType::GoToWork : GoalType::GoHome;
    goal.Target.MapId = source.MapId;
    goal.Target.X = source.X;
    goal.Target.Y = source.Y;
    goal.Target.Z = source.Z;

    return goal;
}
