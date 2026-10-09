# One NPC return experiment

This opt-in profile owns persistent spawn/agent `900725`, native Elder Black Bear
entry `1186`, on map `725` / area `4988`. Its home and scope come from versioned
`data/realm_lab/aiworldlab/test-points.json`. The bootstrap uses private
`lab-mysql` world/characters credentials. It does not connect to shared auth.

The default `LAB_AI_PROFILE=disabled` keeps AI, inference, advice, group features,
need evolution and test hooks off. `single-return` enables living roles and lab
telemetry for the one explicit spawn. `AIWorld.TestSpawnId` remains `0` in both
profiles. Existing agent/group/history rows remain; bootstrap demotes only the
four legacy Elwynn pilot controls in the private copied characters database.
Any other controlled agent, map-725 spawn, ID collision, changed home or group
membership blocks the operation.

From the dedicated lab checkout, keep `LAB_AI_PROFILE=disabled` in
`deploy/lab/.env` and stop the lab worldserver before database changes:

```sh
docker compose --env-file deploy/lab/.env -f compose.lab.yml stop worldserver
docker compose --env-file deploy/lab/.env -f compose.lab.yml run --rm --no-deps -T --interactive=false tc-dev \
  python3 /workspace/tools/realm_lab/manage.py bootstrap-single /workspace/runtime/lab/data
docker compose --env-file deploy/lab/.env -f compose.lab.yml run --rm --no-deps -T --interactive=false tc-dev \
  python3 /workspace/tools/realm_lab/manage.py preflight-single /workspace/runtime/lab/data
docker compose --env-file deploy/lab/.env -f compose.lab.yml run --rm --no-deps -T --interactive=false tc-dev \
  python3 /workspace/tools/realm_lab/manage.py activate-single /workspace/runtime/lab/data
```

Bootstrap prepares Observe/control `0`; activation changes only the reviewed row
to control `1`. Both verify the native player/NPC confirmation, matching source
revision, eight complete navmesh routes and six live file hashes before any
writes, then verify SQL read-back. One native follow/evade confirmation is the
minimum gate; four physical directions remain a separate experiment, with no
claim that navmesh validation proves physical movement. Keep the worldserver
stopped throughout: validation and mutation use separate SQL sessions, so this
is an offline setup operation.

After successful activation, set `LAB_AI_PROFILE=single-return` and run:

```sh
docker compose --env-file deploy/lab/.env -f compose.lab.yml up -d worldserver world-viewer
```

Every startup rechecks the live bundle and exact controlled row before executing
worldserver. The observer and recorder derive map, area and bounds from the same
versioned test points. Use `.gm on` for the physical test, select the persistent
bear, use native `.npc follow`, then `.npc follow stop` and observe its autonomous
living-role return to the original home. Native `.npc evade` would exercise a
different return path, so it is not the AIWorld acceptance step. Record the same
spawn GUID, initial/final positions and telemetry throughout. For rollback, stop
the lab worldserver, restore `LAB_AI_PROFILE=disabled`, and start it again.
