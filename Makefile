.PHONY: bootstrap build rebuild start stop restart-world logs world-logs shell db-shell clean-build reset-db gpu-test db-import-tdb configure-realm dbc-factions record-aiworld record-aiworld-status record-aiworld-stop test-aiworld analyze-aiworld

COMPOSE := docker compose -f compose.yml -f compose.dev.yml
BUILD_DIR := /build
BUILD_TYPE := RelWithDebInfo
CMAKE_CONFIGURE := cmake -S /workspace -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_INSTALL_PREFIX=$(BUILD_DIR)

# Workflow order matters: bootstrap -> build -> start. authserver/worldserver
# expect /build/bin (the build-data volume) to already contain binaries, so
# build runs in a throwaway tc-dev container and does not require `start`
# (or tc-dev) to already be running.

## first-time setup: .env, runtime dirs, build the dev image
bootstrap:
	@test -f .env || cp .env.example .env
	@mkdir -p runtime/data/dbc runtime/data/maps runtime/data/vmaps runtime/data/mmaps runtime/logs
	$(COMPOSE) build

## incremental TrinityCore build — runs in a throwaway tc-dev container
build:
	$(COMPOSE) run --rm tc-dev bash -c "$(CMAKE_CONFIGURE) && cmake --build $(BUILD_DIR) && cmake --install $(BUILD_DIR) --config $(BUILD_TYPE)"

## reconfigure + build without wiping the build/ccache volumes
rebuild:
	$(COMPOSE) run --rm tc-dev bash -c "$(CMAKE_CONFIGURE) && cmake --build $(BUILD_DIR) --clean-first && cmake --install $(BUILD_DIR) --config $(BUILD_TYPE)"

## start mysql, authserver, worldserver, ai-server (requires `make build` first)
start:
	$(COMPOSE) up -d

stop:
	$(COMPOSE) down

restart-world:
	$(COMPOSE) restart worldserver

logs:
	$(COMPOSE) logs -f

world-logs:
	$(COMPOSE) logs -f worldserver

## record Observer every 5s for 4 hours, independently of the SSH session
## override e.g. AIWORLD_RECORD_HOURS=8 make record-aiworld
record-aiworld:
	$(COMPOSE) up -d aiworld-recorder

## Detached four-hour behavior test; final report and exit status in recorder.
test-aiworld: record-aiworld

## Example: make analyze-aiworld SESSION=aiworld-20260924T155848Z-d7abf197
analyze-aiworld:
	@test -n "$(SESSION)" || (echo "Set SESSION to a directory under runtime/recordings"; exit 2)
	$(COMPOSE) run --rm --no-deps aiworld-analysis "/recordings/$(SESSION)"

record-aiworld-status:
	$(COMPOSE) ps -a aiworld-recorder
	$(COMPOSE) logs --tail=5 aiworld-recorder

## gracefully finish the current gzip file before collecting the session folder
record-aiworld-stop:
	$(COMPOSE) stop aiworld-recorder

## interactive dev shell — throwaway container, independent of `start`
shell:
	$(COMPOSE) run --rm tc-dev bash

db-shell:
	$(COMPOSE) exec mysql sh -c 'mysql -u"$$TC_DB_USER" -p"$$TC_DB_PASSWORD"'

## deliberate, explicit clean build — wipes the persistent build volume
clean-build:
	$(COMPOSE) run --rm tc-dev bash -c 'find $(BUILD_DIR) -mindepth 1 -delete'
	$(MAKE) build

## deliberate, explicit destructive reset of the development database
reset-db:
	$(COMPOSE) stop mysql
	$(COMPOSE) rm -f mysql
	docker volume rm aitc_mysql-data
	$(COMPOSE) up -d mysql

gpu-test:
	$(COMPOSE) --profile gpu-check run --rm gpu-check

## import a pinned TDB world-content dump: make db-import-tdb TDB_VERSION=TDB335.25101 [TDB_SHA256=...]
db-import-tdb:
	@test -n "$(TDB_VERSION)" || (echo "Usage: make db-import-tdb TDB_VERSION=TDB335.25101 [TDB_SHA256=...]"; exit 1)
	$(COMPOSE) up -d mysql
	$(COMPOSE) run --rm -e TDB_VERSION=$(TDB_VERSION) -e TDB_SHA256=$(TDB_SHA256) tc-dev bash /workspace/docker/scripts/download-tdb.sh

## configure auth.realmlist address/port from REALM_* in .env (versioned alternative to a manual UPDATE realmlist)
configure-realm:
	$(COMPOSE) up -d mysql
	$(COMPOSE) run --rm tc-dev bash /workspace/docker/scripts/configure-realm.sh

## regenerate runtime/data/dbc/Faction.dbc from data/elwynn/factions/factions.csv
## (see tools/dbc/build_elwynn_factions.py). Runs on the host with python3,
## not through tc-dev like every other target above - tc-dev's own image
## (docker/trinitycore/Dockerfile.dev) installs no Python interpreter at all,
## and this script has no TrinityCore build dependency to justify adding one.
dbc-factions:
	python3 tools/dbc/build_elwynn_factions.py
