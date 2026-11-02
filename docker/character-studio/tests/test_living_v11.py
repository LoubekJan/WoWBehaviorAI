"""The living profile keeps all 45 authored identities and durable drafts."""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from fastapi.testclient import TestClient

from app.main import create_app
from app.models import DraftInput
from app.seed_residents import seed
from app.storage import DraftStore
from app.targets import TargetRegistry
from test_character_studio import draft, fixture_catalog


ROOT=Path(__file__).resolve().parents[3]
IDENTITIES=set(range(902000,902045))


class LivingV11StudioTests(unittest.TestCase):
    def test_actual_registry_preserves45_current_identities_and_reports_control_without_claiming_llm(self):
        registry=TargetRegistry.load(ROOT,'living-v11')
        self.assertTrue(registry.available,registry.reason)
        self.assertEqual({t.spawn_id for t in registry.targets},IDENTITIES)
        self.assertTrue(all(t.eligible and t.control_mode=='AI_WORLD_CONTROLLED' for t in registry.targets))
        self.assertEqual({t.spawn_id for t in registry.targets if t.entry==38},set(range(902035,902045)))
        self.assertTrue(all(t.entry==11328 for t in registry.targets if t.spawn_id<902035))
        for spawn in sorted(IDENTITIES):
            with self.subTest(spawn=spawn):
                target=registry.validate(str(spawn),str(spawn),725)
                self.assertTrue(target['eligible'])
                self.assertEqual(target['target']['control_mode'],'AI_WORLD_CONTROLLED')
                self.assertIn('LLM řízení se zapíná samostatně',target['target']['reason'])
        for spawn in (900725,901014,901035,901099,902045):
            self.assertFalse(registry.validate(str(spawn),str(spawn),725)['eligible'])
        self.assertFalse(registry.validate('902000','902001',725)['eligible'])
        self.assertFalse(registry.validate('902000','902000',0)['eligible'])

    def test_all45_native_template_exports_have_unique_entries_and_correct_faction_sources(self):
        registry=TargetRegistry.load(ROOT,'living-v11')
        self.assertTrue(registry.available,registry.reason)
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);catalog=root/'catalog.json'
            catalog.write_text(json.dumps(fixture_catalog()),encoding='utf-8')
            app=create_app(data_dir=root/'drafts',catalog_path=catalog,source_dir=root/'source',
                appearance_dir=root/'appearances',model_data_dir=root/'models',ai_url='',password='',target_registry=registry)
            with TestClient(app) as client:
                headers={'X-CSRF-Token':client.get('/api/session').json()['csrf_token']}
                listing=client.get('/api/catalog').json()
                self.assertEqual(listing['target_registry']['profile'],'living-v11')
                self.assertEqual(len(listing['targets']),45)
                self.assertTrue(all(t['control_mode']=='AI_WORLD_CONTROLLED' for t in listing['targets']))
                entries=set()
                for spawn in sorted(IDENTITIES):
                    response=client.post('/api/characters',json=draft(str(spawn)),headers=headers)
                    self.assertEqual(response.status_code,201,response.text)
                    endpoint=f"/api/characters/{response.json()['id']}"
                    result=client.get(endpoint+'/template')
                    self.assertEqual(result.status_code,200,result.text)
                    template=result.json();entries.add(template['entry'])
                    self.assertEqual(template['entry'],9810000+spawn-902000)
                    self.assertEqual(template['base_entry'],38 if spawn>=902035 else 11328)
                    self.assertEqual((template['spawn_id'],template['map_id']),(spawn,725))
                self.assertEqual(entries,set(range(9810000,9810045)))
                invalid=client.post('/api/characters',json=draft('901014'),headers=headers).json()
                endpoint=f"/api/characters/{invalid['id']}"
                self.assertEqual(client.get(endpoint+'/template').status_code,422)
                self.assertEqual(client.get(endpoint).json(),invalid)

    def test_bundle_keeps_original_game_appearance_and_explains_separate_llm_activation(self):
        registry=TargetRegistry.load(ROOT,'living-v11')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);catalog=root/'catalog.json'
            catalog.write_text(json.dumps(fixture_catalog()),encoding='utf-8')
            app=create_app(data_dir=root/'drafts',catalog_path=catalog,source_dir=root/'source',
                appearance_dir=root/'appearances',model_data_dir=root/'models',ai_url='',password='',target_registry=registry)
            with TestClient(app) as client:
                headers={'X-CSRF-Token':client.get('/api/session').json()['csrf_token']}
                for spawn in (902000,902016,902022,902025,902035,902044):
                    response=client.post('/api/characters',json=draft(str(spawn)),headers=headers)
                    self.assertEqual(response.status_code,201,response.text)
                    saved=response.json();endpoint=f"/api/characters/{saved['id']}"
                    refused=client.get(endpoint+'/appearance')
                    self.assertEqual(refused.status_code,422,refused.text)
                    bundle=client.get(endpoint+'/bundle.zip')
                    self.assertEqual(bundle.status_code,200,bundle.text if bundle.status_code!=200 else '')
                    with zipfile.ZipFile(io.BytesIO(bundle.content)) as archive:
                        self.assertEqual(set(archive.namelist()),{f'npc_agents/{spawn}.json',
                            f'npc_templates/{spawn}.json','character-studio-draft.json','README.txt'})
                        self.assertEqual(json.loads(archive.read('character-studio-draft.json'))['appearance'],saved['appearance'])
                        self.assertEqual(json.loads(archive.read(f'npc_templates/{spawn}.json'))['base_entry'],38 if spawn>=902035 else 11328)
                        self.assertEqual(json.loads(archive.read(f'npc_agents/{spawn}.json'))['backstory'],saved['card']['backstory'])
                        readme=archive.read('README.txt').decode('utf-8')
                        self.assertIn('původní herní vzhled a řízení AI World',readme)
                        self.assertIn('LLM řízení se zapíná samostatně',readme)
                        self.assertIn('Do hry se tímto balíčkem neinstaluje',readme)

    def test_native_to_living_seed_preserves_every_draft_and_all_source_cards_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'cards';source.mkdir()
            for spawn in sorted(IDENTITIES):
                (source/f'{spawn}.json').write_bytes((ROOT/'data/npc_agents'/f'{spawn}.json').read_bytes())
            before_sources={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in source.glob('*.json')}
            catalog=root/'catalog.json';catalog.write_text(json.dumps(fixture_catalog()),encoding='utf-8')
            args=dict(project_root=ROOT,source_dir=source,catalog_path=catalog,data_dir=root/'drafts')
            self.assertEqual(seed(**args,profile='residents-v11'),dict(created=45,preserved=0,residents=45))
            store=DraftStore(root/'drafts')
            for identity in ('902000','902035'):
                row=next(r for r in store.list() if r['card']['spawn_id']==identity)
                edited=DraftInput.model_validate({k:row[k] for k in ('card','appearance','authoring')})
                edited.card.name='Vlastní soused' if identity=='902000' else 'Vlastní Defias'
                edited.card.backstory='Autorem upravený příběh se musí zachovat.'
                edited.appearance.skin=1;edited.appearance.face=1
                edited.authoring.notes='Vlastní poznámka autora.'
                store.update(row['id'],row['revision'],edited)
            before_drafts={r['id']:r for r in store.list()}
            for _ in range(2):
                self.assertEqual(seed(**args,profile='living-v11'),dict(created=0,preserved=45,residents=45))
                self.assertEqual({r['id']:r for r in store.list()},before_drafts)
            self.assertEqual({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in source.glob('*.json')},before_sources)
            self.assertEqual(sum(r['authoring']['faction']=='Defias' for r in store.list()),10)


if __name__=='__main__':unittest.main()
