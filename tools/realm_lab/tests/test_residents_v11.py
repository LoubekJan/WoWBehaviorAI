"""V11 preserves households and adds only reviewed inn, hall and Defias actors."""
import copy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.realm_lab import character_templates, manage, residents_population as old, residents_v11 as current
from tools.realm_lab.tests.test_residents_population import fixture as old_fixture, write_json, native_snapshot as old_snapshot
from tools.realm_lab.tests.test_setup import environment


def fixture(root):
    old_metadata, old_geometry, _, old_proof = old_fixture(root)
    baseline = old.Population.load(old_metadata, old_geometry)
    metadata = root / "preview-v11"
    metadata.mkdir()
    geometry = SimpleNamespace(**{**old_geometry.__dict__, "source_revision": "preview-v11", "metadata": metadata})
    proof = copy.deepcopy(old_proof)
    proof.update(profile=current.PROFILE, source_revision=current.SOURCE_REVISION)
    proof["native_navigation"].update(valid_projections=46, required_paths=90, complete_paths=90)
    for group in proof["buildings"]:
        for point in group["positions"]:
            point["spawn_id"] = 902000 + current.HOUSE_IDS.index(group["house_id"]) * 2 + point["slot"] - 1
    for house, count, start in ((209, 3, 902022), (199, 10, 902025)):
        points = []
        for slot in range(1, count + 1):
            point = copy.deepcopy(old_proof["buildings"][0]["positions"][0])
            point.update(slot=slot, spawn_id=start + slot - 1, x=300 + slot * 3, y=300 if house == 209 else 400)
            points.append(point)
        proof["buildings"].append(dict(house_id=house, mesh_file="farm.vmo", mesh_sha256="a"*64, positions=points))
    points = []
    for slot in range(1, 11):
        point = copy.deepcopy(old_proof["buildings"][0]["positions"][0])
        point.update(slot=slot, spawn_id=902034 + slot, x=100 + slot * 3, y=1000)
        point["geometry"].update(indoor=False, group_flags=0, headroom=2.4)
        points.append(point)
    proof["zones"] = [dict(zone_id="left-tent-camp", kind="outdoor", source_ids=[223,224,225], positions=points)]
    proof_path = metadata / current.POSITION_PROOF
    write_json(proof_path, proof)
    faction_path = metadata / current.FACTION_PROOF
    write_json(faction_path,dict(schema_version=1,profile=current.PROFILE,source_revision=current.SOURCE_REVISION,
                                realm_id=2,map_id=725,ok=True))
    positions = {p["spawn_id"]:p for group in [*proof["buildings"], *proof["zones"]] for p in group["positions"]}
    actors = []
    for spawn in sorted(current.SPAWN_IDS):
        house, slot, role, faction, entry = current.placement(spawn)
        point = positions[spawn]
        actors.append(dict(spawn_id=spawn, entry=entry, house_id=house, slot=slot, role=role, faction=faction,
                           label=f"aiworld_lab_resident_{spawn}", **{k:point[k] for k in ("x","y","z","orientation")}))
    document = dict(schema_version=1, profile=current.PROFILE, realm_id=2, map_id=725, area_id=4988,
                    source_revision=current.SOURCE_REVISION, geometry_sha256=current.geometry_digest(geometry.geometry_files),
                    position_proof_sha256=hashlib.sha256(proof_path.read_bytes()).hexdigest(), respawn_seconds=300,
                    faction_proof_sha256=hashlib.sha256(faction_path.read_bytes()).hexdigest(),
                    templates=list(current.TEMPLATES), smart_scripts=list(current.DEFIAS_SMART_SCRIPTS), actors=actors)
    write_json(metadata / current.MANIFEST, document)
    return metadata, geometry, document, proof, baseline


def native_snapshot(population):
    rows = old_snapshot(population)
    rows.insert(0, {"kind":"template", **current.DEFIAS_TEMPLATE})
    rows.extend(copy.deepcopy(current.DEFIAS_SMART_SCRIPTS))
    rows.extend((dict(kind="source_faction",entry=38,world_faction=2),dict(kind="source_faction",entry=11328,world_faction=1)))
    rows.extend(dict(kind="faction_default", entry=current.expected_entry(a.spawn_id), world_faction=2 if a.spawn_id in current.DEFIAS_IDS else 1)
                for a in population.actors)
    return rows


def telemetry(population):
    rows = []
    for a in population.actors:
        point=dict(map_id=725,x=a.x,y=a.y,z=a.z)
        rows.append(dict(spawn_id=a.spawn_id,agent_id=a.spawn_id,entry=current.expected_entry(a.spawn_id),template_entry=current.expected_entry(a.spawn_id),
            native_creature=True, control_mode="OBSERVE_ONLY", position={**point,"source":"live"},home=point,
            world_faction=2 if a.spawn_id in current.DEFIAS_IDS else 1,faction_template_id=2300 if a.spawn_id in current.DEFIAS_IDS else 12,
            needs=None,economy=None,living_role=None,supply=None,raid=None,llm_agent=None,in_combat=False))
    return dict(version=4,stale=False,captured_at_ms=12345,agents=rows)


class V11ResidentsTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.metadata,self.geometry,self.document,self.proof,self.baseline=fixture(Path(self.tmp.name))
        patcher=patch.object(old.Population,"load",return_value=self.baseline)
        patcher.start(); self.addCleanup(patcher.stop)
        self.population=current.Population.load(self.metadata,self.geometry)
        self.settings=manage.Settings.load({**environment(),"LAB_AI_PROFILE":current.PROFILE,"LAB_WILDLIFE_ENABLED":"0"})

    def test_exact_45_assignment_preserves_every_existing_household_home(self):
        self.assertEqual(len(self.population.actors),45)
        self.assertEqual({a.spawn_id for a in self.population.actors},current.SPAWN_IDS)
        for actual,old_actor in zip(self.population.actors[:22],self.baseline.actors): self.assertEqual(actual,old_actor)
        self.assertEqual(sum(a.house_id==209 for a in self.population.actors),3)
        self.assertEqual(sum(a.house_id==199 for a in self.population.actors),10)
        self.assertEqual({a.spawn_id for a in self.population.actors if a.faction=="defias"},current.DEFIAS_IDS)
        self.assertTrue(all(a.entry==38 for a in self.population.actors if a.spawn_id in current.DEFIAS_IDS))

    def test_reassignment_or_moved_existing_home_cannot_be_adopted(self):
        for index,changes in ((0,{"x":105}), (35,{"faction":"stormwind"}), (22,{"house_id":199}), (35,{"entry":11328})):
            doc=copy.deepcopy(self.document);doc["actors"][index].update(changes)
            write_json(self.metadata/current.MANIFEST,doc)
            with self.subTest(index=index), self.assertRaises(current.ProfileError):current.Population.load(self.metadata,self.geometry)

    def test_template_cards_accept_23_new_clones_but_not_legacy_or_arbitrary_sources(self):
        specs=[]
        for a in self.population.actors:
            doc=dict(schema_version=1,spawn_id=a.spawn_id,map_id=725,entry=current.expected_entry(a.spawn_id),base_entry=a.entry,name=f"NPC {a.spawn_id}",title="Obyvatel")
            specs.append(character_templates.spec_from_document(doc,self.population))
            with self.assertRaises(current.ProfileError):character_templates.spec_from_document({**doc,"base_entry":295},self.population)
        self.assertEqual(len({s.entry for s in specs}),45)
        self.assertEqual(sum(s.base_entry==38 for s in specs),10)

    def test_bootstrap_accepts_only_reviewed_old_subset_and_inserts_45_faction_defaults(self):
        rows=native_snapshot(self.population)
        current.validate_snapshot(rows,self.population)
        old_rows=[r for r in rows if r["kind"] in ("template","smart_script","source_faction") or r.get("guid") in current.HOUSEHOLD_IDS]
        current.validate_snapshot(old_rows,self.population,bootstrap=True)
        with self.assertRaises(current.ProfileError):current.validate_snapshot(old_rows,self.population)
        sql=current.bootstrap_sql(self.population)
        self.assertEqual(sql.count("INSERT INTO world.creature ("),45)
        self.assertEqual(sql.count("INSERT INTO world.ai_world_faction_entry_defaults"),45)
        self.assertIn("SELECT 9810035,2 WHERE NOT EXISTS",sql)
        self.assertNotIn("UPDATE",sql);self.assertNotIn("DELETE",sql)
        for corrupt in (dict(kind="faction_default",entry=9810035,world_faction=1),dict(kind="agent",id=902035)):
            bad=[r for r in rows if not(r.get("kind")==corrupt["kind"] and r.get("entry")==corrupt.get("entry"))]
            with self.assertRaises(current.ProfileError):current.validate_snapshot([*bad,corrupt],self.population,bootstrap=True)

    def test_native_faction_and_full_clone_identity_remain_visible_without_agents(self):
        state=telemetry(self.population)
        self.assertEqual(current.validate_telemetry(state,self.population)["defias"],10)
        for key,value in (("world_faction",1),("faction_template_id",17),("entry",38),("template_entry",38),("native_creature",False)):
            bad=copy.deepcopy(state);bad["agents"][35][key]=value
            with self.subTest(key=key),self.assertRaises(current.ProfileError):current.validate_telemetry(bad,self.population)
        fighting=copy.deepcopy(state);fighting["agents"][35]["in_combat"]=True;fighting["agents"][35]["position"]["x"]+=10
        current.validate_telemetry(fighting,self.population)
        fighting["agents"][35]["in_combat"]=False
        with self.assertRaises(current.ProfileError):current.validate_telemetry(fighting,self.population)

    def test_managed_bootstrap_accepts_exact_old_22_installed_clones_and_runtime_requires45(self):
        bindings=tuple(SimpleNamespace(spawn_id=a.spawn_id,base_entry=a.entry,entry=current.expected_entry(a.spawn_id)) for a in self.population.actors)
        rows=native_snapshot(self.population)
        for row in rows:
            if row["kind"]=="spawn":row["entry"]=current.expected_entry(row["guid"])
        raw="\n".join(json.dumps(r) for r in rows)
        with patch.object(current,"verify_data",return_value=self.population),patch.object(character_templates,"verify",return_value=bindings),patch.object(manage,"lab_mysql",return_value=raw):
            self.assertIs(manage.residents_preflight(Path("data"),self.settings),self.population)
        with patch.object(current,"verify_data",return_value=self.population),patch.object(character_templates,"verify",return_value=bindings[:22]),patch.object(manage,"lab_mysql",return_value="\n".join(json.dumps(r) for r in native_snapshot(self.population) if r["kind"]!="spawn" or r["guid"]<902022)):
            manage.residents_preflight(Path("data"),self.settings,bootstrap=True)
            with self.assertRaises(manage.SetupError):manage.residents_preflight(Path("data"),self.settings)

    def test_custom_defias_dbc_requires_matching_bytes_and_preserved_native_reactions(self):
        data=Path(self.tmp.name)/'runtime-data';(data/'dbc').mkdir(parents=True)
        names=b'\0Defias Brotherhood\0'
        faction=[0]*57;faction[0]=1201;faction[23]=1
        templates=[[7,15]+[0]*12,[2300,1201]+[0]*12]
        def dbc(rows,fields,strings=b'\0'):
            return struct.pack('<4s4I',b'WDBC',len(rows),fields,fields*4,len(strings))+b''.join(struct.pack(f'<{fields}I',*r) for r in rows)+strings
        (data/'dbc/Faction.dbc').write_bytes(dbc([faction],57,names))
        (data/'dbc/FactionTemplate.dbc').write_bytes(dbc(templates,14))
        proof=dict(schema_version=1,profile=current.PROFILE,source_revision=current.SOURCE_REVISION,realm_id=2,map_id=725,ok=True,
            custom_defias=dict(world_faction_id=2,faction_id=1201,faction_template_id=2300,source_template_id=7,
                faction_template_row=templates[1],faction_name='Defias Brotherhood',native_reactions_preserved=True))
        def pin():
            proof['files']=[dict(path='dbc/'+name,bytes=(data/'dbc'/name).stat().st_size,
                sha256=hashlib.sha256((data/'dbc'/name).read_bytes()).hexdigest(),fields=fields)
                for name,fields in (('Faction.dbc',57),('FactionTemplate.dbc',14))]
            path=self.metadata/current.FACTION_PROOF;write_json(path,proof)
            doc={**self.document,'faction_proof_sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
            write_json(self.metadata/current.MANIFEST,doc)
        pin();current.verify_factions(data,self.metadata)
        raw=bytearray((data/'dbc/FactionTemplate.dbc').read_bytes());raw[-2]=1
        (data/'dbc/FactionTemplate.dbc').write_bytes(raw)
        with self.assertRaises(current.ProfileError):current.verify_factions(data,self.metadata)
        templates[1][2]=1
        (data/'dbc/FactionTemplate.dbc').write_bytes(dbc(templates,14))
        proof['custom_defias']['faction_template_row']=templates[1]
        pin()
        with self.assertRaises(current.ProfileError):current.verify_factions(data,self.metadata)


if __name__=="__main__":unittest.main()
