from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from fastapi.testclient import TestClient

from app.main import create_app
from app.targets import TargetRegistry
from test_character_studio import draft, fixture_catalog


class AuthoringEmptyTests(unittest.TestCase):
    def test_empty_map_does_not_authorize_previous_population_identities(self):
        registry = TargetRegistry.load(Path("missing-authoring-checkout"), "authoring-empty")
        self.assertFalse(registry.available)
        self.assertEqual(registry.targets, ())
        self.assertEqual(registry.profile, "authoring-empty")
        self.assertIn("nemá registrovaná NPC", registry.reason)
        for identity in ("901014", "901035", "901036", "901099"):
            with self.subTest(identity=identity):
                binding = registry.validate(identity, identity, 725)
                self.assertFalse(binding["eligible"])
                self.assertFalse(binding["registry_available"])
                self.assertIsNone(binding["target"])
                self.assertEqual(binding["reason"], registry.reason)

    def test_drafts_survive_save_and_backup_while_all_game_exports_stay_blocked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.json"
            catalog.write_text(json.dumps(fixture_catalog()), encoding="utf-8")
            app = create_app(data_dir=root / "drafts", catalog_path=catalog,
                             source_dir=root / "no-sources", appearance_dir=root / "no-appearances",
                             model_data_dir=root / "no-models", ai_url="", password="",
                             project_root=root / "no-checkout", population_profile="authoring-empty")
            with TestClient(app) as client:
                headers = {"X-CSRF-Token": client.get("/api/session").json()["csrf_token"]}
                listing = client.get("/api/catalog").json()
                self.assertEqual(listing["targets"], [])
                self.assertEqual(listing["target_registry"]["profile"], "authoring-empty")
                self.assertFalse(listing["target_registry"]["available"])
                response = client.post("/api/characters", json=draft(""), headers=headers)
                self.assertEqual(response.status_code, 201, response.text)
                saved = response.json()
                endpoint = f"/api/characters/{saved['id']}"
                self.assertEqual(client.get(endpoint).json(), saved)
                backup = client.get(endpoint + "/draft")
                self.assertEqual(backup.status_code, 200)
                self.assertEqual(backup.json()["card"], saved["card"])
                self.assertEqual(backup.json()["appearance"], saved["appearance"])
                self.assertEqual(backup.json()["authoring"], saved["authoring"])
                # Filling an old once-valid ID into a future draft cannot
                # restore the retired population or authorize its exports.
                updated = client.put(endpoint, json={"revision": saved["revision"], **draft()}, headers=headers)
                self.assertEqual(updated.status_code, 200, updated.text)
                saved = updated.json()
                for export in ("profile", "appearance", "template", "bundle.zip"):
                    with self.subTest(export=export):
                        refused = client.get(endpoint + "/" + export)
                        self.assertEqual(refused.status_code, 503, refused.text)
                        self.assertIn("nemá registrovaná NPC", refused.json()["detail"])
                self.assertEqual(client.get(endpoint).json(), saved)


if __name__ == "__main__":
    unittest.main()
