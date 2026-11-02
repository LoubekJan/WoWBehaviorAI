"""Map-only upgrades retain the V11 living controller, every home and SQL state."""
import copy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.realm_lab import character_templates, living_v11, living_v12, manage, residents_population as v10, residents_v11, residents_v12, settlement_population
from tools.realm_lab.tests.test_living_v11 import fixture as previous_fixture, controlled_snapshot, telemetry
from tools.realm_lab.tests.test_residents_population import write_json
from tools.realm_lab.tests.test_setup import environment


def fixture(root):
    metadata11,geometry11,native11,living_document,old_baseline=previous_fixture(root)
    with patch.object(v10.Population,'load',return_value=old_baseline),patch.object(living_v11,'LOCATIONS_HEADER',metadata11/'locations.h'):
        baseline=living_v11.Population.load(metadata11,geometry11)
    metadata=root/'preview-v12';metadata.mkdir()
    geometry=SimpleNamespace(**{**geometry11.__dict__,'metadata':metadata,'source_revision':'preview-v12'})
    def clone(name,profile):
        value=json.loads((metadata11/name).read_text(encoding='utf-8'))
        value.update(profile=profile,source_revision='preview-v12')
        write_json(metadata/name,value)
        return value
    clone(residents_v12.FACTION_PROOF,residents_v12.PROFILE)
    clone(residents_v12.POSITION_PROOF,residents_v12.PROFILE)
    native_doc=clone(residents_v12.MANIFEST,residents_v12.PROFILE)
    native_doc.update(position_proof_sha256=hashlib.sha256((metadata/residents_v12.POSITION_PROOF).read_bytes()).hexdigest(),
        faction_proof_sha256=hashlib.sha256((metadata/residents_v12.FACTION_PROOF).read_bytes()).hexdigest())
    write_json(metadata/residents_v12.MANIFEST,native_doc)
    proof=clone(living_v12.POSITION_PROOF,living_v12.PROFILE)
    proof['resident_position_proof_sha256']=native_doc['position_proof_sha256']
    write_json(metadata/living_v12.POSITION_PROOF,proof)
    with patch.object(v10.Population,'load',return_value=old_baseline),patch.object(residents_v12,'baseline_population',return_value=native11):
        native12=residents_v12.Population.load(metadata,geometry)
    with patch.object(living_v11,'LOCATIONS_HEADER',metadata11/'locations.h'),patch.object(living_v12,'compiled_contract',return_value=(baseline,living_document)):
        _proof,contract=living_v12.position_contract(native12)
        document=living_v12.manifest_document(native12,hashlib.sha256((metadata/residents_v12.MANIFEST).read_bytes()).hexdigest(),contract)
    write_json(metadata/living_v12.MANIFEST,document)
    return metadata,geometry,native11,baseline,living_document,old_baseline,metadata11


class LivingV12Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.metadata,self.geometry,self.native11,self.baseline,self.original,self.old_baseline,self.metadata11=fixture(Path(self.tmp.name))
        for owner,name,value in ((v10.Population,'load',self.old_baseline),(residents_v12,'baseline_population',self.native11),
                (living_v12,'compiled_contract',(self.baseline,self.original))):
            p=patch.object(owner,name,return_value=value);p.start();self.addCleanup(p.stop)
        p=patch.object(living_v11,'LOCATIONS_HEADER',self.metadata11/'locations.h');p.start();self.addCleanup(p.stop)
        self.population=living_v12.Population.load(self.metadata,self.geometry)

    def test_geometry_revision_changes_without_actor_catalog_controller_or_sql_changes(self):
        self.assertEqual(self.population.profile,'living-v12')
        self.assertEqual(self.population.source_revision,'preview-v12')
        self.assertEqual(self.population.actors,self.baseline.actors)
        self.assertEqual(self.population.config(),{**self.baseline.config(),living_v12.transit.FLAG:'0'})
        self.assertEqual(living_v12.runtime_catalog(self.population.positions),living_v11.runtime_catalog(self.baseline.positions))
        self.assertEqual(living_v12.bootstrap_sql(self.population),living_v11.bootstrap_sql(self.baseline))
        rows=controlled_snapshot(self.population)
        living_v12.validate_snapshot(rows,self.population)
        self.assertIn('control_mode=0',living_v12.bootstrap_sql(self.population))

    def test_profile_revision_and_compiled_catalog_pairs_cannot_be_mixed(self):
        raw=(self.metadata/living_v12.MANIFEST).read_bytes()
        document=json.loads(raw)
        for field,value in (('profile','living-v11'),('source_revision','preview-v11'),('behavior_profile','living-v12'),
                             ('compiled_position_proof_sha256','f'*64)):
            write_json(self.metadata/living_v12.MANIFEST,{**document,field:value})
            with self.subTest(field=field),self.assertRaises(living_v12.ProfileError):
                living_v12.Population.load(self.metadata,self.geometry)
        (self.metadata/living_v12.MANIFEST).write_bytes(raw)
        proof=json.loads((self.metadata/living_v12.POSITION_PROOF).read_text())
        proof['depots'][0]['position']['x']+=1
        write_json(self.metadata/living_v12.POSITION_PROOF,proof)
        with self.assertRaises(living_v12.ProfileError):living_v12.Population.load(self.metadata,self.geometry)

    def test_all45_homes_including_inn_hall_and_tents_are_immutable(self):
        raw=(self.metadata/residents_v12.MANIFEST).read_bytes()
        document=json.loads(raw)
        for index in (0,22,25,35,44):
            changed=copy.deepcopy(document);changed['actors'][index]['x']+=1
            write_json(self.metadata/residents_v12.MANIFEST,changed)
            with self.subTest(index=index),self.assertRaises(living_v12.ProfileError):
                living_v12.Population.load(self.metadata,self.geometry)
        (self.metadata/residents_v12.MANIFEST).write_bytes(raw)

    def test_live_and_paused_settings_preserve_the_existing_runtime_feature(self):
        s=manage.Settings.load({**environment(),'LAB_AI_PROFILE':'living-v12','LAB_WILDLIFE_ENABLED':'1'})
        flags=manage.config_overrides(s,self.population)
        self.assertEqual(flags['AIWorld.Enable'],'1')
        self.assertEqual(flags['AIWorld.LivingResidentsV11.Enabled'],'1')
        self.assertEqual(flags['AIWorld.NpcAgentEnabled'],'0')
        self.assertEqual(flags['AIWorld.LivingNeedEvolutionEnabled'],'1')
        paused=manage.config_overrides(replace(s,wildlife_enabled=False),self.population)
        self.assertEqual(paused['AIWorld.LivingResidentsV11.Enabled'],'0')
        self.assertEqual(paused['AIWorld.ScopeSpawnIds'],flags['AIWorld.ScopeSpawnIds'])

    def test_telemetry_keeps_the_compiled_v11_payload_and_all45_controlled_identities(self):
        state=telemetry(self.population)
        self.assertEqual(living_v12.validate_telemetry(state,self.population)['managed'],45)
        state['agents'][0]['supply']['population_profile']='living-v12'
        with self.assertRaises(living_v12.ProfileError):living_v12.validate_telemetry(state,self.population)

    def test_complete_existing_bindings_and_cards_are_required_during_adoption(self):
        s=manage.Settings.load({**environment(),'LAB_AI_PROFILE':'living-v12'})
        specs=tuple(character_templates.spec_from_document(dict(schema_version=1,spawn_id=a.spawn_id,map_id=725,
            entry=9810000+a.spawn_id-902000,base_entry=a.entry,name='Obyvatel',title=''),self.population) for a in self.population.actors)
        self.assertEqual(len(specs),45)
        with patch.object(living_v12,'verify_data',return_value=self.population),patch.object(character_templates,'verify',return_value=()),patch.object(manage,'lab_mysql') as query:
            with self.assertRaisesRegex(manage.SetupError,'complete set'):
                manage.residents_preflight(Path('data'),s,bootstrap=True)
        query.assert_not_called()


if __name__=='__main__':unittest.main()
