"""Mixed population gates must not certify an all-native or spoofed AI batch."""
import copy
import unittest

from tools.realm_lab import settlement_population as settlement, telemetry_contract, wildlife_population as wildlife


def frame(controlled=False):
    actors = []
    for identity in sorted(settlement.SPAWN_IDS):
        active = controlled and identity in wildlife.SPAWN_IDS
        actors.append({"spawn_id": identity, "agent_id": identity,
                       "position": {"map_id": 725, "source": "live"}, "alive": True,
                       "native_creature": not active,
                       "control_mode": "AI_WORLD_CONTROLLED" if active else "OBSERVE_ONLY",
                       "needs": {"hunger": 0.7} if active else None, "economy": {} if active else None,
                       "runtime_guid": f"Creature-{identity}",
                       "living_role": {"enabled": True, "extensions_enabled": True,
                                       "role": "PREDATOR" if identity in wildlife.PREDATOR_IDS else "PREY",
                                       "lifetime_ms": 1} if active else None})
    return {"version": 4, "captured_at_ms": 10, "stale": False, "agents": actors}


class TelemetryContractTests(unittest.TestCase):
    def test_active_wildlife_and_paused_native_population_are_distinct(self):
        result = telemetry_contract.validate_snapshot(frame(True), wildlife.PROFILE)
        self.assertEqual((result["managed"], result["native"], result["live_roles"]), (20, 80, 20))
        self.assertEqual(telemetry_contract.validate_snapshot(frame(), wildlife.PROFILE, False)["managed"], 0)
        self.assertEqual(telemetry_contract.validate_snapshot(frame(), settlement.PROFILE)["native"], 100)
        with self.assertRaises(settlement.ProfileError):
            telemetry_contract.validate_snapshot(frame(), wildlife.PROFILE)

    def test_stale_duplicate_foreign_or_incomplete_population_is_rejected(self):
        original = frame(True)
        for mutate in (lambda f: f.update(stale=True), lambda f: f["agents"].pop(),
                       lambda f: f["agents"][0].update(spawn_id=901072),
                       lambda f: f["agents"][0]["position"].update(map_id=0)):
            candidate = copy.deepcopy(original)
            mutate(candidate)
            with self.subTest(mutate=mutate), self.assertRaises(settlement.ProfileError):
                telemetry_contract.validate_snapshot(candidate, wildlife.PROFILE)

    def test_native_ai_fields_invalid_needs_or_missing_incarnation_never_pass(self):
        for field, value in (("runtime_guid", None), ("needs", None), ("living_role", None),
                             ("needs", {"hunger": float("nan")}), ("needs", {"hunger": True})):
            candidate = frame(True)
            next(a for a in candidate["agents"] if a["spawn_id"] == 901076)[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(settlement.ProfileError):
                telemetry_contract.validate_snapshot(candidate, wildlife.PROFILE)
        candidate = frame(True)
        candidate["agents"][0]["needs"] = {"hunger": 0}
        with self.assertRaises(settlement.ProfileError):
            telemetry_contract.validate_snapshot(candidate, wildlife.PROFILE)

    def test_dead_or_respawning_wildlife_remains_in_the_population(self):
        candidate = frame(True)
        animal = next(a for a in candidate["agents"] if a["spawn_id"] == 901072)
        animal.update(alive=False, runtime_guid=None, living_role=None)
        animal["position"]["source"] = "spawn"
        self.assertEqual(telemetry_contract.validate_snapshot(candidate, wildlife.PROFILE)["managed"], 20)


if __name__ == "__main__":
    unittest.main()
