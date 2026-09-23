/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "TelemetryJsonCodec.h"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace
{
    void WriteString(std::ostream& out, std::string const& value)
    {
        out << '"';
        for (unsigned char c : value)
        {
            switch (c)
            {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c)
                            << std::dec << std::setfill(' ');
                    }
                    else
                        out << char(c);
            }
        }
        out << '"';
    }

    void WriteFloat(std::ostream& out, float value)
    {
        if (std::isfinite(value))
            out << value;
        else
            out << "null";
    }

    template<typename T> void WriteValue(std::ostream& out, T value) { out << value; }
    void WriteValue(std::ostream& out, float value) { WriteFloat(out, value); }
    void WriteValue(std::ostream& out, bool value) { out << (value ? "true" : "false"); }
    void WriteValue(std::ostream& out, std::string const& value) { WriteString(out, value); }
    void WriteValue(std::ostream& out, GoalType value) { WriteString(out, ToString(value)); }
    void WriteValue(std::ostream& out, ActionPosition const& point)
    {
        out << "{\"map_id\":" << point.MapId << ",\"x\":";
        WriteFloat(out, point.X);
        out << ",\"y\":"; WriteFloat(out, point.Y);
        out << ",\"z\":"; WriteFloat(out, point.Z);
        out << '}';
    }
    template<typename T> void WriteValue(std::ostream& out, std::optional<T> const& value)
    {
        if (value) WriteValue(out, *value); else out << "null";
    }
    // Use a distinct name from the database Field class brought in by gamePCH.h.
    template<typename T> void WriteJsonField(std::ostream& out, char const* key, T const& value)
    {
        out << ','; WriteString(out, key); out << ':'; WriteValue(out, value);
    }

    void WriteValue(std::ostream& out, LivingRoleTelemetry const& role)
    {
        out << "{\"enabled\":"; WriteValue(out, role.Enabled);
        WriteJsonField(out, "extensions_enabled", role.ExtensionsEnabled);
        WriteJsonField(out, "role", role.Role); WriteJsonField(out, "status", role.Status);
        WriteJsonField(out, "phase", role.Phase); WriteJsonField(out, "activity", role.Activity);
        WriteJsonField(out, "awareness", role.Awareness); WriteJsonField(out, "movement_purpose", role.MovementPurpose);
        WriteJsonField(out, "caution", role.Caution); WriteJsonField(out, "started_at_ms", role.StartedAtMs);
        WriteJsonField(out, "decision_wait_ms", role.DecisionWaitMs);
        WriteJsonField(out, "danger_remaining_ms", role.DangerRemainingMs); WriteJsonField(out, "alarm_remaining_ms", role.AlarmRemainingMs);
        WriteJsonField(out, "hunt_status", role.HuntStatus); WriteJsonField(out, "hunt_end", role.HuntEnd);
        WriteJsonField(out, "assist_status", role.AssistStatus);
        WriteJsonField(out, "nearby_prey", role.NearbyPrey); WriteJsonField(out, "attackable_prey", role.AttackablePrey);
        WriteJsonField(out, "nearby_allies", role.NearbyAllies); WriteJsonField(out, "allies_in_combat", role.AlliesInCombat);
        WriteJsonField(out, "companion_spawn_id", role.CompanionSpawnId);
        WriteJsonField(out, "last_hunt_target_spawn_id", role.LastHuntTargetSpawnId);
        WriteJsonField(out, "hunt_target_distance", role.HuntTargetDistance);
        WriteJsonField(out, "prey_run_speed", role.PreyRunSpeed); WriteJsonField(out, "prey_move_speed", role.PreyMoveSpeed);
        WriteJsonField(out, "sprint_multiplier", role.SprintMultiplier); WriteJsonField(out, "sprint_remaining_ms", role.SprintRemainingMs);
        out << '}';
    }
    void WriteValue(std::ostream& out, MovementTelemetry const& movement)
    {
        out << "{\"moving\":"; WriteValue(out, movement.Moving);
        WriteJsonField(out, "blocked", movement.Blocked); WriteJsonField(out, "cannot_reach_target", movement.CannotReachTarget);
        WriteJsonField(out, "evading", movement.Evading); WriteJsonField(out, "run_speed", movement.RunSpeed);
        WriteJsonField(out, "move_speed", movement.MoveSpeed); WriteJsonField(out, "home_distance", movement.HomeDistance);
        out << '}';
    }
    void WriteValue(std::ostream& out, TargetTelemetry const& target)
    {
        out << "{\"spawn_id\":" << target.SpawnId;
        WriteJsonField(out, "entry", target.Entry); WriteJsonField(out, "name", target.Name); WriteJsonField(out, "position", target.Position);
        out << '}';
    }
    void WriteValue(std::ostream& out, GroupTelemetry const& group)
    {
        out << "{\"id\":" << group.Id;
        WriteJsonField(out, "kind", group.Kind); WriteJsonField(out, "profile", group.Profile);
        WriteJsonField(out, "member_count", group.MemberCount); WriteJsonField(out, "resources", group.Resources);
        WriteJsonField(out, "territory", group.Territory);
        out << '}';
    }

    // Overloads must be declared before the optional template instantiates for
    // nested DTOs (ordinary lookup in templates is fixed at definition time).
    template<typename T> void OptionalObject(std::ostream& out, char const* key, std::optional<T> const& value)
    {
        out << ','; WriteString(out, key); out << ':';
        if (value) WriteValue(out, *value); else out << "null";
    }

}

std::string SerializeAgentTelemetry(std::vector<AgentTelemetrySnapshot> const& snapshots, uint64 capturedAtMs)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(7);
    out << "{\"version\":2,\"captured_at_ms\":" << capturedAtMs << ",\"agents\":[";
    bool first = true;
    for (AgentTelemetrySnapshot const& item : snapshots)
    {
        if (!first)
            out << ',';
        first = false;
        out << "{\"agent_id\":" << item.Agent.Value << ",\"spawn_id\":" << item.SpawnId
            << ",\"entry\":" << item.Entry << ",\"name\":";
        WriteString(out, item.Name);
        out << ",\"type\":";
        WriteString(out, ToString(item.Type));
        out << ",\"control_mode\":";
        WriteString(out, ToString(item.ControlMode));
        out << ",\"world_faction\":" << item.WorldFaction.Value
            << ",\"world_state\":";
        WriteString(out, item.Live ? "MATERIALIZED" : "ABSTRACT");
        out << ",\"simulation_tier\":";
        WriteString(out, ToString(item.Tier));
        out << ",\"position\":{\"map_id\":" << item.MapId << ",\"x\":";
        WriteFloat(out, item.Live ? item.Live->X : item.SpawnX);
        out << ",\"y\":";
        WriteFloat(out, item.Live ? item.Live->Y : item.SpawnY);
        out << ",\"z\":";
        WriteFloat(out, item.Live ? item.Live->Z : item.SpawnZ);
        out << ",\"source\":\"" << (item.Live ? "live" : "spawn") << "\"}";
        out << ",\"health\":";
        if (item.Live) out << item.Live->Health; else out << "null";
        out << ",\"max_health\":";
        if (item.Live) out << item.Live->MaxHealth; else out << "null";
        out << ",\"alive\":";
        if (item.Live) out << (item.Live->Alive ? "true" : "false"); else out << "null";
        out << ",\"in_combat\":";
        if (item.Live) out << (item.Live->InCombat ? "true" : "false"); else out << "null";
        out << ",\"needs\":{\"health_pressure\":";
        WriteFloat(out, item.Needs.HealthPressure);
        out << ",\"hunger\":";
        WriteFloat(out, item.Needs.Hunger);
        out << ",\"fatigue\":";
        WriteFloat(out, item.Needs.Fatigue);
        out << ",\"safety_pressure\":";
        WriteFloat(out, item.Needs.SafetyPressure);
        out << ",\"resource_pressure\":";
        WriteFloat(out, item.Needs.ResourcePressure);
        out << "},\"goal\":";
        if (item.Goal) WriteString(out, ToString(*item.Goal)); else out << "null";
        out << ",\"goal_utility\":";
        if (item.GoalUtility) WriteFloat(out, *item.GoalUtility); else out << "null";
        out << ",\"routine_goal\":";
        if (item.RoutineGoal) WriteString(out, ToString(*item.RoutineGoal)); else out << "null";
        out << ",\"action\":";
        if (item.Action) WriteString(out, ToString(*item.Action)); else out << "null";
        out << ",\"group_id\":";
        if (item.GroupId) out << *item.GroupId; else out << "null";
        out << ",\"economy\":{\"money\":";
        // Decimal string preserves uint64 copper exactly in JavaScript.
        WriteString(out, std::to_string(item.Economy.Money));
        WriteJsonField(out, "food", item.Economy.Food); WriteJsonField(out, "resource", item.Economy.Resource);
        out << '}';
        WriteJsonField(out, "home", item.Home); WriteJsonField(out, "work", item.Work);
        WriteJsonField(out, "reputation_faction_id", item.ReputationFactionId);
        WriteJsonField(out, "faction_template_id", item.Live ? item.FactionTemplateId : std::nullopt);
        WriteJsonField(out, "effective_goal", item.EffectiveGoal); WriteJsonField(out, "goal_owner", item.GoalOwner);
        WriteJsonField(out, "action_source_goal", item.ActionSourceGoal); WriteJsonField(out, "action_started_at_ms", item.ActionStartedAtMs);
        WriteJsonField(out, "coordination_goal", item.CoordinationGoal); WriteJsonField(out, "coordination_group_id", item.CoordinationGroupId);
        WriteJsonField(out, "coordination_phase", item.CoordinationPhase); WriteJsonField(out, "routine_activity", item.RoutineActivity);
        WriteJsonField(out, "living_wolf", item.Live && item.LivingWolf);
        WriteJsonField(out, "meal_target_spawn_id", item.Live ? item.MealTargetSpawnId : std::nullopt);
        WriteJsonField(out, "destination", item.Live ? item.Destination : std::nullopt);
        OptionalObject(out, "living_role", item.Live ? item.LivingRole : std::nullopt);
        OptionalObject(out, "movement", item.Live ? item.Movement : std::nullopt);
        OptionalObject(out, "target", item.Live ? item.Target : std::nullopt);
        out << ",\"groups\":[";
        bool firstGroup = true;
        for (GroupTelemetry const& group : item.Groups)
        {
            if (!firstGroup) out << ',';
            firstGroup = false;
            WriteValue(out, group);
        }
        out << ']';
        out << '}';
    }
    out << "]}";
    return out.str();
}

