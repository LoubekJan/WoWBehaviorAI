# WoWBehaviorAI

An authored living world for **TrinityCore 3.3.5a**, with deterministic NPC
behavior and optional LLM characters that can act and talk in the game.

## Current state — 31 October 2026

The active branch is **`AI-World-lab`**. One canonical stack runs realm **2**,
map **725**, area **4988**, using the authored **`preview-v8`** map and
**`full-v8`** population. All **100 NPCs** (`901000`–`901099`) are controlled
by AIWorld; there are **0 native-only NPCs** in that population. AIWorld
ownership does not automatically give an NPC an LLM card.

The original Elwynn realm is retired. Existing login accounts and realm-2
characters were preserved. Root `compose.yml`, root `.env` and `Makefile`
are the current entry points; the old Compose names are compatibility
wrappers, not separate worlds.

| Service | Purpose | Host port |
| --- | --- | --- |
| `authserver` | Login and realm list | 3724 |
| `worldserver` | Authored game world | 9086 |
| `world-viewer` | Live NPC telemetry and map | 9091 |
| `character-studio` | Character appearance, personality and backstory editor | 9092 |
| `account-web` | Game account registration | 9088 |
| `auth-mysql`, `lab-mysql`, `ai-server` | Accounts, world/characters and AI protocols | Internal only |

`tc-dev` is a tools container, and `aiworld-recorder` is an optional recording
service. Neither is a permanent ninth game service. The LLM backend is
configured separately from these eight services.

**Defaults differ from the deployed world.** `.env.example` intentionally
selects `settlement-v8` with LLM requests disabled. The verified deployment
uses `full-v8`, with three explicitly installed LLM cards and the existing
Qwen backend. Copying the example does not reproduce private credentials,
authored cards, extracted map data or model settings.

## Implemented behavior

- Wildlife: deer forage; bears hunt, eat and return through validated movement.
- Food supply: human and gnoll hunters gather finite meat, carry it and
  deliver it to stores. Residents consume actual stored portions.
- Farming: twelve workers use authored fields and village stores;
  incomplete work does not create food.
- Defias: ten raiders can form parties of 2–5 and steal physical food;
  six camp residents consume supplies. Guards can interrupt raids.
- City supply: two carrier/escort pairs transport food from village stores
  to the city store, which supplies two city residents.
- Ordinary guards, service NPCs and other inhabitants retain their reviewed
  roles and game service flags within the same 100-NPC population.
- Optional LLM characters: needs and observations inform validated actions,
  movement, emotes, player chat and conversations between nearby characters.
  Individual cards supply names, personalities and backstories.
- Character Studio: saved drafts, humanoid customization and equipment,
  local client-data previews, character-card generation and complete exports.
- Independent native templates: offline deployment creates a complete clone
  with its own name and subtitle, preserving Spawn ID, home and source-based
  behavior. Other NPCs sharing the source template keep their original names.

Saving a Studio draft does not change the live game. Its full ZIP contains
the card, appearance, native-template descriptor, draft and deployment guide.
A controlled deployment installs these inputs. Log into WoW again after a
name change so the client loads the updated label cache. Character changes
require no additional map patch; geometry and shared DBC changes still need
matching client and server data.

Food, hunger/fatigue, crops, raid experience/parties and caravans currently
reset on worldserver restart. Persisting these living-world systems is
explicitly deferred. Persistent accounts, characters, AI identity/memory
infrastructure and saved Studio drafts are separate concerns.
The LLM controller's dialogue history and episodic memories are currently
runtime-only too; they are not saved by the older AI memory infrastructure.

Territory capture, a complete faction economy and long-term balance across
the whole population remain future work. Short acceptance recordings do not
prove those larger outcomes.

## Documentation

| Guide | Contents |
| --- | --- |
| [Development and operations](README_DEV.md) | Installation, configuration, build, deployment and backups |
| [Realm and map](doc/RealmLab.md) | Current map, population profiles and physical acceptance boundaries |
| [Current roadmap](AIWorld_Current_Roadmap.md) | Completed work, verified baseline, TODO and research history |
| [Character Studio](doc/CharacterStudio.md) | Appearance, backstory, full exports and installation |
| [LLM NPC agents](doc/NpcAgents.md) | Cards, actions, dialogue, models and native identity |
| [Map data index](data/realm_lab/aiworldlab/README.md) | Current manifests and earlier geometry experiments |
| [Observer](docker/world-viewer/README.md) and [recordings](doc/ObserverRecording.md) | Telemetry, recording and evaluation limits |
| [Living roles](doc/LivingRoles.md) and [movement timing](doc/NPCMovementTiming.md) | Ownership, scheduling and diagnostics |
| [Recovery planning](doc/RecoveryPlanning.md), [navigation pilot](doc/RecoveryNavigationPilot.md), [hunt regressions](doc/HuntReturnRegression.md) | Recovery design and scoped tests |
| [Deploy runner](deploy/runner/README.md) and [runtime scripts](docker/scripts/README.md) | CI/CD infrastructure and script responsibilities |

The dated [August status](AIWorld_Status_2026-08-25.md),
[original lab plan](doc/archive/RealmLab-2026-10-11.md), Elwynn census and
earlier return/hunt profiles are historical references. They do not define
the current population or deployment procedure. Upstream TrinityCore and
dependency documentation retain their upstream scope.

## Architecture

**AI proposes. Server validation authorizes. TrinityCore executes.**

```text
World observations and events
    -> perception, needs and memory
    -> deterministic role or explicitly enabled LLM controller
    -> validated action request
    -> TrinityCore movement, combat, activity or dialogue
    -> observed result and memory
```

Live `Creature*`, `Player*`, `Map*` and `Unit*` never cross an asynchronous
inference boundary. Workers receive value DTOs; replies are checked and
consumed on the world thread. A model cannot mutate SQL directly or bypass
movement, target, scope and action constraints. AIWorld does not force-load
grids merely to simulate an agent.

`AgentId` matches the persistent Spawn ID. Runtime GUIDs identify a particular
materialization and can change after restart or respawn. Groups coordinate
individual agents; they never replace several creatures with an aggregate mob.

The older remote `/decision` path remains a separate dry-run protocol.
Live `/npc-agent` character control and the earlier `/dynamic-task` quest
vertical slice have their own validation and ownership. These are not one
globally enabled inference switch.

Dedicated template entries supply game identity; reviewed source entries
retain behavioral classification. Telemetry `entry` preserves that source
meaning, while `template_entry` reports the actual native entry. Native
template names supply the observer and game client, not only the LLM prompt.

## Verification baseline

Revision **`664d84687d7a`** was built and deployed on 31 October 2026 through
[Realm Lab CI/CD](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/41158138495):
**846 Python tests** and **699 native tests** passed. Post-deployment checks
found eight healthy services, 100 controlled NPCs, three verified independent
templates and all three installed LLM controllers active. Exports, source
preservation and actual native GUID entries were also checked.

This is a dated baseline, not a permanent test count or proof of every
physical behavior. Earlier wildlife, supply, consumer and raid recordings
remain valid only for their revisions and geometry. Full-population and
caravan acceptance need suitable recording gates. New nameplates and the
rendered editor were not visually inspected in this verification.

## Build and operation

Follow [README_DEV.md](README_DEV.md) before a fresh installation. Client
3.3.5a data, the matching authored-map bundle and private configuration are
required and are not distributed in Git.

Use `make config` to check the canonical configuration. Local `make build`
and `make test` use the deployment build volume: **stop both worldserver
and authserver before replacing their shared binaries**, as described in
the operations guide. Normal server changes use gated CI/CD, which performs
this stop itself. Do not manually build into volumes used by running
servers or trusted CI.

`make start` starts the canonical stack after preflight. `make backup` writes
protected account and world/character backups under `runtime/lab/backups`.
Do not repeat the TDB import to deploy a code or character change.

Current logs are in `runtime/lab/logs/Server.log`; `make world-logs` follows
container output. Recording is opt-in: `make record`, `make record-status`,
`make record-stop`. A generic analysis PASS does not accept every role.

## Repository layout

```text
src/server/game/AIWorld/    AIWorld, roles, inference, dialogue and telemetry
docker/                    services, Character Studio, Observer and scripts
deploy/                    server configuration and deploy runner
tools/realm_lab/           reviewed profiles, installers and acceptance tools
data/realm_lab/            map/population manifests and dated evidence
data/npc_agents/           example cards and operator-installed private cards
data/npc_appearances/      operator-installed appearance files
data/npc_templates/        operator-installed native-template descriptors
sql/                       TrinityCore and project database updates
runtime/lab/               private data, logs, recordings, receipts and backups
compose.yml                canonical stack and optional tools
Makefile                   current operator entry points
```

Installed private authoring JSON is not automatically ignored by Git.
Keep it out of commits; the tracked sample cards are separate from private
server inputs. Credentials and private binary/runtime data belong outside
the public repository.

## License

The underlying TrinityCore project is licensed under **GPL-2.0**.
See [COPYING](COPYING) and [AUTHORS](AUTHORS).
