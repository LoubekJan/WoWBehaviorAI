"""Physical caravans expose paired routes and exact food without theft capabilities."""
import copy
import unittest

from pydantic import ValidationError

from app.telemetry import Agent, LivingSupply, TelemetryBatch
from test_supply_telemetry import agent, supply


def warehouse(identity, *, stock=12):
    return dict(id=identity, name="City warehouse" if identity == 7 else f"Village {identity}",
                position=dict(map_id=725, x=identity * 10, y=600, z=.15), stock=stock,
                capacity=120 if identity == 7 else 30, low_threshold=12 if identity == 7 else 4,
                delivered="20", consumed="8", stolen="0", exported="0")


def caravan(identity=1, *, escort=False, source=None):
    leader, guard, default_source = (901040, 901042, 4) if identity == 1 else (901041, 901043, 6)
    value = agent(guard if escort else leader)
    value["supply"] = supply()
    value["supply"].update(role="CARAVAN_ESCORT" if escort else "CARAVAN_CARRIER",
                           phase="TO_CITY", depot_id=7, depot=warehouse(7),
                           cargo=0 if escort else 4, capacity=0 if escort else 6,
                           harvested="0", hunting_ground=None,
                           accounting=dict(initial_food="24", harvested="0", in_depots="20",
                                           in_cargo="4", consumed="0", lost="0", conserved=True),
                           caravan=dict(id=identity, leader_spawn_id=str(leader),
                                        member_spawn_ids=[str(leader), str(guard)],
                                        source_depot_id=default_source if source is None else source,
                                        destination_depot_id=7, stage="TO_CITY", trip="18446744073709551615",
                                        waypoint=3, member_ready=escort, loaded="0" if escort else "18446744073709551615",
                                        completed_trips="12", source_depot=warehouse(default_source if source is None else source),
                                        destination_depot=warehouse(7)))
    return value


class CaravanTelemetryTests(unittest.TestCase):
    def test_fixed_pairs_retain_individual_cargo_and_exact_counters(self):
        for identity, sources in ((1, (4, 5)), (2, (6, 1))):
            for source in sources:
                with self.subTest(identity=identity, source=source):
                    payload = dict(version=4, captured_at_ms=1000,
                                   agents=[caravan(identity, source=source), caravan(identity, source=source, escort=True)])
                    parsed = TelemetryBatch.model_validate(payload)
                    self.assertEqual([a.supply.cargo for a in parsed.agents], [4, 0])
                    self.assertEqual(parsed.agents[0].supply.caravan.loaded, str(2**64-1))
                    self.assertNotEqual(parsed.agents[0].supply.caravan.member_ready, parsed.agents[1].supply.caravan.member_ready)
                    self.assertEqual(TelemetryBatch.model_validate_json(parsed.model_dump_json()), parsed)

    def test_rejects_forged_roster_leader_source_destination_and_capabilities(self):
        mutations = (
            lambda a: a["supply"]["caravan"].update(id=3),
            lambda a: a["supply"]["caravan"].update(leader_spawn_id="901041"),
            lambda a: a["supply"]["caravan"].update(member_spawn_ids=["901040", "901040"]),
            lambda a: a["supply"]["caravan"].update(member_spawn_ids=["901042", "901040"]),
            lambda a: a["supply"]["caravan"].update(source_depot_id=6),
            lambda a: a["supply"]["caravan"].update(destination_depot_id=3),
            lambda a: a["supply"]["caravan"]["source_depot"].update(id=5),
            lambda a: a["supply"]["caravan"]["destination_depot"]["position"].update(map_id=0),
            lambda a: a["supply"].update(capacity=7),
            lambda a: a["supply"].update(cargo=7),
            lambda a: a["supply"].update(harvested="1"),
            lambda a: a["supply"].update(stolen="1"),
            lambda a: a["supply"].update(role="CARAVAN_ESCORT"),
            lambda a: a["supply"].update(caravan=None),
            lambda a: a["supply"].update(depot=None),
            lambda a: a.update(spawn_id=901014),
            lambda a: a["position"].update(map_id=0),
        )
        for index, mutate in enumerate(mutations):
            value = caravan(); mutate(value)
            with self.subTest(mutation=index), self.assertRaises(ValidationError):
                Agent.model_validate(value)

    def test_escort_cannot_claim_carried_food_or_loading_receipts(self):
        for field, invalid in (("cargo", 1), ("capacity", 6), ("delivered", "1"), ("lost", "1")):
            value = caravan(escort=True); value["supply"][field] = invalid
            with self.subTest(field=field), self.assertRaises(ValidationError):
                Agent.model_validate(value)
        value = caravan(escort=True); value["supply"]["caravan"]["loaded"] = "1"
        with self.assertRaises(ValidationError): Agent.model_validate(value)

    def test_city_consumers_have_only_city_food_capability(self):
        value = caravan()
        value["spawn_id"] = value["agent_id"] = 901014
        value["supply"].update(role="CITY_CONSUMER", caravan=None, cargo=0, capacity=0)
        parsed = Agent.model_validate(value)
        self.assertEqual(parsed.supply.depot.id, 7)
        value["supply"].update(depot_id=4, depot=warehouse(4))
        with self.assertRaises(ValidationError): Agent.model_validate(value)

    def test_legacy_optional_supply_and_depots_stay_compatible(self):
        self.assertIsNone(Agent.model_validate(agent()).supply)
        self.assertIsNone(LivingSupply.model_validate(supply()).caravan)
        self.assertEqual(LivingSupply.model_validate(supply()).depot.exported, "0")
        value = caravan()
        value["supply"]["caravan"].update(source_depot=None, destination_depot=None)
        self.assertEqual(Agent.model_validate(value).supply.caravan.source_depot_id, 4)

    def test_decimal_counters_reject_rounding_overflow_and_booleans(self):
        for field in ("loaded", "trip", "completed_trips", "leader_spawn_id"):
            for invalid in (1, True, "01", "-1", str(2**64)):
                value = caravan(); value["supply"]["caravan"][field] = invalid
                with self.subTest(field=field, invalid=invalid), self.assertRaises(ValidationError):
                    Agent.model_validate(value)
        value = caravan(); value["supply"]["caravan"]["member_ready"] = 1
        with self.assertRaises(ValidationError): Agent.model_validate(value)

    def test_shared_route_fields_cannot_diverge_within_a_snapshot(self):
        for field, changed in (("stage", "LOADING"), ("trip", "1"), ("waypoint", 4),
                               ("completed_trips", "13")):
            carrier, escort = caravan(), caravan(escort=True)
            escort["supply"]["caravan"][field] = changed
            with self.subTest(field=field), self.assertRaises(ValidationError):
                TelemetryBatch.model_validate(dict(version=4, captured_at_ms=1000, agents=[carrier, escort]))

    def test_all_seven_warehouses_and_nested_copies_use_one_snapshot(self):
        carrier = caravan()
        others = []
        for identity in range(1, 8):
            resident = agent(901000+identity)
            resident["supply"] = supply()
            depot = copy.deepcopy(carrier["supply"]["depot"]) if identity == 7 else warehouse(identity)
            resident["supply"].update(role="CONSUMER", depot_id=identity, depot=depot,
                                      hunting_ground=None, cargo=0, capacity=0, harvested="0",
                                      accounting=None)
            others.append(resident)
        payload = dict(version=4, captured_at_ms=1000, agents=[carrier, *others])
        self.assertEqual(len(TelemetryBatch.model_validate(payload).agents), 8)
        others[3]["supply"]["depot"]["stock"] -= 1
        with self.assertRaises(ValidationError): TelemetryBatch.model_validate(payload)
        others[3]["supply"]["depot"]["stock"] += 1
        others[6]["supply"]["depot"]["exported"] = "1"
        with self.assertRaises(ValidationError): TelemetryBatch.model_validate(payload)


if __name__ == "__main__":
    unittest.main()
