# WoWBehaviorAI

Experimental **persistent AI world layer for TrinityCore 3.3.5**.

The project extends TrinityCore with long-lived individual agents that can perceive the world, remember events, develop needs and goals, propose actions, participate in deterministic routines, and coordinate through persistent social groups without bypassing TrinityCore's authoritative gameplay rules.

The goal is not to replace TrinityCore AI with a language model. Deterministic systems handle common behavior; external AI/inference is used only where it adds value.

> Active branch: **`AI-World-lab`**. Updated **2026-10-23**.
>
> The project now runs one authored world: realm **2**, map **725**, area
> **4988**, with **100 native NPCs** and live observer telemetry. The
> original Elwynn runtime is retired through the account-preserving
> migration; existing realm-2 characters and world data are retained.
>
> Root **`compose.yml`** is the complete stack: dedicated auth and world
> databases, login/game servers, observer, AI protocol service and account
> registration. Ports: login **3724**, world **9086**, observer **9091**,
> account registration **9088**.

Use [README_DEV.md](README_DEV.md) for setup and operation and
[doc/RealmLab.md](doc/RealmLab.md) for the current map, population,
account migration and deployment model. Autonomous raids, hunger and
territory capture remain future gameplay work; the active settlement
keeps normal TrinityCore NPC behavior.

The AI subsystem notes below preserve the project's implementation
history. [AIWorld_Current_Roadmap.md](AIWorld_Current_Roadmap.md) records
the earlier Elwynn research plan; current runtime instructions are the
authored-world guides linked above.

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

Core invariants:

- no live `Creature*`, `Player*`, `Map*` or `Unit*` crosses an async inference/persistence boundary;
- async/network workers receive only value DTOs;
- async responses are consumed and validated on the world thread;
- AIWorld never force-loads grids just to simulate an agent;
- loaded individual agent = materialized TrinityCore `Creature`; unloaded persistent agent = `AgentRecord` without a live pointer;
- `AgentWorldState` (world binding) is separate from `SimulationTier` (simulation policy);
- `AgentGroup` is a social/coordination entity over individual `AgentId`s, never a 1:1 replacement for a mob and never bound to a `Creature`;
- group intent coordinates members through per-member `ActionRequest`s that pass normal validation;
- remote decisions are untrusted intent, never permission to mutate world state;
- deterministic behavior is preferred where sufficient.

---

## AI subsystem implementation history

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
- deterministic Goal System including `GET_FOOD`, `FLEE_DANGER`, farmer routing and interruption semantics;
- safe Action API primitives/vertical slices `FLEE`, `MOVE_TO`, `EAT`, `WORK`, `REST`, `ATTACK`;
- full `GET_FOOD → MOVE_TO → ARRIVED → EAT → CONSUMED → NEED_SATISFIED` feedback loop;
- persistent farmer routine and economy state;
- bounded multi-agent decision scheduling and coarse simulation scheduling;
- separate persistent `AgentGroup` identity, membership and group coarse simulation;
- runtime group lifecycle `CreateGroup → JoinGroup → LeaveGroup → DissolveGroup` over existing individual agents.

### Async decision protocol — 2.9A through 2.9E

The versioned async decision path carries a full value-only `AgentContext` over Protocol V2 and returns structured `DecisionIntent`.

Safeguards include:

- request/agent/snapshot correlation;
- goal-attempt and RuntimeGuid provenance;
- stale response rejection;
- fail-closed unsupported intents;
- world-thread authoritative `ActionRequest` translation;
- `ActionSystem::Validate()` before any execution ownership could be transferred.

Remote `/decision` execution is still intentionally dry-run; the separate `/dynamic-task` path is live and produces server-validated player quests. The deterministic Goal→Action pipeline remains the real action owner until execution ownership is transferred explicitly and safely.

### Scheduler and simulation policy — 2.10 CLOSED

2.10A–2.10D are closed with static/runtime PASS.

Individual materialized decision tiers:

```text
NEARBY → ~1 s decision cadence
ACTIVE → ~5 s decision cadence
```

Individual non-materialized policy:

```text
BACKGROUND → default coarse interval 60 s
```

Decision admission is bounded by `AIWorld.DecisionMaxInFlight`. Capacity-skipped agents stay due rather than entering an unbounded request queue. Effective due-time ordering preserves fairness and allows live `ACTIVE ↔ NEARBY` cadence changes without stale deadlines.

Background coarse work is bounded by `AIWorld.CoarseSimulationMaxPerPass`, uses deterministic phase staggering, has no catch-up loop, and does not force-load grids.

The old `SimulationTier::ABSTRACT` concept was removed during the AgentGroup identity refactor. Groups now have their own `GroupId`-keyed coarse scheduler instead of pretending to be abstract physical agents.

### Persistent farmer — 2.11 CLOSED

The first persistent civilian vertical slice uses **Pa Maclure** in Elwynn.

Implemented and runtime verified:

- persistent Home/Work locations;
- deterministic synthetic-day routing `GO_TO_WORK` / `GO_HOME`;
- emergency interruption by higher-priority goals such as `FLEE_DANGER`;
- movement through the normal Action layer;
- transient `WORK` / `REST` activity;
- `ActionType::Work` / `ActionType::Rest` with ActionExecutor ownership;
- persistent economy state and work-window replay suppression.

The implementation intentionally separates routing goals from work/rest activity. Pa Maclure remains `AgentType::Civilian`; profession/routine is not a new physical agent type.

### AgentGroup / wolf-coalition foundation — 2.12

The original aggregate wolf-pack design was abandoned. A group does **not** merge several mobs into one pseudo-agent.

```text
Wolf A AgentId 101 ┐
Wolf B AgentId 102 ├── AgentGroup / Coalition #7
Wolf C AgentId 103 ┘
Wolf D AgentId 104     // independent
```

Each wolf keeps its own:

- `AgentId` and spawn identity;
- `Creature` materialization lifecycle;
- memory and needs;
- goals and decisions;
- validated actions.

The group owns social/coordination state only:

- independent `GroupId`;
- `AgentGroupKind { Loose, Stable }`;
- membership edges to individual `AgentId`s;
- territory/shared environmental resources;
- shared REGROUP/ROAM/HUNT intent; roles/leadership remain conditional on a demonstrated need.

Population is derived from membership count; it is not an aggregate mutable mob count.

#### 2.12D — separate group identity

Implemented:

- `AgentGroupRecord`, `AgentGroupRegistry`, `AgentGroupPersistence`;
- `ai_agent_groups` separate from `ai_agents`;
- `ai_agent_group_members` persistent membership;
- `AgentType::AgentGroup` removed;
- group `Creature` binding impossible by construction;
- `AgentGroupKind` persistence loads fail-closed;
- bounded `GroupCoarseSimulationScheduler` keyed by `GroupId`;
- group simulation continues whether members are loaded or unloaded;
- saturated no-op group resource ticks skip persistence/version churn.

Runtime presence evidence confirmed three individual wolves materializing as their own Creatures while the same group changed from `loadedMembers=0` to `loadedMembers=3`; group simulation continued and no grid was force-loaded.

#### 2.12E1 — group lifecycle CLOSED

Implemented lifecycle API:

```text
CreateGroup(...)
JoinGroup(GroupId, AgentId)
LeaveGroup(GroupId, AgentId)
DissolveGroup(GroupId)
```

Properties:

- `AgentGroupLifecycleSystem` owns lifecycle orchestration;
- join requires an existing group and an existing individual `AgentRecord`;
- duplicate membership is rejected;
- group lifecycle never spawns/despawns/moves a member Creature;
- no fake `SpawnId` or pseudo physical group agent is created;
- membership writes are confirmed before runtime registry mutation;
- dissolve removes membership rows + group row transactionally before registry removal;
- `GroupId` uses persistent monotonic `ai_agent_group_id_sequence`;
- allocator load is fail-closed and validated against physical `MAX(ai_agent_groups.group_id)`;
- sequence reservation is read-back-confirmed before group creation;
- dissolved `GroupId`s are not reused after restart.

Historical 2.12E1 runtime acceptance evidence (individual AgentIds precede the 2.12F4A2 spawn-aligned migration):

```text
sequence = 2
Create GroupId 2
Join AgentId 1,2,3
Leave AgentId 3
Dissolve GroupId 2
PASS

DB after dissolve:
GroupId 2 gone
membership rows for GroupId 2 gone
sequence = 3

restart
Create GroupId 3
Join/Leave/Dissolve
PASS
```

This proves persistent GroupId provenance and non-reuse while the individual wolves remain ordinary agents.

---

## Earlier research gate

Etapa 2 is **CLOSED / POC COMPLETE**. Its later gates added async-safe group lifecycle, Loose/Stable policy, generic automatic formation/maintenance, Elwynn population activation (`3540 / 3540 AIWorldControlled`), a second real coalition profile, REGROUP/ROAM/HUNT, and a local LLM dynamic quest lifecycle with outcome feedback. The final 2.14 gate confirmed real coalition HUNT through per-member ActionSystem validation and TrinityCore movement/combat.

The earlier **Etapa 3** plan focused on Elwynn scope and a complete census. Development now proceeds on the authored map described in [RealmLab.md](doc/RealmLab.md); the prior plan remains historical context for the reusable AI subsystem.

## What remains beyond the POC

- Authored-world settlement semantics, faction activity, economy and territory behavior, integrated in independently verifiable steps.
- World-scale indexing/bounded-work hardening and global rollout (2.12F4C/F4D), required before expanding beyond one location.
- Roles/leadership (2.12G4) only when actual behavior demonstrates a need.
- Richer persistent relationships, active goals and explicitly designed persistence for dynamic quests; current dynamic quests survive reconnect but are discarded on worldserver restart.
- Safe transfer of NPC action ownership from deterministic behavior to selected remote `/decision` results; its parser/privacy hardening remains separate from the validated `/dynamic-task` path.
- `PROTECT_HOME`/`REQUEST_HELP`, richer economy and the original wolves/farmer story remain future world-behavior examples, not prerequisites for the already closed aggregate POC.

Detailed open items and their conditions are maintained in the [roadmap backlog](AIWorld_Current_Roadmap.md#další-postup-a-otevřený-hardening).

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
└── AIWorld_Current_Roadmap.md
```

The repository is a real fork of TrinityCore and keeps upstream history. Active development happens on `AI-World-lab`.

---

## Setup and development

Follow the complete [authored-world setup guide](README_DEV.md). A fresh
installation supplies private `.env` credentials, imports pinned TDB and
installs the matching map/client data before preparing the realm:

```bash
git checkout AI-World-lab
make bootstrap
make build
make test
# Complete the one-time auth, TDB, data and world setup in README_DEV.md.
make start
```

Existing deployments preserve accounts and realm-2 data through the
migration described in [RealmLab.md](doc/RealmLab.md); they do not repeat
the TDB import. Routine source changes use `make build`,
`make restart-world` and `make world-logs`. `make backup` writes protected
auth and world/character backups under `runtime/lab/backups`.

---

## AIWorld configuration

AIWorld settings are versioned in `deploy/worldserver.conf` and rendered by the selected realm profile. The active `settlement-v8` profile keeps AI management disabled and exports native NPC telemetry. The following scheduler/group examples describe the earlier AI research profiles:

```ini
AIWorld.Enable = 1
AIWorld.DecisionSchedulerIntervalMs = 250
AIWorld.DecisionNearbyIntervalMs = 1000
AIWorld.DecisionActiveIntervalMs = 5000
AIWorld.DecisionNearbyPlayerRange = 60.0
AIWorld.DecisionMaxInFlight = 4

AIWorld.BackgroundSimulationIntervalMs = 60000
AIWorld.CoarseSimulationMaxPerPass = 50

AIWorld.GroupSimulationIntervalMs = 300000
AIWorld.GroupSimulationMaxPerPass = 50
AIWorld.AgentGroupResourcesRatePerSecond = 0.0005
```

The old `AIWorld.AbstractSimulationIntervalMs` setting was replaced by `AIWorld.GroupSimulationIntervalMs` when AgentGroup stopped being represented as an abstract individual agent.

The 2.12E1 manual lifecycle smoke test is disabled by default:

```ini
AIWorld.TestGroupMemberAgentId1 = 0
AIWorld.TestGroupMemberAgentId2 = 0
AIWorld.TestGroupMemberAgentId3 = 0
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
grep -E "agent group presence|agent group simulation" runtime/logs/Server.log
grep -E "group lifecycle smoke test|agent group created|agent group join|agent group leave|agent group dissolved" runtime/logs/Server.log
```

`make world-logs` follows container stdout/stderr, whose console threshold may hide DEBUG lines even when the `ai.world` logger is at DEBUG level.

Decision metrics use TrinityCore's existing `Metric.h` infrastructure. Dedicated Influx/backend runtime verification remains a hardening item.

---

## Roadmap handoff

Use [doc/RealmLab.md](doc/RealmLab.md) for the current authored-world direction and acceptance boundaries. [AIWorld_Current_Roadmap.md](AIWorld_Current_Roadmap.md) retains the earlier infrastructure/AI POC history and research backlog.

See [README_DEV.md](README_DEV.md) for environment details and the standard build/test workflow.

## License

The underlying TrinityCore project is licensed under **GPL-2.0**. See [COPYING](COPYING) and [AUTHORS](AUTHORS).
