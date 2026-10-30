from __future__ import annotations

import base64
import copy
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
import zipfile

import httpx
from fastapi.testclient import TestClient

from app.main import create_app
from app.models import SLOTS, UINT64_MAX
from app.targets import Target, TargetRegistry


def fixture_catalog():
    variants = []
    for gender in (0, 1):
        variants.append({"gender": gender, "skins": [0, 1], "faces": [0, 1], "hair_styles": [0, 1],
                         "hair_colors": [0, 1], "facial_hairs": [0, 1],
                         "face_options": [{"skin": 0, "faces": [0]}, {"skin": 1, "faces": [1]}],
                         "hair_options": [{"style": 0, "colors": [0, 1]}, {"style": 1, "colors": [1]}],
                         "facial_options": [{"style": 0, "colors": [0, 1]}, {"style": 1, "colors": [1]}]})
    return {"schema_version": 1, "client_build": 12340,
            "races": [{"id": 1, "name": "Člověk", "gender_models": {"0": 49, "1": 50}, "variants": variants}],
            "equipment_slots": [{"id": slot, "name": slot, "inventory_types": []} for slot in SLOTS],
            "equipment": [{"slot": slot, "item_id": 100 + i, "display_id": 500 + i, "label": slot} for i, slot in enumerate(SLOTS)]}


def native(identity=901035):
    return {"agent_id": identity, "spawn_id": identity, "map_id": 725, "name": "Matěj",
            "backstory": "Jsem soused z vesnice.\nPamatuji si dávný déšť.", "personality": "Mluvím česky a klidně."}


def draft(identity="901035"):
    return {"card": {**native(), "agent_id": identity, "spawn_id": identity},
            "appearance": {"race": 1, "gender": 0, "class_id": 1, "skin": 0, "face": 0,
                           "hair_style": 0, "hair_color": 0, "facial_hair": 0, "equipment": []},
            "authoring": {"role": "Sedlák", "values": "Poctivost"}}


PROPOSAL = {"backstory": "Jsem z malé vesnice. Jednou jsem napravil svou chybu.",
            "personality": "Mluvím česky, klidně a přiznám nejistotu.", "suggestions": ["Kdo zná tuto vzpomínku?"]}


def fixture_targets():
    return TargetRegistry(True, "fixture", tuple(
        Target(identity, 725, "Ověřená postava", "worker", "CIVILIAN", 11328, True, "Ověřený řízený cíl.")
        for identity in (901016, 901035, 901036, UINT64_MAX)) + (
            Target(901014, 725, "Městský obyvatel", "resident", "NONE", 11328, False,
                   "NPC 901014 používá běžné herní AI a není registrované pro LLM řízení."),))


class CharacterStudioTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.sources = self.root / "source"
        self.sources.mkdir()
        self.appearances = self.root / "appearances"
        self.appearances.mkdir()
        self.catalog_path = self.root / "catalog.json"
        self.catalog_path.write_text(json.dumps(fixture_catalog()), encoding="utf-8")
        self.calls = []
        self.model_status = 200
        self.proposal = copy.deepcopy(PROPOSAL)

        def handler(request):
            self.calls.append(request)
            if request.method == "GET":
                return httpx.Response(200, json={"npc_model_enabled": True, "npc_model_configured": True})
            return httpx.Response(self.model_status, json=self.proposal)

        self.transport = httpx.MockTransport(handler)
        self.app = self.new_app()
        self.client = TestClient(self.app)
        self.headers = {"X-CSRF-Token": self.client.get("/api/session").json()["csrf_token"]}

    def new_app(self, **extra):
        return create_app(data_dir=self.root / "data", catalog_path=self.catalog_path,
                          source_dir=self.sources, appearance_dir=self.appearances,
                          model_data_dir=self.root / "models", ai_url="http://ai.invalid", ai_transport=self.transport,
                          password=extra.pop("password", ""), target_registry=extra.pop("target_registry", fixture_targets()), **extra)

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def create(self, data=None):
        response = self.client.post("/api/characters", json=data or draft(), headers=self.headers)
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def test_draft_persists_across_app_restart_without_touching_seed(self):
        source = self.sources / "901035.json"
        source.write_text(json.dumps(native(), ensure_ascii=False), encoding="utf-8")
        original = source.read_bytes()
        saved = self.create()
        with TestClient(self.new_app()) as fresh:
            self.assertEqual(fresh.get("/api/characters").json()["characters"], [saved])
        self.assertEqual(source.read_bytes(), original)

    def test_update_requires_latest_revision_and_preserves_creator(self):
        saved = self.create()
        data = draft()
        data["card"]["name"] = "Adam"
        response = self.client.put(f"/api/characters/{saved['id']}", json={"revision": saved["revision"], **data}, headers=self.headers)
        self.assertEqual(response.status_code, 200, response.text)
        current = response.json()
        self.assertEqual(current["revision"], 2)
        self.assertEqual(current["created_at"], saved["created_at"])
        self.assertEqual(current["card"]["name"], "Adam")
        stale = self.client.put(f"/api/characters/{saved['id']}", json={"revision": 1, **draft()}, headers=self.headers)
        self.assertEqual(stale.status_code, 409)
        self.assertEqual(self.client.get(f"/api/characters/{saved['id']}").json(), current)

    def test_delete_requires_current_revision(self):
        saved = self.create()
        path = f"/api/characters/{saved['id']}"
        self.assertEqual(self.client.delete(path + "?revision=2", headers=self.headers).status_code, 409)
        self.assertEqual(self.client.delete(path + "?revision=1", headers=self.headers).status_code, 200)
        self.assertEqual(self.client.get(path).status_code, 404)

    def test_unknown_or_pathlike_identity_cannot_read_or_modify_store(self):
        self.assertEqual(self.client.get("/api/characters/not-an-id").status_code, 404)
        self.assertEqual(self.client.put("/api/characters/not-an-id", json={"revision": 1, **draft()}, headers=self.headers).status_code, 404)

    def test_writes_require_session_token_even_when_origin_absent(self):
        self.assertEqual(self.client.post("/api/characters", json=draft()).status_code, 403)
        self.assertEqual(TestClient(self.app).post("/api/characters", json=draft(), headers=self.headers).status_code, 403)
        self.assertEqual(self.client.post("/api/characters", json=draft(), headers={"X-CSRF-Token": "x"}).status_code, 403)

    def test_cross_origin_and_cross_site_writes_rejected(self):
        for headers in ({"Origin": "http://evil.invalid"}, {"Origin": "null"}, {"Sec-Fetch-Site": "cross-site"}, {"Sec-Fetch-Site": "same-site"}):
            with self.subTest(headers=headers):
                self.assertEqual(self.client.post("/api/characters", json=draft(), headers={**self.headers, **headers}).status_code, 403)
        self.assertEqual(self.client.post("/api/characters", json=draft(), headers={**self.headers, "Origin": "http://testserver"}).status_code, 201)

    def test_session_shared_across_tabs_and_cookie_private(self):
        response = self.client.get("/api/session")
        self.assertEqual(response.json()["csrf_token"], self.headers["X-CSRF-Token"])
        self.assertIn("HttpOnly", response.headers["set-cookie"])
        self.assertIn("SameSite=strict", response.headers["set-cookie"])
        self.assertEqual(response.headers["cache-control"], "no-store")

    def test_optional_basic_auth_covers_drafts_and_assets_but_health_stays_usable(self):
        with TestClient(self.new_app(password="žluťoučké")) as client:
            self.assertEqual(client.get("/health").status_code, 200)
            self.assertEqual(client.get("/api/characters").status_code, 401)
            self.assertEqual(client.get("/static/missing.js").status_code, 401)
            token = base64.b64encode("studio:žluťoučké".encode()).decode()
            self.assertEqual(client.get("/api/characters", headers={"Authorization": "Basic " + token}).status_code, 200)
            bad = base64.b64encode("špatně:x".encode()).decode()
            self.assertEqual(client.get("/api/characters", headers={"Authorization": "Basic " + bad}).status_code, 401)

    def test_native_export_exact_six_fields_and_max_uint64_lossless(self):
        saved = self.create(draft(str(UINT64_MAX)))
        response = self.client.get(f"/api/characters/{saved['id']}/profile")
        self.assertEqual(response.status_code, 200)
        expected = {**native(UINT64_MAX)}
        self.assertEqual(response.json(), expected)
        self.assertEqual(set(response.json()), set(native()))
        self.assertIn(str(UINT64_MAX).encode(), response.content)
        self.assertIn("attachment", response.headers["content-disposition"])
        self.assertEqual(saved["card"]["agent_id"], str(UINT64_MAX))

    def test_export_rejects_native_observe_only_target_without_losing_draft(self):
        saved = self.create(draft("901014"))
        for suffix in ("profile", "appearance", "bundle", "bundle.zip"):
            with self.subTest(suffix=suffix):
                response = self.client.get(f"/api/characters/{saved['id']}/{suffix}")
                self.assertEqual(response.status_code, 422)
                self.assertIn("901014", response.json()["detail"])
        self.assertEqual(self.client.get(f"/api/characters/{saved['id']}").json(), saved)
        self.assertEqual(self.client.get(f"/api/characters/{saved['id']}/draft").status_code, 200)

    def test_reviewed_full_population_enables_existing_native_draft_without_reassigning_or_installing_it(self):
        saved = self.create(draft("901014"))
        path = f"/api/characters/{saved['id']}"
        self.assertEqual(self.client.get(path + "/profile").status_code, 422)
        registry = TargetRegistry.load(Path(__file__).resolve().parents[3], "full-v8")
        self.assertTrue(registry.available, registry.reason)
        with TestClient(self.new_app(target_registry=registry)) as client:
            self.assertEqual(client.get(path).json(), saved)
            self.assertEqual(client.get(path + "/profile").json(), native(901014))
            appearance = client.get(path + "/appearance")
            self.assertEqual(appearance.status_code, 200)
            self.assertEqual((appearance.json()["spawn_id"], appearance.json()["map_id"]), (901014, 725))
            bundle = client.get(path + "/bundle.zip")
            self.assertEqual(bundle.status_code, 200)
            with zipfile.ZipFile(io.BytesIO(bundle.content)) as archive:
                self.assertEqual(json.loads(archive.read("npc_agents/901014.json")), native(901014))
                self.assertEqual(json.loads(archive.read("character-studio-draft.json")), saved)
            self.assertEqual(client.get(path).json(), saved)
        self.assertEqual(list(self.sources.iterdir()), [])
        self.assertEqual(list(self.appearances.iterdir()), [])

    def test_target_validation_distinguishes_unknown_wrong_map_and_known_controlled(self):
        cases = (("901014", 725, False), ("900000", 725, False), ("901035", 0, False),
                 ("901035", 725, True), ("901036", 725, True), ("901016", 725, True))
        for identity, map_id, allowed in cases:
            with self.subTest(identity=identity, map_id=map_id):
                response = self.client.post("/api/validate-target", json={"agent_id": identity,
                    "spawn_id": identity, "map_id": map_id}, headers=self.headers)
                self.assertEqual(response.status_code, 200)
                self.assertEqual(response.json()["eligible"], allowed)
                self.assertTrue(response.json()["reason"])
        self.assertEqual(self.client.post("/api/validate-target", json={"agent_id": "901035",
            "spawn_id": "901035", "map_id": 725}).status_code, 403)

    def test_all_native_downloads_fail_closed_without_population_metadata(self):
        unavailable = TargetRegistry(False, "fields-v8", reason="Metadata populace nejsou ověřená.")
        with TestClient(self.new_app(target_registry=unavailable)) as client:
            token = client.get("/api/session").json()["csrf_token"]
            response = client.post("/api/characters", json=draft(), headers={"X-CSRF-Token": token})
            self.assertEqual(response.status_code, 201)
            saved = response.json()
            for suffix in ("profile", "appearance", "bundle.zip"):
                self.assertEqual(client.get(f"/api/characters/{saved['id']}/{suffix}").status_code, 503)
            self.assertEqual(client.get(f"/api/characters/{saved['id']}/draft").status_code, 200)
            self.assertFalse(client.get("/api/catalog").json()["target_registry"]["available"])

    def test_unknown_target_and_mismatched_map_cannot_download_profile(self):
        for data in (draft("900000"), {**draft(), "card": {**draft()["card"], "map_id": 0}}):
            saved = self.create(data)
            for suffix in ("profile", "appearance", "bundle.zip"):
                self.assertEqual(self.client.get(f"/api/characters/{saved['id']}/{suffix}").status_code, 422)

    def test_catalog_exposes_verified_targets_with_lossless_ids_and_denial_reason(self):
        catalog = self.client.get("/api/catalog").json()
        self.assertTrue(catalog["target_registry"]["available"])
        rows = {row["spawn_id"]: row for row in catalog["targets"]}
        self.assertFalse(rows["901014"]["eligible"])
        self.assertTrue(rows["901035"]["eligible"])
        self.assertIn(str(UINT64_MAX), rows)
        self.assertTrue(rows["901014"]["reason"])

    def test_profile_import_preserves_exact_text_and_numeric_id(self):
        original = native(UINT64_MAX)
        response = self.client.post("/api/import-profile", content=json.dumps(original, ensure_ascii=False), headers={**self.headers, "Content-Type": "application/json"})
        self.assertEqual(response.status_code, 201, response.text)
        saved = response.json()
        self.assertEqual(saved["card"]["agent_id"], str(UINT64_MAX))
        self.assertEqual(saved["card"]["backstory"], original["backstory"])
        self.assertEqual(self.client.get(f"/api/characters/{saved['id']}/profile").json(), original)

    def test_profile_import_rejects_extras_boolean_float_and_mismatch(self):
        cases = [{**native(), "appearance": {}}, {**native(), "agent_id": True},
                 {**native(), "agent_id": 1.0}, {**native(), "agent_id": 999},
                 {**native(), "spawn_id": "901035"}, {**native(), "name": " "}]
        for value in cases:
            with self.subTest(value=value):
                self.assertEqual(self.client.post("/api/import-profile", json=value, headers=self.headers).status_code, 422)
        self.assertEqual(self.client.get("/api/characters").json()["characters"], [])

    def test_incomplete_draft_can_save_but_cannot_export(self):
        saved = self.create({"appearance": draft()["appearance"]})
        for suffix in ("profile", "appearance", "bundle.zip"):
            self.assertEqual(self.client.get(f"/api/characters/{saved['id']}/{suffix}").status_code, 422)

    def test_bundle_contains_independent_native_files_and_editable_draft(self):
        data = draft()
        data["appearance"]["equipment"] = [{"slot": "head", "item_id": 100, "display_id": 500}]
        saved = self.create(data)
        response = self.client.get(f"/api/characters/{saved['id']}/bundle.zip")
        self.assertEqual(response.status_code, 200)
        standalone = self.client.get(f"/api/characters/{saved['id']}/appearance")
        self.assertEqual(standalone.status_code, 200)
        self.assertEqual(standalone.headers["content-disposition"], 'attachment; filename="901035.json"')
        with zipfile.ZipFile(io.BytesIO(response.content)) as bundle:
            self.assertEqual(set(bundle.namelist()), {"npc_agents/901035.json", "npc_appearances/901035.json", "character-studio-draft.json", "README.txt"})
            self.assertEqual(json.loads(bundle.read("npc_agents/901035.json")), native())
            sidecar = json.loads(bundle.read("npc_appearances/901035.json"))
            self.assertEqual(set(sidecar), {"schema_version", "spawn_id", "map_id", "appearance"})
            self.assertEqual(sidecar["appearance"], data["appearance"])
            self.assertEqual(json.loads(bundle.read("character-studio-draft.json")), saved)

    def test_draft_backup_import_preserves_appearance_but_assigns_fresh_storage_identity(self):
        data = draft()
        data["appearance"]["gender"] = 1
        data["authoring"]["fears"] = "Ztratit důvěru sousedů."
        saved = self.create(data)
        response = self.client.get(f"/api/characters/{saved['id']}/draft")
        self.assertEqual(response.status_code, 200)
        imported = self.client.post("/api/import-draft", json=response.json(), headers=self.headers)
        self.assertEqual(imported.status_code, 201, imported.text)
        value = imported.json()
        self.assertNotEqual(value["id"], saved["id"])
        self.assertEqual(value["revision"], 1)
        for key in ("card", "appearance", "authoring"):
            self.assertEqual(value[key], saved[key])
        bad = self.client.post("/api/import-draft", json={**response.json(), "exec": "x"}, headers=self.headers)
        self.assertEqual(bad.status_code, 422)

    def test_catalog_real_options_seeds_and_safe_model_flags(self):
        (self.sources / "901035.json").write_text(json.dumps(native()), encoding="utf-8")
        response = self.client.get("/api/catalog")
        self.assertEqual(response.status_code, 200, response.text)
        data = response.json()
        self.assertTrue(data["model"]["available"])
        self.assertFalse(data["preview"]["available"])
        self.assertEqual(data["source_profiles"][0]["card"]["name"], "Matěj")
        self.assertEqual(data["source_profiles"][0]["appearance_source"], "default")
        self.assertNotIn("api_key", response.text.lower())
        self.assertNotIn("ai.invalid", response.text)

    def test_source_appearance_is_used_only_with_matching_identity(self):
        (self.sources / "901035.json").write_text(json.dumps(native()), encoding="utf-8")
        appearance = draft()["appearance"]
        appearance["gender"] = 1
        (self.appearances / "901035.json").write_text(json.dumps({"schema_version": 1, "spawn_id": 901035, "map_id": 725, "appearance": appearance}), encoding="utf-8")
        seed = self.client.get("/api/catalog").json()["source_profiles"][0]
        self.assertEqual(seed["appearance_source"], "source")
        self.assertEqual(seed["appearance"]["gender"], 1)

    def test_invalid_sidecar_keeps_source_card_with_explicit_default_look(self):
        (self.sources / "901035.json").write_text(json.dumps(native()), encoding="utf-8")
        (self.appearances / "901035.json").write_text("not-json", encoding="utf-8")
        seed = self.client.get("/api/catalog").json()["source_profiles"][0]
        self.assertEqual(seed["card"]["name"], "Matěj")
        self.assertEqual(seed["appearance_source"], "default")

    def test_preview_availability_requires_completed_native_asset_manifest(self):
        (self.root / "models" / "meta").mkdir(parents=True)
        self.assertFalse(self.client.get("/api/catalog").json()["preview"]["available"])
        (self.root / "models" / "manifest.json").write_text("{}", encoding="utf-8")
        self.assertTrue(self.client.get("/api/catalog").json()["preview"]["available"])

    def test_appearance_checks_actual_dbc_combinations_and_gear_triples(self):
        invalid = [{"race": 9}, {"gender": True}, {"class_id": 2}, {"skin": 2}, {"skin": 1},
                   {"hair_style": 1}, {"facial_hair": 1},
                   {"equipment": [{"slot": "head", "item_id": 100, "display_id": 999}]},
                   {"equipment": [{"slot": "head", "item_id": 100, "display_id": 500}] * 2}]
        for changes in invalid:
            data = draft()
            data["appearance"].update(changes)
            with self.subTest(changes=changes):
                self.assertEqual(self.client.post("/api/characters", json=data, headers=self.headers).status_code, 422)
        valid = draft()
        valid["appearance"].update(skin=1, face=1, hair_style=1, hair_color=1, facial_hair=1)
        self.create(valid)

    def test_plain_text_character_and_utf8_limits_reject_unsafe_card(self):
        for field, value in (("name", "a" * 33), ("name", "\u202eAdam"), ("name", "Adam|cffff"),
                             ("backstory", "x\rtest"), ("personality", "ž" * 1025)):
            data = draft()
            data["card"][field] = value
            self.assertEqual(self.client.post("/api/characters", json=data, headers=self.headers).status_code, 422)
        data = draft()
        data["card"]["backstory"] = "První\r\nDruhý"
        self.assertEqual(self.create(data)["card"]["backstory"], "První\nDruhý")

    def test_json_duplicate_key_oversize_wrong_content_type_and_unknown_field(self):
        self.assertEqual(self.client.post("/api/characters", content='{"card":{},"card":{}}', headers={**self.headers, "Content-Type": "application/json"}).status_code, 422)
        self.assertEqual(self.client.post("/api/characters", content=b" " * 65537, headers={**self.headers, "Content-Type": "application/json"}).status_code, 413)
        self.assertEqual(self.client.post("/api/characters", content="{}", headers=self.headers).status_code, 415)
        self.assertEqual(self.client.post("/api/characters", json={**draft(), "exec": "x"}, headers=self.headers).status_code, 422)

    def test_catalog_unavailable_fails_closed_before_creating_draft(self):
        self.catalog_path.unlink()
        self.assertEqual(self.client.get("/api/catalog").status_code, 503)
        self.assertEqual(self.client.post("/api/characters", json=draft(), headers=self.headers).status_code, 503)
        self.assertEqual(self.client.get("/api/characters").json()["characters"], [])

    def test_generated_text_is_proposal_only_and_keeps_identity_out_of_provider(self):
        saved = self.create()
        data = draft()
        data["appearance"]["equipment"] = [{"slot": "head", "item_id": 100, "display_id": 500}]
        response = self.client.post("/api/generate", json={**data, "instruction": "Rozveď společnou vzpomínku."}, headers=self.headers)
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json(), PROPOSAL)
        self.assertEqual(self.client.get(f"/api/characters/{saved['id']}").json(), saved)
        sent = json.loads(self.calls[-1].content)
        self.assertEqual(set(sent["profile"]), {"name", "backstory", "personality"})
        self.assertEqual(sent["appearance"]["equipment"], [])
        self.assertNotIn("authorization", self.calls[-1].headers)

    def test_generation_error_statuses_do_not_become_fake_proposals(self):
        for status in (429, 503, 504, 502):
            self.model_status = status
            response = self.client.post("/api/generate", json=draft(), headers=self.headers)
            self.assertEqual(response.status_code, status)
            self.assertNotIn("backstory", response.json())
        self.assertEqual(self.client.get("/api/characters").json()["characters"], [])

    def test_generation_malformed_or_oversized_card_rejected(self):
        for proposal in ({**PROPOSAL, "extra": True}, {**PROPOSAL, "personality": "ž" * 1025},
                         {**PROPOSAL, "backstory": " "}, {**PROPOSAL, "suggestions": ["x"] * 5}):
            self.proposal = proposal
            self.assertEqual(self.client.post("/api/generate", json=draft(), headers=self.headers).status_code, 502)


if __name__ == "__main__":
    unittest.main()
