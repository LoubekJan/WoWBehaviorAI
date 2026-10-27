"""Farmers expose bounded crop potential and exactly their own warehouse."""
import copy
import unittest

from pydantic import ValidationError

from app.telemetry import Agent, LivingSupply, TelemetryBatch
from test_supply_telemetry import agent, supply


def farmer():
    current = agent(901016)
    supplied = supply()
    supplied.update(role="FARMER", phase="FIELD_WORKING", cargo=0, capacity=2, depot_id=4,
        hunting_ground=None, field_id=4, field_workpoint_index=0,
        field_workpoint=dict(map_id=725, x=383.5937353670597, y=566.9270617067814, z=0),
        field_remaining=24, field_capacity=24)
    supplied["depot"].update(id=4, name="villageTop warehouse", stock=0, capacity=30, low_threshold=4)
    current["supply"] = supplied
    return current


class FieldTelemetryTests(unittest.TestCase):
    def test_farmer_is_optional_and_bounded_state_roundtrips(self):
        self.assertIsNone(Agent.model_validate(agent()).supply)
        parsed = Agent.model_validate(farmer())
        self.assertEqual((parsed.supply.role, parsed.supply.field_id, parsed.supply.capacity), ("FARMER", 4, 2))
        self.assertEqual(Agent.model_validate_json(parsed.model_dump_json()), parsed)

    def test_farmer_rejects_unbounded_crops_foreign_warehouse_and_hunting(self):
        for key, value in (("field_id", 1), ("depot_id", 5), ("capacity", 3), ("cargo", 3),
            ("field_workpoint_index", 12), ("field_workpoint_index", True), ("field_remaining", 25),
            ("field_capacity", 23), ("field_workpoint", None), ("carcass_remaining", 0),
            ("hunting_ground", dict(map_id=725, x=1, y=2, z=0)), ("stolen", "1")):
            candidate = farmer()["supply"]; candidate[key] = value
            with self.subTest(key=key), self.assertRaises(ValidationError): LivingSupply.model_validate(candidate)
        candidate = farmer()["supply"]; candidate["field_workpoint"]["map_id"] = 0
        with self.assertRaises(ValidationError): LivingSupply.model_validate(candidate)

    def test_legacy_roles_cannot_acquire_fields_and_shared_ledger_copies_remain_consistent(self):
        candidate = farmer()["supply"]; candidate.update(role="HUNTER")
        with self.assertRaises(ValidationError): LivingSupply.model_validate(candidate)
        first, second = farmer(), farmer()
        second.update(agent_id=901017, spawn_id=901017)
        batch = dict(version=4, captured_at_ms=1000, agents=[first, second])
        self.assertEqual(len(TelemetryBatch.model_validate(batch).agents), 2)
        second["supply"]["field_remaining"] = 23
        with self.assertRaises(ValidationError): TelemetryBatch.model_validate(batch)
        second["supply"]["field_remaining"] = 24
        second["supply"]["depot"]["stock"] = 1
        with self.assertRaises(ValidationError): TelemetryBatch.model_validate(batch)
        second["supply"]["depot"]["stock"] = 0
        second["supply"]["accounting"].update(initial_food="5", in_depots="5")
        with self.assertRaises(ValidationError): TelemetryBatch.model_validate(batch)


if __name__ == "__main__": unittest.main()
