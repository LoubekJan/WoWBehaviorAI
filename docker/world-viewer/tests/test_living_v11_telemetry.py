"""Real receiver validation for the separate 45-resident food protocol."""
from __future__ import annotations

import copy
from functools import lru_cache
import json
from pathlib import Path
import unittest

from fastapi.testclient import TestClient
from pydantic import ValidationError

from app.main import create_app
from app.telemetry import Agent, LivingRaid, LivingSupply, TelemetryBatch
from test_field_telemetry import farmer as legacy_farmer
from test_raid_telemetry import raid as legacy_raid
from test_supply_telemetry import agent as legacy_agent, supply as legacy_supply

ROOT=Path(__file__).resolve().parents[3]
METADATA=ROOT/'data/realm_lab/aiworldlab/preview-v11'


@lru_cache(maxsize=1)
def documents():
    # The agent body starts with a production C++ codec fixture. New capability
    # values use the exact emitted fields and the reviewed real map catalog.
    base=json.loads((Path(__file__).parent/'fixtures/telemetry_v2.json').read_text(encoding='utf-8'))['agents'][0]
    population=json.loads((METADATA/'residents-population.json').read_text(encoding='utf-8'))
    positions=json.loads((METADATA/'living-position-verification.json').read_text(encoding='utf-8'))
    return base,population,positions


def depot(identity):
    d=documents()[2]['depots'][identity-1]
    return dict(id=identity,name=d['name'],position=copy.deepcopy(d['position']),stock=0,
        capacity=120 if identity==5 else 30,low_threshold=12 if identity==5 else 4,
        delivered='0',consumed='0',stolen='0',exported='0')


def actor(identity):
    base,population,proof=documents()
    authored=population['actors'][identity-902000]
    row=copy.deepcopy(base)
    defias=identity>=902035
    role='FARMER' if identity<=902011 else 'RESIDENT_CARRIER' if identity in (902016,902017) else 'RAIDER' if defias else 'CONSUMER'
    warehouse=(identity-902000)//4+1 if identity<=902015 else 6 if defias else 5
    point=dict(map_id=725,**{key:authored[key] for key in ('x','y','z')})
    row.update(agent_id=identity,spawn_id=identity,entry=38 if defias else 11328,
        template_entry=9810000+identity-902000,name=f'Resident {identity}',native_creature=False,
        type='COMBATANT' if defias else 'CIVILIAN',world_faction=2 if defias else 1,
        faction_template_id=2300 if defias else 12,control_mode='AI_WORLD_CONTROLLED',
        runtime_guid=f'Creature-0-0-725-{9810000+identity-902000}-{identity}',world_state='MATERIALIZED',
        position={**point,'source':'live'},home=point,work=None,alive=True,in_combat=False,
        goal=None,action=None,routine_goal=None,group_id=None,groups=[],target=None,destination=None,llm_agent=None)
    row['living_role'].update(enabled=True,extensions_enabled=True,role='RAIDER' if defias else 'WORKER',lifetime_ms=100)
    row['supply']=dict(population_profile='living-v11',enabled=True,role=role,phase='READY',depot_id=warehouse,
        cargo=0,capacity=6 if role=='RESIDENT_CARRIER' else 2 if role in ('FARMER','RAIDER') else 0,
        carcass_remaining=None,carcass_spawn_id=None,carcass_life=None,hunting_ground=None,
        harvested='0',delivered='0',consumed='0',lost='0',stolen='0',last_result='READY',depot=depot(warehouse),
        accounting=dict(initial_food='0',harvested='0',in_depots='0',in_cargo='0',consumed='0',lost='0',conserved=True),
        caravan=None,field_id=None,field_workpoint_index=None,field_workpoint=None,field_remaining=None,field_capacity=None)
    if role=='FARMER':
        index=(identity-902000)%4
        work=proof['fields'][warehouse-1]['work_points'][index]
        row['supply'].update(field_id=warehouse,field_workpoint_index=index,
            field_workpoint=dict(map_id=725,**{key:work[key] for key in ('x','y','z')}),field_remaining=24,field_capacity=24)
    row['raid']=(dict(population_profile='living-v11',enabled=True,role='RAIDER',phase='READY',outcome='READY',
        attempt_id='0',target_spawn_id='0',noticed='0',interceptions='0',thefts='0',delivered='0',prevented='0',
        source_depot_id=0,camp_depot_id=6,source_depot=None,camp_depot=depot(6),planning=None,party=None) if defias else None)
    return row


def batch():
    return dict(version=4,captured_at_ms=1_800_000_000_000,agents=[actor(i) for i in range(902000,902045)],memory_page=None)


class LivingV11TelemetryTests(unittest.TestCase):
    def test_real_nested_batch_accepts_all45_roles_and_zero_unselected_raid_sources(self):
        parsed=TelemetryBatch.model_validate(batch())
        self.assertEqual(len(parsed.agents),45)
        self.assertEqual({r.spawn_id for r in parsed.agents},set(range(902000,902045)))
        self.assertEqual([sum(r.supply.role==role for r in parsed.agents) for role in
            ('FARMER','RESIDENT_CARRIER','CONSUMER','RAIDER')],[12,2,21,10])
        self.assertTrue(all(r.supply.population_profile=='living-v11' for r in parsed.agents))
        self.assertTrue(all(r.raid.source_depot is None and r.raid.camp_depot_id==6 for r in parsed.agents if r.raid))
        self.assertEqual(TelemetryBatch.model_validate_json(parsed.model_dump_json()),parsed)

    def test_actual_http_receiver_and_read_api_preserve_all45_and_every_raid_source_state(self):
        with TestClient(create_app(telemetry_token='test-resident-token')) as client:
            for source in (0,1,2,3,4):
                payload=batch()
                for row in payload['agents'][35:]:
                    row['raid'].update(attempt_id='1' if source else '0',source_depot_id=source,
                        source_depot=depot(source) if source else None)
                response=client.post('/internal/telemetry',json=payload,headers={'Authorization':'Bearer test-resident-token'})
                self.assertEqual(response.status_code,200,response.text)
                self.assertEqual(response.json(),{'accepted':45})
                state=client.get('/api/state').json()
                self.assertFalse(state['stale'])
                self.assertEqual(len(state['agents']),45)
                self.assertEqual(state['agents'][16]['supply']['role'],'RESIDENT_CARRIER')
                self.assertIsNone(state['agents'][16]['supply']['caravan'])
                self.assertEqual(state['agents'][35]['raid']['source_depot_id'],source)
                self.assertEqual(state['agents'][35]['raid']['camp_depot']['id'],6)

    def test_new_contract_is_bound_to_exact_native_identity_role_faction_and_own_depot(self):
        cases=(lambda r:r.update(spawn_id=901016),lambda r:r.update(spawn_id=902045),
            lambda r:r.update(agent_id=902001),lambda r:r.update(entry=9810000),lambda r:r.update(template_entry=11328),
            lambda r:r.update(type='GUARD'),lambda r:r.update(world_faction='1'),lambda r:r.update(world_faction=2),
            lambda r:r.update(runtime_guid=None),lambda r:r['position'].update(map_id=0),
            lambda r:r['supply'].pop('population_profile'),lambda r:r['supply'].update(population_profile='future'),
            lambda r:r['supply'].update(role='HUNTER'),lambda r:r['supply'].update(field_workpoint_index=1),
            lambda r:r['supply'].update(field_id=2),lambda r:r['supply'].update(depot_id=2))
        for mutate in cases:
            row=actor(902000);mutate(row)
            with self.subTest(mutate=mutate),self.assertRaises(ValidationError):Agent.model_validate(row)
        for identity in (902012,902016,902022):
            row=actor(identity);row['supply'].pop('population_profile')
            with self.subTest(identity=identity),self.assertRaises(ValidationError):Agent.model_validate(row)

    def test_resident_carriers_are_individual_transport_without_old_paired_caravan_capabilities(self):
        row=actor(902016)
        self.assertEqual(Agent.model_validate(row).supply.capacity,6)
        for changes in (dict(depot_id=7),dict(role='CARAVAN_CARRIER'),dict(capacity=7),dict(cargo=7),
            dict(harvested='1'),dict(stolen='1'),dict(field_id=1),dict(hunting_ground=dict(map_id=725,x=0,y=0,z=0))):
            bad=copy.deepcopy(row);bad['supply'].update(changes)
            with self.subTest(changes=changes),self.assertRaises(ValidationError):Agent.model_validate(bad)
        foreign=copy.deepcopy(row);foreign.update(agent_id=901040,spawn_id=901040)
        with self.assertRaises(ValidationError):Agent.model_validate(foreign)

    def test_resident_raid_source_nullability_and_camp_permissions_cannot_leak_to_legacy(self):
        for mutate in (lambda r:r['raid'].update(source_depot_id=1),
            lambda r:r['raid'].update(source_depot=depot(1)),lambda r:r['raid'].update(source_depot_id=5,source_depot=depot(5)),
            lambda r:r['raid'].update(camp_depot_id=3,camp_depot=depot(3)),
            lambda r:r['raid'].pop('population_profile'),lambda r:r['raid'].update(target_spawn_id='901039'),
            lambda r:r['raid'].update(thefts='1'),lambda r:r.update(raid=None),
            lambda r:r['raid'].update(party=dict(id='1',leader_spawn_id='902035',member_spawn_ids=['902035'],phase='OUTBOUND',role='LEADER'))):
            row=actor(902035);mutate(row)
            with self.subTest(mutate=mutate),self.assertRaises(ValidationError):Agent.model_validate(row)
        wrong=actor(902034);wrong['raid']=actor(902035)['raid']
        with self.assertRaises(ValidationError):Agent.model_validate(wrong)
        old=legacy_raid();old.update(source_depot_id=0,source_depot=None)
        with self.assertRaises(ValidationError):LivingRaid.model_validate(old)
        old=legacy_farmer()['supply'];old.update(field_id=1,depot_id=1);old['depot']['id']=1
        with self.assertRaises(ValidationError):LivingSupply.model_validate(old)

    def test_copies_and_ledger_remain_strict_with_nullable_new_raid_sources(self):
        for mutate in (lambda b:b['agents'][1]['supply']['depot'].update(stock=1),
            lambda b:b['agents'][1]['supply'].update(field_remaining=23),
            lambda b:b['agents'][1]['supply']['accounting'].update(harvested='1',in_depots='1'),
            lambda b:b['agents'][35]['raid']['camp_depot'].update(stock=1),
            lambda b:b['agents'][16]['supply'].update(delivered=1),
            lambda b:b['agents'][0]['supply'].update(cargo=True)):
            payload=batch();mutate(payload)
            with self.subTest(mutate=mutate),self.assertRaises(ValidationError):TelemetryBatch.model_validate(payload)
        payload=batch();old=legacy_agent();old['supply']=legacy_supply();payload['agents'].append(old)
        with self.assertRaises(ValidationError):TelemetryBatch.model_validate(payload)

    def test_unloaded_actor_keeps_identity_without_fabricated_live_food_observations(self):
        payload=batch();row=payload['agents'][0]
        row.update(world_state='ABSTRACT',runtime_guid=None,living_role=None,supply=None,raid=None,
            movement=None,target=None,destination=None,alive=None,in_combat=None,health=None,max_health=None,faction_template_id=None)
        row['position']['source']='spawn'
        self.assertEqual(len(TelemetryBatch.model_validate(payload).agents),45)
        row['supply']=actor(902000)['supply']
        with self.assertRaises(ValidationError):TelemetryBatch.model_validate(payload)


if __name__=='__main__':unittest.main()
