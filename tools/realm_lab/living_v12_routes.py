"""Separately pinned v12 bridge transit; v11 interaction evidence is immutable.

These anchors guide the existing physical planner. Every launched movement is
still revalidated by the engine; this catalog is not movement permission.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import math
from pathlib import Path

try:
    from . import living_v11 as previous, single_return as base, settlement_population as settlement
except ImportError:
    import living_v11 as previous
    import single_return as base
    import settlement_population as settlement

PROFILE='living-v12'
SOURCE_REVISION='preview-v12'
PROOF='transit-route-verification.json'
BRIDGE_PROOF='bridge-navigation-verification.json'
HEADER_RELATIVE='src/server/game/AIWorld/Agent/LivingResidentsV12Routes.h'
HEADER=Path(__file__).resolve().parents[2]/HEADER_RELATIVE
FLAG='AIWorld.LivingResidentsV12.BridgeRoutesEnabled'
PAIRS={(a,b) for a in range(1,7) for b in range(1,7) if a!=b}
REQUIRED_BRIDGE_PAIRS={(4,5),(5,4),(4,6),(6,4)}
require,ProfileError=base.require,base.ProfileError


def digest_file(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def route_catalog(proof):
    """Only values compiled into the separate runtime transit vector."""
    return [dict(from_depot_id=route['from_depot_id'],to_depot_id=route['to_depot_id'],
        points=[dict(map_id=725,**{k:point[k] for k in ('x','y','z')}) for point in route['points']])
        for route in sorted(proof['routes'],key=lambda route:(route['from_depot_id'],route['to_depot_id']))]


def render_header(proof,proof_sha256):
    require(isinstance(proof_sha256,str) and len(proof_sha256)==64 and all(c in '0123456789abcdef' for c in proof_sha256),
        'Transit header requires the exact proof SHA256')
    def number(value):
        text=format(float(value),'.17g')
        return text+('f' if '.' in text or 'e' in text else '.0f')
    lines=['/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.',
        ' * Licensed under the GNU General Public License, version 2 or later. */',
        '#ifndef AIWORLD_LIVINGRESIDENTSV12ROUTES_H','#define AIWORLD_LIVINGRESIDENTSV12ROUTES_H',
        '#include "LivingResidentsV11.h"','#include <vector>','',
        '// Generated from preview-v12/transit-route-verification.json.',
        '// Separate transit override; v11 interaction coordinates remain unchanged.',
        '// The live planner still proves floor, body, tiles, scope and ownership.',
        'namespace LivingResidentsV12','{',
        f'    inline constexpr char ProofSha256[] = "{proof_sha256}";',
        '    inline const std::vector<LivingResidentsV11::RouteSpec> Routes{']
    for route in route_catalog(proof):
        lines.append(f'        {{{route["from_depot_id"]}, {route["to_depot_id"]}, {{')
        for point in route['points']:
            lines.append('            {725, '+', '.join(number(point[k]) for k in ('x','y','z'))+'},')
        lines.append('        }},')
    lines+=['    };','}','#endif','']
    return '\n'.join(lines)


def distance(a,b):return math.dist([a[k] for k in ('x','y','z')],[b[k] for k in ('x','y','z')])


def native_proof(value):
    require(isinstance(value,dict) and isinstance(value.get('status'),dict) and value.get('complete') is True and
        value.get('all_corridor_flags_ground_only') is True and value.get('status',{}).get('succeeded') is True and
        all(value['status'].get(key) is False for key in ('partial','buffer_too_small','out_of_nodes','failed')),
        'Transit leg requires a complete ground-only native path')
    if 'straight_status' in value:
        status=value['straight_status']
        require(isinstance(status,dict) and status.get('succeeded') is True and
            all(status.get(key) is False for key in ('partial','buffer_too_small','out_of_nodes','failed')),
            'Transit native straight-path result is incomplete')


def physical_proof(value):
    require(isinstance(value,dict) and all(value.get(key) is True for key in
        ('body_clear','supported','dry','source_all_water_layers_checked')),
        'Transit route requires full body, supported floor and all-water dry evidence')
    numeric={key:base.finite(value.get(key)) for key in
        ('body_radius_yards','collision_height_yards','max_height_step_yards','sample_spacing_yards')}
    require(numeric['body_radius_yards']>=.306 and numeric['collision_height_yards']>=2.0309 and
        0<=numeric['max_height_step_yards']<=.75 and 0<numeric['sample_spacing_yards']<=.5 and
        all(type(value.get(key)) is int and value[key]>0 for key in ('samples','water_footprint_samples')),
        'Transit physical sampling or actor body bounds were weakened')


def validate_proof(proof,population,positions,baseline_document):
    require(isinstance(proof,dict) and all(proof.get(key)==value for key,value in
        (('schema_version',1),('profile',PROFILE),('source_revision',SOURCE_REVISION),('realm_id',2),
         ('map_id',725),('area_id',4988),('all_directed_pairs_complete',True))),
        'Transit proof belongs to another profile, map or incomplete depot graph')
    require(proof.get('geometry_sha256')==positions['geometry_sha256'] and
        proof.get('geometry_files')==list(population.geometry_files) and proof.get('source_files')==positions['source_files'] and
        proof.get('living_position_proof_sha256')==digest_file(population.metadata/previous.POSITION_PROOF) and
        proof.get('baseline_v11_position_proof_sha256')==baseline_document['position_proof_sha256'] and
        proof.get('legacy_locations_header_sha256')==baseline_document['locations_header_sha256'],
        'Transit proof is not bound to the current geometry and immutable interaction baseline')
    bridge_path=population.metadata/BRIDGE_PROOF
    bridge=base.load_json(bridge_path)
    require(proof.get('bridge_navigation_proof_sha256')==digest_file(bridge_path) and bridge.get('ok') is True and
        bridge.get('geometry_sha256')==positions['geometry_sha256'],
        'Transit bridge evidence does not match the new native map')
    bridge_ids={row['source_unique_id'] for row in bridge['bridges']}
    routes=proof.get('routes')
    require(isinstance(routes,list) and len(routes)==30 and all(isinstance(route,dict) for route in routes) and
        {(r.get('from_depot_id'),r.get('to_depot_id')) for r in routes}==PAIRS,
        'Transit catalog requires each of the exact30 directed depot pairs once')
    depots={row['id']:row['position'] for row in positions['depots']}
    maximum=0.0
    for route in routes:
        start,end=route['from_depot_id'],route['to_depot_id']
        require(type(start) is int and type(end) is int,'Transit depot identities must be integers')
        points=route.get('points')
        require(isinstance(points,list) and 2<=len(points)<=64,'Transit route point count is outside its bound')
        for point in points:
            require(isinstance(point,dict) and point.get('map_id')==725 and all(
                type(point.get(k)) in (int,float) and math.isfinite(point[k]) for k in ('x','y','z')),
                'Transit anchor is not a finite point on the custom map')
            require(settlement.ACTIVE_BOUNDS['min_x']<=point['x']<=settlement.ACTIVE_BOUNDS['max_x'] and
                settlement.ACTIVE_BOUNDS['min_y']<=point['y']<=settlement.ACTIVE_BOUNDS['max_y'],
                'Transit anchor leaves the configured simulation boundary')
        require(all(points[0][k]==depots[start][k] and points[-1][k]==depots[end][k] for k in ('x','y','z')),
            'Transit routes must preserve the exact six existing interaction endpoints')
        lengths=[distance(a,b) for a,b in zip(points,points[1:])]
        require(all(.01<length<=160 for length in lengths),'Transit route has a zero or oversized leg')
        maximum=max(maximum,*lengths)
        require(math.isclose(base.finite(route.get('length_yards')),sum(lengths),rel_tol=1e-8,abs_tol=1e-5),
            'Transit route length differs from its compiled ordered anchors')
        native_proof(route.get('native'));physical_proof(route.get('physical'))
        crossed=route.get('bridge_uids_crossed')
        require(isinstance(crossed,list) and all(type(uid) is int and uid in bridge_ids for uid in crossed) and
            len(crossed)==len(set(crossed)), 'Transit route claims an unknown bridge')
        require((start,end) not in REQUIRED_BRIDGE_PAIRS or crossed,
            'The reviewed southern transit pairs must cross the new physical bridges')
        legs=route.get('legs')
        require(isinstance(legs,list) and len(legs)==len(points)-1 and all(isinstance(leg,dict) for leg in legs),
            'Transit route lacks complete leg evidence')
        for index,leg in enumerate(legs):
            require(leg.get('from_index')==index and leg.get('to_index')==index+1 and
                math.isclose(base.finite(leg.get('length_yards')),lengths[index],rel_tol=1e-8,abs_tol=1e-5),
                'Transit physical leg evidence is reordered or changed')
            native_proof(leg.get('native'));physical_proof(leg.get('physical'))
    require(math.isclose(base.finite(proof.get('maximum_leg_yards')),maximum,rel_tol=1e-8,abs_tol=1e-5) and maximum<=160,
        'Transit maximum leg summary is wrong')
    require(proof.get('routes_sha256')==settlement.digest(route_catalog(proof)),
        'Transit runtime routes do not match their separate catalog digest')


@dataclass(frozen=True)
class TransitCatalog:
    proof: dict
    contract: dict

    @classmethod
    def load(cls,population,positions,baseline_document,expected=None,header=None):
        path=population.metadata/PROOF
        proof=base.load_json(path)
        validate_proof(proof,population,positions,baseline_document)
        compiled=header or HEADER
        require(compiled.read_text(encoding='utf-8')==render_header(proof,digest_file(path)),
            'Compiled v12 transit header does not exactly render the reviewed route proof')
        contract=dict(schema_version=1,source_revision=SOURCE_REVISION,proof_file=PROOF,
            proof_sha256=digest_file(path),header=HEADER_RELATIVE,header_sha256=digest_file(compiled),
            routes_sha256=settlement.digest(route_catalog(proof)),route_count=30)
        require(expected is None or expected==contract,'V12 transit manifest proof/header/catalog pins changed')
        return cls(proof,contract)
