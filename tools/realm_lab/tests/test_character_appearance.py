import json
from pathlib import Path
import struct
import tempfile
import unittest

from tools.realm_lab.character_appearance import RACES, SLOTS, apply_item_names, read_dbc


class CharacterAppearanceCatalogTest(unittest.TestCase):
    def test_item_names_change_only_labels_and_record_missing_ids(self):
        catalog = {"source": {}, "equipment": [{"slot": "head", "item_id": 12, "display_id": 300, "label": "Hlava #12"},
                                                  {"slot": "feet", "item_id": 13, "display_id": 301, "label": "Chodidla #13"}]}
        apply_item_names(catalog, {"12": "Recruit's Hat"})
        self.assertEqual(catalog["equipment"], [{"slot": "head", "item_id": 12, "display_id": 300, "label": "Recruit's Hat"},
                                                 {"slot": "feet", "item_id": 13, "display_id": 301, "label": "Chodidla #13"}])
        self.assertEqual(catalog["source"]["item_names"]["matched_item_ids"], 1)
        self.assertEqual(catalog["source"]["item_names"]["missing_item_ids"], [13])
        self.assertRegex(catalog["source"]["item_names"]["sha256"], r"^[a-f0-9]{64}$")

    def test_item_name_metadata_rejects_noncanonical_identity_or_unsafe_name(self):
        for names in ([], {"01": "Hat"}, {"4294967296": "Hat"}, {"1": "bad\nname"}, {"1": ""}):
            with self.subTest(names=names), self.assertRaises(ValueError):
                apply_item_names({"source": {}, "equipment": []}, names)

    def test_dbc_reader_rejects_bad_envelopes_before_reading_records(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "test.dbc"
            valid = struct.pack("<4s4I4I", b"WDBC", 2, 2, 8, 1, 1, 2, 3, 4) + b"\0"
            path.write_bytes(valid)
            self.assertEqual(read_dbc(path, 2), [(1, 2), (3, 4)])
            for data in (b"", valid[:19], valid[:-1], valid + b"extra",
                         valid.replace(b"WDBC", b"WDB2"),
                         struct.pack("<4s4I", b"WDBC", 0xFFFFFFFF, 2, 8, 1)):
                path.write_bytes(data)
                with self.subTest(data_size=len(data)), self.assertRaises(ValueError):
                    read_dbc(path, 2)
            path.write_bytes(valid)
            with self.assertRaises(ValueError):
                read_dbc(path, 3)

    def test_versioned_catalog_has_real_slot_coverage_and_dependent_customizations(self):
        root = Path(__file__).resolve().parents[3]
        catalog = json.loads((root / "data/character_studio/catalog.json").read_text(encoding="utf-8"))
        self.assertEqual(catalog["client_build"], 12340)
        self.assertEqual(catalog["schema_version"], 1)
        self.assertEqual({race["id"] for race in catalog["races"]}, set(RACES))
        for race in catalog["races"]:
            self.assertEqual(set(race["gender_models"]), {"0", "1"})
            self.assertEqual({v["gender"] for v in race["variants"]}, {0, 1})
            for variant in race["variants"]:
                self.assertTrue(variant["skins"])
                self.assertTrue(variant["hair_options"])
                for skin in variant["skins"]:
                    self.assertTrue(next(v for v in variant["face_options"] if v["skin"] == skin)["faces"])
                for hair in variant["hair_options"]:
                    self.assertTrue(hair["colors"])
                    for color in hair["colors"]:
                        self.assertTrue(any(color in f["colors"] for f in variant["facial_options"]))
        triples = {(r["slot"], r["item_id"], r["display_id"]) for r in catalog["equipment"]}
        self.assertEqual(len(triples), len(catalog["equipment"]))
        self.assertLessEqual(len(triples), 300)
        for slot in SLOTS:
            rows = [r for r in catalog["equipment"] if r["slot"] == slot]
            self.assertGreaterEqual(len({r["display_id"] for r in rows}), 8)
            for row in rows:
                self.assertGreater(row["item_id"], 0)
                self.assertGreater(row["display_id"], 0)
                self.assertIn(row["inventory_type"], SLOTS[slot])
        for source in catalog["source"]["files"]:
            self.assertEqual(Path(source["name"]).name, source["name"])
            self.assertRegex(source["sha256"], r"^[a-f0-9]{64}$")


if __name__ == "__main__":
    unittest.main()
