# AI TrinityCore — Roadmap

> **Výchozí stav:** TrinityCore `3.3.5` + Ubuntu Server + NVIDIA GPU  
> **Rozsah dokumentu:** Etapy 1–3 + výhled Etapy 4  
> **Aktualizováno:** 2026-08-26  
> **Aktivní větev:** `ai-world`

## Stav projektu

**Etapa 1 má splněný runtime gate. Etapa 2 je aktivně rozpracovaná. 2.10 Scheduler/tier foundation je CLOSED/PASS, 2.11 persistentní farmář je CLOSED/PASS a 2.12 dosáhla přes oddělenou group identity až k runtime-ověřenému `AgentGroup` lifecycle `CreateGroup / JoinGroup / LeaveGroup / DissolveGroup` (2.12E1 CLOSED/PASS).**

Na reálném Ubuntu/GPU hostu bylo ověřeno:

- TrinityCore se v Dockeru nakonfiguruje, zkompiluje a nainstaluje do persistentního `/build` volume.
- MySQL se inicializuje a TDB `TDB335.25101` se stáhne/importuje automatizovaným workflow.
- `authserver`, `worldserver`, MySQL a `ai-server` běží přes Compose.
- extrahovaná `dbc/maps/vmaps/mmaps` data jsou připojena do `worldserver`.
- NVIDIA Container Toolkit funguje a Docker vidí dvě RTX 3090.
- WoW 3.3.5a klient na jiném PC v LAN projde authserverem, realm listem a připojí se na worldserver.
- `auth.realmlist` je konfigurován versionovaným `make configure-realm`, nikoli ručním SQL zásahem.
- restart `worldserver` přes `make restart-world` zachová DB/herní stav.

Etapa 2 má runtime ověřený foundation řetězec od persistentní identity přes events/perception/memory/Needs/Goals až po bezpečné Action API, async decision pipeline, scheduler a první dva persistentní vertical slices. Implementovaný stav zahrnuje:

- `AIWorldMgr` lifecycle + read-only snapshot.
- async `AIClient` `/health` + verzovaný `/decision` V2, timeout/fallback, korelace a stale-response ochrana.
- `AgentRegistry` + stabilní `AgentId` + materialized/abstract world binding.
- persistence identity agenta přes `characters` DB.
- `WorldEvent` + `EventBus` + TrinityCore producer.
- witnessed-event, nearby-player a nearby-creature perception.
- `ShortTermMemory` s dedupe/TTL/expiry.
- deterministic importance + persistentní `LongTermMemory` + restart/load.
- deterministic relevant-memory retrieval s Top-N omezením a wire-safe `DecisionMemory` DTO.
- per-agent `NeedsState`: health/hunger/fatigue/safety/resource pressure, live-state coupling, recent-memory safety a threshold events.
- deterministic Goal System pro `GET_FOOD` a `FLEE_DANGER`, včetně retention/interruption/success/timeout lifecycle.
- safe Action API pro `FLEE`, `MOVE_TO`, `EAT`, `WORK`, `REST`; TrinityCore zůstává jediný executor fyzického světa.
- decision protocol 2.9A–2.9E: full `AgentContext`, structured `DecisionIntent`, strict V2 korelace, stale/provenance guard, authoritative validation, metrics a multi-agent submit API.
- scheduler 2.10A–2.10D: bounded multi-agent decision admission, proximity-aware cadence, explicit simulation policy a bounded/staggered coarse scheduling.
- persistentní farmář Pa Maclure: Home/Work location, deterministic rutina, movement, WORK/REST activity/action a persistentní economy reward marker.
- `AgentGroup` social domain oddělený od `AgentRecord`: vlastní `GroupId`, registry, persistence, membership a coarse group simulation.
- runtime group lifecycle: create/join/leave/dissolve nad existujícími individuálními agenty bez fake SpawnId a bez 1:1 vazby group→Creature.
- persistentní monotónní `GroupId` high-water sequence s fail-closed validací proti fyzickému `MAX(group_id)`.

**Aktuální NEXT:** před automatickým vznikem/rozpadem LOOSE coalitions odstranit synchronní DB round-trip z runtime lifecycle cesty. Současné 2.12E1 CRUD je přijatelné pro startup/admin/manual test, ale nesmí být beze změny použito z per-tick/per-decision policy. Potom přidat `Loose/Stable` membership policy a teprve následně automatickou formation/dissolution logiku vlků.

Zbývající položky Etapy 1 jsou **hardening / developer tooling**, nikoli gate pro pokračování AI vrstvy.

---

## Obsah

- [Základní princip](#základní-princip)
- [Přehled etap](#přehled-etap)
- [Etapa 1 — Development Infrastructure](#etapa-1--development-infrastructure)
- [Etapa 2 — AI World Foundation](#etapa-2--ai-world-foundation)
  - [2.0 Cílová architektura](#20-cílová-architektura)
  - [2.1 Persistentní agent](#21-persistentní-agent)
  - [2.2 Persistence](#22-persistence)
  - [2.3 World Event System](#23-world-event-system)
  - [2.4 Perception System](#24-perception-system)
  - [2.5 Memory System](#25-memory-system)
  - [2.6 Needs System](#26-needs-system)
  - [2.7 Goal System](#27-goal-system)
  - [2.8 Bezpečné Action API](#28-bezpečné-action-api)
  - [2.9 AI server — decision protocol](#29-ai-server--decision-protocol)
  - [2.10 Scheduler a úrovně simulace](#210-scheduler-a-úrovně-simulace)
  - [2.11 První experiment — persistentní farmář](#211-první-experiment--persistentní-farmář)
  - [2.12 První experiment — wolf pack / AgentGroup coalition](#212-první-experiment--wolf-pack--agentgroup-coalition)
  - [2.13 LLM dynamic quest / player interaction vertical slice](#213-llm-dynamic-quest--player-interaction-vertical-slice)
  - [2.14 První emergentní end-to-end událost](#214-první-emergentní-end-to-end-událost)
  - [2.15 Testy a diagnostika](#215-testy-a-diagnostika)
- [Doporučené pořadí implementace](#doporučené-pořadí-implementace)
- [Etapa 3 — Elwynn Forest World Preparation](#etapa-3--elwynn-forest-world-preparation)
  - [3.0 Scope a source of truth](#30-scope-a-source-of-truth)
  - [3.1 Kompletní census NPC a creature spawnů](#31-kompletní-census-npc-a-creature-spawnů)
  - [3.2 Sémantická mapa lokací](#32-sémantická-mapa-lokací)
  - [3.3 Faction audit a oprava](#33-faction-audit-a-oprava)
  - [3.4 Coalition pravidla uvnitř frakcí](#34-coalition-pravidla-uvnitř-frakcí)
  - [3.5 Faction presence a pohyb po mapě](#35-faction-presence-a-pohyb-po-mapě)
  - [3.6 World DB cleanup a verifikace](#36-world-db-cleanup-a-verifikace)
  - [Etapa 3 — Definition of Done](#etapa-3--definition-of-done)
- [Etapa 4 — Living World](#etapa-4--living-world)

---

## Základní princip

**AI přemýšlí a navrhuje záměr. TrinityCore a simulační engine vždy rozhodují, co je fyzicky a pravidlově možné a jak se akce provede.**

```text
WORLD STATE
    ↓
EVENT
    ↓
PERCEPTION
    ↓
MEMORY
    ↓
NEED
    ↓
GOAL
    ↓
DECISION / PLANNING
    ↓
ActionRequest
    ↓
ActionSystem::Validate
    ↓
TrinityCore gameplay execution
    └──────────────→ WORLD STATE
```

AI nikdy nesmí přímo zapisovat libovolný stav do světa ani obcházet serverová pravidla.

Základní invariants:

- žádný live `Creature*`/`Player*`/`Map*`/`Unit*` nesmí uniknout přes async hranici;
- async inference/network pracuje pouze s value DTO;
- odpovědi se zpracovávají na world threadu;
- AIWorld nikdy force-loaduje grid kvůli simulaci;
- world binding a simulation policy jsou dva oddělené koncepty;
- načtený individuální agent je materialized `Creature`, nenačtený persistentní `AgentRecord`;
- `AgentGroup` je sociální/koordinační entita nad individuálními `AgentId`, nikdy není 1:1 bind na `Creature`;
- group intent se v budoucnu musí rozložit na per-member intent/`ActionRequest`; group nikdy přímo nepohybuje více Creatures;
- remote AI pouze navrhuje, `ActionSystem` validuje a TrinityCore provádí;
- drahé inference/LLM se používá jen tam, kde deterministic logika nestačí.

## Přehled etap

| Etapa | Stav | Hlavní cíl | Gate pro pokračování |
|---|---|---|---|
| **1 — Development Infrastructure** | ✅ **GATE SPLNĚN** | Reprodukovatelný Docker development stack | build → DB/TDB → worldserver → vzdálený WoW klient → restart/persistence |
| **2 — AI World Foundation** | 🟡 **IN PROGRESS — 2.12E1 CLOSED, next async-safe dynamic group lifecycle** | Vyřešit a runtime ověřit technologické stavební bloky pro persistentní živý AI svět: agenti, události, paměť, cíle, Action API, async AI/LLM bridge, scheduler, coalition model a dynamickou interakci s hráčem | world problem → NPC context → local LLM → validated player task → player action → WORLD STATE |
| **3 — Elwynn Forest World Preparation** | ⚪ **PLANNED** | Připravit jednu konkrétní lokaci jako správně popsaný a opravený datový základ: všechny NPC/spawny, sémantické lokace, frakce, coalition constraints a faction presence | každý relevantní spawn je evidovaný; lokace a frakce jsou explicitní; coalition membership respektuje faction invariant; DB změny jsou verzované a runtime ověřené |
| **4 — Living World** | ⚪ **PLANNED** | Z připraveného Elwynn Forest a technologií Etapy 2 skládat komplexní persistentní svět: populace, zdroje, ekonomiku, vztahy, pohyb frakcí, konflikty a AI questy | dlouhodobě běžící oblast generuje kauzální problémy, interakce a změny bez ručně napsané questové posloupnosti |

---

# Etapa 1 — Development Infrastructure

**Stav: runtime gate SPLNĚN.**

Hotovo:

- Docker development image na Ubuntu 22.04, Ninja, `ccache`, `RelWithDebInfo`.
- persistentní `/build` + `/ccache`.
- Compose služby MySQL, authserver, worldserver, ai-server a GPU check.
- TDB import a TrinityCore DB updater.
- versionovaný realm bootstrap přes `make configure-realm`.
- runtime mount `dbc/maps/vmaps/mmaps`.
- LAN client login runtime PASS.
- NVIDIA Container Toolkit / `nvidia-smi` runtime PASS.
- `make build`, `make restart-world`, `make world-logs` a další základní dev workflow.

Neblokující hardening:

- [ ] přesný extraction návod `dbc/maps/vmaps/mmaps`;
- [ ] samostatný Debug target;
- [ ] měřený no-op/incremental rebuild smoke;
- [ ] `gdb` attach/breakpoint + core dump workflow;
- [ ] CUDA/PyTorch compute smoke v inference image;
- [ ] metrics backend/dashboard runtime evidence.

---

# Etapa 2 — AI World Foundation

## 2.0 Cílová architektura

```text
TrinityCore
│
├── Player / Creature / GameObject
│
└── AIWorldMgr
    ├── AgentRegistry
    ├── AgentGroupRegistry
    ├── AgentGroupLifecycleSystem
    ├── PerceptionSystem
    ├── MemorySystem
    ├── NeedsSystem
    ├── GoalSystem
    ├── ActionSystem
    ├── EventSystem
    ├── DecisionScheduler
    ├── CoarseSimulationScheduler
    ├── GroupCoarseSimulationScheduler
    └── AIClient  -------------------->  ai-server  ---> GPU
```

- [x] subsystem `src/server/game/AIWorld/`;
- [x] lifecycle integrace do `worldserver`;
- [x] AIWorld lze vypnout;
- [x] network/inference async vůči world update loopu;
- [x] response freshness/provenance validation;
- [x] group identity oddělená od physical Agent identity;
- [x] group coarse simulation má vlastní GroupId-keyed scheduler.

## 2.1 Persistentní agent

**Stav: core registry/binding DONE / runtime PASS**

- [x] stabilní `AgentId`;
- [x] `AgentRegistry`;
- [x] `AgentId ↔ Creature/ObjectGuid` binding;
- [x] unload → agent zůstává persistentní bez live `Creature`;
- [x] rematerializace bez ztráty identity/state;
- [x] `RuntimeGuid` provenance platí pouze pro aktuální materialized incarnation;
- [x] group/coalition už není `AgentType` a nevytváří fake physical agent.

`SpawnId` je persistentní spawn identity individuálního creature agenta. `RuntimeGuid` je platný pouze pro aktuální materialized runtime object. `GroupId` je samostatná sociální identity doména a nesmí být odvozována z `AgentId`, `SpawnId` ani `RuntimeGuid`.

## 2.2 Persistence

Aktuálně `characters` DB zahrnuje mimo jiné:

```text
ai_agents
ai_long_term_memories
ai_agent_groups
ai_agent_group_members
ai_agent_group_id_sequence
```

- [x] individual agent identity persistence;
- [x] long-term memory persistence + restart/load;
- [x] persistent Home/Work location;
- [x] persistent economy state + monotonic economy version;
- [x] oddělená group identity/state persistence;
- [x] persistent AgentId membership edges;
- [x] persistent monotonic GroupId high-water sequence;
- [ ] obecné relationships mimo group membership;
- [ ] active goals persistence;
- [ ] historical events/audit persistence.

## 2.3 World Event System

**Stav: první producer + EventBus DONE / runtime PASS**

- [x] `WorldEvent` s typem/časem/lokací/actor/target/payload;
- [x] interní `EventBus`;
- [x] TrinityCore producer hook;
- [x] correlation/cause id;
- [x] debug logging;
- [ ] oddělená persistentní historical event vrstva.

## 2.4 Perception System

**Stav: core sight perception DONE / runtime PASS**

- [x] nearby entity perception;
- [x] range + relevantní LoS checks;
- [x] witnessed event perception;
- [x] `Observation` DTO;
- [ ] skutečná rumor/hearing propagation.

## 2.5 Memory System

**Stav: core memory pipeline DONE / runtime PASS**

```text
Observation
    ↓
ShortTermMemory (dedupe + TTL)
    ↓ importance threshold
LongTermMemory
    ↓ async persistence
restart/load
    ↓
deterministic relevance retrieval
    ↓
Top-N DecisionMemory
```

- [x] short-term dedupe/TTL/expiry;
- [x] deterministic importance;
- [x] persistent long-term memory;
- [x] restart/load;
- [x] relevance retrieval + semantic dedupe + deterministic Top-N;
- [x] wire-safe memory DTO před async inference hranicí.

## 2.6 Needs System

**Stav: 2.6A–2.6C DONE / runtime PASS**

`NeedsState`:

- `HealthPressure`
- `Hunger`
- `Fatigue`
- `SafetyPressure`
- `ResourcePressure`

- [x] deterministic drift;
- [x] clamp `0.0–1.0`;
- [x] live HP/combat coupling;
- [x] recent-memory-driven safety decay;
- [x] edge-triggered `HUNGER_CRITICAL`/`DANGER_HIGH` s hysteresis;
- [x] Needs → deterministic Goal candidates bez LLM.

## 2.7 Goal System

**Stav: core goals + farmer routine vertical slice DONE / runtime PASS**

Runtime implementované goal/routine koncepty:

- `GET_FOOD`
- `FLEE_DANGER`
- `GO_TO_WORK`
- `GO_HOME`

- [x] deterministic candidate generation;
- [x] utility/priority selection;
- [x] retention;
- [x] emergency interruption;
- [x] success/failure/timeout;
- [x] dead agent bez candidate/ActiveGoal;
- [x] routine goal je pod ActiveGoal a emergency goal jej preemptuje.

Poznámka k historii roadmapy: původní 2.11 návrh mluvil o `WORK/REST` goals. Implementovaný vertical slice odděluje routing (`GO_TO_WORK` / `GO_HOME`) od activity/action vrstvy (`WORK` / `REST`). Historie se tím nepřepisuje; aktuální model je přesnější a je runtime ověřený.

## 2.8 Bezpečné Action API

**Stav: deterministic action path DONE / runtime PASS**

Runtime ověřené action primitives / vertical slices:

- `FLEE`
- `MOVE_TO`
- `EAT`
- `WORK`
- `REST`

```text
GET_FOOD
    ↓
MOVE_TO validate/execute
    ↓
TrinityCore movement
    ↓
ARRIVED hard validation
    ↓
PendingEat provenance
    ↓
EAT validate/execute
    ↓
CONSUMED
    ↓
NeedsSystem::SatisfyHunger
    ↓
GET_FOOD SUCCEEDED
```

Farmer routine používá stejný ownership princip: AIWorld/routine navrhne akci, `ActionSystem` ji validuje a TrinityCore provede movement/emote. Group lifecycle zatím nemá žádnou group action execution cestu.

**Pravidlo:** AI navrhuje. `ActionSystem` validuje. TrinityCore provádí.

Remote decision execution zůstává dry-run, aby deterministic Goal→Action pipeline a remote AI nebyly dva vlastníci stejné akce.

## 2.9 AI server — decision protocol

**Stav: 2.9A–2.9E DONE.**

- [x] Protocol V2 + full `AgentContext`;
- [x] sanitizované Top-N memories;
- [x] structured `DecisionIntent`;
- [x] async transport, timeout/fallback;
- [x] request/agent/snapshot correlation;
- [x] goal-attempt + RuntimeGuid provenance;
- [x] world-thread authoritative ActionRequest translation/validation;
- [x] low-cardinality decision metrics;
- [x] multi-agent batch-shaped submit API.

Otevřené hardening:

- [ ] zúžit external DecisionContext privacy boundary (`spawn_id`);
- [ ] nahradit/hardenovat ruční C++ V2 response parser před external/LLM execution;
- [ ] metrics backend/dashboard runtime evidence;
- [ ] bezpečný transfer execution ownership na remote decision path.

## 2.10 Scheduler a úrovně simulace

### Celkový stav

**2.10A DONE / PASS**  
**2.10B DONE / PASS**  
**2.10C DONE / PASS**  
**2.10D DONE / static + runtime PASS**  
**2.10 CLOSED**

World binding a simulation policy jsou oddělené osy. Aktuální model po group identity refactoru:

```text
AgentWorldState:
  MATERIALIZED / ABSTRACT

SimulationTier (individual AgentRecord):
  NEARBY / ACTIVE / BACKGROUND

AgentGroup:
  samostatný GroupId-keyed coarse scheduler
  není SimulationTier::ABSTRACT
```

Historicky 2.10C/2.10D zavedly i `SimulationTier::ABSTRACT` pro tehdejší aggregate-group model. 2.12D tuto část odstranila, protože group už není physical `AgentRecord`; coarse group scheduling nyní běží přes `GroupCoarseSimulationScheduler`.

### 2.10A — bounded multi-agent decision scheduler

Implementation: `d074bc26` feat(ai-world): add bounded multi-agent decision scheduler (2.10A)

- [x] odstraněn single-test-agent decision bottleneck;
- [x] `DecisionScheduler` je pure-value selection;
- [x] per-agent scheduling state;
- [x] hard global `AIWorld.DecisionMaxInFlight`;
- [x] capacity-skipped agent zůstává due, žádná persistentní request queue;
- [x] více agentů může mít `/decision` současně in-flight;
- [x] response drain uvolňuje per-agent duplicate guard;
- [x] stale/provenance/action ownership pravidla 2.9 beze změny.

Runtime ověřeno se třemi guardy a bounded in-flight admission.

### 2.10B — proximity-aware decision cadence + fairness

Implementation: `ed0f3408` feat(ai-world): add proximity-aware decision cadence (2.10B)  
Hardening: `b6271eac` fix(ai-world): recompute cadence deadlines from live class (2.10B P2)

Defaults:

```ini
AIWorld.DecisionSchedulerIntervalMs = 250
AIWorld.DecisionNearbyIntervalMs = 1000
AIWorld.DecisionActiveIntervalMs = 5000
AIWorld.DecisionNearbyPlayerRange = 60.0
```

- [x] scheduler poll je rychlejší než per-agent cadence;
- [x] `NEARBY` ~1 s;
- [x] `ACTIVE` ~5 s;
- [x] current class recomputuje effective due time, žádný stale absolute deadline;
- [x] effective-due-first ordering zabrání permanentní starvation ACTIVE agentů;
- [x] bounded admission z 2.10A zachována;
- [x] žádné live pointery v scheduler state.

Runtime fairness ověřena i s `DecisionMaxInFlight=1`.

### 2.10C — explicit simulation policy

Implementation: `5639c9ec` feat(ai-world): add explicit simulation tier transitions (2.10C)

Current individual derivation:

```text
live Creature + player near → NEARBY
live Creature + no player   → ACTIVE
no live Creature            → BACKGROUND
```

- [x] `AgentWorldState` nebyl nahrazen ani conflated se `SimulationTier`;
- [x] `NEARBY/ACTIVE` jsou decision-eligible;
- [x] `BACKGROUND` neposílá `/decision`;
- [x] žádný force-load;
- [x] transition logging pouze při změně;
- [x] AgentId/Needs/memory/goals se transitionem nemění;
- [x] RuntimeGuid semantics beze změny.

### 2.10D — bounded/staggered coarse scheduling seam

Implementation: `55304729` feat(ai-world): add coarse simulation tier scheduling (2.10D)  
Hardening: `3c2122a9` fix(ai-world): bound and desync coarse simulation ticks (2.10D P2)  
Runtime hardening: `2cda5756` fix(ai-world): stagger coarse simulation tick phase (2.10D P2 runtime)

Default individual background cadence:

```ini
AIWorld.BackgroundSimulationIntervalMs = 60000
AIWorld.CoarseSimulationMaxPerPass = 50
```

- [x] bounded per-pass admission;
- [x] deterministic ordering podle authoritative `NextTickAtMs`, tie-break AgentId;
- [x] capacity-skipped agent zůstává due;
- [x] žádný catch-up loop;
- [x] coarse epoch reset při vstupu/re-entry do Background;
- [x] deterministic one-time phase offset přes stable AgentId hash;
- [x] po ticku pokračuje scheduling přes plain `now + interval`;
- [x] pure-value state (`LastTickAtMs`, `NextTickAtMs`), žádné live pointery;
- [x] runtime potvrzen deterministic staggering i steady-state cadence.

### Známé neblokující scheduler P3

- fast scheduler poll stále prochází registry a live-probuje materializaci každého agenta; před velkou background populací bude potřeba efektivnější materialization/indexing signal;
- coarse selection vytváří/sortuje celý due set před bounded prefix admission; pro velkou populaci může později přijít heap/bucket/deadline index.

## 2.11 První experiment — persistentní farmář

**Stav: 2.11A–2.11E2 CLOSED / static + runtime PASS.**

Referenční NPC: **Pa Maclure** (`SpawnID 80683`, `Entry 250`, Elwynn / Maclure Vineyards). Zůstává `AgentType::Civilian`; profession/routine je samostatná doména, ne nový physical agent type.

### 2.11A — persistent Home / Work

- [x] pure-value `AgentLocation`;
- [x] nullable persistent HomeLocation / WorkLocation;
- [x] restart/load;
- [x] Pa Maclure seed/runtime identity zachována přes stabilní `AgentId`.

### 2.11B — deterministic routine

- [x] Home/Work + synthetic day → `GO_TO_WORK` / `GO_HOME`;
- [x] emergency `FLEE_DANGER` routine potlačí;
- [x] routine runtime state je oddělený od persistent AgentRecord identity;
- [x] runtime `NONE → GO_HOME → GO_TO_WORK → GO_HOME`.

### 2.11C — routine movement přes Action layer

- [x] routine MOVE_TO jen bez aktivního vyššího goal/action ownership;
- [x] target change zastaví starý movement;
- [x] arrival range validation;
- [x] žádné opakované MOVE_TO po dosažení targetu;
- [x] TrinityCore movement zůstává executor.

### 2.11D — WORK / REST activity state

- [x] transient `RoutineActivity` WORK/REST;
- [x] pouze materialized + alive + at target + bez aktivního goal/action + ne engine-moving;
- [x] `GET_FOOD` / `FLEE_DANGER`, unload a death activity správně preempt/clear.

### 2.11E1 — WORK / REST ActionType

- [x] `ActionType::Work` / `ActionType::Rest`;
- [x] provenance + completion flow;
- [x] one-shot emote pouze přes ActionExecutor;
- [x] runtime WORK/REST PASS.

### 2.11E2 — economy persistence

- [x] work-window replay suppression;
- [x] persistent Money/Food/Resource state;
- [x] Money `uint64`;
- [x] monotonic economy version;
- [x] mutation/version bump centralizovaný v persistence API;
- [x] static PASS/CLOSED.

Neblokující caveat: fire-and-forget state snapshot persistence není absolutní crash-durability transakce; pro současný economy vertical slice je to přijaté.

## 2.12 První experiment — wolf pack / AgentGroup coalition

### Aktuální model

Původní aggregate návrh `WolfPack = jeden pseudo-agent s population/hunger` byl během implementace **záměrně opuštěn**. Každý vlk musí zůstat samostatný agent se svou identitou a behavior pipeline.

```text
Wolf A AgentId 101 ┐
Wolf B AgentId 102 ├── AgentGroup / Coalition #7 (GroupId 7)
Wolf C AgentId 103 ┘
Wolf D AgentId 104     // mimo group
```

Každý člen vlastní:

- vlastní `AgentId` a physical spawn binding;
- vlastní memory, needs, goal, decision a actions;
- vlastní materialized/unloaded Creature lifecycle.

`AgentGroup` vlastní pouze sociální/koordinační stav:

- `GroupId`;
- `AgentGroupKind { Loose, Stable }`;
- membership (`AgentId` edges);
- territory/shared environmental resources;
- budoucí cohesion/shared intent/roles/leader facts.

Population se **odvozuje z membership count**, není samostatná mutable aggregate population.

### 2.12A–2.12C — původní group experiment a pivot

- [x] první persistent group state a coarse simulation seam;
- [x] real wolf membership/presence runtime evidence;
- [x] potvrzeno, že přirozené materialization členů nepoužívá force-load;
- [x] architecture review odmítl pseudo-group jako fake `AgentRecord`/SpawnId identity;
- [x] group-level Hunger odstraněn, protože needs patří jednotlivým členům.

### 2.12D — oddělená AgentGroup identity

- [x] samostatný `GroupId`;
- [x] `AgentGroupRecord` / `AgentGroupRegistry` / `AgentGroupPersistence`;
- [x] `ai_agent_groups` oddělená od `ai_agents`;
- [x] `AgentType::AgentGroup` odstraněn;
- [x] `SimulationTier::Abstract` odstraněn z individual simulation policy;
- [x] group coarse tick má `GroupCoarseSimulationScheduler` s `AIWorld.GroupSimulationMaxPerPass`;
- [x] membership loader vyžaduje existující `AgentId`;
- [x] `AgentGroupKind` load je fail-closed (`Loose`/`Stable` pouze);
- [x] group simulation běží nezávisle na tom, zda jsou členové materialized;
- [x] saturated `Resources` no-op už nezvyšuje Version ani nevydává DB write.

Natural presence runtime gate byl ověřen se třemi individuálními vlky (`AgentId 1,2,3`):

```text
loadedMembers=0
→ každý wolf se přirozeně materializuje jako vlastní Creature
→ loadedMembers=3
→ group coarse simulation pokračuje
```

Group se nikdy neváže 1:1 na žádný `Creature`.

### 2.12E1 — runtime group lifecycle

**Stav: CLOSED / STATIC PASS + RUNTIME PASS.**

Implementované API:

```text
CreateGroup(...)
JoinGroup(GroupId, AgentId)
LeaveGroup(GroupId, AgentId)
DissolveGroup(GroupId)
```

- [x] `AgentGroupLifecycleSystem` je jediný owner create/join/leave/dissolve orchestrace;
- [x] join vyžaduje existující group i existující individual `AgentRecord`;
- [x] duplicate membership je odmítnut;
- [x] lifecycle nemutuje individual AgentRecord/Creature world state;
- [x] žádné fake SpawnId / pseudo physical agents;
- [x] membership add/remove DB write je potvrzen read-backem před runtime mutation;
- [x] dissolve maže memberships + group jako jednu DB transakci a runtime registry mění až po potvrzení;
- [x] `GroupId` používá persistentní monotónní `ai_agent_group_id_sequence`;
- [x] sequence load je fail-closed;
- [x] sequence musí být nonzero a `next_group_id > MAX(ai_agent_groups.group_id)` nad fyzickou DB tabulkou;
- [x] sequence reservation je potvrzená read-backem před group INSERT;
- [x] runtime smoke používá pouze existující AgentIds, nevyrábí ghost agenty;
- [x] restart/non-reuse runtime gate PASS.

Runtime evidence:

```text
sequence = 2
Create GroupId 2 → Join 1,2,3 → Leave 3 → Dissolve 2 → PASS
DB: GroupId 2/memberships gone, sequence = 3
restart
load sequence = 3
Create GroupId 3 → Join 1,2,3 → Leave 3 → Dissolve 3 → PASS
```

Tím je potvrzeno, že dissolved `GroupId` se po restartu nerecykluje a individual AgentIds zůstávají nedotčené.

### Next gate před dynamickými coalitions

Současný lifecycle CRUD používá synchronní DB round-trip (`CONNECTION_SYNCH`, read-back, transaction commit). Pro 2.12E1 startup/admin/manual lifecycle je to akceptováno, ale **je to blocker před automatickou per-tick/per-decision formation policy**.

Před prvním automatickým join/leave/create/dissolve callerem:

- [ ] navrhnout non-blocking lifecycle command/persistence boundary;
- [ ] DB operace nesmí blokovat world update thread;
- [ ] async completion se musí aplikovat zpět na world threadu;
- [ ] request musí nést GroupId/AgentId provenance a stale protection;
- [ ] runtime registry se smí změnit až po potvrzeném persistence výsledku;
- [ ] žádný live `Creature*`/`Map*` nesmí překročit async hranici.

Potom:

- [ ] přidat skutečnou `Loose` vs `Stable` membership policy;
- [ ] deterministic formation/dissolution rules pro loose wolf coalition;
- [ ] shared group intent/goal pouze jako koordinace;
- [ ] decompozice group intent na per-member `ActionRequest`;
- [ ] individual emergency (`FLEE_DANGER`, death, invalid action) vždy může group intent preemptovat;
- [ ] combat/movement členů vždy přes existující individual ActionSystem + TrinityCore.

### Známé neblokující P3 po 2.12E1

- [ ] guard proti `uint64` overflow při `_nextGroupId + 1`;
- [ ] failed smoke-test path má udělat best-effort cleanup částečně vytvořené test group;
- [ ] při dissolve explicitně odstranit stale `_groupSimulationSchedule[GroupId]` entry;
- [ ] historická dev-specific/destructive migration cesta před produkčním schema upgrade hardeningem.

## 2.13 LLM dynamic quest / player interaction vertical slice

**Účel této části není stavět komplexní questový obsah Etapy 4. Jejím cílem je před world-preparation Etapou 3 a následným komplexním světem Etapy 4 vyřešit a runtime ověřit technologický řetězec, ze kterého budou dynamické problémy/questy živého světa později vznikat.**

Etapa 2 se nesmí uzavřít pouze mock/deterministic rozhodováním. Musí existovat alespoň jeden runtime vertical slice, ve kterém **skutečný lokální LLM přes oddělený `ai-server`** dostane omezený kontext skutečného problému světa, vrátí strukturovaný návrh interakce/úkolu a server návrh bezpečně validuje před tím, než jej uvidí hráč.

Cílový technický tok:

```text
WORLD STATE / WorldEvent
        ↓
NPC Perception + Memory + Needs/Goal
        ↓
player-help opportunity / REQUEST_HELP
        ↓
sanitisovaný QuestContext DTO
        ↓ async
local ai-server / LLM / GPU
        ↓
structured QuestProposal
        ↓
QuestProposal validation + provenance/stale checks
        ↓
server-owned player-facing task / quest offer
        ↓
player accepts / acts / completes / fails / expires
        ↓
TrinityCore authoritative validation
        ↓
WorldEvent + NPC memory/goal + WORLD STATE
```

### 2.13A — skutečný lokální LLM inference path

- [ ] připojit konkrétní lokální model/backend za existující `ai-server` boundary; konkrétní inference runtime není součástí gameplay API;
- [ ] zachovat async/non-blocking worldserver boundary, timeout a fallback;
- [ ] posílat jen sanitizovaný `QuestContext`, nikdy celý world state ani celou historii agenta;
- [ ] request/response musí nést `request_id`, issuer `AgentId`, relevantní event/cause identity, snapshot/provenance a model/version metadata;
- [ ] LLM response musí být strict structured schema; volný text nesmí být execution contract;
- [ ] invalid JSON/schema, timeout, stale context nebo nedostupný model musí failnout bezpečně bez rozbití world state;
- [ ] runtime evidence musí potvrdit alespoň jeden skutečný local-model request, ne pouze mock response.

### 2.13B — `QuestProposal` contract a serverová validace

Minimální návrhový kontrakt má oddělit narativ od autoritativní mechaniky. LLM smí navrhnout například:

```text
QuestProposal
├── issuer_agent_id
├── source_event / cause_id
├── title / summary / dialogue text
├── objective_type
├── allowed target reference
├── requested amount / condition
├── expiry / urgency intent
└── reward intent / narrative rationale
```

Server je jediná autorita pro skutečnou podobu úkolu.

- [ ] objective type musí být z allowlistu serverem podporovaných primitives;
- [ ] target musí odkazovat na existující a pro NPC známou/povolenou entitu nebo serverem odvozenou kategorii;
- [ ] amount/range/expiry musí mít server-side bounds;
- [ ] odměna je vždy server-owned policy; LLM nesmí libovolně mintovat gold/item/spell/reputation;
- [ ] LLM nesmí generovat ani spouštět SQL, Lua/C++, arbitrary script, spell cast, spawn/delete ani jinou direct world mutation;
- [ ] žádné hallucinated IDs nesmí projít validací;
- [ ] proposal musí mít provenance na problém, který jej vyvolal, a stale proposal se nesmí nabídnout po zániku/změně problému;
- [ ] retry/replay musí být idempotentní a nesmí vytvořit duplicity stejného offeru;
- [ ] textová část může být LLM-generovaná, mechanické podmínky vždy projdou deterministic validator/policy.

### 2.13C — minimální player-facing task lifecycle

Není cílem v Etapě 2 vytvořit obecný content-authoring framework. Stačí nejmenší server-owned seam, který prokáže `WORLD → NPC → PLAYER → WORLD`.

- [ ] NPC umí validovaný dynamický požadavek nabídnout relevantnímu hráči;
- [ ] server eviduje offer/accept/active/completed/failed/expired stav s jednoznačnou identity;
- [ ] acceptance/completion je navázaná na konkrétní player identity a source problem provenance;
- [ ] objective progress vychází z autoritativních TrinityCore eventů, ne z tvrzení LLM;
- [ ] completion/failure/expiry vydá WorldEvent použitelný Perception/Memory/Goal pipeline;
- [ ] výsledek umí změnit skutečný problém světa nebo stav/goal/memory issuer NPC bez direct LLM mutation;
- [ ] restart/reconnect behavior musí být explicitně definovaný alespoň pro aktivní testovací offer.

### 2.13D — runtime gate pro vstup do komplexního světa

Minimální referenční scénář může navazovat na farmář/vlci vertical slice:

```text
wolves/livestock problem exists
        ↓
farmer witnessed/remembers problem
        ↓
PROTECT_HOME / REQUEST_HELP
        ↓
local LLM proposes player-facing help request
        ↓
server validates target/objective/reward bounds
        ↓
player receives + accepts task
        ↓
player action changes authoritative world state
        ↓
completion WorldEvent
        ↓
farmer perceives/remembers result and goal resolves/changes
```

Gate 2.13:

- [ ] jeden skutečný lokální LLM call je součástí runtime end-to-end cesty;
- [ ] LLM output je pouze návrh a není schopen obejít serverová pravidla;
- [ ] validovaný dynamický task je skutečně nabídnut hráči;
- [ ] player action/progress je odvozen z TrinityCore authoritative eventů;
- [ ] dokončení/neúspěch se vrátí do Event → Perception/Memory/Goal/world-state smyčky;
- [ ] timeout, malformed output, stale proposal a LLM outage mají runtime ověřený safe fallback;
- [ ] celý vertical slice má audit/logging dostatečný k reprodukci `source event → proposal → validation → offer → result`.

**Hranice etap:** Etapa 2 řeší technologii a nejmenší ověřené vertical slices. Etapa 3 připravuje a opravuje konkrétní herní prostor, NPC, lokace a frakce. Etapa 4 teprve tyto mechanismy skládá a škáluje do komplexního živého světa; neměla by už objevovat chybějící fundamentální LLM→player, player→world ani world-data architektonickou cestu.

## 2.14 První emergentní end-to-end událost

Cílový scénář se po group architecture pivotu mění tak, že **pack není aggregate mob**. Group může koordinovat shared intent, ale jednotliví vlci jednají samostatně.

```text
individual wolf needs / local opportunity
        ↓
coalition shared intent: approach farm
        ↓
per-member goals / validated MOVE_TO / combat
        ↓
livestock is attacked
        ↓
WorldEvent: LIVESTOCK_KILLED
        ↓
Farmer perception
        ↓
Memory
        ↓
Goal: PROTECT_HOME
        ↓
Decision: REQUEST_HELP
        ↓
validated TrinityCore action
```

- [ ] group shared intent bez direct world mutation;
- [ ] per-member action decomposition;
- [ ] livestock attack event producer;
- [ ] farmer perception/memory of attack;
- [ ] `PROTECT_HOME` / `REQUEST_HELP` vertical slice;
- [ ] end-to-end runtime evidence celé smyčky.

## 2.15 Testy a diagnostika

- [ ] unit testy Goal utility selection;
- [ ] unit testy `ActionRequest` validation;
- [ ] unit testy persistence/serialization;
- [ ] unit testy AgentGroup lifecycle/policy;
- [ ] unit testy `QuestProposal` schema + server-side validation policy;
- [ ] integration AI request → mock response → validation/result;
- [ ] integration local LLM `QuestContext → QuestProposal → validation`;
- [ ] integration timeout/stale/malformed quest proposal → safe fallback;
- [ ] integration player task offer → progress → completion event → NPC/world-state feedback;
- [x] restart → reload memory;
- [x] structured decision/action audit logs;
- [x] simulation tier transition/tick DEBUG observability;
- [x] AgentGroup presence/coarse simulation DEBUG observability;
- [x] AgentGroup lifecycle smoke + restart/non-reuse runtime evidence;
- [ ] debug snapshot podle `AgentId` / `GroupId`;
- [ ] metrics backend/dashboard verification;
- [ ] tier counts / scheduler depth / stale breakdown dashboards.

## Etapa 2 — Definition of Done

- [x] `AIWorldMgr` lifecycle stabilní;
- [x] disable → vanilla gameplay;
- [x] async AIClient non-blocking + timeout/fallback/stale;
- [x] persistent `AgentId` + registry;
- [x] persistent identity + long-term memory přes restart;
- [x] WorldEvent producer;
- [x] perception;
- [x] memory creation + retrieval;
- [x] Needs/Goal lifecycle;
- [x] full `AgentContext` + structured deterministic decision V2;
- [ ] remote AI převezme execution ownership bezpečně — dnes pouze dry-run validation;
- [x] deterministic Goal→Action pipeline pro FLEE/MOVE_TO/EAT;
- [x] farmer routine/action/economy vertical slice;
- [x] výpadek AI serveru nezablokuje worldserver;
- [x] scheduler má rozdílnou NEARBY/ACTIVE cadence a bounded decision admission;
- [x] explicitní individual `NEARBY/ACTIVE/BACKGROUND` simulation policy;
- [x] bounded/staggered coarse scheduling pro individual Background agents;
- [x] oddělená AgentGroup identity/membership persistence;
- [x] bounded GroupId-keyed coarse group scheduling;
- [x] manual/admin AgentGroup create/join/leave/dissolve lifecycle + restart/non-reuse;
- [ ] async-safe dynamic AgentGroup lifecycle persistence;
- [ ] Loose/Stable policy + automatic coalition formation/dissolution;
- [ ] shared group intent → per-member validated actions;
- [ ] skutečný lokální LLM inference request funguje přes async `ai-server` boundary;
- [ ] structured `QuestProposal` + authoritative server validation;
- [ ] player-facing dynamic task lifecycle offer/accept/progress/complete/fail/expire;
- [ ] LLM outage/malformed/stale proposal failuje bezpečně;
- [ ] player task result se vrací přes WorldEvent do NPC memory/goal/world-state;
- [ ] Wolf coalition → livestock → farmer memory → protect/request help end-to-end.

> **Gate:** po Etapě 2 máme technologicky ověřenou uzavřenou smyčku nejen pro NPC akci, ale i pro dynamickou interakci s hráčem:  
> `WORLD STATE → EVENT → PERCEPTION → MEMORY → NEED → GOAL → LOCAL LLM/DECISION → VALIDATED TASK/ACTION → PLAYER/TRINITYCORE → EVENT → WORLD STATE`.

---

# Doporučené pořadí implementace

## Hotovo / runtime ověřeno nebo implementačně uzavřeno

1. [x] Git/fork/upstream základ
2. [x] Docker host + NVIDIA runtime
3. [x] Development image + persistentní build / `ccache`
4. [x] Compose: MySQL + `authserver` + `worldserver` + `ai-server`
5. [x] DB bootstrap + TDB import
6. [x] WoW game data mount a worldserver startup
7. [x] Realm/LAN networking + vzdálený klient
8. [x] Restart/persistence smoke test
9. [x] AIWorldMgr lifecycle + log-only snapshot
10. [x] Async AIClient health/decision + timeout/fallback/stale
11. [x] `AgentRegistry` + stabilní `AgentId`
12. [x] Persistence agenta
13. [x] World Event System
14. [x] Perception System
15. [x] ShortTermMemory
16. [x] Importance + persistent LongTermMemory
17. [x] Relevant-memory retrieval Top-N
18. [x] Needs System 2.6A–2.6C
19. [x] Goal System 2.7A–2.7B2
20. [x] Safe Action API 2.8A–2.8G
21. [x] Decision protocol 2.9A–2.9E
22. [x] 2.10A bounded multi-agent decision admission
23. [x] 2.10B proximity-aware cadence + fairness
24. [x] 2.10C explicit simulation policy
25. [x] 2.10D bounded + deterministic staggered coarse scheduler + runtime closure
26. [x] 2.11A Home/Work persistence
27. [x] 2.11B deterministic farmer routine
28. [x] 2.11C routine movement přes Action layer
29. [x] 2.11D WORK/REST activity
30. [x] 2.11E1 WORK/REST ActionType
31. [x] 2.11E2 persistent economy/reward marker
32. [x] 2.12 AgentGroup identity pivot: group != AgentRecord/Creature
33. [x] 2.12D GroupId/registry/persistence + bounded group coarse scheduler
34. [x] group membership + natural materialization presence runtime evidence
35. [x] 2.12E1 Create/Join/Leave/Dissolve + confirmed persistence
36. [x] persistent monotonic GroupId sequence + restart/non-reuse runtime gate

## Teď

37. [ ] **Async-safe AgentGroup lifecycle persistence boundary před automatickou policy**
38. [ ] **malý 2.12E1 hardening: overflow / smoke cleanup / schedule cleanup**

## Následuje — uzavření Etapy 2

39. [ ] `Loose` vs `Stable` membership policy
40. [ ] deterministic automatic wolf coalition formation/dissolution
41. [ ] shared group intent → per-member goal/action decomposition
42. [ ] 2.13A skutečný local-LLM inference path přes `ai-server`
43. [ ] 2.13B structured `QuestProposal` + authoritative validation
44. [ ] 2.13C player-facing dynamic task lifecycle
45. [ ] 2.13D `WORLD → NPC → LLM → PLAYER → WORLD` runtime gate
46. [ ] 2.14 Wolf coalition → livestock → farmer memory → protect/request help

## Po uzavření Etapy 2 — Etapa 3

47. [ ] exportovat a verzovat kompletní Elwynn Forest NPC/creature spawn census
48. [ ] klasifikovat každý relevantní spawn: role, AI participation, Home/Work/roam a semantic location
49. [ ] vytvořit sémantickou mapu lokalit, sublokalit, cest a resource/danger/faction anchorů
50. [ ] auditovat a opravit Elwynn faction/faction-template a zavést explicitní AI `WorldFactionId`
51. [ ] vynutit invariant `AgentGroup coalition ⊂ jedna WorldFactionId`
52. [ ] připravit faction presence/holdings a kontrolovaný přesun stejné frakce mezi semantic locations
53. [ ] verzovat world DB/spawn/faction/pathing opravy a vytvořit before/after audit
54. [ ] uzavřít Etapu 3 runtime + data-quality gate před spuštěním komplexního světa

## Paralelní hardening

- [ ] Debugger/core dump workflow.
- [ ] Přesný extraction dokument.
- [ ] CUDA compute smoke test.
- [ ] Metrics backend/dashboard runtime evidence pro 2.9D.
- [ ] Nahradit/hardenovat ruční V2 JSON parser před external/LLM execution.
- [ ] Zúžit external DecisionContext privacy boundary (`spawn_id`).
- [ ] Centralizovat semantic identity helper pro širší multi-agent memory scénáře.
- [ ] Sjednotit starší action cancellation paths přes strukturovaný `ActionCompletion`.
- [ ] Optimalizovat fast registry/materialization scan před velkou background populací.
- [ ] Optimalizovat full-sort coarse selection před velkou populací.
- [ ] Produkční hardening historických AIWorld SQL migrací.

---

# Etapa 3 — Elwynn Forest World Preparation

**Cíl:** před spuštěním komplexního živého světa vzít jednu konkrétní oblast — **Elwynn Forest** — a udělat z ní přesně zmapovaný, sémanticky popsaný a datově opravený základ. Etapa 3 není ještě simulace celé společnosti. Je to příprava herního prostoru, aby Etapa 4 nestavěla emergentní chování nad špatnými spawny, nejasnými lokacemi a nesmyslnými faction assignments.

Zásadní pravidlo Etapy 3:

> **Nejdřív musí být jasné kdo je kdo, kde žije/pracuje/působí, ke které frakci patří a jaké části mapy mají význam. Teprve potom má smysl nechat AI svět dlouhodobě měnit.**

## 3.0 Scope a source of truth

- [ ] cílová oblast je Elwynn Forest v používaném TrinityCore 3.3.5/TDB datasetu;
- [ ] přesně definovat hranici census výběru podle authoritative map/zone/area dat, ne podle ručně odhadnutého obdélníku;
- [ ] vytvořit reprodukovatelný export z world DB pro creature spawny, templates, movement/pathing, faction/faction-template, NPC flags a relevantní vazby;
- [ ] uložit odvozený audit/manifest do repozitáře v reviewovatelném formátu (např. CSV/JSON/Markdown generated report); samotný TDB dump se necommitne;
- [ ] každý ruční override musí mít důvod a být verzovaný;
- [ ] z census nesmí mizet spawn jen proto, že zatím není AI-enabled.

Minimální spawn evidence:

```text
SpawnId
Entry
Name/template
Map / Zone / Area
X/Y/Z/O
movement type / path
respawn
NPC flags / role hints
TrinityCore faction/faction-template
AI participation mode
WorldFactionId
SemanticLocationId
Home / Work / Roam anchors
notes / correction status
```

## 3.1 Kompletní census NPC a creature spawnů

Cílem je **zmapovat všechny creature/NPC spawny v Elwynn Forest**, ne jen několik testovacích agentů.

- [ ] vyexportovat 100 % spawnů v definovaném scope;
- [ ] odlišit unikátní named NPC od generických spawnů stejného template;
- [ ] klasifikovat minimálně civilians, guards, merchants/vendors, trainers, quest-related NPC, workers/farmers, travelers, hostile humanoids, predators, prey/fauna a special/scripted entities;
- [ ] u každého relevantního spawnu rozhodnout `FULL_AGENT`, `LIGHTWEIGHT/BACKGROUND`, `VANILLA_ONLY` nebo jiný explicitní participation režim;
- [ ] u AI-enabled NPC připravit role/profession metadata odděleně od physical `AgentType`;
- [ ] připravit Home/Work/Roam/Guard/Resource anchors tam, kde dávají smysl;
- [ ] identifikovat duplicity, nesmyslné spawny, chybné souřadnice, chybné movement types a entity, které se nesmí automaticky převést na persistent AI agenta;
- [ ] vytvořit coverage report, který failne gate, pokud zůstane spawn bez klasifikace nebo explicitního důvodu `VANILLA_ONLY`.

**Gate 3.1:** každý spawn v Elwynn census má explicitní klasifikaci a auditovatelný stav.

## 3.2 Sémantická mapa lokací

AI nesmí chápat svět jen jako surové `x/y/z`. Etapa 3 zavede konkrétní pojmenované lokace a jejich vztahy.

Minimálně zmapovat a runtime ověřit relevantní oblasti jako například:

- Northshire / Northshire Valley;
- Goldshire;
- Stonefield Farm;
- Maclure Vineyards;
- Eastvale Logging Camp;
- Fargodeep Mine;
- Jasperlode Mine;
- Tower of Azora;
- Mirror Lake a okolní body;
- hlavní cesty, křižovatky, mosty, lesní koridory, farmy, kempy, resource sites a danger zones, které mají význam pro simulaci.

Konkrétní seznam a hranice musí vzniknout z map/world dat a runtime kontroly; výše uvedené názvy nejsou náhradou za úplný map audit.

Pro každou semantic location připravit například:

```text
SemanticLocationId
name
type
map/zone/area
center + radius / polygon / bounded region
parent location
adjacent locations
travel connectors / routes
resource tags
danger tags
settlement/farm/mine/road/etc.
faction presence / holding capability
population capacity / role hints
```

- [ ] `HomeLocation`/`WorkLocation` postupně odkazovat na semantic locations/anchors místo náhodných magic coordinates, kde je to vhodné;
- [ ] definovat adjacency a použitelné přesuny mezi lokalitami;
- [ ] rozlišit fyzickou lokaci od politického vlastnictví — jedna location může změnit faction presence bez změny identity;
- [ ] ověřit reprezentativní pathing mezi sousedními semantic locations přes TrinityCore movement/nav data;
- [ ] vytvořit debug výpis/map report, ze kterého lze zjistit, která NPC a frakce jsou přiřazeny k dané lokaci.

## 3.3 Faction audit a oprava

Současné TrinityCore `faction`/`faction_template` hodnoty nejsou samy o sobě dostatečný sociální model AI světa a v cílové oblasti mohou být pro zamýšlenou simulaci nekonzistentní nebo věcně špatné.

Etapa 3 proto oddělí dvě věci:

```text
TrinityCore faction/faction_template
    = combat/reaction/gameplay compatibility

AI WorldFactionId
    = sociální/politická příslušnost pro vztahy,
      coalition eligibility, holdings a budoucí dynamiku světa
```

- [ ] auditovat faction/faction-template u všech Elwynn census spawnů;
- [ ] identifikovat a verzovaně opravit zjevně chybné/inconsistent TrinityCore faction assignments tam, kde ovlivňují gameplay/reaction;
- [ ] zavést explicitní persistentní `WorldFactionId` nebo ekvivalentní sociální identity layer nezávislou na `AgentGroup`;
- [ ] každý AI-enabled agent musí mít explicitní WorldFaction affiliation nebo explicitní `Neutral/Unaffiliated` stav;
- [ ] definovat první konkrétní seznam WorldFaction entit pro Elwynn podle skutečného census, nikoli podle několika předem vymyšlených typů;
- [ ] fauna/predators/humanoids nesmí být automaticky sloučeni do jedné frakce jen proto, že sdílejí combat reaction;
- [ ] faction změna nesmí implicitně přepsat `AgentId`, memory ani individual identity;
- [ ] faction relation/diplomacy matrix pro Etapu 3 může být minimální/static; komplexní změny vztahů patří do Etapy 4.

## 3.4 Coalition pravidla uvnitř frakcí

`AgentGroup`/coalition a `WorldFaction` jsou dvě různé úrovně:

```text
WorldFaction
├── Agent A ┐
├── Agent B ├── AgentGroup / Coalition #1
├── Agent C ┘
├── Agent D ┐
└── Agent E ┴── AgentGroup / Coalition #2
```

Základní invariant:

> **Jedna coalition může obsahovat pouze členy stejné `WorldFactionId`.**

- [ ] `CreateGroup`/`JoinGroup` policy musí znát WorldFaction membership;
- [ ] cross-faction `JoinGroup` failuje před persistence mutation;
- [ ] mixed-faction group se nesmí načíst z DB; invalid persistent state fail-closed / quarantine podle zvolené recovery policy;
- [ ] `Loose`/`Stable` je charakter coalition, nikoli frakce;
- [ ] jedna frakce může mít mnoho současných coalitions a mnoho agentů bez coalition;
- [ ] faction membership sama automaticky nevytváří group;
- [ ] pokud agent někdy v budoucnu změní frakci, jeho nekompatibilní group membership musí být nejprve bezpečně ukončeno/reconciled;
- [ ] runtime test musí potvrdit same-faction join PASS a cross-faction join REJECT bez side effects.

Komplexní diplomacie, přeběhnutí mezi frakcemi a hráčovy faction transitions jsou explicitně **mimo Etapu 3**.

## 3.5 Faction presence a pohyb po mapě

Frakce nesmí být modelována jako statická značka přibitá k jedné souřadnici. Etapa 3 připraví datový/runtime základ pro to, že se **přítomnost frakce může po Elwynn Forest měnit a přesouvat**.

Důležité rozlišení:

```text
WorldFaction identity
    ≠
current faction presence / holdings / occupied locations
```

- [ ] zavést nebo navrhnout `FactionPresence`/`FactionHolding` state nad `SemanticLocationId`;
- [ ] frakce může mít současně přítomnost ve více lokalitách;
- [ ] presence může mít minimálně strength/population/priority nebo jiný malý deterministic state potřebný pro budoucí simulaci;
- [ ] movement znamená přesun/redeployment konkrétních agentů/coalitions mezi semantic locations přes existující movement/action pravidla, ne teleport celé abstraktní frakce;
- [ ] unloaded/background pohyb musí mít později reconciliation na physical spawny bez force-load; Etapa 3 má připravit seam a kontrolovaný smoke, ne kompletní strategickou AI;
- [ ] faction holdings/presence změna nesmí automaticky měnit faction identity NPC, která se v lokaci právě nacházejí;
- [ ] připravit controlled runtime scenario, kde same-faction coalition/presence opustí location A a přesune se do location B se zachováním AgentIds, GroupId a WorldFactionId;
- [ ] všechny změny presence musí být auditovatelné a mít jasný source/cause pro budoucí Event System integraci.

**Etapa 3 připravuje pohyb frakcí jako mechanismus. Proč frakce expanduje, ustupuje, bojuje nebo mění vztahy, bude až dynamika Etapy 4.**

## 3.6 World DB cleanup a verifikace

Mapování není pouze dokumentace. Pokud census odhalí špatná data, musí být cílový Elwynn baseline skutečně opraven.

- [ ] opravy spawn position/orientation tam, kde jsou prokazatelně chybné;
- [ ] opravy movement/pathing/home/wander parametrů;
- [ ] odstranění nebo zdokumentování chybných/duplicitních spawnů;
- [ ] opravy faction/faction-template a dalších flags, pokud neodpovídají zamýšlenému authoritative gameplay;
- [ ] validace vendor/trainer/questgiver/special NPC flags proti skutečné roli;
- [ ] AI metadata a WorldFaction mapování držet ve vlastní versionované vrstvě, pokud není důvod měnit upstream world schema;
- [ ] všechny world DB změny dodat jako versionované TrinityCore SQL updates/migrations, nikdy jako ruční zásah do běžící DB;
- [ ] vytvořit before/after audit report a smoke checklist pro reprezentativní lokace;
- [ ] ověřit, že opravy nerozbily vanilla login, quest/NPC interaction a základní hostile/friendly reaction.

## Etapa 3 — Definition of Done

- [ ] 100 % creature/NPC spawnů v definovaném Elwynn Forest scope je v census manifestu;
- [ ] každý spawn má explicitní classification/participation status;
- [ ] každý AI-enabled agent má semantic location context a explicitní WorldFaction affiliation nebo `Neutral/Unaffiliated`;
- [ ] klíčové lokace, sublokace, cesty a resource/danger anchors jsou verzovaně zmapované;
- [ ] TrinityCore faction/faction-template audit je dokončen a nalezené chyby jsou opravené nebo explicitně zdokumentované;
- [ ] `WorldFactionId` je oddělený od `AgentGroup` i od raw TrinityCore faction template;
- [ ] same-faction coalition membership funguje a cross-faction coalition membership je fail-closed;
- [ ] existuje minimální faction presence/holding model nad semantic locations;
- [ ] controlled same-faction movement/presence test mezi dvěma Elwynn locations projde bez ztráty identity a bez force-load shortcutu;
- [ ] všechny změny world DB jsou reprodukovatelné z Git historie;
- [ ] coverage/audit report neobsahuje nevyřešené „unknown“ položky, které by blokovaly dlouhodobou simulaci;
- [ ] po clean DB bootstrapu a aplikaci updates odpovídá Elwynn runtime připravenému baseline.

> **Gate:** Etapa 3 končí ve chvíli, kdy Elwynn Forest není pro AI jen sada TrinityCore spawnů a souřadnic, ale explicitně popsaný světový prostor s NPC identitami, rolemi, lokacemi, frakcemi a bezpečnými coalition pravidly. Teprve nad tímto baseline se zapíná komplexní dynamika Etapy 4.

---

# Etapa 4 — Living World

**Cíl:** z technologií ověřených v Etapě 2 a z datově připraveného Elwynn Forest z Etapy 3 vytvořit první skutečně komplexní dlouhodobě žijící oblast.

Etapa 4 bude rozvíjet zejména:

- populace a dlouhodobé population pressures;
- zdroje a jejich spotřebu/obnovu;
- ekonomiku a pracovní/obchodní vztahy;
- persistentní relationships mezi agenty;
- dynamické faction relations, konflikty, spolupráci, expanzi a ústup;
- přesuny faction populations/coalitions mezi semantic locations podle skutečných potřeb, hrozeb a příležitostí;
- problémy vznikající z world state místo z pevně napsané quest sequence;
- LLM-generované, serverem validované dynamické questy navázané na tyto skutečné problémy;
- důsledky hráčových akcí, které se vrací do economy/faction/population/memory/world state.

### Hráč a frakce — Etapa 4

Právě zde se může rozvinout myšlenka, že hráč není navždy uzamčený do jedné statické sociální vazby jen kvůli původní quest linii.

- AI questy a dlouhodobé akce hráče mohou měnit vztahy/standing k WorldFaction entitám;
- hráč se může postupně dostat od jedné frakce k jiné, pokud to dovolí serverová pravidla a kauzální historie světa;
- přechod nesmí být jednorázový LLM textový trik — musí vycházet z validovaných činů, vztahů, reputation/standing policy a world events;
- cross-faction diplomacie, defection/allegiance change a přijetí hráče jinou frakcí se řeší jako explicitní systémy;
- coalition invariant zůstává zachovaný: jedna coalition obsahuje členy jedné aktuální WorldFaction; změna faction affiliation nejprve reconciliuje starou coalition membership;
- LLM může navrhovat narativ a sociální možnosti, ale samotná faction transition je vždy server-owned a deterministicky validovaná.

**Etapa 4 už nemá být další infrastrukturní projekt. Má skládat existující technologie a opravená data do komplexního světa a nové technické změny přidávat jen tehdy, když je vyžaduje skutečné emergentní chování Elwynn Forest.**
