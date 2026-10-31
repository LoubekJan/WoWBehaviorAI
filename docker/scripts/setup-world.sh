#!/bin/bash
# Explicit fresh installation / offline preparation of the authored world.
# TDB import, compatible client data and realm registration are prerequisites.
set -euo pipefail
env_file="${WORLD_ENV_FILE:-.env}"
test -f "$env_file"
mkdir -p runtime/lab/receipts runtime/lab/logs/auth
exec 9>runtime/lab/deploy.lock
flock -x 9
compose=(docker compose --env-file "$env_file" -f compose.yml)
"${compose[@]}" config --quiet
# Validate optional authored template inputs before stopping the world.
profile="$("${compose[@]}" run --rm --no-deps -T --interactive=false tc-dev python3 -c 'from tools.realm_lab.manage import Settings; import os; print(Settings.load(os.environ).ai_profile)')"
if [ "$profile" = full-v8 ]; then
  "${compose[@]}" run --rm --no-deps -T --interactive=false tc-dev python3 -m tools.realm_lab.character_templates plan
elif [ "$profile" = authoring-empty ]; then
  # Refuse an uncleared world before stopping it. Setup never deletes NPCs.
  "${compose[@]}" run --rm --no-deps -T --interactive=false tc-dev \
    python3 tools/realm_lab/manage.py preflight-authoring-empty /workspace/runtime/lab/data
fi
world_id="$("${compose[@]}" ps --all -q worldserver)"
if [ -n "$world_id" ]; then
  "${compose[@]}" stop worldserver
  test "$(docker inspect --format '{{.State.Running}}' "$world_id")" = false
  # A stopped Docker endpoint has no reachable IP. Remove only its disposable
  # container after confirming the process stopped; persistent mounts remain.
  docker rm "$world_id"
fi
"${compose[@]}" up -d --wait auth-mysql authserver lab-mysql
run=("${compose[@]}" run --rm --no-deps -T --interactive=false)
test -z "$("${compose[@]}" ps --all -q worldserver)"
run+=(-e LAB_SETTLEMENT_WORLD_ABSENT=1)
if [ "$profile" = full-v8 ]; then
  "${run[@]}" tc-dev python3 -m tools.realm_lab.character_templates restore
fi
receipt="runtime/lab/receipts/start-policy-$(date -u +%Y%m%dT%H%M%SZ)-$(python3 -c 'import uuid; print(uuid.uuid4().hex)').json"
"${run[@]}" tc-dev python3 tools/realm_lab/world_setup.py prepare \
  /workspace/runtime/lab/data /workspace/deploy/worldserver.conf /build/bin/worldserver
"${run[@]}" tc-dev python3 tools/realm_lab/world_setup.py apply-start \
  /workspace/runtime/lab/data --receipt "/workspace/$receipt"
"${run[@]}" tc-dev python3 tools/realm_lab/graveyard.py bootstrap /workspace/runtime/lab/data
profile="$("${run[@]}" tc-dev python3 -c 'from tools.realm_lab.manage import Settings; import os; print(Settings.load(os.environ).ai_profile)')"
case "$profile" in authoring-empty|settlement-v8|wildlife-v8|supply-v8|consumers-v8|raids-v8|fields-v8|defias-v8|full-v8) ;; *) echo 'A reviewed authored-world profile is required.' >&2; exit 2;; esac
if [ "$profile" = authoring-empty ]; then
  "${run[@]}" tc-dev python3 tools/realm_lab/manage.py preflight-authoring-empty /workspace/runtime/lab/data
else
  "${run[@]}" tc-dev python3 tools/realm_lab/manage.py "bootstrap-$profile" /workspace/runtime/lab/data
fi
if [ "$profile" = full-v8 ]; then
  "${run[@]}" tc-dev python3 -m tools.realm_lab.character_templates apply
fi
printf 'Authored world prepared offline; start-policy receipt: %s\n' "$receipt"
