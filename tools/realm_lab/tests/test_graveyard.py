"""Custom-world corpse-run routing and one-file client/server DBC contract."""
from dataclasses import replace
from io import BytesIO
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

try:
    from mpyq import MPQArchive
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "runtime/lab-map-deps"))
    from mpyq import MPQArchive

from tools.realm_lab import graveyard, manage


def table(rows, strings=b"\0Old location\0", fields=22):
    return (struct.pack("<4s4I", b"WDBC", len(rows), fields, fields * 4, len(strings)) +
            b"".join(struct.pack(f"<{fields}I", *row) for row in rows) + strings)


def fixture():
    first = [4, 0, *struct.unpack("<3I", struct.pack("<3f", -1000.5, 65.0, 8.25)), 1, *([0] * 15), 16712190]
    second = [1720, 571, *struct.unpack("<3I", struct.pack("<3f", 100.0, -200.0, 30.0)), 1, *([0] * 15), 16712190]
    return table([first, second])


class GraveyardTests(unittest.TestCase):
    def setUp(self):
        self.location = graveyard.Graveyard.load()

    def test_checked_in_manifest_uses_reviewed_dry_point_without_claiming_physical_recovery(self):
        document = json.loads(graveyard.MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual((self.location.safe_location_id, self.location.map_id,
                          self.location.zone_id, self.location.team), (9000, 725, 4988, 0))
        self.assertEqual((self.location.x, self.location.y), (266.667, 800.0))
        self.assertAlmostEqual(self.location.z - document["position"]["ground_z"], 2.0)
        self.assertFalse(document["source"]["evidence"]["physical_player_walk_verified"])
        self.assertEqual(document["recovery"], {"mode": "ghost_corpse_run", "spirit_healer_spawn": False,
                                              "physical_death_release_and_corpse_reclaim_verified": False})

    def test_append_preserves_original_records_strings_and_float_coordinates(self):
        original = fixture()
        patched = graveyard.patch_world_safe_locs(original, self.location)
        magic, count, fields, size, string_size = struct.unpack_from("<4s4I", patched)
        self.assertEqual((magic, count, fields, size), (b"WDBC", 3, 22, 88))
        self.assertEqual(patched[20:20 + 2 * 88], original[20:20 + 2 * 88])
        old_strings = original[20 + 2 * 88:]
        self.assertEqual(patched[20 + 3 * 88:20 + 3 * 88 + len(old_strings)], old_strings)
        self.assertEqual(string_size, len(old_strings) + len(self.location.name) + 1)
        self.assertEqual(struct.unpack_from("<IIfff", patched, 20 + 2 * 88),
                         (9000, 725, *struct.unpack("<3f", struct.pack("<3f", self.location.x,
                                                                       self.location.y, self.location.z))))
        graveyard.verify_world_safe_locs(patched, self.location)

    def test_repeated_generation_is_byte_identical(self):
        patched = graveyard.patch_world_safe_locs(fixture(), self.location)
        self.assertEqual(graveyard.patch_world_safe_locs(patched, self.location), patched)
        self.assertEqual(graveyard.build_client_patch(fixture(), self.location),
                         graveyard.build_client_patch(patched, self.location))

    def test_reserved_id_collision_or_higher_input_id_is_not_overwritten(self):
        original = bytearray(fixture())
        struct.pack_into("<I", original, 20, 9000)
        with self.assertRaisesRegex(ValueError, "conflicts"):
            graveyard.patch_world_safe_locs(bytes(original), self.location)
        struct.pack_into("<I", original, 20, 9001)
        with self.assertRaisesRegex(ValueError, "above all existing"):
            graveyard.patch_world_safe_locs(bytes(original), self.location)
        patched = bytearray(graveyard.patch_world_safe_locs(fixture(), self.location))
        struct.pack_into("<f", patched, 20 + 2 * 88 + 8, self.location.x + 1)
        with self.assertRaisesRegex(ValueError, "conflicts"):
            graveyard.patch_world_safe_locs(bytes(patched), self.location)

    def test_malformed_dbc_and_duplicate_ids_are_rejected(self):
        for content in (b"", fixture()[:-1], fixture() + b"unexpected"):
            with self.subTest(size=len(content)), self.assertRaises(ValueError):
                graveyard.patch_world_safe_locs(content, self.location)
        invalid = bytearray(fixture())
        struct.pack_into("<I", invalid, 20 + 88, 4)
        with self.assertRaisesRegex(ValueError, "Duplicate DBC IDs"):
            graveyard.patch_world_safe_locs(bytes(invalid), self.location)
        struct.pack_into("<I", invalid, 20 + 88, 1720)
        struct.pack_into("<I", invalid, 20 + 5 * 4, 999999)
        with self.assertRaisesRegex(ValueError, "localized name offset"):
            graveyard.patch_world_safe_locs(bytes(invalid), self.location)
        with self.assertRaisesRegex(ValueError, "existing locations"):
            graveyard.patch_world_safe_locs(table([]), self.location)

    def test_independent_mpq_reader_sees_exactly_the_world_safe_locs_overlay(self):
        archive_bytes, receipt = graveyard.build_client_patch(fixture(), self.location)
        archive = MPQArchive(BytesIO(archive_bytes))
        try:
            self.assertEqual(set(archive.files), {graveyard.ARCHIVE_MEMBER.encode("ascii"), b"(listfile)"})
            dbc = archive.read_file(graveyard.ARCHIVE_MEMBER)
            self.assertEqual(dbc, graveyard.patch_world_safe_locs(fixture(), self.location))
            self.assertEqual(receipt["dbc_bytes"], len(dbc))
            self.assertEqual(receipt["patch_bytes"], len(archive_bytes))
            self.assertIsNone(archive.read_file(r"DBFilesClient\Map.dbc"))
            self.assertIsNone(archive.read_file(r"DBFilesClient\FactionTemplate.dbc"))
            self.assertIsNone(archive.read_file(r"World\Maps\AIWorldLab\AIWorldLab_30_31.adt"))
        finally:
            archive.file.close()

    def test_root_zone_validation_rejects_subareas_or_other_maps(self):
        row = [0] * 36
        row[:3] = [4988, 725, 0]
        graveyard.verify_root_zone(table([row], b"\0", 36), self.location)
        for identity in ((4988, 725, 12), (4988, 0, 0), (12, 725, 0)):
            row[:3] = identity
            with self.subTest(identity=identity), self.assertRaisesRegex(ValueError, "installed root zone"):
                graveyard.verify_root_zone(table([row], b"\0", 36), self.location)

    def test_link_validation_accepts_exact_route_or_explicitly_empty_bootstrap(self):
        expected = {"id": 9000, "zone": 4988, "team": 0, "comment": self.location.comment}
        graveyard.validate_links([expected], self.location)
        graveyard.validate_links([], self.location, allow_absent=True)
        for rows in ([], [expected, expected], [{**expected, "team": 469}], [{**expected, "zone": 12}],
                     [{**expected, "id": 4}], [{**expected, "comment": "other"}]):
            with self.subTest(rows=rows), self.assertRaisesRegex(ValueError, "routing"):
                graveyard.validate_links(rows, self.location)

    def test_bootstrap_only_inserts_its_reserved_zone_link_and_handles_quoted_comment(self):
        sql = graveyard.bootstrap_sql(replace(self.location, comment="Lab's cemetery"))
        self.assertIn("9000,4988,0,'Lab''s cemetery'", sql)
        self.assertIn("WHERE ID=9000 OR GhostZone=4988", sql)
        self.assertIn("WHERE ID=9000 OR GhostZone=4988", graveyard.snapshot_sql(self.location))
        for forbidden in ("DELETE", "UPDATE", "creature", "characters.", "auth."):
            self.assertNotIn(forbidden, sql)

    def test_manifest_rejects_unsafe_location_or_missing_native_evidence(self):
        original = json.loads(graveyard.MANIFEST.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory(dir=graveyard.MANIFEST.parents[3]) as directory:
            path = Path(directory) / "graveyard.json"
            for field, value in (("x", float("nan")), ("y", -1.0), ("z", -20)):
                bad = {**original, "position": {**original["position"], field: value}}
                path.write_text(json.dumps(bad), encoding="utf-8")
                with self.subTest(field=field), self.assertRaisesRegex(ValueError, "position"):
                    graveyard.Graveyard.load(path)
            original["source"]["evidence"]["dry"] = False
            path.write_text(json.dumps(original), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "evidence"):
                graveyard.Graveyard.load(path)

    def test_private_native_proof_must_match_its_hash_and_the_published_point(self):
        document = json.loads(graveyard.MANIFEST.read_text(encoding="utf-8"))
        source = {"source_revision": document["source"]["revision"], "ok": True, "status": "pass",
                  "bounds": graveyard.BOUNDS, "player_entry": {"map": 725, **document["position"]},
                  "evidence": document["source"]["evidence"],
                  "verification_sha256": document["source"]["geometry_verification_sha256"]}
        proof = json.dumps(source).encode("utf-8")
        document["source"]["proof_sha256"] = hashlib.sha256(proof).hexdigest()
        with tempfile.TemporaryDirectory(dir=graveyard.MANIFEST.parents[3]) as directory:
            path = Path(directory) / "graveyard.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            graveyard.verify_point_proof(proof, path)
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                graveyard.verify_point_proof(proof + b" ", path)
            document["position"]["x"] += 1
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "published location"):
                graveyard.verify_point_proof(proof, path)

    def disk_fixture(self, directory):
        root = Path(directory)
        data, base, client = root / "data", root / "dbc-base/WorldSafeLocs.dbc", root / "client-patch/patch-X.MPQ"
        (data / "dbc").mkdir(parents=True)
        base.parent.mkdir(parents=True)
        base.write_bytes(fixture())
        row = [0] * 36
        row[:3] = [4988, 725, 0]
        (data / "dbc/AreaTable.dbc").write_bytes(table([row], b"\0", 36))
        return data, base, client

    def test_disk_generation_preserves_base_upgrades_known_input_and_then_is_a_noop(self):
        with tempfile.TemporaryDirectory(dir=graveyard.MANIFEST.parents[3]) as directory:
            data, base, client = self.disk_fixture(directory)
            output = data / "dbc/WorldSafeLocs.dbc"
            output.write_bytes(base.read_bytes())
            first = graveyard.build_server(data, base, client, self.location)
            self.assertTrue(first["server_written"] and first["client_patch_written"])
            self.assertEqual(base.read_bytes(), fixture())
            graveyard.verify_data(data, self.location)
            timestamps = (output.stat().st_mtime_ns, client.stat().st_mtime_ns)
            second = graveyard.build_server(data, base, client, self.location)
            self.assertFalse(second["server_written"] or second["client_patch_written"])
            self.assertEqual((output.stat().st_mtime_ns, client.stat().st_mtime_ns), timestamps)

    def test_conflicting_staged_patch_or_server_dbc_prevents_both_output_writes(self):
        for conflicting in ("client", "server"):
            with self.subTest(conflicting=conflicting), \
                    tempfile.TemporaryDirectory(dir=graveyard.MANIFEST.parents[3]) as directory:
                data, base, client = self.disk_fixture(directory)
                output = data / "dbc/WorldSafeLocs.dbc"
                target = client if conflicting == "client" else output
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"foreign content")
                with self.assertRaisesRegex(ValueError, "Existing graveyard output differs"):
                    graveyard.build_server(data, base, client, self.location)
                self.assertEqual(target.read_bytes(), b"foreign content")
                self.assertFalse((output if conflicting == "client" else client).exists())
                self.assertEqual(base.read_bytes(), fixture())

    def test_disk_generation_refuses_to_replace_the_vanilla_base_in_place(self):
        with tempfile.TemporaryDirectory(dir=graveyard.MANIFEST.parents[3]) as directory:
            data, base, client = self.disk_fixture(directory)
            output = data / "dbc/WorldSafeLocs.dbc"
            output.write_bytes(fixture())
            with self.assertRaisesRegex(ValueError, "immutable graveyard base"):
                graveyard.build_server(data, output, client, self.location)
            self.assertEqual(output.read_bytes(), fixture())
            self.assertFalse(client.exists())

    def test_bootstrap_checks_offline_and_dbc_before_guarded_insert_and_exact_readback(self):
        expected = {"id": 9000, "zone": 4988, "team": 0, "comment": self.location.comment}
        with tempfile.TemporaryDirectory(dir=graveyard.MANIFEST.parents[3]) as directory:
            data, base, client = self.disk_fixture(directory)
            graveyard.build_server(data, base, client, self.location)
            for before in ("", json.dumps(expected)):
                with self.subTest(already_installed=bool(before)), \
                        patch.object(graveyard.settlement_population, "ensure_world_offline") as offline, \
                        patch.object(manage, "lab_mysql", side_effect=[before, "", json.dumps(expected)]) as sql:
                    result = graveyard.bootstrap(data, SimpleNamespace(ai_profile="settlement-v8"), self.location)
                    self.assertEqual(result["status"], "verified")
                    self.assertEqual(offline.call_count, 3)
                    self.assertEqual(sql.call_count, 3)
                    self.assertEqual(sql.call_args_list[1].args[1], graveyard.bootstrap_sql(self.location))
                    self.assertEqual(sql.call_args_list[2].args[1], graveyard.snapshot_sql(self.location))

    def test_bootstrap_conflicting_sql_dbc_or_open_world_cannot_mutate_routing(self):
        with tempfile.TemporaryDirectory(dir=graveyard.MANIFEST.parents[3]) as directory:
            data, base, client = self.disk_fixture(directory)
            graveyard.build_server(data, base, client, self.location)
            settings = SimpleNamespace(ai_profile="settlement-v8")
            with patch.object(graveyard.settlement_population, "ensure_world_offline"), \
                    patch.object(manage, "lab_mysql", return_value=json.dumps(
                        {"id": 4, "zone": 4988, "team": 0, "comment": "foreign"})) as sql, \
                    self.assertRaisesRegex(ValueError, "routing"):
                graveyard.bootstrap(data, settings, self.location)
            self.assertEqual(sql.call_count, 1)
            self.assertEqual(sql.call_args.args[1], graveyard.snapshot_sql(self.location))
            with patch.object(graveyard.settlement_population, "ensure_world_offline",
                              side_effect=ValueError("world accepting connections")), \
                    patch.object(manage, "lab_mysql") as sql, self.assertRaisesRegex(ValueError, "accepting"):
                graveyard.bootstrap(data, settings, self.location)
            sql.assert_not_called()
            (data / "dbc/WorldSafeLocs.dbc").write_bytes(base.read_bytes())
            with patch.object(graveyard.settlement_population, "ensure_world_offline"), \
                    patch.object(manage, "lab_mysql") as sql, self.assertRaisesRegex(ValueError, "Missing custom"):
                graveyard.bootstrap(data, settings, self.location)
            sql.assert_not_called()

    def test_bootstrap_missing_readback_does_not_claim_success(self):
        with tempfile.TemporaryDirectory(dir=graveyard.MANIFEST.parents[3]) as directory:
            data, base, client = self.disk_fixture(directory)
            graveyard.build_server(data, base, client, self.location)
            with patch.object(graveyard.settlement_population, "ensure_world_offline"), \
                    patch.object(manage, "lab_mysql", return_value="") as sql, \
                    self.assertRaisesRegex(ValueError, "routing"):
                graveyard.bootstrap(data, SimpleNamespace(ai_profile="settlement-v8"), self.location)
            self.assertEqual(sql.call_count, 3)


if __name__ == "__main__":
    unittest.main()
