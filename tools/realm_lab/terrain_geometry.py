"""Read-only Wrath ADT terrain analysis in WoW server coordinates.

Layout and axis order follow src/tools/map_extractor/{adt.h,System.cpp}.
The 9-by-9 corner vertices and 8-by-8 center vertices form four triangles
per square, as in GridMap::getHeightFromFloat (Maps/Map.cpp). ADT ix advances
along decreasing server Y; iy advances along decreasing server X. Model
coordinates follow vmap4_extractor/wmo.h and VMapManager2.cpp.

This describes terrain, not the final collision surface or navmesh. Model
floors and obstacles require regenerated VMAP data and native route checks.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
# Both TILESIZE and SIZE_OF_GRIDS in the native tools round to this float32.
GRID_SIZE = struct.unpack("<f", struct.pack("<f", 533.33333))[0]
UNIT_SIZE = GRID_SIZE / 128
MIDPOINT = GRID_SIZE * 32
DEFAULT_ADT = ROOT / "runtime/lab/map-project/world/maps/aiworldlab/aiworldlab_30_31.adt"
DEFAULT_POPULATION = ROOT / "data/realm_lab/aiworldlab/hunt-population-100.json"
DEFAULT_POINTS = ROOT / "data/realm_lab/aiworldlab/test-points.json"


def chunks(data: bytes):
    """Yield strict top-level chunks; nested MCNR has historical padding."""
    offset = 0
    while offset < len(data):
        if len(data) - offset < 8:
            raise ValueError("Truncated ADT chunk header")
        raw_tag, size = struct.unpack_from("<4sI", data, offset)
        end = offset + 8 + size
        if end > len(data):
            raise ValueError("Truncated ADT chunk body")
        yield raw_tag[::-1], data[offset + 8:end]
        offset = end


def referenced_chunk(cell: bytes, offset: int, tag: bytes) -> bytes:
    # MCNK offsets are relative to its chunk header, before this body.
    start = offset - 8
    if start < 128 or start + 8 > len(cell):
        raise ValueError(f"Invalid MCNK {tag.decode()} offset")
    raw_tag, size = struct.unpack_from("<4sI", cell, start)
    if raw_tag[::-1] != tag or start + 8 + size > len(cell):
        raise ValueError(f"Invalid MCNK {tag.decode()} chunk")
    return cell[start + 8:start + 8 + size]


def triangle_coefficients(h1: float, h2: float, h3: float, h4: float,
                          center: float) -> tuple[tuple[float, float, float], ...]:
    """Planes z=a*u+b*v+c: top, left, right, bottom of the diamond."""
    h5 = 2 * center
    return ((h2 - h1, h5 - h1 - h2, h1),
            (h5 - h1 - h3, h3 - h1, h1),
            (h2 + h4 - h5, h4 - h2, h5 - h4),
            (h4 - h3, h3 + h4 - h5, h5 - h4))


def triangle_index(u: float, v: float) -> int:
    if u + v < 1:
        return 0 if u > v else 1
    return 2 if u > v else 3


def _names(data: bytes, indices: bytes, kind: str) -> list[str]:
    if len(indices) % 4:
        raise ValueError(f"Invalid {kind} model name indices")
    names = []
    for (offset,) in struct.iter_unpack("<I", indices):
        if offset >= len(data) or (offset and data[offset - 1] != 0):
            raise ValueError(f"Invalid {kind} model name offset")
        end = data.find(b"\0", offset)
        if end < 0 or end == offset:
            raise ValueError(f"Invalid {kind} model name string")
        names.append(data[offset:end].decode("ascii"))
    return names


def model_server_position(raw: tuple[float, float, float]) -> dict[str, float]:
    if not all(math.isfinite(value) for value in raw):
        raise ValueError("Non-finite model coordinates")
    return {"x": MIDPOINT - raw[2], "y": MIDPOINT - raw[0], "z": raw[1]}


def model_placements(top: dict[bytes, bytes]) -> list[dict]:
    placements = []
    for kind, placement_tag, names_tag, index_tag, size in (
            ("m2", b"MDDF", b"MMDX", b"MMID", 36),
            ("wmo", b"MODF", b"MWMO", b"MWID", 64)):
        records = top.get(placement_tag, b"")
        if len(records) % size:
            raise ValueError(f"Invalid {placement_tag.decode()} record length")
        names = _names(top.get(names_tag, b""), top.get(index_tag, b""), kind)
        for offset in range(0, len(records), size):
            name_id, unique_id = struct.unpack_from("<2I", records, offset)
            if name_id >= len(names):
                raise ValueError("Model placement references missing name")
            position = struct.unpack_from("<3f", records, offset + 8)
            rotation = struct.unpack_from("<3f", records, offset + 20)
            if not all(math.isfinite(value) for value in rotation):
                raise ValueError("Non-finite model rotation")
            item = {"kind": kind, "name": names[name_id], "unique_id": unique_id,
                    "raw_position": list(position), "server_position": model_server_position(position),
                    "rotation": list(rotation)}
            if kind == "m2":
                scale, flags = struct.unpack_from("<2H", records, offset + 32)
                item.update(scale=scale / 1024, flags=flags)
            else:
                raw_min = struct.unpack_from("<3f", records, offset + 32)
                raw_max = struct.unpack_from("<3f", records, offset + 44)
                minimum, maximum = model_server_position(raw_max), model_server_position(raw_min)
                item["server_bounds"] = {
                    "min_x": minimum["x"], "max_x": maximum["x"],
                    "min_y": minimum["y"], "max_y": maximum["y"],
                    "min_z": maximum["z"], "max_z": minimum["z"]}
                flags, doodad_set, name_set, scale = struct.unpack_from("<4H", records, offset + 56)
                item.update(flags=flags, doodad_set=doodad_set, name_set=name_set,
                            scale_field=scale)
            placements.append(item)
    return placements


@dataclass
class Terrain:
    tile_x: int
    tile_y: int
    v9: list[list[float]]
    v8: list[list[float]]
    holes: dict[tuple[int, int], int]
    counts: dict
    models: list[dict]
    seam_max_delta: float

    @property
    def origin(self) -> tuple[float, float]:
        return (32 - self.tile_y) * GRID_SIZE, (32 - self.tile_x) * GRID_SIZE

    def square(self, x: float, y: float) -> tuple[int, int, float, float]:
        if not math.isfinite(x) or not math.isfinite(y):
            raise ValueError("Non-finite terrain query")
        ox, oy = self.origin
        u, v = (ox - x) / UNIT_SIZE, (oy - y) / UNIT_SIZE
        # Include both tile edges; reject coordinates outside rather than wrap.
        if not (0 <= u <= 128 and 0 <= v <= 128):
            raise ValueError("Terrain query outside ADT tile")
        row, col = min(int(u), 127), min(int(v), 127)
        return row, col, u - row, v - col

    def is_hole(self, row: int, col: int) -> bool:
        mask = self.holes[(col // 8, row // 8)]
        return bool(mask & (1 << ((row % 8 // 2) * 4 + col % 8 // 2)))

    def planes(self, row: int, col: int):
        return triangle_coefficients(self.v9[row][col], self.v9[row + 1][col],
                                     self.v9[row][col + 1], self.v9[row + 1][col + 1],
                                     self.v8[row][col])

    def sample(self, x: float, y: float) -> dict[str, float | int]:
        row, col, u, v = self.square(x, y)
        if self.is_hole(row, col):
            raise ValueError("Terrain query is inside a terrain hole")
        index = triangle_index(u, v)
        a, b, c = self.planes(row, col)[index]
        return {"ground_z": a * u + b * v + c,
                "triangle_slope_degrees": math.degrees(math.atan(math.hypot(a, b) / UNIT_SIZE)),
                "square_row": row, "square_col": col, "triangle": index}

    def height(self, x: float, y: float) -> float:
        return float(self.sample(x, y)["ground_z"])


def read_terrain(data: bytes, tile_x: int = 30, tile_y: int = 31) -> Terrain:
    if not (0 <= tile_x < 64 and 0 <= tile_y < 64):
        raise ValueError("Invalid ADT tile coordinates")
    top = {}
    cells = {}
    cell_locations = {}
    file_offset = 0
    for tag, body in chunks(data):
        chunk_offset = file_offset
        file_offset += 8 + len(body)
        if tag != b"MCNK":
            if tag in top:
                raise ValueError("Duplicate ADT top-level chunk")
            top[tag] = body
            continue
        if len(body) < 128:
            raise ValueError("Truncated MCNK header")
        ix, iy = struct.unpack_from("<2I", body, 4)
        if (ix, iy) in cells or not (0 <= ix < 16 and 0 <= iy < 16):
            raise ValueError("Duplicate or invalid MCNK index")
        cells[(ix, iy)] = body
        cell_locations[(ix, iy)] = (chunk_offset, len(body) + 8)
    if top.get(b"MVER") != struct.pack("<I", 18):
        raise ValueError("Expected Wrath ADT version 18")
    if len(cells) != 256:
        raise ValueError("Expected complete ADT with 256 terrain cells")
    mcin = top.get(b"MCIN", b"")
    if len(mcin) != 256 * 16:
        raise ValueError("Expected complete MCIN terrain index")
    for iy in range(16):
        for ix in range(16):
            location = struct.unpack_from("<2I", mcin, (iy * 16 + ix) * 16)
            if location != cell_locations[(ix, iy)]:
                raise ValueError("MCIN entry does not match its MCNK location")
    v9 = [[math.nan] * 129 for _ in range(129)]
    v8 = [[math.nan] * 128 for _ in range(128)]
    holes, areas, liquid_cells = {}, Counter(), 0
    seam_max_delta = 0.0
    ox, oy = (32 - tile_y) * GRID_SIZE, (32 - tile_x) * GRID_SIZE
    for iy in range(16):
        for ix in range(16):
            body = cells[(ix, iy)]
            raw_x, raw_y, base_z = struct.unpack_from("<3f", body, 104)
            expected_x, expected_y = ox - iy * GRID_SIZE / 16, oy - ix * GRID_SIZE / 16
            if not all(math.isfinite(value) for value in (raw_x, raw_y, base_z)):
                raise ValueError("Non-finite MCNK position")
            if abs(raw_x - expected_x) > 0.02 or abs(raw_y - expected_y) > 0.02:
                raise ValueError("MCNK position does not match tile and chunk index")
            height_offset = struct.unpack_from("<I", body, 20)[0]
            heights = referenced_chunk(body, height_offset, b"MCVT")
            if len(heights) != 145 * 4:
                raise ValueError("Expected 145 MCVT height values")
            values = struct.unpack("<145f", heights)
            if not all(math.isfinite(value) for value in values):
                raise ValueError("Non-finite MCVT height")
            for row in range(9):
                for col in range(9):
                    target_row, target_col = iy * 8 + row, ix * 8 + col
                    value = base_z + values[row * 17 + col]
                    previous = v9[target_row][target_col]
                    if math.isfinite(previous):
                        seam_max_delta = max(seam_max_delta, abs(previous - value))
                    v9[target_row][target_col] = value
            for row in range(8):
                for col in range(8):
                    v8[iy * 8 + row][ix * 8 + col] = base_z + values[row * 17 + 9 + col]
            areas[struct.unpack_from("<I", body, 52)[0]] += 1
            holes[(ix, iy)] = struct.unpack_from("<I", body, 60)[0] & 0xFFFF
            liquid_offset, liquid_size = struct.unpack_from("<2I", body, 96)
            if liquid_size > 8:
                liquid = referenced_chunk(body, liquid_offset, b"MCLQ")
                if len(liquid) < 720:
                    raise ValueError("Truncated legacy MCLQ liquid data")
                liquid_cells += any(flag != 0x0F for flag in liquid[656:720])
    modern_instances = 0
    modern_cells = 0
    liquid_types = Counter()
    mh2o = top.get(b"MH2O")
    if mh2o is not None:
        if len(mh2o) < 256 * 12:
            raise ValueError("Truncated MH2O cell table")
        for offset in range(0, 256 * 12, 12):
            instances, count, attributes = struct.unpack_from("<3I", mh2o, offset)
            if not count:
                continue
            if instances < 256 * 12 or instances + count * 24 > len(mh2o):
                raise ValueError("Invalid MH2O liquid instance offset")
            if attributes and (attributes < 256 * 12 or attributes + 16 > len(mh2o)):
                raise ValueError("Invalid MH2O liquid attributes offset")
            modern_instances += count
            modern_cells += 1
            for index in range(count):
                liquid_type = struct.unpack_from("<H", mh2o, instances + index * 24)[0]
                liquid_types[liquid_type] += 1
    models = model_placements(top)
    counts = {"terrain_cells": len(cells), "terrain_squares": 128 * 128,
              "terrain_triangles": 128 * 128 * 4,
              "area_cells": dict(sorted(areas.items())),
              "m2_placements": sum(model["kind"] == "m2" for model in models),
              "wmo_placements": sum(model["kind"] == "wmo" for model in models),
              "legacy_liquid_cells": liquid_cells, "modern_liquid_cells": modern_cells,
              "modern_liquid_instances": modern_instances,
              "modern_liquid_types": dict(sorted(liquid_types.items())),
              "hole_cells": sum(bool(mask) for mask in holes.values()),
              "hole_blocks": sum(mask.bit_count() for mask in holes.values()),
              "hole_squares": sum(mask.bit_count() for mask in holes.values()) * 4}
    return Terrain(tile_x, tile_y, v9, v8, holes, counts, models, seam_max_delta)


def read_float_map(data: bytes, tile_x: int = 30, tile_y: int = 31) -> Terrain:
    """Read the extractor's -f 0 MAPS/v10/12340 float height section.

    Int-packed, flat, or flight-bound variants are deliberately rejected.
    Layout follows map_fileheader/map_heightHeader in Maps/Map.h.
    """
    if len(data) < 44:
        raise ValueError("Truncated extracted map header")
    magic, version, build, *sections = struct.unpack_from("<4s10I", data)
    if (magic, version, build) != (b"MAPS", 10, 12340):
        raise ValueError("Expected extracted Wrath MAPS version 10, build 12340")
    previous_end = 44
    for offset, size in zip(sections[::2], sections[1::2]):
        if not size:
            if offset:
                raise ValueError("Invalid empty extracted map section")
            continue
        if offset != previous_end or offset + size > len(data):
            raise ValueError("Invalid extracted map section bounds")
        previous_end = offset + size
    if previous_end != len(data):
        raise ValueError("Unexpected extracted map trailing data")
    height_offset, height_size = sections[2:4]
    expected_size = 16 + (129 * 129 + 128 * 128) * 4
    if height_size != expected_size:
        raise ValueError("Expected complete extracted float height arrays")
    magic, flags, minimum, maximum = struct.unpack_from("<4sI2f", data, height_offset)
    if magic != b"MHGT" or flags != 0:
        raise ValueError("Expected MHGT float32 heights with flags=0 (-f 0)")
    values = struct.unpack_from(f"<{129 * 129 + 128 * 128}f", data, height_offset + 16)
    if not all(math.isfinite(value) for value in (*values, minimum, maximum)):
        raise ValueError("Non-finite extracted map height")
    v9 = [list(values[row * 129:(row + 1) * 129]) for row in range(129)]
    v8_values = values[129 * 129:]
    v8 = [list(v8_values[row * 128:(row + 1) * 128]) for row in range(128)]
    holes_offset, holes_size = sections[6:8]
    if holes_size not in (0, 16 * 16 * 2):
        raise ValueError("Invalid extracted map holes array")
    masks = struct.unpack_from("<256H", data, holes_offset) if holes_size else [0] * 256
    holes = {(col, row): masks[row * 16 + col] for row in range(16) for col in range(16)}
    return Terrain(tile_x, tile_y, v9, v8, holes,
                   {"height_flags": flags, "height_min": minimum, "height_max": maximum}, [], 0)


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def gridmap_height(terrain: Terrain, x: float, y: float) -> float:
    """Reproduce GridMap::getHeightFromFloat scalar float32 arithmetic.

    The global 32-X/GRID_SIZE computation loses slightly more precision than
    the local ADT interpolation. Heights remain terrain heights; VMAP/model
    floors and navmesh projections are intentionally not substituted.
    """
    terrain.square(x, y)  # Reject non-finite or outside coordinates first.
    u = f32(128 * f32(32 - f32(f32(x) / GRID_SIZE)))
    v = f32(128 * f32(32 - f32(f32(y) / GRID_SIZE)))
    global_row, global_col = int(u), int(v)
    if global_row // 128 != terrain.tile_y or global_col // 128 != terrain.tile_x:
        raise ValueError("Native height query selects a neighboring extracted map")
    u, v = f32(u - global_row), f32(v - global_col)
    row, col = global_row & 127, global_col & 127
    if terrain.is_hole(row, col):
        raise ValueError("Terrain query is inside a terrain hole")
    h1, h2 = terrain.v9[row][col], terrain.v9[row + 1][col]
    h3, h4 = terrain.v9[row][col + 1], terrain.v9[row + 1][col + 1]
    h5 = f32(2 * terrain.v8[row][col])
    if f32(u + v) < 1:
        if u > v:
            a, b, c = f32(h2 - h1), f32(f32(h5 - h1) - h2), h1
        else:
            a, b, c = f32(f32(h5 - h1) - h3), f32(h3 - h1), h1
    elif u > v:
        a, b, c = f32(f32(h2 + h4) - h5), f32(h4 - h2), f32(h5 - h4)
    else:
        a, b, c = f32(h4 - h3), f32(f32(h3 + h4) - h5), f32(h5 - h4)
    return f32(f32(f32(a * u) + f32(b * v)) + c)


def analyze(terrain: Terrain, source_data: bytes, population: dict,
            points: dict, source_path: str) -> dict:
    heights = [height for row in terrain.v9 + terrain.v8 for height in row]
    maximum, steep55, steep70, valid_triangles = 0.0, 0, 0, 0
    maximum_location = None
    for row in range(128):
        for col in range(128):
            if terrain.is_hole(row, col):
                continue
            for triangle, (a, b, _) in enumerate(terrain.planes(row, col)):
                slope = math.degrees(math.atan(math.hypot(a, b) / UNIT_SIZE))
                valid_triangles += 1
                steep55 += slope > 55
                steep70 += slope > 70
                if slope > maximum:
                    maximum = slope
                    maximum_location = {"square_row": row, "square_col": col,
                                        "triangle": triangle}
    agents = []
    for actor in population["agents"]:
        home = actor["home"]
        sample = terrain.sample(home["x"], home["y"])
        agents.append({"spawn_id": actor["spawn_id"], "entry": actor["entry"],
                       "role": actor["role"], "label": actor["label"],
                       "x": home["x"], "y": home["y"], "old_z": home["z"],
                       **sample,
                       "height_change": sample["ground_z"] - home["z"]})
    test_points = [{"name": "home", "x": points["home"]["x"], "y": points["home"]["y"]}]
    test_points.extend({"name": point["name"], "x": point["x"], "y": point["y"]}
                       for point in points["return_points"])
    for point in test_points:
        point.update(terrain.sample(point["x"], point["y"]))
        point["player_entry_z"] = point["ground_z"] + 2
    risks = []
    if steep55:
        risks.append("Terrain includes triangles steeper than the generator's default 55 degree walkable limit; native navmesh routes must verify reachability.")
    if terrain.models:
        risks.append("Model spawns require regenerated VMAP collision and mmap data. Terrain interpolation cannot identify model floors, walls, or door passages.")
    if terrain.counts["hole_squares"]:
        risks.append("Terrain holes are present; terrain height queries fail at holes.")
    if terrain.counts["legacy_liquid_cells"] or terrain.counts["modern_liquid_cells"]:
        risks.append("Liquid cells are present; swimming and walkability require native liquid/navmesh validation.")
    if terrain.seam_max_delta > 0.001:
        risks.append("Shared chunk corner heights disagree; the extractor's row-major overwrite order was reproduced.")
    ox, oy = terrain.origin
    return {"schema_version": 1, "source_revision": "v3", "map_id": 725,
            "source": {"path": source_path, "bytes": len(source_data),
                       "sha256": hashlib.sha256(source_data).hexdigest()},
            "coordinate_system": "WoW server X/Y/Z in yards",
            "coordinate_evidence": [
                "src/tools/map_extractor/adt.h: MCNK offsets, 145 MCVT values, base height at body offset 112",
                "src/tools/map_extractor/System.cpp: V9/V8 layout and ix/iy transposition",
                "src/server/game/Maps/Map.cpp: GridMap::getHeightFromFloat four triangle interpolation",
                "src/tools/mmaps_generator/TerrainBuilder.h: float32 GRID_SIZE=533.3333f",
                "src/tools/vmap4_extractor/wmo.h and src/common/Collision/Management/VMapManager2.cpp: model axis conversion"],
            "grid_size": GRID_SIZE, "unit_size": UNIT_SIZE,
            "tile": {"adt_x": terrain.tile_x, "adt_y": terrain.tile_y,
                     "server_grid_x": terrain.tile_y, "server_grid_y": terrain.tile_x},
            "bounds": {"min_x": ox - GRID_SIZE, "max_x": ox,
                       "min_y": oy - GRID_SIZE, "max_y": oy},
            "height_min": min(heights), "height_max": max(heights),
            "height_range": max(heights) - min(heights), "counts": terrain.counts,
            "seam_max_delta": terrain.seam_max_delta,
            "slopes": {"maximum_degrees": maximum, "maximum_location": maximum_location,
                       "valid_triangles": valid_triangles, "above_55_degrees": steep55,
                       "above_70_degrees": steep70},
            "model_placements": terrain.models, "agents": agents, "test_points": test_points,
            "passability_risks": risks,
            "scope_note": "ADT terrain interpolation only; final extracted map heights may be quantized and model collision surfaces may differ. No path or physical movement approval is implied."}


def write_nav_points(report: dict, output: Path) -> None:
    rows = ["kind\tid\trole\tx\ty\tz"]
    for actor in report["agents"]:
        rows.append(f"home\t{actor['spawn_id']}\t{actor['role']}\t{actor['x']:.9f}\t{actor['y']:.9f}\t{actor['ground_z']:.9f}")
    for point in report["test_points"]:
        if point["name"] == "home":
            continue
        rows.append(f"test\t{point['name']}\t-\t{point['x']:.9f}\t{point['y']:.9f}\t{point['ground_z']:.9f}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(("\n".join(rows) + "\n").encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adt", type=Path, default=DEFAULT_ADT)
    parser.add_argument("--population", type=Path, default=DEFAULT_POPULATION)
    parser.add_argument("--test-points", type=Path, default=DEFAULT_POINTS)
    parser.add_argument("--output", type=Path, default=ROOT / "runtime/lab-validation/terrain-v3/terrain-analysis.json")
    parser.add_argument("--nav-points", type=Path, default=ROOT / "runtime/lab-validation/terrain-v3/nav-points.tsv")
    args = parser.parse_args()
    source = args.adt.read_bytes()
    report = analyze(read_terrain(source), source,
                     json.loads(args.population.read_text(encoding="utf-8")),
                     json.loads(args.test_points.read_text(encoding="utf-8")), str(args.adt.resolve()))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    write_nav_points(report, args.nav_points)
    print(json.dumps({"source_sha256": report["source"]["sha256"],
                      "height_min": report["height_min"], "height_max": report["height_max"],
                      "counts": report["counts"], "slopes": report["slopes"],
                      "homes": len(report["agents"]), "output": str(args.output)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
