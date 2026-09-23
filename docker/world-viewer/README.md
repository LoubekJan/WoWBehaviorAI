# AI World Viewer

The viewer is a read-only FastAPI service. Compose publishes its browser UI on
all host interfaces at port 8090 by default (`http://<server-LAN-IP>:8090`).
The worldserver sends a complete snapshot about once per second to
`http://world-viewer:8000/internal/telemetry`.

Set a long random `WORLD_VIEWER_TELEMETRY_TOKEN` in `.env` before enabling
export in the worldserver. Compose passes the same token to both containers.
With an empty token, the viewer remains available but refuses all ingestion.
The browser's `GET /api/state` is unauthenticated: anyone on the network who
can reach the published port can read the telemetry.

`POST /internal/telemetry` requires `Authorization: Bearer <token>` and a
version 1 JSON body:

```json
{
  "version": 1,
  "captured_at_ms": 1800000000000,
  "agents": [
    {
      "agent_id": 80542,
      "spawn_id": 80542,
      "entry": 247,
      "name": "Billy Maclure",
      "type": "CIVILIAN",
      "control_mode": "AI_WORLD_CONTROLLED",
      "world_faction": 1,
      "world_state": "MATERIALIZED",
      "simulation_tier": "NEARBY",
      "position": {"x": -9923.68, "y": 38.39, "z": 32.6, "map_id": 0, "source": "live"},
      "health": 84,
      "max_health": 100,
      "alive": true,
      "in_combat": false,
      "needs": {"health_pressure": 0.16, "hunger": 0.71, "fatigue": 0.32, "safety_pressure": 0, "resource_pressure": 0.18},
      "goal": "GET_FOOD",
      "goal_utility": 0.71,
      "action": "MOVE_TO",
      "routine_goal": "GO_HOME",
      "group_id": 17,
      "economy": {"money": 120, "food": 3, "resource": 0},
      "living_role": {
        "role": "CIVILIAN",
        "status": "ACTIVE",
        "phase": "SEEKING_SAFETY",
        "activity": "NONE",
        "awareness": "PREDATOR_SEEN",
        "movement_purpose": "GUARD_REFUGE",
        "caution": 0.42,
        "extensions_enabled": true,
        "hunt_status": null,
        "assist_status": null,
        "nearby_prey": 0,
        "attackable_prey": 0,
        "nearby_allies": 0,
        "allies_in_combat": 0,
        "companion_spawn_id": null
      }
    }
  ]
}
```

Optional agent fields may be omitted or null. `economy` is the persistent
stockpile and is sent for every agent. `living_role` mirrors the
`.aiworld group status` role diagnostics (see `doc/LivingRoles.md`) and is
sent only for materialized agents; `hunt_status` is set only for predators and
`assist_status` only for guards and combatants. The viewer shows the role
phase (including `SEEKING_SAFETY` and `INVESTIGATING`), awareness, movement
purpose and stockpiles, and can filter by phase or by non-`QUIET` awareness. `position.source` is `live`,
`spawn`, or `last_known`; only `live` means a current Creature position.
The next accepted batch replaces the entire previous batch. Requests are
limited to 8 MiB and 10,000 agents. Duplicate AgentIds and malformed batches
are rejected without changing the cached state. The state becomes stale five
seconds after its most recent receipt; stale age is based on the receiver's
monotonic clock, not the source timestamp. Browser polling is every second.

Run the API tests in the built image with:

```sh
docker compose run --rm --no-deps world-viewer python -m unittest discover -s tests -v
```
