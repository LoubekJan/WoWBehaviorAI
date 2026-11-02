"""A controlled v11 world preserves every authored actor and rejects drift."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import living_v11 as living, residents_v11 as native, residents_population as old
from tools.realm_lab.tests.test_residents_population import write_json
from tools.realm_lab.tests.test_residents_v11 import fixture as native_fixture, native_snapshot


def fixture(root):
    metadata,geometry,_,_,baseline=native_fixture(root)
    with patch.object(old.Population,"load",return_value=baseline):
        population=native.Population.load(metadata,geometry)
    proof=json.loads((native.METADATA/living.POSITION_PROOF).read_text(encoding='utf-8'))
    proof.update(geometry_files=list(population.geometry_files),geometry_sha256=native.geometry_digest(population.geometry_files),
        resident_position_proof_sha256=hashlib.sha256((metadata/native.POSITION_PROOF).read_bytes()).hexdigest())
    write_json(metadata/living.POSITION_PROOF,proof)
    sha=hashlib.sha256((metadata/living.POSITION_PROOF).read_bytes()).hexdigest()
    header=metadata/'locations.h';header.write_text(f'inline constexpr char ProofSha256[] = "{sha}";\n',encoding='utf-8')
    _proof,contract=living.position_contract(population,header)
    document=living.manifest_document(population,hashlib.sha256((metadata/native.MANIFEST).read_bytes()).hexdigest(),contract)
    write_json(metadata/living.MANIFEST,document)
    return metadata,geometry,population,document,baseline


def controlled_snapshot(population, control=1):
    rows=native_snapshot(population.native_population)
    for a in population.actors:
        rows.extend((dict(kind="agent",id=a.spawn_id,spawn=a.spawn_id,map=725,type=living.agent_type(a.spawn_id),
            control=control,home_map=725,home_x=a.x,home_y=a.y,home_z=a.z,home_o=a.orientation,
            faction=living.world_faction(a.spawn_id),work_map=None,work_x=None,work_y=None,work_z=None,work_o=None),
            dict(kind="participation",spawn=a.spawn_id,mode=0)))
    return rows


def telemetry(population):
    rows=[]
    for a in population.actors:
        point=dict(map_id=725,x=a.x,y=a.y,z=a.z)
        rows.append(dict(spawn_id=a.spawn_id,agent_id=a.spawn_id,entry=a.entry,template_entry=native.expected_entry(a.spawn_id),
            native_creature=False,control_mode="AI_WORLD_CONTROLLED",world_faction=living.world_faction(a.spawn_id),
            type="COMBATANT" if a.spawn_id in living.DEFIAS_IDS else "CIVILIAN",alive=True,runtime_guid=f'Creature-{a.spawn_id}',
            position={**point,"source":"live"},home=point,needs=dict(hunger=.1,fatigue=.1),economy={},
            living_role=dict(enabled=True,extensions_enabled=True,role="RAIDER" if a.spawn_id in living.DEFIAS_IDS else "WORKER",lifetime_ms=10),
            supply=dict(enabled=True,role={"farmer":"FARMER","village_consumer":"CONSUMER","city_consumer":"CONSUMER",
                "city_carrier":"CARAVAN_CARRIER","raider":"RAIDER"}[living.capability(a.spawn_id)],
                depot_id=living.depot_id(a.spawn_id),accounting=dict(conserved=True)),llm_agent=None))
    return dict(version=4,stale=False,captured_at_ms=12345,agents=rows)


class LivingV11Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.metadata,self.geometry,self.native,self.document,self.baseline=fixture(Path(self.tmp.name))
        p=patch.object(old.Population,"load",return_value=self.baseline);p.start();self.addCleanup(p.stop)
        p=patch.object(living,'LOCATIONS_HEADER',self.metadata/'locations.h');p.start();self.addCleanup(p.stop)
        self.population=living.Population.load(self.metadata,self.geometry)

    def test_all45_homes_entries_factions_preserved_with_separate_control_capabilities(self):
        self.assertEqual(self.population.actors,self.native.actors)
        self.assertEqual(self.population.geometry_files,self.native.geometry_files)
        self.assertEqual(self.population.expected_entry(902035),9810035)
        self.assertEqual(sum(living.agent_type(a.spawn_id)==0 for a in self.population.actors),35)
        self.assertEqual(sum(living.agent_type(a.spawn_id)==5 for a in self.population.actors),10)
        self.assertEqual([sum(r['capability']==k for r in self.document['actors']) for k in
            ('farmer','village_consumer','city_carrier','city_consumer','raider')],[12,4,2,17,10])

    def test_control_manifest_cannot_reassign_identity_home_or_source_geometry(self):
        for mutate in (lambda d:d['actors'][0].update(agent_type=1),
            lambda d:d['actors'][35].update(world_faction_id=1),lambda d:d['actors'][0]['home'].update(x=42),
            lambda d:d.update(native_manifest_sha256='f'*64),lambda d:d['owned_spawn_ids'].append(901000)):
            candidate=copy.deepcopy(self.document);mutate(candidate);write_json(self.metadata/living.MANIFEST,candidate)
            with self.subTest(mutate=mutate),self.assertRaises(living.ProfileError):
                living.Population.load(self.metadata,self.geometry)

    def test_runtime_header_and_native_graph_are_bound_to_exact_reviewed_position_bytes(self):
        header=self.metadata/'locations.h'
        before=header.read_bytes();header.write_bytes(before+b'// unexpected runtime position changes\n')
        with self.assertRaises(living.ProfileError):living.Population.load(self.metadata,self.geometry)
        header.write_bytes(before)
        raw=(self.metadata/living.POSITION_PROOF).read_bytes()
        proof=json.loads(raw)
        for mutate in (lambda p:p['native_navigation'].update(complete_paths=345),
            lambda p:p['waypoint_navigation']['paths'][0].update(complete=False),
            lambda p:p['depots'][0].update(source_unique_id=199),
            lambda p:p['fields'][0]['work_points'][0].update(crop_alpha=0),
            lambda p:p['runtime_routes'].pop(),lambda p:p.update(geometry_sha256='f'*64)):
            candidate=copy.deepcopy(proof);mutate(candidate)
            write_json(self.metadata/living.POSITION_PROOF,candidate)
            with self.subTest(mutate=mutate),self.assertRaises(living.ProfileError):
                living.Population.load(self.metadata,self.geometry)
        (self.metadata/living.POSITION_PROOF).write_bytes(raw)

    def test_native_transition_complete_control_and_paused_phases_have_explicit_gates(self):
        native_rows=native_snapshot(self.native)
        living.validate_snapshot(native_rows,self.population,bootstrap=True)
        with self.assertRaises(living.ProfileError):living.validate_snapshot(native_rows,self.population)
        rows=controlled_snapshot(self.population)
        living.validate_snapshot(rows,self.population)
        paused=controlled_snapshot(self.population,control=0)
        living.validate_snapshot(paused,self.population,bootstrap=True)
        living.validate_snapshot(paused,self.population,control=0)
        with self.assertRaises(living.ProfileError):living.validate_snapshot(paused,self.population)

    def test_partial_state_foreign_refs_retired_memories_and_group_memberships_refused(self):
        rows=controlled_snapshot(self.population)
        for bad in ([r for r in rows if not(r.get('kind')=='agent' and r.get('id')==902000)],
            [*rows,dict(kind='memory',id=901035)], [*rows,dict(kind='membership',member=902000)],
            [*rows,dict(kind='agent',id=901035)], [*rows,dict(kind='participation',spawn=902000,mode=0)]):
            with self.subTest(rows=len(bad)),self.assertRaises(living.ProfileError):
                living.validate_snapshot(bad,self.population,bootstrap=True)
        living.validate_snapshot([*rows,dict(kind='memory',id=902000)],self.population)

    def test_agent_type_work_home_and_control_drift_refused_without_repair(self):
        for changes in (dict(home_x=99),dict(spawn=902001),dict(type=5),dict(faction=2),
            dict(work_map=725),dict(control=3),dict(map=0),dict(home_map=0),dict(home_x=float('nan'))):
            rows=controlled_snapshot(self.population)
            next(r for r in rows if r.get('kind')=='agent' and r['id']==902000).update(changes)
            with self.subTest(changes=changes),self.assertRaises(living.ProfileError):
                living.validate_snapshot(rows,self.population,bootstrap=True)

    def test_native_guard_still_rejects_foreign_spawns_templates_and_custom_factions(self):
        for kind,key,value in (('spawn','x',42),('faction_default','world_faction',99),('template','ai_name','other')):
            rows=controlled_snapshot(self.population)
            next(r for r in rows if r.get('kind')==kind)[key]=value
            with self.subTest(kind=kind),self.assertRaises(living.ProfileError):
                living.validate_snapshot(rows,self.population,bootstrap=True)

    def test_bootstrap_never_inserts_creatures_mutates_templates_or_touches_retired_rows(self):
        sql=living.bootstrap_sql(self.population)
        self.assertEqual(sql.count('INSERT INTO characters.ai_agents ('),45)
        self.assertEqual(sql.count('INSERT INTO world.ai_spawn_participation_defaults ('),45)
        self.assertNotIn('INSERT INTO world.creature',sql)
        self.assertNotIn('UPDATE world.creature',sql)
        self.assertNotIn('DELETE ',sql)
        self.assertIn('SET TRANSACTION ISOLATION LEVEL SERIALIZABLE',sql)
        self.assertIn('WHERE @living_v11_ready=1',sql)
        self.assertIn('a.work_map_id IS NULL',sql)
        self.assertIn('UPDATE characters.ai_agents SET control_mode=1',sql)

    def test_snapshot_retains_native_reference_checks_and_adds_full_agent_proof(self):
        sql=living.snapshot_sql(self.population)
        self.assertIn("'home_map',home_map_id",sql)
        self.assertIn("'mode',participation_mode",sql)
        self.assertIn('BETWEEN 901000 AND 901099',sql)
        self.assertIn('BETWEEN 902000 AND 902044',sql)
        self.assertIn("'kind','membership'",sql)
        self.assertIn("'kind','source_faction'",sql)

    def test_config_enables_resident_runtime_but_cannot_reactivate_old100_or_llm(self):
        config=self.population.config()
        for key in ('AIWorld.Enable','AIWorld.LivingRolesEnabled','AIWorld.LivingRoleExtensionsEnabled',
            'AIWorld.LivingNeedEvolutionEnabled','AIWorld.LivingResidentsV11.Enabled'):
            self.assertEqual(config[key],'1')
        for key in ('AIWorld.LivingSupplyEnabled','AIWorld.LivingConsumersEnabled','AIWorld.LivingRaidsEnabled',
            'AIWorld.LivingFieldsEnabled','AIWorld.LivingDefias.Enabled','AIWorld.LivingCaravans.Enabled'):
            self.assertEqual(config[key],'0')
        self.assertNotIn('901000',config['AIWorld.ScopeSpawnIds'])
        self.assertNotIn('AIWorld.NpcAgentEnabled',config)

    def test_telemetry_requires_all45_controlled_actors_without_moving_their_homes(self):
        state=telemetry(self.population)
        self.assertEqual(living.validate_telemetry(state,self.population)['managed'],45)
        state['agents'][0]['position']['x']+=10
        living.validate_telemetry(state,self.population)
        for key,value in (('native_creature',True),('control_mode','OBSERVE_ONLY'),('entry',9810000),
            ('template_entry',11328),('needs',None),('living_role',None),('world_faction',2),('llm_agent',{})):
            bad=telemetry(self.population);bad['agents'][0][key]=value
            with self.subTest(key=key),self.assertRaises(living.ProfileError):living.validate_telemetry(bad,self.population)
        bad=telemetry(self.population);bad['agents'][0]['home']['x']+=10
        with self.assertRaises(living.ProfileError):living.validate_telemetry(bad,self.population)

    def test_dead_or_unloaded_resident_remains_owned_without_invented_live_role(self):
        state=telemetry(self.population)
        state['agents'][0].update(alive=False,runtime_guid=None,living_role=None,supply=None)
        state['agents'][0]['position']['source']='spawn'
        self.assertEqual(living.validate_telemetry(state,self.population)['live_roles'],44)

    def test_food_capability_and_ledger_or_invalid_needs_cannot_spoof_success(self):
        for mutate in (lambda r:r['needs'].update(hunger=float('nan')),lambda r:r['needs'].update(fatigue=True),
            lambda r:r['supply'].update(role='RAIDER'),lambda r:r['supply'].update(depot_id=6),
            lambda r:r['supply']['accounting'].update(conserved=False),lambda r:r['living_role'].update(role='NONE'),
            lambda r:r.update(runtime_guid=None)):
            state=telemetry(self.population);mutate(state['agents'][0])
            with self.subTest(mutate=mutate),self.assertRaises(living.ProfileError):living.validate_telemetry(state,self.population)


if __name__=='__main__':unittest.main()
