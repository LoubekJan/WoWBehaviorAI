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
world_ip=
if [ -n "$world_id" ]; then
  world_ip="$(docker inspect --format '{{(index .NetworkSettings.Networks "aitc-lab_default").IPAddress}}' "$world_id")"
  "${compose[@]}" stop worldserver
  test "$(docker inspect --format '{{.State.Running}}' "$world_id")" = false
fi
"${compose[@]}" up -d --wait auth-mysql authserver lab-mysql
run=("${compose[@]}" run --rm --no-deps -T --interactive=false)
if [ -n "$world_ip" ]; then
  run+=(-e "LAB_SETTLEMENT_WORLD_IP=$world_ip")
else
  if [ -n "$world_id" ]; then docker rm "$world_id"; fi
  test -z "$("${compose[@]}" ps --all -q worldserver)"
  run+=(-e LAB_SETTLEMENT_WORLD_ABSENT=1)
fi
receipt="runtime/lab/receipts/start-policy-$(date -u +%Y%m%dT%H%M%SZ)-$(python3 -c 'import uuid; print(uuid.uuid4().hex)').json"
"${run[@]}" tc-dev python3 tools/realm_lab/world_setup.py prepare \
  /workspace/runtime/lab/data /workspace/deploy/worldserver.conf /build/bin/worldserver
"${run[@]}" tc-dev python3 tools/realm_lab/world_setup.py apply-start \
  /workspace/runtime/lab/data --receipt "/workspace/$receipt"
"${run[@]}" tc-dev python3 tools/realm_lab/graveyard.py bootstrap /workspace/runtime/lab/data
"${run[@]}" tc-dev python3 tools/realm_lab/manage.py bootstrap-settlement-v8 /workspace/runtime/lab/data
printf 'Authored world prepared offline; start-policy receipt: %s\n' "$receipt"
