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
versioned test points.

For the first physical return, use a 60-second acceptance limit measured from
`follow stop`. This is an experiment limit; production retries have no fixed
whole-episode timeout.

1. Use `.gm on`, select the persistent bear and verify `.npc info` reports spawn
   `900725`. `.aiworld group status` should show `PREDATOR`, `AIWorldControlled`
   and an enabled living role. `.aiworld group navigation` prints the immutable
   runtime home and probes the route without moving the NPC.
2. Use `.npc follow` and walk about 40 yards from the original home. Stop the
   player and wait until the bear catches up and stops; `.movegens` should show
   FOLLOW. Follow and stop preserve the original runtime home.
3. Use `.npc follow stop` and start the timer. Observe the autonomous living
   return, without another movement command. `.aiworld group status` should
   report `RETURN_HOME` / `MOVING` / `MOVE_TO`; `.movegens` shows POINT while
   AIWorld movement runs. Planning may initially wait about 15 seconds.
4. Require the same runtime GUID and fixed home, physical movement and final
   `homeDistance <= 14` yards, followed by another living activity. Predator
   home arrival is a region, so exact arrival at the central coordinate is not
   required. If 60 seconds elapse, capture status, navigation and positions as
   a failed experiment; do not repair it with evade, teleport or respawn.

Native `.npc evade` exercises the previously confirmed native return. It does
not establish AIWorld return acceptance. Record initial/final positions and
telemetry throughout, then repeat ten cycles across four directions. The
playerless repetition is a separate gate. For rollback, stop the lab
worldserver, restore `LAB_AI_PROFILE=disabled`, and start it again.

## Ten-cycle series

The user reported the first autonomous return as working. This is qualitative
confirmation; elapsed time and runtime GUID before/after have not been supplied.
The ten-cycle and playerless displaced-return gates remain open.

Use this order: **-X, +X, -Y, +Y, +X, -X, +Y, -Y, -X, +X**.

| Direction | Player destination X | Player destination Y |
|---|---:|---:|
| -X | 226.667 | 800 |
| +X | 306.667 | 800 |
| -Y | 266.667 | 760 |
| +Y | 266.667 | 840 |

Walk with follow to these destinations. The bear follows at an offset, so record
its actual starting position and home distance. Before each cycle, verify its
full GUID through `.npc info`, fixed home through `.aiworld group navigation`,
and home distance at most 14 yards. Start the 60-second timer at `follow stop`.
Record direction, stop time, arrival time, initial/final distance, GUID before
and after, unchanged home and the next normal activity. One failed or interrupted
cycle is recorded as such; a restart or respawn ends the current life instance.

The existing recorder can capture the series without changing simulation:

```sh
AIWORLD_RECORD_HOURS=0.5 AIWORLD_RECORD_INTERVAL=2 AIWORLD_TEST_MINUTES=10 \
AIWORLD_RECORD_LABEL=map725-ten-returns AIWORLD_RECORD_BUILD_LABEL="$(git rev-parse HEAD)" \
docker compose --env-file deploy/lab/.env -f compose.lab.yml \
  --profile recording up -d --no-deps aiworld-recorder
```

Reuse an existing active recording rather than replacing it. This session lasts
30 minutes and stores gzip JSONL under `runtime/lab/recordings`. Do not push
during the series: lab CD stops the recorder and restarts the worldserver.

The generic behavior report does not verify ten attempts or the 60-second timer:
its return-duration threshold is 600 seconds. The wire contains neither runtime
GUID nor online-player count. Keep the manual observations with the recording;
an automatic PASS is not acceptance of this experiment.

A playerless displaced return requires a separate online-player-count record
showing zero while the bear is still outside home and subsequently returns.
An already completed return followed by logout only proves ongoing simulation.
