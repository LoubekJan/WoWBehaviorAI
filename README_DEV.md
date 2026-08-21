# AI TrinityCore — Development Setup

Implements Etapa 1 (Development Infrastructure) of
[AI_TrinityCore_Roadmap_Etapa_1_2.md](AI_TrinityCore_Roadmap_Etapa_1_2.md).

## Origin

- Base: `TrinityCore/TrinityCore`, branch `3.3.5`.
- Upstream commit this project started from: `2a64b72689cc8d797e4c93a0c96dfa2dc06f64c8`
  ("Core/Misc: Reduce differences between branches", 2026-08-11).
- `origin` is a real GitHub **fork** of `TrinityCore/TrinityCore`
  (`https://github.com/LoubekJan/WoWBehaviorAI.git`), created via GitHub's
  Fork button — not a fresh repo populated with `git push`. TrinityCore's
  history contains old SVN-import commits with malformed committer emails
  that GitHub's push-time `fsck` rejects unconditionally, so a plain
  `git push` of the full history to a brand-new repo cannot work; forking
  is a server-side copy and doesn't go through that check.
- Git remotes:
  - `origin` → `https://github.com/LoubekJan/WoWBehaviorAI.git` (our fork; `3.3.5`, `master`, etc. mirror upstream, `ai-world` carries our scaffold)
  - `upstream` → `https://github.com/TrinityCore/TrinityCore.git` (pull only, for merging upstream `3.3.5` fixes)
- Development branch: `ai-world`, branched from `3.3.5` at the commit above.
  `git fetch upstream && git merge upstream/3.3.5` works as a normal merge
  since `ai-world` shares real history with `upstream/3.3.5`.

## Prerequisites (host)

- Git
- Docker Engine + Docker Compose plugin
- NVIDIA driver + NVIDIA Container Toolkit (for `make gpu-test` and the future `ai-server` GPU workload)

## First-time setup

```bash
make bootstrap   # creates .env from .env.example, runtime/ dirs, builds dev image
```

Edit `.env` if you need non-default ports or credentials.

## WoW game data (one-time, not committed)

`worldserver` needs `dbc`, `maps`, `vmaps` and `mmaps` extracted from your own
licensed 3.3.5a client using TrinityCore's map/vmap/mmap extractors (built as
part of `tools/` in this source tree). Place the results under:

```text
runtime/data/dbc
runtime/data/maps
runtime/data/vmaps
runtime/data/mmaps
```

This directory is gitignored and never copied into any image; it's mounted
into `worldserver` at runtime via `WOW_DATA_DIR` (see `.env.example`).

## Database bootstrap

`deploy/mysql/01-init-users.sh` creates the `auth`/`characters`/`world`
databases and the `TC_DB_USER`/`TC_DB_PASSWORD` application user (from
`.env`) on first `mysql` start. From there, TrinityCore's own updater
applies the base schema and SQL updates on `authserver`/`worldserver`
startup (`Updates.EnableDatabases` + `Updates.AutoSetup = 1` in
`deploy/*.conf`) — that's the empty-schema bootstrap.

The updater reads its SQL from `SourceDirectory` (empty in `deploy/*.conf`,
so it falls back to the build-time `/workspace`, i.e. the source tree
`worldserver`/`authserver` were compiled against). The runtime containers
don't have the full source mounted, only `./sql:/workspace/sql:ro` — that's
the one directory the updater actually needs.

Schema alone is not a playable server: **`TC_DB_USER`/`TC_DB_PASSWORD` are
the single source of truth for DB credentials** — set them in `.env`, never
edit them directly in `deploy/*.conf` (those files use
`__TC_DB_USER__`/`__TC_DB_PASSWORD__` placeholders, rendered at container
start by `docker/scripts/render-conf-and-run.sh`).

### World content (TDB)

`world`'s schema has no creatures, quests, or items until you import a TDB
dataset — the empty-schema bootstrap above is not enough to get from login
to a populated Elwynn Forest. Pick a tag from
[TrinityCore's releases](https://github.com/TrinityCore/TrinityCore/releases)
(e.g. `TDB335.25101`) and pin it:

```bash
make db-import-tdb TDB_VERSION=TDB335.25101 TDB_SHA256=<sha256 of the downloaded asset>
```

`db-import-tdb` brings up `mysql` itself and waits for it to accept
connections before importing, so it's self-contained — no need to start
`mysql` separately first. `docker/scripts/download-tdb.sh` resolves the
release's SQL/7z/zip asset via the GitHub API (never a hardcoded URL),
verifies `TDB_SHA256` if given, and imports it into `world`. Import it
**before** `authserver`/`worldserver` first start — they cache game data in
memory at startup, so importing after they're already up needs a restart to
take effect. Re-running with a newer `TDB_VERSION` re-imports on top of the
current data.

## Day-to-day workflow

Order matters: `authserver`/`worldserver` run binaries out of the
persistent `/build` volume, so it needs to exist before `start`; TDB import
should happen before `worldserver` first loads.

```bash
make bootstrap                     # .env, runtime/ dirs, build the dev image
make db-import-tdb TDB_VERSION=... # see "World content (TDB)" above
make build                         # compile TrinityCore into the build-data volume (throwaway tc-dev container)
make start                         # bring up mysql, authserver, worldserver, ai-server
make restart-world                 # restart only worldserver after a rebuild
make world-logs                    # tail worldserver logs
make shell                         # throwaway interactive shell in the dev container
make db-shell                      # mysql shell as the TC_DB_USER application user
```

Clean build and DB reset are explicit and destructive by design:

```bash
make clean-build   # wipe /build and rebuild from scratch
make reset-db       # drop and recreate the mysql volume
```

## GPU check

```bash
make gpu-test
```

Runs `nvidia-smi` inside a throwaway container via the `gpu-check` Compose
profile to confirm the host driver + NVIDIA Container Toolkit are wired up
correctly. `ai-server` is the only service intended to touch the GPU —
`worldserver` never runs inference directly (see roadmap section 1.10).

## Repository layout

See section [1.2](AI_TrinityCore_Roadmap_Etapa_1_2.md#12-struktura-repozitáře)
of the roadmap. `src/`, `sql/`, `cmake/` etc. are the unmodified TrinityCore
tree; `docker/`, `deploy/`, `compose*.yml`, `.env.example` and this file are
the additions for this project. `runtime/` is host-only, gitignored state.

## Status

This scaffold covers the repository/Compose/Makefile shape from Etapa 1,
with real shared history against `upstream/3.3.5`, working DB credential
plumbing, a `/workspace/sql` mount so the schema updater can actually find
its SQL, a pinned/checksummed TDB import path, and mysql/ai-server kept off
the host network by default. None of it has been run yet — `make bootstrap
&& make build && make start` is untested against an actual Docker/GPU host.

Still open: end-to-end verification against a real container run (client
login → character → Elwynn with mobs/quests), game data extraction docs,
debugging/observability (section 1.9), the async AI health bridge (1.11),
and all of Etapa 2.
