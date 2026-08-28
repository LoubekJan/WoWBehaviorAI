# AIWorld — Current Roadmap

> **Aktualizováno:** 2026-08-29  
> **Aktivní větev:** `ai-world`  
> **Účel:** krátký aktuální execution roadmap nad detailním historickým dokumentem `AI_TrinityCore_Roadmap_Etapa_1_2.md`.  
> **Aktuální code baseline před tímto docs commitem:** `57c656d1f9c963c22b9b15a73b6da52b5c8f9bed`  
> **Detailní roadmap sync před tímto commitem:** `76a1a51b6f02170ff17c13ae0e74da544bfd5004`

## Základní invariant

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
ACTION REQUEST
    ↓
ACTION SYSTEM VALIDATION
    ↓
TRINITYCORE EXECUTION
    ↓
WORLD STATE
```

Platí pro všechny další milníky:

- AI pouze navrhuje; `ActionSystem` je validační autorita a TrinityCore provádí fyzickou změnu světa.
- Žádný live `Creature*`, `Player*`, `Map*` ani `Unit*` nesmí uniknout přes async hranici nebo být uložen pro pozdější použití.
- Async inference/network dostává pouze pure/value DTO a výsledky se aplikují zpět na world threadu.
- AIWorld nikdy force-loaduje grid kvůli simulaci nebo testu.
- Materialized individual agent odpovídá reálnému TrinityCore `Creature`; unloaded agent zůstává persistentní `AgentRecord`.
- Každý individual `AgentRecord` musí odpovídat skutečnému TrinityCore creature spawnu; testy nevyrábějí ghost physical agenty.
- `RuntimeGuid` je provenance pouze aktuální materialized incarnation a nesmí být zaměněn za persistentní identity.
- `GroupId` je samostatná persistentní identity doména; dissolved GroupId se nerecykluje tak, aby stará reference mohla aliasovat novou coalition.
- `AgentGroup` je sociální/koordinační entita, nikdy 1:1 physical entity a nikdy přímo nepohybuje Creatures.
- Group intent se vždy rozkládá na návrhy pro jednotlivé `AgentId`; individual Agent vlastní fyzickou akci.
- Vyšší-prioritní individual goal/action může group coordination preemptovat.
- LLM se používá pouze tam, kde deterministic logika nestačí, a nikdy není execution authority.

---

# Stav projektu

| Oblast | Stav |
|---|---|
| Etapa 1 — development infrastructure | **GATE SPLNĚN** |
| 2.1–2.9 — identity, persistence, event/perception/memory/needs/goals/actions/decision protocol | **CLOSED / runtime foundation PASS** |
| 2.10 — scheduler + simulation tiers | **CLOSED** |
| 2.11 — persistent farmer vertical slice | **CLOSED** |
| 2.12A–D — AgentGroup architecture pivot + separate GroupId domain | **CLOSED** |
| 2.12E1 — Create/Join/Leave/Dissolve | **CLOSED / runtime PASS** |
| 2.12E2 — async-safe lifecycle persistence boundary | **CLOSED** |
| 2.12E3 — Loose/Stable policy | **CLOSED** |
| 2.12E4 — generic profile-driven formation + maintenance | **CLOSED** |
| 2.12F1 — generic group intent (`REGROUP`) | **CLOSED** |
| 2.12F2 — intent → per-member proposal → ActionSystem | **CLOSED** |
| 2.12F3 — integration/lifecycle runtime proof | **CLOSED / STATIC + BUILD + RUNTIME PASS** |
| 2.12G — druhý profil + další generic group behavior | **NEXT** |
| 2.13 — local LLM dynamic task vertical slice | **PLANNED** |
| 2.14 — emergent end-to-end world event | **PLANNED** |
| Etapa 3 — Elwynn world preparation | **PLANNED** |
| Etapa 4 — Living World | **PLANNED** |

---

# 2.12 — AgentGroup / coalition

## Architektonický model

```text
AgentGroup
  ↓
generic observations
  ↓
generic group coordination / intent
  ↓
generic intent decomposition per member
  ↓
individual Agent
  ↓
ActionSystem
  ↓
TrinityCore
```

**Group = coordination. Agent = deciding/physical entity. ActionSystem = execution authority. TrinityCore = world mutation.**

Wolf/WolfLoose je první runtime fixture/profile. Není to produktová architektura. Přidání druhého typu nesmí vést k `RunBandit...()`, `RunGuard...()`, `WolfPackIntentSystem`, `BanditMovementCoordinator` ani jiné species-specific orchestration větvi.

## 2.12A–2.12D — identity a social-domain pivot

**Stav: CLOSED.**

Hotovo:

- group není fake `AgentRecord` a nemá fake SpawnId;
- každý člen zůstává samostatný persistentní Agent;
- vlastní `GroupId`, `AgentGroupRecord`, registry a persistence;
- persistent membership edges `GroupId ↔ AgentId`;
- monotónní persistentní GroupId high-water sequence;
- bounded GroupId-keyed coarse group simulation;
- group presence je odvozena z member materialization, bez force-load;
- group-level Hunger/population pseudo-agent model byl odstraněn.

## 2.12E1 — runtime lifecycle

**Stav: CLOSED / STATIC + RUNTIME PASS.**

```text
CreateGroup
JoinGroup
LeaveGroup
DissolveGroup
```

Lifecycle pracuje nad existujícími individual Agents a nikdy nevytváří physical group entity. Persistence confirmation předchází authoritative registry mutation a GroupId se po dissolve nerecykluje.

## 2.12E2 — async-safe lifecycle boundary

**Stav: CLOSED.**

Hotovo:

- runtime lifecycle nepoužívá blocking synchronous DB round-trip z recurring world-thread cesty;
- persistence requesty jsou async;
- completion se zpracovává na world threadu;
- per-GroupId operation locking/serialization brání překryvu Join/Leave/Dissolve;
- registry se mění až po confirmed persistence result;
- žádné TC live pointery přes async boundary.

## 2.12E3 — Loose / Stable policy

**Stav: CLOSED.**

Hotovo:

- `AgentGroupPolicySystem` odděluje policy od persistence/orchestrace;
- `Manual` vs `AutomaticPolicy` source provenance;
- Loose podporuje automatické leave/dissolve podle policy;
- Stable je chráněná proti automatickému leave/dissolve;
- automatic caller neobchází policy gate.

## 2.12E4 — generic formation + maintenance

**Stav: CLOSED / STATIC + RUNTIME PASS.**

Hotovo:

- `CoalitionFormationProfile` / `CoalitionMaintenanceProfile` jsou profile-driven data;
- generic `RunCoalitionFormation(...)` a `RunCoalitionMaintenance()` orchestrace;
- WolfLoose je pouze první profile fixture;
- deterministic candidate selection;
- async CreateGroup + Join chain;
- bounded recurring discovery/work;
- cross-profile formation reservation brání concurrent member race;
- `ProfileId` provenance je persistentní a validovaná;
- maintenance používá bounded cursor/high-water scan;
- automatic leave + below-min dissolve runtime ověřeno;
- restart neobnoví confirmed-dissolved group;
- automatic formation regression vytvořila nový real AgentGroup nad skutečnými Agents.

## 2.12F1 — generic AgentGroup intent layer

**Stav: CLOSED / STATIC + smoke PASS.**

První intent:

```text
AgentGroupIntentType::Regroup
```

`AgentGroupIntentSystem` je pure/value a nezná species. Profile poskytuje pouze policy data, například `RegroupEnabled` a `RegroupRadius`.

Runtime/pure smoke pokryl:

- far materialized/alive member → REGROUP;
- near member → NONE;
- unloaded → NONE;
- dead → NONE;
- different map → NONE;
- disabled/invalid/mismatched profile → NONE.

## 2.12F2 — group intent → individual ActionSystem dispatch

**Stav: CLOSED / STATIC + RUNTIME PASS.**

```text
AgentGroupIntent(REGROUP)
    ↓
AgentGroupIntentProjector
    ↓
GroupMemberActionProposal
    ↓
individual ActionRequest(MOVE_TO, SourceGoal=REGROUP)
    ↓
ActionSystem::Validate
    ↓
ActionExecutor
    ↓
TrinityCore movement
```

Dispatch před akcí znovu validuje:

- Agent stále existuje;
- membership stále platí;
- group/profile/intent stále platí;
- Agent je materialized + alive;
- map/provenance sedí;
- žádný higher-priority individual owner;
- žádný conflicting active action;
- overlapping regroup-enabled membership failuje zavřeně;
- target je v ActionSystem execution range;
- žádné force-load a žádný pointer escape.

Priority:

```text
Emergency ActiveGoal
    > Normal ActiveGoal
    > RoutineGoal
    > Group REGROUP
```

Runtime bylo potvrzeno:

- generic REGROUP vytvořil per-member `MOVE_TO` request;
- `ActionSystem` vrátil `ALLOWED`;
- execution `STARTED`;
- agent dorazil k group territory;
- po arrival nevznikal duplicate REGROUP spam.

## 2.12F3 — lifecycle / preemption integration proof

**Stav: CLOSED.**

### Static/build gate

- finální hardened hook commit: `57c656d1f9c963c22b9b15a73b6da52b5c8f9bed`;
- poslední static review: **P1=0, P2=0, P3=0**;
- build/runtime použitý pro finální proof: **PASS**;
- hook je defaultně vypnutý a není produkční behavior path.

### Emergency preemption proof

Skutečný běh potvrdil:

```text
REGROUP MOVE_TO running
    ↓
agent enters combat
    ↓
SafetyPressure = 1.0
    ↓
FLEE_DANGER / EMERGENCY activates
    ↓
COORDINATION_PREEMPTED_BY_GOAL
    ↓
REGROUP movement stopped
    ↓
individual FLEE request owns action
```

Tím je runtime potvrzeno, že group coordination nikdy nepřebíjí individual Emergency ownership.

### Dissolve-during-active-REGROUP proof

`AIWorld.TestDissolveOnActiveRegroupGroupId` je one-shot proof hook. Hardened verze:

- validuje configured GroupId až po loadu registry;
- záporný/invalid GroupId failuje zavřeně;
- nekontroluje condition každý world tick, ale pouze po skutečném `RunCoalitionCoordination()` passu;
- trigger vyžaduje plnou ownership/provenance shodu:
  - `ActiveActionState::Type == MoveTo`;
  - `ActiveActionState::SourceGoal == Regroup`;
  - `GroupCoordinationGoalState::Type == Regroup`;
  - správný `SourceGroup`;
  - matching attempt timestamp identity;
  - materialized Agent;
  - transientně resolved live Creature;
  - skutečně běžící AIWorld-owned MoveTo generator;
- hook pouze zavolá authoritative `RequestDissolveGroupWithPolicy(..., Manual)`; sám nezastavuje movement, nemutuje registry a nedělá raw SQL.

Finální runtime proof na nové group potvrdil sled:

```text
formation PASSED → real group created
    ↓
REGROUP request sourceGroup=<group>
    ↓
hook observes real active REGROUP
    ↓
Manual dissolve requested
    ↓
AI agent group dissolved
    ↓
COORDINATION_STOPPED_BY_LIFECYCLE
    ↓
sourceGoal=REGROUP + matching sourceGroup
    ↓
dissolve CONFIRMED
```

To dokazuje, že pohyb zastavila existující produkční lifecycle cesta, nikoli test hook.

### Restart / no-resurrection proof

Po confirmed dissolve, vypnutí hooku a restartu:

- dissolved GroupId se z persistence znovu nenačetl;
- nevznikl žádný stale `sourceGroup=<dissolved id>` REGROUP;
- žádné FAILED/ERROR související s tímto lifecycle proof;
- žádná duplicate group/action resurrection.

### F3 closure

```text
2.12F3 STATIC                    PASS
2.12F3 BUILD                     PASS
2.12F3 REGROUP                   PASS
2.12F3 ARRIVAL / NO-SPAM         PASS
2.12F3 EMERGENCY PREEMPTION      PASS
2.12F3 LIFECYCLE CANCELLATION    PASS
2.12F3 GROUP PROVENANCE          PASS
2.12F3 RESTART / NO RESURRECTION PASS

2.12F3 = CLOSED
```

Po proofu mají testovací flagy zůstat vypnuté:

```ini
AIWorld.TestDissolveGroupId = 0
AIWorld.TestDissolveOnActiveRegroupGroupId = 0
AIWorld.TestGroupIntent = 0
AIWorld.TestGroupIntentProjector = 0
```

---

# NEXT — 2.12G Genericity proof a další group behavior

## 2.12G1 — druhý skutečný coalition profile přes stejnou pipeline

**Priorita: NEXT.**

Než přidáme složitější chování, musí být prakticky dokázáno, že současná architektura není WolfPack systém převlečený za generic API.

Cíl:

```text
second real profile
    ↓
existing generic formation/profile resolution
    ↓
AgentGroup + persistent ProfileId
    ↓
existing generic observations
    ↓
existing AgentGroupIntentSystem
    ↓
existing AgentGroupIntentProjector
    ↓
individual ActionRequest
    ↓
ActionSystem
    ↓
TrinityCore
```

Požadavky:

- druhý profil musí používat skutečné existující TrinityCore creature spawny;
- žádný fake AgentRecord ani synthetic physical member;
- candidate/profile selection může mít profile data, ale ne nový orchestration loop;
- žádný `RunBanditFormation()`, `RunGuardCoordination()`, `BanditIntentSystem`, `GuardMovementCoordinator` apod.;
- generic formation/maintenance/coordination scheduler musí obsloužit oba profily;
- `ProfileId` provenance musí persistovat a po restartu se správně obnovit;
- overlapping membership/arbitration pravidla zůstávají stejná;
- bounded scans/work budgets zůstávají společné;
- ActionSystem zůstává jediná execution autorita.

Výběr konkrétního druhého fixture profilu se má udělat podle reálných spawnů v dostupném testovacím světě (např. guard/bandit/caravan-like skupina), ne vytvořením ghost test entity jen kvůli testu.

Acceptance gate:

```text
WolfLoose + second profile
→ oba přes stejný generic RunCoalitionFormation / maintenance / coordination path
→ oba vytvoří správný persistent AgentGroup
→ oba projdou generic intent/projector/action path
→ žádná species-specific orchestration větev
→ restart zachová identity/provenance bez duplicate group/action
```

**Silné kritérium:** pokud přidání druhého typu vyžaduje `Run<Species>...()` orchestration metodu, návrh se vrací k refactoru a 2.12G1 není splněno.

## 2.12G2 — generic ROAM / territory movement intent

Po G1 přidat druhé viditelné group chování, stále generic a deterministic.

Návrhový směr:

- rozšířit `AgentGroupIntentType` o generic roaming/territory movement intent;
- target vybrat z profile/territory policy, ne z hardcoded species logiky;
- group pouze vybere společný coordination target/constraint;
- projector vytvoří individual proposals;
- individual movement může používat deterministic per-member offset/spacing, aby všichni nesdíleli jeden bod, pokud to runtime ukáže jako nutné;
- ActionSystem validation + stale membership/profile/attempt revalidation zůstává stejná;
- Emergency/Normal/Routine individual ownership nadále preemptuje group movement;
- unloaded member nedostává physical action a nesmí být force-loadnut.

Runtime gate:

- group se skutečně přesune mezi dvěma validními body/anchory;
- členové používají individual actions;
- žádný duplicate movement spam;
- arrival/stop je stabilní;
- leave/dissolve/preemption během roam failuje bezpečně;
- oba profily z G1 mohou stejný intent použít pouze změnou profile data/policy.

## 2.12G3 — generic HUNT / coordinated combat preparation

Až po stabilním G1/G2.

Nejdřív navrhnout explicitní server-owned combat proposal/action contract. Group nesmí přímo volat combat API nad více Creatures.

Požadavky:

- target selection vychází z generic observations + profile policy;
- target identity/provenance je explicitní a stale target failuje zavřeně;
- group intent se decomposuje na per-member combat/approach proposals;
- každý member znovu validuje alive/materialized/map/range/target/membership/action ownership;
- žádný force-load targetu ani membera;
- individual emergency/vanilla combat safety má přednost;
- ActionSystem/TrinityCore zůstává jediná fyzická execution cesta;
- duplicate/replayed attack proposals jsou potlačené;
- leave/dissolve/target death/unload během coordination bezpečně ruší stale group-owned intent.

Runtime gate má nejdřív dokazovat correctness a ownership, ne „chytré smečkové taktiky“.

## 2.12G4 — roles / leader pouze pokud je skutečně potřeba

Leadership se nepřidává preventivně.

Přidat jej jen tehdy, pokud G2/G3 ukáže konkrétní potřebu, kterou nelze čistě vyřešit současným generic group state + deterministic policy.

Pokud vznikne:

- role je social metadata nad `AgentId`, ne nový physical AgentType;
- leader je member reference s explicitní stale handling;
- leader unload/death/leave nesmí rozbít group lifecycle;
- deterministic replacement/election policy;
- role/leader nikdy neobchází per-member ActionSystem ownership.

---

# 2.13 — local LLM dynamic task / player interaction vertical slice

**Stav: PLANNED po uzavření nejbližšího group genericity/behavior gate.**

Cíl není dát LLM kontrolu nad světem. Cíl je dokázat bezpečný řetězec:

```text
WORLD problem/event
    ↓
NPC perception + memory + goal
    ↓
sanitized QuestContext DTO
    ↓ async
local ai-server / LLM
    ↓
structured QuestProposal
    ↓
authoritative server validation
    ↓
player-facing offer/task
    ↓
player action validated by TrinityCore
    ↓
WorldEvent
    ↓
NPC memory/goal/world-state feedback
```

### 2.13A — skutečný local-model request

- skutečný lokální model/backend za existujícím `ai-server` boundary;
- async timeout/fallback;
- strict structured output;
- correlation + snapshot/provenance;
- sanitizovaný minimální kontext;
- žádný live pointer nebo arbitrary world serialization.

### 2.13B — `QuestProposal` validator

Server validuje objective type, target, amount/range/expiry, reward bounds a source-problem provenance. LLM nesmí generovat execution code, SQL, spawn/delete, spell cast ani jinou direct mutation.

### 2.13C — player-facing lifecycle

Minimálně offer → accept → active → completed/failed/expired s jednoznačnou identity a authoritative progress z TrinityCore events.

### 2.13D — runtime gate

Jeden skutečný LLM call musí být součástí end-to-end cesty a timeout/malformed/stale/outage musí mít safe fallback.

---

# 2.14 — první emergentní end-to-end událost

Cíl:

```text
real coalition behavior
    ↓
world consequence
    ↓
WorldEvent
    ↓
farmer/NPC perception
    ↓
memory
    ↓
PROTECT_HOME / REQUEST_HELP
    ↓
validated dynamic player task
    ↓
player changes authoritative world state
    ↓
result event returns to NPC/world
```

Příklad může navazovat na wolves/livestock/farmer, ale architektura nesmí záviset na tomto jednom příběhu.

---

# 2.15 — testy, diagnostika a scale hardening

Průběžně doplnit:

- unit testy Goal/Action/group policy/intent/projector;
- integration lifecycle/coordination tests;
- debug snapshot podle `AgentId` a `GroupId`;
- metrics pro scheduler, stale responses/actions a lifecycle failures;
- bounded-work assertions/profiling pro registry scans a group scans;
- LLM proposal validation + malformed/stale/outage tests;
- restart/replay/duplicate suppression tests;
- action ownership audit logs s source goal/group/attempt provenance.

Scale hardening není důvod předčasně přidávat složitou infrastrukturu. Optimalizace mají následovat skutečně změřený problém, ale recurring world-thread work musí zůstat explicitně bounded už od začátku.

---

# Etapa 2 — Definition of Done

Etapa 2 není hotová pouze tím, že NPC umí chodit.

Hotovo:

- [x] persistent individual Agent identity;
- [x] event/perception/memory/needs/goals;
- [x] safe Action API;
- [x] async decision protocol + stale/provenance protection;
- [x] bounded multi-agent scheduler;
- [x] persistent farmer vertical slice;
- [x] separate persistent AgentGroup identity;
- [x] async-safe AgentGroup lifecycle;
- [x] Loose/Stable policy;
- [x] automatic generic profile-driven formation/maintenance;
- [x] generic group REGROUP intent;
- [x] per-member ActionSystem dispatch;
- [x] Emergency preemption of active group coordination;
- [x] leave/dissolve lifecycle cancellation of active group coordination;
- [x] restart/no-resurrection proof.

Zbývá před uzavřením Etapy 2:

- [ ] second-profile genericity proof;
- [ ] alespoň jedno další skutečné generic group behavior potřebné pro emergentní slice;
- [ ] skutečný local LLM request přes async `ai-server`;
- [ ] structured server-validated player task proposal;
- [ ] player-facing task lifecycle;
- [ ] end-to-end `WORLD → NPC → LLM/DECISION → PLAYER/TRINITYCORE → EVENT → WORLD` runtime gate;
- [ ] safe fallback pro LLM outage/malformed/stale response.

---

# Etapa 3 — Elwynn Forest World Preparation

**Stav: PLANNED.**

Cíl: před komplexním Living World během mít jednu oblast přesně zmapovanou a datově opravenou.

Pořadí:

1. kompletní creature/NPC spawn census pro přesně definovaný Elwynn scope;
2. classification každého relevantního spawnu (`FULL_AGENT`, background/lightweight, vanilla-only apod.);
3. semantic locations/anchors/routes/resources/danger regions;
4. TrinityCore faction/faction-template audit;
5. explicitní AI `WorldFactionId` oddělená od raw TC faction a od AgentGroup;
6. invariant coalition membership pouze uvnitř kompatibilní WorldFaction;
7. faction presence/holdings nad semantic locations;
8. controlled same-faction movement/presence mezi locations bez identity loss a bez force-load;
9. versionované world DB opravy + before/after audit;
10. clean bootstrap/runtime data-quality gate.

Etapa 3 připravuje data a mechanismy. Neimplementuje ještě plnou dynamickou geopolitiku.

---

# Etapa 4 — Living World

**Stav: PLANNED.**

Teprve zde skládat ověřené mechanismy do dlouhodobého světa:

- population pressures;
- resources a jejich obnova/spotřeba;
- economy/trade/work relations;
- persistent relationships;
- faction relations, conflict, cooperation, expansion/retreat;
- movement populations/coalitions mezi semantic locations;
- problems vznikající z world state místo z pevné quest sequence;
- server-validated LLM narrative/tasks nad skutečnými problémy;
- důsledky hráčových akcí zpět do economy/faction/population/memory/world state.

Etapa 4 nemá znovu objevovat základní identity, threading, lifecycle, action ownership nebo LLM safety boundary. Tyto fundamenty musí být uzavřené dříve.

---

# Doporučené pořadí od aktuálního stavu

1. [x] 2.12F3 static/build/runtime closure.
2. [ ] **2.12G1 — second real coalition profile přes stejnou generic pipeline.**
3. [ ] 2.12G2 — generic roaming/territory movement.
4. [ ] 2.12G3 — generic hunt/coordinated-combat ownership seam.
5. [ ] 2.12G4 — roles/leadership pouze pokud G2/G3 prokáže potřebu.
6. [ ] 2.13A — actual local LLM inference path.
7. [ ] 2.13B — structured `QuestProposal` + authoritative validation.
8. [ ] 2.13C — player-facing dynamic task lifecycle.
9. [ ] 2.13D — `WORLD → NPC → LLM → PLAYER → WORLD` runtime gate.
10. [ ] 2.14 — emergent end-to-end world event slice.
11. [ ] 2.15 — remaining diagnostics/scale hardening needed by measured runtime behavior.
12. [ ] Etapa 3 — Elwynn census + semantic locations + faction/world-data preparation.
13. [ ] Etapa 4 — Living World composition.

---

# Development / verification workflow

Pro C++ AIWorld změny:

```bash
git checkout ai-world
git pull --ff-only origin ai-world
git log -1 --oneline
make build
make restart-world
make world-logs
```

Při compiler failure řešit nejdřív první compiler error block, ne následnou kaskádu.

Pro Python `ai-server` změny:

```bash
docker compose -f compose.yml -f compose.dev.yml up -d --build ai-server
make restart-world
make logs
```

Debug AIWorld používat přes `TC_LOG_DEBUG("ai.world", ...)`; DEBUG evidence hledat v `runtime/logs/Server.log`.

Po runtime proof vracet one-shot/test flags na default `0`/disabled. Destruktivní DB reset není standardní testovací krok.

---

# Nejbližší acceptance gate

Další změna se nemá zaměřit na další wolf-only behavior. Nejbližší gate je:

```text
REAL SECOND PROFILE
    ↓
SAME GENERIC FORMATION
    ↓
SAME GENERIC MAINTENANCE
    ↓
SAME GENERIC GROUP INTENT
    ↓
SAME PROJECTOR
    ↓
INDIVIDUAL ACTION
    ↓
ACTION SYSTEM
    ↓
TRINITYCORE
```

Jakmile to projde bez druhé orchestration větve, máme prakticky potvrzené, že `AgentGroup` je skutečně obecná coalition vrstva a můžeme bezpečně stavět další behavior (`ROAM`, následně hunt/combat) nad touto základnou.
