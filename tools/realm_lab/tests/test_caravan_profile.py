"""Only the reviewed full profile enables two physical food caravans."""
import copy
from dataclasses import replace
import unittest

from tools.realm_lab import full_population as full, manage, single_return, telemetry_contract
from tools.realm_lab.tests.test_full_telemetry import frame
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_wildlife_population import write_json
from tools.realm_lab.tests import test_full_population as full_tests


def actor(state, identity):
    return next(actor for actor in state['agents'] if actor['spawn_id'] == identity)


class CaravanProfileTests(unittest.TestCase):
    def test_caravans_are_only_enabled_by_the_full_profile_and_pause_with_the_world(self):
        for profile in ('disabled', 'settlement-v8', 'wildlife-v8', 'supply-v8', 'consumers-v8',
                        'raids-v8', 'fields-v8', 'defias-v8', 'full-v8'):
            settings = manage.Settings.load({**environment(), 'LAB_AI_PROFILE': profile})
            with self.subTest(profile=profile):
                self.assertEqual(manage.config_overrides(settings)['AIWorld.LivingCaravans.Enabled'],
                                 '1' if profile == full.PROFILE else '0')
                self.assertEqual(manage.config_overrides(replace(settings, wildlife_enabled=False))[
                    'AIWorld.LivingCaravans.Enabled'], '0')

    def test_manifest_declares_only_six_new_capabilities_and_preserves_native_identities(self):
        population = full.Population.load()
        manifest = single_return.load_json(full.METADATA / full.MANIFEST)
        self.assertTrue(manifest['runtime_caravans_only'])
        self.assertFalse(manifest['ordinary_roles_only'])
        special = {actor['spawn_id']: actor for actor in manifest['actors']
                   if actor['capability'] != 'ordinary_living_role'}
        self.assertEqual(set(special), {901014, 901015, 901040, 901041, 901042, 901043})
        self.assertEqual({special[identity]['living_role'] for identity in full.CARRIER_IDS}, {'CARAVAN_CARRIER'})
        self.assertEqual({special[identity]['living_role'] for identity in full.ESCORT_IDS}, {'CARAVAN_ESCORT'})
        self.assertEqual({special[identity]['living_role'] for identity in full.CITY_CONSUMER_IDS}, {'CITY_CONSUMER'})
        caravan = manifest['caravans']
        self.assertEqual(caravan['pairs'], list(full.CARAVAN_PAIRS))
        self.assertEqual(caravan['source_depot_ids'], [1, 4, 5, 6])
        self.assertEqual(caravan['city_depot'], full.CITY_DEPOT)
        self.assertEqual((caravan['carrier_capacity'], caravan['source_reserve']), (6, 4))
        self.assertEqual((caravan['load_work_ms'], caravan['unload_work_ms'], caravan['rest_ms']), (5000, 5000, 45000))
        native = {actor.spawn_id: actor for actor in population.settlement.actors}
        for identity, binding in special.items():
            original = native[identity]
            self.assertEqual(binding['entry'], original.entry)
            self.assertEqual(binding['role'], original.role)
            self.assertEqual(binding['agent_type'], full.RECRUIT_TYPES[identity])
            self.assertEqual(binding['world_faction_id'], 1)
            self.assertEqual(binding['home'], dict(x=original.x, y=original.y, z=original.z, o=original.orientation))

    def test_caravan_manifest_cannot_change_pairs_sources_city_reserve_or_restart_policy(self):
        fixture = full_tests.FullPopulationTests('test_every_remaining_binding_is_checked_before_parent_validation')
        self.addCleanup(fixture.doCleanups)
        fixture.setUp()
        manifest = single_return.load_json(fixture.metadata / full.MANIFEST)
        for mutate in (
            lambda document: document.update(runtime_caravans_only=False),
            lambda document: document.update(ordinary_roles_only=True),
            lambda document: document['caravans']['pairs'][0].update(escort_spawn_id=901000),
            lambda document: document['caravans']['pairs'][0].update(source_depot_ids=[2, 3]),
            lambda document: document['caravans']['city_depot']['position'].update(x=0),
            lambda document: document['caravans']['city_depot'].update(capacity=121),
            lambda document: document['caravans'].update(carrier_capacity=7),
            lambda document: document['caravans'].update(source_reserve=0),
            lambda document: document['caravans'].update(load_work_ms=True),
            lambda document: document['caravans'].update(city_consumer_spawn_ids=[901014, 901008]),
        ):
            candidate = copy.deepcopy(manifest); mutate(candidate)
            write_json(fixture.metadata / full.MANIFEST, candidate)
            with self.subTest(mutate=mutate), self.assertRaises(full.ProfileError):
                full.Population.load(fixture.metadata)


class CaravanTelemetryContractTests(unittest.TestCase):
    def test_pairs_cover_each_assigned_village_and_an_unstarted_pair_has_no_remote_stock(self):
        current = frame()
        self.assertEqual(telemetry_contract.validate_snapshot(current, full.PROFILE)['managed'], 100)
        for pair in full.CARAVAN_PAIRS:
            for identity in (pair['carrier_spawn_id'], pair['escort_spawn_id']):
                caravan = actor(current, identity)['supply']['caravan']
                caravan.update(source_depot_id=0, trip='0', source_depot=None, stage='RALLY')
        self.assertEqual(telemetry_contract.validate_snapshot(current, full.PROFILE)['managed'], 100)

    def test_foreign_capabilities_pairs_sources_and_forged_progress_are_rejected(self):
        changes = (
            (901040, lambda row: row.update(supply=None)),
            (901040, lambda row: row['supply'].update(capacity=7)),
            (901040, lambda row: row['supply'].update(cargo=-1)),
            (901040, lambda row: row['supply'].update(field_id=4)),
            (901040, lambda row: row['supply'].update(hunting_ground=dict(map_id=725, x=0, y=0, z=0))),
            (901040, lambda row: row['supply']['caravan'].update(id=True)),
            (901040, lambda row: row['supply']['caravan'].update(leader_spawn_id='901041')),
            (901040, lambda row: row['supply']['caravan'].update(member_spawn_ids=['901040', '901043'])),
            (901040, lambda row: row['supply']['caravan'].update(source_depot_id=6)),
            (901040, lambda row: row['supply']['caravan'].update(destination_depot_id=3)),
            (901040, lambda row: row['supply']['caravan'].update(stage='TELEPORT')),
            (901040, lambda row: row['supply']['caravan'].update(trip='01')),
            (901040, lambda row: row['supply']['caravan'].update(waypoint=True)),
            (901040, lambda row: row['supply']['caravan'].update(waypoint=20)),
            (901040, lambda row: row['supply']['caravan'].update(member_ready=1)),
            (901040, lambda row: row['supply']['caravan'].update(loaded=str(2**64))),
            (901040, lambda row: row['supply']['caravan'].update(completed_trips='2')),
            (901040, lambda row: row['supply']['caravan'].update(source_depot_id=0)),
            (901042, lambda row: row['supply'].update(cargo=1)),
            (901042, lambda row: row['supply']['caravan'].update(loaded='1')),
            (901014, lambda row: row['supply'].update(capacity=6)),
            (901014, lambda row: row['supply'].update(caravan={})),
            (901014, lambda row: row['supply']['depot'].update(capacity=30)),
            (901040, lambda row: row['supply']['depot']['position'].update(x=float('nan'))),
            (901040, lambda row: row['supply']['depot'].update(exported='-1')),
            (901040, lambda row: row['supply']['caravan']['source_depot'].update(stock=1)),
            (901040, lambda row: row['supply']['caravan']['destination_depot'].update(stock=1)),
        )
        for identity, mutate in changes:
            current = frame(); mutate(actor(current, identity))
            with self.subTest(identity=identity, mutate=mutate), self.assertRaises(full.ProfileError):
                telemetry_contract.validate_snapshot(current, full.PROFILE)

    def test_old_profiles_cannot_acquire_caravan_coordination(self):
        from tools.realm_lab.tests.test_defias_population import frame as legacy_frame
        current = legacy_frame()
        actor(current, 901055)['supply']['caravan'] = copy.deepcopy(actor(frame(), 901040)['supply']['caravan'])
        with self.assertRaises(full.ProfileError):
            telemetry_contract.validate_snapshot(current, 'defias-v8')

    def test_deploy_contract_rejects_pre_caravan_roles_without_explicit_historic_mode(self):
        current = frame()
        for identity in full.SUPPLY_RECRUIT_IDS:
            row = actor(current, identity)
            row['supply'] = None
            row['living_role']['role'] = {0: 'WORKER', 1: 'GUARD', 2: 'SERVICE'}[full.RECRUIT_TYPES[identity]]
        with self.assertRaises(full.ProfileError):
            telemetry_contract.validate_snapshot(current, full.PROFILE)
        self.assertEqual(telemetry_contract.validate_snapshot(current, full.PROFILE,
            legacy_ordinary_full=True)['managed'], 100)
        with self.assertRaises(full.ProfileError):
            telemetry_contract.validate_snapshot(current, 'defias-v8', legacy_ordinary_full=True)


if __name__ == '__main__': unittest.main()
