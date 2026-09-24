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

## Observer features

- Living role, current phase/activity, perception, danger/alarm timers and companion.
- Predator hunt/assist diagnostics, last hunt result, prey distance and speed,
  chase sprint multiplier and time remaining. Wolf pack activity is derived from
  its effective goal, independently of the living-role controller.
- Movement speed, blocked movement, unreachable target, evade and home distance.
- Individual/routine/group goal ownership, action age, group hunt phase,
  all group memberships, profiles, resources and territories.
- Money (exact copper), food, resources, home/work coordinates and routine activity.
- WorldFaction, optional mapped reputation Faction.dbc ID and the current live
  FactionTemplate ID. These IDs do not describe a reaction to a particular player.
- Role/activity/group and danger/path filters, activity colors, and selected
  NPC links to its target, destination and live companion.
- Long-term memory on selection: event/observation type, importance, perception
  channel, first/last observation, repetition count, historical participants and
  location. Pages contain 25 records and can be refreshed explicitly.

Hunt status/end describe the last diagnostic result, not necessarily a current
hunt; use the current phase. Background agents expose persisted/registry values
but no live role, movement, target, destination or FactionTemplate observation.
The viewer's scope remains AI-controlled Elwynn agents on map 0.

## Upgrade an existing deployment

From the repository root on the Docker host, update the receiver first. It accepts
protocol versions 1–3, while older receivers reject version 3:

```sh
docker compose up -d --build world-viewer
make build
make restart-world
```

Keep the existing `.env` token and port. No database migration is needed for this
Observer update. Reload the browser after upgrading. `/api/state` reports
`version: 3` after the first snapshot from the rebuilt worldserver. Versions 1
and 2 continue to show their supported fields, with a note that memory requires
an updated worldserver.
No snapshot is retained across a viewer restart; it waits for the next export.

## Several-hour recordings

The optional recorder saves all agents in the Observer's Elwynn scope, including
their complete role, hunt, movement, needs, group and economy fields. It polls the
public `/api/state`; it does not need the ingest token and does not issue gameplay
or memory-page requests. No worldserver rebuild is needed to add this recorder.
See [the Czech test instructions](../../doc/ObserverRecording.md).

From the updated checkout on the Docker host, with the worldserver running:

```sh
make record-aiworld
make record-aiworld-status
# Optional early finish:
make record-aiworld-stop
```

The default is four hours at five-second intervals, running in a detached
`aiworld-recorder` container. It continues after SSH disconnect and stops
automatically. For a different duration/interval, start with:

```sh
AIWORLD_RECORD_HOURS=8 AIWORLD_RECORD_INTERVAL=5 AIWORLD_RECORD_LABEL="Elwynn overnight" make record-aiworld
```

Changing these settings while recording recreates the recorder, ending the
current session and opening a new one. Every run writes a new uniquely named
directory under `runtime/recordings/aiworld-<UTC>-<id>/`; previous runs are retained.
The Compose service has the `recording` profile, so normal `make start` does not
enable it. `make stop` stops it along with the stack. It does not automatically
restart after a host/Docker restart.

Each session contains `summary.json` (updated at status changes and at least every
minute while sampling) and `part-0001.jsonl.gz`, etc. Parts rotate at approximately
64 MiB compressed, at a complete-record boundary. This is a per-part target, not
a total disk quota. Graceful stop closes gzip; a forced kill or power loss may
leave the current part incomplete, while closed previous parts remain readable.
Collect the complete session directory after it stops, including **all parts**.

The summary's `counts.fresh` counts samples with fresh, nonempty agent data.
Other sample statuses are `empty`, `waiting` (no telemetry yet), `unconfigured`,
`stale` and `error` (HTTP/connection/invalid response). A run with no fresh nonempty
samples exits with code 2. A normal exit or `usable: true` only confirms that data
was collected, **not** that NPC behavior was correct. The recorder keeps polling
through outages and records recovery. Check the status after the first minute;
`last_status: fresh` and a growing `counts.fresh` confirm collection.

### File format and analysis limits

Each gzip part is UTF-8 JSON Lines with a `session` metadata record first, followed
by `sample` records. Successful HTTP samples include the full unchanged API object
in `state`; failed requests have an `error` object and no fabricated state. The
final part ends with a `summary` record, also saved separately as `summary.json`.
`format_version: 1` identifies this recording format independently of the telemetry
protocol. Source capture/receipt timestamps, freshness and age are retained;
recorder UTC time, monotonic elapsed time, sequence and request duration are added.
Source-clock resets are retained, not treated as duplicate snapshots.

These are periodic snapshots, not an exhaustive event log: short attacks or
transitions between samples may be missed. Repeated `hunt_end` values do not mean
repeated failures. Analyze them with phase, target, time and position changes;
never interpret stale/error intervals as NPC inactivity. Background agents have
no live movement observation, and paginated long-term memory is not recorded.
Check `AIWorld.ElwynnAlwaysActive = 1` on the running server if the test is intended
to keep simulating all of Elwynn without players. Existing `Server.log` can provide
additional context; copy it before restarting the worldserver, which opens it in
overwrite mode in the current deployment configuration.

The recorder also runs directly with Python 3.11+ (no pip dependencies):

```sh
python3 docker/world-viewer/app/record.py --hours 4 --url http://localhost:8090/api/state
```

Recorder tests use a local HTTP fixture and compressed readback, with no game
server required:

```sh
cd docker/world-viewer
python3 -m unittest discover -s tests -p test_recording.py -v
```

## Protocol

`POST /internal/telemetry` requires `Authorization: Bearer <token>`. The C++
exporter sends version 3; the receiver also accepts versions 1 and 2 for rolling updates.
The following minimal version 1 example remains valid:

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
      "group_id": 17
    }
  ]
}
```

Nullable agent fields may be omitted or null. Version 2 adds `living_role`,
`movement`, `economy`, `groups`, `home`, `work`, `effective_goal`, `goal_owner`,
`action_source_goal`, `action_started_at_ms`, `coordination_goal`,
`coordination_group_id`, `coordination_phase`, `routine_activity`, `living_wolf`,
`meal_target_spawn_id`, `destination`, `target`, `reputation_faction_id` and
`faction_template_id`. Missing collections and flags default to empty/false.
The complete shape is defined in `app/telemetry.py`, with a C++-generated example
in `tests/fixtures/telemetry_v2.json`. `economy.money` is a decimal string because
the engine uses uint64 and JavaScript numbers cannot represent every value.
Capture and action timestamps use the worldserver's clock; the page uses receipt
time for wall-clock display and differences for action duration.

`position.source` is `live`,
`spawn`, or `last_known`; only `live` means a current Creature position.
The next accepted batch replaces the entire previous batch. Requests are
limited to 8 MiB and 10,000 agents. Duplicate AgentIds and malformed batches
are rejected without changing the cached state. The state becomes stale five
seconds after its most recent receipt; stale age is based on the receiver's
monotonic clock, not the source timestamp. Browser polling is every second.

### Long-term memory (v3)

Click an agent and open **Dlouhodobá paměť** in its detail. The browser requests
`GET /api/agents/{agent_id}/memory?offset=0&anchor=0`. This public read endpoint
has the same network visibility as the map, and accepts only agents in the current
Observer snapshot. It reports `pending`, `ready`, `unsupported`, or `offline`.
The default open panel loads automatically on selection; **Obnovit paměť** reloads
the first page. **Předchozí / Další** traverse all records, 25 per page.

The viewer queues a read request and returns its opaque ID, AgentId, offset and
anchor in `X-Observer-Memory-Request` on the next authenticated telemetry response.
The asynchronous exporter validates this narrow header format and hands one read
to the world thread. Capture rechecks the Elwynn scope, copies at most 25 memories
from the live long-term memory index (also for background agents), and sends an
optional root `memory_page` in the next telemetry batch. No grids are loaded and
no database queries or network operations run on the world thread. No gameplay
command can be expressed by this read request.

Pages are separate from `/api/state`, so other map viewers do not receive the
selected NPC's history on every poll. The receiver limits pending reads to 32
(60-second expiry) and cached pages to 64 (30-second expiry). Request IDs prevent
late responses from fulfilling a different request; pending reads rotate fairly.
The normal 8 MiB telemetry limit still applies.

Order is newest insertion first, with loaded records ordered by `memory_id`.
The returned `anchor` pins pagination against new insertions; use `refresh=true`
with offset/anchor zero to include newly added records. These are the agent's
actual long-term records, including current in-memory observation counts, not
short-term memory or the subset selected by decision-context retrieval.
Existing records can still be updated between pages. The capture time is shown.

NPC names come from their historical entry references; their current positions
are not substituted for the remembered location. Players are labelled generically;
player names and runtime GUIDs are not exported. Persistent/event/entity IDs in
records use decimal strings. A persistent ID of `0` means the running memory
index has not learned its database ID, **not** that persistence failed: asynchronous
inserts do not return the ID to that index. Empty, unavailable and pending memory
are displayed distinctly. See `tests/fixtures/telemetry_v3_memory.json` for an
example generated by the C++ codec.

Run the API tests in the built image with:

```sh
docker compose run --rm --no-deps world-viewer python -m unittest discover -s tests -v
```

Run the browser presentation rules with Node on the host:

```sh
node --test docker/world-viewer/tests/test_observer_model.js docker/world-viewer/tests/test_observer_memory.js
```

The C++ test target includes `tests/game/Telemetry.cpp` (`[Telemetry]` tag).
