"""A native clone must pass provenance checks before base-role validation."""
import json
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import character_templates as templates, full_population as full, manage, settlement_population as settlement
from tools.realm_lab.tests.test_full_population import snapshot
from tools.realm_lab.tests.test_setup import environment


class CharacterTemplatePreflightTests(unittest.TestCase):
    def setUp(self):
        self.population = full.Population.load()
        self.settings = manage.Settings.load({**environment(), 'LAB_AI_PROFILE': 'full-v8'})
        self.binding = templates.TemplateSpec(901014, 9800014, 11328, 725, 'Šaša', 'Obyvatelka')
        self.rows = [r for r in snapshot(self.population)
                     if not any(r.get(key) in settlement.LEGACY_IDS for key in ('id', 'spawn', 'guid', 'member'))]
        next(r for r in self.rows if r['kind'] == 'spawn' and r['guid'] == 901014)['entry'] = 9800014

    def invoke(self, **flags):
        raw = '\n'.join(json.dumps(r) for r in self.rows)
        with patch.object(full, 'verify_data', return_value=self.population), \
             patch.object(manage, 'lab_mysql', return_value=raw), \
             patch.object(templates, 'verify', return_value=(self.binding,)) as verified:
            result = manage.full_preflight(Path('unused'), self.settings, **flags)
        return result, verified

    def test_verified_clone_retains_reviewed_spawn_identity_and_worker_contract(self):
        with self.assertRaises(full.ProfileError):
            full.validate_snapshot(self.rows, self.population)
        result, verified = self.invoke()
        self.assertEqual(result, self.population)
        verified.assert_called_once_with(self.settings, allow_restored=False)
        # The database snapshot is not mutated and the trusted source still has11328.
        self.assertEqual(next(r for r in self.rows if r['kind'] == 'spawn' and r['guid'] == 901014)['entry'], 9800014)
        self.assertEqual(next(a for a in result.actors if a.spawn_id == 901014).entry, 11328)

    def test_only_explicit_offline_bootstrap_allows_temporarily_restored_entries(self):
        for flags in ({'bootstrap': True}, {'templates_restored': True}):
            with self.subTest(flags=flags):
                _, verified = self.invoke(**flags)
                verified.assert_called_once_with(self.settings, allow_restored=True)

    def test_unverified_database_changes_are_not_hidden_by_template_projection(self):
        raw = '\n'.join(json.dumps(r) for r in self.rows)
        with patch.object(full, 'verify_data', return_value=self.population), \
             patch.object(manage, 'lab_mysql', return_value=raw), \
             patch.object(templates, 'verify', side_effect=full.ProfileError('tampered clone')):
            with self.assertRaisesRegex(full.ProfileError, 'tampered clone'):
                manage.full_preflight(Path('unused'), self.settings)


if __name__ == '__main__':
    unittest.main()
