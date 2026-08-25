# WoWBehaviorAI

Experimental **persistent AI world layer for TrinityCore 3.3.5**.

The project extends TrinityCore with long-lived agents that can perceive the world, remember events, develop needs and goals, propose actions, and eventually participate in a larger simulated world without bypassing TrinityCore's authoritative gameplay rules.

The long-term goal is not to replace TrinityCore AI with a language model. Deterministic systems handle common behavior; external AI/inference is used only where it adds value.

> Development branch: `ai-world`
>
> Current milestone: **Etapa 2 / 2.10D final runtime closure**
>
> Next after closure: **2.11 — persistent farmer**

For detailed milestone history see [AI_TrinityCore_Roadmap_Etapa_1_2.md](AI_TrinityCore_Roadmap_Etapa_1_2.md).

---

## Core design rule

**AI proposes. `ActionSystem` validates. TrinityCore executes.**

```text
WORLD STATE
    ↓
EVENT
    ↓
PERCEPTION
    ↓
MEMORY
    ↓
NEEDS
    ↓
GOALS
    ↓
DECISION / PLANNING
    ↓
ActionRequest
    ↓
ActionSystem::Validate
    ↓
TrinityCore gameplay execution
    └──────────────► WORLD STATE
```

Safety/threading invariants:

- no live `Creature*`, `Player*`, `Map*` or `Unit*` crosses the async inference boundary;
- async/network workers receive only value DTOs;
- responses are consumed and validated on the world thread;
- AIWorld never force-loads grids just to simulate an agent;
- `AgentWorldState` (materialized/abstract binding) is separate from `SimulationTier` (`NEARBY`/`ACTIVE`/`BACKGROUND`/`ABSTRACT` policy);
- remote decisions are untrusted intent, never permission to mutate world state;
- deterministic behavior is preferred where sufficient.

---

## Current project status

### Infrastructure — runtime verified

The Docker development environment has been exercised on a real Ubuntu/NVIDIA host:

- TrinityCore `3.3.5` builds in a development container;
- persistent `/build` and `ccache` volumes support incremental work;
- MySQL, `authserver`, `worldserver` and `ai-server` run through Compose;
- pinned TDB import works through the Make workflow;
- `dbc/maps/vmaps/mmaps` are mounted from host runtime storage;
- LAN client login is runtime verified;
- NVIDIA Container Toolkit is wired into the stack.

### AIWorld foundation

Implemented and runtime exercised:

- `AIWorldMgr` lifecycle and disable switch;
- stable `AgentId` + `AgentRegistry`;
- materialized ↔ abstract binding without force-loading grids;
- persistent agent identity and long-term memory;
- world events, perception and short/long-term memory;
- deterministic relevant-memory Top-N retrieval;
- Needs System with health/hunger/fatigue/safety/resource pressure;
- deterministic Goal System for `GET_FOOD` and `FLEE_DANGER`;
- safe Action API primitives `FLEE`, `MOVE_TO`, `EAT`;
- full `GET_FOOD → MOVE_TO → ARRIVED → EAT → CONSUMED → NEED_SATISFIED` feedback loop.

### Async decision protocol — 2.9A through 2.9E

The versioned async decision path carries a full value-only `AgentContext` over Protocol V2 and returns structured `DecisionIntent`.

Safeguards include:

- request/agent/snapshot correlation;
- goal-attempt and RuntimeGuid provenance;
- stale response rejection;
- fail-closed unsupported intents;
- world-thread authoritative `ActionRequest` translation;
- `ActionSystem::Validate()` before any execution ownership could be transferred.

Remote decision execution is still intentionally dry-run. The deterministic Goal→Action pipeline owns real FLEE execution so there are not two movement owners racing each other.

### Scheduler and simulation tiers — 2.10

2.10A–2.10C are closed. 2.10D implementation has static PASS and runtime-verified deterministic phase staggering; one final steady-state cadence sample remains before formal closure.

Materialized decision tiers:

```text
NEARBY → ~1 s decision cadence
ACTIVE → ~5 s decision cadence
```

Decision admission is bounded by `AIWorld.DecisionMaxInFlight`. Capacity-skipped agents stay due rather than entering an unbounded request queue. Effective due-time ordering preserves fairness and allows live `ACTIVE ↔ NEARBY` cadence changes without stale deadlines.

Non-materialized simulation tiers:

```text
BACKGROUND → default coarse interval 60 s
ABSTRACT   → default coarse interval 5 min
```

The current coarse tick is **observability/scheduling only**. It does not call `/decision`, `ActionExecutor`, mutate Needs/goals/memory or force-load grids.

`AIWorld.CoarseSimulationMaxPerPass` bounds coarse work per scheduler pass. A per-agent authoritative `NextTickAtMs` plus deterministic `StableAgentHash(AgentId)` entry phase avoids phase-locking agents that enter Background together.

Runtime first-phase evidence for the three test guards:

```text
agent=3 tier=BACKGROUND dt=14253ms
agent=2 tier=BACKGROUND dt=21504ms
agent=1 tier=BACKGROUND dt=46762ms
```

The final 2.10D gate is to observe each agent's next Background tick at approximately `dt=60000ms`. After that, the project moves to **2.11 persistent farmer**.

---

## What is not implemented yet

Major open areas include:

- actual gameplay/state simulation on the Background/Abstract coarse seam;
- persistent farmer daily-life behavior (`WORK`, home/working locations, rest/resources);
- actual GPU/LLM decision inference;
- safe transfer of execution ownership from deterministic behavior to selected remote decisions;
- richer actions such as `WORK`, `REST`, `REQUEST_HELP`, trade/economy;
- relationships/social state and persistent active goals;
- a real aggregate `CREATURE_GROUP` wolf-pack agent;
- the emergent wolf-pack → farm → memory → protect/request-help loop.

Before a real external/LLM provider becomes execution-relevant, the hand-written C++ V2 response parser and external DecisionContext privacy boundary should be hardened.

Known non-blocking scheduler scaling work: the fast scheduler still probes the registered population for live materialization, and coarse selection sorts the whole due set before taking the bounded admitted prefix.

---

## Repository layout

```text
WoWBehaviorAI/
├── src/server/game/AIWorld/       # persistent AI world subsystem
├── sql/                           # TrinityCore + project DB updates
├── docker/                        # development/inference images and scripts
├── deploy/                        # versioned runtime configuration
├── runtime/                       # host-only runtime state, gitignored
├── compose.yml
├── compose.dev.yml
├── .env.example
├── Makefile
├── README_DEV.md
└── AI_TrinityCore_Roadmap_Etapa_1_2.md
```

The repository is a real fork of TrinityCore and keeps upstream history. Active development happens on `ai-world`.

---

## Quick start

```bash
git clone https://github.com/LoubekJan/WoWBehaviorAI.git
cd WoWBehaviorAI
git checkout ai-world
make bootstrap
```

Place extracted WoW 3.3.5a data under the configured `WOW_DATA_DIR` (default `runtime/data`):

```text
runtime/data/dbc
runtime/data/maps
runtime/data/vmaps
runtime/data/mmaps
```

Import world content and start:

```bash
make db-import-tdb TDB_VERSION=TDB335.25101 TDB_SHA256=<sha256>
make build
make start
make configure-realm
make world-logs
```

For a LAN client, configure a reachable `REALM_ADDRESS` in `.env`, not `127.0.0.1`.

---

## Day-to-day development

Typical C++ workflow:

```bash
git checkout ai-world
git pull --ff-only origin ai-world
git log -1 --oneline
make build
make restart-world
make world-logs
```

When Python `ai-server` code changes:

```bash
docker compose -f compose.yml -f compose.dev.yml up -d --build ai-server
make restart-world
make logs
```

`make clean-build` and `make reset-db` are destructive operations and should be used only deliberately.

---

## AIWorld configuration

AIWorld settings are versioned in `deploy/worldserver.conf`.

```ini
AIWorld.Enable = 1
AIWorld.DecisionSchedulerIntervalMs = 250
AIWorld.DecisionNearbyIntervalMs = 1000
AIWorld.DecisionActiveIntervalMs = 5000
AIWorld.DecisionNearbyPlayerRange = 60.0
AIWorld.DecisionMaxInFlight = 4
AIWorld.BackgroundSimulationIntervalMs = 60000
AIWorld.AbstractSimulationIntervalMs = 300000
AIWorld.CoarseSimulationMaxPerPass = 50
```

Set `AIWorld.Enable = 0` to disable the custom subsystem.

---

## Debugging and metrics

Detailed AIWorld DEBUG traces are written to the file appender under:

```text
runtime/logs/Server.log
```

Useful checks:

```bash
grep "AI simulation tier" runtime/logs/Server.log
grep "AI simulation tick" runtime/logs/Server.log
```

`make world-logs` follows container stdout/stderr, whose console threshold may hide DEBUG lines even when the `ai.world` logger is at DEBUG level.

Decision metrics use TrinityCore's existing `Metric.h` infrastructure. Dedicated Influx/backend runtime verification remains a hardening item.

---

## Roadmap handoff

```text
2.8   Safe Action API                         DONE
2.9A  Full AgentContext protocol              DONE
2.9B  Structured deterministic response      DONE
2.9C  World-thread validation seam            DONE
2.9D  Decision metrics                        DONE
2.9E  Multi-agent submit API                  DONE
2.10A Bounded multi-agent admission           DONE
2.10B Proximity-aware decision cadence        DONE
2.10C Explicit simulation tier transitions    DONE
2.10D Coarse Background/Abstract scheduling   FINAL RUNTIME GATE
2.11  Persistent farmer                       NEXT
```

See [README_DEV.md](README_DEV.md) for environment details and [AI_TrinityCore_Roadmap_Etapa_1_2.md](AI_TrinityCore_Roadmap_Etapa_1_2.md) for milestone history.

## License

The underlying TrinityCore project is licensed under **GPL-2.0**. See [COPYING](COPYING) and [AUTHORS](AUTHORS).
