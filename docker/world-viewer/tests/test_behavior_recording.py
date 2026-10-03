"""Acceptance checks exercise behavior, gaps and complete recording archives."""
import contextlib
import copy
import gzip
import io
import json
from pathlib import Path
import tempfile
import unittest

from app import behavior


POLICY = behavior.Policy(minimum_seconds=60, return_seconds=30, return_duration_seconds=90, motion_seconds=20,
                         outside_seconds=30, stock_hunger_seconds=30, empty_stock_seconds=30,
                         predator_hunger_seconds=60, prey_hunger_seconds=60)
METADATA = {"format_version": 1, "session_id": "test-session", "interval_seconds": 5, "label": "test"}


def agent(aid=80418, role="PREDATOR"):
    return {"agent_id": aid, "spawn_id": aid, "name": "Forest Spider" if role == "PREDATOR" else role,
            "control_mode": "AI_WORLD_CONTROLLED", "alive": True, "in_combat": False,
            "position": {"source": "live", "map_id": 0, "x": -9606.48, "y": 218.8026, "z": 48.39812},
            "needs": {"hunger": 0.4}, "economy": {"food": 4, "resource": 20},
            "living_role": {"enabled": True, "extensions_enabled": True, "role": role, "status": "READY",
                            "phase": "IDLE", "activity": "NONE", "movement_purpose": "NONE", "hunt_end": "HUNT_LEASH"},
            "movement": {"moving": False, "blocked": False, "evading": False, "home_distance": 0}}


def samples(count=25, npc=None):
    return [{"kind": "sample", "sequence": i + 1, "elapsed_seconds": i * 5,
             "recorded_at_utc": f"test+{i * 5}s", "status": "fresh",
             "state": {"version": 3, "configured": True, "stale": False, "age_ms": 50,
                       "captured_at_ms": 1000000 + i * 5000, "agents": [copy.deepcopy(npc or agent())]}}
            for i in range(count)]


def summary(rows):
    return {**METADATA, "kind": "summary", "status": "finished", "stop_reason": "duration",
            "elapsed_seconds": rows[-1]["elapsed_seconds"] + 5, "samples": len(rows),
            "counts": dict(behavior.Counter(row["status"] for row in rows))}


def evaluate(rows):
    evaluator = behavior.Evaluator(METADATA, POLICY)
    for row in rows:
        evaluator.observe(row)
    return evaluator.finish(summary(rows))


def stranded():
    npc = agent()
    npc["movement"]["home_distance"] = 47.1
    npc["living_role"]["movement_purpose"] = "RETURN_STEP_BLOCKED"
    return npc


def deferred(npc=None):
    npc = copy.deepcopy(npc or agent())
    npc['living_role'].update(phase='IDLE', activity='NONE', movement_purpose='PLANNING_DEFERRED',
                              advice={'lifetime_ms': 1})
    return npc


class BehaviorTests(unittest.TestCase):
    def test_deferred_decision_at_home_is_a_failure_after_300_seconds(self):
        rows = samples(count=61, npc=deferred())
        report = evaluate(rows)
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual([f['check'] for f in report['findings']], ['planning_deferred'])
        finding = report['findings'][0]
        self.assertEqual((finding['start_seconds'], finding['duration_seconds'], finding['home_distance']), (0, 300, 0))
        self.assertEqual(finding['lifetime_ms'], 1)
        self.assertEqual(finding['severity'], 'failure')
        self.assertFalse(evaluate(rows[:-1])['findings'])
        evaluator = behavior.Evaluator(METADATA, POLICY)
        for row in rows: evaluator.observe(row)
        self.assertIn('planning_deferred', [f['check'] for f in evaluator.checkpoint(
            {**summary(rows), 'status': 'running'}, minimum_seconds=60)['findings']])

    def test_deferred_return_survives_rest_without_charging_care_to_decision_wait(self):
        rows = samples(count=85, npc=deferred(stranded()))
        for i, row in enumerate(rows):
            role = row['state']['agents'][0]['living_role']
            role['return_recovery'] = {'failures': 0, 'strategy': 'CORRIDOR', 'failure': 'NONE'}
            if 20 <= i < 24 or 40 <= i < 44:
                role.update(phase='ACTING', activity='REST', movement_purpose='NEEDS_CARE')
        report = evaluate(rows)
        findings = {f['check']: f for f in report['findings']}
        self.assertEqual(findings['return_duration']['duration_seconds'], 420)
        self.assertEqual(findings['planning_deferred']['duration_seconds'], 370)
        self.assertEqual(findings['planning_deferred']['paused_seconds'], 50)
        # A bounded idle handoff after care preserves the pending decision.
        for i in (24, 44):
            rows[i]['state']['agents'][0]['living_role'].update(movement_purpose='NONE')
        findings = {f['check']: f for f in evaluate(rows)['findings']}
        self.assertEqual(findings['planning_deferred']['duration_seconds'], 360)
        self.assertEqual(findings['planning_deferred']['start_seconds'], 0)
        self.assertEqual(findings['return_duration']['duration_seconds'], 420)

    def test_deferred_foraging_does_not_invent_a_home_return(self):
        rows = samples(count=61, npc=deferred(stranded()))
        findings = {f['check'] for f in evaluate(rows)['findings']}
        self.assertEqual(findings, {'planning_deferred'})
        # A return seeded before the deferral also persists on legacy telemetry
        # that does not retain a return_recovery object.
        rows[0]['state']['agents'][0]['living_role']['movement_purpose'] = 'RETURN_HOME'
        self.assertIn('return_duration', [f['check'] for f in evaluate(rows)['findings']])

    def test_ordinary_rest_and_idle_cannot_seed_or_preserve_a_deferred_wait_forever(self):
        for purpose, phase, activity in (('NONE', 'IDLE', 'NONE'), ('NEEDS_CARE', 'ACTING', 'REST'),
                                         ('PLANNING_DEFERRED', 'ACTING', 'REST')):
            with self.subTest(purpose=purpose, phase=phase):
                npc = deferred()
                npc['living_role'].update(movement_purpose=purpose, phase=phase, activity=activity)
                self.assertNotIn('planning_deferred', [f['check'] for f in evaluate(samples(141, npc))['findings']])
        rows = samples(count=141, npc=deferred())
        for i in range(20, len(rows)):
            rows[i]['state']['agents'][0]['living_role'].update(
                phase='ACTING' if i < 24 else 'IDLE', activity='REST' if i < 24 else 'NONE',
                movement_purpose='NEEDS_CARE' if i < 24 else 'NONE')
        self.assertNotIn('planning_deferred', [f['check'] for f in evaluate(rows)['findings']])

    def test_deferred_wait_and_return_reset_on_real_actions_and_lifecycle_interruptions(self):
        interruptions = ('position', 'food', 'hunting', 'feeding', 'eat', 'graze', 'work', 'talk',
            'combat', 'flee', 'root', 'evade', 'group', 'disabled', 'observe', 'dead', 'missing',
            'abstract', 'map', 'gap', 'sequence', 'clock', 'lifetime', 'spawn')
        for interruption in interruptions:
            with self.subTest(interruption=interruption):
                rows = samples(count=61, npc=deferred(stranded()))
                for row in rows:
                    row['state']['agents'][0]['living_role']['return_recovery'] = {'failures': 0}
                row = rows[30]
                npc = row['state']['agents'][0]
                role = npc['living_role']
                if interruption == 'position': npc['position']['z'] += 2
                elif interruption == 'food': npc['needs']['hunger'] = 0.1
                elif interruption in ('hunting', 'feeding'): role['phase'] = interruption.upper()
                elif interruption in ('eat', 'graze', 'work', 'talk'):
                    role.update(phase='ACTING', activity=interruption.upper())
                elif interruption == 'combat': npc['in_combat'] = True
                elif interruption == 'flee': role['phase'] = 'SEEKING_SAFETY'
                elif interruption == 'root': npc['movement']['blocked'] = True
                elif interruption == 'evade': npc['movement']['evading'] = True
                elif interruption == 'group': role['status'] = 'GROUP_ACTIVITY'
                elif interruption == 'disabled': role['enabled'] = False
                elif interruption == 'observe': npc['control_mode'] = 'OBSERVE_ONLY'
                elif interruption == 'dead': npc['alive'] = False
                elif interruption == 'missing': row['state']['agents'] = [agent(42)]
                elif interruption == 'abstract': npc['position']['source'] = 'spawn'
                elif interruption == 'map': npc['position']['map_id'] = 1
                elif interruption == 'gap': row['status'] = 'stale'
                elif interruption == 'sequence': row['sequence'] += 1
                elif interruption == 'clock': row['state']['captured_at_ms'] = 1
                elif interruption in ('lifetime', 'spawn'):
                    for later in rows[30:]:
                        other = later['state']['agents'][0]
                        if interruption == 'lifetime': other['living_role']['advice']['lifetime_ms'] = 2
                        else: other['spawn_id'] = 99
                findings = {f['check'] for f in evaluate(rows)['findings']}
                self.assertNotIn('planning_deferred', findings)
                # Movement is allowed during a timed return, and an unlabelled
                # feeding drop need not prove home arrival. All actual actions
                # and continuity changes still split its episode.
                if interruption not in ('position', 'food'):
                    evaluator = behavior.Evaluator(METADATA, behavior.replace(POLICY, return_duration_seconds=300))
                    for value in rows: evaluator.observe(value)
                    self.assertNotIn('return_duration', [f['check'] for f in evaluator.finish(summary(rows))['findings']])

    def test_deferred_wait_resets_on_xyz_progress_during_care(self):
        rows = samples(count=85, npc=deferred())
        for i in range(20, 45):
            npc = rows[i]['state']['agents'][0]
            npc['position']['z'] += 2
            npc['living_role'].update(phase='ACTING', activity='LOOK', movement_purpose='NEEDS_CARE')
        self.assertNotIn('planning_deferred', [f['check'] for f in evaluate(rows)['findings']])

    def test_local_recovery_movement_does_not_hide_unfinished_home_return(self):
        rows = samples(count=61, npc=stranded())
        for i, row in enumerate(rows):
            npc = row['state']['agents'][0]
            moving = i % 2 == 0
            npc['position']['x'] += 8 * (i % 2)
            npc['movement']['moving'] = moving
            npc['living_role'].update(phase='MOVING' if moving else 'ACTING',
                movement_purpose='LOCAL_RECOVERY_MOVE' if moving else 'LOCAL_RECOVERY_CARE',
                return_recovery={'refuge_active': True, 'failures': 0})
        report = evaluate(rows)
        self.assertIn('return_duration', [f['check'] for f in report['findings']])
        self.assertNotIn('physical_stall', [f['check'] for f in report['findings']])
        for row in rows: row['state']['agents'][0]['position']['x'] = -9606.48
        self.assertIn('physical_stall', [f['check'] for f in evaluate(rows)['findings']])

    def test_early_navigation_pass_does_not_finish_or_clear_long_run(self):
        rows = samples(count=201)
        evaluator = behavior.Evaluator(METADATA)
        for row in rows:
            evaluator.observe(row)
        running = {**summary(rows), 'status': 'running'}
        before = copy.deepcopy(evaluator.__dict__)
        report = evaluator.checkpoint(running)
        self.assertEqual(report['status'], 'PASS')
        self.assertFalse(report['recording_complete'])
        self.assertEqual(report['scope'], 'early_navigation')
        self.assertNotIn('predator_hunger', [c['id'] for c in report['checks']])
        self.assertEqual(evaluator.__dict__, before)
        self.assertEqual(evaluator.finish(summary(rows))['status'], 'INCONCLUSIVE')
        # The same continuing evaluator must still find a later real stall.
        later = samples(count=281, npc=stranded())
        for row in later[201:]:
            evaluator.observe(row)
        self.assertEqual(evaluator.finish(summary(later))['status'], 'FAIL')

    def test_early_navigation_detects_stall_but_cannot_pass_missing_data(self):
        for npc in (agent(), stranded()):
            rows = samples(count=201, npc=npc)
            evaluator = behavior.Evaluator(METADATA)
            for row in rows: evaluator.observe(row)
            result = evaluator.checkpoint({**summary(rows), 'status': 'running'})
            self.assertEqual(result['status'], 'FAIL' if npc['movement']['home_distance'] else 'PASS')
        short = behavior.Evaluator(METADATA)
        rows = samples(count=2)
        for row in rows: short.observe(row)
        self.assertEqual(short.checkpoint({**summary(rows), 'status': 'running'})['status'], 'INCONCLUSIVE')

    def test_early_navigation_rejects_moving_in_circles_without_reaching_home(self):
        rows = samples(count=201, npc=stranded())
        evaluator = behavior.Evaluator(METADATA)
        for i, row in enumerate(rows):
            npc = row['state']['agents'][0]
            npc['position']['x'] += 8 * (i % 2)
            npc['movement']['moving'] = True
            npc['living_role'].update(phase='MOVING', movement_purpose='RETURN_HOME')
            evaluator.observe(row)
        report = evaluator.checkpoint({**summary(rows), 'status': 'running'})
        self.assertEqual(report['status'], 'FAIL')
        self.assertIn('return_duration', [f['check'] for f in report['findings']])
        self.assertNotIn('physical_stall', [f['check'] for f in report['findings']])

    def test_hunger_evidence_distinguishes_absent_prey_from_stale_scan(self):
        rows = samples(npc=agent())
        for row in rows:
            npc = row['state']['agents'][0]
            npc['needs']['hunger'] = 1.0
            npc['living_role']['forage'] = dict(scanned_at_ms=row['state']['captured_at_ms'],
                nearby_prey=0, attackable_prey=0, reachable_prey=0, navigation={'failure': 'NO_COMPLETE_PATH'})
        finding = next(f for f in evaluate(rows)['findings'] if f['check'] == 'predator_hunger')
        self.assertEqual(finding['food_observation'], 'NO_LOCAL_PREY')
        self.assertEqual(finding['forage']['navigation']['failure'], 'NO_COMPLETE_PATH')
        for row in rows: row['state']['agents'][0]['living_role']['forage']['scanned_at_ms'] = 1
        finding = next(f for f in evaluate(rows)['findings'] if f['check'] == 'predator_hunger')
        self.assertEqual(finding['food_observation'], 'STALE_SCAN')

    def test_threat_return_loop_is_reported_separately_from_calm_hunger(self):
        rows = samples(count=141, npc=agent(role='PREY'))
        for i, row in enumerate(rows):
            npc = row['state']['agents'][0]
            npc['needs']['hunger'] = 1.0
            npc['position']['x'] += (i % 3) * 8
            npc['living_role'].update(awareness='REMEMBERED_DANGER', danger_remaining_ms=50000,
                phase='SEEKING_SAFETY' if i % 3 == 0 else 'MOVING', movement_purpose='RETURN_HOME')
        findings = evaluate(rows)['findings']
        self.assertFalse([f for f in findings if f['check'] == 'prey_hunger'])
        fear = [f for f in findings if f['check'] == 'prey_threat_hunger']
        self.assertEqual(len(fear), 1)
        self.assertEqual(fear[0]['severity'], 'warning')
        self.assertEqual(fear[0]['duration_seconds'], 700)

    def test_queue_wait_checks_actual_tickets_and_accounts_for_load(self):
        rows = samples(count=141)
        for i, row in enumerate(rows):
            row['state']['agents'][0]['living_role']['advice'] = {
                'lifetime_ms': 1, 'enabled': True, 'status': 'WAITING_TURN',
                'queue_wait_ms': i * 5000, 'queue_size': 2, 'queue_dispatchable': True}
        self.assertEqual(len([f for f in evaluate(rows)['findings'] if f['check'] == 'advice_wait']), 1)
        for row in rows:
            row['state']['agents'][0]['living_role']['advice']['queue_size'] = 100
        self.assertFalse([f for f in evaluate(rows)['findings'] if f['check'] == 'advice_wait'])
        for row in rows:
            advice = row['state']['agents'][0]['living_role']['advice']
            advice['queue_size'] = 2
            advice['queue_wait_ms'] %= 300000  # served and queued again between samples
        self.assertFalse([f for f in evaluate(rows)['findings'] if f['check'] == 'advice_wait'])
        for row in rows:
            row['state']['agents'][0]['living_role']['advice'].pop('queue_wait_ms')
        report = evaluate(rows)
        self.assertEqual(next(c['status'] for c in report['checks'] if c['id'] == 'advice_wait'), 'NOT_OBSERVED')

    def test_physical_stall_survives_flee_idle_and_rest_changes(self):
        rows = samples(count=81, npc=stranded())
        for i, row in enumerate(rows):
            role = row['state']['agents'][0]['living_role']
            role.update(phase=('FLEEING' if i % 2 else 'ACTING'),
                        movement_purpose=('BOUNDED_ESCAPE', 'ESCAPE_NO_PATH', 'ESCAPE_NAV_REJOIN', 'RETURN_NO_PATH')[i % 4])
        report = evaluate(rows)
        physical = [f for f in report['findings'] if f['check'] == 'physical_stall']
        self.assertEqual(len(physical), 1)
        self.assertEqual(physical[0]['duration_seconds'], 400)
        self.assertFalse([f for f in report['findings'] if f['check'] == 'return'])

    def test_physical_stall_resets_on_displacement_home_or_lifecycle(self):
        for interruption in ('position', 'home', 'dead', 'missing', 'gap', 'map'):
            with self.subTest(interruption=interruption):
                rows = samples(count=101, npc=stranded())
                npc = rows[50]['state']['agents'][0]
                if interruption == 'position': npc['position']['x'] += 2
                elif interruption == 'home': npc['movement']['home_distance'] = 0
                elif interruption == 'dead': npc['alive'] = False
                elif interruption == 'missing': rows[50]['state']['agents'] = []
                elif interruption == 'gap': rows[50]['status'] = 'stale'
                else: npc['position']['map_id'] = 1
                self.assertNotIn('physical_stall', [f['check'] for f in evaluate(rows)['findings']])

    def test_physical_stall_suspends_combat_and_roots_without_claiming_recovery(self):
        rows = samples(count=141, npc=stranded())
        for i, row in enumerate(rows):
            npc = row['state']['agents'][0]
            npc['in_combat'] = 20 <= i < 40
            npc['movement']['blocked'] = 60 <= i < 80
        physical = [f for f in evaluate(rows)['findings'] if f['check'] == 'physical_stall']
        self.assertEqual(physical[0]['duration_seconds'], 490)
        # Resting away from home, without any failed movement evidence, is
        # insufficient to accuse a worker of being physically stuck.
        rows = samples(count=141, npc=agent(role='WORKER'))
        for row in rows:
            npc = row['state']['agents'][0]
            npc['movement']['home_distance'] = 50
            npc['living_role'].update(phase='ACTING', activity='WORK')
        self.assertNotIn('physical_stall', [f['check'] for f in evaluate(rows)['findings']])

    def test_advice_uses_lifetime_maxima_and_never_masks_a_stalled_return(self):
        rows = samples(npc=stranded())
        for i, row in enumerate(rows):
            advice = {"lifetime_ms": 100 if i < 12 else 200, "enabled": True,
                "status": "MOVE_STARTED", "requests": 2, "selected": 1, "started": 1,
                "arrived": 0, "home_success": 0, "food_success": 0, "rejected": 0,
                "unavailable": 0, "reused": 0}
            row['state']['agents'][0]['living_role']['advice'] = advice
        report = evaluate(rows)
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual(len(report['recovery_advice']), 2)
        self.assertEqual(sum(a['counters']['requests'] for a in report['recovery_advice']), 4)
        self.assertEqual(sum(a['counters']['home_success'] for a in report['recovery_advice']), 0)
        with tempfile.TemporaryDirectory() as temp:
            behavior.write_report(report, Path(temp))
            text = (Path(temp) / 'behavior-report.md').read_text(encoding='utf-8')
            self.assertIn('Pomoc lokální AI', text)
            self.assertIn('MOVE_STARTED', text)

    def test_version_four_preserves_return_diagnostics_through_idle(self):
        rows = samples(npc=stranded())
        for row in rows:
            row['state']['version'] = 4
            role = row['state']['agents'][0]['living_role']
            role['movement_purpose'] = 'NONE'
            role['phase'] = 'IDLE'
            role['return_recovery'] = {'failures': 8, 'strategy': 'TRAIL', 'failure': 'RETURN_NO_PATH',
                                       'rejected': {'path': 8}, 'stalled_ms': 600000}
        report = evaluate(rows)
        self.assertEqual(report['quality']['status'], 'PASS')
        self.assertEqual(report['findings'][0]['return_recovery']['strategy'], 'TRAIL')
        self.assertEqual(report['findings'][0]['check'], 'return')

    def test_empty_worker_stock_is_checked_even_without_food_to_consume(self):
        npc = agent(80683, 'WORKER')
        npc.update(entry=250, home={'map_id': 0}, work={'map_id': 0})
        npc['needs']['hunger'] = 1
        npc['economy']['food'] = 0
        rows = samples(npc=npc)
        report = evaluate(rows)
        self.assertEqual([f['check'] for f in report['findings']], ['empty_stock'])
        # Completed resupply interrupts starvation; ordinary short work/meal
        # cycles are not failures, nor is a woodworker's empty food inventory.
        for row in rows:
            if row['sequence'] % 5 == 0:
                row['state']['agents'][0]['economy']['food'] = 2
        self.assertFalse(evaluate(rows)['findings'])
        npc['entry'] = 1975
        self.assertFalse(evaluate(samples(npc=npc))['findings'])

    def test_legitimate_stationary_roles_and_stock_cap_do_not_fail(self):
        for role in ("SERVICE", "WORKER", "PREDATOR", "PREY", "GUARD"):
            with self.subTest(role=role):
                report = evaluate(samples(npc=agent(role=role)))
                self.assertEqual(report["status"], "PASS")
                self.assertFalse(report["findings"])
                self.assertEqual(report["roles"][role]["resource_increase"], 0)

    def test_long_blocked_return_survives_recovery_animation_and_idle_labels(self):
        rows = samples(npc=stranded())
        for i, row in enumerate(rows):
            role = row["state"]["agents"][0]["living_role"]
            role["phase"] = "ACTING" if i % 2 else "IDLE"
            role["movement_purpose"] = "RETURN_LOS_BLOCKED" if i % 3 == 0 else "NONE"
        report = evaluate(rows)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["exit_code"], 3)
        finding = report["findings"][0]
        self.assertEqual((finding["spawn_id"], finding["check"], finding["duration_seconds"]), (80418, "return", 120))

    def test_actual_return_progress_resets_stationary_episode(self):
        rows = samples(npc=stranded())
        for i, row in enumerate(rows):
            row["state"]["agents"][0]["position"]["x"] += i * 2
        self.assertNotIn("return", [f["check"] for f in evaluate(rows)["findings"]])

    def test_moving_return_loop_is_detected_across_idle_and_recovery_breaks(self):
        rows = samples(npc=stranded())
        for i, row in enumerate(rows):
            npc = row["state"]["agents"][0]
            npc["position"]["x"] += (i % 2) * 12
            npc["movement"]["moving"] = i % 3 == 0
            npc["living_role"].update(phase="MOVING" if i % 3 == 0 else "ACTING",
                                      movement_purpose="RETURN_HOME" if i % 3 == 0 else "NONE")
        report = evaluate(rows)
        self.assertEqual([f["check"] for f in report["findings"]], ["return_duration"])
        self.assertEqual(report["findings"][0]["duration_seconds"], 120)

    def test_long_return_ends_on_arrival_or_new_activity_or_interruption(self):
        for interruption in ("home", "forage", "combat", "flee", "group", "gap", "absent", "dead", "abstract"):
            with self.subTest(interruption=interruption):
                rows = samples(npc=stranded())
                for i, row in enumerate(rows):
                    npc = row["state"]["agents"][0]
                    npc["position"]["x"] += i * 2
                    npc["living_role"].update(phase="MOVING", movement_purpose="RETURN_HOME")
                npc = rows[12]["state"]["agents"][0]
                if interruption == "home": npc["movement"]["home_distance"] = 0
                elif interruption == "forage": npc["living_role"]["movement_purpose"] = "FORAGE_SEARCH"
                elif interruption == "combat": npc["in_combat"] = True
                elif interruption == "flee": npc["living_role"]["phase"] = "SEEKING_SAFETY"
                elif interruption == "group": npc["living_role"]["status"] = "GROUP_ACTIVITY"
                elif interruption == "gap": rows[12]["status"] = "stale"
                elif interruption == "absent": rows[12]["state"]["agents"] = [agent(42)]
                elif interruption == "dead": npc["alive"] = False
                else: npc["position"]["source"] = "spawn"
                self.assertNotIn("return_duration", [f["check"] for f in evaluate(rows)["findings"]])

    def test_prey_starvation_is_a_failure_even_during_movement_but_grazing_resets_it(self):
        npc = agent(80672, "PREY")
        npc["needs"]["hunger"] = 1
        rows = samples(npc=npc)
        for i, row in enumerate(rows):
            npc = row["state"]["agents"][0]
            npc["position"]["x"] += i * 2
            npc["living_role"].update(phase="MOVING", movement_purpose="LOCAL_ROAM")
        self.assertEqual([f["check"] for f in evaluate(rows)["findings"]], ["prey_hunger"])
        for row in rows:
            if row["sequence"] % 6 == 0:
                row["state"]["agents"][0]["needs"]["hunger"] = 0.3
        self.assertFalse(evaluate(rows)["findings"])

    def test_distant_foraging_is_not_an_observed_return(self):
        rows = samples(npc=stranded())
        for i, row in enumerate(rows):
            npc = row["state"]["agents"][0]
            npc["position"]["x"] += i * 2
            npc["living_role"].update(phase="MOVING", movement_purpose="FORAGE_SEARCH")
        self.assertFalse(evaluate(rows)["findings"])

    def test_claimed_movement_without_position_change_is_detected(self):
        npc = agent()
        npc["movement"]["moving"] = True
        npc["living_role"].update(phase="MOVING", movement_purpose="FORAGE_SEARCH")
        report = evaluate(samples(npc=npc))
        self.assertEqual(report["findings"][0]["check"], "motion")
        npc["movement"]["blocked"] = True  # a root is not failed navigation
        self.assertFalse(evaluate(samples(npc=npc))["findings"])

    def test_no_episode_crosses_loss_of_fresh_live_identity(self):
        for interruption in ("stale", "absent", "dead", "abstract", "combat", "root", "clock", "sequence", "invalid"):
            with self.subTest(interruption=interruption):
                rows = samples(13, stranded())
                row = rows[6]
                npc = row["state"]["agents"][0]
                if interruption == "stale":
                    row["status"] = "stale"
                    row["state"]["stale"] = True
                elif interruption == "absent":
                    row["state"]["agents"] = [agent(42)]
                elif interruption == "dead": npc["alive"] = False
                elif interruption == "abstract": npc["position"]["source"] = "spawn"
                elif interruption == "combat": npc["in_combat"] = True
                elif interruption == "root": npc["movement"]["blocked"] = True
                elif interruption == "clock": row["state"]["captured_at_ms"] = 1
                elif interruption == "sequence": row["sequence"] += 1
                elif interruption == "invalid": npc["position"]["z"] = float("nan")
                self.assertFalse(evaluate(rows)["findings"])

    def test_clock_repeat_long_gap_duplicates_and_unknown_protocol_are_inconclusive(self):
        for problem in ("repeat", "gap", "duplicate", "protocol", "unknown-fields", "bad-position", "bad-state"):
            with self.subTest(problem=problem):
                rows = samples()
                if problem == "repeat": rows[6]["state"]["captured_at_ms"] = rows[5]["state"]["captured_at_ms"]
                elif problem == "gap": rows[6]["elapsed_seconds"] += 40
                elif problem == "duplicate": rows[6]["state"]["agents"] *= 2
                elif problem == "protocol": rows[6]["state"]["version"] = 100
                elif problem == "bad-position": rows[6]["state"]["agents"][0]["position"] = "invalid"
                elif problem == "bad-state": rows[6]["state"] = [1]
                else: del rows[6]["state"]["agents"][0]["movement"]["blocked"]
                self.assertEqual(evaluate(rows)["status"], "INCONCLUSIVE")

    def test_short_and_unobserved_runs_never_pass(self):
        self.assertEqual(evaluate(samples(3))["status"], "INCONCLUSIVE")
        for mode in ("OBSERVE_ONLY", "AI_WORLD_CONTROLLED"):
            npc = agent()
            npc["control_mode"] = mode
            npc["living_role"]["status"] = "OUTSIDE_ELWYNN"
            self.assertEqual(evaluate(samples(npc=npc))["status"], "INCONCLUSIVE")

    def test_lowering_minimum_does_not_claim_unfinished_long_checks_passed(self):
        evaluator = behavior.Evaluator(METADATA, behavior.Policy(minimum_seconds=60))
        rows = samples()
        for row in rows:
            evaluator.observe(row)
        report = evaluator.finish(summary(rows))
        self.assertEqual(report["quality"]["status"], "PASS")
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertEqual(next(c for c in report["checks"] if c["id"] == "predator_hunger")["status"], "INCONCLUSIVE")

    def test_leaving_scope_requires_observed_prior_control(self):
        rows = samples(npc=agent(54003, "TRAVELER"))
        for row in rows[1:]:
            row["state"]["agents"][0]["living_role"]["status"] = "OUTSIDE_ELWYNN"
        report = evaluate(rows)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["findings"][0]["spawn_id"], 54003)
        self.assertEqual(report["findings"][0]["check"], "outside")

    def test_hunger_with_stock_and_actual_eating(self):
        npc = agent(80683, "WORKER")
        npc["needs"]["hunger"] = 1
        self.assertEqual(evaluate(samples(npc=npc))["findings"][0]["check"], "stock_hunger")
        rows = samples(npc=npc)
        for i, row in enumerate(rows):
            if i >= 3:
                row["state"]["agents"][0]["needs"]["hunger"] = 0.1
                row["state"]["agents"][0]["economy"]["food"] = 3
        report = evaluate(rows)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["roles"]["WORKER"]["food_decrease"], 1)
        self.assertEqual(report["roles"]["WORKER"]["hunger_drops"], 1)
        npc["economy"]["food"] = 0
        self.assertFalse(evaluate(samples(npc=npc))["findings"])

    def test_hungry_predators_are_warnings_and_retained_hunt_end_is_not_a_new_event(self):
        npc = agent()
        npc["needs"]["hunger"] = 1
        report = evaluate(samples(npc=npc))
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["findings"][0]["severity"], "warning")
        self.assertEqual(report["roles"]["PREDATOR"]["hunting_starts"], 0)

    def test_metrics_do_not_bridge_death_or_missing_agent(self):
        rows = samples()
        rows[2]["state"]["agents"][0]["alive"] = False
        for row in rows[3:]:
            row["state"]["agents"][0]["economy"]["food"] = 1
            row["state"]["agents"][0]["needs"]["hunger"] = 0.1
        metrics = evaluate(rows)["roles"]["PREDATOR"]
        self.assertEqual(metrics["food_decrease"], 0)
        self.assertEqual(metrics["hunger_drops"], 0)


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name) / "session"
        self.directory.mkdir()

    def archive(self, rows, split=10):
        result = summary(rows)
        content = rows + [{k: v for k, v in result.items() if k not in METADATA}]
        parts = []
        for start in range(0, len(content), split):
            part = len(parts) + 1
            name = f"part-{part:04d}.jsonl.gz"
            with gzip.open(self.directory / name, "wt", encoding="utf-8") as stream:
                stream.write(json.dumps({**METADATA, "kind": "session", "part": part}) + "\n")
                for row in content[start:start + split]:
                    stream.write(json.dumps(row) + "\n")
            parts.append(name)
        result["parts"] = parts
        (self.directory / "summary.json").write_text(json.dumps(result), encoding="utf-8")
        return result

    def test_offline_archive_and_stream_evaluation_agree_and_reports_are_written(self):
        rows = samples(npc=stranded())
        self.archive(rows)
        report = behavior.analyze(self.directory, POLICY)
        self.assertEqual(report, evaluate(rows))
        output = Path(self.temp.name) / "report"
        behavior.write_report(report, output)
        self.assertEqual(json.loads((output / "behavior-report.json").read_text(encoding="utf-8"))["status"], "FAIL")
        self.assertIn("80418", (output / "behavior-report.md").read_text(encoding="utf-8"))

    def test_missing_corrupt_parts_and_traversal_cannot_pass(self):
        for problem in ("missing", "truncated", "crc", "path", "counts", "session", "running"):
            with self.subTest(problem=problem):
                info = self.archive(samples())
                part = self.directory / info["parts"][1]
                if problem == "missing": part.unlink()
                elif problem == "truncated": part.write_bytes(part.read_bytes()[:-5])
                elif problem == "crc":
                    data = bytearray(part.read_bytes())
                    data[-8] ^= 1
                    part.write_bytes(data)
                elif problem == "path": info["parts"][1] = "../secret.jsonl.gz"
                elif problem == "counts": info["samples"] += 1
                elif problem == "session": info["session_id"] = "wrong"
                elif problem == "running": info["status"] = "running"
                (self.directory / "summary.json").write_text(json.dumps(info), encoding="utf-8")
                report = behavior.analyze(self.directory, POLICY)
                self.assertEqual(report["status"], "INCONCLUSIVE")
                self.assertTrue(report["quality"]["integrity_errors"])

    def test_cli_exit_codes_and_separate_output_preserve_inputs(self):
        service = agent(role="SERVICE")
        service["economy"]["food"] = 0
        for rows, expected in ((samples(npc=service), 0), (samples(3), 2), (samples(70, stranded()), 3)):
            with self.subTest(expected=expected):
                self.archive(rows, 100)
                original = {p: p.read_bytes() for p in self.directory.iterdir()}
                with contextlib.redirect_stdout(io.StringIO()):
                    code = behavior.main([str(self.directory), "--minimum-minutes", "1",
                                          "--output", str(Path(self.temp.name) / "out")])
                self.assertEqual(code, expected)
                self.assertTrue(all(p.read_bytes() == data for p, data in original.items()))

    def test_cli_deferred_threshold_is_configurable_and_rejects_nonpositive_nonfinite_values(self):
        self.archive(samples(npc=deferred()), 100)
        output = Path(self.temp.name) / 'out'
        with contextlib.redirect_stdout(io.StringIO()):
            code = behavior.main([str(self.directory), '--minimum-minutes', '1',
                                  '--planning-deferred-seconds', '60', '--output', str(output)])
        self.assertEqual(code, 3)
        report = json.loads((output / 'behavior-report.json').read_text(encoding='utf-8'))
        self.assertEqual(report['policy']['planning_deferred_seconds'], 60)
        self.assertEqual([f['check'] for f in report['findings']], ['planning_deferred'])
        for value in ('0', '-1', 'nan', 'inf'):
            with self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    behavior.main([str(self.directory), f'--planning-deferred-seconds={value}'])
                self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
