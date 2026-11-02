"""A v12 transit override never rewrites v11 interactions or silently activates."""
import copy
from dataclasses import replace
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import living_v11,living_v12,living_v12_routes as routes,manage,residents_population as v10,residents_v12,settlement_population
from tools.realm_lab.tests.test_living_v12 import fixture
from tools.realm_lab.tests.test_residents_population import write_json
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests import test_living_v11_runtime_mounts as mounts


def native():
    return dict(complete=True,all_corridor_flags_ground_only=True,
        status=dict(succeeded=True,partial=False,buffer_too_small=False,out_of_nodes=False,failed=False))


def physical():
    return dict(body_radius_yards=.306,collision_height_yards=2.031,max_height_step_yards=.1,
        sample_spacing_yards=.5,body_clear=True,supported=True,dry=True,source_all_water_layers_checked=True,
        samples=2000,water_footprint_samples=26000)


def proof_fixture(population,baseline):
    positions=population.positions;metadata=population.metadata
    write_json(metadata/routes.BRIDGE_PROOF,dict(schema_version=1,ok=True,geometry_sha256=positions['geometry_sha256'],
        bridges=[dict(source_unique_id=232)]))
    depots={row['id']:row['position'] for row in positions['depots']}
    result=dict(schema_version=1,profile='living-v12',source_revision='preview-v12',realm_id=2,map_id=725,area_id=4988,
        all_directed_pairs_complete=True,geometry_sha256=positions['geometry_sha256'],
        geometry_files=list(population.geometry_files),source_files=positions['source_files'],
        living_position_proof_sha256=routes.digest_file(metadata/living_v12.POSITION_PROOF),
        baseline_v11_position_proof_sha256=baseline['position_proof_sha256'],
        legacy_locations_header_sha256=baseline['locations_header_sha256'],
        bridge_navigation_proof_sha256=routes.digest_file(metadata/routes.BRIDGE_PROOF),routes=[])
    maximum=0
    for start,end in sorted(routes.PAIRS):
        a,b=depots[start],depots[end];count=math.ceil(routes.distance(a,b)/100)
        points=[dict(map_id=725,**{k:a[k]+(b[k]-a[k])*i/count for k in ('x','y','z')}) for i in range(count+1)]
        points[0]=dict(map_id=725,**{k:a[k] for k in ('x','y','z')})
        points[-1]=dict(map_id=725,**{k:b[k] for k in ('x','y','z')})
        lengths=[routes.distance(a,b) for a,b in zip(points,points[1:])]
        maximum=max(maximum,*lengths)
        result['routes'].append(dict(from_depot_id=start,to_depot_id=end,points=points,length_yards=sum(lengths),
            bridge_uids_crossed=[232],native=native(),physical=physical(),legs=[dict(from_index=i,to_index=i+1,
                length_yards=length,native=native(),physical=physical()) for i,length in enumerate(lengths)]))
    result.update(maximum_leg_yards=maximum,routes_sha256=settlement_population.digest(routes.route_catalog(result)))
    return result


class LivingV12TransitTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.metadata,self.geometry,self.native11,self.baseline,self.original,self.old_baseline,self.metadata11=fixture(Path(self.tmp.name))
        for owner,name,value in ((v10.Population,'load',self.old_baseline),(residents_v12,'baseline_population',self.native11),
            (living_v12,'compiled_contract',(self.baseline,self.original))):
            p=patch.object(owner,name,return_value=value);p.start();self.addCleanup(p.stop)
        for owner,name,value in ((living_v11,'LOCATIONS_HEADER',self.metadata11/'locations.h'),
            (routes,'HEADER',self.metadata/'transit.h')):
            p=patch.object(owner,name,value);p.start();self.addCleanup(p.stop)
        self.legacy=living_v12.Population.load(self.metadata,self.geometry)
        self.original_manifest=(self.metadata/living_v12.MANIFEST).read_bytes()
        self.proof=proof_fixture(self.legacy,self.original)
        self.publish(self.proof)

    def publish(self,proof):
        candidate=copy.deepcopy(proof)
        candidate['routes_sha256']=settlement_population.digest(routes.route_catalog(candidate))
        write_json(self.metadata/routes.PROOF,candidate)
        routes.HEADER.write_bytes(routes.render_header(candidate,routes.digest_file(self.metadata/routes.PROOF)).encode())
        contract=dict(schema_version=1,source_revision='preview-v12',proof_file=routes.PROOF,
            proof_sha256=routes.digest_file(self.metadata/routes.PROOF),header=routes.HEADER_RELATIVE,
            header_sha256=routes.digest_file(routes.HEADER),routes_sha256=candidate['routes_sha256'],route_count=30)
        document=json.loads(self.original_manifest);document['transit_route_catalog']=contract
        write_json(self.metadata/living_v12.MANIFEST,document)
        return candidate

    def load(self):return living_v12.Population.load(self.metadata,self.geometry)

    def test_catalog_is_separate_preserves_all45_homes_and_sql_and_covers_all_pairs(self):
        population=self.load()
        self.assertEqual(population.actors,self.legacy.actors)
        self.assertEqual(population.positions,self.legacy.positions)
        self.assertEqual(population.geometry_files,self.legacy.geometry_files)
        self.assertEqual(living_v12.bootstrap_sql(population),living_v12.bootstrap_sql(self.legacy))
        self.assertEqual(len(population.transit_route_catalog.proof['routes']),30)
        self.assertEqual(population.config()[routes.FLAG],'1')
        self.assertEqual(population.transit_route_catalog.contract['routes_sha256'],settlement_population.digest(routes.route_catalog(self.proof)))

    def test_historical_manifest_does_not_activate_new_files_by_discovery(self):
        (self.metadata/living_v12.MANIFEST).write_bytes(self.original_manifest)
        routes.HEADER.unlink()
        legacy=self.load()
        self.assertIsNone(legacy.transit_route_catalog)
        self.assertEqual(legacy.config()[routes.FLAG],'0')

    def test_active_paused_v11_and_empty_profiles_have_explicit_config_gates(self):
        population=self.load()
        settings=manage.Settings.load({**environment(),'LAB_AI_PROFILE':'living-v12','LAB_WILDLIFE_ENABLED':'1'})
        active=manage.config_overrides(settings,population)
        self.assertEqual(active[routes.FLAG],'1');self.assertEqual(active['AIWorld.NpcAgentEnabled'],'0')
        paused=manage.config_overrides(replace(settings,wildlife_enabled=False),population)
        self.assertEqual(paused[routes.FLAG],'0');self.assertEqual(paused['AIWorld.LivingResidentsV11.Enabled'],'0')
        previous=manage.Settings.load({**environment(),'LAB_AI_PROFILE':'living-v11','LAB_WILDLIFE_ENABLED':'1'})
        self.assertEqual(manage.config_overrides(previous,self.baseline)[routes.FLAG],'0')
        empty=manage.Settings.load({**environment(),'LAB_AI_PROFILE':'authoring-empty'})
        self.assertEqual(manage.config_overrides(empty)[routes.FLAG],'0')

    def test_missing_directed_pair_and_reordered_leg_or_changed_endpoint_are_rejected_even_with_new_pins(self):
        changes=(lambda p:p['routes'].pop(),lambda p:p['routes'][0]['legs'][0].update(to_index=2),
            lambda p:p['routes'][0]['points'][0].update(x=p['routes'][0]['points'][0]['x']+.1))
        for change in changes:
            proof=copy.deepcopy(self.proof);change(proof);self.publish(proof)
            with self.subTest(change=change),self.assertRaises(routes.ProfileError):self.load()

    def test_partial_native_or_weakened_body_water_sampling_or_unknown_bridge_are_rejected(self):
        changes=(lambda p:p['routes'][0]['native']['status'].update(partial=True),
            lambda p:p['routes'][0]['legs'][0]['native'].update(complete=False),
            lambda p:p['routes'][0]['legs'][0]['native'].update(straight_status={**native()['status'],'buffer_too_small':True}),
            lambda p:p['routes'][0]['legs'][0]['physical'].update(body_radius_yards=.1),
            lambda p:p['routes'][0]['physical'].update(dry=False),
            lambda p:p['routes'][0]['physical'].update(sample_spacing_yards=1),
            lambda p:p['routes'][0].update(bridge_uids_crossed=[999]))
        for change in changes:
            proof=copy.deepcopy(self.proof);change(proof);self.publish(proof)
            with self.subTest(change=change),self.assertRaises(routes.ProfileError):self.load()

    def test_geometry_baseline_and_compiled_source_cannot_be_substituted(self):
        for key in ('geometry_sha256','living_position_proof_sha256','baseline_v11_position_proof_sha256',
            'legacy_locations_header_sha256','bridge_navigation_proof_sha256'):
            proof=copy.deepcopy(self.proof);proof[key]='f'*64;self.publish(proof)
            with self.subTest(key=key),self.assertRaises(routes.ProfileError):self.load()
        self.publish(self.proof)
        routes.HEADER.write_bytes(routes.HEADER.read_bytes()+b'// unreviewed compiled route\n')
        document=json.loads((self.metadata/living_v12.MANIFEST).read_text())
        document['transit_route_catalog']['header_sha256']=routes.digest_file(routes.HEADER)
        write_json(self.metadata/living_v12.MANIFEST,document)
        with self.assertRaisesRegex(routes.ProfileError,'exactly render'):self.load()

    def test_southern_pairs_cannot_fall_back_to_the_old_coast_only_catalog(self):
        proof=copy.deepcopy(self.proof)
        route=next(route for route in proof['routes'] if (route['from_depot_id'],route['to_depot_id'])==(4,5))
        route['bridge_uids_crossed']=[];self.publish(proof)
        with self.assertRaisesRegex(routes.ProfileError,'southern'):self.load()

    def test_existing64_anchor_and160_yard_caps_remain_explicit(self):
        for count in (64,65):
            proof=copy.deepcopy(self.proof);route=proof['routes'][0];a,b=route['points'][0],route['points'][-1]
            points=[dict(map_id=725,**{k:a[k]+(b[k]-a[k])*i/(count-1) for k in ('x','y','z')}) for i in range(count)]
            points[0]=a;points[-1]=b;route['points']=points
            lengths=[routes.distance(a,b) for a,b in zip(points,points[1:])]
            route['length_yards']=sum(lengths)
            route['legs']=[dict(from_index=i,to_index=i+1,length_yards=length,native=native(),physical=physical())
                for i,length in enumerate(lengths)]
            self.publish(proof)
            if count==64:self.assertEqual(len(self.load().transit_route_catalog.proof['routes'][0]['points']),64)
            else:
                with self.assertRaisesRegex(routes.ProfileError,'point count'):self.load()
        proof=copy.deepcopy(self.proof);route=proof['routes'][0]
        route['points']=[route['points'][0],route['points'][-1]];self.publish(proof)
        with self.assertRaisesRegex(routes.ProfileError,'oversized leg'):self.load()


class V12TransitRuntimeBindings(unittest.TestCase):
    @unittest.skipUnless(shutil.which('git'),'Git is required to verify cross-platform header checkout bytes')
    def test_windows_style_checkout_keeps_the_digest_pinned_header_as_lf(self):
        # Exercise Git's checkout machinery with autocrlf enabled. A bare
        # text assertion would not prove the runtime header keeps its hashed
        # bytes when checked out on a Windows deployment workstation.
        with tempfile.TemporaryDirectory(prefix='v12-header-checkout-') as folder:
            root=Path(folder).resolve()
            self.assertTrue(root.is_relative_to(Path(tempfile.gettempdir()).resolve()))
            (root/'.gitattributes').write_bytes((mounts.ROOT/'.gitattributes').read_bytes())
            path=root/routes.HEADER_RELATIVE;path.parent.mkdir(parents=True)
            header=routes.render_header(dict(routes=[dict(from_depot_id=1,to_depot_id=2,
                points=[dict(map_id=725,x=1,y=2,z=3),dict(map_id=725,x=4,y=5,z=6)])]),'a'*64).encode()
            path.write_bytes(header)
            for command in (['init','-q'],['add','.gitattributes',routes.HEADER_RELATIVE],
                ['-c','core.autocrlf=true','checkout-index','--force','--all','--prefix=checked-out/']):
                result=subprocess.run(['git',*command],cwd=root,capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stderr[-1500:])
            checked=(root/'checked-out'/routes.HEADER_RELATIVE).read_bytes()
            self.assertNotIn(b'\r\n',checked)
            self.assertEqual(hashlib.sha256(checked).digest(),hashlib.sha256(header).digest())

    def test_exact_four_consumers_mount_new_header_as_readonly_without_whole_checkout(self):
        blocks=mounts.consumer_blocks()
        binding=f'./{routes.HEADER_RELATIVE}:/workspace/{routes.HEADER_RELATIVE}:ro'
        self.assertEqual({name for name,block in blocks.items() if binding in block},set(mounts.CONSUMERS))
        for name in mounts.CONSUMERS:
            self.assertIn((routes.HEADER_RELATIVE,routes.HEADER_RELATIVE,'ro'),mounts.workspace_binds(blocks[name]))
            self.assertNotIn('.:/workspace',blocks[name])

    @unittest.skipUnless(shutil.which('docker'),'Docker Compose CLI is required for normalized mount validation')
    def test_compose_normalized_catalog_mount_is_readonly_for_all_consumers(self):
        result=subprocess.run(['docker','compose','--profile','*','--env-file',str(mounts.ROOT/'.env.example'),
            '-f',str(mounts.ROOT/'compose.yml'),'config','--format','json'],cwd=mounts.ROOT,
            env=dict(os.environ),capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stderr[-2000:])
        services=json.loads(result.stdout)['services']
        for name in mounts.CONSUMERS:
            matches=[v for v in services[name]['volumes'] if v['target']=='/workspace/'+routes.HEADER_RELATIVE]
            self.assertEqual(len(matches),1)
            self.assertEqual(matches[0]['type'],'bind');self.assertIs(matches[0]['read_only'],True)
            self.assertEqual(Path(matches[0]['source']).resolve(),routes.HEADER.resolve())


if __name__=='__main__':unittest.main()
