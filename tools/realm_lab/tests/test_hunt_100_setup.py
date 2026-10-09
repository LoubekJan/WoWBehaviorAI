"""Integration boundaries of the reviewed 100-actor startup profile."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import hunt_population as hunt, manage, single_return
from tools.realm_lab.tests.test_hunt_population import snapshot
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_single_return import fixture

ROOT = Path(__file__).resolve().parents[3]


class HuntHundredSetupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data = fixture(Path(self.tmp.name))
        for name in ('hunt-population.json', 'hunt-population-100.json'):
            (self.metadata / name).write_bytes((single_return.METADATA / name).read_bytes())
        self.population = hunt.Population.load(self.metadata, profile='hunt-100')

    def test_profile_renders_exact_hundred_and_keeps_models_groups_hooks_off(self):
        settings = manage.Settings.load({**environment(), 'LAB_AI_PROFILE': 'hunt-100'})
        config = manage.config_overrides(settings, self.population)
        ids = config['AIWorld.ScopeSpawnIds'].strip('"').split(',')
        self.assertEqual([int(x) for x in ids], list(range(900725, 900825)))
        for key in ('Enable', 'LivingRolesEnabled', 'TelemetryEnabled', 'LivingNeedEvolutionEnabled',
                    'ScopeRestrictAgents', 'ScopeAlwaysActive'):
            self.assertEqual(config[f'AIWorld.{key}'], '1')
        for key in ('RemoteInferenceEnabled', 'LivingRoleExtensionsEnabled', 'GroupCoarseSimulationEnabled',
                    'RecoveryAdviceEnabled', 'GroupCoordination', 'CoalitionMaintenance', 'TestSpawnId'):
            self.assertEqual(config[f'AIWorld.{key}'], '0')
        self.assertEqual(config['AIWorld.NeedsHungerRatePerSecond'], '0.003')
        self.assertEqual(config['Updates.EnableDatabases'], '6')
        text = manage.render_config((ROOT / 'deploy/worldserver.conf').read_text(encoding='utf-8'), settings, self.population)
        self.assertIn('AIWorld.ScopeSpawnIds = "' + ','.join(ids) + '"\n', text)
        self.assertIn('AIWorld.RemoteInferenceEnabled = 0\n', text)
        small_settings = manage.Settings.load({**environment(), 'LAB_AI_PROFILE': 'hunt-cycle'})
        with self.assertRaisesRegex(manage.SetupError, 'selected lab profile'):
            manage.config_overrides(small_settings, self.population)
        with self.assertRaises(manage.SetupError):
            manage.Settings.load({**environment(), 'LAB_AI_PROFILE': 'hunt-100', 'LAB_REALM_ID': '3'})

    def test_preflight_failure_prevents_every_sql_and_server_exec(self):
        env = {**environment(), 'LAB_AI_PROFILE': 'hunt-100', 'WORLD_VIEWER_TELEMETRY_TOKEN': 'test-token'}
        with patch.dict(manage.os.environ, env), \
             patch.object(manage.sys, 'argv', ['manage.py', 'run-world', 'unused.conf', 'worldserver']), \
             patch.object(hunt, 'verify_data', side_effect=hunt.ProfileError('Hash mismatch')) as verify, \
             patch.object(manage, 'lab_mysql') as private_sql, \
             patch.object(manage, 'mysql') as auth_sql, \
             patch.object(manage.os, 'execv') as execute:
            with self.assertRaises(SystemExit) as raised:
                manage.main()
            self.assertEqual(raised.exception.code, 2)
            self.assertEqual(verify.call_args.kwargs['profile'], 'hunt-100')
            private_sql.assert_not_called()
            auth_sql.assert_not_called()
            execute.assert_not_called()

    def test_disabled_migration_uses_hundred_profile_and_private_sql_only(self):
        settings = manage.Settings.load(environment())
        small = hunt.Population.load(self.metadata)
        for function, before, after in (
            (manage.bootstrap_hunt, snapshot(small, 1), snapshot(self.population, 0)),
            (manage.activate_hunt, snapshot(self.population, 0), snapshot(self.population, 1)),
        ):
            outputs = ['\n'.join(json.dumps(row) for row in before), '',
                       '\n'.join(json.dumps(row) for row in after)]
            results = [type('Result', (), {'returncode': 0, 'stdout': text})() for text in outputs]
            with self.subTest(command=function.__name__), \
                 patch.object(manage.subprocess, 'run', side_effect=results) as run, \
                 patch.object(manage, 'mysql') as auth_sql:
                function(self.data, settings, self.metadata, profile='hunt-100')
                self.assertEqual(run.call_count, 3)
                auth_sql.assert_not_called()
                for call in run.call_args_list:
                    self.assertIn('--host=lab-mysql', call.args[0])
                    self.assertNotIn('auth.', call.kwargs['input'])
                    self.assertNotIn(settings.auth_password, call.kwargs['input'])

    def test_hundred_enabled_profile_refuses_offline_population_mutation(self):
        settings = manage.Settings.load({**environment(), 'LAB_AI_PROFILE': 'hunt-100'})
        for function in (manage.bootstrap_hunt, manage.activate_hunt):
            with self.subTest(command=function.__name__), \
                 patch.object(manage, 'population_preflight') as preflight, \
                 patch.object(manage, 'lab_mysql') as sql:
                with self.assertRaisesRegex(manage.SetupError, 'LAB_AI_PROFILE=disabled'):
                    function(self.data, settings, self.metadata, profile='hunt-100')
                preflight.assert_not_called()
                sql.assert_not_called()


if __name__ == '__main__':
    unittest.main()
