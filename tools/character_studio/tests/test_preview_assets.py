"""Bounded legacy model decoding, independent of proprietary client assets."""
import importlib.util
from pathlib import Path
import struct
import unittest

SPEC = importlib.util.spec_from_file_location("preview_assets", Path(__file__).resolve().parents[1] / "build_preview_assets.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def fixture():
    model = bytearray(640)
    model[:4] = b"MD20"
    struct.pack_into("<I", model, 4, 264)
    struct.pack_into("<2I", model, 60, 3, 256)
    struct.pack_into("<2I", model, 80, 1, 416)
    struct.pack_into("<4I", model, 416, 1, 0, 0, 0)
    struct.pack_into("<2I", model, 128, 1, 448)
    struct.pack_into("<H", model, 448, 0)
    for index, position in enumerate(((1, 2, 3), (2, 3, 4), (3, 4, 5))):
        struct.pack_into("<3f4B4B3f2f2f", model, 256 + index * 48,
                         *position, 255, 0, 0, 0, 0, 0, 0, 0,
                         0, 0, 1, .25, .75, 0, 0)
    skin = bytearray(256)
    skin[:4] = b"SKIN"
    struct.pack_into("<2I", skin, 4, 3, 48)
    struct.pack_into("<3H", skin, 48, 2, 0, 1)
    struct.pack_into("<2I", skin, 12, 3, 56)
    struct.pack_into("<3H", skin, 56, 0, 1, 2)
    struct.pack_into("<2I", skin, 28, 1, 80)
    struct.pack_into("<10H", skin, 80, 201, 0, 0, 3, 0, 3, 1, 0, 1, 0)
    struct.pack_into("<2I", skin, 36, 1, 144)
    struct.pack_into("<2B11H", skin, 144, 0, 0, 0, 0, 0, 65535, 0, 0, 1, 0, 0, 0, 0)
    return model, skin


class LegacyGeometryTests(unittest.TestCase):
    def test_coordinate_normal_uv_and_skin_vertex_indirection(self):
        model, skin = fixture()
        decoded = MODULE.geometry(model, skin)
        self.assertEqual(decoded["positions"][:3], [-2.0, 3.0, 1.0])
        self.assertEqual(decoded["normals"][:3], [-0.0, 1.0, 0.0])
        self.assertEqual(decoded["uvs"][:2], [.25, .75])
        self.assertEqual(decoded["parts"], [{"geoset": 201, "material": 0, "indices": [2, 0, 1]}])
        self.assertEqual(decoded["textures"], [{"kind": 1, "name": ""}])

    def test_only_legacy_m2_and_skin_are_admitted(self):
        for kind in ("version", "m2", "skin"):
            with self.subTest(kind=kind):
                model, skin = fixture()
                if kind == "version": struct.pack_into("<I", model, 4, 999)
                elif kind == "m2": model[:4] = b"MD21"
                else: skin[:4] = b"BAD!"
                with self.assertRaises(ValueError): MODULE.geometry(model, skin)

    def test_unbounded_or_out_of_file_arrays_fail(self):
        for count, offset in ((1000001, 256), (3, 630), (0xFFFFFFFF, 0)):
            with self.subTest(count=count, offset=offset):
                model, skin = fixture()
                struct.pack_into("<2I", model, 60, count, offset)
                with self.assertRaises(ValueError): MODULE.geometry(model, skin)

    def test_invalid_indices_triangles_and_nonfinite_vertices_fail(self):
        for kind in ("vertex", "lookup", "triangle", "nan"):
            with self.subTest(kind=kind):
                model, skin = fixture()
                if kind == "vertex": struct.pack_into("<H", skin, 48, 99)
                elif kind == "lookup": struct.pack_into("<H", skin, 56, 99)
                elif kind == "triangle": struct.pack_into("<H", skin, 80 + 10, 2)
                else: struct.pack_into("<f", model, 256, float("nan"))
                with self.assertRaises(ValueError): MODULE.geometry(model, skin)


class PaletteAlphaTests(unittest.TestCase):
    @staticmethod
    def image(depth, alpha, blp1=False):
        palette_offset = 156 if blp1 else 148
        offset = palette_offset + 1024
        data = bytearray(offset + 4 + len(alpha))
        data[:4] = b"BLP1" if blp1 else b"BLP2"
        struct.pack_into("<I", data, 4, 1)
        if blp1:
            struct.pack_into("<I", data, 8, depth)
        else:
            data[8:12] = bytes((1, depth, 8, 1))
        struct.pack_into("<2I", data, 12, 4, 1)
        struct.pack_into("<I", data, 28 if blp1 else 20, offset)
        struct.pack_into("<I", data, 92 if blp1 else 84, 4 + len(alpha))
        # Palette fourth bytes intentionally zero: they are not the alpha plane.
        data[palette_offset:palette_offset + 8] = bytes((20, 40, 80, 0, 30, 60, 120, 0))
        data[offset:offset + 4] = bytes((0, 1, 0, 1))
        data[offset + 4:] = alpha
        return MODULE.decode_blp(bytes(data))

    def test_no_alpha_plane_means_opaque_palette_colors(self):
        for blp1 in (False, True):
            with self.subTest(blp1=blp1):
                image = self.image(0, b"", blp1)
                self.assertEqual(list(image.getdata()), [(80, 40, 20, 255), (120, 60, 30, 255)] * 2)

    def test_eight_bit_alpha_uses_mip_plane(self):
        for blp1 in (False, True):
            with self.subTest(blp1=blp1):
                self.assertEqual(list(self.image(8, bytes((0, 64, 128, 255)), blp1).getchannel("A").getdata()), [0, 64, 128, 255])

    def test_four_bit_alpha_uses_low_nibble_first(self):
        for blp1 in (False, True):
            with self.subTest(blp1=blp1):
                self.assertEqual(list(self.image(4, bytes((0xF0, 0x78)), blp1).getchannel("A").getdata()), [0, 255, 136, 119])

    def test_one_bit_alpha_uses_low_bit_first(self):
        for blp1 in (False, True):
            with self.subTest(blp1=blp1):
                self.assertEqual(list(self.image(1, bytes((0b1010,)), blp1).getchannel("A").getdata()), [0, 255, 0, 255])

if __name__ == "__main__":
    unittest.main()
