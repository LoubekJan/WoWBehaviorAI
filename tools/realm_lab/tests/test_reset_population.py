"""Retirement cannot widen beyond the reviewed source population or stale plan."""
import copy
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.realm_lab import character_templates as templates, reset_population as reset
from tools.realm_lab.tests.test_character_templates import document


class ResetPopulationTests(unittest.TestCase):
    def setUp(self):
        self.settings = SimpleNamespace(realm_id=2, ai_profile='full-v8')
        self.spec = templates.spec_from_document(document())
        self.ids = set(reset.ACTIVE_IDS | reset.LEGACY_IDS)
        self.spawns = [dict(guid=i, id=self.spec.entry if i == self.spec.spawn_id else 11328, map=725,
                            StringId='test', position_x=1.14286) for i in sorted(self.ids)]
        self.agents = [dict(agent_id=i, spawn_id=i, map_id=725, control_mode=1 if i in reset.ACTIVE_IDS else 0)
                       for i in sorted(self.ids)]
        self.sources = dict(template=dict(entry=11328, name='Eastvale Peasant', subname=None), children={})
        self.schema = {
            'world.creature': [('guid','int'),('id','int'),('map','smallint'),('StringId','varchar'),('position_x','float')],
            'world.creature_template': [('entry','int'),('name','varchar'),('subname','varchar')],
            'world.ai_spawn_participation_defaults': [('spawn_id','bigint'),('participation_mode','tinyint')],
            'world.'+templates.LEDGER: [('spawn_id','bigint'),('entry','int')],
            'world.creature_addon': [('guid','int'),('path_id','int')],
            'world.npc_vendor': [('entry','int'),('item','int')],
            'characters.ai_agents': [('agent_id','bigint'),('spawn_id','bigint'),('map_id','int'),('control_mode','tinyint')],
            'characters.ai_long_term_memories': [(c,'bigint') for c in
                ('memory_id','agent_id','actor_agent_id','target_agent_id','actor_spawn_id','target_spawn_id')],
            'characters.ai_agent_groups': [('group_id','bigint'),('territory_map_id','int')],
            'characters.ai_agent_group_members': [('group_id','bigint'),('member_agent_id','bigint')],
            'characters.respawn': [('type','smallint'),('spawnId','int'),('mapId','smallint'),('instanceId','int')],
        }
        self.side = []
        self.memories = [dict(memory_id=3,agent_id=901014,actor_agent_id=901035,target_agent_id=0,actor_spawn_id=901035,target_spawn_id=0)]
        self.preflight = patch.object(reset.manage,'full_preflight',return_value=object()).start()
        self.native_verify = patch.object(templates,'verify',return_value=(self.spec,)).start()
        patch.object(reset,'_schema',side_effect=lambda _:copy.deepcopy(self.schema)).start()
        patch.object(reset,'_foreign_keys',return_value=[]).start()
        patch.object(reset,'_read',side_effect=self.read).start()
        patch.object(reset,'_memory_fingerprint',side_effect=self.memory_fingerprint).start()
        patch.object(templates,'_bundle',return_value=self.sources).start()
        self.query = patch.object(reset,'_query',side_effect=AssertionError('Plan must never mutate/query outside read helpers')).start()
        self.addCleanup(patch.stopall)

    def memory_fingerprint(self, settings, schema, predicate, owners):
        reset.require(all(row['agent_id'] in owners for row in self.memories), 'Other agents own memories referring to this population; review required')
        return dict(count=len(self.memories),sha256=templates._digest(self.memories),bytes=len(self.memories)*64,foreign_rows=0)

    def read(self, settings, schema, database, table, predicate):
        if table == 'creature':
            return copy.deepcopy(self.spawns) if predicate.startswith('`map`=725') else []
        if table == 'ai_agents': return copy.deepcopy(self.agents)
        if table == 'ai_spawn_participation_defaults':
            return [dict(spawn_id=i,participation_mode=0) for i in sorted(self.ids)]
        if table == 'ai_long_term_memories': return copy.deepcopy(self.memories)
        if table == 'respawn': return [dict(type=0,spawnId=901014,mapId=725,instanceId=0)]
        if table == 'creature_addon': return copy.deepcopy(self.side)
        if table == templates.LEDGER: return [dict(spawn_id=901014,entry=9800014)]
        if table == 'creature_template':
            if not predicate.startswith('`entry`='):return []
            entry=int(predicate.removeprefix('`entry`='))
            return [dict(entry=entry,name='Šaša' if entry==9800014 else 'Eastvale Peasant',subname=None)]
        if table == 'npc_vendor':
            entry=int(predicate.split('=',1)[1].split()[0])
            return [dict(entry=entry,item=123)]
        return []

    def test_plan_is_read_only_complete_and_deterministic_with_masked_legacy(self):
        first=reset.plan(self.settings,Path('/data'))
        self.assertEqual(first,reset.plan(self.settings,Path('/data')))
        self.assertEqual(reset.summary(first)['spawns'],200)
        self.assertEqual(first['legacy_spawn_ids'],sorted(reset.LEGACY_IDS))
        self.assertEqual(first['template_entries'],[9800014])
        source_records=[r for r in first['records'] if r['table']=='creature_template' and r['predicate']=='`entry`=11328']
        self.assertEqual(len(source_records),1)
        self.assertFalse(source_records[0]['delete'])
        self.assertFalse(any(r['database']=='auth' or r['table']=='characters' for r in first['records']))
        self.query.assert_not_called()

    def test_absent_legacy_is_supported_but_partial_or_foreign_population_is_refused(self):
        self.ids=set(reset.ACTIVE_IDS)
        self.spawns=[r for r in self.spawns if r['guid'] in self.ids]
        self.agents=[r for r in self.agents if r['agent_id'] in self.ids]
        self.assertEqual(len(reset.plan(self.settings)['spawn_ids']),100)
        self.spawns.append(dict(guid=900725,id=1186,map=725))
        with self.assertRaisesRegex(reset.ProfileError,'complete current population'):
            reset.plan(self.settings)
        self.spawns[-1]['guid']=123456
        with self.assertRaises(reset.ProfileError): reset.plan(self.settings)
        self.query.assert_not_called()

    def test_identity_collision_and_cross_owner_memory_and_side_references_fail_closed(self):
        self.agents[0]['map_id']=0
        with self.assertRaisesRegex(reset.ProfileError,'AI identities'):reset.plan(self.settings)
        self.agents[0]['map_id']=725
        self.memories[0]['agent_id']=11
        with self.assertRaisesRegex(reset.ProfileError,'Other agents own memories'):reset.plan(self.settings)
        self.memories[0]['agent_id']=901014
        self.side=[dict(guid=901014,path_id=333)]
        with self.assertRaisesRegex(reset.ProfileError,'dependent rows'):reset.plan(self.settings)
        self.query.assert_not_called()

    def test_source_profile_realm_and_new_fk_must_be_reviewed(self):
        for settings in (SimpleNamespace(realm_id=1,ai_profile='full-v8'),SimpleNamespace(realm_id=2,ai_profile='settlement-v8')):
            with self.assertRaisesRegex(reset.ProfileError,'source profile'):reset.plan(settings)
        with patch.object(reset,'_foreign_keys',return_value=[{'table':'unknown'}]):
            with self.assertRaisesRegex(reset.ProfileError,'foreign keys'):reset.plan(self.settings)
        self.query.assert_not_called()

    def test_spawn_vendor_vehicle_smart_target_and_creature_condition_references_are_refused(self):
        references = (
            ('game_event_npc_vendor', [('guid','int')]),
            ('vehicle_accessory', [('guid','int')]),
            ('smart_scripts', [('entryorguid','bigint'),('source_type','int'),('target_type','int'),('target_param1','int')]),
            ('conditions', [('ConditionTypeOrReference','int'),('ConditionValue1','int'),('ConditionValue3','int')]),
        )
        for table,columns in references:
            with self.subTest(table=table):
                self.schema['world.'+table]=columns
                def read(settings,schema,database,selected,predicate):
                    if selected==table:return [{columns[0][0]:901014}]
                    return self.read(settings,schema,database,selected,predicate)
                with patch.object(reset,'_read',side_effect=read):
                    with self.assertRaisesRegex(reset.ProfileError,'dependent rows'):reset.plan(self.settings)
                del self.schema['world.'+table]
        self.query.assert_not_called()

    def test_all_deleted_tables_are_covered_by_foreign_key_guard(self):
        expected=reset.plan(self.settings)
        targets=reset._delete_table_names()
        self.assertTrue(all(record['table'] in targets[record['database']] for record in expected['records'] if record['delete']))
        for name in ('ai_long_term_memories','respawn'):
            self.assertIn(templates._literal(name),reset._foreign_key_predicate())
        for resource in templates.RESOURCES:
            self.assertIn(templates._literal(resource.table),reset._foreign_key_predicate())

    def test_tampered_and_stale_plans_cannot_execute_deletes(self):
        expected=reset.plan(self.settings)
        with patch.object(reset.settlement,'ensure_world_offline'):
            tampered=copy.deepcopy(expected);tampered['spawn_ids'].append(11)
            with self.assertRaisesRegex(reset.ProfileError,'hash changed'):reset.apply(self.settings,tampered)
            self.spawns[0]['position_x']=2
            with self.assertRaisesRegex(reset.ProfileError,'stale'):reset.apply(self.settings,expected)
        self.query.assert_not_called()

    def test_apply_rechecks_complete_rows_once_and_only_deletes_owned_resources(self):
        expected=reset.plan(self.settings)
        self.query.side_effect=None;self.query.return_value='1'
        with patch.object(reset.settlement,'ensure_world_offline') as offline, \
             patch.object(reset,'verify_empty',return_value={'spawns':0}), \
             patch.object(templates,'verify',return_value=()), \
             patch.object(templates,'_schema',return_value={}):
            with patch.object(reset,'plan',return_value=expected):result=reset.apply(self.settings,expected)
        self.assertEqual(result['removed_spawns'],200)
        self.assertEqual(result['removed_native_templates'],1)
        self.assertEqual(offline.call_count,2)
        self.assertEqual(self.query.call_count,1)
        sql=self.query.call_args.args[1]
        self.assertIn('ISOLATION LEVEL SERIALIZABLE',sql)
        self.assertIn('FROM `characters`.`ai_agents`',sql)
        self.assertIn('CAST(1.14286 AS FLOAT)',sql)
        self.assertIn('DELETE FROM `world`.`creature_template` WHERE (`entry`=9800014)',sql)
        self.assertIn('DELETE FROM `world`.`npc_vendor` WHERE (`entry`=9800014',sql)
        self.assertNotIn('DELETE FROM `world`.`creature_template` WHERE (`entry`=11328)',sql)
        self.assertNotIn('DELETE FROM `world`.`npc_vendor` WHERE (`entry`=11328',sql)
        self.assertNotIn('DELETE FROM `characters`.`characters`',sql)
        self.assertNotIn('DROP ',sql)
        self.assertIn('JSON_CONTAINS((SELECT JSON_OBJECT',sql)
        self.assertIn('group_concat_max_len=16777216',sql)
        self.assertTrue(all('@population_reset_ready=1' in line for line in sql.splitlines() if line.startswith('DELETE')))

    def test_transaction_guard_failure_does_not_report_success(self):
        expected=reset.plan(self.settings)
        self.query.side_effect=None;self.query.return_value='0'
        with patch.object(reset.settlement,'ensure_world_offline'),patch.object(reset,'plan',return_value=expected), \
             patch.object(reset,'verify_empty') as verify:
            with self.assertRaisesRegex(reset.ProfileError,'guard refused'):reset.apply(self.settings,expected)
        verify.assert_not_called()


class MemoryFingerprintTests(unittest.TestCase):
    def test_truncated_oversized_or_foreign_memory_digest_is_refused(self):
        valid=dict(count=32448,sha256='a'*64,bytes=32448*64,foreign_rows=0)
        schema={'characters.ai_long_term_memories':[('memory_id','bigint'),('agent_id','bigint')]}
        for values in (dict(bytes=100),dict(count=reset.MEMORY_MAX_ROWS+1),dict(foreign_rows=1),dict(sha256='bad')):
            with self.subTest(values=values),patch.object(reset,'_json_rows',return_value=[{**valid,**values}]):
                with self.assertRaises(reset.ProfileError):reset._memory_fingerprint(object(),schema,'1',{901014})
        with patch.object(reset,'_json_rows',return_value=[valid]) as query:
            self.assertEqual(reset._memory_fingerprint(object(),schema,'1',{901014}),valid)
        self.assertIn('ORDER BY `memory_id`',query.call_args.args[1])


if __name__=='__main__':unittest.main()
