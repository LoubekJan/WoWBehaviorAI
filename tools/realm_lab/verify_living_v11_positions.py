"""Read-only geometry diagnostics for v11 physical village food routes.

Requires optional NumPy/SciPy, the unchanged authored map and matching native
data. This does not change the map, SQL, native spawns or the running realm.
``--prepare`` emits exact inputs for verify_resident_navmesh.cpp; a subsequent
run with ``--native-result`` independently validates its complete output.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile

import numpy as np
from scipy import ndimage

try:
    from . import map_bundle, residents_v11, terrain_geometry as tg
    from . import verify_resident_positions as resident
except ImportError:
    import map_bundle
    import residents_v11
    import terrain_geometry as tg
    import verify_resident_positions as resident

CROP = "tileset/elwynn/elwynncrop92.blp"
STEP = tg.GRID_SIZE / 1024
GROUPS = ((1, "villageRight", 180, (902000, 902001, 902002, 902003)),
          (2, "villageTop", 186, (902004, 902005, 902006, 902007)),
          (3, "villageLeft", 212, (902008, 902009, 902010, 902011)))
DEPOT_CANDIDATES = ((1, "villageRight", 180, 196.33984375, 306.4765625),
                    (2, "villageTop", 186, 350.23828125, 593.912109375),
                    (3, "villageLeft", 212, 82.783203125, 840.591796875),
                    (4, "villageBottom", None, -405., 481.),
                    (5, "Stormwind city", None, 65., 590.),
                    (6, "Defias camp", None, 118.5, 1018.5))


def check(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def position(point):
    return {key: point[key] for key in ("x", "y", "z")}


class Scene:
    def __init__(self, source, data, metadata):
        self.geometry = map_bundle.verify_data(data, metadata.parent, revision="preview-v11")
        self.population = residents_v11.Population.load(metadata, self.geometry)
        self.terrains, self.tops, self.models = {}, {}, {}
        manifest = json.loads((metadata / "project-manifest.json").read_text(encoding="utf-8"))
        for row in manifest["files"]:
            path = source / row["path"]
            check(path.resolve().is_relative_to(source.resolve()) and path.stat().st_size == row["bytes"] and
                  digest(path) in (row["source_sha256"], row["packaged_sha256"]), "Source map no longer matches v11")
        for tx, ty in map_bundle.TILES:
            raw = (source / f"world/maps/aiworldlab/aiworldlab_{tx}_{ty}.adt").read_bytes()
            terrain = tg.read_terrain(raw, tx, ty)
            self.terrains[(tx, ty)] = terrain
            self.tops[(tx, ty)] = {tag: body for tag, body in tg.chunks(raw) if tag != b"MCNK"}
            for model in terrain.models:
                ident = model["unique_id"]
                check(ident not in self.models or self.models[ident] == model, "Conflicting authored model")
                self.models[ident] = model
        self.collision = resident.collision_scene(data)
        check(len(self.collision) == 329, "The reviewed native collision scene changed")
        self.crop = self.crop_mask(source)

    def terrain(self, x, y):
        key = next((key for key, terrain in self.terrains.items()
                    if terrain.origin[0]-tg.GRID_SIZE <= x <= terrain.origin[0] and
                    terrain.origin[1]-tg.GRID_SIZE <= y <= terrain.origin[1]), None)
        check(key is not None, "Point leaves the four authored map tiles")
        return self.terrains[key], self.tops[key]

    def ground(self, x, y):
        terrain, _ = self.terrain(x, y)
        center = terrain.sample(x, y)
        floor = center["ground_z"]
        values, slopes = [], []
        for dx, dy in [(0., 0.)]+[(.75*math.cos(n*math.tau/12), .75*math.sin(n*math.tau/12)) for n in range(12)]:
            terrain, top = self.terrain(x+dx, y+dy)
            sample = terrain.sample(x+dx, y+dy)
            check(sample["triangle_slope_degrees"] <= 15 and abs(sample["ground_z"]-floor) <= .35,
                  "Standing cylinder leaves level authored terrain")
            values.append(sample["ground_z"])
            slopes.append(sample["triangle_slope_degrees"])
            check(all(floor > water["surface_max"]+.1 for water in
                      resident.source_water(top, terrain, x+dx, y+dy)), "Standing cylinder intersects authored water")
        check(max(values)-min(values) <= .5 and
              resident.clear_scene(self.collision, dict(x=x, y=y, z=floor), 2.4),
              "Standing cylinder intersects full native collision scene")
        return dict(x=x, y=y, z=floor+.1, geometry=dict(floor_z=floor, slope_degrees=center["triangle_slope_degrees"],
                    max_ring_slope_degrees=max(slopes), floor_circle_height_range=max(values)-min(values),
                    clear_radius=.75, headroom=2.4, full_native_scene_cylinder_clear=True,
                    source_all_water_layers_checked=True, dry=True, indoor=False))

    def crop_mask(self, source):
        crop = np.zeros((2048, 2048), dtype=np.uint8)
        for tx, ty in map_bundle.TILES:
            raw = (source / f"world/maps/aiworldlab/aiworldlab_{tx}_{ty}.adt").read_bytes()
            chunks = list(tg.chunks(raw))
            names = next(body for tag, body in chunks if tag == b"MTEX").decode("ascii").strip("\0").split("\0")
            for tag, body in chunks:
                if tag != b"MCNK":
                    continue
                ix, iy, count = struct.unpack_from("<3I", body, 4)
                layers = list(struct.iter_unpack("<4I", tg.referenced_chunk(body, struct.unpack_from("<I", body, 28)[0], b"MCLY")))
                check(len(layers) == count, "Invalid texture layer count")
                selected = [n for n, row in enumerate(layers) if names[row[0]].replace("\\", "/").lower() == CROP]
                if not selected:
                    continue
                check(len(selected) == 1 and selected[0] > 0 and len(layers) <= 4, "Unsupported crop texture layout")
                alpha = tg.referenced_chunk(body, struct.unpack_from("<I", body, 36)[0], b"MCAL")
                weights = []
                for n, (_, flags, offset, _) in enumerate(layers):
                    if not n:
                        continue
                    check(flags == 0x100 and offset == (n-1)*4096 and offset+4096 <= len(alpha),
                          "Crop diagnostics require uncompressed Wrath big-alpha layers")
                    weights.append((n, np.frombuffer(alpha[offset:offset+4096], dtype=np.uint8).reshape(64, 64)))
                value = next(weight for n, weight in weights if n == selected[0]).copy()
                # Keep only dominant crop pixels, irrespective of other layer ordering.
                for n, other in weights:
                    if n != selected[0]:
                        value[other > 31] = 0
                crop[(ty-31)*1024+iy*64:(ty-31)*1024+(iy+1)*64,
                     (tx-30)*1024+ix*64:(tx-30)*1024+(ix+1)*64] = value
        return crop


def prepare(source, data, metadata):
    scene = Scene(source, data, metadata)
    source_pins = json.loads((metadata / "project-manifest.json").read_text(encoding="utf-8"))["files"]
    depots = []
    for ident, name, uid, x, y in DEPOT_CANDIDATES:
        if uid:
            model = scene.models[uid]
            check(model["kind"] == "m2" and model["name"].lower().endswith("westfallgrainsilo01.m2"),
                  "Village granary identity changed")
        point = None
        # The granary candidates are already outside their actual collision envelope;
        # outdoor depots may move a few yards to the first independently safe spot.
        offsets = [(0., 0.)] if uid else [(0., 0.)]+[(r*math.cos(a*math.tau/16), r*math.sin(a*math.tau/16))
                                                          for r in (2., 4., 6., 8., 10.) for a in range(16)]
        for dx, dy in offsets:
            try:
                point = scene.ground(x+dx, y+dy)
                break
            except ValueError:
                continue
        check(point is not None, f"No safe outdoor storage point for {name}")
        depots.append(dict(id=ident, name=name, source_unique_id=uid,
                           location_kind="granary_approach" if uid else "outdoor_stock",
                           world_faction_id=2 if ident == 6 else 1, position=dict(map_id=725, **position(point)),
                           geometry=point["geometry"]))
    labels, count = ndimage.label(scene.crop >= 128)
    components = []
    for label in range(1, count+1):
        rows, cols = np.where(labels == label)
        if len(rows) < 100:
            continue
        components.append((label, rows, cols))
    check(len(components) == 3, "Current map must contain exactly three actual painted crop fields")
    fields, assignments, used = [], [], set()
    for ident, name, uid, workers in GROUPS:
        silo = scene.models[uid]["server_position"]
        label, rows, cols = min(components, key=lambda item: math.hypot(
            tg.GRID_SIZE-(item[1].mean()+.5)*STEP-silo["x"],
            2*tg.GRID_SIZE-(item[2].mean()+.5)*STEP-silo["y"]))
        check(label not in used, "Two village groups share an unintended crop field")
        used.add(label)
        r0, r1, c0, c1 = rows.min(), rows.max()+1, cols.min(), cols.max()+1
        mask = labels[r0:r1, c0:c1] == label
        clearance = ndimage.distance_transform_edt(mask)*STEP
        candidates = []
        for rr, cc in zip(*np.where(mask & (clearance >= 4) & (scene.crop[r0:r1, c0:c1] >= 224))):
            candidates.append((float(clearance[rr, cc]), int(rr+r0), int(cc+c0)))
        points = []
        for distance, row, col in sorted(candidates, reverse=True):
            x, y = tg.GRID_SIZE-(row+.5)*STEP, 2*tg.GRID_SIZE-(col+.5)*STEP
            if any(math.hypot(x-p["x"], y-p["y"]) < 8 for p in points):
                continue
            try:
                point = scene.ground(x, y)
            except ValueError:
                continue
            point.update(id=f"field_{ident}_{len(points)}", crop_alpha=int(scene.crop[row, col]),
                         crop_boundary_clearance_yards=distance)
            points.append(point)
            if len(points) == 12:
                break
        check(len(points) == 12, f"Insufficient distinct clear painted work points at {name}")
        fields.append(dict(id=ident, name=name, crop_texture=CROP, crop_pixels_alpha_ge_128=len(rows),
                           crop_area_square_yards=len(rows)*STEP*STEP, work_points=points))
        for ordinal, spawn in enumerate(workers):
            assignments.append(dict(spawn_id=spawn, field_id=ident, depot_id=ident,
                                    work_point_indices=[ordinal+4*cycle for cycle in range(3)]))
    population = {actor.spawn_id: actor for actor in scene.population.actors}
    points = []
    for actor in scene.population.actors:
        points.append(dict(kind="home", id=f"home_{actor.spawn_id}", role="resident", spawn_id=actor.spawn_id,
                           x=actor.x, y=actor.y, z=actor.z))
    for depot in depots:
        points.append(dict(kind="depot", id=f"depot_{depot['id']}", role="storage", **position(depot["position"])))
    for field in fields:
        points.extend(dict(kind="field", role="farmer", **{k: p[k] for k in ("id", "x", "y", "z")})
                      for p in field["work_points"])
    routes = set()
    def both(a, b):
        routes.update(((a, b), (b, a)))
    for worker in assignments:
        home, depot = f"home_{worker['spawn_id']}", f"depot_{worker['depot_id']}"
        both(home, depot)
        for index in worker["work_point_indices"]:
            point = f"field_{worker['field_id']}_{index}"
            both(home, point)
            both(point, depot)
    for spawn in range(902012, 902045):
        home = f"home_{spawn}"
        if spawn in (902016, 902017):
            for ident in range(1, 6):
                both(home, f"depot_{ident}")
        elif spawn >= 902035:
            for ident in (1, 2, 3, 4, 6):
                both(home, f"depot_{ident}")
        else:
            both(home, "depot_4" if spawn <= 902015 else "depot_5")
    for ident in range(1, 5):
        both(f"depot_{ident}", "depot_5")
        both(f"depot_{ident}", "depot_6")
    check(len(points) == 87 and len(population) == 45, "Unexpected living-world geometry cohort")
    return dict(schema_version=1, profile="living-v11", source_revision="preview-v11", realm_id=2,
                map_id=725, area_id=4988, geometry_sha256=residents_v11.geometry_digest(scene.geometry.geometry_files),
                geometry_files=list(scene.geometry.geometry_files), source_files=source_pins,
                resident_position_proof_sha256=digest(metadata/residents_v11.POSITION_PROOF),
                homes_unchanged=True, native_collision_instances=len(scene.collision),
                source_all_water_layers_checked=True, crop_method="Direct uncompressed 64x64 MCAL; crop alpha>=224, other overlays<=31; four-yard mask clearance",
                depots=depots, fields=fields, worker_assignments=assignments, points=points,
                routes=[dict(from_id=a, to_id=b) for a, b in sorted(routes)],
                physical_walk_verified=False, source_map_modified=False)


def native_inputs(document):
    points = "kind\tid\trole\tx\ty\tz\n"+"".join(
        f"{p['kind']}\t{p['id']}\t{p['role']}\t{p['x']:.12f}\t{p['y']:.12f}\t{p['z']:.12f}\n"
        for p in document["points"])
    routes = "from\tto\n"+"".join(f"{p['from_id']}\t{p['to_id']}\n" for p in document["routes"])
    return points, routes


def prepare_route_hints(document, native, source, data, metadata):
    """Short runtime query goals near native corridor bends, never straight shortcuts.

    A subsequent native probe must recheck every changed/split leg. Bends near
    collision walls or steep banks are shifted only to nearby dry, clear ground.
    The runtime still computes a native path for each leg before moving.
    """
    scene = Scene(source, data, metadata)
    paths = {(p["from"], p["to"]): p for p in native["paths"]}
    depots = {d["id"]: d for d in document["depots"]}
    points, routes, chains = {}, set(), []
    for ident in range(1, 5):
        for target in (5, 6):
            path = paths[(f"depot_{ident}", f"depot_{target}")]["straight_path"]
            # Insert along the native straight path; only the next individual
            # goal is passed to the bounded runtime planner.
            candidates = []
            for a, b in zip(path, path[1:]):
                length = math.hypot(a["x"]-b["x"], a["y"]-b["y"])
                parts = max(1, math.ceil(length/140))
                for n in range(parts):
                    t = n/parts
                    candidates.append(dict(x=a["x"]+(b["x"]-a["x"])*t,
                                           y=a["y"]+(b["y"]-a["y"])*t))
            candidates.append(path[-1])
            chain = []
            for index, q in enumerate(candidates):
                if index in (0, len(candidates)-1):
                    depot_id = ident if index == 0 else target
                    point = dict(id=f"depot_{depot_id}", **position(depots[depot_id]["position"]))
                else:
                    safe = None
                    before, after = candidates[index-1], candidates[index+1]
                    vx, vy = after["x"]-before["x"], after["y"]-before["y"]
                    length2 = vx*vx+vy*vy
                    along = max(0., min(1., ((q["x"]-before["x"])*vx+(q["y"]-before["y"])*vy)/length2)) if length2 else 0.
                    # A convex corridor corner bends away from the obstacle;
                    # preserve that side before trying other nearby directions.
                    preferred = math.atan2(q["y"]-before["y"]-along*vy, q["x"]-before["x"]-along*vx)
                    for radius in (0., 4., 6., 8., 10., 12., 15., 20.):
                        for angle in range(16 if radius else 1):
                            x = q["x"]+radius*math.cos(preferred+angle*math.tau/16)
                            y = q["y"]+radius*math.sin(preferred+angle*math.tau/16)
                            try:
                                safe = scene.ground(x, y)
                                break
                            except ValueError:
                                continue
                        if safe is not None:
                            break
                    check(safe is not None, "No nearby dry body-clear runtime route waypoint")
                    point = dict(id=f"route_{ident}_{target}_{index}", **position(safe),
                                 geometry=safe["geometry"], native_bend_offset_yards=radius)
                if chain and math.hypot(point["x"]-chain[-1]["x"], point["y"]-chain[-1]["y"]) < 2:
                    if index != len(candidates)-1:
                        continue
                    chain.pop()
                chain.append(point)
            check(len(chain) >= 2 and all(math.hypot(a["x"]-b["x"], a["y"]-b["y"]) <= 180
                                         for a, b in zip(chain, chain[1:])), "Runtime route goal exceeds 180 yards")
            for point in chain:
                points[point["id"]] = dict(kind="waypoint", role="runtime_route", **position(point), id=point["id"])
            for a, b in zip(chain, chain[1:]):
                routes.update(((a["id"], b["id"]), (b["id"], a["id"])))
            chains.append(dict(from_depot_id=ident, to_depot_id=target, points=chain))
            chains.append(dict(from_depot_id=target, to_depot_id=ident, points=list(reversed(chain))))
    check(len(points) <= 100, "Runtime waypoint probe exceeds its bounded input count")
    hints = dict(points=list(points.values()), routes=[dict(from_id=a, to_id=b) for a, b in sorted(routes)])
    document["runtime_routes"] = chains
    document["waypoint_probe"] = hints
    return hints


def verify_native(document, native):
    points = {p["id"]: p for p in document["points"]}
    pairs = {(p["from_id"], p["to_id"]) for p in document["routes"]}
    check(native.get("ok") is True and all(native.get(k) == v for k, v in
          (("map_id", 725), ("poly_ref_bits", 64), ("tiles", 4), ("include_flags", 1), ("exclude_flags", 65534),
           ("valid_projections", len(points)), ("required_paths", len(pairs)), ("complete_paths", len(pairs)))),
          "Native ground projection or full path check failed")
    projections, paths = native.get("projections", []), native.get("paths", [])
    check(len(projections) == len(points) and {p.get("id") for p in projections} == set(points) and
          len(paths) == len(pairs) and {(p.get("from"), p.get("to")) for p in paths} == pairs,
          "Native probe did not cover the exact requested points/routes")
    for projection in projections:
        source = points[projection["id"]]
        check(all(abs(projection["input"][k]-source[k]) < 1e-8 for k in ("x", "y", "z")) and
              projection["valid"] is True and projection["flags"] == 1 and projection["xy_delta"] <= .05 and
              abs(projection["z_delta"]) <= .5 and projection["status"]["detail_bits"] == 0,
              "Native point differs from the authored dry standing point")
    for path in paths:
        check(path["complete"] is True and path["all_corridor_flags_ground_only"] is True and
              path["endpoint_delta"] <= .01 and path["status"]["detail_bits"] == path["straight_status"]["detail_bits"] == 0,
              "Native route is partial, truncated, or leaves ground polygons")
    document.update(ok=True, native_navigation=native)
    document["navigation_inputs_sha256"] = {key: hashlib.sha256(value.encode()).hexdigest()
                                             for key, value in zip(("points", "routes"), native_inputs(document))}
    document["limitations"] = ["Static geometry and native Detour checks do not prove a live NPC completed farming, eating, hauling or stealing.",
                                "Three granaries are authored; southern village, town and Defias use explicit outdoor stock points.",
                                "There are exactly three painted crop fields; southern villagers are consumers, not farmers."]
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-project", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, default=residents_v11.METADATA)
    parser.add_argument("--prepare", type=Path, help="Write candidate JSON and sibling points/routes TSV files")
    parser.add_argument("--native-result", type=Path)
    parser.add_argument("--native-probe", type=Path)
    parser.add_argument("--prepare-route-hints", type=Path, help="After full point/route check, prepare a separate waypoint leg probe")
    parser.add_argument("--waypoint-result", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        document = prepare(args.source_project, args.data, args.metadata)
        points, routes = native_inputs(document)
        if args.prepare:
            args.prepare.write_text(json.dumps(document, indent=2, allow_nan=False)+"\n", encoding="utf-8")
            args.prepare.with_suffix(".points.tsv").write_bytes(points.encode())
            args.prepare.with_suffix(".routes.tsv").write_bytes(routes.encode())
        if args.native_result:
            native = json.loads(args.native_result.read_text(encoding="utf-8"))
        elif args.native_probe:
            with tempfile.TemporaryDirectory(prefix="living-v11-nav-") as work:
                a, b = Path(work)/"points.tsv", Path(work)/"routes.tsv"
                a.write_bytes(points.encode()); b.write_bytes(routes.encode())
                run = subprocess.run([str(args.native_probe.resolve()), str(args.data.resolve()), str(a), str(b)], capture_output=True, check=True)
            native = json.loads(run.stdout)
        else:
            check(args.prepare is not None, "Provide a native probe/result or request preparation")
            print(json.dumps(dict(prepared=True, points=len(document["points"]), routes=len(document["routes"]),
                                  depots=document["depots"], fields=[dict(id=f["id"], points=len(f["work_points"])) for f in document["fields"]])))
            return 0
        document = verify_native(document, native)
        hints = prepare_route_hints(document, native, args.source_project, args.data, args.metadata)
        if args.prepare_route_hints:
            args.prepare_route_hints.write_text(json.dumps(document, indent=2, allow_nan=False)+"\n", encoding="utf-8")
            a, b = native_inputs(hints)
            args.prepare_route_hints.with_suffix(".points.tsv").write_bytes(a.encode())
            args.prepare_route_hints.with_suffix(".routes.tsv").write_bytes(b.encode())
        if args.waypoint_result:
            waypoint_native = json.loads(args.waypoint_result.read_text(encoding="utf-8"))
            verify_native(hints, waypoint_native)
            document["waypoint_navigation"] = waypoint_native
            document["runtime_route_legs_verified"] = True
        else:
            check(args.prepare_route_hints is not None, "Runtime route legs require a separate native probe/result")
            document["ok"] = False
        if args.output:
            check(document["ok"] is True, "Cannot publish a proof before every runtime route leg passed")
            args.output.write_text(json.dumps(document, indent=2, allow_nan=False)+"\n", encoding="utf-8")
        print(json.dumps(dict(ok=document["ok"], waypoint_probe_prepared=args.prepare_route_hints is not None,
                             points=len(document["points"]), routes=len(document["routes"]),
                             fields=3, farmers=12, depots=6, geometry_sha256=document["geometry_sha256"])))
        return 0
    except (ValueError, OSError, KeyError, TypeError, AssertionError, subprocess.CalledProcessError) as exc:
        print(json.dumps(dict(ok=False, error=str(exc))))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
