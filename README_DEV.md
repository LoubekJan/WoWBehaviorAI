# AI TrinityCore — Development Setup

Development guide for the WoWBehaviorAI fork of TrinityCore `3.3.5`.

For architecture and milestone history see [AI_TrinityCore_Roadmap_Etapa_1_2.md](AI_TrinityCore_Roadmap_Etapa_1_2.md).

## Current status

The development stack has been exercised on a real Ubuntu/NVIDIA host. The old README status that said Docker/GPU runtime was untested is no longer true.

Runtime-verified infrastructure includes:

- TrinityCore build/install in Docker with persistent `/build` and `ccache` volumes;
- MySQL bootstrap and TDB import;
- `authserver`, `worldserver`, MySQL and `ai-server` through Compose;
- mounted `dbc/maps/vmaps/mmaps` game data;
- LAN WoW 3.3.5a client login;
- versioned realm configuration via `make configure-realm`;
- worldserver restart without losing DB state;
- NVIDIA Container Toolkit visibility from Docker.

AIWorld development has progressed through the scheduler/tier foundation. 2.10A–2.10C are closed; 2.10D implementation/static review and deterministic phase staggering are verified, with one final steady-state coarse cadence sample still pending before formal 2.10 closure.

## Origin

- Base: `TrinityCore/TrinityCore`, branch `3.3.5`.
- Upstream starting commit: `2a64b72689cc8d797e4c93a0c96dfa2dc06f64c8`.
- `origin`: `https://github.com/LoubekJan/WoWBehaviorAI.git`.
- `upstream`: `https://github.com/TrinityCore/TrinityCore.git`.
- Development branch: `ai-world`.

The repository keeps TrinityCore history, so normal upstream merges remain possible.

## Prerequisites

Host requirements:

- Git
- Docker Engine
- Docker Compose plugin
- NVIDIA driver + NVIDIA Container Toolkit for GPU checks/future inference
- legally obtained WoW 3.3.5a client data for `dbc/maps/vmaps/mmaps`

## First-time setup

```bash
make bootstrap
```

This creates `.env` from `.env.example` when needed, prepares runtime directories and builds the development image.

Edit `.env` before first real startup. DB credentials and realm/network settings should come from `.env`, not hand-edited deployment files.

## WoW game data

Place extracted client data under:

```text
runtime/data/dbc
runtime/data/maps
runtime/data/vmaps
runtime/data/mmaps
```

`runtime/` is host-only, gitignored state and is mounted into `worldserver`.

A precise step-by-step extraction document is still a hardening backlog item.

## Database bootstrap

`deploy/mysql/01-init-users.sh` creates the TrinityCore databases and application DB user on first MySQL start. TrinityCore's own updater applies schema updates from the mounted `sql/` tree.

Import a pinned TDB world dataset before first useful worldserver startup:

```bash
make db-import-tdb TDB_VERSION=TDB335.25101 TDB_SHA256=<sha256>
```

The import helper waits for MySQL and verifies the checksum when provided.

### Realm address

Set the realm values in `.env`, then run:

```bash
make configure-realm
```

For a client on another machine, `REALM_ADDRESS` must be a reachable server address, not `127.0.0.1`.

## Day-to-day workflow

Typical C++ workflow:

```bash
git checkout ai-world
git pull --ff-only origin ai-world
git log -1 --oneline
make build
make restart-world
make world-logs
```

Useful targets:

| Command | Purpose |
|---|---|
| `make bootstrap` | prepare `.env`, runtime dirs and dev image |
| `make build` | incremental configure/build/install |
| `make rebuild` | clean-first rebuild without deleting DB state |
| `make start` | start the Compose stack |
| `make stop` | stop the stack |
| `make restart-world` | restart only worldserver |
| `make logs` | follow all services |
| `make world-logs` | follow worldserver container stdout/stderr |
| `make shell` | development container shell |
| `make db-shell` | MySQL shell as application user |
| `make gpu-test` | run `nvidia-smi` through the GPU profile |
| `make db-import-tdb` | import pinned TDB |
| `make configure-realm` | update `auth.realmlist` from `.env` |

`make clean-build` and `make reset-db` are intentionally destructive. Use them only when a clean build or DB reset is explicitly intended.

## Python ai-server workflow

When Python decision-service code changes, rebuild that service:

```bash
docker compose -f compose.yml -f compose.dev.yml up -d --build ai-server
make restart-world
make logs
```

There is no separate `make restart-ai` workflow.

## AIWorld configuration

AIWorld settings are versioned in `deploy/worldserver.conf` and mirrored in `src/server/worldserver/worldserver.conf.dist`.

The subsystem can be disabled:

```ini
AIWorld.Enable = 0
```

Current scheduler controls include:

```ini
AIWorld.DecisionSchedulerIntervalMs = 250
AIWorld.DecisionNearbyIntervalMs = 1000
AIWorld.DecisionActiveIntervalMs = 5000
AIWorld.DecisionNearbyPlayerRange = 60.0
AIWorld.DecisionMaxInFlight = 4
AIWorld.BackgroundSimulationIntervalMs = 60000
AIWorld.AbstractSimulationIntervalMs = 300000
AIWorld.CoarseSimulationMaxPerPass = 50
```

Semantics:

```text
NEARBY     → materialized, player near, ~1 s decision cadence
ACTIVE     → materialized, no nearby player, ~5 s decision cadence
BACKGROUND → unloaded ordinary agent, coarse scheduler seam
ABSTRACT   → future aggregate/group agent, coarse scheduler seam
```

`BACKGROUND/ABSTRACT` never send `/decision`, execute actions or force-load grids in the current 2.10D scope.

## Logging

Primary categories used by the current AIWorld implementation include:

```text
ai.world
ai.agent
```

`make world-logs` follows the worldserver container console. Detailed AIWorld DEBUG records are normally written to the file appender at:

```text
runtime/logs/Server.log
```

Useful runtime checks:

```bash
grep "AI simulation tier" runtime/logs/Server.log
grep "AI simulation tick" runtime/logs/Server.log
```

The console appender may filter DEBUG messages even when the `ai.world` logger itself is configured at DEBUG level, so `Server.log` is the preferred source for detailed tier/scheduler evidence.

## Current scheduler runtime handoff

2.10D deterministic entry staggering is runtime verified. For the three current test guards the first Background ticks were observed at different phases:

```text
agent=3 tier=BACKGROUND dt=14253ms
agent=2 tier=BACKGROUND dt=21504ms
agent=1 tier=BACKGROUND dt=46762ms
```

The remaining final gate is to observe the next tick for each agent with approximately:

```text
dt≈60000ms
```

After that, 2.10 can be marked CLOSED/PASS and development moves to 2.11 persistent farmer.

## Debugging and metrics hardening

Still open:

- exact game-data extraction walkthrough;
- convenient Debug build target;
- measured no-op/incremental rebuild smoke;
- `gdb` attach/breakpoint workflow;
- core dump/crash artifact workflow;
- CUDA/PyTorch compute smoke;
- Influx/metrics backend runtime evidence;
- scaling improvements for the fast registry/materialization scan and full-sort coarse due selection.

## Persistence

Project-specific persistent state currently uses the `characters` database.

Implemented tables include:

```text
ai_agents
ai_long_term_memories
```

Persistent `AgentId` is separate from runtime `ObjectGuid`. A spawn can unload/reload or produce a new runtime Creature incarnation while the logical agent persists.

## Safety invariants for development

When changing AIWorld code, preserve these rules:

- no live TrinityCore pointer across async boundaries;
- no blocking network/DB work on the world thread;
- no force-loading grids to simulate agents;
- async responses are consumed on the world thread;
- `RuntimeGuid` provenance must protect runtime Creature incarnation changes;
- AI proposes, `ActionSystem` validates, TrinityCore executes;
- do not create duplicate execution ownership between deterministic and remote decision paths;
- prefer deterministic logic and use LLM/inference only when it adds real value.
