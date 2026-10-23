# Canonical authored-world stack. All persistent world/build identities match
# the deployed realm; auth uses its own database and preserved accounts.
WORLD_ENV_FILE ?= .env
COMPOSE := docker compose --env-file $(WORLD_ENV_FILE) -f compose.yml
RUN := $(COMPOSE) run --rm --no-deps -T --interactive=false
.DEFAULT_GOAL := help
.PHONY: help bootstrap config build rebuild test init-auth setup-world import-tdb db-import-tdb dbc-factions graveyard-data register-realm configure-realm preflight start stop restart-world logs world-logs shell db-shell backup record record-aiworld test-aiworld record-status record-aiworld-status record-stop record-aiworld-stop analyze-aiworld test-recovery-navigation

help:
	@echo 'Authored world: bootstrap -> build -> init-auth -> register-realm -> import-tdb -> dbc-factions -> graveyard-data -> setup-world -> start'
	@echo 'Provide the map/client data and private .env first; see README_DEV.md.'
	@echo 'Operations: config test backup logs world-logs restart-world stop record record-status record-stop shell db-shell'

bootstrap:
	@test -f $(WORLD_ENV_FILE) || (umask 077; cp .env.example $(WORLD_ENV_FILE))
	@mkdir -p runtime/lab/data/dbc runtime/lab/data/maps runtime/lab/data/vmaps runtime/lab/data/mmaps runtime/lab/logs/auth runtime/lab/recordings runtime/lab/client-patch runtime/lab/dbc-base runtime/lab/backups
	$(COMPOSE) config --quiet
	$(COMPOSE) build

config:
	$(COMPOSE) config --quiet

build:
	$(RUN) tc-dev bash /workspace/docker/scripts/build-lab.sh

rebuild:
	$(RUN) tc-dev cmake --build /build --clean-first
	$(MAKE) build

test:
	$(RUN) tc-dev ctest --test-dir /build --output-on-failure --no-tests=error

init-auth:
	@mkdir -p runtime/lab/logs/auth
	$(COMPOSE) up -d --wait auth-mysql authserver

register-realm:
	$(RUN) tc-dev python3 tools/realm_lab/world_setup.py register

configure-realm: register-realm

import-tdb:
	@test -n "$(TDB_VERSION)" && test -n "$(TDB_SHA256)" || (echo 'Set TDB_VERSION and TDB_SHA256'; exit 2)
	$(COMPOSE) up -d --wait lab-mysql
	$(RUN) -e TDB_DB_HOST=lab-mysql -e TDB_VERSION=$(TDB_VERSION) -e TDB_SHA256=$(TDB_SHA256) tc-dev bash /workspace/docker/scripts/download-tdb.sh

db-import-tdb: import-tdb

# Shared faction definitions are retained because the current NPCs use them.
dbc-factions:
	python3 tools/dbc/build_elwynn_factions.py --dbc-base-dir runtime/lab/dbc-base --out-dir runtime/lab/data/dbc

graveyard-data:
	python3 tools/realm_lab/graveyard.py build-server runtime/lab/data --base runtime/lab/dbc-base/WorldSafeLocs.dbc --client-patch runtime/lab/client-patch/patch-X.MPQ

setup-world:
	WORLD_ENV_FILE=$(WORLD_ENV_FILE) bash docker/scripts/setup-world.sh

preflight:
	$(COMPOSE) up -d --wait auth-mysql authserver lab-mysql
	$(RUN) tc-dev python3 tools/realm_lab/manage.py preflight /workspace/runtime/lab/data

start: preflight
	$(COMPOSE) up -d --wait auth-mysql authserver lab-mysql ai-server world-viewer worldserver account-web

# No command here removes database or build volumes.
stop:
	$(COMPOSE) down

restart-world:
	$(COMPOSE) restart worldserver

logs:
	$(COMPOSE) logs -f

world-logs:
	$(COMPOSE) logs -f worldserver

shell:
	$(COMPOSE) run --rm --no-deps tc-dev bash

db-shell:
	$(COMPOSE) exec lab-mysql sh -ec 'MYSQL_PWD="$$TC_DB_PASSWORD" exec mysql --user="$$TC_DB_USER"'

backup:
	WORLD_ENV_FILE=$(WORLD_ENV_FILE) bash docker/scripts/backup-world.sh

record:
	$(COMPOSE) up -d --no-deps --force-recreate aiworld-recorder

record-aiworld: record
test-aiworld: record

record-status:
	$(COMPOSE) ps -a aiworld-recorder
	$(COMPOSE) logs --tail=5 aiworld-recorder

record-aiworld-status: record-status

record-stop:
	$(COMPOSE) stop --timeout 30 aiworld-recorder

record-aiworld-stop: record-stop

analyze-aiworld:
	@test -n "$(SESSION)" || (echo 'Set SESSION to a directory under runtime/lab/recordings'; exit 2)
	$(COMPOSE) run --rm --no-deps --entrypoint python aiworld-recorder /behavior.py "/recordings/$(SESSION)"

test-recovery-navigation:
	$(RUN) tc-dev ctest --test-dir /build --output-on-failure --no-tests=error -R '^(Recovery (navigation|food)|Planning )'
