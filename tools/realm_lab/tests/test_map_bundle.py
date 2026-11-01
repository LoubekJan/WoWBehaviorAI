"""Empty-map revisions remain byte-pinned without relying on retired NPC homes."""
import hashlib
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import tempfile
import shutil
import subprocess
import sys
import unittest
from unittest.mock import patch

from tools.realm_lab import authoring_empty, graveyard, manage, map_bundle, settlement_population, world_setup
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_world_setup import before_rows, after_rows
from tools.realm_lab.tests.test_graveyard import fixture as safe_locations_fixture, table


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, allow_nan=False), encoding="utf-8")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(root, revision=map_bundle.REVISION):
    metadata, data = root / "metadata", root / "data"
    directory = metadata / revision
    files = []
    names = sorted(map_bundle.DBC_FILES | map_bundle.MAP_FILES | map_bundle.MMAP_FILES |
                   {"vmaps/725.vmtree", "vmaps/725_31_32.vmtile", "vmaps/mine.wmo.vmo"})
    for name in names:
        content = ("native-fixture:" + name).encode()
        target = data / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        files.append({"path": name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
    base = dict(schema_version=1, source_revision=revision, map_id=725, area_id=4988,
                tiles=[list(tile) for tile in map_bundle.TILES])
    manifest_sha = write(directory / "server-manifest.json", {**base, "files": files})
    vmap = {"present": True, "models": [], "tiles": []}
    for row in files:
        item = {**row, "file": row["path"].split("/")[-1]}
        if row["path"].endswith(".vmtree"):
            vmap["tree"] = item
        elif row["path"].endswith(".vmtile"):
            vmap["tiles"].append(item)
        elif row["path"].endswith(".vmo"):
            vmap["models"].append(item)
    native = {"maps": [], "mmtiles": []}
    for row in files:
        item = {**row, "file": row["path"].split("/")[-1]}
        if row["path"].endswith(".map"):
            native["maps"].append(item)
        elif row["path"].endswith(".mmtile"):
            native["mmtiles"].append(item)
        elif row["path"].endswith(".mmap"):
            native["mmap_parameters"] = item
    proof_sha = write(directory / "data-verification.json", {**base, "ok": True, "status": "pass",
        "errors": [], "navigation_checked": True, "geometry_files": files, "vmap": vmap, **native})
    evidence = {"dry": True, "outside_native_and_source_terrain_holes": True,
        "slope_degrees": 1.5, "outside_collision_bounds_margin": 5,
        "nav_ground_projection": {"flags": 1, "ground_delta": .2, "nav_z": 2.4},
        "physical_player_walk_verified": False}
    point = {"map": 725, "x": 266.667, "y": 800., "z": 4.2, "ground_z": 2.2}
    entry_sha = write(directory / "player-entry.json", {"source_revision": revision,
        "ok": True, "status": "pass", "bounds": settlement_population.BOUNDS,
        "verification_sha256": proof_sha, "player_entry": point, "evidence": evidence})
    index = {"schema_version": 1, "profile": "authoring-empty", "realm_id": 2, "map_id": 725,
        "area_id": 4988, "source_revision": revision, "server_manifest_sha256": manifest_sha,
        "verification_sha256": proof_sha, "entry_proof_sha256": entry_sha}
    write(metadata / map_bundle.INDEX, index)
    write(directory / "world-start.json", {"schema_version": 1, "source_revision": revision,
        "map_id": 725, "area_id": 4988, "x": point["x"], "y": point["y"], "z": point["z"],
        "orientation": 0, "geometry_sha256": world_setup.geometry_digest(files),
        "source_entry_receipt_sha256": entry_sha, "evidence": {"ground_z": point["ground_z"],
            "dry": True, "outside_collision_bounds": True, "outside_terrain_holes": True,
            "slope_degrees": 1.5, "nav_ground_projection": evidence["nav_ground_projection"]}})
    location = json.loads(graveyard.MANIFEST.read_text(encoding="utf-8"))
    location.update(profile="authoring-empty", position={k: point[k] for k in ("x", "y", "z", "ground_z")})
    location["source"] = {"revision": revision, "proof_sha256": entry_sha,
                          "geometry_verification_sha256": proof_sha, "evidence": evidence}
    write(directory / "graveyard.json", location)
    return metadata, data


class AuthoredMapBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.metadata, self.data = fixture(self.root)

    def repin(self, name, key, mutation):
        path = self.metadata / map_bundle.REVISION / name
        document = json.loads(path.read_text(encoding="utf-8"))
        mutation(document)
        digest = write(path, document)
        index_path = self.metadata / map_bundle.INDEX
        index = json.loads(index_path.read_text(encoding="utf-8"))
        index[key] = digest
        write(index_path, index)

    def test_variable_native_model_closure_requires_no_npc_or_sixty_one_file_assumption(self):
        bundle = map_bundle.verify_data(self.data, self.metadata)
        self.assertEqual(bundle.actors, ())
        self.assertEqual(bundle.source_revision, "preview-v9")
        self.assertEqual(len(bundle.geometry_files), 15)
        for row in bundle.geometry_files:
            original = (self.data / row["path"]).read_bytes()
            (self.data / row["path"]).write_bytes(original + b"changed")
            with self.subTest(path=row["path"]), self.assertRaisesRegex(settlement_population.ProfileError, "hash mismatch"):
                map_bundle.verify_data(self.data, self.metadata)
            (self.data / row["path"]).write_bytes(original)

    def test_reviewed_v10_uses_its_own_geometry_start_and_graveyard(self):
        metadata, data = fixture(self.root / "v10", revision="preview-v10")
        bundle = map_bundle.verify_data(data, metadata)
        self.assertEqual(bundle.source_revision, "preview-v10")
        self.assertEqual(bundle.metadata, metadata / "preview-v10")
        self.assertEqual(bundle.actors, ())
        start = world_setup.WorldStart.load(bundle)
        location = graveyard.Graveyard.load(bundle.metadata / "graveyard.json")
        self.assertEqual((start.x, start.y, start.z), (location.x, location.y, location.z))
        with patch.object(map_bundle, "METADATA", metadata), \
             patch.object(map_bundle.GeometryBundle, "load", return_value=bundle), \
             patch.dict(os.environ, {"LAB_AI_PROFILE": "authoring-empty"}):
            self.assertEqual(graveyard.Graveyard.load(), location)
        (data / "mmaps/7253130.mmtile").write_bytes(b"changed native navigation")
        with self.assertRaisesRegex(settlement_population.ProfileError, "hash mismatch"):
            map_bundle.verify_data(data, metadata)

    def test_unsupported_revision_is_rejected_before_loading_revision_files(self):
        for revision in ("preview-v11", "../preview-v10", "preview-v10/../../other"):
            index_path = self.metadata / map_bundle.INDEX
            index = json.loads(index_path.read_text(encoding="utf-8"))
            index["source_revision"] = revision
            write(index_path, index)
            with self.subTest(revision=revision), patch.object(map_bundle, "_pinned") as pinned:
                with self.assertRaisesRegex(settlement_population.ProfileError, "unsupported authored revision"):
                    map_bundle.GeometryBundle.load(self.metadata)
                pinned.assert_not_called()

    def test_v10_cannot_repin_mixed_revision_native_or_entry_proofs(self):
        records = (("server-manifest.json", "server_manifest_sha256", "bundle identity"),
                   ("data-verification.json", "verification_sha256", "mismatched native"),
                   ("player-entry.json", "entry_proof_sha256", "another native geometry"))
        for name, pin, message in records:
            metadata, _ = fixture(self.root / name, revision="preview-v10")
            index_path = metadata / map_bundle.INDEX
            index = json.loads(index_path.read_text(encoding="utf-8"))
            path = metadata / "preview-v10" / name
            document = json.loads(path.read_text(encoding="utf-8"))
            document["source_revision"] = "preview-v9"
            index[pin] = write(path, document)
            write(index_path, index)
            with self.subTest(proof=name), self.assertRaisesRegex(settlement_population.ProfileError, message):
                map_bundle.GeometryBundle.load(metadata)

    def test_v10_start_and_graveyard_cannot_use_v9_revision_labels(self):
        metadata, data = fixture(self.root / "v10-mixed-start", revision="preview-v10")
        bundle = map_bundle.verify_data(data, metadata)
        path = bundle.metadata / "world-start.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        document["source_revision"] = "preview-v9"
        write(path, document)
        with self.assertRaisesRegex(manage.SetupError, "reviewed map"):
            world_setup.WorldStart.load(bundle)
        path = bundle.metadata / "graveyard.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        document["source"]["revision"] = "preview-v9"
        write(path, document)
        with self.assertRaisesRegex(ValueError, "pinned native safe point"):
            graveyard.Graveyard.load(path)

    def test_legacy_tiles_and_extra_navigation_files_are_rejected(self):
        for name in ("maps/7253330.map", "mmaps/7253330.mmtile", "vmaps/725_30_33.vmtile"):
            target = self.data / name
            target.write_bytes(b"stale")
            with self.subTest(name=name), self.assertRaisesRegex(settlement_population.ProfileError, "footprint"):
                map_bundle.verify_data(self.data, self.metadata)
            target.unlink()

    def test_incomplete_vmap_model_proof_is_not_repaired_by_repinning_the_document(self):
        self.repin("data-verification.json", "verification_sha256", lambda p: p["vmap"]["models"].clear())
        with self.assertRaisesRegex(settlement_population.ProfileError, "closure"):
            map_bundle.GeometryBundle.load(self.metadata)

    def test_map_identity_path_escape_and_incomplete_manifest_fail_before_live_file_reads(self):
        for mutation in (lambda p: p.update(map_id=0),
                         lambda p: p["files"][0].update(path="dbc/../../secret"),
                         lambda p: p["files"].pop()):
            metadata, _ = fixture(self.root / str(id(mutation)))
            path = metadata / map_bundle.REVISION / "server-manifest.json"
            document = json.loads(path.read_text(encoding="utf-8"))
            mutation(document)
            index_path = metadata / map_bundle.INDEX
            index = json.loads(index_path.read_text(encoding="utf-8"))
            index["server_manifest_sha256"] = write(path, document)
            write(index_path, index)
            with self.assertRaises(settlement_population.ProfileError):
                map_bundle.GeometryBundle.load(metadata)

    def test_existing_v8_geometry_is_not_silently_used_when_v9_index_is_corrupt(self):
        (self.metadata / map_bundle.INDEX).write_text("{}", encoding="utf-8")
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": "authoring-empty"})
        with patch.object(map_bundle, "METADATA", self.metadata), \
             patch.object(settlement_population, "verify_data") as legacy, \
             patch.object(authoring_empty, "verify_empty") as empty:
            # Bind the explicit fixture root, independent of Python default arguments.
            with patch.object(authoring_empty.map_bundle, "verify_data", side_effect=lambda data: map_bundle.GeometryBundle.load(self.metadata)):
                with self.assertRaises(settlement_population.ProfileError):
                    authoring_empty.preflight(self.data, settings)
            legacy.assert_not_called()
            empty.assert_not_called()

    def test_authoring_start_and_graveyard_match_the_same_pinned_native_point(self):
        bundle = map_bundle.verify_data(self.data, self.metadata)
        start = world_setup.WorldStart.load(bundle)
        location = graveyard.Graveyard.load(bundle.metadata / "graveyard.json")
        self.assertEqual((start.x, start.y, start.z), (location.x, location.y, location.z))
        with patch.object(map_bundle, "METADATA", self.metadata), \
             patch.object(map_bundle.GeometryBundle, "load", return_value=bundle), \
             patch.dict(os.environ, {"LAB_AI_PROFILE": "authoring-empty"}):
            self.assertEqual(graveyard.Graveyard.load(), location)
        bad = json.loads((bundle.metadata / "world-start.json").read_text(encoding="utf-8"))
        bad["x"] += 1
        write(bundle.metadata / "world-start.json", bad)
        with self.assertRaisesRegex(manage.SetupError, "pinned native safe point"):
            world_setup.WorldStart.load(bundle)
        bad = json.loads((bundle.metadata / "graveyard.json").read_text(encoding="utf-8"))
        bad["source"]["proof_sha256"] = "0" * 64
        write(bundle.metadata / "graveyard.json", bad)
        with self.assertRaisesRegex(ValueError, "pinned native safe point"):
            graveyard.Graveyard.load(bundle.metadata / "graveyard.json")

    def test_authoring_boot_requires_the_matching_installed_graveyard_before_empty_sql(self):
        bundle = map_bundle.verify_data(self.data, self.metadata)
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": "authoring-empty"})
        with patch.object(authoring_empty, "geometry", return_value=bundle), \
             patch.object(graveyard, "verify_data", side_effect=ValueError("wrong safe location")) as installed, \
             patch.object(authoring_empty, "verify_empty") as sql:
            with self.assertRaisesRegex(settlement_population.ProfileError, "start/graveyard"):
                authoring_empty.preflight(self.data, settings)
            installed.assert_called_once()
            sql.assert_not_called()

    def test_cli_in_authoring_container_builds_the_v9_graveyard_instead_of_v8(self):
        bundle = map_bundle.GeometryBundle.load(self.metadata)
        base = self.root / "base.dbc"
        base.write_bytes(safe_locations_fixture())
        row = [0] * 36
        row[:3] = [4988, 725, 0]
        (self.data / "dbc/AreaTable.dbc").write_bytes(table([row], b"\0", 36))
        client = self.root / "patch-X.MPQ"
        arguments = ["graveyard.py", "build-server", str(self.data), "--base", str(base), "--client-patch", str(client)]
        with patch.object(map_bundle, "METADATA", self.metadata), \
             patch.object(map_bundle.GeometryBundle, "load", return_value=bundle), \
             patch.dict(os.environ, {"LAB_AI_PROFILE": "authoring-empty"}), \
             patch.object(sys, "argv", arguments), redirect_stdout(io.StringIO()):
            graveyard.main()
        location = graveyard.Graveyard.load(bundle.metadata / "graveyard.json")
        graveyard.verify_world_safe_locs((self.data / "dbc/WorldSafeLocs.dbc").read_bytes(), location)
        self.assertAlmostEqual(location.z, 4.2)
        self.assertTrue(client.is_file())

    @unittest.skipUnless(shutil.which("make"), "Make invocation contract runs where GNU Make is installed")
    def test_make_graveyard_uses_compose_profile_and_root_managed_absolute_outputs(self):
        executable = self.root / "bin/docker"
        executable.parent.mkdir()
        capture = self.root / "invocation.json"
        executable.write_text(f"#!{sys.executable}\nimport json,os,sys\n"
            "from pathlib import Path\n"
            "Path(os.environ['CAPTURE']).write_text(json.dumps(sys.argv[1:]))\n", encoding="utf-8")
        executable.chmod(0o755)
        private_env = self.root / "chosen.env"
        private_env.write_text("LAB_AI_PROFILE=authoring-empty\n", encoding="utf-8")
        env = {**os.environ, "PATH": str(executable.parent) + os.pathsep + os.environ["PATH"], "CAPTURE": str(capture)}
        env.pop("LAB_AI_PROFILE", None)
        result = subprocess.run([shutil.which("make"), "-f", str(map_bundle.ROOT / "Makefile"),
            "graveyard-data", "WORLD_ENV_FILE=" + str(private_env)], cwd=self.root, env=env,
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = json.loads(capture.read_text(encoding="utf-8"))
        self.assertEqual(argv[:5], ["compose", "--env-file", str(private_env), "-f", "compose.yml"])
        self.assertEqual(argv[argv.index("--user") + 1], "0:0")
        self.assertIn("--interactive=false", argv)
        self.assertEqual(argv[argv.index("tc-dev") + 1:argv.index("build-server")],
                         ["python3", "/workspace/tools/realm_lab/graveyard.py"])
        self.assertEqual(argv[argv.index("build-server") + 1:], ["/workspace/runtime/lab/data", "--base",
            "/workspace/runtime/lab/dbc-base/WorldSafeLocs.dbc", "--client-patch",
            "/workspace/runtime/lab/client-patch/patch-X.MPQ"])

    def test_empty_start_policy_uses_new_geometry_without_loading_old_population(self):
        bundle = map_bundle.verify_data(self.data, self.metadata)
        start = world_setup.WorldStart.load(bundle)
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": "authoring-empty"})
        receipt = self.root / "start-receipt.json"
        with patch.object(settlement_population, "ensure_world_offline"), \
             patch.object(manage, "authoring_preflight", return_value=bundle), \
             patch.object(settlement_population, "verify_data") as old_population, \
             patch.object(world_setup, "starts_snapshot", side_effect=[before_rows(), after_rows(start)]), \
             patch.object(manage, "lab_mysql") as sql:
            world_setup.apply_start(self.data, receipt, settings)
        old_population.assert_not_called()
        self.assertEqual(json.loads(receipt.read_text(encoding="utf-8"))["source_revision"], "preview-v9")
        self.assertIn("UPDATE world.playercreateinfo", sql.call_args.args[1])
        self.assertNotIn("characters.", sql.call_args.args[1])


if __name__ == "__main__":
    unittest.main()
