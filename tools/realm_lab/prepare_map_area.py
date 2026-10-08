"""Complete a saved Noggit lab map's AreaTable and terrain area references.

Client files and server data are untouched. Close Noggit before --prepare.
Existing project files are backed up before fixed-size terrain edits.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_dbc(path: Path, fields: int):
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"Short DBC: {path.name}")
    magic, count, actual_fields, size, string_size = struct.unpack_from("<4s4I", data)
    if magic != b"WDBC" or actual_fields != fields or size != fields * 4:
        raise ValueError(f"Unexpected DBC layout: {path.name}")
    if len(data) != 20 + count * size + string_size:
        raise ValueError(f"Invalid DBC length: {path.name}")
    rows = [struct.unpack_from(f"<{fields}I", data, 20 + i * size) for i in range(count)]
    if len({row[0] for row in rows}) != count:
        raise ValueError(f"Duplicate DBC IDs: {path.name}")
    strings = data[20 + count * size:]
    if not strings.startswith(b"\0") or not strings.endswith(b"\0"):
        raise ValueError(f"Invalid string block: {path.name}")
    return data, rows, strings


def text(strings: bytes, offset: int) -> str:
    if offset >= len(strings):
        raise ValueError("Invalid string offset")
    return strings[offset:].split(b"\0", 1)[0].decode("utf-8")


def chunks(data: bytes):
    offset = 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError("Truncated chunk header")
        tag, size = struct.unpack_from("<4sI", data, offset)
        end = offset + 8 + size
        if end > len(data):
            raise ValueError("Truncated chunk body")
        yield tag[::-1], offset + 8, size
        offset = end


def editor_closed():
    if os.name == "nt":
        result = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command",
             "if (Get-Process -Name noggit -ErrorAction SilentlyContinue) { exit 2 }"],
            capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW,
        )
        if result.returncode:
            raise ValueError("Close Noggit before preparing its project files")


def prepare(args):
    if not 1 <= args.area_id <= 65535:
        raise ValueError("AreaID must fit the map extractor's uint16 range (1..65535)")
    project = args.project.resolve()
    base_data, base_rows, base_strings = read_dbc(args.base_area, 36)
    if args.area_id in {row[0] for row in base_rows}:
        raise ValueError("Lab AreaID collides with the baseline client")
    _, base_map_rows, base_map_strings = read_dbc(args.base_map, 66)
    _, map_rows, map_strings = read_dbc(project / "DBFilesClient/Map.dbc", 66)
    map_by_id = {row[0]: row for row in map_rows}
    if any(map_by_id.get(row[0]) != row for row in base_map_rows):
        raise ValueError("Baseline map records were modified")
    if not map_strings.startswith(base_map_strings):
        raise ValueError("Baseline map strings were modified")
    if args.map_id in {row[0] for row in base_map_rows}:
        raise ValueError("Lab MapID collides with the baseline client")
    selected = [row for row in map_rows if row[0] == args.map_id]
    if len(selected) != 1 or not 1 <= args.map_id <= 999:
        raise ValueError("Invalid or missing lab MapID")
    map_row = selected[0]
    if map_row[2] != 0 or map_row[22] != args.area_id or map_row[63] != 2:
        raise ValueError("Expected WotLK world map with the selected AreaID")
    directory = text(map_strings, map_row[1])
    name = text(map_strings, map_row[5])
    if not directory or directory in (".", "..") or any(c in directory for c in "/\\:"):
        raise ValueError("Invalid map directory")
    world = project / "world/maps" / directory.lower()
    wdt = (world / f"{directory.lower()}.wdt").read_bytes()
    mains = [(start, size) for tag, start, size in chunks(wdt) if tag == b"MAIN"]
    if len(mains) != 1 or mains[0][1] != 64 * 64 * 8:
        raise ValueError("Invalid WDT MAIN")
    active = [(i % 64, i // 64) for i in range(64 * 64)
              if struct.unpack_from("<I", wdt, mains[0][0] + i * 8)[0] & 1]
    if len(active) != 1:
        raise ValueError("This initial lab project requires one active tile")
    x, y = active[0]
    tile = world / f"{directory.lower()}_{x}_{y}.adt"
    original = tile.read_bytes()
    updated = bytearray(original)
    cells = [(start, size) for tag, start, size in chunks(original) if tag == b"MCNK"]
    if len(cells) != 256 or any(size < 128 for _, size in cells):
        raise ValueError("Invalid ADT cell layout")
    if {struct.unpack_from("<2I", original, start + 4) for start, _ in cells} != {
            (cx, cy) for cx in range(16) for cy in range(16)}:
        raise ValueError("Missing or duplicate terrain cells")
    before_areas = sorted({struct.unpack_from("<I", original, start + 52)[0]
                           for start, _ in cells})
    for start, _ in cells:
        struct.pack_into("<I", updated, start + 52, args.area_id)
    allowed = {offset for start, _ in cells for offset in range(start + 52, start + 56)}
    if any(i not in allowed for i, (a, b) in enumerate(zip(original, updated)) if a != b):
        raise ValueError("Unexpected terrain modification")

    area_path = project / "DBFilesClient/AreaTable.dbc"
    if area_path.exists():
        area_data, rows, strings = read_dbc(area_path, 36)
    else:
        area_data, rows, strings = base_data, base_rows, base_strings
    baseline_by_id = {row[0]: row for row in base_rows}
    current_by_id = {row[0]: row for row in rows}
    if any(current_by_id.get(key) != row for key, row in baseline_by_id.items()):
        raise ValueError("Baseline area records were modified")
    if not strings.startswith(base_strings):
        raise ValueError("Baseline area strings were modified")
    found = current_by_id.get(args.area_id)
    if found:
        if (found[1], found[2], text(strings, found[11])) != (args.map_id, 0, name):
            raise ValueError("Existing lab AreaID has a different owner or name")
        if sum(row[3] == found[3] for row in rows) != 1 or found[3] >= 4096:
            raise ValueError("Invalid or duplicate exploration bit")
        area_bit = found[3]
        new_area_data = area_data
    else:
        area_bit = max(row[3] for row in rows) + 1
        if area_bit >= 4096:
            raise ValueError("No exploration bit available within the client limit")
        row = [0] * 36
        row[0:5] = [args.area_id, args.map_id, 0, area_bit, 0x04000000]
        row[11] = len(strings)
        row[27] = baseline_by_id[12][27]  # Existing client's localized-string mask.
        row[34] = struct.unpack("<I", struct.pack("<f", 1.0))[0]
        new_strings = strings + name.encode("utf-8") + b"\0"
        records = area_data[20:20 + len(rows) * 144] + struct.pack("<36I", *row)
        new_area_data = struct.pack("<4s4I", b"WDBC", len(rows) + 1, 36, 144,
                                    len(new_strings)) + records + new_strings
    changes = {path: data for path, data in ((area_path, new_area_data),
                                             (tile, bytes(updated)))
               if not path.exists() or path.read_bytes() != data}
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    backup = project.parent / "map-project-backups" / stamp
    if args.prepare and changes:
        editor_closed()
        for path in changes:
            if path.exists():
                destination = backup / path.relative_to(project)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(path.read_bytes())
        for path, data in changes.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = path.with_name(path.name + ".lab-tmp")
            temporary.write_bytes(data)
            temporary.replace(path)
    elif changes:
        raise ValueError("Area record or terrain assignments missing; use --prepare with Noggit closed")
    report = {
        "map_id": args.map_id, "area_id": args.area_id, "area_bit": area_bit,
        "name": name, "directory": directory, "instance_type": map_row[2],
        "expansion": map_row[63], "active_tiles": active, "terrain_cells": len(cells),
        "previous_terrain_area_ids": before_areas,
        "baseline_area_records_preserved": True, "baseline_map_records_preserved": True,
        "area_dbc_sha256": sha(new_area_data), "adt_sha256": sha(bytes(updated)),
        "changed_files": [str(path.relative_to(project)) for path in changes],
        "backup": str(backup) if args.prepare and changes else None,
    }
    if args.prepare:
        receipt = project.parent / "map-area-receipt.json"
        receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=ROOT / "runtime/lab/map-project")
    parser.add_argument("--base-area", type=Path,
                        default=ROOT / "runtime/lab/client-baseline/dbc/AreaTable.dbc")
    parser.add_argument("--base-map", type=Path,
                        default=ROOT / "runtime/lab/client-baseline/dbc/Map.dbc")
    parser.add_argument("--map-id", type=int, default=725)
    parser.add_argument("--area-id", type=int, default=4988)
    parser.add_argument("--prepare", action="store_true")
    args = parser.parse_args()
    try:
        prepare(args)
    except (ValueError, OSError) as error:
        parser.exit(1, f"ERROR: {error}\n")


if __name__ == "__main__":
    main()
