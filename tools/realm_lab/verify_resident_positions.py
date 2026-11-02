"""Independently recheck resident floors, clearance, water and native routes.

Diagnostic only: NumPy is optional and is never imported by realm startup.
Example (use a frozen source project matching project-manifest.json)::

    python tools/realm_lab/verify_resident_positions.py --source-project SOURCE \
        --data NATIVE_DATA --metadata data/realm_lab/aiworldlab/preview-v10 \
        --native-probe PATH_TO_verify_resident_navmesh

Build verify_resident_navmesh.cpp with the matching repository Detour source
files, include src/common, src/common/Utilities, src/common/Collision/Maps and
 dep/recastnavigation/Detour/Include. DT_POLYREF64 must match the native data.
This is a static geometry check, not a client walkthrough or behavior test.
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

try:
    import numpy as np
except ImportError as exc:
    raise SystemExit("Resident geometry diagnostics require the optional NumPy package") from exc

try:
    from . import map_bundle, terrain_geometry as tg
except ImportError:
    import map_bundle
    import terrain_geometry as tg

MAGIC = b"VMAP_4.8"
MID = struct.unpack("<f", struct.pack("<f", 533.33333333 * 32))[0]
HOUSE_IDS = (178, 179, 183, 184, 193, 194, 196, 197, 213, 214, 215)

def check(value, message):
    if not value:
        raise ValueError(message)

class Reader:
    def __init__(self, data, name):
        self.data, self.pos, self.name = data, 0, name
    def take(self, size):
        check(0 <= size <= len(self.data)-self.pos, f"{self.name}: truncated data")
        value = self.data[self.pos:self.pos+size]
        self.pos += size
        return value
    def unpack(self, fmt): return struct.unpack(fmt, self.take(struct.calcsize(fmt)))
    def u32(self): return self.unpack("<I")[0]
    def tag(self, tag): check(self.take(len(tag)) == tag, f"{self.name}: invalid tag {tag!r}")
    def floats(self, n):
        value = self.unpack(f"<{n}f")
        check(all(math.isfinite(x) for x in value), "Nonfinite native geometry")
        return value
    def end(self): check(self.pos == len(self.data), f"{self.name}: trailing data")

def bih(r):
    r.floats(6)
    nodes = r.u32()
    check(3 <= nodes <= 100000000 and nodes % 3 == 0, "Invalid BIH nodes")
    r.take(nodes*4)
    count = r.u32()
    check(count <= 100000000, "Invalid BIH object count")
    r.take(count*4)

def spawn(r):
    flags, adtid, ident = r.unpack("<IHI")
    position, rotation, scale = r.floats(3), r.floats(3), r.floats(1)[0]
    if flags & 4: r.floats(6)
    length = r.u32()
    check(0 < length <= 500 and scale > 0, "Invalid native spawn")
    name = r.take(length).decode("ascii")
    check(not any(c in name for c in "/\\\0") and name not in (".", ".."), "Unsafe native model name")
    return dict(id=ident, flags=flags, adt_id=adtid, name=name,
                server_position=dict(x=MID-position[0], y=MID-position[1], z=position[2]),
                rotation=list(rotation), scale=scale)

def models(path):
    r = Reader(path.read_bytes(), path.name)
    r.tag(MAGIC); r.tag(b'WMOD'); check(r.u32() == 8, 'Bad WMOD size')
    root = r.u32(); groups = []
    if r.pos < len(r.data):
        r.tag(b'GMOD'); count = r.u32()
        for _ in range(count):
            bounds = r.floats(6)
            flags, ident = r.unpack('<2I')
            r.tag(b'VERT'); size, n = r.unpack('<2I')
            assert size == 4 + n * 12
            vertices = np.asarray(r.floats(n*3)).reshape((-1,3))
            triangles = np.empty((0,3), dtype=int)
            if n:
                r.tag(b'TRIM'); size, nt = r.unpack('<2I')
                assert size == 4 + nt * 12
                triangles = np.asarray(r.unpack(f'<{nt*3}I')).reshape((-1,3))
                r.tag(b'MBIH'); bih(r)
                r.tag(b'LIQU'); size = r.u32(); r.take(size)
            groups.append({'flags': flags, 'id': ident, 'vertices': vertices, 'triangles': triangles})
        r.tag(b'GBIH'); bih(r)
    r.end()
    return groups


def rotation(rot):
    x,y,z = np.radians(rot)
    cz,sz = math.cos(y),math.sin(y)
    cy,sy = math.cos(x),math.sin(x)
    cx,sx = math.cos(z),math.sin(z)
    return np.array(((cz,-sz,0),(sz,cz,0),(0,0,1))) @ np.array(((cy,0,sy),(0,1,0),(-sy,0,cy))) @ np.array(((1,0,0),(0,cx,-sx),(0,sx,cx)))


def scene_model(m, native):
    name = m['name'].replace('\\','/').rsplit('/',1)[-1].replace(' ','_') + '.vmo'
    paths = [p for p in (native/'vmaps').glob('*') if p.name.lower()==name.lower()]
    assert len(paths) == 1
    groups = models(paths[0])
    R = rotation(m['rotation'])
    s = m.get('scale',1.0)
    pos = m['server_position']
    arrays, flags, ids = [], [], []
    for g in groups:
        vertices = (g['vertices'] @ R.T) * s
        # Native world geometry mirrors both horizontal axes on the server.
        vertices[:,0] = pos['x']-vertices[:,0]
        vertices[:,1] = pos['y']-vertices[:,1]
        vertices[:,2] += pos['z']
        triangles = vertices[g['triangles']]
        arrays.append(triangles)
        flags += [g['flags']]*len(triangles)
        ids += [g['id']]*len(triangles)
    triangles = np.concatenate(arrays)
    return {'model':m, 'triangles':triangles, 'flags':np.asarray(flags), 'groups':np.asarray(ids),
            'mesh_file': paths[0].name, 'mesh_sha256':hashlib.sha256(paths[0].read_bytes()).hexdigest()}


def vertical_hits(mesh, x, y):
    tris = mesh['triangles']
    a,b,c = tris[:,0],tris[:,1],tris[:,2]
    v0,v1 = b-a,c-a
    det = v0[:,0]*v1[:,1]-v1[:,0]*v0[:,1]
    valid = np.abs(det)>1e-9
    u = np.divide((x-a[:,0])*v1[:,1]-(y-a[:,1])*v1[:,0],det,out=np.zeros(len(det)),where=valid)
    v = np.divide(v0[:,0]*(y-a[:,1])-v0[:,1]*(x-a[:,0]),det,out=np.zeros(len(det)),where=valid)
    good = np.flatnonzero(valid & (u>=-1e-8) & (v>=-1e-8) & (u+v<=1+1e-8))
    normals = np.cross(v0[good],v1[good])
    slopes = np.degrees(np.arctan2(np.linalg.norm(normals[:,:2],axis=1), np.abs(normals[:,2])))
    zs = a[good,2]+u[good]*v0[good,2]+v[good]*v1[good,2]
    return sorted([(float(z),float(slope),int(i)) for z,slope,i in zip(zs,slopes,good)])


def ray(mesh, origin, end):
    tris = mesh['triangles']
    a,b,c = tris[:,0],tris[:,1],tris[:,2]
    e1,e2 = b-a,c-a
    direction = np.asarray(end)-origin
    length = np.linalg.norm(direction)
    direction /= length
    p=np.cross(np.broadcast_to(direction,e2.shape),e2)
    det=np.sum(e1*p,axis=1)
    valid=np.abs(det)>1e-7
    inv=np.divide(1.0,det,out=np.zeros(len(det)),where=valid)
    s=np.asarray(origin)-a
    u=inv*np.sum(s*p,axis=1)
    q=np.cross(s,e1)
    v=inv*np.sum(q*direction,axis=1)
    t=inv*np.sum(e2*q,axis=1)
    hits=np.flatnonzero(valid & (u>=-1e-7)&(v>=-1e-7)&(u+v<=1+1e-7)&(t>1e-4)&(t<length-1e-4))
    return float(np.min(t[hits])) if len(hits) else None


def collision_scene(native):
    """Full native scene, including the collision doodads embedded in WMOs."""
    instances={}
    for path in sorted((native/'vmaps').glob('725_*.vmtile')):
        r=Reader(path.read_bytes(),path.name);r.tag(MAGIC)
        for _ in range(r.u32()):
            m=spawn(r);r.u32()
            assert m['id'] not in instances or instances[m['id']]==m
            instances[m['id']]=m
        r.end()
    meshes=[]
    for m in instances.values():
        mesh=scene_model(m,native)
        tri=mesh['triangles']
        if not len(tri):continue
        mesh['min']=tri.min(axis=(0,1));mesh['max']=tri.max(axis=(0,1))
        meshes.append(mesh)
    return meshes


def clear_scene(meshes, point):
    x,y,z=point['x'],point['y'],point['z']
    lo=np.array((x-.75,y-.75,z+.12));hi=np.array((x+.75,y+.75,z+2.2))
    relevant=[m for m in meshes if np.all(m['max']>=lo) and np.all(m['min']<=hi)]
    angles=np.linspace(0,math.tau,12,endpoint=False)
    offsets=[(.0,.0)]+[(.75*math.cos(a),.75*math.sin(a)) for a in angles]
    for mesh in relevant:
        for dx,dy in offsets:
            if ray(mesh,np.array((x+dx,y+dy,z+.12)),np.array((x+dx,y+dy,z+2.2))) is not None:
                return False
        for level in (.3,1.,2.1):
            for dx,dy in offsets[1:]:
                if ray(mesh,np.array((x,y,z+level)),np.array((x+dx,y+dy,z+level))) is not None:
                    return False
    return True


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_water(top, terrain, x, y):
    """Conservative maximum of the four visible vertices, for every layer."""
    row, col, _, _ = terrain.square(x, y)
    mh = top.get(b"MH2O")
    if mh is None:
        return []
    offset, count, _ = struct.unpack_from("<3I", mh, ((row//8)*16+col//8)*12)
    check(not count or 3072 <= offset <= len(mh)-count*24, "Truncated source water instances")
    result = []
    for layer in range(count):
        typ, fmt, _, _, ox, oy, w, h, bits, vertices = struct.unpack_from("<2H2f4B2I", mh, offset+layer*24)
        rr, cc = row % 8-oy, col % 8-ox
        if not (0 <= rr < h and 0 <= cc < w):
            continue
        length = math.ceil(w*h/8)
        check(not bits or 3072 <= bits <= len(mh)-length, "Truncated source water mask")
        mask = int.from_bytes(mh[bits:bits+length], "little") if bits else (1 << (w*h))-1
        if not mask & (1 << (rr*w+cc)):
            continue
        check(fmt in (0, 1) and 3072 <= vertices <= len(mh)-(w+1)*(h+1)*4,
              "Unsupported visible source water format")
        values = struct.unpack_from(f"<{(w+1)*(h+1)}f", mh, vertices)
        heights = [values[(rr+dr)*(w+1)+cc+dc] for dr, dc in ((0, 0), (0, 1), (1, 0), (1, 1))]
        check(all(math.isfinite(v) for v in heights), "Nonfinite source water")
        result.append(dict(layer=layer, type=typ, surface_max=max(heights)))
    return result


def verify(source: Path, data: Path, metadata: Path, probe: Path):
    geometry = map_bundle.verify_data(data, metadata.parent)
    check(geometry.source_revision == "preview-v10" and geometry.metadata.resolve() == metadata.resolve(),
          "Resident verifier requires the selected preview-v10 bundle")
    proof = json.loads((metadata / "residents-position-verification.json").read_text(encoding="utf-8"))
    geometry_hash = hashlib.sha256(json.dumps(list(geometry.geometry_files), sort_keys=True,
                                             separators=(",", ":"), allow_nan=False).encode()).hexdigest()
    check((proof.get("profile"), proof.get("source_revision"), proof.get("map_id"), proof.get("ok"),
           proof.get("geometry_sha256")) == ("residents-v10", "preview-v10", 725, True, geometry_hash),
          "Resident proof belongs to different geometry")
    source_manifest = json.loads((metadata / "project-manifest.json").read_text(encoding="utf-8"))
    for record in source_manifest["files"]:
        path = source / record["path"]
        check(path.resolve().is_relative_to(source.resolve()) and path.stat().st_size == record["bytes"] and
              digest(path) in (record["packaged_sha256"], record["source_sha256"]),
              f"Frozen source mismatch: {record['path']}")
    terrains, tops, houses = {}, {}, {}
    for ix, iy in ((30, 31), (31, 31), (30, 32), (31, 32)):
        raw = (source / f"world/maps/aiworldlab/aiworldlab_{ix}_{iy}.adt").read_bytes()
        terrain = tg.read_terrain(raw, ix, iy)
        terrains[(ix, iy)] = terrain
        tops[(ix, iy)] = {tag: body for tag, body in tg.chunks(raw) if tag != b"MCNK"}
        for model in terrain.models:
            if model["kind"] == "wmo" and "farm" in model["name"].lower():
                ident = model["unique_id"]
                check(ident not in houses or houses[ident] == model, "Conflicting house placement")
                houses[ident] = model
    check(set(houses) == set(HOUSE_IDS), "Source residential house inventory changed")
    buildings = proof["buildings"]
    check(len(buildings) == 11 and {b["house_id"] for b in buildings} == set(HOUSE_IDS),
          "Proof must cover exactly eleven houses")
    scene = collision_scene(data)
    check(len(scene) == 329, "Native collision scene inventory changed")
    all_points = []
    for building in buildings:
        model = houses[building["house_id"]]
        check(building["asset"] == model["name"], "House asset changed")
        mesh = scene_model(model, data)
        check(building["mesh_file"] == mesh["mesh_file"] and
              building["mesh_sha256"] == mesh["mesh_sha256"], "House collision model changed")
        positions = building["positions"]
        check(len(positions) == 2 and {p["slot"] for p in positions} == {1, 2}, "House slots changed")
        check(math.hypot(positions[0]["x"]-positions[1]["x"],
                         positions[0]["y"]-positions[1]["y"]) >= 2.4, "Residents overlap")
        for point in positions:
            x, y, z = (point[k] for k in ("x", "y", "z"))
            check(all(type(v) in (float, int) and math.isfinite(v) for v in (x, y, z, point["orientation"])) and
                  0 <= point["orientation"] < math.tau, "Invalid resident coordinates")
            floor = point["geometry"]["floor_z"]
            hits = vertical_hits(mesh, x, y)
            floors = [(height, slope, index) for height, slope, index in hits if
                      abs(height-floor) < .00001 and slope <= 15 and mesh["flags"][index] & 0x2000]
            check(floors and 0 <= z-floor <= .15, "Resident does not stand on its indoor collision floor")
            hit = min(floors, key=lambda row: abs(row[0]-floor))
            check(abs(hit[1]-point["geometry"]["slope_degrees"]) < .00001 and
                  int(mesh["groups"][hit[2]]) == point["geometry"]["group_id"], "Indoor floor proof changed")
            roof = [height for height, _, _ in hits if height > floor+2.3]
            check(roof and roof[0]-floor <= 12 and
                  not any(floor+.12 < height < floor+2.2 for height, _, _ in hits), "Missing indoor headroom")
            offsets = [(0., 0.)] + [(.75*math.cos(n*math.tau/12), .75*math.sin(n*math.tau/12)) for n in range(12)]
            ring = []
            for dx, dy in offsets:
                px, py = x+dx, y+dy
                tile = next((key for key, terrain in terrains.items()
                             if terrain.origin[0]-tg.GRID_SIZE <= px <= terrain.origin[0] and
                             terrain.origin[1]-tg.GRID_SIZE <= py <= terrain.origin[1]), None)
                check(tile is not None, "Standing cylinder leaves authored map")
                terrain = terrains[tile]
                try:
                    height = terrain.height(px, py)
                except ValueError:
                    height = -100000.  # WMO floor can legitimately span an authored hole.
                check(height <= floor+.12, "Terrain occludes the indoor standing floor")
                layers = source_water(tops[tile], terrain, px, py)
                check(all(floor > water["surface_max"]+.1 for water in layers), "Resident floor intersects source water")
                if dx or dy:
                    near = [height for height, slope, index in vertical_hits(mesh, px, py) if
                            slope < 20 and abs(height-floor) < .35 and mesh["flags"][index] & 0x2000]
                    check(near, "Standing cylinder leaves the indoor floor")
                    ring.append(min(near, key=lambda value: abs(value-floor)))
            check(max(ring)-min(ring) <= .25 and clear_scene(scene, dict(x=x, y=y, z=floor)),
                  "Standing cylinder intersects native scene collision")
            all_points.append(point)
    entry = geometry.entry_proof["player_entry"]
    rows = ["kind\tid\trole\tx\ty\tz",
            f"test\tentry\t-\t{entry['x']}\t{entry['y']}\t{entry['ground_z']}"]
    routes = ["from\tto"]
    for point in all_points:
        ident = f"house_{next(b['house_id'] for b in buildings if point in b['positions'])}_{point['slot']}"
        check(point["id"] == ident, "Resident point identifier changed")
        rows.append(f"home\t{ident}\tresident\t{point['x']:.12f}\t{point['y']:.12f}\t{point['geometry']['floor_z']:.12f}")
        routes.extend((f"entry\t{ident}", f"{ident}\tentry"))
    with tempfile.TemporaryDirectory(prefix="resident-nav-") as directory:
        points_path, routes_path = Path(directory)/"points.tsv", Path(directory)/"routes.tsv"
        points_path.write_bytes(("\n".join(rows)+"\n").encode())
        routes_path.write_bytes(("\n".join(routes)+"\n").encode())
        result = subprocess.run([str(probe.resolve()), str(data.resolve()), str(points_path), str(routes_path)],
                                capture_output=True, check=True)
    native = json.loads(result.stdout)
    check(native["ok"] is True and native["poly_ref_bits"] == 64 and native["tiles"] == 4 and
          native["valid_projections"] == 23 and native["complete_paths"] == native["required_paths"] == 44,
          "Native resident projection or route failed")
    for projection in native["projections"]:
        check(projection["valid"] is True and projection["flags"] == 1 and projection["xy_delta"] < .05 and
              abs(projection["z_delta"]) < .5 and projection["status"]["detail_bits"] == 0,
              "Native projection is not a nearby ground polygon")
    check(all(path["complete"] is True and path["all_corridor_flags_ground_only"] is True and
              path["status"]["detail_bits"] == path["straight_status"]["detail_bits"] == 0
              for path in native["paths"]), "Native path is partial or leaves ground polygons")
    return dict(ok=True, map_id=725, profile="residents-v10", source_revision="preview-v10",
                geometry_sha256=geometry_hash, native_files=len(geometry.geometry_files), source_files=9,
                houses=11, residents=22, native_collision_instances=len(scene), indoor_floor_clearance=True,
                all_source_water_layers_checked=True, native_navigation=native)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-project", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--native-probe", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    try:
        result = verify(arguments.source_project, arguments.data, arguments.metadata, arguments.native_probe)
        if arguments.output:
            arguments.output.write_bytes((json.dumps(result, indent=2, allow_nan=False)+"\n").encode())
        print(json.dumps({key: value for key, value in result.items() if key != "native_navigation"}))
        return 0
    except (ValueError, OSError, KeyError, TypeError, AssertionError, subprocess.CalledProcessError) as exc:
        print(json.dumps(dict(ok=False, error=str(exc))))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
