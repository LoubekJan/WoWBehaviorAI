"""All authored ownership is distinct from the retained mixed populations."""
import copy
import unittest

from tools.realm_lab import full_population as full, telemetry_contract
from tools.realm_lab.tests.test_defias_population import frame as defias_frame
from tools.realm_lab.tests.test_telemetry_contract import frame as native_frame


def refresh_caravan_ledger_copies(current):
    """Synthetic fixture helper: use this frame's actual shared depot copies."""
    depots = {}
    accounting = next(actor['supply']['accounting'] for actor in current['agents']
        if actor['spawn_id'] not in full.SUPPLY_RECRUIT_IDS and isinstance(actor.get('supply'), dict))
    for actor in current['agents']:
        supplied, raid = actor.get('supply'), actor.get('raid')
        copies = ([supplied['depot']] if isinstance(supplied, dict) and isinstance(supplied.get('depot'), dict) else [])
        if isinstance(raid, dict): copies.extend((raid['source_depot'], raid['camp_depot']))
        for depot in copies:
            depot.setdefault('low_threshold', 12 if depot['id'] == 7 else 4)
            for key in ('delivered', 'consumed', 'stolen', 'exported'): depot.setdefault(key, '0')
            if actor['spawn_id'] not in full.SUPPLY_RECRUIT_IDS:
                depots[depot['id']] = depot
    for actor in current['agents']:
        if actor['spawn_id'] not in full.SUPPLY_RECRUIT_IDS: continue
        supplied = actor['supply']
        supplied['accounting'] = copy.deepcopy(accounting)
        caravan = supplied.get('caravan')
        if caravan and caravan['source_depot_id']:
            caravan['source_depot'] = copy.deepcopy(depots[caravan['source_depot_id']])


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
        if identity in full.SUPPLY_RECRUIT_IDS:
            city = dict(id=7, name='Stormwind city', position=copy.deepcopy(full.CITY_DEPOT['position']),
                stock=0, capacity=120, low_threshold=12, delivered='0', consumed='0', stolen='0', exported='0')
            actor['supply'] = dict(enabled=True, role=full.RECRUIT_ROLES[identity], depot_id=7,
                cargo=0, capacity=6 if identity in full.CARRIER_IDS else 0, depot=city,
                phase='WAITING', harvested='0', delivered='0', consumed='0', lost='0', stolen='0')
            if identity in full.CARAVAN_IDS:
                pair = next(pair for pair in full.CARAVAN_PAIRS
                    if identity in (pair['carrier_spawn_id'], pair['escort_spawn_id']))
                actor['supply']['caravan'] = dict(id=pair['id'], leader_spawn_id=str(pair['carrier_spawn_id']),
                    member_spawn_ids=list(map(str, (pair['carrier_spawn_id'], pair['escort_spawn_id']))),
                    source_depot_id=pair['source_depot_ids'][0], destination_depot_id=7, stage='RALLY', trip='1',
                    waypoint=0, member_ready=False, loaded='0', completed_trips='0', source_depot=None,
                    destination_depot=copy.deepcopy(city))
    refresh_caravan_ledger_copies(current)
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
