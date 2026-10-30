"""All authored ownership is distinct from the retained mixed populations."""
import copy
import unittest

from tools.realm_lab import full_population as full, telemetry_contract
from tools.realm_lab.tests.test_defias_population import frame as defias_frame
from tools.realm_lab.tests.test_telemetry_contract import frame as native_frame


def frame():
    current = defias_frame()
    for actor in current['agents']:
        identity = actor['spawn_id']
        if identity not in full.RECRUIT_IDS:
            continue
        actor.update(native_creature=False, control_mode='AI_WORLD_CONTROLLED',
            type={0: 'CIVILIAN', 1: 'GUARD', 2: 'MERCHANT', 5: 'COMBATANT'}[full.RECRUIT_TYPES[identity]],
            world_faction=full.RECRUIT_FACTIONS[identity], needs={'hunger': .2}, economy={},
            living_role=dict(enabled=True, extensions_enabled=True,
                             role=full.RECRUIT_ROLES[identity], lifetime_ms=1),
            supply=None, raid=None)
    return current


class FullTelemetryTests(unittest.TestCase):
    def test_every_actor_has_explicit_control_and_a_known_live_role(self):
        counts = telemetry_contract.validate_snapshot(frame(), full.PROFILE)
        self.assertEqual(counts, dict(total=100, managed=100, native=0, live_roles=100))
        self.assertEqual(telemetry_contract.validate_snapshot(native_frame(), full.PROFILE, False)['managed'], 0)
        with self.assertRaises(full.ProfileError):
            telemetry_contract.validate_snapshot(defias_frame(), full.PROFILE)

    def test_generic_recruits_do_not_acquire_unassigned_specialized_capabilities(self):
        for identity, field, value in ((901014, 'living_role', dict(enabled=True, extensions_enabled=True, role='PREY', lifetime_ms=1)),
                (901008, 'type', 'CIVILIAN'), (901066, 'world_faction', 1),
                (901097, 'supply', dict(enabled=True)), (901065, 'raid', dict(enabled=True)),
                (901015, 'control_mode', 'OBSERVE_ONLY'), (901041, 'native_creature', True)):
            candidate = frame()
            next(actor for actor in candidate['agents'] if actor['spawn_id'] == identity)[field] = value
            with self.subTest(identity=identity, field=field), self.assertRaises(full.ProfileError):
                telemetry_contract.validate_snapshot(candidate, full.PROFILE)

    def test_existing_specializations_and_runtime_death_remain_observable(self):
        current = frame()
        dead = next(actor for actor in current['agents'] if actor['spawn_id'] == 901070)
        dead.update(alive=False, runtime_guid=None, living_role=None)
        dead['position']['source'] = 'spawn'
        self.assertEqual(telemetry_contract.validate_snapshot(current, full.PROFILE)['managed'], 100)
        for identity, field, value in ((901016, 'type', 'MERCHANT'),
                (901055, 'raid', None), (901035, 'supply', None)):
            candidate = copy.deepcopy(current)
            next(actor for actor in candidate['agents'] if actor['spawn_id'] == identity)[field] = value
            with self.subTest(identity=identity, field=field), self.assertRaises(full.ProfileError):
                telemetry_contract.validate_snapshot(candidate, full.PROFILE)


if __name__ == '__main__':
    unittest.main()
