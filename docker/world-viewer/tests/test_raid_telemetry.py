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


class RaidTelemetryTests(unittest.TestCase):
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
