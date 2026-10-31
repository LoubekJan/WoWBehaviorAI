# Docker scripts

Current canonical entry points for the authored realm, updated 2026-10-31.
Run from the repository root with private `.env`, root `compose.yml` and
`Makefile`. The deployed profile is `full-v8`; the clean example still
defaults to `settlement-v8` with LLM requests disabled. See
[README_DEV.md](../../README_DEV.md) for installation and operation.

| Script | Current caller and behavior |
| --- | --- |
| `build-lab.sh [install-prefix]` | `make build` and trusted CI/CD. Configures CMake/Ninja, builds servers and extractors with two jobs, runs CTest, installs binaries and checks their version. CI uses isolated build/cache volumes; deployment uses runtime volumes. |
| `setup-world.sh` | `make setup-world`. Explicit offline preparation after TDB, realm registration and map data exist. Takes the deploy lock, validates optional template exports, stops/removes only the worldserver container, prepares schemas/start/graveyard/population and applies dedicated character templates. Leaves starting the world to `make start`. |
| `deploy-lab.sh <tested-revision>` | Trusted branch deployment. Requires the exact `AI-World-lab` checkout revision, takes the deploy lock, builds images, checks data and validates character inputs before stopping the world. Stops worldserver and authserver before replacing shared binaries; applies reviewed bootstrap and character templates, then starts and verifies all eight services, exact binary version and fresh telemetry. |
| `backup-world.sh` | `make backup`. Writes private compressed auth and world/characters dumps, configuration, revision and SHA256 sums below `runtime/lab/backups/`. Each DB has a consistent transaction; a joint cutover needs writes stopped separately. It does not remove volumes. |
| `download-tdb.sh` | `make import-tdb TDB_VERSION=... TDB_SHA256=...` (alias `make db-import-tdb`). Resolves the pinned upstream release, verifies the supplied checksum and imports its world SQL into `lab-mysql`. Only for a new empty world DB. The Make target requires both version and checksum even though the helper's legacy standalone mode permits no checksum. |

For `full-v8`, setup/deploy bracket the unchanged strict population bootstrap
with `tools.realm_lab.character_templates restore` and `apply`. Validation
of the author's files runs before stopping the world; runtime verification
uses the installed DB ledger independently of editable cards. The installer
copies complete native rows and entry-keyed resources, preserves homes and
source templates, and rejects collisions or provenance drift. It does not
make the studio a live SQL editor.

`WORLD_ENV_FILE` selects the private environment file for setup and backup;
the canonical deployment intentionally uses root `.env`. Normal server
startup is handled by `tools/realm_lab/auth.py run-auth` and
`tools/realm_lab/manage.py run-world` from Compose. Rendered credentials
stay inside the container; worldserver runs without an interactive console.

The remaining two helpers are legacy compatibility tools:

- `render-conf-and-run.sh` substitutes the old `__TC_DB_*__` placeholders.
  Canonical auth/world startup uses the Python launchers above instead.
- `configure-realm.sh` uses legacy `REALM_*` variables and a single database
  endpoint. Current `make register-realm` uses
  `tools/realm_lab/world_setup.py register` with the canonical split auth DB.

These legacy scripts do not describe the current provisioning path and
should not be substituted for the current Make targets.
