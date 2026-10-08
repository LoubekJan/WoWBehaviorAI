#!/bin/bash
# Executed in the dedicated lab checkout after trusted branch CI has passed.
# Does not register realms, update auth schemas, or restart the shared login.
set -euo pipefail
expected_revision="${1:?Expected tested revision is required}"
test "$(git branch --show-current)" = AI-World-lab
test "$(git rev-parse HEAD)" = "$expected_revision"
test -f deploy/lab/.env

lab_compose=(docker compose --env-file deploy/lab/.env -f compose.lab.yml)
"${lab_compose[@]}" config --quiet
mkdir -p runtime/lab/logs runtime/lab/recordings
services="$("${lab_compose[@]}" --profile recording ps --services --status running)"
if grep -Fxq aiworld-recorder <<< "$services"; then
  "${lab_compose[@]}" stop --timeout 30 aiworld-recorder
fi

"${lab_compose[@]}" build
make -f Makefile.lab dbc-factions
"${lab_compose[@]}" run --rm --no-deps -T --interactive=false tc-dev python3 tools/realm_lab/manage.py preflight /workspace/runtime/lab/data

# Stop only the lab world before replacing its installed binaries.
"${lab_compose[@]}" stop worldserver
"${lab_compose[@]}" run --rm --no-deps -T --interactive=false tc-dev bash /workspace/docker/scripts/build-lab.sh
"${lab_compose[@]}" up -d lab-mysql ai-server world-viewer worldserver

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
for service in lab-mysql ai-server world-viewer worldserver; do
  wait_healthy "$service"
done

version="$("${lab_compose[@]}" exec -T --interactive=false worldserver /build/bin/worldserver --version)"
grep -Fq "${expected_revision:0:12}" <<< "$version"
"${lab_compose[@]}" exec -T --interactive=false -w /workspace worldserver python3 -c '
from pathlib import Path
from tools.realm_lab.manage import Settings, render_config
import os
config = Path("/tmp/lab-worldserver.conf").read_text()
assert "Updates.EnableDatabases = 6" in config
assert "AIWorld.Enable = 0" in config
assert f"RealmID = {Settings.load(os.environ).realm_id}\n" in config
print("Lab realm ID, shared-auth migration exclusion and phase-0 AI gate verified.")
'
"${lab_compose[@]}" ps
echo "Infrastructure deployed. Custom map and AI behavior acceptance are pending."
