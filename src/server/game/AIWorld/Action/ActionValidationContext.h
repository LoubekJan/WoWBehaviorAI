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

#ifndef AIWORLD_ACTIONVALIDATIONCONTEXT_H
#define AIWORLD_ACTIONVALIDATIONCONTEXT_H

#include "ActionPosition.h"
#include "Agent/AgentType.h"
#include "Define.h"
#include "Goal/GoalType.h"
#include "Goal/RoutineActivityType.h"
#include "ObjectGuid.h"
#include <optional>

// Milestone 2.8A/2.8B/2.8D: the world-thread facts ActionSystem::Validate()
// is allowed to see, as plain values - AIWorldMgr resolves Materialized/
// Alive/the actor's current ActiveGoal/current threat victim/current
// position itself (it already has the live Creature and AgentRecord at
// the call site) and hands over only this. ActionSystem never sees a
// Creature*, AgentRecord*, Unit*, Map*, or the registry.
struct ActionValidationContext
{
    bool Materialized = false;
    bool Alive = false;

    // Milestone 2.12F4A: the actor's own AgentRecord::ControlMode, as a
    // plain value - AgentControlMode is a pure enum (AgentType.h), so
    // carrying it here does not pull a Creature*/AgentRecord*/registry
    // reference across the pure/value boundary this context exists to
    // enforce. This is the mandatory authoritative safety gate: Validate()
    // rejects any request whose context.ControlMode is not
    // AIWorldControlled, unconditionally, before any per-ActionType check
    // - see Validate()'s own comment. Defaults to ObserveOnly (the same
    // fail-closed default AgentRecord::ControlMode itself has), so a
    // caller that forgets to set this explicitly gets REJECTED rather than
    // silently ALLOWED.
    AgentControlMode ControlMode = AgentControlMode::ObserveOnly;

    // Milestone 2.11C: despite the name, not always literally
    // AgentRecord::ActiveGoalState - for a routine-sourced MOVE_TO
    // (GoalType::GoToWork/GoHome) AIWorldMgr sets this from
    // RoutineGoalState instead, since Validate()'s honesty check only
    // needs "the caller's claimed current goal identity", not specifically
    // a Need-driven one. See ActionRequest::SourceGoal.
    std::optional<GoalType> ActiveGoalType;
    uint64 ActiveGoalStartedAtMs = 0;

    // Milestone 2.8B: the actor's actual current threat victim GUID
    // (ThreatManager::GetCurrentVictim()), empty if it has none. Compared
    // against ActionRequest::FleeFromGuid for a Flee request - reality,
    // not the request's claim.
    ObjectGuid FleeSourceGuid;

    // Milestone 2.8D: the actor's actual current position, for MoveTo's
    // map-match and max-range checks - reality, not wherever the request
    // claims the actor is.
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;

    // Milestone 2.8D P2 fix: whether the actor's MOTION_SLOT_ACTIVE
    // already has a movement generator running (GetCurrentMovementGenerator
    // (MOTION_SLOT_ACTIVE) != nullptr) - e.g. an in-progress FLEE.
    // ValidateMoveTo() rejects rather than let a new MoveTo either replace
    // a movement it doesn't own or queue silently behind one and only
    // actually start once that unrelated movement ends, by which point the
    // destination's range/map validation may no longer be true. MoveTo may
    // only start from an idle actor.
    bool HasActiveMovement = false;

    // Milestone 2.8G: the actor's actual current combat state. ValidateEat()
    // rejects while true - if danger appears the same tick the actor
    // arrives at a food target, it must not start eating a moment before
    // an emergency FLEE_DANGER takes over.
    bool InCombat = false;

    // Milestone 2.8G P2 fix: the authoritative "a MOVE_TO just arrived
    // here" facts for a GET_FOOD attempt - set only by AIWorldMgr, from
    // the ActionCompletion/AgentRecord::PendingEat that actually produced
    // them, never from the ActionRequest being validated. ValidateEat()
    // requires the request's Destination/SourceGoal/GoalStartedAtMs to
    // match this exactly; ArrivedDestination empty means no MOVE_TO has
    // just arrived at all, so no Eat request can be validated regardless
    // of what it claims. This is the boundary a future async/LLM-sourced
    // caller must not be able to route around by proposing Eat from
    // wherever the actor currently stands while GET_FOOD merely happens
    // to be active.
    std::optional<ActionPosition> ArrivedDestination;
    GoalType ArrivedSourceGoal = GoalType::GetFood;
    uint64 ArrivedGoalStartedAtMs = 0;

    // Milestone 2.11E1 P3 fix: independent of ActiveGoalType/
    // ActiveGoalStartedAtMs above - for Work/Rest, AIWorldMgr currently
    // populates both of those from this same RoutineActivityState in the
    // same synchronous call, which makes Validate()'s generic honesty
    // check tautological (a request built from a queued/stale/external
    // source could satisfy it purely because both sides were copied from
    // whatever the caller currently holds). Set only from AgentRecord::
    // RoutineActivityState, never from the ActionRequest being validated -
    // ValidateWork()/ValidateRest() cross-check the request's SourceGoal/
    // GoalStartedAtMs against this authoritative pair too, the same
    // "two independent copies, checked for equality" pattern
    // ArrivedDestination/ArrivedSourceGoal/ArrivedGoalStartedAtMs above
    // already uses for Eat. Unset (nullopt) means no RoutineActivityState
    // exists at all, so no Work/Rest request can be validated regardless
    // of what it claims.
    std::optional<RoutineActivityType> ExpectedRoutineActivity;
    uint64 RoutineActivityStartedAtMs = 0;
};

#endif // AIWORLD_ACTIONVALIDATIONCONTEXT_H
