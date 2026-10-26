#!/bin/bash
# Older upstream images clear .runner but leave migrated settings/credentials.
# Non-reuse starts must register from one coherent empty configuration state.
set -euo pipefail
if [[ -z "${CONFIGURED_ACTIONS_RUNNER_FILES_DIR:-}" && "${DEBUG_ONLY:-false}" != true ]]; then
  rm -f -- /actions-runner/.runner \
    /actions-runner/.runner_migrated \
    /actions-runner/.credentials \
    /actions-runner/.credentials_migrated \
    /actions-runner/.credentials_rsaparams
fi
cd /actions-runner
exec /entrypoint.sh "$@"
