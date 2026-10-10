"""Build the AIWorldLab client patch without modifying its project.

The default keeps the reviewed six-file, single-tile patch unchanged. Explicit
--all-tiles builds exactly the terrain tiles declared in this map's WDT.

MPQ format version 0 (the original 32-byte header), neutral locale/platform,
uncompressed single-unit files and standard encrypted hash/block tables.
See dep/libmpq/libmpq/{common,mpq}.c for the format and table algorithms.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
MAP_ID = 725
AREA_ID = 4988
TILE = (30, 31)
HASH_COUNT = 16
FILE_FLAGS = 0x81000000  # EXISTS | SINGLE_UNIT; no compression/encryption.
MASK = 0xFFFFFFFF
WHITELIST = (
    ("DBFilesClient/Map.dbc", r"DBFilesClient\Map.dbc"),
    ("DBFilesClient/AreaTable.dbc", r"DBFilesClient\AreaTable.dbc"),
    ("DBFilesClient/Light.dbc", r"DBFilesClient\Light.dbc"),
    ("world/maps/aiworldlab/aiworldlab.wdt", r"World\Maps\AIWorldLab\AIWorldLab.wdt"),
    ("world/maps/aiworldlab/aiworldlab.wdl", r"World\Maps\AIWorldLab\AIWorldLab.wdl"),
    ("world/maps/aiworldlab/aiworldlab_30_31.adt", r"World\Maps\AIWorldLab\AIWorldLab_30_31.adt"),
)


def encryption_table() -> tuple[int, ...]:
    seed = 0x00100001
    table = [0] * 0x500
    for byte in range(256):
        for group in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            high = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[byte + group * 256] = high | (seed & 0xFFFF)
    return tuple(table)


CRYPT = encryption_table()


def mpq_hash(name: str, kind: int) -> int:
    seed1, seed2 = 0x7FED7FED, 0xEEEEEEEE
    for byte in name.upper().encode("ascii"):
        seed1 = (CRYPT[kind * 256 + byte] ^ (seed1 + seed2)) & MASK
        seed2 = (byte + seed1 + seed2 + (seed2 << 5) + 3) & MASK
    return seed1


def encrypt_table(data: bytes, key: int) -> bytes:
    seed1, seed2 = key, 0xEEEEEEEE
    output = bytearray()
    for (plain,) in struct.iter_unpack("<I", data):
        seed2 = (seed2 + CRYPT[0x400 + (seed1 & 0xFF)]) & MASK
        output.extend(struct.pack("<I", (plain ^ (seed1 + seed2)) & MASK))
        seed1 = (((~seed1 << 21) + 0x11111111) | (seed1 >> 11)) & MASK
        seed2 = (plain + seed2 + (seed2 << 5) + 3) & MASK
    return bytes(output)


def dbc(data: bytes, fields: int, name: str) -> tuple[list[tuple[int, ...]], bytes]:
    if len(data) < 20:
        raise ValueError(f"Truncated DBC: {name}")
    magic, count, actual, size, strings_size = struct.unpack_from("<4s4I", data)
    if magic != b"WDBC" or actual != fields or size != fields * 4:
        raise ValueError(f"Unexpected DBC layout: {name}")
    if len(data) != 20 + count * size + strings_size:
        raise ValueError(f"Invalid DBC length: {name}")
    rows = [struct.unpack_from(f"<{fields}I", data, 20 + i * size) for i in range(count)]
    if len({row[0] for row in rows}) != count:
        raise ValueError(f"Duplicate DBC IDs: {name}")
    strings = data[20 + count * size:]
    if not strings.startswith(b"\0") or not strings.endswith(b"\0"):
        raise ValueError(f"Invalid DBC strings: {name}")
    return rows, strings


def chunks(data: bytes):
    offset = 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError("Truncated map chunk header")
        tag, size = struct.unpack_from("<4sI", data, offset)
        end = offset + 8 + size
        if end > len(data):
            raise ValueError("Truncated map chunk body")
        yield tag[::-1], data[offset + 8:end]
        offset = end


def active_tiles(wdt: bytes) -> tuple[tuple[int, int], ...]:
    mains = [body for tag, body in chunks(wdt) if tag == b"MAIN"]
    if len(mains) != 1 or len(mains[0]) != 64 * 64 * 8:
        raise ValueError("Invalid WDT MAIN")
    return tuple((i % 64, i // 64) for i in range(4096)
                 if struct.unpack_from("<I", mains[0], i * 8)[0] & 1)


def tile_whitelist(tiles: tuple[tuple[int, int], ...]) -> tuple[tuple[str, str], ...]:
    if not tiles or len(set(tiles)) != len(tiles) or any(
            not (0 <= x < 64 and 0 <= y < 64) for x, y in tiles):
        raise ValueError("Expected nonempty unique WDT terrain tiles")
    return WHITELIST[:5] + tuple(
        (f"world/maps/aiworldlab/aiworldlab_{x}_{y}.adt",
         rf"World\Maps\AIWorldLab\AIWorldLab_{x}_{y}.adt")
        for x, y in tiles)


def validate_project(files: dict[str, bytes], *, tiles: tuple[tuple[int, int], ...] = (TILE,)) -> None:
    map_rows, strings = dbc(files[WHITELIST[0][1]], 66, "Map.dbc")
    selected = [row for row in map_rows if row[0] == MAP_ID]
    if len(selected) != 1:
        raise ValueError("Missing lab MapID 725")
    row = selected[0]
    directory = strings[row[1]:].split(b"\0", 1)[0]
    if directory != b"AIWorldLab" or (row[2], row[22], row[63]) != (0, AREA_ID, 2):
        raise ValueError("Expected AIWorldLab world map 725, area 4988, WotLK")
    area_rows, _ = dbc(files[WHITELIST[1][1]], 36, "AreaTable.dbc")
    if not any(row[0] == AREA_ID and row[1] == MAP_ID for row in area_rows):
        raise ValueError("Missing lab AreaID 4988 for map 725")
    light_rows, _ = dbc(files[WHITELIST[2][1]], 15, "Light.dbc")
    if not any(row[1] == MAP_ID for row in light_rows):
        raise ValueError("Missing Light.dbc entry for map 725")
    whitelist = tile_whitelist(tiles)
    if active_tiles(files[WHITELIST[3][1]]) != tiles:
        if tiles == (TILE,):
            raise ValueError("Expected only WDT tile (30, 31)")
        raise ValueError("WDT terrain tiles differ from the selected ADTs")
    for _, internal in whitelist[5:]:
        cells = [body for tag, body in chunks(files[internal]) if tag == b"MCNK"]
        if len(cells) != 256 or any(len(body) < 128 for body in cells):
            raise ValueError(f"Expected 256 ADT terrain cells: {internal}")
        if {struct.unpack_from("<2I", body, 4) for body in cells} != {
                (x, y) for x in range(16) for y in range(16)}:
            raise ValueError(f"Missing or duplicate ADT terrain cells: {internal}")
        if any(struct.unpack_from("<I", body, 52)[0] != AREA_ID for body in cells):
            raise ValueError(f"ADT terrain must use AreaID 4988 throughout: {internal}")
    # Parse WDL boundaries too: truncation must never become a client patch.
    if not list(chunks(files[WHITELIST[4][1]])):
        raise ValueError("Empty WDL")


def build_archive(files: dict[str, bytes], whitelist: tuple[tuple[str, str], ...] = WHITELIST) -> bytes:
    names = [internal for _, internal in whitelist]
    if not names or any(not name for name in names) or len({name.upper() for name in names}) != len(names) or "(LISTFILE)" in {
            name.upper() for name in names}:
        raise ValueError("Patch whitelist names must be nonempty and unique")
    if set(files) != set(names) or any(not files[name] for name in names):
        if whitelist == WHITELIST:
            raise ValueError("Patch input must contain exactly the six nonempty whitelisted files")
        raise ValueError("Patch input must contain exactly the nonempty selected whitelisted files")
    entries = [(name, files[name]) for name in names]
    entries.append(("(listfile)", ("\r\n".join(names + ["(listfile)"]) + "\r\n").encode("ascii")))
    payload = bytearray()
    blocks = bytearray()
    # Keep the original hash layout for the default seven archive entries, but
    # reserve empty slots for larger patches so linear probing cannot cycle.
    hash_count = HASH_COUNT
    while hash_count < len(entries) * 2:
        hash_count *= 2
    hashes = [b"\xff" * 16 for _ in range(hash_count)]
    for index, (name, data) in enumerate(entries):
        offset = 32 + len(payload)
        if offset + len(data) > MASK:
            raise ValueError("MPQ exceeds the original format's 4 GiB limit")
        blocks.extend(struct.pack("<4I", offset, len(data), len(data), FILE_FLAGS))
        payload.extend(data)
        slot = mpq_hash(name, 0) & (hash_count - 1)
        while hashes[slot] != b"\xff" * 16:
            slot = (slot + 1) & (hash_count - 1)
        hashes[slot] = struct.pack("<2I2HI", mpq_hash(name, 1), mpq_hash(name, 2), 0, 0, index)
    hash_offset = 32 + len(payload)
    block_offset = hash_offset + hash_count * 16
    archive_size = block_offset + len(blocks)
    if archive_size > MASK:
        raise ValueError("MPQ exceeds the original format's 4 GiB limit")
    header = struct.pack("<4s2I2H4I", b"MPQ\x1a", 32, archive_size, 0, 3,
                         hash_offset, block_offset, hash_count, len(entries))
    return (header + payload + encrypt_table(b"".join(hashes), mpq_hash("(hash table)", 3))
            + encrypt_table(bytes(blocks), mpq_hash("(block table)", 3)))


def build_patch(project: Path, output: Path, *, all_tiles: bool = False) -> dict:
    project = project.resolve()
    output = output.resolve()
    if output.suffix.lower() != ".mpq" or output.is_relative_to(project):
        raise ValueError("Output must be an MPQ outside the authoring project")
    def source_file(relative: str) -> Path:
        source = (project / relative).resolve()
        if not source.is_relative_to(project):
            raise ValueError(f"Project input escapes its directory: {relative}")
        return source

    tiles = (TILE,)
    whitelist = WHITELIST
    if all_tiles:
        tiles = active_tiles(source_file(WHITELIST[3][0]).read_bytes())
        whitelist = tile_whitelist(tiles)
        map_directory = source_file("world/maps/aiworldlab")
        found = {path.name.lower() for path in map_directory.iterdir()
                 if path.is_file() and path.suffix.lower() == ".adt"}
        expected = {Path(relative).name.lower() for relative, _ in whitelist[5:]}
        if found != expected:
            raise ValueError(f"ADT files must match WDT terrain tiles exactly; "
                             f"missing={sorted(expected - found)}, extra={sorted(found - expected)}")
    files = {}
    for relative, internal in whitelist:
        source = source_file(relative)
        files[internal] = source.read_bytes()
    validate_project(files, tiles=tiles)
    archive = build_archive(files, whitelist)
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        # Exclusive creation protects an existing user patch, including races.
        with output.open("xb") as stream:
            stream.write(archive)
        created = True
    except FileExistsError:
        if output.read_bytes() != archive:
            raise ValueError("Existing output differs; choose a new output path instead of replacing it")
        created = False
    result = {"map_id": MAP_ID, "area_id": AREA_ID, "format_version": 0,
            "output": str(output), "created": created, "bytes": len(archive),
            "sha256": hashlib.sha256(archive).hexdigest(),
            "files": [{"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                      for name, data in files.items()]}
    if all_tiles:
        result["tiles"] = [{"x": x, "y": y} for x, y in tiles]
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=ROOT / "runtime/lab/map-project")
    parser.add_argument("--output", type=Path, default=ROOT / "runtime/lab/client-patches/patch-4.MPQ")
    parser.add_argument("--all-tiles", action="store_true",
                        help="Include exactly all ADT tiles declared by the AIWorldLab WDT")
    args = parser.parse_args()
    try:
        result = build_patch(args.project, args.output, all_tiles=args.all_tiles)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Map patch build refused: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
