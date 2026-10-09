import math
from pathlib import Path
import struct
import tempfile
import unittest

from tools.realm_lab.terrain_geometry import (
    GRID_SIZE, MIDPOINT, ROOT, UNIT_SIZE, Terrain, chunks, model_placements,
    f32, gridmap_height, read_float_map, read_terrain, write_nav_points,
)


def chunk(tag, body):
    return struct.pack("<4sI", tag[::-1], len(body)) + body


def tile_bytes(height=lambda x, y: 7.0, modify=None):
    result = []
    mcin = bytearray(256 * 16)
    offset = 12 + 8 + len(mcin)
    for iy in range(16):
        for ix in range(16):
            body = bytearray(128)
            struct.pack_into("<2I", body, 4, ix, iy)
            struct.pack_into("<I", body, 20, 136)
            struct.pack_into("<I", body, 52, 4988)
            x, y = GRID_SIZE - iy * GRID_SIZE / 16, 2 * GRID_SIZE - ix * GRID_SIZE / 16
            struct.pack_into("<3f", body, 104, x, y, 7.0)
            values = []
            for row in range(9):
                values.extend(height(x - row * UNIT_SIZE, y - col * UNIT_SIZE) - 7
                              for col in range(9))
                if row < 8:
                    values.extend(height(x - (row + 0.5) * UNIT_SIZE,
                                         y - (col + 0.5) * UNIT_SIZE) - 7
                                  for col in range(8))
            body += chunk(b"MCVT", struct.pack("<145f", *values))
            if modify is not None:
                modify(ix, iy, body)
            encoded = chunk(b"MCNK", body)
            struct.pack_into("<2I", mcin, (iy * 16 + ix) * 16, offset, len(encoded))
            offset += len(encoded)
            result.append(encoded)
    return chunk(b"MVER", struct.pack("<I", 18)) + chunk(b"MCIN", mcin) + b"".join(result)


def float_map_bytes(terrain):
    values = [height for row in terrain.v9 + terrain.v8 for height in row]
    area = struct.pack("<4s2H", b"AREA", 1, 4988)
    heights = struct.pack("<4sI2f", b"MHGT", 0, min(values), max(values))
    heights += struct.pack(f"<{len(values)}f", *values)
    return struct.pack("<4s10I", b"MAPS", 10, 12340,
                       44, len(area), 44 + len(area), len(heights), 0, 0, 0, 0) + area + heights


class TerrainGeometryTests(unittest.TestCase):
    def test_base_height_and_native_tile_axes(self):
        terrain = read_terrain(tile_bytes(lambda x, y: 100 + 0.2 * x - 0.3 * y))
        # A nonsymmetric plane detects an ix/iy swap or reversed server axes.
        for x, y in ((266.667, 800), (326.667, 800), (100, 950),
                     (0, GRID_SIZE), (GRID_SIZE, 2 * GRID_SIZE)):
            self.assertAlmostEqual(terrain.height(x, y), 100 + 0.2 * x - 0.3 * y, places=4)
        self.assertEqual(terrain.counts["terrain_cells"], 256)
        self.assertEqual(terrain.counts["area_cells"], {4988: 256})
        self.assertLess(terrain.seam_max_delta, 0.00002)

    def test_four_center_triangles_use_actual_center_height(self):
        v9 = [[0.0] * 129 for _ in range(129)]
        v8 = [[0.0] * 128 for _ in range(128)]
        v9[1][0], v9[0][1], v9[1][1], v8[0][0] = 2, 3, 5, 10
        terrain = Terrain(30, 31, v9, v8,
                          {(x, y): 0 for x in range(16) for y in range(16)}, {}, [], 0)
        ox, oy = terrain.origin
        # Expected values come from independent barycentric weights on each
        # triangle, not bilinear interpolation of the four outside corners.
        for u, v, expected in ((0.4, 0.1, 2.6), (0.1, 0.4, 2.9),
                               (0.9, 0.6, 5.1), (0.6, 0.9, 5.4),
                               (0.5, 0.5, 10), (1, 1, 5)):
            self.assertAlmostEqual(terrain.height(ox - u * UNIT_SIZE, oy - v * UNIT_SIZE),
                                   expected, places=9)

    def test_both_tile_edges_are_included_but_neighbors_do_not_wrap(self):
        terrain = read_terrain(tile_bytes())
        for x in (0, GRID_SIZE):
            for y in (GRID_SIZE, 2 * GRID_SIZE):
                self.assertEqual(terrain.height(x, y), 7)
        for x, y in ((-0.01, 800), (GRID_SIZE + 0.01, 800),
                     (100, GRID_SIZE - 0.01), (100, 2 * GRID_SIZE + 0.01)):
            with self.assertRaisesRegex(ValueError, "outside ADT"):
                terrain.height(x, y)
        with self.assertRaisesRegex(ValueError, "Non-finite"):
            terrain.height(math.nan, 800)

    def test_holes_cover_two_by_two_squares_and_reject_ground_queries(self):
        def set_hole(ix, iy, body):
            if (ix, iy) == (0, 0):
                struct.pack_into("<I", body, 60, 1)
        terrain = read_terrain(tile_bytes(modify=set_hole))
        ox, oy = terrain.origin
        for row in (0, 1):
            for col in (0, 1):
                with self.assertRaisesRegex(ValueError, "terrain hole"):
                    terrain.height(ox - (row + 0.5) * UNIT_SIZE, oy - (col + 0.5) * UNIT_SIZE)
        self.assertEqual(terrain.height(ox - 2.5 * UNIT_SIZE, oy - 0.5 * UNIT_SIZE), 7)
        self.assertEqual(terrain.counts["hole_cells"], 1)
        self.assertEqual(terrain.counts["hole_blocks"], 1)
        self.assertEqual(terrain.counts["hole_squares"], 4)

    def test_known_planar_slope_is_measured_in_world_units(self):
        terrain = read_terrain(tile_bytes(lambda x, y: 7 + 0.3 * x + 0.4 * y))
        self.assertAlmostEqual(terrain.sample(266.667, 800)["triangle_slope_degrees"],
                               math.degrees(math.atan(0.5)), places=3)

    def test_corrupt_top_level_chunk_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Truncated ADT chunk body"):
            read_terrain(tile_bytes()[:-1])
        with self.assertRaisesRegex(ValueError, "chunk header"):
            list(chunks(b"bad"))

    def test_mcin_and_mcvt_must_describe_the_same_terrain(self):
        data = bytearray(tile_bytes())
        struct.pack_into("<I", data, 20, 999)
        with self.assertRaisesRegex(ValueError, "MCIN entry"):
            read_terrain(bytes(data))

    def test_corrupt_mcvt_pointer_is_rejected(self):
        def corrupt(ix, iy, body):
            if (ix, iy) == (0, 0):
                struct.pack_into("<I", body, 20, 132)
        with self.assertRaisesRegex(ValueError, "MCVT offset"):
            read_terrain(tile_bytes(modify=corrupt))

    def test_corrupt_mcvt_length_is_rejected(self):
        def corrupt(ix, iy, body):
            if (ix, iy) == (0, 0):
                struct.pack_into("<I", body, 132, 144 * 4)
        with self.assertRaisesRegex(ValueError, "145 MCVT"):
            read_terrain(tile_bytes(modify=corrupt))

    def test_nonfinite_height_is_rejected(self):
        def corrupt(ix, iy, body):
            if (ix, iy) == (0, 0):
                struct.pack_into("<f", body, 136, math.inf)
        with self.assertRaisesRegex(ValueError, "Non-finite MCVT"):
            read_terrain(tile_bytes(modify=corrupt))

    def test_duplicate_cell_is_rejected(self):
        def corrupt(ix, iy, body):
            if (ix, iy) == (1, 0):
                struct.pack_into("<I", body, 4, 0)
        with self.assertRaisesRegex(ValueError, "Duplicate or invalid MCNK"):
            read_terrain(tile_bytes(modify=corrupt))

    def test_wrong_tile_position_is_rejected(self):
        def corrupt(ix, iy, body):
            if (ix, iy) == (0, 0):
                struct.pack_into("<f", body, 104, 1)
        with self.assertRaisesRegex(ValueError, "position does not match"):
            read_terrain(tile_bytes(modify=corrupt))

    def test_model_coordinates_and_wmo_bounds_use_native_axis_conversion(self):
        m2 = struct.pack("<2I6f2H", 0, 10, MIDPOINT - 800, 12, MIDPOINT - 200,
                         0, 90, 0, 2048, 0)
        wmo = struct.pack("<2I12f4H", 0, 11,
                          MIDPOINT - 810, 15, MIDPOINT - 210, 0, 45, 0,
                          MIDPOINT - 830, 10, MIDPOINT - 230,
                          MIDPOINT - 790, 25, MIDPOINT - 190,
                          0, 0, 0, 1024)
        top = {b"MDDF": m2, b"MMDX": b"building.m2\0", b"MMID": struct.pack("<I", 0),
               b"MODF": wmo, b"MWMO": b"building.wmo\0", b"MWID": struct.pack("<I", 0)}
        models = model_placements(top)
        for actual, expected in zip(models[0]["server_position"].values(), (200, 800, 12)):
            self.assertAlmostEqual(actual, expected, places=3)
        self.assertEqual(models[0]["scale"], 2)
        bounds = models[1]["server_bounds"]
        for key, expected in (("min_x", 190), ("max_x", 230), ("min_y", 790),
                               ("max_y", 830), ("min_z", 10), ("max_z", 25)):
            self.assertAlmostEqual(bounds[key], expected, places=3)

    def test_invalid_model_reference_is_rejected(self):
        top = {b"MDDF": struct.pack("<2I6f2H", 1, 10, 0, 0, 0, 0, 0, 0, 1024, 0),
               b"MMDX": b"building.m2\0", b"MMID": struct.pack("<I", 0)}
        with self.assertRaisesRegex(ValueError, "missing name"):
            model_placements(top)

    def test_nav_points_preserve_roles_xy_and_use_measured_heights(self):
        report = {"agents": [{"spawn_id": 900725, "role": "predator", "x": 266.667,
                               "y": 800, "ground_z": 22.25}],
                  "test_points": [{"name": "home", "x": 266.667, "y": 800, "ground_z": 22.25},
                                  {"name": "minus_x", "x": 226.667, "y": 800, "ground_z": 14.5}]}
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            output = Path(directory) / "nav-points.tsv"
            write_nav_points(report, output)
            rows = output.read_text(encoding="utf-8").splitlines()
        self.assertEqual(rows[0], "kind\tid\trole\tx\ty\tz")
        self.assertEqual(rows[1].split("\t"), ["home", "900725", "predator", "266.667000000", "800.000000000", "22.250000000"])
        self.assertEqual(rows[2].split("\t"), ["test", "minus_x", "-", "226.667000000", "800.000000000", "14.500000000"])

    def test_extracted_map_plane_preserves_axes_and_float32_precision(self):
        adt = read_terrain(tile_bytes(lambda x, y: 100 + 0.2 * x - 0.3 * y))
        terrain = read_float_map(float_map_bytes(adt))
        self.assertEqual(terrain.v9, [[f32(value) for value in row] for row in adt.v9])
        self.assertEqual(terrain.v8, [[f32(value) for value in row] for row in adt.v8])
        for x, y in ((266.667, 800), (326.667, 800), (100, 950), (351.667, 774)):
            self.assertAlmostEqual(gridmap_height(terrain, x, y),
                                   100 + 0.2 * x - 0.3 * y, delta=0.001)

    def test_extracted_map_truncation_and_integer_flags_are_rejected(self):
        data = bytearray(float_map_bytes(read_terrain(tile_bytes())))
        with self.assertRaisesRegex(ValueError, "section bounds"):
            read_float_map(data[:-1])
        struct.pack_into("<I", data, 56, 2)
        with self.assertRaisesRegex(ValueError, "float32 heights"):
            read_float_map(data)

    def test_extracted_map_bad_section_pointer_is_rejected(self):
        data = bytearray(float_map_bytes(read_terrain(tile_bytes())))
        struct.pack_into("<I", data, 20, 53)
        with self.assertRaisesRegex(ValueError, "section bounds"):
            read_float_map(data)


if __name__ == "__main__":
    unittest.main()
