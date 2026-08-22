# AI TrinityCore — Roadmap

> **Výchozí stav:** TrinityCore `3.3.5` + Ubuntu Server + NVIDIA GPU  
> **Rozsah dokumentu:** Etapa 1 — Development Infrastructure, Etapa 2 — AI World Foundation  
> **Aktualizováno:** 2026-08-22  
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

Etapa 2 má runtime ověřený základ až po Needs threshold events (2.6C):

- `AIWorldMgr` lifecycle + read-only snapshot.
- async `AIClient` `/health` + `/decision` stub, timeout/fallback a stale-response ochrana.
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

**Aktuální NEXT:** `2.7A — deterministic Goal Candidate generation` (předstupeň před `2.7 Goal System`).

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
| **2 — AI World Foundation** | 🟡 **IN PROGRESS — NEXT 2.7A** | Persistentní agenti, události, paměť, cíle, Action API a async AI bridge | Wolf attack → memory → goal → decision → validated action |

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

**Stav: DONE / runtime PASS; detailní metriky zůstávají observability hardening.**

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
- [ ] Kompletní observability: `request_id`, latency, timeout a response status jako strukturované metriky.
- [x] Vypnout `ai-server` během běžícího světa a ověřit, že `worldserver` pokračuje bez blokace/crashe.

### Gate pro další práci

**SPLNĚNO:** 2A + 2B jsou runtime ověřené; práce pokračovala přes persistence, events, perception a memory.

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
- [x] Každá externí decision odpověď má deadline a kontrolu freshness.

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
- [ ] LLM nikdy neposílat celou historii; posílat pouze vybrané relevantní záznamy — **retrieval Top-N je připraven, ale decision request jej zatím nepřenáší**.

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
- `/decision` request zatím zůstává beze změny; retrieval je zatím diagnostický vstup pro další Needs/Goals/DecisionContext práci.

## 2.6 Needs System

**Stav: 2.6A + 2.6B1 + 2.6B2 + 2.6C (NeedsState + deterministic drift + live world-state coupling + recent-memory safety + threshold events) DONE / runtime PASS**

- [x] Zavést minimální potřeby: `health`, `hunger`, `fatigue`, `safety`, `money/resource pressure` — **`NeedsState` zavedena se všemi pěti poli; `health`/`safety` pressure jsou odvozené z live Creature stavu (2.6B1) s recent-memory-driven fallback po skončení combatu (2.6B2)**.
- [x] Potřeby aktualizovat deterministicky v simulation ticku — `hunger`/`fatigue`/`resource pressure` drift na vlastní ~1s cadence, nezávislé na snapshot cadence.
- [x] Potřeby omezit do definovaného rozsahu `0.0–1.0`.
- [x] Přidat threshold events, například `HUNGER_CRITICAL` nebo `DANGER_HIGH` — **edge-triggered, hysteresis latch, audit/debug log only (2.6C); katalog zatím jen `HUNGER_CRITICAL`/`DANGER_HIGH`**.
- [ ] Nechat potřeby generovat kandidáty na cíle bez LLM — **2.7A (deterministic Goal Candidate generation, plánovaný předstupeň před plným 2.7 Utility AI)**.

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
- `/decision` beze změny.

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
- žádná world mutation / DB / `/decision` změna.

> Otevřená sémantická otázka pro 2.7: při `dead` zůstává `SafetyPressure` zmražená na poslední hodnotě (ne resetovaná na 0). Neblokující pro 2.6B1/2.6B2, ale je potřeba ji definitivně rozhodnout, než Goals začnou `SafetyPressure` číst.

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
- žádný nový config; žádná world mutation / DB / `/decision` změna.

### Implementovaný stav 2.6C

```text
NeedsState (Hunger, SafetyPressure)
    ↓
NeedsSystem::EvaluateThresholds (per-agent NeedsThresholdState latch)
    ↓ enter >= 0.80 (jednou), reset < 0.60 (rearm)
NeedsThresholdEvent (HUNGER_CRITICAL / DANGER_HIGH) - pure value, ne WorldEvent
    ↓
audit/debug log (žádný EventBus, žádný Goal System, žádná /decision změna)
```

Implementation: `52b46ff7` feat(ai-world): emit needs threshold events (2.6C)

Runtime ověřeno:

- `Hunger`/`SafetyPressure >= 0.80` → přesně jeden `HUNGER_CRITICAL`/`DANGER_HIGH` event.
- hodnota setrvává nad 0.80 → žádný další event (latch ACTIVE).
- hodnota klesne pod 0.60 → latch se rearms.
- další přechod přes 0.80 → nový event.
- žádný `WorldEvent`/`EventBus` publish - agentova vlastní potřeba není automaticky pozorovatelná ostatními.
- žádný Goal System, žádný Action API, žádná `/decision` změna.
- žádná threshold persistence, žádné nové config knoby.

> `NeedsThresholdState` je owned v `AgentRecord` vedle `NeedsState`, takže staticky přežije Creature unload/reload stejným mechanismem - tohle konkrétně jsme ale samostatným runtime testem (unload → reload → ověřit latch) neprošli, jen jsme ho odvodili z ownershipu.

**Další implementace:** `2.7A — deterministic Goal Candidate generation` (`NeedsState` → `GoalCandidate[]`, zatím bez LLM a bez Action API; předstupeň před plným `2.7 Goal System`/Utility AI).

## 2.7 Goal System

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

- [ ] Definovat `Goal` objekt s prioritou/utility, zdrojem, timeoutem a success condition.
- [ ] Implementovat základní Utility AI pro volbu mezi jednoduchými cíli.
- [ ] Oddělit volbu cíle od konkrétní akce.
- [ ] Přidat možnost cíl přerušit při nouzové situaci.
- [ ] Přidat success/failure stav.
- [ ] LLM/GPU použít až pro komplexnější plánování nebo výběr mezi nestrukturovanými variantami.

## 2.8 Bezpečné Action API

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

- [ ] Definovat `ActionRequest` a `ActionResult`.
- [ ] Pro každou akci implementovat serverovou validaci.
- [ ] Ověřit existenci cíle, stav agenta, pathing/range/LoS podle typu akce.
- [ ] Nevalidní AI odpověď nikdy nesmí rozbít stav serveru.
- [ ] Přidat timeout a cancel pro dlouhé akce.
- [ ] Přidat fallback behavior při chybě AI služby.
- [ ] Každá provedená AI akce musí být auditovatelná: kdo ji navrhl, proč, s jakým contextem a výsledkem validace.

> **Pravidlo:** AI navrhuje. `ActionSystem` validuje. TrinityCore provádí.

## 2.9 AI server — decision protocol

```text
AgentContext
    │
    ▼
DecisionRequest
    │
    ▼
ai-server / GPU
    │
    ▼
Decision
    │
    ▼
Action validation
```

- [ ] Definovat finální verzovaný request/response kontrakt pro plný `AgentContext`.
- [ ] Posílat jen informace, které agent smí znát.
- [ ] Posílat dostupné akce explicitně.
- [x] Rozhodování volat asynchronně — současný deterministic `/decision` stub.
- [x] `worldserver` nesmí čekat na inference.
- [x] Přidat `request_id`, `agent_id` a state/snapshot token pro korelaci a stale kontrolu současného stubu.
- [x] Přidat state/version token pro detekci stale inference odpovědi v současném snapshot flow.
- [ ] Měřit latency, queue time, timeout rate a invalid decision rate jako skutečné metriky.
- [ ] Připravit API pro batching více agentů.
- [x] Při nedostupnosti AI služby použít deterministic fallback / zachovat běžící svět.

> Současný `/decision` je bezpečný no-op stub (`NONE`) a neprovádí world mutation. Finální DecisionContext s Needs/Goals/Top-N memories přijde až po příslušných subsystemech.

## 2.10 Scheduler a úrovně simulace

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
- [ ] Připravit backpressure při přetížení AI queue.
- [ ] Umět přesunout agenta mezi `ACTIVE`, `NEARBY`, `BACKGROUND` a `ABSTRACT` simulačním tierem.

> Agent registry už rozlišuje materialized vs. abstract world binding bez force-load gridů; plný scheduler/background simulation cadence je ale stále otevřený milestone.

## 2.11 První experiment — persistentní farmář

Vybrat jedno NPC v malé testovací oblasti a dát mu jednoduchý, pozorovatelný denní cyklus.

- [ ] Agent má home lokaci.
- [ ] Agent má working lokaci.
- [ ] Agent má money/food/resource stav.
- [ ] Agent má potřeby a základní cíle.
- [ ] Ráno jde pracovat.
- [ ] Při nebezpečí uteče nebo požádá o pomoc.
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
- [ ] Integration test restart → reload memory.
- [ ] Structured log pro každé AI decision.
- [ ] Možnost vypsat debug snapshot jednoho agenta podle `AgentId`.
- [ ] Metrics:
  - agent count podle tieru,
  - event rate,
  - inference queue,
  - latency,
  - timeouts,
  - stale responses,
  - invalid actions.

## Etapa 2 — Definition of Done

- [x] `AIWorldMgr` se stabilně inicializuje a ukončuje s `worldserver`.
- [x] `AIWorldMgr` lze vypnout a vanilla gameplay funguje beze změny.
- [x] Async `AIClient` nikdy neblokuje world update loop a má timeout/fallback/stale-response ochranu.
- [x] Existuje persistentní `AgentId` a `AgentRegistry`.
- [x] Minimálně jeden NPC agent přežije restart se zachovanou persistentní identitou a long-term pamětí.
- [x] `WorldEvent` system zachytí vybranou událost z TrinityCore.
- [x] Perception určí, který agent událost viděl.
- [x] `MemorySystem` z události vytvoří a retrievuje relevantní vzpomínku.
- [ ] Needs/Goal system vytvoří nebo upraví cíl.
- [ ] AI server obdrží plný `AgentContext` (Top-N memories + Needs + Goals) asynchronně a vrátí rozhodnutí.
- [ ] `ActionSystem` rozhodnutí validuje a provede pouze povolenou akci.
- [x] Výpadek AI serveru nezablokuje `worldserver`.
- [ ] Scheduler umí různé update cadence a abstraktní agent state.
- [ ] Wolf pack → livestock attack → farmer memory → protect goal → request help funguje end-to-end.

> **Gate:** po Etapě 2 máme skutečnou smyčku  
> `WORLD STATE → EVENT → PERCEPTION → MEMORY → GOAL → DECISION → ACTION → WORLD STATE`.

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

## Teď

18. [ ] **Needs System**

## Následuje

19. [ ] Goal System
20. [ ] Bezpečné Action API
21. [ ] Decision context/protocol — Top-N memories + Needs + Goals
22. [ ] Scheduler / simulation tiers
23. [ ] Persistentní farmář
24. [ ] Wolf pack
25. [ ] End-to-end emergentní událost

## Paralelní hardening

- [ ] Debugger/core dump workflow.
- [ ] Přesný extraction dokument.
- [ ] CUDA compute smoke test.
- [ ] Aktualizovat zastaralý status v `README_DEV.md`.
- [ ] Observability metriky pro inference/decision path.
- [ ] Centralizovat semantic identity helper pro širší multi-agent memory scénáře, až bude potřeba.

## Co bude následovat

**Etapa 3:** jedna živá oblast (například Elwynn Forest) s populacemi, zdroji, ekonomikou, vztahy, frakcemi a dynamickými problémy/questy.