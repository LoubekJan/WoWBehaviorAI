"""Independently prove v12 bridge decks and bank-to-bank native ground links.

The exact Stranglechasmbridge model has its long axis on local Y. Each bridge
is checked at both banks and three deck anchors; mandatory adjacent legs force
the route over the deck instead of accepting a distant route around the river.
This is static source/collision/navigation evidence, not a physical walkthrough.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile

import numpy as np

try:
    from . import verify_living_v12_positions as living
    from . import verify_resident_positions as resident
except ImportError:
    import verify_living_v12_positions as living
    import verify_resident_positions as resident

check, digest = resident.check, resident.digest
BRIDGE_IDS = (232, 233, 234)
ASSET = "world/azeroth/stranglethorn/passivedoodads/bridge/stranglechasmbridge.m2"
# Detour's merged/detail surface is an approximation near the deck/bank
# overlap. The physical support is independently queried from exact collision
# triangles/terrain below. This explicit bridge-only bound matches the authored
# player-entry contract; resident and living-point verifiers retain .5 yard.
MAX_NAV_FLOOR_DELTA = 1.0


def local_to_world(model, x, y, z=0.):
    offset = resident.rotation(model["rotation"]) @ np.array((x, y, z))*model["scale"]
    origin = model["server_position"]
    return dict(x=float(origin["x"]-offset[0]), y=float(origin["y"]-offset[1]), z=float(origin["z"]+offset[2]))


def deck_point(scene, model, mesh, longitudinal):
    xy = local_to_world(model, 0., longitudinal)
    x, y = xy["x"], xy["y"]
    hits = [(height, slope, index) for height, slope, index in resident.vertical_hits(mesh, x, y) if slope <= 15]
    check(hits, "Bridge centerline has no level native collision surface")
    floor, slope, index = max(hits)
    values = []
    water_probes = []
    for dx, dy in [(0., 0.)]+[(.75*math.cos(n*math.tau/12), .75*math.sin(n*math.tau/12)) for n in range(12)]:
        terrain, top = scene.terrain(x+dx, y+dy)
        check(terrain.height(x+dx, y+dy) <= floor+.12, "Bridge deck anchor is buried in source terrain")
        near = [h for h, s, _ in resident.vertical_hits(mesh, x+dx, y+dy) if s <= 15 and abs(h-floor) < .15]
        check(near, "Standing cylinder leaves bridge collision deck")
        values.append(min(near, key=lambda h: abs(h-floor)))
        layers = resident.source_water(top, terrain, x+dx, y+dy)
        check(all(floor > water["surface_max"]+.1 for water in layers), "Bridge deck intersects authored water")
        water_probes.append(dict(dx=dx, dy=dy, visible_layers=layers))
    check(max(values)-min(values) <= .15 and resident.clear_scene(scene.collision, dict(x=x, y=y, z=floor), 2.4),
          "Bridge deck does not have full native body/head clearance")
    return dict(x=x, y=y, z=floor, geometry=dict(floor_z=floor, slope_degrees=slope,
        group_id=int(mesh["groups"][index]), clear_radius=.75, headroom=2.4,
        floor_circle_height_range=max(values)-min(values), full_native_scene_cylinder_clear=True,
        source_all_water_layers_checked=True, dry=True, water_probes=water_probes))


def verify_native(document, native):
    points = {p["id"]: p for p in document["points"]}
    pairs = {(p["from_id"], p["to_id"]) for p in document["routes"]}
    check(native.get("ok") is True and all(native.get(key) == value for key, value in
          (("map_id", 725), ("poly_ref_bits", 64), ("tiles", 4), ("include_flags", 1), ("exclude_flags", 65534),
           ("valid_projections", len(points)), ("required_paths", len(pairs)), ("complete_paths", len(pairs)))),
          "Native bridge ground projection or connectivity failed")
    check(len(native["projections"]) == len(points) and {p["id"] for p in native["projections"]} == set(points) and
          len(native["paths"]) == len(pairs) and {(p["from"], p["to"]) for p in native["paths"]} == pairs,
          "Native bridge probe did not cover its exact anchors and directed routes")
    for projection in native["projections"]:
        source = points[projection["id"]]
        check(all(abs(projection["input"][key]-source[key]) < 1e-8 for key in ("x", "y", "z")) and
              projection["valid"] is True and projection["flags"] == 1 and projection["xy_delta"] <= .05 and
              abs(projection["z_delta"]) <= MAX_NAV_FLOOR_DELTA and projection["status"]["detail_bits"] == 0,
              "Bridge projection left the exact XY ground surface or exceeded its explicit raster-height bound")
    check(all(p["complete"] is True and p["all_corridor_flags_ground_only"] is True and
              p["endpoint_delta"] <= .01 and p["status"]["detail_bits"] == p["straight_status"]["detail_bits"] == 0
              for p in native["paths"]), "Bridge route is partial, truncated or not ground-only")


def supported_surface(scene, mesh, x, y):
    terrain, top = scene.terrain(x, y)
    sample = terrain.sample(x, y)
    hits = [(h, slope) for h, slope, _ in resident.vertical_hits(mesh, x, y) if slope <= 35]
    deck = max(hits, default=None)
    floor = max(sample["ground_z"], deck[0] if deck else -100000.)
    slope = sample["triangle_slope_degrees"] if not deck or sample["ground_z"] > deck[0] else deck[1]
    check(slope <= 35 and all(floor > water["surface_max"]+.1 for water in resident.source_water(top, terrain, x, y)),
          "Bridge approach lacks a dry physical collision/terrain support surface")
    return floor, dict(source_terrain_z=sample["ground_z"], native_model_floor_z=deck[0] if deck else None,
                       physical_floor_z=floor, physical_slope_degrees=slope, dry=True)


def dense_recheck(scene, model, mesh, data, probe):
    ident = model["unique_id"]
    points, routes = [], []
    for longitudinal in range(-30, 31):
        xy = local_to_world(model, 0., longitudinal)
        floor, support = supported_surface(scene, mesh, xy["x"], xy["y"])
        point = dict(kind="bridge", role="dense_deck_bank_link", id=f"bridge_{ident}_dense_{longitudinal}",
                     x=xy["x"], y=xy["y"], z=floor, local_longitudinal=longitudinal, geometry=support)
        check(resident.clear_scene(scene.collision, point, 2.4), "Dense bridge line intersects native body/head collision")
        point["geometry"].update(clear_radius=.75, headroom=2.4, full_native_scene_cylinder_clear=True)
        if points:
            routes.extend((dict(from_id=points[-1]["id"], to_id=point["id"]),
                           dict(from_id=point["id"], to_id=points[-1]["id"])))
        points.append(point)
    document = dict(points=points, routes=routes)
    native = living.run_native(document, probe, data)
    verify_native(document, native)
    # Check the actual joins on each side of the model, independently of the
    # navmesh height interpolation. A small overlap or a sub-half-yard step
    # connects the physical deck to the authored bank.
    origin = model["server_position"]
    transformed = mesh["triangles"].reshape((-1, 3)).copy()
    transformed[:, 0] = origin["x"]-transformed[:, 0]
    transformed[:, 1] = origin["y"]-transformed[:, 1]
    transformed[:, 2] -= origin["z"]
    local = transformed @ resident.rotation(model["rotation"])/model["scale"]
    joins = []
    for endpoint in (float(local[:, 1].min()), float(local[:, 1].max())):
        pair = []
        for offset in (-.01, .01):
            xy = local_to_world(model, 0., endpoint+offset)
            height, support = supported_surface(scene, mesh, xy["x"], xy["y"])
            pair.append(dict(local_longitudinal=endpoint+offset, **xy, physical_floor_z=height, support=support))
        step = abs(pair[1]["physical_floor_z"]-pair[0]["physical_floor_z"])
        check(step <= .5, "Physical bridge end has a detached step above half a yard")
        joins.append(dict(endpoint_local_y=endpoint, physical_step_yards=step, samples=pair))
    return dict(sample_spacing_yards=1, points=points, routes=routes, native_navigation=native,
                physical_edge_joins=joins, max_abs_nav_floor_delta=max(abs(p["z_delta"]) for p in native["projections"]))


def verify(source, data, metadata, probe):
    scene = living.Scene(source, data, metadata)
    bridges = []
    points, routes = [], []
    for ident in BRIDGE_IDS:
        model = scene.models.get(ident)
        check(isinstance(model, dict) and model["kind"] == "m2" and
              model["name"].replace("\\", "/").lower() == ASSET and model["scale"] == 1.,
              "The three reviewed bridge identities changed")
        mesh = resident.scene_model(model, data)
        anchors = []
        for label, longitudinal in (("bank_a", -26.), ("deck_a", -10.), ("deck_center", 0.),
                                     ("deck_b", 10.), ("bank_b", 26.)):
            if label.startswith("bank"):
                xy = local_to_world(model, 0., longitudinal)
                point = scene.ground(xy["x"], xy["y"])
                point["z"] = point["geometry"]["floor_z"]
            else:
                point = deck_point(scene, model, mesh, longitudinal)
            point.update(id=f"bridge_{ident}_{label}", role=label, kind="bridge", local_longitudinal=longitudinal)
            anchors.append(point)
            points.append(point)
        for a, b in zip(anchors, anchors[1:]):
            routes.extend((dict(from_id=a["id"], to_id=b["id"]), dict(from_id=b["id"], to_id=a["id"])))
        routes.extend((dict(from_id=anchors[0]["id"], to_id=anchors[-1]["id"]),
                       dict(from_id=anchors[-1]["id"], to_id=anchors[0]["id"])))
        bridges.append(dict(source_unique_id=ident, asset=model["name"], placement=model,
                            mesh_file=mesh["mesh_file"], mesh_sha256=mesh["mesh_sha256"], anchors=anchors))
    document = dict(points=points, routes=routes)
    point_input, route_input = living.original.native_inputs(document)
    with tempfile.TemporaryDirectory(prefix="v12-bridge-probe-") as directory:
        a, b = Path(directory)/"points.tsv", Path(directory)/"routes.tsv"
        a.write_bytes(point_input.encode()); b.write_bytes(route_input.encode())
        result = subprocess.run([str(probe.resolve()), str(data.resolve()), str(a), str(b)], capture_output=True, check=True)
    native = json.loads(result.stdout)
    verify_native(document, native)
    check((native["valid_projections"], native["required_paths"], native["complete_paths"]) == (15, 30, 30),
          "Bridge proof did not cover every bank and deck leg")
    dense = []
    for bridge in bridges:
        model = bridge["placement"]
        origin = model["server_position"]
        rotation = resident.rotation(model["rotation"])
        ident = bridge["source_unique_id"]
        paths = [p for p in native["paths"] if p["from"].startswith(f"bridge_{ident}_")]
        for path in paths:
            for point in path["straight_path"]:
                # A remote ford cannot satisfy this corridor even if a native
                # path happens to exist between the two bank endpoints.
                internal = np.array((origin["x"]-point["x"], origin["y"]-point["y"], point["z"]-origin["z"]))
                local = rotation.T @ internal / model["scale"]
                check(abs(local[0]) <= 10.5 and abs(local[1]) <= 28.,
                      f"Bridge {ident} route detours outside the physical bridge corridor")
        bridge.update(native_routes=paths, forced_adjacent_deck_legs_verified=True,
                      direct_bank_routes_verified=True, remote_river_detour_rejected=True)
        mesh = resident.scene_model(model, data)
        rechecked = dense_recheck(scene, model, mesh, data, probe)
        for path in rechecked["native_navigation"]["paths"]:
            for point in path["straight_path"]:
                internal = np.array((origin["x"]-point["x"], origin["y"]-point["y"], point["z"]-origin["z"]))
                local = rotation.T @ internal / model["scale"]
                check(abs(local[0]) <= 10.5 and abs(local[1]) <= 32., "Dense bridge leg detours outside its corridor")
        bridge["dense_physical_and_native_recheck"] = rechecked
        dense.append(rechecked)
    return dict(schema_version=1, profile="living-v12", source_revision="preview-v12", realm_id=2,
        map_id=725, area_id=4988, ok=True, geometry_files=list(scene.geometry.geometry_files),
        geometry_sha256=living.residents_v12.geometry_digest(scene.geometry.geometry_files),
        source_files=json.loads((metadata/"project-manifest.json").read_text(encoding="utf-8"))["files"],
        native_collision_instances=len(scene.collision), bridges=bridges, native_navigation=native,
        dense_sample_points=sum(len(d["points"]) for d in dense),
        dense_complete_paths=sum(d["native_navigation"]["complete_paths"] for d in dense),
        nav_height_approximation=dict(allowed_abs_delta_yards=MAX_NAV_FLOOR_DELTA,
            max_primary_anchor_abs_delta_yards=max(abs(p["z_delta"]) for p in native["projections"]),
            max_dense_abs_delta_yards=max(d["max_abs_nav_floor_delta"] for d in dense),
            off_limit_points=[],
            reason="Detour merged/detail raster height near bank/deck overlap differs from exact physical support. Collision/terrain floor, dry body-clear deck, real joins and ground-only connectivity are independently proved; runtime validates the actual floor."),
        native_inputs_sha256=dict(points=hashlib.sha256(point_input.encode()).hexdigest(),
                                  routes=hashlib.sha256(route_input.encode()).hexdigest()),
        source_map_modified=False, physical_walk_verified=False,
        scope="All three stock-model bridges: exact source/native collision, supported dry deck and both banks, body clearance, physical edge joins, 183 one-yard floor samples and 360 adjacent native ground legs plus30 principal links constrained to the bridge corridor.")


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
        print(json.dumps(dict(ok=True, bridges=3, projections=15, native_paths=30,
                              dense_points=document["dense_sample_points"], dense_paths=document["dense_complete_paths"],
                              max_nav_floor_delta=document["nav_height_approximation"]["max_dense_abs_delta_yards"],
                              remote_detours_rejected=True, proof_sha256=digest(args.output))))
        return 0
    except (ValueError, OSError, KeyError, TypeError, AttributeError, AssertionError, subprocess.CalledProcessError) as exc:
        print(json.dumps(dict(ok=False, error=str(exc))))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
