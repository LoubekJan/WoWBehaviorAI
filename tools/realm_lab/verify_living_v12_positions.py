"""Reprove the immutable v11 village catalog on the v12 bridge geometry.

Unlike location preparation, this diagnostic never selects replacement points
or rebuilds runtime waypoint chains from new shortest paths. All 45 homes,
36 work points, six depots and existing runtime anchors remain unchanged.
Requires optional NumPy/SciPy and a matching verify_resident_navmesh executable.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from scipy import ndimage

try:
    from . import living_v11, map_bundle, residents_v12, terrain_geometry as tg
    from . import verify_living_v11_positions as original
    from . import verify_resident_positions as resident
except ImportError:
    import living_v11
    import map_bundle
    import residents_v12
    import terrain_geometry as tg
    import verify_living_v11_positions as original
    import verify_resident_positions as resident

check, digest, position = original.check, original.digest, original.position


class Scene(original.Scene):
    """Reuse the v11 terrain/crop/body algorithms with explicit v12 inputs."""
    def __init__(self, source, data, metadata):
        self.geometry = map_bundle.verify_data(data, metadata.parent, revision="preview-v12")
        self.population = residents_v12.Population.load(metadata, self.geometry)
        self.terrains, self.tops, self.models = {}, {}, {}
        manifest = json.loads((metadata/"project-manifest.json").read_text(encoding="utf-8"))
        for row in manifest["files"]:
            path = source/row["path"]
            check(path.resolve().is_relative_to(source.resolve()) and path.stat().st_size == row["bytes"] and
                  digest(path) in (row["source_sha256"], row["packaged_sha256"]), "Source differs from frozen v12")
        for tx, ty in map_bundle.TILES:
            raw = (source/f"world/maps/aiworldlab/aiworldlab_{tx}_{ty}.adt").read_bytes()
            terrain = tg.read_terrain(raw, tx, ty)
            self.terrains[(tx, ty)] = terrain
            self.tops[(tx, ty)] = {tag: body for tag, body in tg.chunks(raw) if tag != b"MCNK"}
            for model in terrain.models:
                ident = model["unique_id"]
                check(ident not in self.models or self.models[ident] == model, "Conflicting authored model")
                self.models[ident] = model
        self.collision = resident.collision_scene(data)
        check(len(self.collision) == 332, "V12 must contain the original scene and three new bridge instances")
        self.crop = self.crop_mask(source)


def run_native(document, probe, data):
    points, routes = original.native_inputs(document)
    with tempfile.TemporaryDirectory(prefix="living-v12-recheck-") as directory:
        a, b = Path(directory)/"points.tsv", Path(directory)/"routes.tsv"
        a.write_bytes(points.encode()); b.write_bytes(routes.encode())
        result = subprocess.run([str(probe.resolve()), str(data.resolve()), str(a), str(b)],
                                capture_output=True, check=True)
    return json.loads(result.stdout)


def verify(source, data, metadata, probe):
    baseline_metadata = metadata.parent/"preview-v11"
    baseline_population = living_v11.Population.load(baseline_metadata)
    baseline_path = baseline_metadata/living_v11.POSITION_PROOF
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    baseline_manifest = json.loads((baseline_metadata/living_v11.MANIFEST).read_text(encoding="utf-8"))
    check(digest(baseline_path) == baseline_manifest["position_proof_sha256"], "Immutable v11 position proof changed")
    scene = Scene(source, data, metadata)
    document = copy.deepcopy(baseline)
    for key in ("native_navigation", "waypoint_navigation", "ok", "navigation_inputs_sha256"):
        document.pop(key, None)
    document.update(profile="living-v12", source_revision="preview-v12",
                    geometry_files=list(scene.geometry.geometry_files),
                    geometry_sha256=residents_v12.geometry_digest(scene.geometry.geometry_files),
                    source_files=json.loads((metadata/"project-manifest.json").read_text(encoding="utf-8"))["files"],
                    resident_position_proof_sha256=digest(metadata/residents_v12.POSITION_PROOF),
                    native_collision_instances=len(scene.collision),
                    baseline_position_proof_sha256=digest(baseline_path),
                    compiled_v11_position_proof_sha256=baseline_manifest["position_proof_sha256"],
                    locations_header_sha256=baseline_manifest["locations_header_sha256"],
                    runtime_catalog_sha256=baseline_manifest["runtime_catalog_sha256"],
                    runtime_catalog_unchanged=True, source_map_modified=False,
                    homes_unchanged=True, source_all_water_layers_checked=True)
    baseline_native = baseline_population.native_population
    # Population API retains all old actor homes through native_population.
    old_homes = {a.spawn_id: a for a in baseline_native.actors}
    new_homes = {a.spawn_id: a for a in scene.population.actors}
    check(set(new_homes) == set(old_homes) == set(range(902000, 902045)), "Living NPC identities changed")
    check(all(all(getattr(old_homes[i], k) == getattr(new_homes[i], k) for k in ("x", "y", "z", "orientation"))
              for i in old_homes), "A living actor home changed")
    cache = {}
    def supported(point):
        key = tuple(point[k] for k in ("x", "y", "z"))
        if key not in cache:
            actual = scene.ground(point["x"], point["y"])
            check(all(actual[k] == point[k] for k in ("x", "y", "z")),
                  "A frozen living interaction point changed ground height or lost support")
            cache[key] = actual["geometry"]
        return copy.deepcopy(cache[key])
    for depot in document["depots"]:
        if depot["source_unique_id"]:
            model = scene.models[depot["source_unique_id"]]
            check(model["kind"] == "m2" and model["name"].lower().endswith("westfallgrainsilo01.m2"),
                  "Living granary model changed")
        depot["geometry"] = supported(depot["position"])
    labels, _ = ndimage.label(scene.crop >= 128)
    clearances = {}
    for field in document["fields"]:
        for point in field["work_points"]:
            point["geometry"] = supported(point)
            row = int((tg.GRID_SIZE-point["x"])/original.STEP)
            col = int((2*tg.GRID_SIZE-point["y"])/original.STEP)
            check(0 <= row < 2048 and 0 <= col < 2048 and
                  int(scene.crop[row, col]) == point["crop_alpha"] and point["crop_alpha"] >= 224,
                  "A frozen farmer work point is no longer on the same painted crop")
            ident = int(labels[row, col])
            check(ident > 0, "Farmer crop component disappeared")
            if ident not in clearances:
                rr, cc = np.where(labels == ident)
                r0, r1, c0, c1 = rr.min(), rr.max()+1, cc.min(), cc.max()+1
                mask = labels[r0:r1, c0:c1] == ident
                clearances[ident] = (r0, c0, ndimage.distance_transform_edt(mask)*original.STEP)
            r0, c0, values = clearances[ident]
            clearance = float(values[row-r0, col-c0])
            check(clearance >= 4 and abs(clearance-point["crop_boundary_clearance_yards"]) < 1e-8,
                  "A frozen crop boundary clearance changed")
    for route in document["runtime_routes"]:
        for point in route["points"]:
            geo = supported(point)
            if "geometry" in point:
                point["geometry"] = geo
    for point in document["points"]:
        if point["kind"] == "home":
            actor = new_homes[point["spawn_id"]]
            check(all(point[k] == getattr(actor, k) for k in ("x", "y", "z")), "Living home probe changed")
        else:
            supported(point)
    hints = {key: copy.deepcopy(baseline["waypoint_probe"][key]) for key in ("points", "routes")}
    for point in hints["points"]:
        supported(point)
    check(living_v11.runtime_catalog(document) == living_v11.runtime_catalog(baseline),
          "Revalidation changed compiled fields, depots, assignments or runtime route coordinates")
    check(living_v11.settlement.digest(living_v11.runtime_catalog(document)) == baseline_manifest["runtime_catalog_sha256"],
          "Frozen runtime catalog digest changed")
    check(digest(living_v11.LOCATIONS_HEADER) == baseline_manifest["locations_header_sha256"],
          "The original compiled location header changed")
    native = run_native(document, probe, data)
    document = original.verify_native(document, native)
    waypoint = run_native(hints, probe, data)
    original.verify_native(hints, waypoint)
    document.update(waypoint_probe=hints, waypoint_navigation=waypoint, runtime_route_legs_verified=True,
                    revalidated_outdoor_interaction_positions=len(cache))
    check((native["valid_projections"], native["complete_paths"], waypoint["valid_projections"], waypoint["complete_paths"])
          == (87, 346, 70, 144), "Frozen living graph coverage changed")
    check(living_v11.runtime_catalog(document) == living_v11.runtime_catalog(baseline), "Final runtime catalog changed")
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-project", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--native-probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        document = verify(args.source_project, args.data, args.metadata, args.native_probe)
        args.output.write_bytes((json.dumps(document, indent=2, allow_nan=False)+"\n").encode())
        print(json.dumps(dict(ok=True, profile="living-v12", source_revision="preview-v12",
                              points=87, routes=346, runtime_leg_points=70, runtime_legs=144,
                              runtime_catalog_unchanged=True, proof_sha256=digest(args.output))))
        return 0
    except (ValueError, OSError, KeyError, TypeError, AttributeError, AssertionError, subprocess.CalledProcessError) as exc:
        print(json.dumps(dict(ok=False, error=str(exc))))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
