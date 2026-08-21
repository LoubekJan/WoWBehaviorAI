# AI TrinityCore — Roadmap

> **Výchozí stav:** TrinityCore `3.3.5` + Ubuntu Server + NVIDIA GPU  
> **Rozsah dokumentu:** Etapa 1 — Development Infrastructure, Etapa 2 — AI World Foundation  
> **Aktualizováno:** 2026-08-21  
> **Aktivní větev:** `ai-world`

## Stav projektu

**Etapa 1 má splněný runtime gate a neblokuje zahájení Etapy 2.**

Na reálném Ubuntu/GPU hostu bylo ověřeno:

- TrinityCore se v Dockeru nakonfiguruje, zkompiluje a nainstaluje do persistentního `/build` volume.
- MySQL se inicializuje a TDB `TDB335.25101` se stáhne/importuje automatizovaným workflow.
- `authserver`, `worldserver`, MySQL a `ai-server` běží přes Compose.
- extrahovaná `dbc/maps/vmaps/mmaps` data jsou připojena do `worldserver`.
- NVIDIA Container Toolkit funguje a Docker vidí dvě RTX 3090.
- WoW 3.3.5a klient na jiném PC v LAN projde authserverem, realm listem a připojí se na worldserver.
- `auth.realmlist` je konfigurován versionovaným `make configure-realm`, nikoli ručním SQL zásahem.
- restart `worldserver` přes `make restart-world` zachová DB/herní stav.

Zbývající položky Etapy 1 jsou **hardening / developer tooling**, nikoli gate pro zahájení AI vrstvy.

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
| **2 — AI World Foundation** | ▶ **NEXT** | Persistentní agenti, události, paměť, cíle, Action API a async AI bridge | Wolf attack → memory → goal → decision → validated action |

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

Etapa 1 potřebuje pouze izolovanou AI službu a healthcheck. **Skutečný async bridge z `worldserver` je součást vstupu do Etapy 2**, protože bez `AIWorldMgr/AIClient` zatím v TrinityCore není konzument rozhodovacího API.

- [x] `ai-server` běží jako samostatný container.
- [x] FastAPI `/health` endpoint.
- [x] Compose healthcheck potvrzuje dostupnost služby.
- [x] AI služba je na interní Docker síti a její HTTP port není standardně publikovaný na hosta.
- [x] `worldserver` není procesově ani GPU-runtime svázán s `ai-server`.
- [ ] `worldserver` → `ai-server` neblokující health request — **Etapa 2 / první milestone**.
- [ ] Timeout/reconnect/fallback metriky — **Etapa 2**.
- [ ] Runtime fault test: vypnout `ai-server` a prokázat, že aktivní svět pokračuje — provést po přidání prvního bridge.

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

**Verdikt:** Etapa 2 může začít.

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

- [ ] Přidat `AIWorldMgr` singleton/subsystem bez zásahu do existující CreatureAI hierarchie.
- [ ] Inicializovat `AIWorldMgr` při startu `worldserver`.
- [ ] Korektně ho ukončit při shutdownu.
- [ ] Přidat config flag, kterým lze celý AIWorld vypnout.
- [ ] S vypnutým AIWorld musí běžet vanilla TrinityCore chování beze změny.
- [ ] Vybrat jedno stabilní testovací NPC, například guard v Goldshire.
- [ ] Jednou za bezpečný interval vytvořit malý read-only snapshot jeho stavu.
- [ ] Snapshot pouze zalogovat; **žádná AI akce zatím nesmí měnit svět**.
- [ ] Ověřit start → login → běžná hra → restart worldserveru s aktivním `AIWorldMgr`.

### Milestone 2B — první async bridge

Teprve po 2A:

```text
AIWorldMgr
   │
   └── AIClient -- async --> ai-server /health or /decision
                          <-- response
```

- [ ] Implementovat neblokující klienta z `worldserver` do `ai-server`.
- [ ] Nikdy nečekat na síť/inference v world update hot path.
- [ ] Přidat timeout a deterministic fallback.
- [ ] Zahodit stale response, který dorazí po deadline nebo po změně relevantního agent state.
- [ ] Zalogovat `request_id`, latency, timeout a response status.
- [ ] Vypnout `ai-server` během běžícího světa a ověřit, že `worldserver` pokračuje bez blokace/crashe.

### Gate pro další práci

Dokud 2A + 2B nejsou stabilní, nezačínat velké persistentní NPC systémy.

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

- [ ] Vytvořit subsystem `src/server/game/AIWorld/`.
- [ ] `AIWorld` se inicializuje při startu `worldserver` a korektně se ukončí při shutdownu.
- [ ] Normální TrinityCore gameplay funguje i při vypnutém `AIWorld`.
- [ ] Všechny AI/network requesty jsou asynchronní vůči world update loopu.
- [ ] Každá externí decision odpověď má deadline a kontrolu freshness.

## 2.1 Persistentní agent

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

- [ ] Zavést stabilní `AgentId`.
- [ ] Vytvořit `AgentRegistry`.
- [ ] Umět propojit `AgentId` s aktuálním `Creature/ObjectGuid`.
- [ ] Umět agenta odpojit od `Creature` a ponechat jej jako abstraktní stav.
- [ ] Umět agenta znovu materializovat do světa.
- [ ] Připravit typy agentů minimálně:
  - `CIVILIAN`
  - `GUARD`
  - `MERCHANT`
  - `CREATURE_GROUP`

## 2.2 Persistence

AI stav musí přežít restart `worldserver`.

Navrhované tabulky:

```text
ai_agents
ai_agent_memory
ai_agent_relationships
ai_agent_goals
ai_events
ai_locations
```

- [ ] Navrhnout DB schema s verzováním/migracemi.
- [ ] Persistovat identitu, základní stav a vazbu na svět.
- [ ] Persistovat dlouhodobou paměť.
- [ ] Persistovat vztahy mezi agenty a hráči.
- [ ] Persistovat aktivní cíle.
- [ ] Persistovat důležité world events pro audit a replay/debug.
- [ ] Ověřit save → restart → load jednoho agenta.

## 2.3 World Event System

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

- [ ] Definovat `WorldEvent` s typem, časem, lokací, aktorem, cílem a payloadem.
- [ ] Vytvořit `EventBus` uvnitř `AIWorld`.
- [ ] Napojit první TrinityCore hooky na vznik událostí.
- [ ] Oddělit transient event od persistentní historické události.
- [ ] Přidat `correlation/cause id`, aby šlo sledovat řetězec příčina → následek.
- [ ] Přidat debug log událostí.

## 2.4 Perception System

- [ ] Agent nesmí automaticky vědět globální stav světa.
- [ ] Přidat nearby entity perception.
- [ ] Přidat základní range check.
- [ ] Tam, kde je vhodné, respektovat line-of-sight.
- [ ] Přidat perception událostí, kterých byl agent svědkem.
- [ ] Přidat přípravu pro informace z rozhovorů/rumorů v pozdější etapě.
- [ ] Z perception vytvářet `Observation` objekty použitelné pro memory a decision context.

## 2.5 Memory System

```text
ShortTermMemory
LongTermMemory
Knowledge
Relationships
```

- [ ] Implementovat krátkodobou paměť s expirací.
- [ ] Implementovat dlouhodobou paměť pro důležité události.
- [ ] Přidat importance score.
- [ ] Přidat čas a zdroj informace.
- [ ] Přidat vazbu na osoby, lokaci a událost.
- [ ] Připravit retrieval relevantních vzpomínek podle aktuální situace.
- [ ] LLM nikdy neposílat celou historii; posílat pouze vybrané relevantní záznamy.

## 2.6 Needs System

- [ ] Zavést minimální potřeby:
  - `health`
  - `hunger`
  - `fatigue`
  - `safety`
  - `money/resource pressure`
- [ ] Potřeby aktualizovat deterministicky v simulation ticku.
- [ ] Potřeby omezit do definovaného rozsahu, například `0.0–1.0`.
- [ ] Přidat threshold events, například `HUNGER_CRITICAL` nebo `DANGER_HIGH`.
- [ ] Nechat potřeby generovat kandidáty na cíle bez LLM.

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

- [ ] Definovat verzovaný request/response kontrakt.
- [ ] Posílat jen informace, které agent smí znát.
- [ ] Posílat dostupné akce explicitně.
- [ ] Rozhodování volat asynchronně.
- [ ] `worldserver` nesmí čekat na inference.
- [ ] Přidat `request_id`, `agent_id`, deadline a model/version metadata.
- [ ] Přidat state/version token pro detekci stale inference odpovědi.
- [ ] Měřit latency, queue time, timeout rate a invalid decision rate.
- [ ] Připravit API pro batching více agentů.
- [ ] Při nedostupnosti AI služby použít deterministic fallback.

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
- [ ] Drahou inference nikdy nepouštět v combat hot path.
- [ ] Připravit backpressure při přetížení AI queue.
- [ ] Umět přesunout agenta mezi `ACTIVE`, `NEARBY`, `BACKGROUND` a `ABSTRACT` stavem.

## 2.11 První experiment — persistentní farmář

Vybrat jedno NPC v malé testovací oblasti a dát mu jednoduchý, pozorovatelný denní cyklus.

- [ ] Agent má home lokaci.
- [ ] Agent má working lokaci.
- [ ] Agent má money/food/resource stav.
- [ ] Agent má potřeby a základní cíle.
- [ ] Ráno jde pracovat.
- [ ] Při nebezpečí uteče nebo požádá o pomoc.
- [ ] Večer se vrátí domů a odpočívá.
- [ ] Pamatuje si jednu důležitou událost i po restartu serveru.

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

- [ ] `AIWorldMgr` se stabilně inicializuje a ukončuje s `worldserver`.
- [ ] `AIWorldMgr` lze vypnout a vanilla gameplay funguje beze změny.
- [ ] Async `AIClient` nikdy neblokuje world update loop a má timeout/fallback/stale-response ochranu.
- [ ] Existuje persistentní `AgentId` a `AgentRegistry`.
- [ ] Minimálně jeden NPC agent přežije restart se zachovanou pamětí a stavem.
- [ ] `WorldEvent` system zachytí vybranou událost z TrinityCore.
- [ ] Perception určí, který agent událost viděl.
- [ ] `MemorySystem` z události vytvoří relevantní vzpomínku.
- [ ] Needs/Goal system vytvoří nebo upraví cíl.
- [ ] AI server obdrží `AgentContext` asynchronně a vrátí rozhodnutí.
- [ ] `ActionSystem` rozhodnutí validuje a provede pouze povolenou akci.
- [ ] Výpadek AI serveru nezablokuje `worldserver`.
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

## Teď

9. [ ] **AIWorldMgr lifecycle + log-only snapshot jednoho NPC**
10. [ ] **Async AIClient health/decision smoke + timeout/fallback**
11. [ ] `AgentRegistry` + stabilní `AgentId`
12. [ ] Persistence jednoho agenta
13. [ ] World Events
14. [ ] Perception + Memory
15. [ ] Needs + Goals
16. [ ] Bezpečné Action API
17. [ ] Decision protocol + scheduler
18. [ ] Persistentní farmář
19. [ ] Wolf pack
20. [ ] End-to-end emergentní událost

## Paralelní hardening

- [ ] Debugger/core dump workflow.
- [ ] Přesný extraction dokument.
- [ ] CUDA compute smoke test.
- [ ] Aktualizovat zastaralý status v `README_DEV.md`.

## Co bude následovat

**Etapa 3:** jedna živá oblast (například Elwynn Forest) s populacemi, zdroji, ekonomikou, vztahy, frakcemi a dynamickými problémy/questy.