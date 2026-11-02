"""Pinned native geometry for an empty authored map, independent of NPC layouts.

The legacy v8 population contracts remain immutable. Only authoring-empty can
select a reviewed empty-map bundle; it still checks every byte and the complete
map-725 footprint before a world starts.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re

try:
    from . import settlement_population as settlement
except ImportError:
    import settlement_population as settlement

ROOT = Path(__file__).resolve().parents[2]
METADATA = ROOT / "data/realm_lab/aiworldlab"
INDEX = "authoring-map.json"
REVISION = "preview-v9"
SUPPORTED_REVISIONS = frozenset({REVISION, "preview-v10", "preview-v11", "preview-v12"})
# Historical population loaders must not follow a newly selected authoring map.
# These immutable pins certify their original evidence independently of INDEX.
FROZEN_REVISION_PINS = {
    "preview-v10": {
        "server_manifest_sha256": "6555a53b2461cd1e549146d590ded78ea4cfc85e2995e959ea9f92334c475408",
        "verification_sha256": "eff8d27982dbf5cf9c1d5bc1e697675279e52b8daa934817a7eba6fcd5b514bc",
        "entry_proof_sha256": "3251febfad6510ab63653eb003f67110f2623dd1b806d0eb7866afe9ac771cfa",
    },
    "preview-v11": {
        "server_manifest_sha256": "6d29b517b72f97363b51ea8fa29014606bb4103d29b527d57a97e2320250bd12",
        "verification_sha256": "d3e4224936042d8a7f5abae8e268e11844def61b07416826ef0362bd449c893c",
        "entry_proof_sha256": "5409c6a3c02f1c1fe59d21eb6da2bf342e391b603846222d995dd0144c35d7ee",
    },
    "preview-v12": {
        "server_manifest_sha256": "c655c7ea785ca8cc64a9fbadb53a4d0a627b2ee05338ea63f2520c5d5a46514c",
        "verification_sha256": "180ee3995c3a8b2755fd63c90095a5774edeed0155098aaf81a7ea4eb5ee424e",
        "entry_proof_sha256": "5b33dee769eade1b23597fb55edcd8019d7a6c6754c74c1422ca5cda108004e0",
    },
}
TILES = ((30, 31), (30, 32), (31, 31), (31, 32))
DBC_FILES = {"dbc/Map.dbc", "dbc/AreaTable.dbc", "dbc/Light.dbc"}
MAP_FILES = {f"maps/725{y:02}{x:02}.map" for x, y in TILES}
MMAP_FILES = {"mmaps/725.mmap", *(f"mmaps/725{y:02}{x:02}.mmtile" for x, y in TILES)}
VMAP_TILES = {f"vmaps/725_{x:02}_{y:02}.vmtile" for x, y in TILES}
require = settlement.require


def _reject_constant(value):
    raise ValueError("Non-finite map metadata")


def _read(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"), parse_constant=_reject_constant)
    require(isinstance(value, dict), "Authored map metadata must be an object")
    return value


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _pinned(path: Path, expected) -> dict:
    require(isinstance(expected, str) and re.fullmatch(r"[0-9a-f]{64}", expected) is not None and
            _digest(path) == expected, f"Authored map metadata hash mismatch: {path.name}")
    return _read(path)


def _tiles(document: dict) -> None:
    tiles = document.get("tiles")
    require(isinstance(tiles, list) and len(tiles) == 4 and
            all(isinstance(tile, list) and len(tile) == 2 and all(type(v) is int for v in tile)
                for tile in tiles) and {tuple(tile) for tile in tiles} == set(TILES),
            "Authored map must contain exactly the four reviewed tiles")


@dataclass(frozen=True)
class GeometryBundle:
    geometry_files: tuple[dict, ...]
    metadata: Path
    entry_proof: dict
    verification_sha256: str
    entry_proof_sha256: str
    source_revision: str = REVISION
    actors: tuple = ()

    @classmethod
    def load(cls, metadata: Path = METADATA, *, revision: str | None = None) -> GeometryBundle:
        try:
            if revision is not None:
                require(revision in SUPPORTED_REVISIONS, "Unsupported explicit authored revision")
            if revision in FROZEN_REVISION_PINS:
                index = {"schema_version": 1, "profile": "authoring-empty", "realm_id": 2,
                         "map_id": 725, "area_id": 4988, "source_revision": revision,
                         **FROZEN_REVISION_PINS[revision]}
            else:
                index = _read(metadata / INDEX)
                require(revision is None or index.get("source_revision") == revision,
                        "Explicit authored revision is not pinned by the selected index")
            revision = index.get("source_revision")
            require((index.get("schema_version"), index.get("profile"), index.get("realm_id"),
                     index.get("map_id"), index.get("area_id")) ==
                    (1, "authoring-empty", 2, 725, 4988) and
                    isinstance(revision, str) and revision in SUPPORTED_REVISIONS,
                    "Wrong empty-map identity or unsupported authored revision")
            directory = metadata / revision
            manifest = _pinned(directory / "server-manifest.json", index.get("server_manifest_sha256"))
            require((manifest.get("schema_version"), manifest.get("map_id"), manifest.get("area_id"),
                     manifest.get("source_revision")) == (1, 725, 4988, revision),
                    "Wrong authored native bundle identity")
            _tiles(manifest)
            files = manifest.get("files")
            require(isinstance(files, list) and 13 <= len(files) <= 2000,
                    "Incomplete or unbounded authored native bundle")
            records = {}
            for row in files:
                require(isinstance(row, dict), "Invalid native geometry file")
                name = row.get("path")
                require(isinstance(name, str) and name not in records and
                        re.fullmatch(r"(?:dbc|maps|mmaps|vmaps)/[A-Za-z0-9_ .-]+", name) is not None and
                        name.split("/")[1] not in (".", "..") and
                        type(row.get("bytes")) is int and 0 < row["bytes"] <= 100000000 and
                        isinstance(row.get("sha256"), str) and
                        re.fullmatch(r"[0-9a-f]{64}", row["sha256"]) is not None,
                        "Native geometry path, size, identity or hash invalid")
                require(name in DBC_FILES | MAP_FILES | MMAP_FILES | VMAP_TILES | {"vmaps/725.vmtree"} or
                        (name.startswith("vmaps/") and name.endswith(".vmo")),
                        "Authored bundle escapes the reviewed map-725 file closure")
                records[name] = row
            required = DBC_FILES | MAP_FILES | MMAP_FILES | {"vmaps/725.vmtree"}
            require(required <= records.keys() and records.keys() & VMAP_TILES and
                    any(name.endswith(".vmo") for name in records), "Incomplete map/terrain/navigation/model closure")
            proof = _pinned(directory / "data-verification.json", index.get("verification_sha256"))
            require((proof.get("schema_version"), proof.get("source_revision"), proof.get("map_id"),
                     proof.get("area_id")) == (1, revision, 725, 4988) and proof.get("status") == "pass" and
                    proof.get("ok") is True and proof.get("errors") == [] and
                    proof.get("navigation_checked") is True and proof.get("geometry_files") == files,
                    "Missing or mismatched native authored-map verification")
            _tiles(proof)
            for label, prefix, expected in (("maps", "maps/", MAP_FILES),
                                             ("mmtiles", "mmaps/", MMAP_FILES - {"mmaps/725.mmap"})):
                rows = proof.get(label)
                require(isinstance(rows, list), "Native terrain/navigation proof is missing")
                checked = set()
                for row in rows:
                    require(isinstance(row, dict) and isinstance(row.get("file"), str),
                            "Invalid native terrain/navigation proof")
                    name = prefix + row["file"]
                    require(name in expected and name not in checked and
                            row.get("bytes") == records[name]["bytes"] and row.get("sha256") == records[name]["sha256"],
                            "Native terrain/navigation proof belongs to different bytes")
                    checked.add(name)
                require(checked == expected, "Native terrain/navigation proof is incomplete")
            require(isinstance(proof.get("mmap_parameters"), dict) and
                    proof["mmap_parameters"].get("sha256") == records["mmaps/725.mmap"]["sha256"],
                    "Native navigation parameters proof changed")
            vmap = proof.get("vmap", {})
            require(isinstance(vmap, dict) and vmap.get("present") is True and
                    isinstance(vmap.get("tree"), dict) and isinstance(vmap.get("tiles"), list) and
                    isinstance(vmap.get("models"), list), "Native VMAP closure proof is missing")
            checked = {}
            for row in [vmap["tree"], *vmap["tiles"], *vmap["models"]]:
                require(isinstance(row, dict) and isinstance(row.get("file"), str), "Invalid native VMAP proof")
                name = "vmaps/" + row["file"]
                require(name not in checked and name in records and
                        row.get("bytes") == records[name]["bytes"] and row.get("sha256") == records[name]["sha256"],
                        "Native VMAP model/tile/tree closure changed")
                checked[name] = row
            require(set(checked) == {name for name in records if name.startswith("vmaps/")},
                    "Native VMAP proof does not cover the complete model closure")
            entry = _pinned(directory / "player-entry.json", index.get("entry_proof_sha256"))
            require(entry.get("source_revision") == revision and entry.get("status") == "pass" and
                    entry.get("ok") is True and entry.get("bounds") == settlement.BOUNDS and
                    entry.get("verification_sha256") == index["verification_sha256"],
                    "Safe entry belongs to another native geometry proof")
            point, evidence = entry.get("player_entry", {}), entry.get("evidence", {})
            require(point.get("map") == 725 and all(type(point.get(k)) in (float, int) and
                    math.isfinite(point[k]) for k in ("x", "y", "z", "ground_z")) and
                    settlement.BOUNDS["min_x"] < point["x"] < settlement.BOUNDS["max_x"] and
                    settlement.BOUNDS["min_y"] < point["y"] < settlement.BOUNDS["max_y"] and
                    abs(point["z"] - point["ground_z"] - 2) < .00001 and evidence.get("dry") is True and
                    evidence.get("outside_native_and_source_terrain_holes") is True and
                    evidence.get("nav_ground_projection", {}).get("flags") == 1 and
                    type(evidence.get("nav_ground_projection", {}).get("ground_delta")) in (float, int) and
                    abs(evidence["nav_ground_projection"]["ground_delta"]) <= 1 and
                    type(evidence.get("slope_degrees")) in (float, int) and
                    0 <= evidence["slope_degrees"] <= 15 and
                    type(evidence.get("outside_collision_bounds_margin")) in (float, int) and
                    math.isfinite(evidence["outside_collision_bounds_margin"]) and
                    evidence["outside_collision_bounds_margin"] >= 2,
                    "Authored entry lacks a dry native ground/collision/navigation proof")
            return cls(tuple(files), directory, entry, index["verification_sha256"], index["entry_proof_sha256"],
                       source_revision=revision)
        except (OSError, KeyError, TypeError, ValueError) as exc:
            if isinstance(exc, settlement.ProfileError):
                raise
            raise settlement.ProfileError("Missing or malformed versioned authored-map metadata") from exc

    @classmethod
    def load_revision(cls, metadata: Path, revision: str) -> GeometryBundle:
        return cls.load(metadata, revision=revision)


def verify_data(data: Path, metadata: Path = METADATA, *, revision: str | None = None) -> GeometryBundle:
    bundle = GeometryBundle.load(metadata) if revision is None else GeometryBundle.load_revision(metadata, revision)
    root = data.resolve()
    for row in bundle.geometry_files:
        path = data / row["path"]
        require(path.resolve().is_relative_to(root) and path.is_file() and path.stat().st_size == row["bytes"] and
                _digest(path) == row["sha256"], f"Authored map live geometry hash mismatch: {row['path']}")
    for directory, patterns in (("maps", ("725*.map",)), ("mmaps", ("725.mmap", "725*.mmtile")),
                                ("vmaps", ("725.vmtree", "725*.vmtile"))):
        actual = {f"{directory}/{p.name}" for pattern in patterns for p in (data / directory).glob(pattern)}
        expected = {row["path"] for row in bundle.geometry_files if row["path"].startswith(directory + "/") and
                    not row["path"].endswith(".vmo")}
        require(actual == expected, "Unexpected or missing native authored-map footprint")
    return bundle
