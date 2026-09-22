# Living wolves — first runtime pilot

The implementation is limited to AIWorld-controlled creatures matching
`AIWorld.WolfGroupCreatureEntry` and the WolfLoose faction. The deployment
configuration uses entry **69 (Diseased Timber Wolf)** and prey entry
**721 (Rabbit)**.
Other species retain their existing behavior. Roaming and hunting still require
membership in a valid WolfLoose group; a lone wolf can defend or flee but does
not acquire prey independently in this pilot.

## Behavior

- Idle packs use the existing path-checked territory roaming (enabled in the
  deployment configuration). Each member now has a stable slot around the
  shared destination. With the deployment defaults, two to five members aim
  for at least three yards between adjacent slot centers. Slots use the whole
  sorted membership, reserving places for temporarily unloaded/dead members.
- Wolf hunters approach separate ground/path-checked points around live prey,
  within the existing three-yard attack-arrival bound. Hunting and defense use
  distinct chase angles so members do not all occupy the same side of a target.
  Feeding and sleep retain the resulting positions. This is destination spacing,
  not physical collision between creatures; paths may cross during movement.
- Hunger grows by 0.003/second of materialized simulation. A hungry member can
  initiate a hunt at 0.65 (about 217 seconds from empty hunger). Satiated members
  do not join a new hunt. Ordinary GET_FOOD no longer interrupts this cohort's
  hunt to look for civilian food locations.
- Healthy hunters continue fighting their prey. An attacked wolf can defend
  against the actual threat, including a player; this does not enable proactive
  hunting of players. Defense is bounded to 30 seconds or 30 yards from its start.
- Nearby members of the same WolfLoose group can interrupt hunting, feeding or
  sleep to assist a packmate in live combat against an external threat. Both the
  packmate and threat must be visible and within 30 yards of the helper, and the
  threat must be attackable. A configured prey animal fighting back does not
  trigger pack defense. An existing personal non-hunt threat keeps priority;
  injured helpers flee instead. Assistance uses the same bounded defense action.
- At 30% health or below, wolves flee. The recovery threshold is above 50% health.
  A short escape also ends an overlong defense; the resolved previous attacker
  can sustain that escape for up to eight seconds after combat cleanup.
- A successful hunt offers a meal only to an engaged participant within five
  yards of the resolved corpse. Eating takes five seconds, with corpse, range,
  line-of-sight and combat revalidation before nutrition is awarded.
- A completed meal clears hunger, then starts twenty seconds of sleep. Danger
  interrupts eating/sleep; interrupted eating grants no nutrition. Completed
  sleep clears fatigue. Group movement becomes eligible again after waking.
- Meals and individual actions are tied to the materialized incarnation and
  cleared on unload/rebind. They are not persisted across a server restart.

## Configuration

`AIWorld.LivingWolvesEnabled = 1` is enabled in `deploy/worldserver.conf`; the
upstream-style `worldserver.conf.dist` retains a default of `0`. The existing
WolfGroupAutoFormation, CoalitionMaintenance, GroupCoordination and
WolfGroupHuntEnabled settings must remain enabled. WolfGroupRoamEnabled is now
also enabled in the deployment configuration. Apply configuration through the
project's normal render/build/restart workflow; editing the tracked file alone
does not alter a running server.

The hunt target must also pass TrinityCore's normal attackability checks. Entry
525 (Mangy Wolf), used in the initial configuration, is friendly to entry 69 in
the local faction data and fails with `TARGET_NOT_ATTACKABLE`. Entry 721 uses
FactionTemplate 31 (Faction 28), which entry 69's FactionTemplate 32 explicitly
lists as an enemy. Merely configuring a target entry does not override combat
reactions.

Prey does not need an AIWorld AgentRecord: nearby perception observes ordinary
creatures too. The hunters must already be AIWorld-controlled WolfLoose members.
On a test server, `.npc add 721` can supply nearby prey; it creates a persistent
spawn, so record its spawn ID with `.npc info` and remove that test spawn with
`.npc delete <spawnID>` afterward. Use `.aiworld group status` on a hunter.

## Automated verification

`tests/game/LivingWolf.cpp` is included in the normal Catch2 suite. It can also
exercise the real ActionSystem and policy without a full server build:

```bash
mkdir -p runtime
g++ -std=c++20 -DAIWORLD_STANDALONE_TEST \
  -Isrc/common -Isrc/common/Utilities \
  -Isrc/server/game/Entities/Object -Isrc/server/game/AIWorld \
  tests/game/LivingWolf.cpp \
  src/server/game/AIWorld/Action/ActionSystem.cpp \
  src/server/game/AIWorld/Action/ArrivalTolerance.cpp \
  -o runtime/living-wolf-tests
./runtime/living-wolf-tests
```

Local result (2026-09-22): MSVC compiled and ran the standalone test successfully,
70 checks passed. This covers thresholds/hysteresis, timers, defense authority,
control mode, stale goal identity, movement conflicts, corpse identity,
combat/distance/LOS meal rejection, rest validation, and pack-defense selection
(membership, live combat, distance, visibility, attackability, prey exclusion,
friendly-fire exclusion and deterministic threat choice). It does **not** exercise
live TrinityCore movement, animation, group formation, or the manager lifecycle.
Full server CMake configuration is blocked locally by missing Boost >= 1.78.

The spacing change also passed 141 standalone MSVC checks in
`tests/game/GroupMemberFormation.cpp`; the original 70 LivingWolf checks passed
again. The new checks run the real intent system, projector and ActionSystem,
covering separation for two to five members, stable slots under reordered or
missing live observations, the roaming envelope, no repeated moves after arrival
on sloping terrain, regroup offsets, unchanged unspaced profiles, authorized
hunt approach slots and chase-angle validation. They do not run live navmesh or
ChaseMovementGenerator behavior.

To run this additional component test on the normal Linux build host:

```bash
g++ -std=c++20 -DAIWORLD_FORMATION_STANDALONE_TEST \
  -Isrc/common -Isrc/common/Utilities \
  -Isrc/server/game/Entities/Object -Isrc/server/game/AIWorld \
  tests/game/GroupMemberFormation.cpp \
  src/server/game/AIWorld/Agent/AgentGroupIntentSystem.cpp \
  src/server/game/AIWorld/Agent/AgentGroupIntentProjector.cpp \
  src/server/game/AIWorld/Action/ActionSystem.cpp \
  src/server/game/AIWorld/Action/ArrivalTolerance.cpp \
  -o runtime/wolf-formation-tests
./runtime/wolf-formation-tests
```

## Runtime verification

On 2026-09-22, the user reported a successful in-game test after changing the
hunt target to entry 721: the wolves performed the described hunt, feeding and
rest cycle as expected. This is user-reported confirmation of the basic cycle;
it does not establish that every interruption, unload or regression case below
was exercised. Exact timings and DEBUG log markers were not separately supplied.

The next in-game report confirmed that an attacked hunter defended itself while
the other hunters continued hunting. That version only had individual defense.
After nearby pack assistance was added, the user confirmed on 2026-09-22 that
the repeated in-game test worked: nearby members joined the defense when one
hunting wolf was attacked. This confirms the basic pack-assistance scenario;
distance/visibility boundaries, cross-group isolation and assistance during
feeding/sleep still need separate runtime confirmation.

The following report identified overlapping pack members. Stable formation
slots, separate hunt approach points and attack angles were added locally;
their in-game verification is pending. No new configuration is required when
LivingWolvesEnabled is already enabled. Formation offsets stay inside the
existing roaming envelope; inaccessible slots do not fall back to the common
center, and incomplete paths are rejected for spaced destinations.

### Acceptance checklist

1. Build the server and run the full test suite using README_DEV's normal gate.
2. Observe a materialized entry-69 pack with valid prey nearby for at least five
   minutes: territory movement, hungry hunt, corpse feeding, sleep, then movement.
3. Confirm `AI living wolf ... action=FEED`, `meal=CONSUMED`, and
   `action=WILDLIFE_REST` in DEBUG-level `ai.world` logging and compare with the
   client/telemetry. Verify the animal model actually renders the animations.
4. Attack a healthy wolf: it defends. Lower its health below 30%: its owned chase
   ends and it flees. Stop attacking: it eventually becomes eligible for its
   group's ordinary movement again.
5. Interrupt feeding and sleeping with an attack. No meal credit on interrupted
   feeding; no residual sleep pose while defending/fleeing.
6. Despawn/unload the prey during feeding and unload/reload the wolf during an
   individual action. No delayed nutrition or stale action may survive.
7. Check another creature species and ObserveOnly agents retain their behavior.
8. After rebuilding with pack assistance, verify the nearby wolves have the same
   group ID, let them hunt and attack one healthy member with an attackable
   player. Eligible nearby members should interrupt the hunt and defend, normally
   within one or two needs updates (configured at one second). Look for
   `AI living wolf ... assistMember=... threat=... action=DEFEND`; the helpers'
   status should show `goal=DEFEND action=ATTACK`. Repeat during feeding/sleep.
   Verify wolves from another group or beyond the assistance radius do not join
   because of pack assistance, and low-health wolves flee. Check that defense
   still ends on death/invalid target or its existing time/distance bound.
9. After rebuilding with spacing, observe a two-to-five-member pack for several
   roam phases: members should settle at distinct positions and remain still
   between phases. Repeat a rabbit hunt and a player-triggered pack defense;
   members should approach from different sides and retain separated positions
   while feeding/resting. Check a slope and a nearby obstacle for endless small
   corrections or movement through terrain. Brief crossing of paths while
   moving is allowed; persistent stacking after arrival is a failure.

The local unit-test result and the user-reported runtime result are separate
evidence; remaining checklist items still need their own confirmation.

## Diagnosing `.aiworld group status`

The status command now uses brace-aware formatting, so it prints actual IDs
and values instead of literal `{}`. It also prints the selected creature's
entry/spawn, control mode, health, hunger, current goal/action, and a read-only
snapshot of WolfLoose formation eligibility.

- `ENTRY_MISMATCH`: compare `entry` with `expectedEntry`. Only entry 69 is a
  WolfLoose member candidate in this pilot. The configured prey entry 721 does
  not need group membership or an AgentRecord; an unregistered target reports
  `has no registered AgentRecord` before the formation diagnostic is reached.
- `OBSERVE_ONLY`: the agent is registered but AIWorld does not control it.
- `FACTION_MISMATCH`: its AI world faction does not match the wolf profile.
- `ALREADY_IN_LOOSE_GROUP`: the membership lines below identify that group.
- `AUTOFORMATION_DISABLED` / `AIWORLD_DISABLED`: a required runtime setting is off.
- `MEMBER_RESERVED`, `PROFILE_FORMATION_IN_FLIGHT`, `GLOBAL_FORMATION_BUDGET_BUSY`:
  a formation operation is pending; check again after the next few passes.
- `NO_FORMATION_PROPOSAL`: the production selector cannot currently form any
  eligible WolfLoose group. The nearby count includes the selected agent and
  excludes existing Loose members and reserved candidates.
- `IN_CURRENT_FORMATION_PROPOSAL` / `NOT_IN_CURRENT_FORMATION_PROPOSAL`: the
  selected agent is/is not included in the selector's first current proposal.
  This is not confirmation of a completed database join.

Nearby counts are shown only when proposal evaluation was reached. Radius and
minimum member count come from the running server, not hard-coded assumptions.
The command does not create groups, force membership, or load grids. An absent
group alone is not proof of a behavior bug. Capture the entire status output
for the same selected wolf when investigating a failed formation.
