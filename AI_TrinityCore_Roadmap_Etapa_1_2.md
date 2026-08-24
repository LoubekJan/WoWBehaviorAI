# AI TrinityCore — Roadmap

> **Výchozí stav:** TrinityCore `3.3.5` + Ubuntu Server + NVIDIA GPU  
> **Rozsah dokumentu:** Etapa 1 — Development Infrastructure, Etapa 2 — AI World Foundation  
> **Aktualizováno:** 2026-08-24  
> **Aktivní větev:** `ai-world`

## Stav projektu

**Etapa 1 má splněný runtime gate. Etapa 2 je aktivně rozpracovaná.**

Na reálném Ubuntu/GPU hostu bylo ověřeno:

- TrinityCore se v Dockeru nakonfiguruje, zkompiluje a nainstaluje do persistentního `/build` volume.
- MySQL se inicializuje a TDB `TDB335.25101` se stáhne/importuje automatizovaným workflow.
- `authserver`, `worldserver`, MySQL a `ai-server` běží přes Compose.
- extrahovaná `dbc/maps/vmaps/mmaps` data jsou připojena do `worldserver`.
- NVIDIA Container Toolkit funguje a Docker vidí dvě RTX 3090.
- WoW 3.3.5a klient na jiném PC v LAN projde authserverem, realm listem a připojí se na worldserver.
- `auth.realmlist` je konfigurován versionovaným `make configure-realm`, nikoli ručním SQL zásahem.
- restart `worldserver` přes `make restart-world` zachová DB/herní stav.

Etapa 2 má runtime ověřenou kompletní Needs → Goal → Action → TrinityCore pipeline pro tři action typy, včetně arrival/completion trackingu a plného GET_FOOD feedback loopu (2.8A–2.8G, pro `FLEE`, `MOVE_TO` a `EAT`; `GET_FOOD` je dvoufázový `MOVE_TO` → `EAT` a skutečně uspokojí Hunger):

- `AIWorldMgr` lifecycle + read-only snapshot.
- async `AIClient` `/health` + verzovaný `/decision` V2, timeout/fallback, korelace a stale-response ochrana.
- `AgentRegistry` + stabilní `AgentId` + materialized/abstract binding.
- persistence identity agenta přes `characters` DB.
- `WorldEvent` + `EventBus` + první TrinityCore producer.
- witnessed-event, nearby-player a nearby-creature perception.
- `ShortTermMemory` s dedupe/TTL/expiry.
- deterministic importance + persistentní `LongTermMemory`.
- restart/load long-term memory.
- deterministic retrieval short-term + long-term memories s relevance rankingem a Top-N omezením.
- per-agent `NeedsState` (2.6A): deterministic hunger/fatigue/resource pressure drift, vlastní cadence, clamp 0.0–1.0, live Creature existence jako authority.
- `NeedsState` live world-state coupling (2.6B1): `HealthPressure` z HP ratio, `SafetyPressure` z `InCombat`, hunger/fatigue/resource zmrazené při smrti.
- `NeedsState` recent-memory-driven safety (2.6B2): `SafetyPressure` po skončení combatu odvozená z čerstvých dangerous memories, deterministický decay přes `AIWorld.ShortTermMemoryTtlMs`.
- Needs threshold events (2.6C): edge-triggered `HUNGER_CRITICAL`/`DANGER_HIGH` s hysteresis latch, audit/debug log only.
- deterministic Goal Candidate generation (2.7A): `NeedsState` → `GoalCandidate[]` (`GET_FOOD`/`FLEE_DANGER`), level-triggered, audit/debug log only.
- deterministic goal selection a `ActiveGoal` (2.7B1): jeden aktivní cíl, deterministický `SelectBest`, retention `>= 0.60` proti activation `>= 0.80`, emergency interruption, dead agent nemá candidate ani `ActiveGoal`.
- `ActiveGoal` lifecycle outcomes (2.7B2): retention threshold = success condition (`SUCCEEDED reason=NEED_SATISFIED`), fixní 30s `TimeoutMs` bez uspokojení Need → `FAILED reason=TIMEOUT`, interrupt má přednost před oběma.
- safe Action API scaffold (2.8A): `ActionRequest`/`ActionSystem::Validate()`, pure-value validation boundary.
- AIWorld creature control takeover (2.8A.5): `AIWorldCreatureAI` potlačuje default auto-aggro/chase/melee/DB movement jen pro `OwnsSpawn()` shodu; TrinityCore combat/threat/damage/death beze změny.
- první skutečná FLEE execution (2.8B): `ActionExecutor::ExecuteFlee`/`StopFlee` přes `MoveFleeing`/`MotionMaster`, TrinityCore pathfinding, scoped stop movement.
- structured `ActionResult` (2.8C): `ExecuteFlee()` vrací `ActionResult` (Status/Reason) místo bool; `GoalCompletion`/`ActionValidationResult`/`ActionResult` jsou tři oddělené, jasně vymezené hranice.
- `MOVE_TO` Action primitiv (2.8D): `ActionSystem` dispatchuje `ValidateFlee`/`ValidateMoveTo` podle `ActionType`, `ActionExecutor::ExecuteMoveTo`/`StopMoveTo` scoped na vlastní `MovePointId`, prokázáno že Action API není FLEE-specific.
- `GET_FOOD` target resolution (2.8E): `FoodTargetResolver` (fixed config zdroj) → `MOVE_TO` jen na `GET_FOOD Activated`, cleanup na interrupt/completion.
- MOVE_TO arrival/completion tracking (2.8F): `AIWorldCreatureAI::MovementInform` → `ActionEngineEventBus` → hard-validated `ActionCompletion` (pozice v toleranci, ne jen callback), reconciliation pro dropped events. Arrival != `EAT`; `Hunger` se nemění.
- `EAT` jako třetí Action primitiv (2.8G): `GET_FOOD` je dvoufázový `MOVE_TO` → `EAT`; `MOVE_TO ARRIVED` jen zaznamená `AgentRecord::PendingEat`, teprve `EAT SUCCEEDED/CONSUMED` zavolá `NeedsSystem::SatisfyHunger()`. Continuation provenance vynucuje `ActionSystem` přes authoritative arrival facts, ne přes důvěru v `ActionRequest`; `PendingEat` odloží `EAT` až za aktuální goal-selection pass, takže same-tick `FLEE_DANGER` emergency pořád vyhraje nad `GET_FOOD`.
- decision protocol 2.9A–2.9E: plný `AgentContext` + Top-N memories/Needs/ActiveGoal/available actions, structured `DecisionIntent`, strict V2 korelace, world-thread stale/provenance guard, authoritative FLEE translation do `ActionSystem::Validate()` jako dry-run, decision metrics a multi-agent batch-shaped submit API.

**Aktuální NEXT:** `2.10A` — scheduler/admission model nad hotovým `SubmitDecisions(vector<AIRequest>)` seamem; odstranit dnešní globální single-decision bottleneck bezpečnou, bounded politikou bez blokace world threadu.

Zbývající položky Etapy 1 jsou **hardening / developer tooling**, nikoli gate pro pokračování AI vrstvy.

---

## Obsah

- [Základní princip](#základní-princip)
- [Přehled etap](#přehled-etap)
- [Etapa 1 — Development Infrastructure](#etapa-1--development-infrastructure)
  - [1.0 Git a výchozí stav](#10-git-a-výchozí-stav)
  - [1.1 Ubuntu host](#11-ubuntu-host)
  - [1.2 Struktura repozitáře](#12-struktura-repozitáře)
  - [1.3 TrinityCore development image](#13-trinitycore-development-image)
  - [1.4 Incremental build a cache](#14-incremental-build-a-cache)
  - [1.5 Docker Compose stack](#15-docker-compose-stack)
  - [1.6 Databáze, TDB a realm bootstrap](#16-databáze-tdb-a-realm-bootstrap)
  - [1.7 WoW game data](#17-wow-game-data)
  - [1.8 Developer UX](#18-developer-ux)
  - [1.9 Debugging a observability](#19-debugging-a-observability)
  - [1.10 GPU infrastruktura](#110-gpu-infrastruktura)
  - [1.11 AI service skeleton](#111-ai-service-skeleton)
  - [Etapa 1 — Definition of Done](#etapa-1--definition-of-done)
  - [Etapa 1 — neblokující hardening backlog](#etapa-1--neblokující-hardening-backlog)
- [Etapa 2 — AI World Foundation](#etapa-2--ai-world-foundation)
  - [První milestone Etapy 2 — AIWorldMgr smoke](#první-milestone-etapy-2--aiworldmgr-smoke)
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
  - [2.12 První experiment — wolf pack](#212-první-experiment--wolf-pack)
  - [2.13 První emergentní end-to-end událost](#213-první-emergentní-end-to-end-událost)
  - [2.14 Testy a diagnostika](#214-testy-a-diagnostika)
  - [Etapa 2 — Definition of Done](#etapa-2--definition-of-done)
- [Doporučené pořadí implementace](#doporučené-pořadí-implementace)

---

## Základní princip

Nejdříve vytvořit rychlé a reprodukovatelné vývojové prostředí. Teprve potom budovat persistentní AI vrstvu.

**AI přemýšlí a navrhuje záměr. TrinityCore a simulační engine vždy rozhodují, co je fyzicky a pravidlově možné a jak se akce provede.**

```text
AI / planner
    │
    │ ActionRequest / intent
    ▼
validation + simulation rules
    │
    ▼
TrinityCore gameplay execution
    │
    ▼
WorldEvent / nový world state
```

AI nikdy nesmí přímo zapisovat libovolný stav do světa ani obcházet serverová pravidla.

> **Předpoklad:** projekt vychází z TrinityCore `3.3.5`. WoW client data, DB volumes, tajné hodnoty a AI modely se neukládají do Git repozitáře.

## Přehled etap

| Etapa | Stav | Hlavní cíl | Gate pro pokračování |
|---|---|---|---|
| **1 — Development Infrastructure** | ✅ **GATE SPLNĚN** | Reprodukovatelný Docker development stack | build → DB/TDB → worldserver → vzdálený WoW klient → restart/persistence |
| **2 — AI World Foundation** | 🟡 **IN PROGRESS — NEXT 2.10A (scheduler/admission)** | Persistentní agenti, události, paměť, cíle, Action API a async AI bridge | Wolf attack → memory → goal → decision → validated action |

---

# Etapa 1 — Development Infrastructure

**Cíl:** dostat projekt do stavu, kdy je vývoj rychlý, opakovatelný a bezpečný. Změna C++ souboru nesmí vyžadovat ruční rekonstrukci serveru nebo neversionované zásahy do deploymentu.

## 1.0 Git a výchozí stav

- [x] Používat TrinityCore upstream branch `3.3.5`.
- [x] Zaznamenat výchozí upstream commit: `2a64b72689cc8d797e4c93a0c96dfa2dc06f64c8`.
- [x] `origin` je fork `LoubekJan/WoWBehaviorAI`.
- [x] `upstream` je `TrinityCore/TrinityCore`.
- [x] Hlavní vývojová branch je `ai-world`.
- [x] Zachovat společnou historii a možnost mergovat upstream `3.3.5`.
- [x] Ignorovat runtime data, lokální `.env`, modely a lokální build artefakty.

## 1.1 Ubuntu host

- [x] Git.
- [x] Docker Engine.
- [x] Docker Compose plugin.
- [x] NVIDIA driver.
- [x] NVIDIA Container Toolkit.
- [x] `nvidia-smi` funguje na hostu.
- [x] GPU je dostupné z Docker containeru.
- [x] Běžný development lze provádět bez spouštění celého workflow jako root.

## 1.2 Struktura repozitáře

Aktuální projektová vrstva nad TrinityCore:

```text
WoWBehaviorAI/
├── src/
├── sql/
├── cmake/
├── docker/
│   ├── trinitycore/
│   │   └── Dockerfile.dev
│   ├── ai/
│   └── scripts/
├── deploy/
│   ├── worldserver.conf
│   ├── authserver.conf
│   └── mysql/
├── runtime/                  # gitignored host state
│   ├── data/
│   │   ├── dbc/
│   │   ├── maps/
│   │   ├── vmaps/
│   │   └── mmaps/
│   └── logs/
├── compose.yml
├── compose.dev.yml
├── .env.example
├── Makefile
├── README_DEV.md
└── AI_TrinityCore_Roadmap_Etapa_1_2.md
```

- [x] `runtime/` se necommituje.
- [x] Deployment konfigurace a helper skripty jsou verzované.
- [x] Tajné/deployment-specific hodnoty jsou v gitignored `.env`; `.env.example` je verzovaná šablona.
- [x] AI model files jsou gitignored.

## 1.3 TrinityCore development image

- [x] `Dockerfile.dev` je založený na Ubuntu 22.04.
- [x] Obsahuje compiler toolchain, CMake, Ninja, Boost, OpenSSL, MySQL dev knihovny, Git, `gdb` a `ccache`.
- [x] Source tree je bind-mounted do `/workspace`.
- [x] Source není kopírován do image při každé změně `.cpp`.
- [x] `/build` a `/ccache` jsou oddělené persistentní volumes.
- [x] Development container používá non-root uživatele.
- [ ] Samostatný minimální runtime image — **odloženo; není gate Etapy 1**.

## 1.4 Incremental build a cache

- [x] Oddělený `/build` adresář v Docker named volume.
- [x] `ccache` je nakonfigurován do persistentního `/ccache` volume.
- [x] Build používá Ninja.
- [x] Výchozí development profil je `RelWithDebInfo`.
- [x] `make build` provádí `cmake --build` a následně `cmake --install`, takže `/build/bin/authserver` a `/build/bin/worldserver` skutečně vzniknou.
- [x] Čistý build TrinityCore byl ověřen na reálném hostu až po link `worldserver`.
- [ ] Samostatný `Debug` profil jako pohodlný Make target.
- [ ] Změřit a zaznamenat no-op rebuild bez změn.
- [ ] Explicitně ověřit/logovat rebuild po změně jediného `.cpp` jako incremental test.

> **Běžný workflow:** změna kódu → `make build` → `make restart-world`. Clean build jen explicitně.

## 1.5 Docker Compose stack

```text
docker compose
├── mysql
├── authserver
├── worldserver
├── tc-dev
├── ai-server
└── gpu-check
```

- [x] MySQL 8 s persistentním volume.
- [x] MySQL healthcheck.
- [x] Databáze `auth`, `characters` a `world`.
- [x] `authserver` jako samostatná service.
- [x] `worldserver` jako samostatná service.
- [x] Persistentní build volume je sdílené s runtime službami.
- [x] Game data a logy jsou mountované mimo image.
- [x] Start order vůči MySQL je řízen `depends_on` + healthcheck.
- [x] AI server je samostatná interní service s healthcheckem.
- [x] `3724` a `8085` jsou publikované na hosta; MySQL a AI HTTP port nejsou implicitně vystavené ven.
- [x] `worldserver` má `stdin_open` + `tty` pro interaktivní development konzoli.

## 1.6 Databáze, TDB a realm bootstrap

- [x] Automatické vytvoření `auth`, `characters`, `world` a aplikačního DB uživatele.
- [x] TrinityCore auto-setup a standardní SQL updates.
- [x] Runtime má mountnuté potřebné `sql/` zdroje pro updater.
- [x] TDB import je automatizovaný přes `make db-import-tdb`.
- [x] Použitý world dataset je pinovatelný; runtime ověřen s `TDB335.25101`.
- [x] TDB downloader čeká na připravenost MySQL místo race condition při startu.
- [x] `make reset-db` existuje jako explicitní destruktivní operace.
- [x] `auth.realmlist` se konfiguruje přes versionovaný `make configure-realm`.
- [x] `REALM_*` nemají tichý fallback na `127.0.0.1`; chybějící hodnoty způsobí fail-fast.
- [x] Realm konfigurátor po zápisu provádí DB read-back a ověřuje skutečný stav.
- [x] LAN realm address byla ověřena reálným klientem z jiného PC.

## 1.7 WoW game data

- [x] `runtime/data/dbc` je připojeno do `worldserver`.
- [x] `runtime/data/maps` je připojeno do `worldserver`.
- [x] `runtime/data/vmaps` je připojeno do `worldserver`.
- [x] `runtime/data/mmaps` je připojeno do `worldserver`.
- [x] Data nejsou commitována ani kopírována do běžného source image.
- [x] Reálný `worldserver` s těmito daty překonal critical-data startup gate a vstoupil do běžící konzole `TC>`.
- [ ] Doplnit do `README_DEV.md` přesný krokový extraction postup a příkazy pro TrinityCore extractory.

## 1.8 Developer UX

`Makefile` je primární development rozhraní:

| Příkaz | Účel | Stav |
|---|---|---|
| `make bootstrap` | `.env`, runtime adresáře, build image | ✅ |
| `make build` | incremental build + install TrinityCore | ✅ |
| `make rebuild` | clean-first build bez ručního mazání volumes | ✅ |
| `make start` | spuštění stacku | ✅ |
| `make stop` | zastavení stacku | ✅ |
| `make restart-world` | restart pouze `worldserver` | ✅ runtime ověřeno |
| `make logs` | společné logy | ✅ |
| `make world-logs` | follow worldserver logu | ✅ |
| `make shell` | development shell | ✅ |
| `make db-shell` | MySQL shell | ✅ |
| `make clean-build` | explicitní čistý build | ✅ |
| `make reset-db` | explicitní reset development DB | ✅ |
| `make gpu-test` | NVIDIA test z containeru | ✅ runtime ověřeno |
| `make db-import-tdb` | download/import pinovaného TDB | ✅ runtime ověřeno |
| `make configure-realm` | versionovaná konfigurace `auth.realmlist` | ✅ runtime ověřeno |

## 1.9 Debugging a observability

- [x] Výchozí build `RelWithDebInfo` obsahuje debug informace vhodné pro development diagnostiku.
- [x] `gdb` je součástí development image.
- [x] `worldserver` má interaktivní TTY podporu v Compose.
- [x] Server log directory je mountovaný na hosta.
- [ ] Ověřit attach `gdb` na běžící `worldserver`.
- [ ] Ověřit testovací breakpoint a čitelný stack trace.
- [ ] Povolit a ověřit core dumps v development stacku.
- [ ] Definovat/persistovat crash artefakty.
- [ ] Přidat logging kategorie:
  - `ai.world`
  - `ai.agent`
  - `ai.player`
  - `ai.simulation`
  - `ai.inference`

Tyto neověřené položky neblokují první `AIWorldMgr` smoke test; před větší AI implementací je ale nutné je průběžně zavřít.

## 1.10 GPU infrastruktura

- [x] `gpu-check` Compose profil.
- [x] `nvidia-smi` uvnitř containeru.
- [x] Docker vidí GPU model i VRAM; ověřeny dvě NVIDIA GeForce RTX 3090.
- [x] GPU inference není součástí procesu `worldserver`.
- [ ] Ověřit CUDA/PyTorch runtime přímo v budoucím inference image `ai-server`.
- [ ] Spustit skutečný jednoduchý GPU compute workload, ne pouze `nvidia-smi`.

```text
worldserver  <---- async network / IPC ---->  ai-server  ---->  GPU
```

## 1.11 AI service skeleton

Etapa 1 potřebuje pouze izolovanou AI službu a healthcheck. Async bridge z `worldserver` byl následně implementován a runtime ověřen v Etapě 2.

- [x] `ai-server` běží jako samostatný container.
- [x] FastAPI `/health` endpoint.
- [x] Compose healthcheck potvrzuje dostupnost služby.
- [x] AI služba je na interní Docker síti a její HTTP port není standardně publikovaný na hosta.
- [x] `worldserver` není procesově ani GPU-runtime svázán s `ai-server`.
- [x] `worldserver` → `ai-server` neblokující health request — **Etapa 2 / runtime PASS**.
- [ ] Timeout/reconnect/fallback metriky — observability hardening.
- [x] Runtime fault test: vypnout `ai-server` a prokázat, že aktivní svět pokračuje bez blokace/crashe.

## Etapa 1 — Definition of Done

### Runtime gate — SPLNĚNO

- [x] Projekt lze na Ubuntu hostu sestavit přes Docker bez ruční instalace TrinityCore build dependencies na hostu.
- [x] Docker sestaví a nainstaluje TrinityCore binaries do persistentního build volume.
- [x] MySQL se inicializuje automaticky.
- [x] TDB bootstrap proběhne automatizovaným workflow.
- [x] `authserver` a `worldserver` se stabilně spustí přes Compose.
- [x] Extrahovaná WoW data jsou správně připojena a worldserver je načte.
- [x] WoW 3.3.5a klient na jiném PC v LAN projde přes authserver a připojí se na realm/worldserver.
- [x] Realm networking nepoužívá chybné `127.0.0.1` pro vzdáleného klienta.
- [x] Restart `worldserver` přes `make restart-world` zachová DB/herní stav.
- [x] GPU je dostupné uvnitř Dockeru.
- [x] Prázdný `ai-server` běží healthy jako samostatná služba.

**Verdikt:** Etapa 2 byla zahájena a její foundation část je průběžně runtime ověřována.

## Etapa 1 — neblokující hardening backlog

- [ ] Přesný extraction návod pro `dbc/maps/vmaps/mmaps` v `README_DEV.md`.
- [ ] Samostatný pohodlný `Debug` build target.
- [ ] Měřený no-op/incremental build smoke test.
- [ ] `gdb` attach + breakpoint smoke test.
- [ ] Core dumps + crash artefakty.
- [ ] AI logging categories.
- [ ] CUDA compute smoke test v AI inference image.
- [ ] Aktualizovat `README_DEV.md` status tak, aby už netvrdil, že reálný Docker/GPU runtime nebyl otestován.

---

# Etapa 2 — AI World Foundation

**Cíl:** vytvořit persistentní vrstvu nad TrinityCore, ve které mohou NPC a další entity existovat jako dlouhodobí agenti se stavem, pamětí, cíli a omezenou sadou akcí.

GPU/AI služba se používá pro rozhodování tam, kde dává smysl. Fyziku, authoritative world state a pravidla stále vynucuje TrinityCore.

## První milestone Etapy 2 — AIWorldMgr smoke

Nezačínat persistence tabulkami ani LLM plánováním. První změna musí být malý, auditovatelný vertikální řez, který dokáže, že nová vrstva správně žije uvnitř TrinityCore lifecycle.

### Milestone 2A — lifecycle + log-only agent snapshot

**Stav: DONE / runtime PASS**

Navrhovaná struktura:

```text
src/server/game/AIWorld/
├── AIWorldMgr.h
├── AIWorldMgr.cpp
├── Agent/
├── Events/
├── Inference/
├── Simulation/
└── Debug/
```

- [x] Přidat `AIWorldMgr` singleton/subsystem bez zásahu do existující CreatureAI hierarchie.
- [x] Inicializovat `AIWorldMgr` při startu `worldserver`.
- [x] Korektně ho ukončit při shutdownu.
- [x] Přidat config flag, kterým lze celý AIWorld vypnout.
- [x] S vypnutým AIWorld musí běžet vanilla TrinityCore chování beze změny.
- [x] Vybrat jedno stabilní testovací NPC.
- [x] Jednou za bezpečný interval vytvořit malý read-only snapshot jeho stavu.
- [x] Snapshot pouze zalogovat; **žádná AI akce zatím nesmí měnit svět**.
- [x] Ověřit start → login → běžná hra → restart worldserveru s aktivním `AIWorldMgr`.

### Milestone 2B — první async bridge

**Stav: DONE / runtime PASS; decision observability implementována v 2.9D, metrics backend runtime evidence zůstává neblokující pending.**

Teprve po 2A:

```text
AIWorldMgr
   │
   └── AIClient -- async --> ai-server /health or /decision
                          <-- response
```

- [x] Implementovat neblokující klienta z `worldserver` do `ai-server`.
- [x] Nikdy nečekat na síť/inference v world update hot path.
- [x] Přidat timeout a deterministic fallback.
- [x] Zahodit stale response, který dorazí po deadline nebo po změně relevantního agent state.
- [x] Decision observability: `request_id` v audit logu; latency/queue/outcome/timeout/invalid/stale jako nízkokardinalitní metriky (2.9D). Metrics backend runtime evidence ještě nebyla samostatně ověřena.
- [x] Vypnout `ai-server` během běžícího světa a ověřit, že `worldserver` pokračuje bez blokace/crashe.

### Gate pro další práci

**SPLNĚNO:** 2A + 2B jsou runtime ověřené; práce pokračovala přes persistence, events, perception, memory, Needs, Goals, Action API a decision protocol.

---

## 2.0 Cílová architektura

```text
TrinityCore
│
├── Player / Creature / GameObject
│
└── AIWorldMgr
    ├── AgentRegistry
    ├── PerceptionSystem
    ├── MemorySystem
    ├── NeedsSystem
    ├── GoalSystem
    ├── ActionSystem
    ├── EventSystem
    ├── Scheduler
    └── AIClient  -------------------->  ai-server  ---> GPU
```

- [x] Vytvořit subsystem `src/server/game/AIWorld/`.
- [x] `AIWorld` se inicializuje při startu `worldserver` a korektně se ukončí při shutdownu.
- [x] Normální TrinityCore gameplay funguje i při vypnutém `AIWorld`.
- [x] Všechny AI/network requesty jsou asynchronní vůči world update loopu.
- [x] Každá externí decision odpověď má deadline a kontrolu freshness/provenance.

## 2.1 Persistentní agent

**Stav: core registry/binding DONE / runtime PASS**

Oddělit dočasný TrinityCore objekt od dlouhodobé identity agenta. `Creature` může zmizet z aktivní mapy, ale agent stále existuje v simulaci.

```text
AIAgent
├── AgentId
├── WorldBinding (Creature / Player / Abstract)
├── Identity
├── Personality
├── Needs
├── Goals
├── Memory
├── Relationships
├── Economy
└── RuntimeState
```

- [x] Zavést stabilní `AgentId`.
- [x] Vytvořit `AgentRegistry`.
- [x] Umět propojit `AgentId` s aktuálním `Creature/ObjectGuid`.
- [x] Umět agenta odpojit od `Creature` a ponechat jej jako abstraktní stav.
- [x] Umět agenta znovu materializovat do světa.
- [x] Připravit typy agentů minimálně:
  - `CIVILIAN`
  - `GUARD`
  - `MERCHANT`
  - `CREATURE_GROUP`

## 2.2 Persistence

AI stav musí přežít restart `worldserver`.

Aktuálně implementované tabulky používají `characters` DB (`ai_agents`, `ai_long_term_memories`). Další tabulky vzniknou až s jejich skutečným subsystemem.

Navrhované/logické oblasti persistence:

```text
ai_agents
ai_long_term_memories
ai_agent_relationships
ai_agent_goals
ai_events
ai_locations
```

- [x] Navrhnout DB schema s verzováním/migracemi pro aktuálně implementovaný agent/memory scope.
- [x] Persistovat identitu, základní stav a vazbu na svět.
- [x] Persistovat dlouhodobou paměť.
- [ ] Persistovat vztahy mezi agenty a hráči.
- [ ] Persistovat aktivní cíle.
- [ ] Persistovat důležité world events pro audit a replay/debug.
- [x] Ověřit save → restart → load jednoho agenta.

## 2.3 World Event System

**Stav: první event producer + EventBus DONE / runtime PASS**

Události jsou základ kauzality. AI nemá dostávat náhodný příběh; má reagovat na skutečné změny stavu světa.

První typy událostí:

```text
CreatureKilled
NPCInjured
PlayerSeen
ItemStolen
TradeCompleted
LivestockKilled
WolfPackMoved
FoodShortage
NPCDied
```

- [x] Definovat `WorldEvent` s typem, časem, lokací, aktorem, cílem a payloadem.
- [x] Vytvořit `EventBus` uvnitř `AIWorld`.
- [x] Napojit první TrinityCore hooky na vznik událostí.
- [ ] Oddělit transient event od persistentní historické události.
- [x] Přidat `correlation/cause id`, aby šlo sledovat řetězec příčina → následek.
- [x] Přidat debug log událostí.

## 2.4 Perception System

**Stav: core sight perception DONE / runtime PASS**

- [x] Agent nesmí automaticky vědět globální stav světa.
- [x] Přidat nearby entity perception.
- [x] Přidat základní range check.
- [x] Tam, kde je vhodné, respektovat line-of-sight.
- [x] Přidat perception událostí, kterých byl agent svědkem.
- [x] Přidat přípravu pro informace z rozhovorů/rumorů v pozdější etapě.
- [x] Z perception vytvářet `Observation` objekty použitelné pro memory a decision context.

> `Sight` je aktuálně runtime použitý kanál; `Hearing/Rumor` jsou připravené jako pozdější rozšíření, nikoli hotová rumor propagation.

## 2.5 Memory System

**Stav: core memory pipeline DONE / runtime PASS**

```text
ShortTermMemory
LongTermMemory
Knowledge
Relationships
```

- [x] Implementovat krátkodobou paměť s expirací.
- [x] Implementovat dlouhodobou paměť pro důležité události.
- [x] Přidat importance score.
- [x] Přidat čas a zdroj informace.
- [x] Přidat vazbu na osoby, lokaci a událost.
- [x] Připravit retrieval relevantních vzpomínek podle aktuální situace.
- [x] LLM/inference nikdy neposílat celou historii; posílat pouze vybrané relevantní záznamy — **Top-N retrieval se od 2.9A přenáší v `AgentContext::RelevantMemories` jako sanitizované `DecisionMemory` DTO**.

### Implementovaný stav 2.5

```text
Observation
    ↓
deterministic importance
    ↓
ShortTermMemory (dedupe + TTL + expiry)
    ↓ importance threshold
LongTermMemory
    ↓
async characters DB persistence
    ↓ restart/load
deterministic relevance retrieval
    ↓
Top N relevant memories
```

Runtime ověřeno:

- `PlayerSeen`, `CreatureSeen` a witnessed `WorldEvent` → memory.
- opakovaná observation refreshuje stejné short-term memory ID a `ObservationCount`.
- short-term memory expiruje podle TTL a po novém encounteru vznikne nové ID.
- `CREATURE_KILLED` dostane importance `0.85`, překročí threshold a promuje se do long-term memory.
- runtime DB INSERT je asynchronní vůči world threadu.
- persistentní long-term memory se po restartu načte se stejným DB `memory_id`.
- retrieval kombinuje aktivní short-term + long-term records.
- relevance používá deterministic importance/freshness/locality scoring.
- stejný semantic fact v short-term a long-term se ve výsledku deduplikuje.
- výsledky mají deterministic pořadí a `AIWorld.MemoryRetrievalTopN` limit.
- od 2.9A se retrieval Top-N mapuje na world threadu do wire-safe `DecisionMemory` a posílá v `/decision` `AgentContext`; actor/target se zužují na `Entry` + `AgentId`, bez live pointerů.

## 2.6 Needs System

**Stav: 2.6A + 2.6B1 + 2.6B2 + 2.6C (NeedsState + deterministic drift + live world-state coupling + recent-memory safety + threshold events) DONE / runtime PASS**

- [x] Zavést minimální potřeby: `health`, `hunger`, `fatigue`, `safety`, `money/resource pressure` — **`NeedsState` zavedena se všemi pěti poli; `health`/`safety` pressure jsou odvozené z live Creature stavu (2.6B1) s recent-memory-driven fallback po skončení combatu (2.6B2)**.
- [x] Potřeby aktualizovat deterministicky v simulation ticku — `hunger`/`fatigue`/`resource pressure` drift na vlastní ~1s cadence, nezávislé na snapshot cadence.
- [x] Potřeby omezit do definovaného rozsahu `0.0–1.0`.
- [x] Přidat threshold events, například `HUNGER_CRITICAL` nebo `DANGER_HIGH` — **edge-triggered, hysteresis latch, audit/debug log only (2.6C); katalog zatím jen `HUNGER_CRITICAL`/`DANGER_HIGH`**.
- [x] Nechat potřeby generovat kandidáty na cíle bez LLM — **2.7A generuje deterministic `GET_FOOD`/`FLEE_DANGER` candidates přímo z `NeedsState`**.

### Implementovaný stav 2.6A

```text
AgentRecord.Needs (registry-owned, přežije Creature unload/reload)
    ↓ vlastní ~1s cadence (AIWorld.NeedsUpdateIntervalMs)
live Creature lookup (authority pro Materialized/Abstract)
    ↓
NeedsSystem::Update (pure value transform, bez AgentId/Creature/Map)
    ↓
hunger/fatigue/resource drift + clamp 0.0-1.0
```

Implementation: `11f1b0b7` feat(ai-world): add per-agent Needs drift (2.6A)
Hardening: `546526a7` fix(ai-world): use live Creature lookup in UpdateNeeds, not stale WorldState (P2)

Runtime ověřeno:

- nezávislá ~1s Needs cadence.
- deterministic hunger/fatigue/resource drift.
- `HealthPressure`/`SafetyPressure` zatím `0.0`.
- clamp `0.0–1.0` PASS.
- `AgentRecord` drží `NeedsState` přes Creature unload/reload.
- live Creature existence je authority pro Materialized/Abstract (ne zastaralý `WorldState`).
- žádná Needs persistence.
- žádná world mutation.
- `/decision` beze změny v tomto milestone; od 2.9A se `NeedsState` posílá v `AgentContext`.

### Implementovaný stav 2.6B1

```text
live Creature (Health, MaxHealth, Alive, InCombat)
    ↓
NeedsUpdateContext (plain values, žádný Creature*)
    ↓
NeedsSystem::Update (stále pure value transform)
    ↓
HealthPressure = 1 - HP/MaxHP  (1.0 když dead nebo MaxHealth == 0)
SafetyPressure  = InCombat ? 1.0 : 0.0
```

Implementation: `d120e1d8` feat(ai-world): derive needs from live world state (2.6B1)

Runtime ověřeno:

- `HealthPressure = 1 - HP/MaxHP`.
- `InCombat` → `SafetyPressure = 1`.
- konec combatu → `SafetyPressure = 0`.
- `dead` → `HealthPressure = 1`.
- `dead` → hunger/fatigue/resource zmrazené (nedriftují).
- respawn → drift pokračuje z předchozí hodnoty.
- `NeedsSystem` stále pure-value (jen `NeedsUpdateContext`, žádný `Creature*`).
- žádná world mutation / DB změna.

> Otevřená sémantická poznámka z 2.6B1: při `dead` zůstává `SafetyPressure` zmražená na poslední hodnotě. Goal layer to dnes bezpečně obchází tím, že dead agent nemá candidates ani ActiveGoal.

### Implementovaný stav 2.6B2

```text
RetrievedMemory (bez _memoryRetrievalTopN truncation)
    ↓
NeedsSystem::EvaluateMemorySafety (whitelist: CreatureKilled/NPCInjured/LivestockKilled/WolfPackMoved/NPCDied)
    ↓ lineární decay přes AIWorld.ShortTermMemoryTtlMs od LastObservedAtMs
MemorySafetyPressure
    ↓
SafetyPressure = InCombat ? 1.0 : MemorySafetyPressure
```

Implementation: `0155b507` feat(ai-world): derive safety pressure from recent memory (2.6B2)
Hardening: `f29923a5` fix(ai-world): don't truncate safety-relevant memories to MemoryRetrievalTopN (P2)

Runtime ověřeno:

- combat končí, ale existuje čerstvá dangerous memory → `SafetyPressure > 0` bez combatu.
- `MemorySafetyPressure` deterministicky klesá k 0 v rámci `AIWorld.ShortTermMemoryTtlMs` window.
- po vypršení window → `MemorySafetyPressure = 0`, `SafetyPressure = 0`.
- staré/persistentní long-term dangerous memory mimo recent window nedrží agenta permanentně "v nebezpečí".
- non-dangerous typy (`PlayerSeen`, `TradeCompleted`, ...) nepřispívají do `MemorySafetyPressure`.
- combat má vždy přednost před memory (`SafetyPressure = 1.0` bez ohledu na memory).
- safety retrieval už není omezen `AIWorld.MemoryRetrievalTopN` (P2 fix) - ten limit zůstává jen pro decision-context retrieval.
- žádný nový config; žádná world mutation / DB změna.

### Implementovaný stav 2.6C

```text
NeedsState (Hunger, SafetyPressure)
    ↓
NeedsSystem::EvaluateThresholds (per-agent NeedsThresholdState latch)
    ↓ enter >= 0.80 (jednou), reset < 0.60 (rearm)
NeedsThresholdEvent (HUNGER_CRITICAL / DANGER_HIGH) - pure value, ne WorldEvent
    ↓
audit/debug log (žádný EventBus)
```

Implementation: `52b46ff7` feat(ai-world): emit needs threshold events (2.6C)

Runtime ověřeno:

- `Hunger`/`SafetyPressure >= 0.80` → přesně jeden `HUNGER_CRITICAL`/`DANGER_HIGH` event.
- hodnota setrvává nad 0.80 → žádný další event (latch ACTIVE).
- hodnota klesne pod 0.60 → latch se rearms.
- další přechod přes 0.80 → nový event.
- žádný `WorldEvent`/`EventBus` publish - agentova vlastní potřeba není automaticky pozorovatelná ostatními.
- žádná threshold persistence, žádné nové config knoby.

> `NeedsThresholdState` je owned v `AgentRecord` vedle `NeedsState`, takže staticky přežije Creature unload/reload stejným mechanismem - tohle konkrétně jsme ale samostatným runtime testem (unload → reload → ověřit latch) neprošli, jen jsme ho odvodili z ownershipu.

## 2.7 Goal System

**Stav: 2.7A + 2.7B1 + 2.7B2 (deterministic Goal Candidate generation + selection/ActiveGoal + lifecycle outcomes) DONE / runtime PASS - deterministický Goal System základ je hotový**

První katalog cílů:

```text
SURVIVE
GET_FOOD
MAKE_MONEY
PROTECT_HOME
HELP_FAMILY
FLEE_DANGER
WORK
REST
INVESTIGATE
REQUEST_HELP
```

> 2.7A generuje kandidáty jen pro dva z nich (`GET_FOOD`, `FLEE_DANGER`) - zbytek katalogu čeká na runtime-ověřený mechanismus.

- [x] Definovat `Goal` objekt s prioritou/utility, zdrojem, timeoutem a success condition — **`ActiveGoal` má Type/Priority/Source/Utility/StartedAtMs/`TimeoutMs` (2.7B1 + `TimeoutMs` v 2.7B2); success/failure je terminal `GoalCompletion`/`GoalStatus` (2.7B2), ne pole na `ActiveGoal` samotném**.
- [x] Implementovat základní Utility AI pro volbu mezi jednoduchými cíli — **`GoalSystem::UpdateActiveGoal()` (2.7B1): deterministický `SelectBest` (priority rank → utility → fixed tie-break), nezávislý na pořadí vectoru**.
- [x] Oddělit volbu cíle od konkrétní akce — **GoalSystem zůstává pure-value; 2.8 nad jeho tranzicemi staví oddělené `ActionRequest` → `ActionSystem::Validate()` → `ActionExecutor` pipeline**.
- [x] Přidat možnost cíl přerušit při nouzové situaci — **emergency interruption (2.7B1): striktně vyšší priorita okamžitě přeruší aktivní goal, obráceně nikdy; kontrolováno před success/timeout (2.7B2), aby emergency nikdy nečekal jeden tick**.
- [x] Přidat success/failure stav — **`GoalStatus`/`GoalCompletionReason`/`GoalCompletion` (2.7B2): Need pod retention threshold → `SUCCEEDED reason=NEED_SATISFIED`; `TimeoutMs` uplynulý beze změny Need → `FAILED reason=TIMEOUT`**.
- [ ] LLM/GPU použít až pro komplexnější plánování nebo výběr mezi nestrukturovanými variantami — **současný 2.9 ai-server je stále deterministic stub; skutečný model zůstává pozdější krok**.

### Implementovaný stav 2.7A

```text
NeedsState (Hunger, SafetyPressure)
    ↓ level-triggered (>= 0.80, žádná candidate-side hysteresis)
GoalSystem::GenerateCandidates
    ↓
GoalCandidate[] (Type, Priority, Source, Utility) - pure value
```

Implementation: `e34ab2f9` feat(ai-world): generate deterministic goal candidates (2.7A)

Runtime ověřeno:

- `Hunger < 0.80` a `SafetyPressure < 0.80` → žádný candidate.
- `SafetyPressure >= 0.80` (combat nebo recent-memory danger) → `FLEE_DANGER`, `priority=EMERGENCY`, `utility` odpovídá `SafetyPressure`.
- memory decay pod 0.80 → `FLEE_DANGER` zmizí (level-triggered podle aktuálního stavu, ne latched).
- `Hunger >= 0.80` → `GET_FOOD`, `priority=NORMAL`, `utility` odpovídá `Hunger`.
- oba Needs současně nad threshold → oba candidates zároveň (`GET_FOOD NORMAL` + `FLEE_DANGER EMERGENCY`).
- candidate se generuje z aktuálního `NeedsState`, ne z `NeedsThresholdEvent` - zůstává i přes více ticků, kdy threshold event už znovu nevznikne.

### Implementovaný stav 2.7B1

```text
current ActiveGoal? + NeedsState + GoalCandidate[] + nowMs
    ↓
GoalSystem::UpdateActiveGoal (pure value, jeden transition za volání)
    ↓
žádný current            → Activate nejlepší candidate (nebo nic)
vyšší-priority candidate → Interrupt current okamžitě
current Need < 0.60      → Release current
jinak                    → beze změny (Transition::None)
    ↓
AgentRecord.ActiveGoalState + debug log jen skutečných transitions
```

Implementation: `a8221eda` feat(ai-world): select and retain active goals (2.7B1)
Hardening: `fe320b77` fix(ai-world): clear active goals while dead (2.7B1 P2)

Runtime ověřeno:

- `Hunger=1.0` → `ACTIVATED GET_FOOD`.
- `Hunger` zůstává `1.0` přes další ticky → žádná další `ACTIVATED`, `ActiveGoal` beze změny.
- combat → `SafetyPressure=1.0` → `INTERRUPTED GET_FOOD → FLEE_DANGER` (`EMERGENCY` vždy okamžitě přeruší `NORMAL`).
- combat skončí, `SafetyPressure` klesá: `0.84`/`0.79`/`0.61` → `FLEE_DANGER` drží (retention `>= 0.60`, nezávisle na tom, že candidate už na `0.79` zmizel).
- `SafetyPressure=0.59` → `RELEASED FLEE_DANGER`.
- další tick, `Hunger` stále `>= 0.80` → znovu `ACTIVATED GET_FOOD` (čerstvý `StartedAtMs`).
- selekce je deterministická a nezávislá na pořadí `GoalCandidate[]` (priority rank → utility → fixed `GoalType` tie-break).
- `NORMAL` candidate, byť s vyšší utilitou, nikdy nepřeruší aktivní `EMERGENCY` goal.
- **P2 fix:** dead agent nemá žádný candidate ani `ActiveGoal` - zmrazená `SafetyPressure`/`Hunger` z okamžiku smrti už negeneruje `FLEE_DANGER`/`GET_FOOD` a existující `ActiveGoal` se při smrti okamžitě uvolní (`RELEASED ... reason=DEAD`).
- respawn → Needs/candidates/`ActiveGoal` se znovu vyhodnocují z live stavu.

### Implementovaný stav 2.7B2

```text
current ActiveGoal? + NeedsState + GoalCandidate[] + nowMs
    ↓
1. žádný current            → Activate nejlepší candidate
2. vyšší-priority candidate → Interrupt current okamžitě (před 3. a 4.)
3. current Need < 0.60      → Succeeded (reason=NEED_SATISFIED)
4. elapsed >= TimeoutMs     → Failed (reason=TIMEOUT)
5. jinak                    → beze změny (Transition::None)
    ↓
AgentRecord.ActiveGoalState + debug log (SUCCEEDED/FAILED nesou reason a durationMs)
```

Implementation: `b2efbca2` feat(ai-world): add active goal lifecycle outcomes (2.7B2)

Runtime ověřeno:

- `FLEE_DANGER` active, `SafetyPressure` klesá na `0.61` → drží (stejné retention jako 2.7B1).
- `SafetyPressure=0.59` → `SUCCEEDED FLEE_DANGER reason=NEED_SATISFIED`.
- `GET_FOOD` active, `Hunger=1.0` beze změny 30 s → `FAILED GET_FOOD reason=TIMEOUT`.
- další tick, `Hunger` stále `>= 0.80` → nový `ACTIVATED GET_FOOD` s čerstvým `StartedAtMs`. Historicky v 2.7B2 ještě Action API nebylo; od 2.8E–2.8G už GET_FOOD umí Hunger skutečně uspokojit přes MOVE_TO → EAT.
- `GET_FOOD` active + combat → `INTERRUPTED GET_FOOD → FLEE_DANGER` funguje beze změny; interrupt má přednost i kdyby `GET_FOOD` zároveň splňoval success/timeout ten samý tick.
- death s `ActiveGoal` → `RELEASED reason=DEAD` beze změny.
- dead ticks → žádné candidates.

**Deterministický 2.7 Goal System základ (2.7A + 2.7B1 + 2.7B2) je DONE / runtime PASS a dnes napájí runtime-ověřenou Action API pipeline 2.8.**

## 2.8 Bezpečné Action API

**Stav: 2.8A + 2.8A.5 + 2.8B + 2.8C + 2.8D + 2.8E + 2.8F + 2.8G DONE / runtime PASS pro `FLEE`, `MOVE_TO` a `EAT`; `GET_FOOD` je plně uzavřený dvoufázový cyklus**

AI nemá přístup k libovolnému C++ ani k přímým zápisům do světa. Smí pouze požádat o akci z povoleného katalogu.

```text
MOVE_TO
FOLLOW
ATTACK
FLEE
TALK
TRADE
EAT
SLEEP
WORK
INVESTIGATE
REQUEST_HELP
```

> Runtime ověřeno: `FLEE`, `MOVE_TO` a `EAT`. `GET_FOOD` (2.8E) dojde k target souřadnicím (2.8F) a skutečně sní (2.8G) - `Hunger` klesne, následující tick `GET_FOOD` dokončí jako `SUCCEEDED reason=NEED_SATISFIED`.

- [x] Definovat `ActionRequest` a `ActionResult` — **`ActionRequest` (2.8A, rozšířena o `Destination` v 2.8D); `ActionResult` jako vlastní execution DTO (`ActionExecutionStatus`/`ActionExecutionReason`, 2.8C)**.
- [x] Pro každou implementovanou akci serverová validace — **hotovo pro `FLEE`, `MOVE_TO`, `EAT`; další katalogové typy zatím nejsou implementované**.
- [x] Ověřit existenci cíle, stav agenta, pathing/range/LoS podle typu akce — **stav agenta (`Materialized`/`Alive`) validuje `ActionSystem`; FLEE/MOVE_TO/EAT mají vlastní target/provenance/range pravidla; pathing deleguje TrinityCore**.
- [x] Nevalidní AI odpověď nikdy nesmí rozbít stav serveru — **REJECTED nikdy nedosáhne engine execution; 2.9C navíc remote FLEE intent překládá z authoritative world-thread dat a validuje pouze dry-run**.
- [ ] Přidat obecný timeout/cancel mechanismus pro dlouhé akce — **FLEE dnes končí přes goal-level timeout; MOVE_TO má lifecycle/reconciliation; jednotný per-action timeout/cancel stále neexistuje**.
- [x] Fallback při chybě AI služby — **async `/decision` je advisory/dry-run a stávající deterministic Goal→Action pipeline pokračuje bez AI služby; runtime fault test potvrzen**.
- [x] Každá provedená AI akce musí být auditovatelná: kdo ji navrhl, proč, s jakým contextem a výsledkem validace — **debug log na request/validation/execution/completion/stop hranicích**.

> **Pravidlo:** AI navrhuje. `ActionSystem` validuje. TrinityCore provádí.

### Implementovaný stav 2.8A + 2.8A.5 + 2.8B

```text
FLEE_DANGER Activated/Interrupted
    ↓
ActionRequest (pure value: Actor, Type, SourceGoal, GoalStartedAtMs, FleeFromGuid)
    ↓
ActionSystem::Validate
    ↓
ALLOWED
    ↓
ActionExecutor::ExecuteFlee (world thread)
    ↓
Creature::GetMotionMaster()->MoveFleeing
```

Souběžně (2.8A.5): `FactorySelector::SelectAI()` vrací `AIWorldCreatureAI` jen pro `sAIWorldMgr->OwnsSpawn()` shodu - default `CreatureAI` auto-aggro/chase/melee/DB movement je pro tyto NPC potlačené, aby AIWorld pipeline nesoutěžila s TrinityCore default AI o stejné rozhodnutí. Combat/threat/damage/death bookkeeping v TrinityCore zůstává nedotčené.

Implementation: `231f42f0` feat(ai-world): add safe action request validation scaffold (2.8A)
Implementation: `08fe8ace1` feat(ai-world): isolate controlled creatures from default AI behavior (2.8A.5)
Implementation: `752edcca3` feat(ai-world): execute validated flee actions through TrinityCore (2.8B)
Hardening: `8808a105d` fix(ai-world): stop only active flee movement (2.8B P2)

Runtime ověřeno:

- takeover je scoped přes `OwnsSpawn(MapId, SpawnId)`; ostatní NPC pokračují původní cestou.
- `ACTIVATED`/`INTERRUPTED` do `FLEE_DANGER` → `ActionRequest FLEE` → `ALLOWED` → `EXECUTED` → NPC se fyzicky rozběhne pryč od `fleeSource`.
- `inCombat=true` zůstává pravda po dobu útěku.
- `SafetyPressure < 0.60` → `SUCCEEDED FLEE_DANGER` → scoped stop → NPC idle.
- timeout goalu → flee movement odstraněn.
- `StopFlee()` maže pouze vlastní FLEE movement a neruší cizí movement.

**Otevřené P3 z review, neblokující:** `FleeSourceMismatch` je na současných deterministic call-sitech stále prakticky tautologický; memory-only `FLEE_DANGER` bez current threat victim může skončit `NO_FLEE_SOURCE` bez retry v témže goal attempt.

### Implementovaný stav 2.8C

`ActionExecutor::ExecuteFlee` vrací pure-value `ActionResult` místo bool.

Implementation: `e1599bf5` feat(ai-world): add structured action execution result (2.8C)

### Implementovaný stav 2.8D

`MOVE_TO` používá `ActionRequest.Destination`, `ValidateMoveTo`, TrinityCore `MovePoint`, vlastní `MovePointId` a scoped `StopMoveTo`.

Implementation: `3e71497a` feat(ai-world): add validated move-to action primitive (2.8D)
Hardening: `1b183fe2` fix(ai-world): reject move-to while actor movement is busy (2.8D P2)
Hardening: `f0b96b0b` fix(ai-world): cancel move-to on emergency goal interrupt (2.8D P2)

### Implementovaný stav 2.8E

`GET_FOOD Activated` → `FoodTargetResolver` → `GoalTarget` → `ActionRequest MOVE_TO` → validate → execute. Fixed test target je jediný source; generic TestMoveTo trigger byl odstraněn.

Implementation: `8e64b907` feat(ai-world): resolve food target into move-to action (2.8E)
Hardening: `5f41d80b` fix(ai-world): stop move-to when get-food goal completes (2.8E P2)

### Implementovaný stav 2.8F

```text
MOVE_TO Started
    ↓
TrinityCore movement
    ↓
MovementInform → ActionEngineEventBus
    ↓
world-thread hard validation + arrival tolerance/reconciliation
    ↓
ActionCompletion
```

Implementation: `4414c0d8` feat(ai-world): track move-to arrival and action completion (2.8F)
Hardening: `873b9d1f` fix(ai-world): harden move-to completion lifecycle (2.8F P2)
Docs: `16ff6102` docs(ai-world): tighten stale action-comment references (2.8F P3)

Runtime ověřeno: arrival je ověřená podle skutečné pozice, death zavře active action, reconciliation pokrývá dropped event/engine stop. Arrival samo o sobě nemění Hunger.

### Implementovaný stav 2.8G

```text
MOVE_TO ARRIVED (GET_FOOD)
    ↓
PendingEat (pure DTO)
    ↓
aktuální goal-selection pass
    ↓
exact provenance stále platí?
    ↓ ano
EAT validate → execute → CONSUMED
    ↓
NeedsSystem::SatisfyHunger
    ↓
GET_FOOD SUCCEEDED/NEED_SATISFIED
```

Implementation: `be081a0c` feat(ai-world): continue get-food with eat action (2.8G)
Hardening: `1a00efa3` fix(ai-world): enforce eat continuation safety (2.8G P2)

Runtime ověřeno včetně emergency regression: same-tick FLEE_DANGER interruption zruší PendingEat a emergency vždy vyhraje.

**Etapa 2 má `FLEE`, `MOVE_TO` a `EAT` runtime-verified přes celou `Needs → Goal → Action → TrinityCore` feedback loop.**

**Otevřené P3:** exact float equality v EAT continuation je dnes bezpečná kvůli stejné DTO kopii, ale budoucí serialize/deserialize by mohlo false-rejectnout; starší cancellation paths nejsou všechny sjednocené přes jeden `ActionCompletion` entry point.

## 2.9 AI server — decision protocol

**Stav: 2.9A + 2.9B + 2.9C + 2.9D + 2.9E DONE. 2.9A/2.9B/2.9E runtime PASS; 2.9C translation/validation je staticky PASS a transport runtime PASS, detailní DEBUG evidence validačního seam nebyla samostatně zachycena; 2.9D metrics kód je static PASS + runtime pipeline PASS, ale Influx/metrics backend evidence je neblokující pending.**

```text
AgentContext
    │
    ▼
DecisionRequest V2
    │ async
    ▼
ai-server deterministic policy
    │
    ▼
structured DecisionIntent
    │
    ▼
world-thread stale/provenance checks
    │
    ▼
authoritative ActionRequest translation
    │
    ▼
ActionSystem::Validate
    │
    └── remote decision path je zatím dry-run; ActionExecutor se zde nevolá
```

- [x] Definovat verzovaný request/response kontrakt pro plný `AgentContext` — **V2; `DecisionRequest`/`DecisionResponse`, strict protocol gate a korelace**.
- [ ] Posílat jen informace, které agent smí znát — **memory DTO jsou sanitizované (`Entry` + `AgentId`, bez live pointerů/raw memory actor GUIDů), ale `Self` wire context stále obsahuje interní `spawn_id`; před externím/LLM providerem znovu projít privacy/minimal-knowledge boundary**.
- [x] Posílat dostupné akce explicitně — `AvailableActions` je capability catalog, nikdy ne náhrada za `ActionSystem::Validate()`.
- [x] Rozhodování volat asynchronně — deterministic `/decision` V2 stub.
- [x] `worldserver` nesmí čekat na inference.
- [x] Přidat `request_id`, `agent_id` a snapshot sequence pro korelaci.
- [x] Přidat stale/context provenance ochranu — exact response correlation + request-time Goal/GoalStartedAtMs/RuntimeGuid provenance; nový runtime Creature incarnation nebo změněný goal attempt odpověď zahodí.
- [x] Měřit latency, queue time, timeout/outcome a invalid decision rate jako skutečné low-cardinality metriky — **2.9D; backend runtime evidence pending**.
- [x] Připravit API pro batching více agentů — **2.9E `SubmitDecisions(vector<AIRequest>) -> vector<DecisionSubmitResult>`; transport zatím stále single `/decision`, skutečné batching/backpressure přijde v 2.10**.
- [x] Při nedostupnosti AI služby zachovat běžící svět/deterministic fallback.

### Implementovaný stav 2.9A — full AgentContext protocol

`DecisionRequest` nese versioned `AgentContext`: Self snapshot, Needs, ActiveGoal, Top-N `DecisionMemory`, explicitní available actions. Mapping probíhá na world threadu do pure DTO; žádný live `Creature*`/`Map*`/`Unit*` nepřechází async hranici.

Implementation: `a0775724` feat(ai-world): add versioned decision context protocol (2.9A)
Hardening: `fac8b2ac` fix(ai-world): harden decision context protocol (2.9A P2)

P2 hardening doplnil plnou memory semantic lokaci/actor/target wire informaci a strict server/current protocol gate.

### Implementovaný stav 2.9B — structured deterministic DecisionResponse

V2 response:

```json
{
  "protocol_version": 2,
  "request_id": 42,
  "agent_id": 1,
  "snapshot_sequence": 8,
  "decision": { "type": "FLEE" }
}
```

Policy: `FLEE` jen když request-time ActiveGoal je `FLEE_DANGER` a FLEE je v `AvailableActions`; jinak `NONE`. `GET_FOOD` zůstává deterministic worldserver-owned MOVE_TO → EAT pipeline.

Implementation: `520b87cf` feat(ai-world): return structured deterministic decisions (2.9B)
Hardening: `f20a0f0e` fix(ai-world): fix decision protocol compatibility (2.9B P2)

P2 fix odstranil Boost.JSON dependency nekompatibilní s Ubuntu 22.04/Boost 1.74 a bumpnul nekompatibilní wire shape na Protocol V2.

**Známý P3:** současný C++ response parser je ručně psaný strict parser, ne obecný JSON parser. Před skutečným external/LLM execution trust boundary ho nahradit/hardenovat. Python `DecisionIntent.type` je zatím `str`; C++ neznámý typ fail-closed odmítá.

### Implementovaný stav 2.9C — world-thread DecisionIntent validation seam

`DecisionIntent::FLEE` se na world threadu přeloží do `ActionRequest` výhradně z authoritative aktuálních dat: ActiveGoal/StartedAtMs + fresh current threat victim. Request-time provenance navíc chrání proti změně goal attempt i proti despawn/respawn nové runtime Creature instance stejného SpawnId.

Implementation: `590577af` feat(ai-world): validate decision intents on world thread (2.9C)
Hardening: `c2d053a4` fix(ai-world): reject stale creature decision provenance (2.9C P2)

`NONE` je no-op; remote `MOVE_TO`/`EAT` jsou fail-closed unsupported. FLEE response path končí na `ActionSystem::Validate()` + audit log. **ActionExecutor se zde záměrně nevolá**, protože skutečný FLEE stále vlastní deterministic Goal→Action pipeline; tím nevznikají dva vlastníci stejného movementu.

### Implementovaný stav 2.9D — decision metrics

Používá existující TrinityCore `Metric.h`/`TC_METRIC_VALUE`, žádnou paralelní telemetry vrstvu.

Series:

```text
ai.world.decision.submit
ai.world.decision.queue_ms
ai.world.decision.latency_ms
ai.world.decision.result
ai.world.decision.discard
ai.world.decision.validation
ai.world.decision.validity
```

`validity` sjednocuje skutečný invalid-decision denominator: fresh NONE/FLEE ALLOWED = valid; malformed/unsupported/FLEE REJECTED = invalid; stale/discard ani čisté transport chyby do denominatoru nevstupují.

Implementation: `2f357a2b` feat(ai-world): add decision pipeline metrics (2.9D)
Hardening: `b149cc06` fix(ai-world): unify decision validity metrics (2.9D P2)

**Runtime poznámka:** instrumentovaná decision pipeline běží, ale TrinityCore metrics jsou defaultně vypnuté a posílají se přes `Metric.ConnectionInfo` do InfluxDB; samostatný backend evidence test zatím neproběhl.

### Implementovaný stav 2.9E — multi-agent submit API scaffold

```text
vector<AIRequest>
    ↓
AIWorldMgr::SubmitDecisionContexts
    ↓
AIClient::SubmitDecisions
    ↓
vector<DecisionSubmitResult { Agent, RequestId, Status }>
```

Implementation: `7de5daee` feat(ai-world): add multi-agent decision submit API (2.9E)

API je batch-shaped, ale záměrně stále deleguje jednotlivé položky do existujícího `SubmitDecision()` a zachovává globální `DecisionInFlight`. Žádný `/decision/batch`, žádný Protocol V3, žádný scheduler. Runtime single-agent smoke PASS přes nový seam.

**Otevřený P3 pro 2.10:** `AIWorldMgr::SubmitDecisionContexts()` dnes vrací `void` a zahazuje per-agent `DecisionSubmitResult`; scheduler by měl admission outcome skutečně spotřebovat. Globální `DecisionInFlight` je současný bottleneck, který má 2.10 nahradit bounded politikou.

**Další implementace:** `2.10A — scheduler/admission model`.

## 2.10 Scheduler a úrovně simulace

**Stav: NEXT — 2.10A scheduler/admission model. 2.9E už poskytuje batch-shaped caller API a 2.9D baseline metriky `submitted/skipped_in_flight`; skutečná scheduler cadence, per-agent priority a bounded backpressure zatím nejsou implementované.**

| Stav agenta | Orientační cadence | Účel |
|---|---:|---|
| Combat / reflex | 50–200 ms | rychlá pravidla na CPU; žádné drahé LLM |
| Vedle hráče | 250–1000 ms | reakce a krátkodobé taktické volby |
| Aktivní NPC | 2–5 s | cíle, plánování, běžný život |
| Background agent | 30–120 s | agregované změny stavu |
| Abstraktní skupina | minuty | populace, zdroje, teritorium |

- [ ] Implementovat scheduler tak, aby AI neměla jeden globální tick pro všechny entity.
- [ ] Prioritizovat agenty poblíž reálného hráče.
- [x] Drahou inference nikdy nepouštět v combat hot path — současná inference cesta je async a oddělená od gameplay execution.
- [ ] Připravit bounded backpressure/admission při přetížení AI queue — **dnešní globální `DecisionInFlight` je jen bezpečný baseline, ne cílová politika**.
- [ ] Umět přesunout agenta mezi `ACTIVE`, `NEARBY`, `BACKGROUND` a `ABSTRACT` simulačním tierem.

> Agent registry už rozlišuje materialized vs. abstract world binding bez force-load gridů. Scheduler musí pracovat jen s registry/value stavem a live TrinityCore objekty resolveovat pouze lokálně na world threadu pro konkrétní materialized práci.

## 2.11 První experiment — persistentní farmář

Vybrat jedno NPC v malé testovací oblasti a dát mu jednoduchý, pozorovatelný denní cyklus.

- [ ] Agent má home lokaci.
- [ ] Agent má working lokaci.
- [ ] Agent má money/food/resource stav.
- [x] Agent má potřeby a základní cíle — **Needs + GET_FOOD/FLEE_DANGER mechanismus existuje; farmer-specific WORK/REST/home model ještě ne**.
- [ ] Ráno jde pracovat.
- [x] Při nebezpečí umí utéct — **FLEE_DANGER → validated FLEE runtime PASS; REQUEST_HELP ještě ne**.
- [ ] Večer se vrátí domů a odpočívá.
- [x] Pamatuje si jednu důležitou událost i po restartu serveru — mechanismus long-term memory runtime ověřen na testovacím agentovi.

## 2.12 První experiment — wolf pack

```text
WolfPack #1
├── population
├── territory
├── hunger
├── fear
├── home
└── current_goal
```

- [ ] Modelovat smečku jako skupinového agenta mimo bezprostřední combat.
- [ ] Hlad postupně roste.
- [ ] Smečka vyhledává potravu podle dostupných zdrojů.
- [ ] Při nedostatku potravy zvyšuje toleranci rizika.
- [ ] Smečka se může přiblížit k farmě.
- [ ] Při materializaci použít normální TrinityCore `Creature` / combat logiku.
- [ ] Po ztrátě členů aktualizovat `population` state.

## 2.13 První emergentní end-to-end událost

Etapa 2 má skončit prvním skutečným kauzálním řetězcem, který nebyl napsán jako klasický quest.

```text
WolfPack hunger rises
        │
        ▼
wolves move toward farm
        │
        ▼
livestock is attacked
        │
        ▼
WorldEvent: LIVESTOCK_KILLED
        │
        ▼
Farmer perceives event
        │
        ▼
Memory is created
        │
        ▼
Goal: PROTECT_HOME
        │
        ▼
Decision: REQUEST_HELP
        │
        ▼
validated TrinityCore action
```

## 2.14 Testy a diagnostika

- [ ] Unit testy pro utility výběr cílů.
- [ ] Unit testy validace `ActionRequest`.
- [ ] Unit testy serializace/persistence agenta.
- [ ] Integration test AI request → mock response → action result.
- [ ] Integration test timeout/stale response → deterministic fallback.
- [x] Integration/runtime restart → reload memory.
- [x] Structured/audit log pro decision request/response a Action validation seam.
- [ ] Možnost vypsat debug snapshot jednoho agenta podle `AgentId`.
- [ ] Metrics backend/dashboard runtime verification pro již emitované 2.9D decision series.
- [ ] Další metrics:
  - agent count podle tieru,
  - event rate,
  - scheduler/inference queue depth,
  - stale/discard breakdown dashboard,
  - invalid actions dashboard.

## Etapa 2 — Definition of Done

- [x] `AIWorldMgr` se stabilně inicializuje a ukončuje s `worldserver`.
- [x] `AIWorldMgr` lze vypnout a vanilla gameplay funguje beze změny.
- [x] Async `AIClient` nikdy neblokuje world update loop a má timeout/fallback/stale-response ochranu.
- [x] Existuje persistentní `AgentId` a `AgentRegistry`.
- [x] Minimálně jeden NPC agent přežije restart se zachovanou persistentní identitou a long-term pamětí.
- [x] `WorldEvent` system zachytí vybranou událost z TrinityCore.
- [x] Perception určí, který agent událost viděl.
- [x] `MemorySystem` z události vytvoří a retrievuje relevantní vzpomínku.
- [x] Needs/Goal system vytvoří/upraví cíl (`GET_FOOD`, `FLEE_DANGER`) a lifecycle je runtime ověřený.
- [x] AI server obdrží plný aktuální `AgentContext` (Top-N memories + Needs + ActiveGoal + available actions) asynchronně a vrátí structured deterministic decision V2.
- [ ] Remote AI decision převezme execution ownership bezpečně přes `ActionSystem` — **dnes se FLEE intent authoritative překládá a validuje dry-run; skutečný FLEE dál vlastní deterministic Goal→Action pipeline, aby nebyli dva vlastníci**.
- [x] Deterministic Goal→Action pipeline validuje a provádí pouze povolené FLEE/MOVE_TO/EAT akce přes TrinityCore.
- [x] Výpadek AI serveru nezablokuje `worldserver`.
- [ ] Scheduler umí různé update cadence, bounded admission/backpressure a abstraktní agent state.
- [ ] Wolf pack → livestock attack → farmer memory → protect goal → request help funguje end-to-end.

> **Gate:** po Etapě 2 máme skutečnou smyčku  
> `WORLD STATE → EVENT → PERCEPTION → MEMORY → NEED → GOAL → DECISION → ACTION → WORLD STATE`.

---

# Doporučené pořadí implementace

## Hotovo / runtime ověřeno

1. [x] Git/fork/upstream základ
2. [x] Docker host + NVIDIA runtime
3. [x] Development image + persistentní build / `ccache`
4. [x] Compose: MySQL + `authserver` + `worldserver` + `ai-server`
5. [x] DB bootstrap + TDB import
6. [x] WoW game data mount a worldserver startup
7. [x] Realm/LAN networking + vzdálený klient
8. [x] Restart/persistence smoke test
9. [x] AIWorldMgr lifecycle + log-only snapshot jednoho NPC
10. [x] Async AIClient health/decision smoke + timeout/fallback/stale protection
11. [x] `AgentRegistry` + stabilní `AgentId`
12. [x] Persistence jednoho agenta
13. [x] World Event System — první runtime producer
14. [x] Perception System — witnessed event + nearby Player/Creature
15. [x] ShortTermMemory — dedupe + TTL + expiry
16. [x] Importance + persistent LongTermMemory
17. [x] Relevant-memory retrieval — deterministic ST/LT Top N
18. [x] Needs System 2.6A–2.6C
19. [x] Goal System 2.7A–2.7B2
20. [x] Bezpečné Action API 2.8A–2.8G — FLEE/MOVE_TO/EAT runtime loop
21. [x] Decision protocol 2.9A–2.9E — AgentContext V2, structured intent, stale/provenance validation, metrics, multi-agent submit API

## Teď

22. [ ] **2.10A Scheduler / admission / bounded backpressure**

## Následuje

23. [ ] Scheduler simulation tiers (`ACTIVE`/`NEARBY`/`BACKGROUND`/`ABSTRACT`)
24. [ ] Persistentní farmář
25. [ ] Wolf pack
26. [ ] End-to-end emergentní událost

## Paralelní hardening

- [ ] Debugger/core dump workflow.
- [ ] Přesný extraction dokument.
- [ ] CUDA compute smoke test.
- [ ] Aktualizovat zastaralý status v `README_DEV.md`.
- [ ] Metrics backend/dashboard runtime evidence pro 2.9D.
- [ ] Nahradit/hardenovat ruční V2 JSON response parser před skutečným external/LLM execution.
- [ ] Zúžit externí DecisionContext privacy boundary (zejména interní `spawn_id`) před external/LLM providerem.
- [ ] Centralizovat semantic identity helper pro širší multi-agent memory scénáře, až bude potřeba.
- [ ] Sjednotit starší action cancellation paths přes strukturovaný `ActionCompletion`, až to přinese praktický benefit.

## Co bude následovat

**Etapa 3:** jedna živá oblast (například Elwynn Forest) s populacemi, zdroji, ekonomikou, vztahy, frakcemi a dynamickými problémy/questy.