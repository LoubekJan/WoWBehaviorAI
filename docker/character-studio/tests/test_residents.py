from __future__ import annotations

import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from fastapi.testclient import TestClient

from app.main import create_app
from app.models import DraftInput
from app.seed_residents import seed
from app.storage import DraftStore
from app.targets import Target, TargetRegistry
from test_character_studio import draft, fixture_catalog


ROOT = Path(__file__).resolve().parents[3]


def registry(profile='residents-v10'):
    end=902045 if profile=='residents-v11' else 902022
    return TargetRegistry(True, profile, tuple(Target(spawn, 725, 'Obyvatel', 'defias_camp' if spawn>=902035 else 'resident',
        'COMBATANT' if spawn>=902035 else 'CIVILIAN', 38 if spawn>=902035 else 11328,
        True, 'Běžné herní AI', 'NATIVE_CORE') for spawn in range(902000,end)))


class ResidentStudioTests(unittest.TestCase):
    def test_actual_reviewed_resident_registry_is_native_and_rejects_retired_identities(self):
        current = TargetRegistry.load(ROOT, 'residents-v10')
        self.assertTrue(current.available, current.reason)
        self.assertEqual({target.spawn_id for target in current.targets}, set(range(902000, 902022)))
        self.assertTrue(all(target.eligible and target.control_mode=='NATIVE_CORE' and target.entry==11328
                            for target in current.targets))
        for spawn in range(902000, 902022):
            self.assertTrue(current.validate(str(spawn), str(spawn), 725)['eligible'])
        for spawn in (900725, 901014, 902022):
            self.assertFalse(current.validate(str(spawn), str(spawn), 725)['eligible'])
        self.assertFalse(current.validate('902000','902001',725)['eligible'])
        self.assertFalse(current.validate('902000','902000',0)['eligible'])

    def test_native_residents_export_unique_templates_and_keep_native_control(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root/'catalog.json'
            catalog.write_text(json.dumps(fixture_catalog()), encoding='utf-8')
            app = create_app(data_dir=root/'drafts', catalog_path=catalog, source_dir=root/'source',
                             appearance_dir=root/'appearances', model_data_dir=root/'models',
                             ai_url='', password='', target_registry=registry())
            entries = set()
            with TestClient(app) as client:
                headers = {'X-CSRF-Token':client.get('/api/session').json()['csrf_token']}
                rows = client.get('/api/catalog').json()['targets']
                self.assertEqual(len(rows), 22)
                self.assertTrue(all(row['control_mode']=='NATIVE_CORE' for row in rows))
                for spawn in range(902000, 902022):
                    saved = client.post('/api/characters', json=draft(str(spawn)), headers=headers)
                    self.assertEqual(saved.status_code, 201, saved.text)
                    endpoint = f"/api/characters/{saved.json()['id']}"
                    exported = client.get(endpoint+'/template')
                    self.assertEqual(exported.status_code, 200, exported.text)
                    template = exported.json()
                    self.assertEqual(template['entry'], 9810000+spawn-902000)
                    self.assertEqual(template['base_entry'], 11328)
                    entries.add(template['entry'])
                    if spawn==902000:
                        self.assertEqual(client.get(endpoint+'/appearance').status_code, 422)
                        bundle = client.get(endpoint+'/bundle.zip')
                        with zipfile.ZipFile(io.BytesIO(bundle.content)) as archive:
                            self.assertEqual(json.loads(archive.read('npc_templates/902000.json')), template)
                            self.assertFalse(any(name.startswith('npc_appearances/') for name in archive.namelist()))
                            self.assertIn('původní herní vzhled', archive.read('README.txt').decode())
                            self.assertIn('appearance', json.loads(archive.read('character-studio-draft.json')))
                self.assertEqual(len(entries), 22)
                invalid = client.post('/api/characters', json=draft('901014'), headers=headers).json()
                self.assertEqual(client.get(f"/api/characters/{invalid['id']}/template").status_code, 422)

    def test_seed_is_repeatable_and_preserves_authored_edits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root/'cards'; source.mkdir()
            catalog = root/'catalog.json'
            catalog.write_text(json.dumps(fixture_catalog()), encoding='utf-8')
            for spawn in range(902000, 902022):
                card = draft(str(spawn))['card']
                card.update(agent_id=spawn, spawn_id=spawn)
                (source/f'{spawn}.json').write_text(json.dumps(card), encoding='utf-8')
            args = dict(project_root=ROOT, source_dir=source, catalog_path=catalog,
                        data_dir=root/'drafts', profile='residents-v10')
            with patch('app.seed_residents.TargetRegistry.load', return_value=registry()):
                self.assertEqual(seed(**args), {'created':22, 'preserved':0, 'residents':22})
                store = DraftStore(root/'drafts')
                first = store.list()[0]
                changed = DraftInput.model_validate({key:first[key] for key in ('card','appearance','authoring')})
                changed.card.name='Upravený obyvatel'
                before = store.update(first['id'], first['revision'], changed)
                self.assertEqual(seed(**args), {'created':0, 'preserved':22, 'residents':22})
                self.assertEqual(store.get(first['id']), before)
                self.assertEqual(len(store.list()), 22)
                with self.assertRaises(ValueError): seed(**{**args, 'profile':'authoring-empty'})
                self.assertEqual(store.get(first['id']), before)

    def test_v11_registry_and_exports_retain_custom_defias_source_for_only_ten_tent_people(self):
        current=TargetRegistry.load(ROOT,'residents-v11')
        self.assertTrue(current.available,current.reason)
        self.assertEqual({t.spawn_id for t in current.targets},set(range(902000,902045)))
        self.assertEqual({t.spawn_id for t in current.targets if t.entry==38},set(range(902035,902045)))
        self.assertTrue(all(t.control_mode=='NATIVE_CORE' for t in current.targets))
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);catalog=root/'catalog.json';catalog.write_text(json.dumps(fixture_catalog()),encoding='utf-8')
            app=create_app(data_dir=root/'drafts',catalog_path=catalog,source_dir=root/'source',appearance_dir=root/'appearances',
                           model_data_dir=root/'models',ai_url='',password='',target_registry=current)
            with TestClient(app) as client:
                headers={'X-CSRF-Token':client.get('/api/session').json()['csrf_token']}
                for spawn in (902022,902025,902035,902044):
                    response=client.post('/api/characters',json=draft(str(spawn)),headers=headers)
                    self.assertEqual(response.status_code,201,response.text)
                    endpoint=f"/api/characters/{response.json()['id']}"
                    result=client.get(endpoint+'/template')
                    self.assertEqual(result.status_code,200,result.text)
                    self.assertEqual(result.json()['base_entry'],38 if spawn>=902035 else 11328)
                    self.assertEqual(result.json()['entry'],9810000+spawn-902000)
                    self.assertEqual(client.get(endpoint+'/appearance').status_code,422)

    def test_v11_seeds_only_23_additions_and_preserves_22_existing_edited_drafts(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'cards';source.mkdir()
            catalog=root/'catalog.json';catalog.write_text(json.dumps(fixture_catalog()),encoding='utf-8')
            def add_cards(start,end):
                for spawn in range(start,end):
                    card=draft(str(spawn))['card'];card.update(agent_id=spawn,spawn_id=spawn)
                    (source/f'{spawn}.json').write_text(json.dumps(card),encoding='utf-8')
            add_cards(902000,902022)
            args=dict(project_root=ROOT,source_dir=source,catalog_path=catalog,data_dir=root/'drafts',profile='residents-v10')
            with patch('app.seed_residents.TargetRegistry.load',return_value=registry()):
                self.assertEqual(seed(**args)['created'],22)
            store=DraftStore(root/'drafts');doc=store.list()[0]
            changed=DraftInput.model_validate({key:doc[key] for key in ('card','appearance','authoring')})
            changed.card.name='Zachovaný obyvatel'
            preserved=store.update(doc['id'],doc['revision'],changed)
            before={row['id']:row for row in store.list()}
            add_cards(902022,902045)
            with patch('app.seed_residents.TargetRegistry.load',return_value=registry('residents-v11')):
                self.assertEqual(seed(**{**args,'profile':'residents-v11'}),dict(created=23,preserved=22,residents=45))
                self.assertEqual(seed(**{**args,'profile':'residents-v11'}),dict(created=0,preserved=45,residents=45))
            self.assertEqual(len(store.list()),45)
            self.assertEqual(store.get(doc['id']),preserved)
            self.assertTrue(all(store.get(identity)==row for identity,row in before.items()))
            self.assertEqual(sum(row['authoring']['faction']=='Defias' for row in store.list()),10)


if __name__=='__main__': unittest.main()
