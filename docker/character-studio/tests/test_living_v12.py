"""The map-only V12 profile preserves all V11 Studio identities and edits."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from fastapi.testclient import TestClient
from app.main import create_app
from app.models import DraftInput
from app.seed_residents import seed
from app.storage import DraftStore
from app.targets import TargetRegistry
from test_character_studio import draft,fixture_catalog

ROOT=Path(__file__).resolve().parents[3]


class LivingV12StudioTests(unittest.TestCase):
    def test_actual_registry_and_exports_preserve_every_template_and_control_mode(self):
        old=TargetRegistry.load(ROOT,'living-v11')
        current=TargetRegistry.load(ROOT,'living-v12')
        self.assertTrue(current.available,current.reason)
        self.assertEqual(current.targets,old.targets)
        self.assertEqual(current.profile,'living-v12')
        self.assertEqual(len(current.targets),45)
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);catalog=root/'catalog.json';catalog.write_text(json.dumps(fixture_catalog()),encoding='utf-8')
            app=create_app(data_dir=root/'drafts',catalog_path=catalog,source_dir=root/'source',appearance_dir=root/'appearance',
                model_data_dir=root/'models',password='',ai_url='',target_registry=current)
            with TestClient(app) as client:
                headers={'X-CSRF-Token':client.get('/api/session').json()['csrf_token']}
                self.assertEqual(client.get('/api/catalog').json()['target_registry']['profile'],'living-v12')
                for spawn in (902000,902011,902016,902022,902025,902035,902044):
                    saved=client.post('/api/characters',json=draft(str(spawn)),headers=headers)
                    self.assertEqual(saved.status_code,201,saved.text)
                    path=f"/api/characters/{saved.json()['id']}"
                    template=client.get(path+'/template')
                    self.assertEqual(template.status_code,200,template.text)
                    self.assertEqual(template.json()['entry'],9810000+spawn-902000)
                    self.assertEqual(template.json()['base_entry'],38 if spawn>=902035 else 11328)
                    self.assertEqual(client.get(path+'/appearance').status_code,422)

    def test_living_v11_to_v12_seed_changes_no_draft_card_or_appearance(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);cards=root/'cards';cards.mkdir()
            for source in (ROOT/'data/npc_agents').glob('*.json'):(cards/source.name).write_bytes(source.read_bytes())
            hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in cards.glob('*.json')}
            catalog=root/'catalog.json';catalog.write_text(json.dumps(fixture_catalog()),encoding='utf-8')
            args=dict(project_root=ROOT,source_dir=cards,catalog_path=catalog,data_dir=root/'drafts')
            self.assertEqual(seed(**args,profile='living-v11'),dict(created=45,preserved=0,residents=45))
            store=DraftStore(root/'drafts');first=store.list()[0]
            edited=DraftInput.model_validate({key:first[key] for key in ('card','appearance','authoring')})
            edited.card.name='Vlastní zachované jméno';edited.authoring.notes='Zachovat při nasazení mostů.'
            store.update(first['id'],first['revision'],edited)
            before={row['id']:row for row in store.list()}
            for _ in range(2):
                self.assertEqual(seed(**args,profile='living-v12'),dict(created=0,preserved=45,residents=45))
                self.assertEqual({row['id']:row for row in store.list()},before)
            self.assertEqual({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in cards.glob('*.json')},hashes)


if __name__=='__main__':unittest.main()
