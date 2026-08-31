# AIWorld — Current Roadmap

> **Aktualizováno:** 2026-08-31  
> **Aktivní větev:** `ai-world`  
> **Účel:** krátký aktuální execution roadmap nad detailním historickým dokumentem `AI_TrinityCore_Roadmap_Etapa_1_2.md`.  
> **Aktuální code baseline před tímto docs commitem:** `e1b5e675fb3bb56575b8597634173d607e2acf80`  
> **Detailní roadmap sync před tímto commitem:** `21feceefdfe00134ea8ab203c2a4691c23e72c23`

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
| 2.12F4A–F4B3 — ControlMode gate, TrinityCore-aligned identity, spawn reconciliation, scoped Elwynn population + full Control activation | **CLOSED / STATIC + BUILD + RUNTIME PASS (Elwynn: 3540/3540 `AIWorldControlled`)** |
| 2.12F4C/F4D — world-scale hardening (O(1) index, bounded recurring work) + full-world bootstrap | **DEFERRED — not required for single-location work; required before any eventual full-world rollout, see 2.12F4C's own Priorita** |
| 2.12G1 — druhý coalition profile (genericity proof) | **CLOSED / STATIC + BUILD + RUNTIME PASS** |
| 2.12G2 — generic ROAM/territory movement intent | **CLOSED / STATIC + BUILD + RUNTIME PASS** |
| 2.12G3 — generic HUNT/coordinated combat contract | **NEXT / CONTRACT DESIGN** |
| 2.12G4 — roles/leadership | **NOT NEEDED YET — viz 2.12G4's own Priorita** |
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

# NEXT — 2.12F4 Global Agent Population

**Priorita: foundation gate před 2.12G1.** Vloženo před druhý coalition profil, protože 2.12G1 vyžaduje reálné, ne synteticky vyrobené spawny druhého profilu, a ty by dnes narazily na přesně ty problémy, které 2.12F4 řeší.

## Proč je to samostatný gate, ne jednoduchý INSERT

Dnes je „AI agent" prakticky každý řádek v `characters.ai_agents`; `LoadAgents()` při startu všechny načte a `CreateCreatureAgent()` vytváří vazbu `(map_id, spawn_id) → AgentId`. Současně ale `OwnsSpawn()` rozhoduje, jestli `Creature` dostane `AIWorldCreatureAI` místo normálního TrinityCore AI, a `AIWorldCreatureAI` při převzetí nastaví `REACT_PASSIVE` a `MoveIdle()`. Prostá registrace všech spawnů by tedy dnes převzala i guardy, vendory, quest NPC, bossy a scripted NPC a rozbila jejich vanilla/script chování.

Současná runtime vrstva navíc ještě není připravená na desetitisíce agentů — scheduler discovery, world-event perception, nearby perception, needs update a coalition candidate discovery jsou dnes lineární `_registry.GetAgents()`/`FindBySpawn()` scany nad celou registry. Existující komentář v kódu přímo říká, že lineární event scan je přijatelný pro „single-digit/dozens" a spatial index má přijít při stovkách+ agentech.

**Varianta „INSERT všechny spawny do `ai_agents` a hotovo" je zamítnutá** — v současném kódu by byla funkčně (přebití vanilla/scripted AI) i výkonnostně (recurring O(all agents) scany) nebezpečná.

## Cílový model

```text
REAL TRINITYCORE CREATURE SPAWN (non-instance / base-world)
        ↓
persistent AgentId / AgentRecord
        ↓
ALL persistent non-instance creatures are AIWorld-known agents
        ↓
ControlMode
   ├── ObserveOnly
   └── AIWorldControlled
              ↓
        AI proposes actions
              ↓
          ActionSystem
              ↓
          TrinityCore
```

Každá reálná mobka/NPC dostane persistentní identitu (memory/relationships/group eligibility), aniž bychom zničili TrinityCore chování dřív, než pro daný typ existuje adekvátní AIWorld behavior.

**Terminologie:** enum má přesně dvě hodnoty, `ObserveOnly` a `AIWorldControlled`. Nikde jinde v tomto dokumentu ani v kódu se nepoužívá `VanillaControlled` ani `FullControl` — jsou to jména pro tutéž semantiku a vedla by ke dvěma pojmenováním jednoho konceptu.

**Bezpečné pořadí (STATIC review, P2 nálezy proti dřívější verzi tohoto gate):** `ControlMode` schema a jeho hard enforcement musí existovat *před* jakýmkoli bulk bootstrapem, ne až po něm. Dřívější pořadí (nejdřív bootstrapovat všechny `world.creature` do `AgentId`, teprve pak přidat ownership split) by samo vytvořilo přesně ten nebezpečný přechodný stav, který tento gate má odstranit — `AgentRegistry` dnes žádný `ControlMode` koncept nemá a každý načtený persistentní record považuje za běžného agenta. Proto je pořadí A→B→C→D, kde `ControlMode` (A) je prerequisite reconciliace (B), ne naopak.

**Scope: non-instance / base-world spawny (STATIC review P2 fix).** Současná identita používá pouze `(mapId, spawnId) → AgentId` a jeden `RuntimeGuid` na `AgentRecord` — to funguje pro persistentní open-world spawn, ale ne obecně pro instance/raid mapy, kde stejný `(map_id, spawn_id)` může existovat současně jako několik různých `Creature` objektů v různých instancích (runtime na řadě míst už dnes za live/materializovaného světa používá `FindBaseNonInstanceMap(record->MapId)`; event enrichment přiřazuje `AgentId` jen podle `MapId`+`SpawnId`, bez instance identity). Globální registrace instance spawnu by dvě různé incarnace namapovala na stejný `AgentId`/`RuntimeGuid` — sdílená persistent identity, smíchané eventy/memory/provenance, přímý konflikt s invariantem „individual Agent = konkrétní physical entity". **`2.12F4` scope je proto výhradně persistentní non-instance/base-world creature spawny.** Instance/raid creature zůstávají mimo, dokud nebude samostatně navržená instance-aware identity semantics — to je jiný problém, ne 2.12F4. Rozšiřovat `AgentId` o `InstanceId` v rámci tohoto gate by bylo scope creep.

**Scope predicate ≠ `FindBaseNonInstanceMap()` (STATIC review P3 fix).** `FindBaseNonInstanceMap(mapId)` nejprve volá `FindBaseMap(mapId)`, které vrátí `nullptr`, pokud daná base map ještě v procesu nebyla vytvořena — teprve pokud mapa existuje, kontroluje `Instanceable()`. Je to tedy runtime resolver nad už materializovaným světem, ne autorita pro census/reconciliation: startup reconciliation (`2.12F4B`) nesmí spawn přeskočit jen proto, že `FindBaseNonInstanceMap()` zrovna vrátil `nullptr`, protože ta konkrétní base map ještě nebyla v procesu vytvořená — to by legitimní open-world spawny vynechávalo nedeterministicky, podle toho, které mapy už byly zrovna materializované. Scope predicate pro `2.12F4B` musí být deterministický nad statickými map metadaty, nezávislý na tom, jestli je daná mapa zrovna vytvořená:

```text
MapEntry existuje
AND MapEntry::Instanceable() == false
```

`FindBaseNonInstanceMap()` zůstává tím, čím je dnes — runtime resolver pro live/materializovaný svět (perception, event enrichment, dispatch) — jen se nesmí použít jako scope filtr při `2.12F4B` census/reconciliation.

## 2.12F4A — ControlMode foundation

**Stav: CLOSED / STATIC PASS (`967282a543`, `543a235fc8`, `416016164b`).** Build/runtime nebyly pro F4A samostatně verifikovány odděleně od navazujících F4A2–F4B3 komitů, ale ControlMode gate je od té doby beze změny součástí každého dalšího runtime-ověřeného kroku (viz F4B3 níže).

**Priorita: první krok, prerequisite všeho ostatního v tomto gate.**

Existence `AgentRecord` už nesmí sama o sobě znamenat, že AIWorld převezme TrinityCore `CreatureAI` ani že smí způsobit jakoukoli fyzickou akci nad Creature. Potřebujeme explicitní, persistentní control/ownership stav oddělený od pouhé identity:

```text
ControlMode
   ├── ObserveOnly       — AgentId/AgentRecord existuje, TrinityCore/scripted AI běží beze změny
   └── AIWorldControlled — AIWorldCreatureAI vlastní CreatureAI, AI proposes / ActionSystem / TrinityCore pipeline
```

`ObserveOnly` je tvrdý invariant, ne jen "nepřevezmi `CreatureAI`":

```text
AIWorld may observe/state-track this agent.
AIWorld MUST NOT cause physical world mutation for this agent.
```

`AgentRecord` dnes nese needs, active goal, routine goal, group coordination goal i active action — u tisíců nových `ObserveOnly` recordů nestačí zabránit jen vzniku `AIWorldCreatureAI`.

**`ActionSystem::Validate()` je mandatory authoritative gate, ne "ideální" defense-in-depth (STATIC review P2 fix proti dřívější verzi).** `ActionSystem` je už dnes explicitně definovaný jako safety boundary: *AI proposes → ActionSystem validates → ActionExecutor executes only on ALLOWED.* `ActionExecutor` je engine boundary, jehož vlastní kontrakt už dnes předpokládá, že se volá až poté, co `ActionSystem::Validate()` vrátil `Allowed` — psát `ControlMode` gate až na `ActionExecutor` by tento kontrakt obracelo. Správný pipeline:

```text
scheduler/routine/group gates
    ↓          performance + early rejection, ne safety boundary
ActionRequest
    ↓
ActionValidationContext.ControlMode
    ↓
ActionSystem::Validate()
    ↓
ObserveOnly       → REJECT
AIWorldControlled → pokračovat
    ↓
ActionExecutor
```

`ControlMode` jde do `ActionValidationContext` jako čistá hodnota — context je už dnes DTO světových faktů, které sestavuje `AIWorldMgr`, takže tím neporušíme pure/value hranici `ActionSystem`u. `ActionExecutor` nemusí znát `AgentRecord` ani `ControlMode` a zůstává třetím krokem až po úspěšné validaci.

`ControlMode` tedy gatuje na dvou různých úrovních, s různým účelem:

- **performance/early-rejection** (ne safety boundary samo o sobě) — decision scheduler / `ProcessAgent()`, routine → action proposal, group action proposal/dispatch (`DispatchGroupMemberActionProposal()` a ekvivalenty), automatic coalition membership pokud z ní může vzniknout fyzická akce — `ObserveOnly` agent se sem nemá dostat vůbec, aby se nezbytečně stavěl `ActionRequest`, který stejně skončí `REJECT`;
- **authoritative safety gate** — `ActionSystem::Validate()` přes `ActionValidationContext.ControlMode`, mandatory, ne volitelné; i kdyby některý z gatů výše selhal nebo byl obejit, `Validate()` musí `ObserveOnly` request odmítnout.

Bez tohoto gatingu na `ActionSystem` úrovni může vanilla/scripted AI stále vlastnit `CreatureAI`, zatímco AIWorld pošle stejnému `Creature` `MOVE_TO`/`FLEE`/`EAT` atd. — dual ownership nad jedním fyzickým objektem. `2.12F4C` (scale hardening) toto neřeší; ownership bezpečnost musí být hotová už zde.

Kroky:

- `ControlMode` jako persistentní pole `AgentRecord`/`ai_agents`;
- `OwnsSpawn()` (nebo ekvivalentní rozhodovací bod) se řídí `ControlMode`, ne pouhou existencí `AgentRecord`;
- `ActionValidationContext` získá `ControlMode` pole; `ActionSystem::Validate()` odmítá `ObserveOnly` bezpodmínečně, před jakoukoli per-`ActionType` validací;
- performance-gates výše (scheduler/routine/group) zůstávají, ale jsou dokumentované jako optimalizace, ne jako náhrada `ActionSystem` gate;
- migrace: současné 4 testovací mobky → `AIWorldControlled` (explicitní, ne implicitní default);
- nový/default bootstrap row → `ObserveOnly`.

Runtime gate: `ObserveOnly` agent nikdy neprojde decision schedulerem, nikdy nedostane routine/group action proposal a `ActionSystem::Validate()` odmítne jakýkoli `ActionRequest` pro `ObserveOnly` agenta i v případě, že by performance-gate výše selhal — `ActionExecutor` se pro `ObserveOnly` nikdy nezavolá, protože `Validate()` mu to nedovolí, ne protože to sám kontroluje.

## 2.12F4A2 — TrinityCore-aligned Agent identity (`AgentId == SpawnId`)

**Stav: CLOSED / STATIC PASS (`3c910a4e2a`, `f6211fbeab`).** Invariant `AgentId == SpawnId` je od tohoto bodu vynucený fail-closed v `CreateCreatureAgent()`/`LoadAgents()`/reconciliation a runtime-ověřený navazujícími F4B/F4B2/F4B3 běhy nad reálnou populací (128849 full-world, 3540 Elwynn) beze zjištěného mismatch.

**Priorita: až po 2.12F4A, striktně před 2.12F4B.** `2.12F4B` má vytvořit řádově tisíce nových `AgentRecord`ů z `world.creature`; změna identity schématu až po tomto bulk bootstrapu by znamenala migrovat tisíce cizích klíčů napříč `ai_agents`, `ai_agent_group_members`, `ai_long_term_memories` a dalšími tabulkami místo dnešních čtyř — udělat to teď je řádově levnější a bezpečnější.

Dnešní `AgentId` je `ai_agents.agent_id`, MySQL `AUTO_INCREMENT`, nezávislý na tom, ke kterému `world.creature` spawnu patří (`AgentId 1 → spawn 80335`, ...). To je zbytečná indirection: k přečtení "který spawn/creature patří k agentovi X" je vždy nutná zpětná lookup přes `ai_agents`, a v logu/debug session `agent=1` nic neříká o tom, co reálně hledat v `creature`/`creature_template`.

Nové pravidlo pro persistentní non-instance Creature agenty:

```text
AgentId.Value == TrinityCore Creature SpawnId (world.creature.guid)
```

`AgentId` (persistentní identita v rámci AIWorld API) a `Creature::GetGUID()`/`RuntimeGuid` (aktuálně materializovaná instance té identity) zůstávají oddělené, jen numericky sladěné s `SpawnId`, ne s `RuntimeGuid`:

```text
world.creature.guid = 80683
        ↓
AgentId = 80683           ← persistentní identita, stabilní přes restart
        ↓
AgentRecord
    SpawnId     = 80683   ← provenance/binding na world.creature, viz níže
    MapId       = 0
    RuntimeGuid = <aktuální ObjectGuid nebo empty>   ← mění se mezi materializacemi/restarty
```

`AgentRecord::SpawnId` se **neruší**, i když bude číselně identické s `AgentId::Value`. Mají jiný význam — `AgentId` je identita entity uvnitř AIWorld API, `SpawnId` je explicitní provenance/foreign binding na konkrétní `world.creature` řádek. To umožňuje invariant assert (`record.Id.Value == record.SpawnId`) jako levnou debug/consistency kontrolu, ne jako redundanci k odstranění.

**Instance scope zůstává mimo tento krok.** Stejný DB spawn může mít víc současně živých instancí (`spawn 12345` → instance 17 i instance 42 zároveň) — ty nemohou sdílet jeden `AgentId 12345`, jsou to fyzicky oddělené bytosti. `2.12F4` (celé, včetně F4A2) zůstává scoped jen na non-instance/base-world spawny (viz Scope predicate výše, `MapEntry` existuje a `Instanceable() == false`) přesně proto, aby se identity-per-instance problém nemusel řešit teď. Instance identity (např. `RuntimeAgentIdentity{ SpawnId, InstanceId }` nad stejným persistentním `SpawnId`) je vlastní budoucí milestone, ne součást `2.12F4A2` — tady se kvůli instancím nesmí znovu zavést anonymní/generated `AgentId`.

Kroky:

- `AgentPersistence::CreateCreatureAgent()` už nespoléhá na MySQL `AUTO_INCREMENT` pro odvození `AgentId` u creature agentů — insert explicitně nese `agent_id = spawn_id`; read-back-by-binding disciplína (viz `FindBinding()`) zůstává, jen už neslouží k zjištění MySQL-přiděleného ID, ale k potvrzení, že řádek s očekávaným `agent_id` skutečně existuje;
- migrace existujících 4 agentů na jejich vlastní `SpawnId`: `1 → 80335`, `2 → 214023`, `3 → 214021`, `4 → 80683`, včetně navazujících řádků v `ai_agent_group_members` (`member_agent_id`) a `ai_long_term_memories` (`agent_id`) a jakékoli další tabulky, která dnes ukládá `agent_id` jako cizí klíč na `ai_agents`;
- žádné místo v kódu ani dokumentaci nesmí předpokládat `AgentId = AIWorld-generated sequence` — `AgentRegistry::Add()` dnes negeneruje/nepřiděluje nic sama, ale kdekoli se dřív implicitně počítalo s malými sekvenčními hodnotami (logy, testy, komentáře), je nutné to opravit;
- **namespace policy je rozhodnutá teď, ne odložená na 2.12F4B (P2 fix, STATIC review):** `ai_agents.agent_id` přestává být `AUTO_INCREMENT` úplně. `AgentType` dnes zahrnuje jen `Civilian`/`Guard`/`Merchant` — každý z nich reálný creature agent — takže není žádný současný důvod nechávat v tomto namespace možnost anonymní MySQL-generated hodnoty; ponechaný `AUTO_INCREMENT` by znamenal riziko kolize budoucí auto-přidělené hodnoty s reálným `spawn_id` při `2.12F4B` reconciliation. Platí natvrdo: `Creature AgentId namespace == TrinityCore SpawnId namespace, žádná náhodně generovaná ID.` Budoucí ne-creature `AgentType` (pokud kdy vznikne) musí mít vlastní oddělený namespace nebo vlastní tabulku, nikdy tichou auto-alokaci do tohoto sloupce/rozsahu;
- `AgentPersistence::CreateCreatureAgent()` a `LoadAgents()` vynucují invariant fail-closed, ne jen logují: existující binding s `agent_id != spawn_id` se odmítne znovupoužít, čerstvý INSERT jehož read-back neodpovídá `spawn_id` se odmítne vrátit, a při loadu se řádek porušující invariant do `AgentRegistry` vůbec nepřidá (quarantine, ERROR log) — žádná z těchto cest nesmí nechat běžet agenta, o kterém systém sám ví, že porušuje `AgentId == SpawnId`.

Runtime gate: pro každý persistentní non-instance Creature platí `AgentId.Value == TrinityCore Creature SpawnId`; `agent=<id>` v logu je přímo `SELECT * FROM creature WHERE guid = <id>` bez zpětné lookup přes `ai_agents`; existující 4 test agenti fungují po migraci identicky (`ControlMode`, group membership, long-term memory beze ztráty dat); jakýkoli řádek porušující invariant je fail-closed odmítnut/quarantined (nikdy tiše nefunguje dál jako platný agent) a `ai_agents.agent_id` už nikdy nepřidělí hodnotu mimo `spawn_id`-derived namespace.

## 2.12F4B — Global spawn reconciliation

**Stav: CLOSED / STATIC + BUILD + RUNTIME PASS (`ad80db5949`, `149e400927`, `326e7a7b19`) — engine runtime-ověřen jak nad plnou světovou populací (128849, viz Runtime evidence níže), tak scoped nad Elwynn (2.12F4B2/F4B3).**

**Priorita: až po 2.12F4A a 2.12F4A2** — reconciliace smí vytvářet nové `AgentRecord`s pouze do bezpečného `ControlMode` (2.12F4A), a musí od prvního nově vytvořeného záznamu používat finální identitu `AgentId == SpawnId` (2.12F4A2) — dělat bulk bootstrap se starou `AUTO_INCREMENT` identitou a pak ji migrovat přes tisíce řádků by bylo přesně to draho/rizikové, čemu má 2.12F4A2 předejít.

Roadmap dříve specifikovala jen jednosměrný bootstrap (`world.creature` → chybějící `ai_agents`: vytvoř). To je neúplné vůči vlastnímu invariantu projektu — **každý individuální `AgentRecord` musí odpovídat skutečnému TrinityCore Creature spawnu.** `AgentRegistry` dnes drží persistentní record jako `Abstract` i bez právě načteného `Creature` a sám nepozná, že DB spawn byl definitivně odstraněn. `2.12F4B` proto musí být obousměrná reconciliace, ne pouze bootstrap — a stejně jako zbytek `2.12F4` **jen nad non-instance/base-world spawny** (viz Scope výše; instance/raid spawny se do žádné z níže uvedených tří větví nezahrnují):

```text
world.creature (non-instance) → chybí v ai_agents:
    CREATE (ControlMode = ObserveOnly, viz 2.12F4A)

ai_agents → existuje odpovídající non-instance world.creature:
    valid, beze změny

ai_agents → world.creature spawn už neexistuje:
    fail-closed / quarantine / controlled cleanup
    NESMÍ se načíst jako legitimní persistent agent
```

Bez směru "dolů" (spawn byl z TrinityCore DB odstraněn) bychom po smazání spawnu vytvořili přesně ten typ ghost persistent agenta, který architektura jinde zakazuje.

Druhý otevřený bod: **`AgentType` provenance.** `CreateCreatureAgent(AgentType type, uint32 mapId, uint64 spawnId)` vyžaduje typ a `LoadAgents()` `agent_type` načítá jako součást persistentní identity; dnes existují jen `Civilian`/`Guard`/`Merchant`. `world.creature` ale obsahuje wolves/beasts, bosses, quest creatures atd. — bulk bootstrap nesmí prostě všechno uložit jako `Civilian`. Před spuštěním `2.12F4B` je nutné explicitně rozhodnout jednu variantu:

- rozšířit identity model o obecnější typ, nebo
- mít deterministickou classification policy (např. z `creature_template`), nebo
- oddělit základní Creature-agent identitu od gameplay/archetype klasifikace.

Žádná heuristická ani fingovaná provenance — nejasný typ musí být explicitní `Unknown`/quarantine stav, ne tichý default na `Civilian`.

Kroky:

- `world.creature` (non-instance/base-world scope — `MapEntry` existuje a `Instanceable() == false`, ne `FindBaseNonInstanceMap()`, viz Scope predicate výše) je source of truth, žádné fake/synthetic spawny;
- reconciliation je idempotentní: existující `(map_id, spawn_id) → AgentId` binding se zachová, nový spawn dostane `AgentId == SpawnId` (viz 2.12F4A2, ne nově generovanou hodnotu) do `ObserveOnly`, restart nevytvoří duplicity;
- chybějící/odstraněné spawny se detekují a fail-closed karanténují, ne tiše zůstávají v `_registry` jako živý agent;
- deterministická `AgentType`/provenance politika je rozhodnutá a implementovaná před prvním bulk bootstrapem;
- temporary summons bez persistentního `SpawnId` se do reconciliace nepočítají;
- reconciliation běží jako bounded/administered krok (startup nebo explicit admin trigger), ne jako recurring per-tick world-thread práce.

Runtime gate: opakovaný restart nad stejným `world.creature` datasetem nikdy nevytvoří duplicitní `AgentId`, nikdy nevynechá nový spawn přidaný mezi restarty, a smazaný spawn nikdy nezůstane v `_registry` jako platný `AgentRecord`.

**Implementační stav:** engine je hotový (`ad80db5949` + STATIC review fixy `149e400927`, `326e7a7b19`) — census/diff/reconcile, fail-closed `AgentId`/`agent_id` collision handling, instance/out-of-scope rozlišení od skutečně smazaných spawnů, i sám bulk insert (chunked multi-row `INSERT`, ne per-row). Gated za `AIWorld.EnableSpawnReconciliation` (default 0).

**Runtime evidence (už naměřeno, ne odhad):** engine byl reálně spuštěn nad celým světem (`AIWorld.EnableSpawnReconciliation = 1`, bez zone scope) a naměřil `census=128849 valid=128849 identity mismatch=0 ObserveOnly=128845 Controlled=4` — identitní invarianty (`AgentId == SpawnId`, no ghosts, no duplicates) tedy drží i při plné světové populaci. Zároveň ale způsobil viditelné runtime zpomalení worldserveru/NPC processing. `2.12F4B2` proto NENÍ "first population proof" — ten už proběhl a byl to global stress experiment, který skončil performance FAIL před `2.12F4C`. `2.12F4B2` je navazující, bounded locality proof nad Elwynn: zjistit, jestli current dev scope (menší, ale reálná populace) funguje bez `2.12F4C`. I kdyby Elwynn proof prošel čistě, `2.12F4C` zůstává povinný krok před jakýmkoli eventual full-world rolloutem — Elwynn úspěch neznamená přeskočit scale hardening, jen dává informaci o tom, jak naléhavý/velký musí být.

## 2.12F4B2 — Scoped rollout proof (Elwynn, zoneId 12)

**Stav: CLOSED / STATIC + BUILD + RUNTIME PASS (`e5f5043463`, `fb11e29b18`).** Finální F4B2 eligible census = `3540`, přesně odpovídá raw `zoneId = 12` počtu — rozdíl je `0`, žádná dodatečná non-instance/eligibility filtrace ho nezmenšila. `3540` nových `AgentRecord`ů vytvořeno jako `ObserveOnly`, identity invarianty (`AgentId == SpawnId`, no duplicates) drží, restart/idempotence ověřeno (druhý běh: `missing=0 created=0`), mimo-Elwynn populace nedotčena (`OutOfScopeCount` mechanismus).

**Priorita: až po 2.12F4B, před rozhodnutím o rozsahu 2.12F4C.**

**Vstupy (už naměřeno):** `world.creature` total `151822`; raw `zoneId = 12` (Elwynn) `3540`; `2.12F4B` full-world census `128849`, `PERFORMANCE FAIL`; současný `ai_agents` `4` řádky; reconciliation `OFF`.

Cesta:

```text
2.12F4B reconciliation engine (hotovo, runtime evidence: 128849 agentů, performance FAIL)
    ↓
TrinityCore Zone/Area data preparation
    ↓
2.12F4B scoped reconciliation: Elwynn zoneId=12
    ↓
runtime test nad skutečnou Elwynn populací
    ↓
podle výsledku rozhodnout rozsah 2.12F4C
```

**Zone/Area data preparation.** `creature.zoneId`/`areaId` existují jako sloupce ve world DB schema, ale `ObjectMgr::LoadCreatures()` je nenačítá do `CreatureData` a jsou populované jen pokud na dané DB někdy proběhl `Calculate.Creature.Zone.Area.Data = 1` (TrinityCore core config, ne AIWorld). Bez populovaných sloupců je jediná cesta k zone/area `Map::GetZoneAndAreaId()`, která vyžaduje live `Map*` (`sMapMgr->CreateBaseMap()`) a čte terrain/vmap data z disku — to by do census pipeline poprvé zavedlo přesně tu závislost (live `Map*`, per-spawn I/O), které se `CreatureSpawnCensus` dosud důsledně vyhýbal. Proto:

- scoped census čte `creature.zoneId` přímo přes vlastní úzký `WorldDatabase` SELECT (`SELECT guid FROM creature WHERE zoneId = ?`), nikdy nepočítá zone/area za běhu přes `Map::GetZoneAndAreaId()`;
- **precondition není jen "je populovaný", ale "je aktuálně přepočítaný pro současný stav `world.creature`" (P2 fix, STATIC review):** `Calculate.Creature.Zone.Area.Data = 1` dopočítá `zoneId`/`areaId` jen pro spawny, které `ObjectMgr::LoadCreatures()` právě načítá při daném běhu s daným configem - novější `world.creature` řádek přidaný PO posledním takovém běhu bude mít `zoneId = 0`, a `WHERE zoneId = 12` ho tiše vynechá. Výsledek pak není prázdný (což by bylo bezpečně nápadné), ale částečný a zdánlivě validní - to je horší než žádný výsledek. Administrativní pravidlo pro `2.12F4B2` test: bezprostředně před testem jednorázově spustit `Calculate.Creature.Zone.Area.Data = 1`, restartovat, ověřit v DB že cílové `world.creature` řádky mají nenulový `zoneId`, teprve pak spustit scoped reconciliation - a `Calculate.Creature.Zone.Area.Data` po testu zase vypnout (není to AIWorld config, nemá důvod zůstat trvale zapnutý kvůli tomuto testu).

Kroky:

- nová config hodnota `AIWorld.SpawnReconciliationZoneId` (default `0`), skládá se s existujícím `AIWorld.EnableSpawnReconciliation` - **fail-closed kombinace (P2 fix, STATIC review), ne implicitní fallback na global**:

  ```text
  EnableSpawnReconciliation = 0
      → OFF, žádná reconciliation (dnešní stav)

  EnableSpawnReconciliation = 1
  SpawnReconciliationZoneId = nenulová hodnota
      → scoped reconciliation jen nad tímto zoneId

  EnableSpawnReconciliation = 1
  SpawnReconciliationZoneId = 0 / neuvedeno
      → REFUSE: log ERROR, reconciliation se vůbec nespustí
  ```

  Důvod: než `2.12F4C` scale hardening existuje, nesmí být "zapomenout nastavit ZoneId" dost na to, aby se znovu spustila plná globální reconciliation (~128849 agentů) - přesně ten běh, který už reálně způsobil runtime zpomalení. Globální (`ZoneId = 0` jako explicitní "žádný scope") režim dostane samostatný, explicitní override (např. `AIWorld.SpawnReconciliationAllowGlobal`), zavedený až spolu s `2.12F4C` nebo `2.12F4D`, ne jako tichý default cesta dnešního přepínače;
- scoped census: eligible množina (`BuildCreatureSpawnCensus()`) se před předáním do `BuildReconciliationPlan()` protne s množinou guidů z `WHERE zoneId = ?`;
- `allKnownSpawnIds` (`BuildAllKnownCreatureSpawnIds()`) zůstává **neomezená** (žádný zone filtr) — existující `OutOfScopeCount` mechanismus (viz `2.12F4B` P2 fix `326e7a7b19`) tak správně pokryje i "existuje, ale mimo dnešní zone scope" bez jakékoli změny `BuildReconciliationPlan()`'s vlastní logiky: mimo-Elwynn agent nikdy neskončí jako `Orphaned`;
- žádná změna `ControlMode` chování — nové Elwynn agenty vznikají stejně jako dnes, `ObserveOnly`.

**Acceptance nesmí být `SELECT COUNT(*) FROM ai_agents` (P2 fix, STATIC review):** `ai_agents` legitimně obsahuje i historické 4 controlled agenty, mimo-Elwynn agenty, out-of-scope řádky a quarantined řádky (reconciliation je fyzicky nemaže) - celkový počet tedy obecně `≠` počet Elwynn agentů. Acceptance musí porovnávat identity sety, ne agregovaný počet:

```text
A = eligible world.creature se zoneId = 12 (podle stejného predikátu jako scoped census)
B = odpovídající ai_agents řádky (AgentId == SpawnId, MapId shoduje)

A - B = 0
a žádný NOVĚ vytvořený ai_agents řádek nepatří mimo A
```

Prakticky: uložit baseline `ai_agents` (nebo alespoň jeho identity set) před testem, po reconciliation spočítat deltu a tu porovnat proti `A`, ne spoléhat na jedno absolutní `COUNT(*)` číslo.

Runtime gate: se `AIWorld.EnableSpawnReconciliation = 1` a `AIWorld.SpawnReconciliationZoneId = 12` vytvoří reconciliation právě množinu `A` (viz acceptance výše) jako nové `AgentRecord`y, žádný mimo-Elwynn spawn se nedotkne, vanilla/scripted AI beze změny, a naměřený per-tick world-thread dopad nad touto menší, ale reálnou populací je vstup pro rozhodnutí o skutečném rozsahu/prioritě `2.12F4C` - `2.12F4C` samo zůstává povinné před jakýmkoli budoucím full-world rolloutem bez ohledu na výsledek Elwynn testu.

## 2.12F4B3 — Scoped Control activation (Elwynn, zoneId 12)

**Stav: CLOSED / STATIC + BUILD + RUNTIME PASS (`32e5df3793`, `a4b093afb2`, `c1fc0792b9`).** `2.12F4B2` samo o sobě je bootstrap/identity/scope proof, ne cílový stav — nové agenty vznikají bez `ControlMode`, tedy schema default `ObserveOnly`. Cílový stav pro jednu lokaci je celá scoped populace `AIWorldControlled`, dosažená bezpečnou/opakovatelnou aktivační cestou v AIWorld kódu, ne jednorázovým ručním `UPDATE ai_agents SET control_mode=1`.

**Priorita: až po 2.12F4B2, před rozhodnutím o rozsahu 2.12F4C.**

```text
2.12F4B2 — world.creature zoneId=12 → 3540 AgentRecords (ObserveOnly)
    ↓
2.12F4B3 — explicit zone control activation
    ↓
3540 AgentRecords AIWorldControlled
    ↓
AIWorld owns those Creatures
    ↓
Perception / Needs / Decision / ActionSystem
```

Kroky:

- nová config hodnota `AIWorld.EnableZoneControlActivation` / `AIWorld.ControlZoneId`, stejná fail-closed kombinace jako `2.12F4B2` (`Enable=1` + `ZoneId<=0` → `REFUSE`, žádný unscoped/global promotion fallback);
- promotion set je přesně stejný scoped identity set, který `2.12F4B2` prokázala: `world.creature zoneId=12 ∩ persistent non-instance F4B eligibility ∩ AgentId == SpawnId ∩ existující AgentRecord` — nepromuje se prostě celé `ai_agents`;
- `AgentPersistence::PromoteControlModeBatch()` — `SetControlMode()` dělá jeden `UPDATE` + jeden readback na agenta, což se nedává do smyčky přes celou scoped populaci; batch promotion s jedním bulk readbackem, stejně jako batch creation v `2.12F4B`;
- **fail-closed whole-zone, ne partial (P2 fix, STATIC review):** pokud jediný scoped-eligible spawn nemá validní `AgentRecord` ještě (reconciliation pro danou zónu neproběhla), aktivace se vůbec nespustí (`ERROR` log, žádný `UPDATE`) — nikdy nepromuje jen ty kandidáty, které vyšly, a nenechá zbytek `ObserveOnly` beze změny;
- **atomicita samotného DB zápisu (P2 fix, STATIC review):** `PromoteControlModeBatch()` chunkuje `UPDATE` po `1000` id, ale všechny chunky jsou v jedné `CharacterDatabaseTransaction`/`DirectCommitTransaction()` — selhání jednoho chunku po commitu předchozích by jinak mohlo zanechat přesně ten mixed `Controlled`/`ObserveOnly` stav, kterému má whole-zone guard výše zabránit;
- `ActionSystem::Validate()`/`OwnsSpawn()` (`2.12F4A`) se neupravují — aktivace je nový orchestration entrypoint, který jim posílá víc agentů, ne nový safety mechanismus.

Acceptance (naměřeno):

```text
DB:
total Elwynn agents      3540
Controlled                3540
ObserveOnly                  0
AgentId != SpawnId            0
outside-zone promoted         0

runtime:
3540 agentů load Controlled
server ready
no ownership outside Elwynn
```

Runtime gate: `3540 / 3540 AIWorldControlled` pro Elwynn, scope správný (žádný mimo-Elwynn agent), identita správná (`AgentId == SpawnId`), restart/idempotence `PASS`, runtime přijatelný — toto je výrazně náročnější performance test než samotné `2.12F4B2` (tam šlo primárně o velikost registry/persistence u `ObserveOnly` populace; zde se poprvé zatíží decision/needs/perception/action cesty pro celou lokaci).

## 2.12F4C — bounded/indexed runtime at world scale

**Stav: NOT STARTED. Priorita přehodnocena (STATIC review nad `c1fc0792b9`): `2.12F4C` je world-scale hardening (O(1) index, odstranění recurring full-registry scanů) motivovaný eventual populací kolem `128849` agentů, ne blocker pro jednu lokaci. `2.12F4B3`'s runtime gate (`3540 / 3540 AIWorldControlled`, decision/needs/perception/action cesty pod skutečnou zátěží) prakticky ověřil, že pro rozsah jedné lokace (Elwynn, `3540` agentů) `2.12F4C` blocker není. `2.12F4C` proto NENÍ další povinný krok před pokračováním práce nad Elwynn populací (např. `2.12G1` druhý coalition profil) — zůstává povinný teprve před jakýmkoli budoucím rozšířením na další lokace/eventual full-world rollout (`2.12F4D`), kde už `2.12F4B`'s vlastní full-world experiment (`census=128849`, `PERFORMANCE FAIL`) ukázal, že current architecture bez `2.12F4C` neobstojí.**

- minimálně O(1) index `(mapId, spawnId) → AgentId` místo současného lineárního `FindBySpawn()`, který dnes prochází celý `_agents` map;
- odstranit recurring full-registry discovery z world-thread cest (scheduler discovery, world-event perception, nearby perception, needs update, coalition candidate discovery) a nahradit bounded cursory / materialized indexes;
- práce za tick nesmí být úměrná celkovému počtu creature ve světě, jen počtu skutečně `AIWorldControlled`/decision-eligible agentů.

Runtime gate: recurring world-thread práce musí zůstat bounded a nesmí růst úměrně s celkovou `AgentRegistry` populací - `2.12F4B3` už ověřil Elwynn scoped populaci (`3540`, `PASS`); zbývá ověřit eventual full-world dataset (aktuálně ~`128849` persistentních non-instance agentů v tomto TDB, kde `2.12F4B`'s vlastní experiment už ukázal `PERFORMANCE FAIL` bez tohoto hardeningu) - ne jedno číslo předem odhadnuté.

## 2.12F4D — Global bootstrap/runtime proof

**Priorita: až po scale gate (2.12F4C).**

- skutečné spuštění plné `ObserveOnly` populace (2.12F4A+B) v runtime;
- teprve po `2.12F4C` lze přepnout větší populace z `ObserveOnly` na `AIWorldControlled`;
- scripted/boss/pet/special TrinityCore AI musí mít explicitní policy (allowlist/denylist nebo ekvivalent), aby nebyl nechtěně přebit jen proto, že má `AgentId`;
- přechod z `ObserveOnly` na `AIWorldControlled` je vždy explicitní administered krok, nikdy implicitní vedlejší efekt existence `AgentRecord`.

Runtime gate: globální reconciliation proběhla, `ObserveOnly` populace neovlivnila vanilla/script chování (žádný `REACT_PASSIVE`, žádné `MoveIdle()`, žádná ztráta scripted AI), vybraná `AIWorldControlled` podmnožina funguje stejně jako dnešní 4 testovací mobky, a runtime práce zůstává bounded i s plnou populací.

---

# AFTER 2.12F4 — 2.12G Genericity proof a další group behavior

## 2.12G1 — druhý skutečný coalition profile přes stejnou pipeline

**Stav: CLOSED / STATIC + BUILD + RUNTIME PASS.**

**Priorita: NEXT po 2.12F4.**

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

**Vybraný profil (podle reálných dat):** `DefiasLoose`, `CreatureEntry = 38` (Defias Thug), `71` skutečných Elwynn (`zoneId = 12`) spawnů - vybráno z `SELECT ... FROM creature JOIN creature_template ... WHERE zoneId = 12 GROUP BY id HAVING COUNT(*) >= 3` nad reálným world DB, ne odhadem. `WolfLoose` zůstává beze změny jako první historický fixture/profile (jeho vlastní runtime proof z dob F/E/F1-F3 používal `CreatureEntry = 1423`, Stormwind Guard, protože skuteční wolf NPC v té době ještě nebyli `AIWorldControlled` - to není důvod k migraci/redesignu WolfLoose teď).

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

**Runtime evidence (potvrzeno):** WolfLoose i DefiasLoose mají reálné persistentní `AgentGroup`y (skuteční Elwynn members, ne ghost/test agenti), se správným persistentním `ProfileId`, vytvořené přes stejný generic `RunCoalitionFormation`/maintenance/coordination path a stejný intent/projector/`ActionSystem` pipeline. Identita/provenance obou skupin drží beze změny po restartu. Žádná species-specific orchestration větev (`RunWolfFormation()`, `RunDefiasFormation()` apod.) nevznikla.

```text
2.12G1 STATIC   PASS
2.12G1 BUILD    PASS
2.12G1 RUNTIME  PASS (WolfLoose + DefiasLoose, restart-stable)

2.12G1 = CLOSED
```

## 2.12G2 — generic ROAM / territory movement intent

**Stav: CLOSED / STATIC + BUILD + RUNTIME PASS.**

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

**Runtime evidence (potvrzeno):** pure smoke `PASS` (Evaluate/Project, deterministic target, REGROUP > ROAM priority, fail-closed unknown intent); reálný runtime `PASS` pro WolfLoose (group `16`) i DefiasLoose (group `15`) — společný deterministický ROAM target, individual `MOVE_TO` request per member, validace přes `ActionSystem`, stabilní `ARRIVED`, žádný viditelný duplicate movement spam. Po testu byl systém vrácen do defaultního disabled stavu (`AIWorld.*GroupRoamEnabled = 0`).

**Lifecycle runtime evidence (potvrzeno):** `2.12G2R` deterministicky ověřilo higher-priority individual preemption, manual leave a manual dissolve během skutečně aktivního ROAM. Ve všech třech případech produkční ownership/lifecycle cesta zastavila původní `MOVE_TO`, vyčistila `GroupCoordinationGoalState` i engine movement generator a zachovala persistence/registry invarianty.

```text
2.12G2 STATIC        PASS
2.12G2 BUILD         PASS
2.12G2 WOLF ROAM     PASS
2.12G2 DEFIAS ROAM   PASS
2.12G2 PREEMPTION    PASS
2.12G2 LEAVE         PASS
2.12G2 DISSOLVE      PASS
2.12G2 SAFE-OFF      PASS
2.12G2 DB INTEGRITY  PASS

2.12G2 = CLOSED
```

## 2.12G2R — ROAM lifecycle runtime closure

**Stav: CLOSED / STATIC + BUILD + RUNTIME PASS.**

Tři defaultně vypnuté one-shot hooky ověřily lifecycle a ownership během skutečně aktivního ROAM:

### Higher-priority individual preemption

- agent `80237`, group `36`;
- hook počkal na skutečně aktivní ROAM;
- produkční `GET_FOOD` goal preemptoval group coordination;
- původní ROAM skončil jako `COORDINATION_PREEMPTED_BY_GOAL`;
- zaznamenaný preemptor byl skutečný individual `GET_FOOD`;
- coordination state i engine movement generator byly úplně vyčištěny.

### Manual leave during active ROAM

- agent `80209`, group `32`;
- hook počkal na skutečně aktivní ROAM;
- zavolal produkční `RequestLeaveGroupWithPolicy(..., Manual)`;
- aktivní movement skončil jako `COORDINATION_STOPPED_BY_LIFECYCLE`;
- coordination state i engine movement generator byly vyčištěny;
- persistence potvrdila odstranění membership;
- group `32` zůstala konzistentní se členy `80210, 80224, 80226`.

### Manual dissolve during active ROAM

- group `36`, členové `80237, 80256, 80257`;
- hook počkal, až všichni tři členové měli skutečně aktivní ROAM;
- zavolal produkční `RequestDissolveGroupWithPolicy(..., Manual)`;
- všechny tři movement akce skončily jako `COORDINATION_STOPPED_BY_LIFECYCLE`;
- coordination state i engine movement generátory byly vyčištěny;
- persistence potvrdila úplné odstranění group `36` i jejích membership edges.

### Safety a databázová postcondition

- žádný fatal, assert, segmentation fault, DB failure ani unknown config;
- group `36` po dissolve neexistuje;
- nezůstalo žádné osiřelé membership;
- group `32` zůstala konzistentní;
- všechny testovací hooky byly po proofu vráceny na `0`.

```text
2.12G2R PREEMPTION    PASS
2.12G2R LEAVE         PASS
2.12G2R DISSOLVE      PASS
2.12G2R PERSISTENCE   PASS
2.12G2R DB INTEGRITY  PASS
2.12G2R SAFE-OFF      PASS

2.12G2R = CLOSED
```

Po proofu musí zůstat:

```ini
AIWorld.TestPreemptOnActiveRoamAgentId = 0
AIWorld.TestLeaveOnActiveRoamAgentId = 0
AIWorld.TestDissolveOnActiveRoamGroupId = 0
```

## 2.12G3 — generic HUNT / coordinated combat preparation

**Stav: NOT STARTED.**

Až po stabilním G1/G2 (tedy až po `2.12G2R` closure výše) — první commit ještě neútočí, jen navrhuje kontrakt.

Nejdřív navrhnout explicitní server-owned combat proposal/action contract. Group nesmí přímo volat combat API nad více Creatures.

```text
Group HuntIntent
    ↓
explicit target identity/provenance
    ↓
per-member HuntProposal
    ↓
ActionRequest
    ↓
target/membership/map/range revalidation
    ↓
TrinityCore execution
```

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

**Doporučené rozdělení (ne nutně samostatné milníky, ale samostatné commity/review kroky):**

- **G3A** — pure DTO a target provenance contract (`HuntIntent`/`HuntProposal` shape, žádná orchestrace);
- **G3B** — intent/projector a pure smoke testy (stejná disciplína jako `2.12F1`/`2.12F2`);
- **G3C** — per-member validation a ownership/preemption (stejná disciplína jako `2.12F2`/`2.12F3`);
- **G3D** — runtime proof: target death/unload/leave/dissolve během aktivního HUNT bezpečně ruší stale group-owned intent.

## 2.12G4 — roles / leader pouze pokud je skutečně potřeba

**Stav: NOT NEEDED YET.** `2.12G2`'s dosavadní ROAM runtime evidence neprokázala žádnou potřebu leadera/role — shared deterministic target + generic per-member proposal stačí. G4 se nezačíná preventivně jen proto, že G2/G3 existují.

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

- [x] global agent population foundation gate pro jednu lokaci (2.12F4A–F4B3: `ControlMode` split, TrinityCore-aligned `AgentId == SpawnId` identity, bidirectional spawn reconciliation, scoped Elwynn population + full Control activation — `3540 / 3540 AIWorldControlled`, STATIC + BUILD + RUNTIME PASS) — `2.12F4C`/`2.12F4D` (world-scale hardening, eventual full-world bootstrap) zůstávají otevřené, ale nejsou blocker pro second-profile proof nad již reálnou, reconciled Elwynn populací;
- [x] second-profile genericity proof;
- [x] alespoň jedno další skutečné generic group behavior potřebné pro emergentní slice;
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
2. [x] **2.12F4A — ControlMode foundation (`ObserveOnly` vs `AIWorldControlled`, hard-gated na decision/routine/group/action cestách, existing 4 → `AIWorldControlled`, default = `ObserveOnly`).**
3. [x] **2.12F4A2 — TrinityCore-aligned Agent identity (`AgentId == SpawnId` pro persistentní non-instance Creature agenty, migrace existujících 4 + navazujících group/memory FK, před bulk bootstrapem v 2.12F4B).**
4. [x] **2.12F4B — global spawn reconciliation (non-instance `world.creature` ↔ `ai_agents` bidirectional, fail-closed na smazané spawny, deterministická `AgentType` provenance, no ghosts). Runtime-ověřeno nad plnou světovou populací (`128849`, `PERFORMANCE FAIL` bez scale hardeningu) i scoped nad Elwynn.**
5. [x] **2.12F4B2 — scoped rollout proof (Elwynn, `zoneId=12`) - `3540` eligible `AgentRecord`ů, `ObserveOnly`, identity/scope/idempotence `PASS`.**
6. [x] **2.12F4B3 — scoped Control activation (Elwynn) - `3540 / 3540 AIWorldControlled`, fail-closed whole-zone + atomická DB promotion, decision/needs/perception/action runtime `PASS`.**
7. [ ] 2.12F4C — bounded/indexed runtime at world scale (O(1) spawn index, remove recurring full-registry scans) - **DEFERRED, ne blocker pro pokračování nad Elwynn**; povinné před rozšířením na další lokace/eventual full-world (2.12F4D).
8. [ ] 2.12F4D — global bootstrap/runtime proof (plná `ObserveOnly` populace, vanilla/script chování beze změny, bounded work) - až po 2.12F4C.
9. [x] 2.12G1 — second real coalition profile přes stejnou generic pipeline (nad reálnou reconciled Elwynn populací z 2.12F4B2/F4B3).
10. [x] 2.12G2/G2R — generic ROAM včetně preemption/leave/dissolve lifecycle closure.
11. [ ] 2.12G3A — pure HUNT DTO a explicitní target provenance contract.
12. [ ] 2.12G4 — roles/leadership pouze pokud G2/G3 prokáže potřebu.
13. [ ] 2.13A — actual local LLM inference path.
14. [ ] 2.13B — structured `QuestProposal` + authoritative validation.
15. [ ] 2.13C — player-facing dynamic task lifecycle.
16. [ ] 2.13D — `WORLD → NPC → LLM → PLAYER → WORLD` runtime gate.
17. [ ] 2.14 — emergent end-to-end world event slice.
18. [ ] 2.15 — remaining diagnostics/scale hardening needed by measured runtime behavior.
19. [ ] Etapa 3 — Elwynn census + semantic locations + faction/world-data preparation.
20. [ ] Etapa 4 — Living World composition.

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

**2.12F4A–F4B3 je CLOSED pro jednu lokaci (Elwynn) — STATIC + BUILD + RUNTIME PASS, `3540 / 3540 AIWorldControlled`.** Cesta, kterou tento gate prošel:

```text
ControlMode schema + ActionSystem::Validate() as mandatory authoritative gate (2.12F4A)
    ↓
existing 4 test mobs → AIWorldControlled, default → ObserveOnly
    ↓
AgentId == TrinityCore Creature SpawnId for persistent non-instance agents (2.12F4A2)
    ↓
existing 4 test mobs migrated to spawn-aligned AgentId (incl. group/memory FKs)
    ↓
ALL non-instance world.creature SPAWNS reconciled ↔ ai_agents (2.12F4B)
    ↓
missing spawn → CREATE (ObserveOnly); deleted spawn → fail-closed/quarantine, no ghosts
    ↓
existing vanilla/scripted AI unaffected for ObserveOnly
    ↓
measured global run: 128849 agents, identity invariants hold, but world-thread performance FAIL
    ↓
scoped reconciliation over real Elwynn (zoneId=12) population only - 3540 AgentRecords, ObserveOnly (2.12F4B2)
    ↓
scoped Control activation over the same Elwynn population - 3540 / 3540 AIWorldControlled (2.12F4B3)
    ↓
decision/needs/perception/action runtime PASS over the real Elwynn location
```

`2.12F4C`/`2.12F4D` (O(1) spawn index + bounded recurring work, then global bootstrap/selective rollout) zůstávají **DEFERRED** — `2.12F4B3`'s vlastní runtime gate prakticky ověřil, že pro rozsah jedné lokace nejsou blocker; zůstávají povinné teprve před rozšířením na další lokace nebo eventual full-world rollout (kde `2.12F4B`'s vlastní `128849`-agent experiment už ukázal `PERFORMANCE FAIL` bez nich).

S touto foundation lze bezpečně stavět druhý coalition profil nad skutečnými, už `AIWorldControlled` spawny (`2.12G1`), ne synteticky vyrobenými:

```text
REAL SECOND PROFILE (nad 2.12F4 reconciled spawny)
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

Jakmile i tohle projde bez druhé orchestration větve, máme prakticky potvrzené, že `AgentGroup` je skutečně obecná coalition vrstva a můžeme bezpečně stavět další behavior (`ROAM`, následně hunt/combat) nad touto základnou.
