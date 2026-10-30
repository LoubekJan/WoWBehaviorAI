"""Raid observations preserve finite stolen portions and explicit attempt identity."""
import copy
import unittest

from pydantic import ValidationError

from app.telemetry import Agent, LivingRaid, LivingSupply, TelemetryBatch
from test_field_telemetry import farmer
from test_supply_telemetry import agent, supply


def raid():
    source = copy.deepcopy(supply()["depot"])
    source.update(stock=4, capacity=30, low_threshold=4, delivered="6", consumed="0", stolen="2")
    camp = copy.deepcopy(source)
    camp.update(id=3, name="Defias camp", stock=0, delivered="0", consumed="0", stolen="0")
    camp["position"].update(x=58.6867446881, y=971.2687759381, z=0.15)
    return dict(enabled=True, role="RAIDER", phase="RETURNING_CAMP", outcome="STOLEN", attempt_id="1",
                target_spawn_id="901039", noticed="0", interceptions="0", thefts="1", delivered="0",
                prevented="0", source_depot_id=1, camp_depot_id=3, source_depot=source, camp_depot=camp)


def raider():
    current = agent(901055)
    current["raid"] = raid()
    current["supply"] = supply()
    current["supply"].update(role="RAIDER", capacity=2, cargo=2, depot_id=3,
                             hunting_ground=None, harvested="0", stolen="2", depot=copy.deepcopy(current["raid"]["camp_depot"]))
    return current


def planning():
    return dict(reason="EXPLORE_UNKNOWN", memory_records=1, route_id=2, known_stock=None,
                stock_age_ms=None, risk=0.0, retry_remaining_ms=0,
                danger=dict(position=dict(map_id=725, x=40.25, y=872.56, z=0.15),
                            guard_spawn_id="901026", age_ms=120000))


def party(*, leader="901055", phase="OUTBOUND", role="LEADER"):
    return dict(id="18446744073709551615", leader_spawn_id=leader,
                member_spawn_ids=["901055", "901056"], phase=phase, role=role)


class RaidTelemetryTests(unittest.TestCase):
    def test_party_identity_roster_and_actor_role_roundtrip_without_combining_food(self):
        first, second = raider(), raider()
        first["raid"].update(party=party(), planning=planning())
        second.update(agent_id=901056, spawn_id=901056)
        second["raid"].update(party=party(role="MEMBER"), planning=planning())
        second["supply"].update(cargo=0, stolen="0")
        parsed = TelemetryBatch.model_validate(dict(version=4, captured_at_ms=1000, agents=[first, second]))
        self.assertEqual(parsed.agents[0].raid.party.id, str(2**64-1))
        self.assertEqual([actor.supply.cargo for actor in parsed.agents], [2, 0])
        self.assertEqual(TelemetryBatch.model_validate_json(parsed.model_dump_json()), parsed)

    def test_party_rejects_foreign_members_invalid_leaders_and_conflicting_objectives(self):
        for mutation in ("zero_id", "duplicate", "zero_member", "outside_leader", "foreign_actor", "role", "guard", "too_many"):
            bad = raider(); bad["raid"]["party"] = party()
            current = bad["raid"]["party"]
            if mutation == "zero_id": current["id"] = "0"
            elif mutation == "duplicate": current["member_spawn_ids"] = ["901055", "901055"]
            elif mutation == "zero_member": current["member_spawn_ids"] = ["901055", "0"]
            elif mutation == "outside_leader": current["leader_spawn_id"] = "901050"
            elif mutation == "foreign_actor": current["member_spawn_ids"] = ["901056"]
            elif mutation == "role": current["role"] = "MEMBER"
            elif mutation == "guard": bad["raid"]["role"] = "GUARD"
            elif mutation == "too_many": current["member_spawn_ids"] += [str(901057+i) for i in range(4)]
            with self.subTest(mutation=mutation), self.assertRaises(ValidationError): Agent.model_validate(bad)
        first, second = raider(), raider()
        first["raid"].update(party=party(), planning=planning())
        second.update(agent_id=901056, spawn_id=901056)
        second["raid"].update(party=party(role="MEMBER"), planning=planning())
        for mutation in ("source", "attempt", "route", "phase", "leader"):
            bad = copy.deepcopy(second)
            if mutation == "source":
                bad["raid"]["source_depot_id"] = bad["raid"]["source_depot"]["id"] = 4
            elif mutation == "attempt": bad["raid"]["attempt_id"] = "2"
            elif mutation == "route": bad["raid"]["planning"]["route_id"] = 1
            elif mutation == "phase": bad["raid"]["party"]["phase"] = "RETURNING"
            elif mutation == "leader": bad["raid"]["party"].update(leader_spawn_id="901056", role="LEADER")
            with self.subTest(mutation=mutation), self.assertRaises(ValidationError):
                TelemetryBatch.model_validate(dict(version=4, captured_at_ms=1000, agents=[first, bad]))

    def test_live_raid_is_optional_and_decimal_attempt_and_event_counters_roundtrip(self):
        self.assertIsNone(Agent.model_validate(agent()).raid)
        current = raider()
        current["raid"].update(attempt_id=str(2**64 - 1), noticed=str(2**64 - 1))
        parsed = Agent.model_validate(current)
        self.assertEqual(parsed.raid.attempt_id, str(2**64 - 1))
        self.assertEqual(parsed.supply.stolen, "2")
        self.assertEqual(parsed.raid.source_depot.stolen, "2")
        self.assertEqual(Agent.model_validate_json(parsed.model_dump_json()), parsed)

    def test_raider_cannot_acquire_hunting_or_more_than_two_portions(self):
        for field, invalid in (("capacity", 6), ("cargo", 3), ("harvested", "1"),
                               ("hunting_ground", dict(map_id=725, x=0, y=0, z=0)),
                               ("carcass_remaining", 1), ("depot", None)):
            with self.subTest(field=field):
                bad = raider()["supply"]
                bad[field] = invalid
                with self.assertRaises(ValidationError): LivingSupply.model_validate(bad)
        hunter = supply()
        hunter["stolen"] = "1"
        with self.assertRaises(ValidationError): LivingSupply.model_validate(hunter)

    def test_planning_keeps_unknown_stock_and_route_danger_separate_from_real_source(self):
        current = raider()
        current["raid"].update(source_depot_id=6, planning=planning())
        current["raid"]["source_depot"].update(id=6, name="Right village", stock=30)
        current["raid"]["source_depot"]["position"].update(x=151.1844, y=282.1731)
        parsed = Agent.model_validate(current)
        self.assertIsNone(parsed.raid.planning.known_stock)
        self.assertIsNone(parsed.raid.planning.stock_age_ms)
        self.assertEqual(parsed.raid.source_depot.stock, 30)
        self.assertEqual(parsed.raid.planning.danger.guard_spawn_id, "901026")
        self.assertNotEqual(parsed.raid.planning.danger.position, parsed.raid.source_depot.position)
        self.assertEqual(Agent.model_validate_json(parsed.model_dump_json()), parsed)
        current["raid"]["planning"].update(known_stock=7, stock_age_ms=300000)
        parsed = Agent.model_validate(current)
        self.assertEqual(parsed.raid.planning.known_stock, 7)
        self.assertEqual(parsed.raid.source_depot.stock, 30)

    def test_local_guard_checks_are_optional_bounded_and_separate_from_remembered_danger(self):
        current = raider(); current["raid"]["planning"] = planning()
        parsed = Agent.model_validate(current)
        self.assertEqual(parsed.raid.planning.checked_positions, 0)
        self.assertIsNone(parsed.raid.planning.last_check)
        checked = copy.deepcopy(planning()["danger"])
        checked.update(guard_spawn_id=str(2**64-1), age_ms=2500)
        current["raid"]["planning"].update(checked_positions=2, last_check=checked)
        parsed = Agent.model_validate(current)
        self.assertEqual(parsed.raid.planning.checked_positions, 2)
        self.assertEqual(parsed.raid.planning.last_check.guard_spawn_id, str(2**64-1))
        self.assertEqual(parsed.raid.planning.last_check.age_ms, 2500)
        self.assertEqual(parsed.raid.planning.danger.age_ms, 120000)
        self.assertEqual(Agent.model_validate_json(parsed.model_dump_json()), parsed)

    def test_local_guard_checks_reject_invalid_counts_locations_and_event_identity(self):
        for value in (-1, 129, True, 1.5, "1"):
            bad = raider(); bad["raid"]["planning"] = planning()
            bad["raid"]["planning"]["checked_positions"] = value
            with self.subTest(count=value), self.assertRaises(ValidationError): Agent.model_validate(bad)
        for mutation in ("foreign_map", "nan", "infinity", "numeric_guard", "leading_zero_guard", "negative_age", "boolean_age", "incomplete"):
            bad = raider(); bad["raid"]["planning"] = planning()
            checked = copy.deepcopy(planning()["danger"])
            if mutation == "foreign_map": checked["position"]["map_id"] = 0
            elif mutation == "nan": checked["position"]["x"] = float("nan")
            elif mutation == "infinity": checked["position"]["z"] = float("inf")
            elif mutation == "numeric_guard": checked["guard_spawn_id"] = 901026
            elif mutation == "leading_zero_guard": checked["guard_spawn_id"] = "0901026"
            elif mutation == "negative_age": checked["age_ms"] = -1
            elif mutation == "boolean_age": checked["age_ms"] = True
            elif mutation == "incomplete": checked.pop("position")
            bad["raid"]["planning"].update(checked_positions=1, last_check=checked)
            with self.subTest(mutation=mutation), self.assertRaises(ValidationError): Agent.model_validate(bad)
    def test_planning_rejects_unbounded_or_incomplete_observations_and_guard_ownership(self):
        for field, value in (("memory_records", 129), ("memory_records", True), ("route_id", -1), ("route_id", 3),
                             ("known_stock", 31), ("stock_age_ms", -1), ("risk", float("nan")),
                             ("risk", float("inf")), ("risk", -0.1), ("risk", 21), ("risk", True),
                             ("retry_remaining_ms", -1), ("reason", "x"*81)):
            with self.subTest(field=field, value=value):
                bad = raider(); bad["raid"]["planning"] = planning()
                bad["raid"]["planning"][field] = value
                with self.assertRaises(ValidationError): Agent.model_validate(bad)
        for observation in ((7, None), (None, 500)):
            bad = raider(); bad["raid"]["planning"] = planning()
            bad["raid"]["planning"].update(zip(("known_stock", "stock_age_ms"), observation))
            with self.assertRaises(ValidationError): Agent.model_validate(bad)
        bad = raider(); bad["raid"]["planning"] = planning()
        bad["raid"]["planning"]["danger"]["position"]["map_id"] = 0
        with self.assertRaises(ValidationError): Agent.model_validate(bad)
        bad = raid(); bad.update(role="GUARD", planning=planning())
        with self.assertRaises(ValidationError): LivingRaid.model_validate(bad)

    def test_raider_can_observe_each_village_source_without_changing_own_camp(self):
        for depot_id, name in ((1, "South"), (4, "Top"), (5, "Left"), (6, "Right")):
            with self.subTest(depot_id=depot_id):
                current = raider()
                current["raid"]["source_depot_id"] = depot_id
                current["raid"]["source_depot"].update(id=depot_id, name=name, stolen="2")
                # Actor counters cover every attempt; this depot reports only
                # the portions withdrawn from this particular warehouse.
                current["supply"]["stolen"] = "8"
                parsed = Agent.model_validate(current)
                self.assertEqual(parsed.raid.source_depot.id, depot_id)
                self.assertEqual(parsed.raid.source_depot.stolen, "2")
                self.assertEqual(parsed.supply.stolen, "8")
                self.assertEqual((parsed.supply.depot_id, parsed.raid.camp_depot_id), (3, 3))
                self.assertEqual(Agent.model_validate_json(parsed.model_dump_json()), parsed)
                current["raid"]["source_depot_id"] = depot_id + 10
                with self.assertRaises(ValidationError): Agent.model_validate(current)

    def test_switching_target_preserves_each_warehouse_stock_and_theft_counter(self):
        thief = raider()
        workers = []
        for depot_id, spawn, stock, stolen in ((4, 901016, 24, "2"), (5, 901022, 19, "4"), (6, 901028, 13, "0")):
            worker = farmer()
            worker.update(agent_id=spawn, spawn_id=spawn)
            worker["supply"].update(field_id=depot_id, depot_id=depot_id)
            worker["supply"]["depot"].update(id=depot_id, name=f"Village {depot_id}", stock=stock, stolen=stolen)
            workers.append(worker)
        for worker in workers:
            with self.subTest(source_depot_id=worker["supply"]["depot_id"]):
                thief["raid"].update(source_depot_id=worker["supply"]["depot_id"],
                    source_depot=copy.deepcopy(worker["supply"]["depot"]))
                batch = dict(version=4, captured_at_ms=1000, agents=[thief, *workers])
                self.assertEqual(len(TelemetryBatch.model_validate(batch).agents), 4)
                mismatched = copy.deepcopy(batch)
                mismatched["agents"][0]["raid"]["source_depot"]["stolen"] = "99"
                with self.assertRaises(ValidationError): TelemetryBatch.model_validate(mismatched)

    def test_guard_keeps_its_zero_cargo_consumer_capability(self):
        current = raider()
        current.update(agent_id=901039, spawn_id=901039, type="GUARD")
        current["raid"].update(role="GUARD", phase="INTERCEPTING", target_spawn_id="901055")
        current["supply"].update(role="CONSUMER", capacity=0, cargo=0, depot_id=1, stolen="0",
                                 depot=copy.deepcopy(current["raid"]["source_depot"]))
        self.assertEqual(Agent.model_validate(current).raid.role, "GUARD")
        current["supply"]["role"] = "RAIDER"
        with self.assertRaises(ValidationError): Agent.model_validate(current)

    def test_raid_requires_live_controlled_supply_and_depots_on_the_actor_map(self):
        for mutation in ("native", "spawn", "observe", "supply", "map"):
            with self.subTest(mutation=mutation):
                bad = raider()
                if mutation == "native": bad.update(native_creature=True, control_mode="OBSERVE_ONLY", needs=None)
                elif mutation == "spawn": bad["position"]["source"] = "spawn"
                elif mutation == "observe": bad["control_mode"] = "OBSERVE_ONLY"
                elif mutation == "supply": bad["supply"] = None
                else: bad["raid"]["source_depot"]["position"]["map_id"] = 0
                with self.assertRaises(ValidationError): Agent.model_validate(bad)

    def test_attempts_counters_and_depot_identity_are_strict(self):
        for field in ("attempt_id", "target_spawn_id", "noticed", "interceptions", "thefts", "delivered", "prevented"):
            for invalid in (1, True, "-1", "01", str(2**64)):
                with self.subTest(field=field, invalid=invalid):
                    bad = raid(); bad[field] = invalid
                    with self.assertRaises(ValidationError): LivingRaid.model_validate(bad)
        for field, invalid in (("source_depot_id", 3), ("camp_depot_id", 1), ("source_depot", None),
                               ("phase", "x" * 81), ("outcome", "x" * 101), ("unknown", True)):
            with self.subTest(field=field):
                bad = raid(); bad[field] = invalid
                with self.assertRaises(ValidationError): LivingRaid.model_validate(bad)

    def test_source_and_camp_copies_must_match_every_other_supply_snapshot(self):
        first, other = raider(), agent(901034)
        other["supply"] = supply()
        other["supply"]["depot"] = copy.deepcopy(first["raid"]["source_depot"])
        batch = dict(version=4, captured_at_ms=1000, agents=[first, other])
        self.assertEqual(len(TelemetryBatch.model_validate(batch).agents), 2)
        other["supply"]["depot"]["stolen"] = "1"
        with self.assertRaises(ValidationError): TelemetryBatch.model_validate(batch)
        other["supply"]["depot"]["stolen"] = "2"
        first["raid"]["camp_depot"]["stock"] = 1
        with self.assertRaises(ValidationError): TelemetryBatch.model_validate(batch)


if __name__ == "__main__":
    unittest.main()
