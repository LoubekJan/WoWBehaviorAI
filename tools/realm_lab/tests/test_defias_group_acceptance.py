"""Synthetic group evidence cannot replace owned movement or food accounting."""
from contextlib import redirect_stdout
from copy import deepcopy
from functools import lru_cache
import io
import json
from pathlib import Path
import tempfile
import unittest

from tools.realm_lab import defias_group_acceptance as acceptance, defias_population as population
from tools.realm_lab import field_acceptance, full_population as full
from tools.realm_lab.tests import test_field_acceptance as fields
from tools.realm_lab.tests.test_full_telemetry import frame as full_frame

PARTY = tuple(range(901050, 901055))


@lru_cache(maxsize=2)
def samples(repelled=False):
    """Synthetic only: five raiders, never a live recording or theft receipt."""
    result = fields.happy_samples(end=120)
    spec = population.Population.load()
    authored = {actor.spawn_id: actor for actor in spec.settlement.actors}
    managed = {actor.spawn_id: actor for actor in spec.actors}
    camp, warehouse = fields.DEPOTS[3], fields.DEPOTS[5]
    encounter = dict(x=camp["x"], y=camp["y"]-70, z=camp["z"])
    destination = encounter if repelled else warehouse
    for record in result:
        t = record["elapsed_seconds"]
        actors = {actor["spawn_id"]: actor for actor in record["state"]["agents"]}
        template = deepcopy(actors[901055])
        for sid in population.DEFIAS_IDS:
            actor = actors[sid]
            if sid != 901055:
                actor.clear(); actor.update(deepcopy(template))
            expected, binding = authored[sid], managed[sid]
            actor.update(agent_id=sid, spawn_id=sid, native_creature=False, runtime_guid=f"SYNTHETIC-defias-{sid}",
                entry=expected.entry, name=expected.label, world_faction=2, control_mode="AI_WORLD_CONTROLLED",
                type={0: "CIVILIAN", 1: "GUARD", 5: "COMBATANT"}[binding.agent_type],
                home=dict(map_id=725, x=expected.x, y=expected.y, z=expected.z), work=None,
                action=None, action_source_goal=None, goal=None, goal_owner="", action_started_at_ms=None)
            raider = sid in population.RAIDER_IDS
            actor["living_role"].update(role="RAIDER" if raider else "GUARD" if sid in population.GUARD_IDS else "WORKER",
                phase="IDLE", activity="NONE", started_at_ms=0)
            actor["supply"].update(role="RAIDER" if raider else "CONSUMER", capacity=2 if raider else 0)
            actor["raid"] = deepcopy(template["raid"]) if raider else None
            fields.move(actor, actor["home"])
            if not raider:
                continue
            source = actors[901022]["supply"]["depot"]
            actor["raid"].update(source_depot_id=5, source_depot=deepcopy(source), attempt_id="1",
                planning=dict(reason="EXPLORE_UNKNOWN_STOCK", memory_records=int(repelled and t >= 26), route_id=1,
                              known_stock=None, stock_age_ms=None, risk=0.0, retry_remaining_ms=0, danger=None))
            if sid not in PARTY:
                continue
            phase = "ASSEMBLING" if t <= 2 else "OUTBOUND" if t <= 24 else "RAIDING" if t <= 27 else "RETURNING" if t <= 49 else "COMPLETE"
            actor["raid"]["party"] = dict(id="123", leader_spawn_id=str(PARTY[0]), member_spawn_ids=list(map(str, PARTY)),
                                            phase=phase, role="LEADER" if sid == PARTY[0] else "MEMBER")
            fields.move(actor, camp)
            if 3 <= t <= 24:
                fields.move(actor, fields.blend(camp, destination, (t-3)/21), moving=True)
                fields.owned(actor, "TO_SOURCE", "NONE", 3, moving=True, purpose="RAID_SOURCE")
                actor["raid"]["phase"] = "TO_SOURCE"
            elif 25 <= t <= 27:
                fields.move(actor, destination)
                actor["raid"]["phase"] = "STEALING"
            elif 28 <= t <= 49:
                fields.move(actor, fields.blend(destination, camp, (t-28)/21), moving=True)
                fields.owned(actor, "RETURNING_EMPTY", "NONE", 28, moving=True, purpose="RAID_EMPTY_RETURN")
                actor["raid"]["phase"] = "RETURNING_EMPTY"
            else:
                actor["raid"]["phase"] = "COOLDOWN"
        if repelled and t == 25:
            guard = actors[901026]
            guard.update(type="GUARD", alive=True, in_combat=True, target=dict(spawn_id=PARTY[0]))
            guard["position"].update(destination, source="live")
            actors[PARTY[0]]["in_combat"] = True
    return result


def respawn_samples(*, witnessed_death=True, dropped_cargo=False):
    """One old member retires; four survivors return after its new life starts."""
    result = deepcopy(samples(True))
    camp = fields.DEPOTS[3]
    encounter = dict(x=camp["x"], y=camp["y"]-70, z=camp["z"])
    dead_sid = PARTY[-1]
    for record in result:
        t = record["elapsed_seconds"]
        for sid in PARTY:
            actor = fields.actor(record, sid)
            if sid == dead_sid:
                if witnessed_death and 26 <= t < 56:
                    actor.update(alive=False, health=0, in_combat=False, action=None,
                                 action_source_goal=None, goal=None, goal_owner="")
                    actor["living_role"].update(lifetime_ms=0, phase="IDLE", activity="NONE", started_at_ms=0,
                                               movement_purpose="NONE", enabled=True, role="RAIDER")
                    fields.move(actor, encounter)
                if t >= 56:
                    actor.update(runtime_guid=f"SYNTHETIC-new-life-{sid}", alive=True, health=100,
                                 in_combat=False, action=None, action_source_goal=None, goal=None, goal_owner="")
                    actor["living_role"].update(lifetime_ms=fields.BASE+56000, phase="IDLE")
                    actor["raid"].pop("party", None)
                    fields.move(actor, actor["home"])
                continue
            if t >= 28:
                phase = "RETURNING" if t <= 100 else "COMPLETE"
                actor["raid"]["party"]["phase"] = phase
                if t <= 100:
                    fields.move(actor, fields.blend(encounter, camp, (t-28)/72), moving=True)
                    fields.owned(actor, "RETURNING_EMPTY", "NONE", 28, moving=True, purpose="RAID_EMPTY_RETURN")
        # Before its respawn, the corpse's party copy reports the shared phase.
        if t >= 28 and t < 56:
            fields.actor(record, dead_sid)["raid"]["party"]["phase"] = "RETURNING"
        if dropped_cargo:
            stolen, lost = int(t >= 20), int(t >= 26)
            for actor in record["state"]["agents"]:
                supplied = actor.get("supply")
                copies = []
                if supplied:
                    accounting = supplied["accounting"]
                    accounting["initial_food"] = "2"
                    accounting["in_depots"] = str(int(accounting["in_depots"])+2*(1-stolen))
                    accounting["in_cargo"] = str(int(accounting["in_cargo"])+2*(stolen-lost))
                    accounting["lost"] = str(2*lost)
                    copies.append(supplied["depot"])
                if actor.get("raid"):
                    copies.extend((actor["raid"]["source_depot"], actor["raid"]["camp_depot"]))
                for depot in copies:
                    if depot["id"] == 5:
                        depot["stock"] += 2*(1-stolen)
                        depot["stolen"] = str(2*stolen)
            fields.actor(record, dead_sid)["supply"].update(stolen=str(2*stolen), lost=str(2*lost), cargo=2*(stolen-lost))
    # Keep one continuous 300-second session. The repeated final state carries
    # no new action/transfer evidence; it only extends the fresh ledger window.
    for t in range(121, 301):
        record = deepcopy(result[-1])
        record.update(sequence=t+1, elapsed_seconds=t)
        record["state"]["captured_at_ms"] = fields.BASE+t*1000
        result.append(record)
    return result


@lru_cache(maxsize=1)
def full_samples():
    """Same synthetic party evidence, plus the reviewed ordinary full cohort."""
    result = deepcopy(samples())
    ordinary = {actor['spawn_id']: actor for actor in full_frame()['agents']
                if actor['spawn_id'] in full.RECRUIT_IDS}
    for record in result:
        for actor in record['state']['agents']:
            binding = ordinary.get(actor['spawn_id'])
            if binding is not None:
                location = actor['position']
                actor.update(deepcopy(binding))
                actor['position'] = {**location, **binding['position']}
    return result


class DefiasGroupAcceptanceTests(unittest.TestCase):
    def test_explicit_full_profile_accepts_the_same_party_with_all_100_owned_actors(self):
        report = acceptance.evaluate_samples(full_samples(), minimum_seconds=120, profile=full.PROFILE)
        self.assertEqual(report['status'], 'PASS', report)
        self.assertEqual((report['profile'], report['expected_owned'], report['expected_native']), (full.PROFILE, 100, 0))
        self.assertEqual(set(report['receipts'][0]['joint_travel_members']), set(PARTY))
        self.assertFalse(report['receipts'][0]['theft_success_certified'])
        # Profile is an operator choice, never inferred from telemetry labels.
        self.assertEqual(acceptance.evaluate_samples(full_samples(), minimum_seconds=120)['status'], 'FAIL')
        with self.assertRaises(ValueError):
            acceptance.evaluate_samples([], profile='unknown-v8')

    def test_full_profile_cannot_hide_falsified_generic_roles_behind_a_valid_party_trip(self):
        for identity, field, value in ((901014, 'living_role', dict(enabled=True, extensions_enabled=True, role='PREY', lifetime_ms=1)),
                                      (901008, 'type', 'CIVILIAN'), (901097, 'supply', dict(enabled=True))):
            rows = deepcopy(full_samples())
            fields.actor(rows[10], identity)[field] = value
            with self.subTest(identity=identity, field=field):
                report = acceptance.evaluate_samples(rows, minimum_seconds=120, profile=full.PROFILE)
                self.assertEqual(report['status'], 'FAIL', report)
                self.assertIn('full_population_runtime_contract_changed', [v['code'] for v in report['violations']])

    def test_explicit_full_profile_remains_in_inconclusive_reports_and_cli_errors(self):
        report = acceptance.evaluate_samples([], profile=full.PROFILE)
        self.assertEqual((report['status'], report['profile']), ('INCONCLUSIVE', full.PROFILE))
        self.assertEqual((report['expected_owned'], report['expected_native']), (100, 0))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'report.json'
            with redirect_stdout(io.StringIO()):
                code = acceptance.main([str(Path(directory) / 'missing.jsonl'), '--profile', full.PROFILE,
                                        '--output', str(output)])
            saved = json.loads(output.read_text(encoding='utf-8'))
            self.assertEqual((code, saved['status'], saved['profile']), (2, 'INCONCLUSIVE', full.PROFILE))
            self.assertIn('readable_closed_recording', saved['missing_evidence'])

    def test_witnessed_dead_member_can_respawn_before_the_survivors_return(self):
        report = acceptance.evaluate_samples(respawn_samples())
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["quality"]["witnessed_dead_raider_incarnation_continued"], 1)
        self.assertEqual(report["quality"]["witnessed_dead_raider_role_reset_continued"], 1)
        receipt = report["receipts"][0]
        terminal = receipt["member_outcomes"][PARTY[-1]]
        self.assertEqual((terminal["kind"], terminal["capture_ms"]), ("DEATH", fields.BASE+26000))
        self.assertEqual(terminal["runtime_guid"], f"SYNTHETIC-defias-{PARTY[-1]}")
        self.assertLess(terminal["lifetime_ms"], fields.BASE+56000)
        self.assertEqual(terminal["observed_role_lifetime_ms"], 0)
        self.assertEqual(terminal["prior_alive_capture_ms"], fields.BASE+25000)
        self.assertGreater(receipt["completed_capture_ms"], fields.BASE+56000)
        # A creature may respawn under the same runtime GUID. Its newer role
        # lifetime still separates the new life from the old death receipt.
        rows = respawn_samples()
        for record in rows:
            if record["elapsed_seconds"] >= 56:
                fields.actor(record, PARTY[-1])["runtime_guid"] = terminal["runtime_guid"]
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["receipts"][0]["member_outcomes"][PARTY[-1]], terminal)

    def test_missing_live_or_unwitnessed_new_life_cannot_retire_a_member(self):
        rows = respawn_samples(witnessed_death=False)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["receipts"], [])
        self.assertNotIn("witnessed_dead_raider_incarnation_continued", report["quality"])
        rows = respawn_samples()
        for record in rows:
            if record["elapsed_seconds"] >= 55:
                fields.actor(record, PARTY[1])["raid"].pop("party", None)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["receipts"], [])

    def test_zero_corpse_lifetime_requires_the_actual_previously_bound_guid(self):
        rows = respawn_samples()
        for record in rows:
            if 26 <= record["elapsed_seconds"] < 56:
                fields.actor(record, PARTY[-1])["runtime_guid"] = "SYNTHETIC-foreign-corpse"
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["receipts"], [])
        self.assertNotIn("witnessed_dead_raider_role_reset_continued", report["quality"])

    def test_a_new_life_cannot_rejoin_the_old_party_binding(self):
        rows = respawn_samples()
        for record in rows:
            if record["elapsed_seconds"] >= 56:
                fields.actor(record, PARTY[-1])["raid"]["party"] = deepcopy(fields.actor(record, PARTY[0])["raid"]["party"])
                fields.actor(record, PARTY[-1])["raid"]["party"]["role"] = "MEMBER"
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["receipts"], [])

    def test_death_cargo_loss_remains_conserved_through_retirement_and_respawn(self):
        rows = respawn_samples(dropped_cargo=True)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["violations"], [])
        rows = deepcopy(rows)
        for record in rows:
            if record["elapsed_seconds"] >= 56:
                fields.actor(record, PARTY[-1])["supply"]["cargo"] = 2
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "FAIL", report)
        self.assertIn("cargo_total_disagrees_with_accounting", [item["code"] for item in report["violations"]])

    def test_counter_rebase_or_capture_gap_still_breaks_retired_member_proof(self):
        rows = respawn_samples(dropped_cargo=True)
        for record in rows:
            if record["elapsed_seconds"] >= 56:
                fields.actor(record, PARTY[-1])["supply"].update(stolen="0", lost="0")
        report = acceptance.evaluate_samples(rows)
        self.assertNotEqual(report["status"], "PASS", report)
        self.assertEqual(report["receipts"], [])
        rows = respawn_samples()
        rows[40]["status"] = "stale"
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["receipts"], [])

    def test_shared_warehouse_trip_preserves_individual_identity_and_all_roster_returns(self):
        report = acceptance.evaluate_samples(samples(), minimum_seconds=120)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["profile"], population.PROFILE)
        self.assertEqual((report["expected_owned"], report["expected_native"]), (55, 45))
        receipt = report["receipts"][0]
        self.assertEqual(set(receipt["joint_travel_members"]), set(PARTY))
        self.assertEqual(set(map(int, receipt["member_outcomes"])), set(PARTY))
        self.assertFalse(receipt["theft_success_certified"])

    def test_actual_guard_repel_and_empty_returns_can_pass_without_claiming_theft(self):
        report = acceptance.evaluate_samples(samples(True), minimum_seconds=120)
        self.assertEqual(report["status"], "PASS", report)
        receipt = report["receipts"][0]
        self.assertEqual(receipt["objective_evidence"][PARTY[0]]["kind"], "GUARD_INTERCEPTION")
        self.assertTrue(receipt["observed_memory_records"])
        self.assertFalse(receipt["theft_success_certified"])

    def test_labels_without_owned_joint_motion_or_without_observed_assembly_are_inconclusive(self):
        rows = deepcopy(samples())
        for record in rows:
            for sid in PARTY:
                fields.actor(record, sid)["action_source_goal"] = None
        report = acceptance.evaluate_samples(rows, minimum_seconds=120)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["receipts"], [])
        self.assertEqual(acceptance.evaluate_samples(samples()[3:], minimum_seconds=110)["status"], "INCONCLUSIVE")

    def test_one_return_does_not_hide_members_still_outside_camp(self):
        rows = deepcopy(samples())
        for record in rows:
            if record["elapsed_seconds"] >= 45:
                actor = fields.actor(record, PARTY[-1])
                fields.move(actor, fields.blend(fields.DEPOTS[5], fields.DEPOTS[3], 17/21))
        report = acceptance.evaluate_samples(rows, minimum_seconds=120)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["receipts"], [])

    def test_a_witnessed_bound_member_death_is_a_terminal_outcome_not_a_fake_return(self):
        rows = deepcopy(samples(True))
        for record in rows:
            if record["elapsed_seconds"] >= 26:
                actor = fields.actor(record, PARTY[-1])
                actor.update(alive=False, health=0, in_combat=False)
                fields.move(actor, dict(x=fields.DEPOTS[3]["x"], y=fields.DEPOTS[3]["y"]-70, z=.15))
                actor.update(action=None, action_source_goal=None, goal=None, goal_owner="")
        report = acceptance.evaluate_samples(rows, minimum_seconds=120)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["receipts"][0]["member_outcomes"][PARTY[-1]]["kind"], "DEATH")

    def test_a_party_teleport_or_freshness_gap_cannot_complete_a_linked_trip(self):
        rows = deepcopy(samples())
        fields.move(fields.actor(rows[12], PARTY[-1]), fields.DEPOTS[1], moving=True)
        self.assertEqual(acceptance.evaluate_samples(rows, minimum_seconds=120)["status"], "INCONCLUSIVE")
        rows = deepcopy(samples())
        rows[20]["status"] = "stale"
        report = acceptance.evaluate_samples(rows, minimum_seconds=90)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["receipts"], [])

    def test_unwitnessed_guard_labels_do_not_prove_a_common_objective(self):
        rows = deepcopy(samples(True))
        for record in rows:
            fields.actor(record, 901026)["target"] = None
        report = acceptance.evaluate_samples(rows, minimum_seconds=120)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)

    def test_conflicting_party_objective_and_hidden_food_break_the_gate(self):
        rows = deepcopy(samples())
        fields.actor(rows[10], PARTY[-1])["raid"]["attempt_id"] = "2"
        report = acceptance.evaluate_samples(rows, minimum_seconds=120)
        self.assertEqual(report["status"], "FAIL", report)
        self.assertIn("conflicting_defias_party_copies", [v["code"] for v in report["violations"]])
        rows = deepcopy(samples())
        fields.actor(rows[60], 901048)["supply"]["cargo"] = 1
        report = acceptance.evaluate_samples(rows, minimum_seconds=120)
        self.assertEqual(report["status"], "FAIL", report)
        self.assertIn("cargo_total_disagrees_with_accounting", [v["code"] for v in report["violations"]])
        self.assertIn("defias_runtime_capability_changed", [v["code"] for v in report["violations"]])

    def test_multi_actor_theft_is_debited_to_each_independent_source(self):
        rows = deepcopy(samples())
        for record in rows:
            t = record["elapsed_seconds"]
            stolen = int(t >= 60)
            for current in record["state"]["agents"]:
                supplied = current.get("supply")
                if supplied:
                    supplied["accounting"]["initial_food"] = "4"
                    supplied["accounting"]["in_cargo"] = str(int(supplied["accounting"]["in_cargo"])+4*stolen)
                    supplied["accounting"]["in_depots"] = str(int(supplied["accounting"]["in_depots"])+4*(1-stolen))
                copies = ([supplied["depot"]] if supplied else [])
                if current.get("raid"):
                    copies += [current["raid"]["source_depot"], current["raid"]["camp_depot"]]
                for depot in copies:
                    if depot["id"] in (4, 5):
                        depot["stock"] += 2*(1-stolen)
                        depot["stolen"] = str(2*stolen)
            for sid, source_id in ((PARTY[0], 5), (901055, 4)):
                actor = fields.actor(record, sid)
                actor["supply"].update(stolen=str(2*stolen), cargo=2*stolen)
                source = next(a["supply"]["depot"] for a in record["state"]["agents"]
                              if a.get("supply") and a["supply"]["depot_id"] == source_id)
                actor["raid"].update(source_depot_id=source_id, source_depot=deepcopy(source))
        evaluator = field_acceptance._Evaluator(120, 2, population_spec=population.Population.load(),
            raider_ids=population.RAIDER_IDS, camp_members=population.DEFIAS_IDS, profile=population.PROFILE)
        for record in rows: evaluator.observe(record)
        self.assertEqual(evaluator.report()["violations"], [])
        self.assertEqual(evaluator.previous_depots[4]["stolen"], "2")
        self.assertEqual(evaluator.previous_depots[5]["stolen"], "2")
        # Assigning the second thief to the wrong warehouse cannot reconcile
        # both real source ledgers merely because the total stolen count fits.
        for record in rows:
            fields.actor(record, 901055)["raid"]["source_depot_id"] = 5
        evaluator = field_acceptance._Evaluator(120, 2, population_spec=population.Population.load(),
            raider_ids=population.RAIDER_IDS, camp_members=population.DEFIAS_IDS)
        for record in rows: evaluator.observe(record)
        self.assertIn("depot_stock_change_not_reconciled", [v["code"] for v in evaluator.report()["violations"]])


if __name__ == "__main__":
    unittest.main()
