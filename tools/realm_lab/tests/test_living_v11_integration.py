"""Canonical setup, CD and character identities use the same new population."""
from dataclasses import replace
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import character_templates, graveyard, living_v11, manage, run_scoped, world_setup
from tools.realm_lab.tests import test_authoring_integration as authoring_tests
from tools.realm_lab.tests.test_setup import environment

ROOT=Path(__file__).resolve().parents[3]


class LivingIntegrationTests(unittest.TestCase):
    def test_live_and_paused_configuration_keep45identities_and_separate_llm_switch(self):
        settings=manage.Settings.load({**environment(),'LAB_AI_PROFILE':'living-v11','LAB_WILDLIFE_ENABLED':'1'})
        population=living_v11.Population.load()
        world_setup.validate_profile(settings)
        rendered=manage.render_config((ROOT/'deploy/worldserver.conf').read_text(),settings,population)
        for text in ('AIWorld.Enable = 1\n','AIWorld.LivingResidentsV11.Enabled = 1\n',
                     'AIWorld.LivingRoleExtensionsEnabled = 1\n','AIWorld.NpcAgentEnabled = 0\n',
                     'AIWorld.LivingSupplyEnabled = 0\n','AIWorld.ScopeMapId = 725\n'):
            self.assertIn(text,rendered)
        paused=manage.config_overrides(replace(settings,wildlife_enabled=False),population)
        self.assertEqual(paused['AIWorld.Enable'],'0')
        self.assertEqual(paused['AIWorld.LivingResidentsV11.Enabled'],'0')
        self.assertEqual(paused['AIWorld.ScopeSpawnIds'],population.config()['AIWorld.ScopeSpawnIds'])
        with self.assertRaises(manage.SetupError):
            manage.Settings.load({**environment(),'LAB_AI_PROFILE':'living-v11','LAB_NPC_AGENTS_ENABLED':'1'})

    def test_character_clone_identity_and_graveyard_follow_v11_instead_of_retired_v8(self):
        population=living_v11.Population.load()
        self.assertEqual(character_templates.population_for_profile('living-v11'),population)
        for actor in population.actors:
            spec=character_templates.spec_from_document(dict(schema_version=1,spawn_id=actor.spawn_id,
                entry=9810000+actor.spawn_id-902000,base_entry=actor.entry,map_id=725,name='Obyvatel',title=''),population)
            self.assertEqual(spec.spawn_id,actor.spawn_id)
        with patch.dict('os.environ',{'LAB_AI_PROFILE':'living-v11'}):
            location=graveyard.Graveyard.load()
        self.assertEqual(graveyard.authored_manifest('living-v11'),population.metadata/'graveyard.json')
        self.assertEqual(location,graveyard.Graveyard.load(population.metadata/'graveyard.json'))

    def test_pilot_cleanup_preserves_new_controlled_membership_but_refuses_foreign_agents(self):
        settings=manage.Settings.load({**environment(),'LAB_AI_PROFILE':'living-v11'})
        with patch('tools.realm_lab.settlement_population.ensure_world_offline'), \
             patch.object(manage,'lab_mysql',side_effect=['','0']) as sql:
            world_setup.demote_retired_pilots(settings)
        predicate=sql.call_args.args[1]
        self.assertIn('902000',predicate)
        self.assertIn('902044',predicate)
        self.assertNotIn('901000',predicate)
        with patch('tools.realm_lab.settlement_population.ensure_world_offline'), \
             patch.object(manage,'lab_mysql',side_effect=['','1']):
            with self.assertRaises(manage.SetupError):world_setup.demote_retired_pilots(settings)

    def test_adoption_requires_all45_owned_character_templates_before_query_or_mutation(self):
        settings=manage.Settings.load({**environment(),'LAB_AI_PROFILE':'living-v11'})
        population=living_v11.Population.load()
        with patch.object(living_v11,'verify_data',return_value=population), \
             patch.object(character_templates,'verify',return_value=()), \
             patch.object(manage,'lab_mysql') as sql:
            with self.assertRaisesRegex(manage.SetupError,'complete set'):
                manage.residents_preflight(Path('data'),settings,bootstrap=True)
        sql.assert_not_called()


@unittest.skipUnless(authoring_tests.bash_binary(),'Bash is required for deployment-flow checks')
class LivingShellIntegrationTests(unittest.TestCase):
    HARNESS=authoring_tests.AuthoringShellIntegrationTests.HARNESS.replace(
        "*'preflight-authoring-empty'*)", "*'preflight-living-v11-adoption'*) if [ \"$FAIL_PREFLIGHT\" = 1 ]; then return 42; fi;;\n    *'preflight-authoring-empty'*)")
    run_script=authoring_tests.AuthoringShellIntegrationTests.run_script

    def test_both_flows_verify_adoption_before_stop_and_never_bootstrap_old100(self):
        for filename in ('setup-world.sh','deploy-lab.sh'):
            with self.subTest(script=filename):
                result,calls=self.run_script(filename,profile='living-v11')
                self.assertEqual(result.returncode,0,result.stderr[-2000:])
                order=[next(i for i,call in enumerate(calls) if token in call) for token in
                    ('preflight-living-v11-adoption','character_templates plan','stop worldserver',
                     'character_templates restore','world_setup.py prepare','bootstrap-living-v11','character_templates apply')]
                self.assertEqual(order,sorted(order))
                self.assertFalse(any('bootstrap-full-v8' in call or 'bootstrap-settlement-v8' in call for call in calls))
                if filename=='deploy-lab.sh':
                    self.assertTrue(any('app.seed_residents' in call for call in calls))
                    self.assertTrue(any('telemetry_contract.py' in call for call in calls))

    def test_foreign_adoption_refuses_stop_or_any_mutation(self):
        for filename in ('setup-world.sh','deploy-lab.sh'):
            with self.subTest(script=filename):
                result,calls=self.run_script(filename,profile='living-v11',fail=True)
                self.assertEqual(result.returncode,42,result.stderr[-2000:])
                self.assertFalse(any('stop worldserver' in call or 'bootstrap-' in call for call in calls))


if __name__=='__main__':unittest.main()
