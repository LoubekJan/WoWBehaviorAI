"""Finite supply telemetry preserves quantities and never invents legacy stock."""
from __future__ import annotations

import copy
import unittest

from pydantic import ValidationError

from app.telemetry import Agent, FoodAccounting, LivingSupply, TelemetryBatch
from test_world_viewer import agent as legacy_agent


def agent(identity=901034):
    value = legacy_agent(identity)
    value["position"]["map_id"] = 725
    return value


def supply():
    return dict(enabled=True, role="HUNTER", phase="TO_DEPOT", depot_id=1, cargo=6, capacity=6,
                carcass_remaining=None, hunting_ground=dict(map_id=725, x=10, y=20, z=0),
                harvested="6", delivered="0", consumed="0", lost="0", last_result="HARVESTED",
                depot=dict(id=1, name="Jižní vesnice", position=dict(map_id=725, x=1, y=2, z=0),
                           stock=4, capacity=24, low_threshold=6, delivered="0", consumed="0"),
                accounting=dict(initial_food="4", harvested="6", in_depots="4", in_cargo="6",
                                consumed="0", lost="0", conserved=True))


class SupplyTelemetryTests(unittest.TestCase):
    def test_live_hunter_and_predator_supply_are_optional(self):
        legacy = Agent.model_validate(agent())
        self.assertIsNone(legacy.supply)
        payload = agent()
        payload["supply"] = supply()
        parsed = Agent.model_validate(payload)
        self.assertEqual(parsed.supply.cargo, 6)
        self.assertEqual(parsed.supply.depot.name, "Jižní vesnice")
        self.assertEqual(parsed.supply.accounting.harvested, "6")
        predator = supply()
        predator.update(role="PREDATOR", phase="FEEDING", depot_id=0, depot=None,
                         hunting_ground=None, cargo=0, capacity=0, carcass_remaining=3)
        self.assertEqual(LivingSupply.model_validate(predator).carcass_remaining, 3)

    def test_decimal_counters_preserve_uint64_and_reject_invalid_representations(self):
        value = supply()
        value["harvested"] = str(2**64 - 1)
        value["depot"]["delivered"] = str(2**64 - 1)
        parsed = LivingSupply.model_validate(value)
        self.assertEqual(parsed.harvested, str(2**64 - 1))
        for invalid in (-1, 1, True, "-1", "01", "1.0", str(2**64)):
            with self.subTest(invalid=invalid):
                broken = copy.deepcopy(value)
                broken["harvested"] = invalid
                with self.assertRaises(ValidationError):
                    LivingSupply.model_validate(broken)

    def test_impossible_cargo_depot_and_source_quantities_are_rejected(self):
        cases = (("cargo", 7), ("cargo", True), ("capacity", -1),
                 ("carcass_remaining", -1), ("carcass_remaining", 2**32),
                 ("depot_id", 2), ("role", "WORKER"))
        for field, invalid in cases:
            with self.subTest(field=field, invalid=invalid):
                broken = supply()
                broken[field] = invalid
                with self.assertRaises(ValidationError):
                    LivingSupply.model_validate(broken)
        for field in ("stock", "low_threshold"):
            broken = supply()
            broken["depot"][field] = 25
            with self.assertRaises(ValidationError):
                LivingSupply.model_validate(broken)
        broken = supply()
        broken["hunting_ground"]["map_id"] = 0
        with self.assertRaises(ValidationError):
            LivingSupply.model_validate(broken)

    def test_carcass_source_uses_a_paired_positive_ledger_incarnation(self):
        value = supply()
        value.update(carcass_spawn_id="901088", carcass_life="2", carcass_remaining=3)
        self.assertEqual(LivingSupply.model_validate(value).carcass_life, "2")
        for field, invalid in (("carcass_life", None), ("carcass_life", "0"), ("carcass_spawn_id", "0"),
                               ("carcass_life", 2), ("carcass_spawn_id", str(2**64))):
            with self.subTest(field=field, invalid=invalid):
                broken = copy.deepcopy(value)
                broken[field] = invalid
                with self.assertRaises(ValidationError):
                    LivingSupply.model_validate(broken)

    def test_conservation_uses_exact_integers_and_exposes_a_real_failure(self):
        value = supply()["accounting"]
        value.update(initial_food=str(2**64 - 2), harvested="1", in_depots=str(2**64 - 1), in_cargo="0")
        self.assertTrue(FoodAccounting.model_validate(value).conserved)
        value["in_cargo"] = "1"
        with self.assertRaises(ValidationError):
            FoodAccounting.model_validate(value)
        value["conserved"] = False
        self.assertFalse(FoodAccounting.model_validate(value).conserved)

    def test_native_and_unloaded_actors_cannot_supply_live_food_evidence(self):
        payload = agent()
        payload["supply"] = supply()
        payload["position"]["source"] = "spawn"
        with self.assertRaises(ValidationError):
            Agent.model_validate(payload)
        payload = agent()
        payload.update(control_mode="OBSERVE_ONLY", supply=supply())
        with self.assertRaises(ValidationError):
            Agent.model_validate(payload)
        payload = agent()
        payload["supply"] = supply()
        payload["position"]["map_id"] = 0
        with self.assertRaises(ValidationError):
            Agent.model_validate(payload)
        payload = agent()
        payload.update(native_creature=True, control_mode="OBSERVE_ONLY", needs=None, economy=None,
                       living_role=None, supply=supply())
        with self.assertRaises(ValidationError):
            Agent.model_validate(payload)

    def test_batch_rejects_conflicting_depot_and_global_accounting_copies(self):
        first, second = agent(901061), agent(901062)
        first["supply"], second["supply"] = supply(), supply()
        batch = dict(version=4, captured_at_ms=1000, agents=[first, second])
        self.assertEqual(len(TelemetryBatch.model_validate(batch).agents), 2)
        second["supply"]["depot"]["stock"] = 5
        with self.assertRaises(ValidationError):
            TelemetryBatch.model_validate(batch)
        second["supply"]["depot"]["stock"] = 4
        second["supply"]["accounting"].update(initial_food="5", in_depots="5")
        with self.assertRaises(ValidationError):
            TelemetryBatch.model_validate(batch)


if __name__ == "__main__":
    unittest.main()
