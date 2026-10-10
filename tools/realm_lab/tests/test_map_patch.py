from io import BytesIO
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

try:
    from mpyq import MPQArchive
except ImportError:
    # Local authoring dependency; CI installs the pinned independent reader.
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "runtime/lab-map-deps"))
    from mpyq import MPQArchive

from tools.realm_lab.build_map_patch import (
    FILE_FLAGS, HASH_COUNT, WHITELIST, build_archive, build_patch, tile_whitelist, validate_project,
)


def chunk(tag, body):
    return tag[::-1] + struct.pack("<I", len(body)) + body


def dbc(fields, row, strings=b"\0"):
    return struct.pack("<4s4I", b"WDBC", 1, fields, fields * 4, len(strings)) + struct.pack(
        f"<{fields}I", *row) + strings


def fixture():
    map_row = [0] * 66
    map_row[0], map_row[1], map_row[22], map_row[63] = 725, 1, 4988, 2
    area_row = [0] * 36
    area_row[0], area_row[1] = 4988, 725
    light_row = [0] * 15
    light_row[0], light_row[1] = 9999, 725
    main = bytearray(64 * 64 * 8)
    struct.pack_into("<I", main, (31 * 64 + 30) * 8, 1)
    terrain = bytearray()
    for y in range(16):
        for x in range(16):
            body = bytearray(128)
            struct.pack_into("<2I", body, 4, x, y)
            struct.pack_into("<I", body, 52, 4988)
            terrain.extend(chunk(b"MCNK", body))
    values = [dbc(66, map_row, b"\0AIWorldLab\0"), dbc(36, area_row),
              dbc(15, light_row), chunk(b"MAIN", main),
              chunk(b"MVER", struct.pack("<I", 18)), bytes(terrain)]
    return {internal: data for (_, internal), data in zip(WHITELIST, values)}


FOUR_TILES = ((30, 31), (31, 31), (30, 32), (31, 32))


def multi_fixture(tiles=FOUR_TILES):
    files = fixture()
    main = bytearray(64 * 64 * 8)
    for x, y in tiles:
        struct.pack_into("<I", main, (y * 64 + x) * 8, 1)
    files[WHITELIST[3][1]] = chunk(b"MAIN", main)
    terrain = files.pop(WHITELIST[5][1])
    files.update({internal: terrain for _, internal in tile_whitelist(tiles)[5:]})
    return files


def write_project(project, files, whitelist=WHITELIST):
    for relative, internal in whitelist:
        source = project / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(files[internal])


class MapPatchTests(unittest.TestCase):
    def test_all_six_files_roundtrip_with_independent_reader_and_no_extra_files(self):
        files = fixture()
        archive = MPQArchive(BytesIO(build_archive(files)))
        expected = set(files) | {"(listfile)"}
        self.assertEqual({name.decode("ascii") for name in archive.files}, expected)
        self.assertEqual(archive.header["format_version"], 0)
        self.assertEqual(archive.header["header_size"], 32)
        self.assertEqual(len(archive.block_table), 7)
        for name, data in files.items():
            with self.subTest(name=name):
                self.assertEqual(archive.read_file(name), data)
                self.assertEqual(archive.read_file(name.swapcase()), data)
        self.assertIsNone(archive.read_file(r"World\Maps\Azeroth\Azeroth.wdt"))
        self.assertIsNone(archive.read_file("AI World Lab.noggitproj"))
        self.assertTrue(all(block.flags == FILE_FLAGS for block in archive.block_table))

    def test_hash_table_placement_supports_libmpq_linear_lookup(self):
        archive = MPQArchive(BytesIO(build_archive(fixture())))
        # mpyq searches the whole table; independently exercise libmpq's probe rule.
        for raw_name in archive.files:
            name = raw_name.decode("ascii")
            slot = archive._hash(name, "TABLE_OFFSET") & (HASH_COUNT - 1)
            wanted = (archive._hash(name, "HASH_A"), archive._hash(name, "HASH_B"))
            for _ in range(HASH_COUNT):
                entry = archive.hash_table[slot]
                self.assertNotEqual(entry.block_table_index, 0xFFFFFFFF, name)
                if (entry.hash_a, entry.hash_b) == wanted:
                    self.assertEqual((entry.locale, entry.platform), (0, 0))
                    break
                slot = (slot + 1) & (HASH_COUNT - 1)
            else:
                self.fail(f"Missing hash-chain entry: {name}")

    def test_archive_is_deterministic_and_rejects_extra_input(self):
        files = fixture()
        self.assertEqual(build_archive(files), build_archive(files))
        # Recorded from the reviewed six-file builder before multi-tile support.
        self.assertEqual(hashlib.sha256(build_archive(files)).hexdigest(),
                         "38484f823fdace22a1d8738b465a17c2da726afeb2edced27381fd91ff81a667")
        with self.assertRaisesRegex(ValueError, "exactly the six"):
            build_archive({**files, "uid.ini": b"secret"})

    def test_output_is_exclusive_and_identical_rerun_leaves_file_untouched(self):
        # Keep fixtures and their cleanup inside the writable checkout on Windows.
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3]) as directory:
            project = Path(directory) / "project"
            output = Path(directory) / "patch-4.MPQ"
            files = fixture()
            for relative, internal in WHITELIST:
                source = project / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(files[internal])
            (project / "uid.ini").write_bytes(b"must not be included")
            self.assertTrue(build_patch(project, output)["created"])
            previous = output.stat().st_mtime_ns
            self.assertFalse(build_patch(project, output)["created"])
            self.assertEqual(output.stat().st_mtime_ns, previous)
            output.write_bytes(b"existing user patch")
            with self.assertRaisesRegex(ValueError, "Existing output differs"):
                build_patch(project, output)
            self.assertEqual(output.read_bytes(), b"existing user patch")

    def test_malformed_project_and_wrong_map_are_rejected(self):
        files = fixture()
        validate_project(files)
        for internal in (WHITELIST[0][1], WHITELIST[3][1], WHITELIST[5][1]):
            with self.subTest(file=internal), self.assertRaises(ValueError):
                validate_project({**files, internal: files[internal][:-1]})
        wrong_map = bytearray(files[WHITELIST[0][1]])
        struct.pack_into("<I", wrong_map, 20, 726)
        with self.assertRaisesRegex(ValueError, "MapID 725"):
            validate_project({**files, WHITELIST[0][1]: bytes(wrong_map)})
        wrong_area = bytearray(files[WHITELIST[5][1]])
        struct.pack_into("<I", wrong_area, 8 + 52, 12)
        with self.assertRaisesRegex(ValueError, "AreaID 4988"):
            validate_project({**files, WHITELIST[5][1]: bytes(wrong_area)})

    def test_independent_reader_rejects_corrupt_magic_and_truncated_tables(self):
        archive = build_archive(fixture())
        with self.assertRaises(ValueError):
            MPQArchive(BytesIO(b"BAD!" + archive[4:]))
        with self.assertRaises(struct.error):
            MPQArchive(BytesIO(archive[:16]))
        with self.assertRaises(struct.error):
            MPQArchive(BytesIO(archive[:-1]))

    def test_explicit_four_tile_patch_roundtrips_all_tiles_and_omits_foreign_map(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3]) as directory:
            project = Path(directory) / "project"
            output = Path(directory) / "patch-4.MPQ"
            files = multi_fixture()
            write_project(project, files, tile_whitelist(FOUR_TILES))
            unrelated = project / "world/maps/azeroth/azeroth_31_49.adt"
            unrelated.parent.mkdir(parents=True)
            unrelated.write_bytes(b"must not be included")
            result = build_patch(project, output, all_tiles=True)
            self.assertEqual(result["tiles"], [{"x": x, "y": y} for x, y in FOUR_TILES])
            self.assertEqual(len(result["files"]), 9)
            archive = MPQArchive(BytesIO(output.read_bytes()))
            self.assertEqual({name.decode("ascii") for name in archive.files}, set(files) | {"(listfile)"})
            self.assertEqual(len(archive.hash_table), 32)
            self.assertEqual(len(archive.block_table), 10)
            for name, data in files.items():
                self.assertEqual(archive.read_file(name), data)
            self.assertIsNone(archive.read_file(r"World\Maps\Azeroth\Azeroth_31_49.adt"))
            previous = output.stat().st_mtime_ns
            self.assertFalse(build_patch(project, output, all_tiles=True)["created"])
            self.assertEqual(output.stat().st_mtime_ns, previous)
            with self.assertRaisesRegex(ValueError, "Expected only WDT tile"):
                build_patch(project, Path(directory) / "default.MPQ")

    def test_missing_and_extra_adts_are_rejected_without_creating_patch(self):
        for missing in (True, False):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory(
                    dir=Path(__file__).resolve().parents[3]) as directory:
                project = Path(directory) / "project"
                output = Path(directory) / "patch-4.MPQ"
                files = multi_fixture()
                whitelist = tile_whitelist(FOUR_TILES)
                write_project(project, files, whitelist)
                if missing:
                    (project / whitelist[-1][0]).unlink()
                else:
                    (project / "world/maps/aiworldlab/aiworldlab_32_32.adt").write_bytes(
                        files[whitelist[-1][1]])
                with self.assertRaisesRegex(ValueError, "ADT files must match WDT terrain tiles exactly"):
                    build_patch(project, output, all_tiles=True)
                self.assertFalse(output.exists())

    def test_every_multitile_adt_must_have_the_reviewed_area_and_complete_cells(self):
        files = multi_fixture()
        validate_project(files, tiles=FOUR_TILES)
        for _, name in tile_whitelist(FOUR_TILES)[5:]:
            wrong = bytearray(files[name])
            struct.pack_into("<I", wrong, 8 + 52, 0)
            with self.subTest(file=name), self.assertRaisesRegex(ValueError, "AreaID 4988"):
                validate_project({**files, name: bytes(wrong)}, tiles=FOUR_TILES)
            with self.subTest(file=name), self.assertRaises(ValueError):
                validate_project({**files, name: files[name][:-1]}, tiles=FOUR_TILES)

    def test_large_archive_hash_table_keeps_empty_slots_and_valid_probe_chains(self):
        tiles = tuple((x, y) for y in range(31, 34) for x in range(30, 36))
        files = multi_fixture(tiles)
        archive = MPQArchive(BytesIO(build_archive(files, tile_whitelist(tiles))))
        hash_count = len(archive.hash_table)
        self.assertGreaterEqual(hash_count, 2 * len(archive.block_table))
        self.assertEqual(hash_count & (hash_count - 1), 0)
        for raw_name in archive.files:
            name = raw_name.decode("ascii")
            slot = archive._hash(name, "TABLE_OFFSET") & (hash_count - 1)
            wanted = (archive._hash(name, "HASH_A"), archive._hash(name, "HASH_B"))
            for _ in range(hash_count):
                entry = archive.hash_table[slot]
                self.assertNotEqual(entry.block_table_index, 0xFFFFFFFF, name)
                if (entry.hash_a, entry.hash_b) == wanted:
                    break
                slot = (slot + 1) & (hash_count - 1)
            else:
                self.fail(f"Missing multi-tile hash-chain entry: {name}")
            if name != "(listfile)":
                self.assertEqual(archive.read_file(name), files[name])

    def test_empty_wdt_and_duplicate_archive_names_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "nonempty unique"):
            tile_whitelist(())
        with self.assertRaisesRegex(ValueError, "nonempty unique"):
            tile_whitelist(((30, 31), (30, 31)))
        with self.assertRaisesRegex(ValueError, "unique"):
            build_archive(fixture(), WHITELIST + (WHITELIST[-1],))
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3]) as directory:
            project = Path(directory) / "project"
            output = Path(directory) / "patch-4.MPQ"
            files = fixture()
            files[WHITELIST[3][1]] = chunk(b"MAIN", bytes(64 * 64 * 8))
            write_project(project, files)
            with self.assertRaisesRegex(ValueError, "nonempty unique"):
                build_patch(project, output, all_tiles=True)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
