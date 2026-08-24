# WoWBehaviorAI

Experimental **persistent AI world layer for TrinityCore 3.3.5**.

The project extends TrinityCore with long-lived agents that can perceive the world, remember events, develop needs and goals, propose actions, and eventually participate in a larger simulated world without bypassing TrinityCore's authoritative gameplay rules.

The long-term goal is not to replace TrinityCore AI with a language model. The goal is to build a safe simulation layer where deterministic systems handle common behavior and external AI/inference is used only where it adds value.

> Development branch: `ai-world`
>
> Current milestone: **Etapa 2 / 2.9E complete**
>
> Next: **2.10A — scheduler, admission and bounded backpressure**

For the detailed implementation roadmap see [AI_TrinityCore_Roadmap_Etapa_1_2.md](AI_TrinityCore_Roadmap_Etapa_1_2.md).

---

## Core design rule

**AI proposes. `ActionSystem` validates. TrinityCore executes.**

```text
WORLD STATE
    │
    ▼
EVENT
    │
    ▼
PERCEPTION
    │
    ▼
MEMORY
    │
    ▼
NEEDS
    │
    ▼
GOALS
    │
    ▼
DECISION / PLANNING
    │
    ▼
ActionRequest
    │
    ▼
ActionSystem::Validate
    │
    ▼
TrinityCore gameplay execution
    │
    └──────────────► WORLD STATE
```

AIWorld is intentionally not allowed to mutate arbitrary game state. Movement, combat, damage, death, pathfinding and other engine behavior stay owned by TrinityCore.

### Safety and threading invariants

- No live `Creature*`, `Player*`, `Map*` or `Unit*` crosses the async inference boundary.
- Async/network workers operate only on value DTOs.
- Inference responses are consumed and validated on the world thread.
- AIWorld never force-loads grids just to simulate an agent.
- A loaded agent may be backed by a live `Creature`; an unloaded agent may continue to exist only as persistent/abstract state.
- Remote decisions are treated as untrusted intent, never as permission to mutate the world.
- Deterministic behavior is preferred where it is sufficient; LLM/GPU inference is a later layer, not the foundation.

---

## Current project status

### Infrastructure — runtime verified

The development environment is Docker-based and has been exercised on a real Ubuntu/NVIDIA host:

- TrinityCore `3.3.5` builds in a development container.
- Incremental builds use persistent `/build` and `ccache` volumes.
- MySQL, `authserver`, `worldserver` and `ai-server` run through Docker Compose.
- TDB world data can be imported through the versioned Make workflow.
- `dbc`, `maps`, `vmaps` and `mmaps` are mounted from host runtime storage and are not committed.
- LAN client login to the realm has been runtime verified.
- NVIDIA Container Toolkit is wired into the development stack; GPU inference itself is not yet the active decision engine.

### Persistent agent foundation

Implemented and runtime exercised:

- `AIWorldMgr` lifecycle integrated into `worldserver`.
- Stable `AgentId` and `AgentRegistry`.
- Materialized ↔ abstract agent binding without forcing grids to load.
- Persistent agent identity in the `characters` database.
- Long-term memory persistence and reload after `worldserver` restart.

### Events, perception and memory

Implemented:

- `WorldEvent` + internal event bus.
- TrinityCore event producers.
- Nearby player/creature perception.
- Witnessed world-event perception.
- Short-term memory with dedupe, TTL and expiry.
- Deterministic importance scoring.
- Persistent long-term memories.
- Deterministic relevant-memory retrieval with Top-N limiting.
- Decision-safe memory DTOs containing only selected semantic data.

### Needs and goals

The current deterministic behavior stack supports:

- `HealthPressure`
- `Hunger`
- `Fatigue`
- `SafetyPressure`
- `ResourcePressure`

Safety can be derived from both live combat state and recent dangerous memories.

Implemented goals:

- `GET_FOOD`
- `FLEE_DANGER`

Goal selection includes deterministic utility ranking, emergency priority, retention, interruption, success and timeout handling.

### Safe Action API

Currently runtime-verified action primitives:

- `FLEE`
- `MOVE_TO`
- `EAT`

The `GET_FOOD` loop is fully connected:

```text
Hunger high
    ↓
GET_FOOD activated
    ↓
Food target resolved
    ↓
MOVE_TO validated
    ↓
TrinityCore MovePoint/pathfinding
    ↓
arrival verified
    ↓
EAT validated
    ↓
Hunger satisfied
    ↓
GET_FOOD succeeded
```

`FLEE_DANGER` uses the same Action API boundary and executes through TrinityCore `MotionMaster::MoveFleeing()` rather than AIWorld inventing its own movement system.

### Async decision protocol — 2.9A through 2.9E

The project now has a versioned async decision path:

```text
AgentContext
    ↓
DecisionRequest V2
    ↓ async HTTP
ai-server
    ↓
DecisionResponse V2
    ↓
world-thread correlation + stale/provenance checks
    ↓
authoritative ActionRequest translation
    ↓
ActionSystem::Validate
```

The current `ai-server` policy is deliberately deterministic. It can propose `FLEE` when the current request context already contains an active `FLEE_DANGER` goal; otherwise it returns `NONE`.

Implemented safeguards include:

- protocol version validation,
- `request_id`, `agent_id` and snapshot correlation,
- request-time goal-attempt provenance,
- request-time runtime `Creature` incarnation provenance,
- stale response rejection,
- unsupported intent rejection,
- fresh world-thread `ActionSystem` validation.

**Remote decision execution is still intentionally dry-run.** The response path validates the proposed FLEE intent but does not call `ActionExecutor`, because the deterministic Goal → Action pipeline currently owns real FLEE execution. This prevents two independent owners from racing the same movement.

### Decision observability

Decision instrumentation uses TrinityCore's existing metrics system and emits low-cardinality series for:

- submissions / skipped-in-flight,
- queue time,
- request latency,
- transport outcomes,
- stale/discard reasons,
- validation results,
- unified valid/invalid decision rate.

The instrumented runtime path is exercised, but the external Influx/metrics backend has not yet received a dedicated runtime verification pass.

### Multi-agent submit API

`AIClient` exposes a batch-shaped caller API:

```text
vector<AIRequest>
    ↓
SubmitDecisions(...)
    ↓
vector<DecisionSubmitResult>
```

This is an API seam for the upcoming scheduler. It is **not** HTTP batching yet: each item still delegates to the existing `/decision` request path and the current global `DecisionInFlight` guard remains in place.

---

## What is not implemented yet

The project is still an active prototype. Major open areas include:

- multi-agent scheduler and simulation tiers,
- bounded inference admission/backpressure,
- actual GPU/LLM decision inference,
- safe transfer of execution ownership from deterministic behavior to selected remote decisions,
- richer action catalog (`WORK`, `REST`, `REQUEST_HELP`, trade/economy, etc.),
- relationships and social state,
- persistent active goals,
- farmer daily-life simulation,
- abstract wolf-pack simulation,
- the final emergent world-event loop described in the roadmap.

The current C++ V2 response parser is also a deliberately small hand-written parser for the controlled deterministic service. It should be replaced or hardened before a real external/LLM provider becomes an execution-relevant trust boundary.

---

## Repository layout

The repository is a real fork of TrinityCore and keeps upstream history.

```text
WoWBehaviorAI/
├── src/
│   └── server/game/AIWorld/       # persistent AI world subsystem
├── sql/                           # TrinityCore + project DB updates
├── docker/
│   ├── trinitycore/               # development build image
│   ├── ai/                        # FastAPI decision service
│   └── scripts/
├── deploy/                        # versioned server configuration
├── runtime/                       # local runtime state, gitignored
│   ├── data/                      # dbc/maps/vmaps/mmaps
│   └── logs/
├── compose.yml
├── compose.dev.yml
├── .env.example
├── Makefile
├── README_DEV.md                  # detailed environment/bootstrap notes
└── AI_TrinityCore_Roadmap_Etapa_1_2.md
```

Upstream base:

```text
TrinityCore/TrinityCore
branch: 3.3.5
```

The active project branch is `ai-world`.

---

## Requirements

Host requirements:

- Linux development host (the current environment is Ubuntu)
- Git
- Docker Engine
- Docker Compose plugin
- extracted World of Warcraft 3.3.5a `dbc/maps/vmaps/mmaps` data
- NVIDIA driver + NVIDIA Container Toolkit only if using the GPU development checks / future inference workload

No game client data, DB runtime volumes, secrets or AI model files belong in Git.

---

## Quick start

Clone the repository and use the `ai-world` branch:

```bash
git clone https://github.com/LoubekJan/WoWBehaviorAI.git
cd WoWBehaviorAI
git checkout ai-world
```

Create the local environment and build the development images:

```bash
make bootstrap
```

`make bootstrap` creates `.env` from `.env.example` if it does not exist and creates the expected runtime directories.

Edit `.env` before first real startup. Important values include:

```text
TC_DB_USER
TC_DB_PASSWORD
WORLDSERVER_PORT
AUTHSERVER_PORT
WOW_DATA_DIR
REALM_NAME
REALM_ADDRESS
REALM_LOCAL_ADDRESS
REALM_LOCAL_SUBNET_MASK
REALM_PORT
```

For a client on another machine, `REALM_ADDRESS` must point at the server's reachable LAN address, not `127.0.0.1`.

### Add WoW 3.3.5a data

Place extracted client data under the path configured by `WOW_DATA_DIR` (default `./runtime/data`):

```text
runtime/data/dbc
runtime/data/maps
runtime/data/vmaps
runtime/data/mmaps
```

These files are intentionally gitignored.

### Import world content

Import a pinned TrinityCore Database release, for example:

```bash
make db-import-tdb TDB_VERSION=TDB335.25101 TDB_SHA256=<sha256>
```

### Build and start

```bash
make build
make start
```

Configure the realm row from `.env`:

```bash
make configure-realm
```

Then follow the worldserver log:

```bash
make world-logs
```

---

## Day-to-day development

Typical C++ workflow:

```bash
git checkout ai-world
git pull --ff-only origin ai-world
make build
make restart-world
make world-logs
```

Useful Make targets:

| Command | Purpose |
|---|---|
| `make bootstrap` | create `.env`, runtime dirs and build dev images |
| `make build` | incremental configure/build/install into persistent `/build` |
| `make rebuild` | clean-first rebuild without deleting the build volume |
| `make start` | start MySQL, authserver, worldserver and ai-server |
| `make stop` | stop the Compose stack |
| `make restart-world` | restart only worldserver |
| `make logs` | follow all service logs |
| `make world-logs` | follow worldserver logs |
| `make shell` | interactive development container |
| `make db-shell` | MySQL shell as the application DB user |
| `make gpu-test` | run `nvidia-smi` through the GPU Compose profile |
| `make db-import-tdb` | import a pinned TDB world database |
| `make configure-realm` | update `auth.realmlist` from `.env` |

`make clean-build` and `make reset-db` are deliberately destructive operations. They should only be used when a clean build or database reset is explicitly intended.

---

## AIWorld configuration

AIWorld settings live in the versioned `deploy/worldserver.conf` under the **AI WORLD SETTINGS** section.

The subsystem can be disabled entirely:

```ini
AIWorld.Enable = 0
```

When disabled, the custom AIWorld subsystem does not run and normal TrinityCore behavior remains available.

The development configuration also contains cadence, test-agent, memory, needs and test-target settings used by the current milestones. Treat these as development controls rather than a final production configuration surface.

---

## AI service

`docker/ai/app/main.py` is currently a small FastAPI service used to exercise the async protocol and safety boundaries.

It provides:

```text
GET  /health
POST /decision
```

The service currently uses protocol V2 and a deterministic policy. It is intentionally not a real LLM implementation yet.

When Python `ai-server` code changes, rebuild that service rather than only restarting worldserver:

```bash
docker compose -f compose.yml -f compose.dev.yml up -d --build ai-server
make restart-world
make logs
```

---

## Persistence

Project-specific persistent state currently uses the TrinityCore `characters` database.

Implemented areas include:

```text
ai_agents
ai_long_term_memories
```

The system keeps the persistent `AgentId` separate from any particular runtime `Creature` instance. A spawn can unload/reload or be represented by a new runtime GUID while the logical agent continues to exist.

---

## Debugging and metrics

Primary development logging category:

```text
ai.world
```

Most detailed AIWorld lifecycle/action/decision traces are DEBUG-level logs, so a normal worldserver console may not show every validation line unless that category is configured accordingly.

Decision metrics use TrinityCore `Metric.h`. TrinityCore metrics are disabled by default and require `Metric.Enable` plus a valid `Metric.ConnectionInfo` to send data to the configured InfluxDB-compatible backend.

---

## Roadmap

The authoritative project status and milestone history live in:

[AI_TrinityCore_Roadmap_Etapa_1_2.md](AI_TrinityCore_Roadmap_Etapa_1_2.md)

Current handoff:

```text
2.8  Safe Action API                     DONE
2.9A Full AgentContext protocol          DONE
2.9B Structured deterministic response  DONE
2.9C World-thread validation seam        DONE
2.9D Decision metrics                    DONE
2.9E Multi-agent submit API              DONE
2.10A Scheduler/admission                NEXT
```

The next architecture step is to replace the current global single-decision bottleneck with scheduler-driven, bounded admission while preserving the existing world-thread, DTO and stale-provenance safety rules.

---

## Relationship to TrinityCore

WoWBehaviorAI is built as a fork of [TrinityCore](https://github.com/TrinityCore/TrinityCore) and retains its upstream history. TrinityCore remains the authoritative MMORPG server framework underneath the experimental AIWorld layer.

This repository is not a replacement distribution of World of Warcraft client data. You must supply your own compatible client data and follow the legal requirements applicable to your environment.

## License

The underlying TrinityCore project is licensed under **GPL-2.0**. See [COPYING](COPYING) and [AUTHORS](AUTHORS) for the inherited license and attribution information.
