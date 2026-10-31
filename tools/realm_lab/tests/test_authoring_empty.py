"""Empty-map startup refuses residual data without weakening populated profiles."""
from dataclasses import replace
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import authoring_empty, manage, settlement_population
from tools.realm_lab.tests.test_setup import environment


class EmptyAuthoringTests(unittest.TestCase):
    def setUp(self):
        self.settings = manage.Settings.load({**environment(), 'LAB_AI_PROFILE': 'authoring-empty'})

    def test_empty_profile_cannot_enable_the_model_or_use_another_realm(self):
        for changes in ({'LAB_NPC_AGENTS_ENABLED': '1'}, {'LAB_REALM_ID': '3'}):
            with self.subTest(changes=changes), self.assertRaises(manage.SetupError):
                manage.Settings.load({**environment(), 'LAB_AI_PROFILE': 'authoring-empty', **changes})

    def test_native_render_disables_every_simulation_and_uses_current_map(self):
        flags = manage.config_overrides(self.settings)
        for key in ('AIWorld.Enable', 'AIWorld.NpcAgentEnabled', 'AIWorld.TelemetryEnabled',
                    'AIWorld.TelemetryNativeCreaturesEnabled', 'AIWorld.LivingRolesEnabled',
                    'AIWorld.LivingSupplyEnabled', 'AIWorld.LivingCaravans.Enabled',
                    'AIWorld.GroupCoordination', 'AIWorld.ScopeRestrictAgents', 'AIWorld.ScopeAlwaysActive'):
            self.assertEqual(flags[key], '0', key)
        self.assertEqual(flags['AIWorld.ScopeMapId'], '725')
        self.assertEqual(flags['AIWorld.ScopeZoneIds'], '"4988"')
        self.assertEqual(flags['AIWorld.ScopeSpawnIds'], '""')
        self.assertEqual(float(flags['AIWorld.ScopeMaxY']), settlement_population.BOUNDS['max_y'])

    def test_empty_read_back_is_read_only_and_rejects_every_residual_category(self):
        cases = ('world.creature', 'characters.ai_agents', 'ai_spawn_participation_defaults',
                 'ai_agent_group_members', 'ai_long_term_memories', 'characters.respawn',
                 'world.ai_character_templates')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for residual in (None, *cases):
                calls = []
                def query(settings, sql):
                    calls.append(sql)
                    if 'information_schema.tables' in sql:
                        return '1'
                    return '1' if residual is not None and residual in sql else '0'
                with self.subTest(residual=residual), patch.object(manage, 'lab_mysql', side_effect=query):
                    if residual is None:
                        self.assertFalse(any(authoring_empty.verify_empty(self.settings, root=root).values()))
                    else:
                        with self.assertRaises(manage.SetupError):
                            authoring_empty.verify_empty(self.settings, root=root)
                self.assertTrue(all(sql.startswith('SELECT ') for sql in calls))
                self.assertIn('WHERE map=725', calls[0])
                self.assertNotIn('spawnMask', calls[0])

    def test_authoring_json_blocks_start_even_with_zero_database_rows(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for directory in ('npc_agents', 'npc_appearances', 'npc_templates'):
                path = root / 'data' / directory / '901014.json'
                path.parent.mkdir(parents=True)
                path.write_text('{}', encoding='utf-8')
                with self.subTest(directory=directory), patch.object(manage, 'lab_mysql', return_value='0'):
                    with self.assertRaises(manage.SetupError):
                        authoring_empty.verify_empty(self.settings, root=root)
                path.unlink()

    def test_invalid_read_back_and_wrong_profile_are_refused(self):
        with patch.object(manage, 'lab_mysql', return_value='bad'):
            with self.assertRaises(manage.SetupError):
                authoring_empty.verify_empty(self.settings)
        with self.assertRaises(manage.SetupError):
            authoring_empty.verify_empty(replace(self.settings, ai_profile='full-v8'))

    def test_geometry_must_be_verified_before_accepting_zero_actor_state(self):
        population = object()
        data = Path('data-under-test')
        with patch.object(settlement_population, 'verify_data', return_value=population) as geometry, \
             patch.object(authoring_empty, 'verify_empty') as empty:
            self.assertIs(manage.authoring_preflight(data, self.settings), population)
            geometry.assert_called_once_with(data)
            empty.assert_called_once_with(self.settings)
        with patch.object(settlement_population, 'verify_data', side_effect=manage.SetupError('bad geometry')), \
             patch.object(authoring_empty, 'verify_empty') as empty:
            with self.assertRaises(manage.SetupError):
                manage.authoring_preflight(data, self.settings)
            empty.assert_not_called()


if __name__ == '__main__':
    unittest.main()
