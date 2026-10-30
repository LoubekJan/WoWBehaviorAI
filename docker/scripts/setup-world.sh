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
receipt="runtime/lab/receipts/start-policy-$(date -u +%Y%m%dT%H%M%SZ)-$(python3 -c 'import uuid; print(uuid.uuid4().hex)').json"
"${run[@]}" tc-dev python3 tools/realm_lab/world_setup.py prepare \
  /workspace/runtime/lab/data /workspace/deploy/worldserver.conf /build/bin/worldserver
"${run[@]}" tc-dev python3 tools/realm_lab/world_setup.py apply-start \
  /workspace/runtime/lab/data --receipt "/workspace/$receipt"
"${run[@]}" tc-dev python3 tools/realm_lab/graveyard.py bootstrap /workspace/runtime/lab/data
profile="$("${run[@]}" tc-dev python3 -c 'from tools.realm_lab.manage import Settings; import os; print(Settings.load(os.environ).ai_profile)')"
case "$profile" in settlement-v8|wildlife-v8|supply-v8|consumers-v8|raids-v8|fields-v8|defias-v8) ;; *) echo 'A reviewed authored-world profile is required.' >&2; exit 2;; esac
"${run[@]}" tc-dev python3 tools/realm_lab/manage.py "bootstrap-$profile" /workspace/runtime/lab/data
printf 'Authored world prepared offline; start-policy receipt: %s\n' "$receipt"
