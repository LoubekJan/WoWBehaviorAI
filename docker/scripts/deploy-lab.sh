#!/bin/bash
# Deploy the complete authored world after trusted branch CI has passed.
# Auth owns its schema; worldserver only updates world and characters.
set -euo pipefail
expected_revision="${1:?Expected tested revision is required}"
test "$(git branch --show-current)" = AI-World-lab
test "$(git rev-parse HEAD)" = "$expected_revision"
test -f .env

mkdir -p runtime/lab
exec 9>runtime/lab/deploy.lock
flock -x 9

lab_compose=(docker compose --env-file .env -f compose.yml)
"${lab_compose[@]}" config --quiet
mkdir -p runtime/lab/logs/auth runtime/lab/recordings runtime/lab/receipts
services="$("${lab_compose[@]}" --profile recording ps --services --status running)"
if grep -Fxq aiworld-recorder <<< "$services"; then
  "${lab_compose[@]}" stop --timeout 30 aiworld-recorder
fi

"${lab_compose[@]}" build
make -f Makefile.lab dbc-factions
make graveyard-data
"${lab_compose[@]}" up -d --wait auth-mysql authserver lab-mysql
"${lab_compose[@]}" run --rm --no-deps -T --interactive=false tc-dev python3 tools/realm_lab/manage.py preflight /workspace/runtime/lab/data

# Retain the lab-only address while Docker DNS still advertises its container.
# A stopped container can return SERVFAIL rather than NXDOMAIN for its name.
lab_profile="$("${lab_compose[@]}" run --rm --no-deps -T --interactive=false tc-dev python3 -c 'from tools.realm_lab.manage import Settings; import os; print(Settings.load(os.environ).ai_profile)')"
world_id="$("${lab_compose[@]}" ps --all -q worldserver)"
world_ip=
if [ -n "$world_id" ]; then
  world_ip="$(docker inspect --format '{{(index .NetworkSettings.Networks "aitc-lab_default").IPAddress}}' "$world_id")"
fi
if [ -z "$world_ip" ] && [ -f runtime/lab/receipts/world-promotion.json ]; then
  world_ip="$(python3 -c 'import json; from pathlib import Path; print(json.loads(Path("runtime/lab/receipts/world-promotion.json").read_text()).get("prior_world", {}).get("address", ""))')"
fi
# Both executables share the build volume; stop both before replacing binaries.
"${lab_compose[@]}" stop worldserver authserver
run_args=(run --rm --no-deps -T --interactive=false)
if [ -n "$world_id" ]; then
  test "$(docker inspect --format '{{.State.Running}}' "$world_id")" = false
fi
if [ -n "$world_ip" ]; then
  run_args+=(-e "LAB_SETTLEMENT_WORLD_IP=$world_ip")
else
  # An already-stopped container has no retained IP. Recreate its disposable
  # container later; all database/build/runtime data remain on persistent mounts.
  if [ -n "$world_id" ]; then docker rm "$world_id"; fi
  test -z "$("${lab_compose[@]}" ps --all -q worldserver)"
  run_args+=(-e LAB_SETTLEMENT_WORLD_ABSENT=1)
fi
if [ "$lab_profile" = settlement-v8 ] || [ "$lab_profile" = wildlife-v8 ]; then
  "${lab_compose[@]}" "${run_args[@]}" tc-dev python3 tools/realm_lab/world_setup.py prepare \
    /workspace/runtime/lab/data /workspace/deploy/worldserver.conf /build/bin/worldserver
  receipt="/workspace/runtime/lab/receipts/start-policy-deploy-$(date -u +%Y%m%dT%H%M%SZ)-${expected_revision:0:12}.json"
  "${lab_compose[@]}" "${run_args[@]}" tc-dev python3 tools/realm_lab/world_setup.py apply-start \
    /workspace/runtime/lab/data --receipt "$receipt"
  "${lab_compose[@]}" "${run_args[@]}" tc-dev python3 tools/realm_lab/graveyard.py bootstrap /workspace/runtime/lab/data
  "${lab_compose[@]}" "${run_args[@]}" tc-dev \
    python3 tools/realm_lab/manage.py "bootstrap-$lab_profile" /workspace/runtime/lab/data
fi
"${lab_compose[@]}" run --rm --no-deps -T --interactive=false tc-dev bash /workspace/docker/scripts/build-lab.sh
"${lab_compose[@]}" up -d auth-mysql authserver lab-mysql ai-server world-viewer worldserver account-web

wait_healthy() {
  local service="$1" cid state
  for attempt in $(seq 1 90); do
    cid="$("${lab_compose[@]}" ps -q "$service")"
    if [ -n "$cid" ]; then
      state="$(docker inspect --format '{{.State.Health.Status}}' "$cid")"
      if [ "$state" = healthy ]; then return 0; fi
      if [ "$state" = unhealthy ]; then break; fi
    fi
    sleep 5
  done
  echo "Lab service did not become healthy: $service" >&2
  "${lab_compose[@]}" logs --tail=100 "$service"
  return 1
}
for service in auth-mysql authserver lab-mysql ai-server world-viewer worldserver account-web; do
  wait_healthy "$service"
done
if [ "$lab_profile" = settlement-v8 ] || [ "$lab_profile" = wildlife-v8 ]; then
  "${lab_compose[@]}" exec -T --interactive=false -w /workspace worldserver \
    python3 tools/realm_lab/manage.py "preflight-$lab_profile" /runtime/data
fi

version="$("${lab_compose[@]}" exec -T --interactive=false worldserver /build/bin/worldserver --version)"
grep -Fq "${expected_revision:0:12}" <<< "$version"
"${lab_compose[@]}" exec -T --interactive=false -w /workspace worldserver python3 -c '
from pathlib import Path
from tools.realm_lab.manage import Settings, render_config
import os
settings = Settings.load(os.environ)
config = Path("/tmp/lab-worldserver.conf").read_text(encoding="utf-8")
template = Path("/etc/trinitycore/worldserver.conf").read_text(encoding="utf-8")
assert config == render_config(template, settings)
assert "Updates.EnableDatabases = 6" in config
expected_enable = "1" if settings.ai_profile in ("single-return", "hunt-cycle", "hunt-100", "hunt-terrain-100") else "0"
if settings.ai_profile == "wildlife-v8":
    expected_enable = str(int(settings.wildlife_enabled))
assert f"AIWorld.Enable = {expected_enable}\n" in config
if settings.ai_profile in ("settlement-v8", "wildlife-v8"):
    assert "AIWorld.TelemetryEnabled = 1\n" in config
    assert "AIWorld.TelemetryNativeCreaturesEnabled = 1\n" in config
assert f"RealmID = {settings.realm_id}\n" in config
print("Lab realm ID, shared-auth migration exclusion and rendered AI profile verified:", settings.ai_profile)
'
if [ "$lab_profile" = settlement-v8 ] || [ "$lab_profile" = wildlife-v8 ]; then
  "${lab_compose[@]}" exec -T --interactive=false -w /workspace worldserver \
    python3 tools/realm_lab/telemetry_contract.py
fi
"${lab_compose[@]}" ps
echo "Lab deployed. Physical movement and return acceptance remain separate from service health."
