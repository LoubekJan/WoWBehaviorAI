"""Extract a bounded static character preview from an owned WoW 3.3.5a client.

Install Pillow and mpyq before running. No client data is downloaded or changed.
Generated geometry/textures are private runtime files, never repository assets.
The renderer uses original bind-pose geometry; animation is intentionally absent.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import struct

RACES = {1: "Human", 2: "Orc", 3: "Dwarf", 4: "NightElf", 5: "Scourge",
         6: "Tauren", 7: "Gnome", 8: "Troll", 10: "BloodElf", 11: "Draenei"}


def dbc(path):
    raw = path.read_bytes()
    if len(raw) < 20:
        raise ValueError("Truncated DBC")
    magic, count, fields, stride, string_bytes = struct.unpack_from("<4s4I", raw)
    if (magic != b"WDBC" or stride != fields * 4 or count > 100000
            or len(raw) != 20 + count * stride + string_bytes):
        raise ValueError("Unsupported DBC")
    strings = raw[20 + count * stride:]
    rows = [struct.unpack_from("<" + "I" * fields, raw, 20 + i * stride) for i in range(count)]

    def text(offset):
        if offset >= len(strings):
            raise ValueError("Invalid DBC string offset")
        return strings[offset:strings.index(b"\0", offset)].decode("utf-8")
    return rows, text


class Client:
    def __init__(self, directory):
        from mpyq import MPQArchive
        self.archives = []
        paths = list((directory / "Data").glob("*.MPQ"))

        def priority(path):
            name = path.stem.lower()
            if name == "patch":
                return 100
            if name.startswith("patch-"):
                suffix = name[6:]
                return 100 + (int(suffix) if suffix.isdecimal() else 1000 + sum(map(ord, suffix)))
            return {"common": 0, "common-2": 1, "expansion": 2, "lichking": 3}.get(name, -1)

        for path in sorted(paths, key=priority, reverse=True):
            archive = MPQArchive(str(path), listfile=False)
            # mpyq's general hash lookup scans every entry. A prebuilt map
            # retains the archive's locale preference and makes conversion bounded.
            table = {}
            for entry in archive.hash_table:
                table.setdefault((entry.hash_a, entry.hash_b), entry)
            archive.get_hash_table_entry = lambda name, a=archive, t=table: t.get(
                (a._hash(name, "HASH_A"), a._hash(name, "HASH_B")))
            self.archives.append(archive)
        self.cache = {}

    def read(self, name):
        name = name.replace("/", "\\")
        if name in self.cache:
            return self.cache[name]
        if not name or ".." in name or name.startswith("\\") or ":" in name:
            raise ValueError("Unsafe archive member")
        for archive in self.archives:
            value = archive.read_file(name)
            if value:
                if len(value) > 32 * 1024 * 1024:
                    raise ValueError("Oversized archive member")
                self.cache[name] = value
                return value
        raise FileNotFoundError(name)


def checked_array(raw, offset, count, stride):
    if count > 1000000 or offset < 0 or offset + count * stride > len(raw):
        raise ValueError("Invalid model array")


def decode_blp(raw):
    """Decode palette alpha from the mip's separate plane, not palette byte 4.

    Legacy BLP palette entries are B,G,R,unused. Pillow's palette BLP decoder
    treats that unused byte as alpha, hiding clothes/face overlays entirely.
    Other BLP encodings continue through Pillow's tested DXT/JPEG decoder.
    """
    from PIL import Image
    if len(raw) < 148:
        raise ValueError("Truncated BLP")
    if raw[:4] == b"BLP2" and struct.unpack_from("<I", raw, 4)[0] == 1 and raw[8] == 1:
        depth, palette_offset, offsets_offset, sizes_offset = raw[9], 148, 20, 84
    elif raw[:4] == b"BLP1" and struct.unpack_from("<I", raw, 4)[0] == 1:
        depth, palette_offset, offsets_offset, sizes_offset = struct.unpack_from("<I", raw, 8)[0], 156, 28, 92
    else:
        return Image.open(io.BytesIO(raw)).convert("RGBA")
    width, height = struct.unpack_from("<2I", raw, 12)
    if not 0 < width <= 4096 or not 0 < height <= 4096 or depth not in (0, 1, 4, 8):
        raise ValueError("Unsupported paletted BLP")
    pixels = width * height
    offset = struct.unpack_from("<I", raw, offsets_offset)[0]
    size = struct.unpack_from("<I", raw, sizes_offset)[0]
    alpha_bytes = (pixels * depth + 7) // 8
    if (palette_offset + 1024 > len(raw) or offset < palette_offset + 1024
            or size < pixels + alpha_bytes or offset + size > len(raw)):
        raise ValueError("Truncated BLP mip or palette")
    palette = raw[palette_offset:palette_offset + 1024]
    indexed = Image.frombytes("P", (width, height), raw[offset:offset + pixels])
    indexed.putpalette(bytes(channel for i in range(256) for channel in
                             (palette[i * 4 + 2], palette[i * 4 + 1], palette[i * 4])))
    image = indexed.convert("RGBA")
    plane = raw[offset + pixels:offset + pixels + alpha_bytes]
    if depth == 8:
        alpha = plane
    elif depth == 4:
        alpha = bytes(((plane[i // 2] >> ((i % 2) * 4)) & 15) * 17 for i in range(pixels))
    elif depth == 1:
        alpha = bytes(((plane[i // 8] >> (i % 8)) & 1) * 255 for i in range(pixels))
    else:
        alpha = b"\xff" * pixels
    image.putalpha(Image.frombytes("L", (width, height), alpha))
    return image


def geometry(model, skin):
    if model[:4] != b"MD20" or struct.unpack_from("<I", model, 4)[0] != 264 or skin[:4] != b"SKIN":
        raise ValueError("Expected legacy 3.3.5a M2 and SKIN")
    count, offset = struct.unpack_from("<2I", model, 60)
    checked_array(model, offset, count, 48)
    vertices = [struct.unpack_from("<3f4B4B3f2f2f", model, offset + i * 48) for i in range(count)]
    positions, normals, uvs = [], [], []
    for v in vertices:
        if any(not math.isfinite(value) for value in (*v[:3], *v[11:])):
            raise ValueError("Non-finite model vertex")
        positions.extend(round(f, 6) for f in (-v[1], v[2], v[0]))
        normals.extend(round(f, 6) for f in (-v[12], v[13], v[11]))
        uvs.extend(round(f, 6) for f in v[14:16])
    n, o = struct.unpack_from("<2I", skin, 4)
    checked_array(skin, o, n, 2)
    lookup = struct.unpack_from("<" + "H" * n, skin, o)
    n, o = struct.unpack_from("<2I", skin, 12)
    checked_array(skin, o, n, 2)
    triangles = struct.unpack_from("<" + "H" * n, skin, o)
    n, o = struct.unpack_from("<2I", skin, 28)
    checked_array(skin, o, n, 48)
    sections = [struct.unpack_from("<10H", skin, o + i * 48) for i in range(n)]
    n, o = struct.unpack_from("<2I", model, 80)
    checked_array(model, o, n, 16)
    textures = []
    for i in range(n):
        kind, flags, length, start = struct.unpack_from("<4I", model, o + i * 16)
        checked_array(model, start, length, 1)
        textures.append({"kind": kind, "name": model[start:start + length].rstrip(b"\0").decode("utf-8") if kind == 0 else ""})
    n, o = struct.unpack_from("<2I", model, 128)
    checked_array(model, o, n, 2)
    texture_lookup = struct.unpack_from("<" + "H" * n, model, o)
    n, o = struct.unpack_from("<2I", skin, 36)
    checked_array(skin, o, n, 24)
    materials = {}
    for i in range(n):
        batch = struct.unpack_from("<2B11H", skin, o + i * 24)
        section, combo = batch[3], batch[9]
        if section >= len(sections) or combo >= len(texture_lookup) or texture_lookup[combo] >= len(textures):
            raise ValueError("Invalid model texture lookup")
        materials.setdefault(section, texture_lookup[combo])
    parts = []
    for i, s in enumerate(sections):
        start, length = s[4] + s[1] * 65536, s[5]
        if start + length > len(triangles) or length % 3:
            raise ValueError("Invalid skin triangle range")
        selected = triangles[start:start + length]
        if any(t >= len(lookup) for t in selected):
            raise ValueError("Invalid skin lookup index")
        indices = [lookup[t] for t in selected]
        if any(v >= count for v in indices):
            raise ValueError("Invalid skin vertex index")
        parts.append({"geoset": s[0], "material": materials.get(i, 0), "indices": indices})
    return {"positions": positions, "normals": normals, "uvs": uvs, "parts": parts, "textures": textures}


def build(client_directory, output, races, catalog_path=None, equipment_only=False):
    from PIL import Image
    client = Client(client_directory)
    dbc_directory = client_directory / "dbc"
    sections, section_string = dbc(dbc_directory / "CharSections.dbc")
    hairs, _ = dbc(dbc_directory / "CharHairGeosets.dbc")
    facials, _ = dbc(dbc_directory / "CharacterFacialHairStyles.dbc")
    output.mkdir(parents=True, exist_ok=True)
    (output / "models").mkdir(exist_ok=True)
    (output / "textures").mkdir(exist_ok=True)
    converted = {}
    missing = set()

    def convert(name):
        if not name:
            return None
        key = name.lower()
        if key in converted:
            return converted[key]
        digest = hashlib.sha256(key.encode("utf-8")).hexdigest()[:24]
        relative = f"textures/{digest}.png"
        try:
            source = client.read(name)
            image = decode_blp(source)
            if image.width > 4096 or image.height > 4096:
                raise ValueError("Oversized texture")
            image.convert("RGBA").save(output / relative, format="PNG", optimize=False)
        except (FileNotFoundError, ValueError, OSError) as error:
            missing.add(name)
            converted[key] = None
            return None
        converted[key] = relative
        return relative

    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8")) if equipment_only else {
                "schema_version": 1, "format": "wow335-native-static", "client_build": 12340,
                "pose": "bind", "animations": False, "equipment_supported": False,
                "source": "owned-local-client", "variants": {}}
    for race in ([] if equipment_only else races):
        race_name = RACES[race]
        for gender, sex in ((0, "Male"), (1, "Female")):
            stem = f"Character\\{race_name}\\{sex}\\{race_name}{sex}"
            mesh = geometry(client.read(stem + ".m2"), client.read(stem + "00.skin"))
            for texture in mesh["textures"]:
                if texture["kind"] == 0:
                    texture["file"] = convert(texture["name"])
                texture.pop("name", None)
            key = f"{race}_{gender}"
            relative = f"models/{key}.json"
            (output / relative).write_text(json.dumps(mesh, separators=(",", ":")), encoding="utf-8")
            entries = []
            for row in sections:
                if row[1] != race or row[2] != gender or not row[7] & 1 or row[7] & 4:
                    continue
                files = [convert(section_string(offset)) for offset in row[4:7]]
                entries.append({"section": row[3], "style": row[8], "color": row[9], "files": files})
            variant = {"mesh": relative, "sections": entries,
                       "hair_geosets": {str(r[3]): {"geoset": r[4], "bald": bool(r[5])}
                                        for r in hairs if r[1] == race and r[2] == gender},
                       "facial_geosets": {str(r[2]): list(r[3:8])
                                          for r in facials if r[0] == race and r[1] == gender}}
            manifest["variants"][key] = variant
            print(json.dumps({"variant": key, "vertices": len(mesh["positions"]) // 3,
                              "parts": len(mesh["parts"]), "textures": len(converted)}, ensure_ascii=True), flush=True)
    # Some native DBC rows themselves name absent textures. Preserve that
    # information so the UI does not silently claim an exact completed look.
    section_index = {(r[1], r[2], r[3], r[8], r[9]): r for r in sections}
    for key, variant in manifest["variants"].items():
        race, gender = map(int, key.split("_"))
        for entry in variant["sections"]:
            row = section_index.get((race, gender, entry["section"], entry["style"], entry["color"]))
            entry["missing_layers"] = [i for i, offset in enumerate(row[4:7])
                                       if offset and not entry["files"][i]] if row else []
            if row:
                missing.update(section_string(row[4 + index]) for index in entry["missing_layers"])
    if catalog_path is not None:
        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        requested = {item["display_id"] for item in catalog["equipment"]}
        item_rows, item_string = dbc(dbc_directory / "ItemDisplayInfo.dbc")
        components = ("ArmUpperTexture", "ArmLowerTexture", "HandTexture", "TorsoUpperTexture",
                      "TorsoLowerTexture", "LegUpperTexture", "LegLowerTexture", "FootTexture")
        item_meta = {}
        for row in item_rows:
            if row[0] not in requested:
                continue
            genders = {}
            for gender, suffix in ((0, "M"), (1, "F")):
                layers = []
                for component, offset in zip(components, row[15:23]):
                    name = item_string(offset)
                    found = None
                    if name:
                        for variant in ("_" + suffix, "_U", ""):
                            candidate = f"Item\\TextureComponents\\{component}\\{name}{variant}.blp"
                            try:
                                client.read(candidate)
                            except FileNotFoundError:
                                continue
                            found = convert(candidate)
                            if found:
                                break
                    layers.append(found)
                genders[str(gender)] = layers
            item_meta[str(row[0])] = {"layers": genders, "geoset_groups": list(row[7:10])}
        manifest["equipment"] = item_meta
        manifest["equipment_surface_slots"] = ["shirt", "chest", "waist", "legs", "feet", "wrist", "hands", "tabard"]
        manifest["equipment_supported"] = "body-textures-only"
    manifest["texture_count"] = len(list((output / "textures").glob("*.png")))
    manifest["missing_texture_count"] = len(missing)
    revision = hashlib.sha256()
    for asset in sorted(p for p in output.rglob("*") if p.is_file() and p.name != "manifest.json"):
        revision.update(asset.relative_to(output).as_posix().encode("utf-8"))
        revision.update(hashlib.sha256(asset.read_bytes()).digest())
    manifest["asset_revision"] = revision.hexdigest()
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--races", default=",".join(map(str, RACES)))
    parser.add_argument("--catalog", type=Path, default=Path(__file__).resolve().parents[2] / "data/character_studio/catalog.json")
    parser.add_argument("--equipment-only", action="store_true", help="Add catalog clothing textures to existing generated models")
    args = parser.parse_args()
    races = [int(value) for value in args.races.split(",")]
    if not races or any(r not in RACES for r in races) or len(set(races)) != len(races):
        parser.error("Unsupported or duplicate races")
    try:
        manifest = build(args.client.resolve(strict=True), args.output.resolve(), races, args.catalog, args.equipment_only)
    except ImportError:
        parser.exit(2, "Install dependencies: python -m pip install Pillow==12.0.0 mpyq==0.2.5\n")
    print(json.dumps({"variants": len(manifest["variants"]), "textures": manifest["texture_count"],
                      "missing_textures": manifest["missing_texture_count"], "output": str(args.output)}))


if __name__ == "__main__":
    main()
