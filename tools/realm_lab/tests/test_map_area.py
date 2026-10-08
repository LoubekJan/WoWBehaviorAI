from contextlib import redirect_stdout
import io
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.realm_lab.prepare_map_area import chunks, prepare, read_dbc

ROOT = Path(__file__).resolve().parents[3]


def dbc_bytes(fields, rows, strings):
    records = b"".join(struct.pack(f"<{fields}I", *row) for row in rows)
    return struct.pack("<4s4I", b"WDBC", len(rows), fields, fields * 4,
                       len(strings)) + records + strings


def chunk(tag, body):
    return struct.pack("<4sI", tag[::-1], len(body)) + body


class MapAreaPreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / "project"
        dbc_dir = self.project / "DBFilesClient"
        dbc_dir.mkdir(parents=True)
        self.base_area = self.root / "baseline-area.dbc"
        self.base_map = self.root / "baseline-map.dbc"
        self.area_path = dbc_dir / "AreaTable.dbc"
        self.map_path = dbc_dir / "Map.dbc"
        area = [0] * 36
        area[:5] = [12, 0, 0, 3617, 64]
        area[11] = 1
        area[27] = 16712190
        self.base_area.write_bytes(dbc_bytes(36, [area], b"\0Elwynn Forest\0"))
        original_map = [0] * 66
        self.base_map.write_bytes(dbc_bytes(66, [original_map], b"\0"))
        lab_map = [0] * 66
        lab_map[0] = 725
        lab_map[1] = 1
        lab_map[5] = 12
        lab_map[22] = 4988
        lab_map[63] = 2
        self.map_path.write_bytes(dbc_bytes(66, [original_map, lab_map],
                                            b"\0AIWorldLab\0AI World Lab\0"))
        self.world = self.project / "world/maps/aiworldlab"
        self.world.mkdir(parents=True)
        self.wdt_path = self.world / "aiworldlab.wdt"
        self.tile_path = self.world / "aiworldlab_30_31.adt"
        main = bytearray(64 * 64 * 8)
        struct.pack_into("<I", main, (31 * 64 + 30) * 8, 1)
        self.wdt_path.write_bytes(chunk(b"MAIN", main))
        terrain = [chunk(b"MVER", struct.pack("<I", 18))]
        for y in range(16):
            for x in range(16):
                cell = bytearray([0x5A] * 128)
                struct.pack_into("<2I", cell, 4, x, y)
                struct.pack_into("<I", cell, 52, 0)
                terrain.append(chunk(b"MCNK", cell))
        self.tile_path.write_bytes(b"".join(terrain))
        self.args = SimpleNamespace(project=self.project, base_area=self.base_area,
                                    base_map=self.base_map, map_id=725, area_id=4988,
                                    prepare=True)

    def run_prepare(self):
        with patch("tools.realm_lab.prepare_map_area.editor_closed"), redirect_stdout(io.StringIO()):
            prepare(self.args)

    def project_files(self):
        return {str(path.relative_to(self.project)): path.read_bytes()
                for path in self.project.rglob("*") if path.is_file()}

    def assert_rejected_without_project_changes(self, message):
        before = self.project_files()
        with self.assertRaisesRegex(ValueError, message):
            self.run_prepare()
        self.assertEqual(before, self.project_files())
        self.assertFalse((self.root / "map-project-backups").exists())

    def test_preparation_preserves_baseline_and_only_changes_terrain_area_bytes(self):
        baseline = self.base_area.read_bytes()
        old_terrain = self.tile_path.read_bytes()
        original_map = self.map_path.read_bytes()
        self.run_prepare()
        _, old_rows, old_strings = read_dbc(self.base_area, 36)
        _, rows, strings = read_dbc(self.area_path, 36)
        self.assertEqual(rows[:-1], old_rows)
        self.assertEqual(strings, old_strings + b"AI World Lab\0")
        self.assertEqual(rows[-1][:5], (4988, 725, 0, 3618, 0x04000000))
        self.assertEqual(self.base_area.read_bytes(), baseline)
        self.assertEqual(self.map_path.read_bytes(), original_map)
        new_terrain = self.tile_path.read_bytes()
        self.assertEqual(len(old_terrain), len(new_terrain))
        expected = bytearray(old_terrain)
        cells = [start for tag, start, _ in chunks(old_terrain) if tag == b"MCNK"]
        self.assertEqual(len(cells), 256)
        for start in cells:
            struct.pack_into("<I", expected, start + 52, 4988)
        self.assertEqual(new_terrain, bytes(expected))
        backups = list((self.root / "map-project-backups").glob("*/world/maps/aiworldlab/*.adt"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), old_terrain)
        before = self.project_files()
        self.args.prepare = False
        self.run_prepare()
        self.assertEqual(self.project_files(), before)
        self.assertEqual(len(list((self.root / "map-project-backups").iterdir())), 1)

    def test_missing_assignments_in_check_mode_do_not_write_any_project_file(self):
        self.args.prepare = False
        self.assert_rejected_without_project_changes("use --prepare")

    def test_open_editor_blocks_mutation(self):
        before = self.project_files()
        with patch("tools.realm_lab.prepare_map_area.editor_closed",
                   side_effect=ValueError("Close Noggit")), redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "Close Noggit"):
                prepare(self.args)
        self.assertEqual(self.project_files(), before)
        self.assertFalse((self.root / "map-project-backups").exists())

    def test_modified_baseline_area_record_is_rejected(self):
        data = bytearray(self.base_area.read_bytes())
        struct.pack_into("<I", data, 20 + 4 * 4, 0)
        self.area_path.write_bytes(data)
        self.assert_rejected_without_project_changes("Baseline area records")

    def test_modified_baseline_map_record_is_rejected(self):
        data = bytearray(self.map_path.read_bytes())
        struct.pack_into("<I", data, 20 + 3 * 4, 8)
        self.map_path.write_bytes(data)
        self.assert_rejected_without_project_changes("Baseline map records")

    def test_exploration_bit_overflow_is_rejected(self):
        data = bytearray(self.base_area.read_bytes())
        struct.pack_into("<I", data, 20 + 3 * 4, 4095)
        self.base_area.write_bytes(data)
        self.assert_rejected_without_project_changes("No exploration bit")

    def test_area_ids_outside_extractor_range_are_rejected(self):
        for area_id in (-1, 0, 65536):
            with self.subTest(area_id=area_id):
                self.args.area_id = area_id
                self.assert_rejected_without_project_changes("AreaID must fit")

    def test_duplicate_terrain_coordinates_are_rejected(self):
        data = bytearray(self.tile_path.read_bytes())
        cells = [start for tag, start, _ in chunks(data) if tag == b"MCNK"]
        struct.pack_into("<2I", data, cells[1] + 4, 0, 0)
        self.tile_path.write_bytes(data)
        self.assert_rejected_without_project_changes("Missing or duplicate terrain cells")

    def test_truncated_chunk_is_rejected(self):
        self.tile_path.write_bytes(self.tile_path.read_bytes()[:-1])
        self.assert_rejected_without_project_changes("Truncated chunk body")


if __name__ == "__main__":
    unittest.main()
