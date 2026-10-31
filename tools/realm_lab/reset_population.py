"""Explicit offline retirement of the reviewed preview-v8 NPC population.

This is an operator migration, never a startup hook. A deterministic private
before-image plan must still match immediately before its guarded transaction.
Accounts, player characters, map data, source templates and shared resources
are not deleted. Studio drafts and private authoring files are separate inputs.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

try:
    from . import character_templates as templates, full_population, manage, settlement_population as settlement
except ImportError:
    import character_templates as templates
    import full_population
    import manage
    import settlement_population as settlement

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DATA = ROOT / 'runtime/lab/data'
ACTIVE_IDS = settlement.SPAWN_IDS
LEGACY_IDS = settlement.LEGACY_IDS
REVIEWED_IDS = ACTIVE_IDS | LEGACY_IDS
MEMORY_MAX_ROWS = 200000
MEMORY_CONCAT_BYTES = 16 * 1024 * 1024
require = templates.require
ProfileError = templates.ProfileError

# Any such row is unexpected in the reviewed static population. Refuse rather
# than infer ownership of event, formation, pool, path or third-party behavior.
SIDE_REFS = (
    ('world', 'creature_addon', ('guid',), '`guid` IN ({ids})'),
    ('world', 'creature_movement_override', ('SpawnId',), '`SpawnId` IN ({ids})'),
    ('world', 'creature_formations', ('leaderGUID', 'memberGUID'), '`leaderGUID` IN ({ids}) OR `memberGUID` IN ({ids})'),
    ('world', 'game_event_creature', ('guid',), '`guid` IN ({ids})'),
    ('world', 'game_event_model_equip', ('guid',), '`guid` IN ({ids})'),
    ('world', 'game_event_npcflag', ('guid',), '`guid` IN ({ids})'),
    ('world', 'game_event_npc_vendor', ('guid',), '`guid` IN ({ids})'),
    ('world', 'vehicle_accessory', ('guid',), '`guid` IN ({ids})'),
    ('world', 'spawn_group', ('spawnType', 'spawnId'), '`spawnType`=0 AND `spawnId` IN ({ids})'),
    ('world', 'pool_members', ('type', 'spawnId'), '`type`=0 AND `spawnId` IN ({ids})'),
    ('world', 'linked_respawn', ('guid', 'linkedGuid', 'linkType'),
     '(`guid` IN ({ids}) AND `linkType` IN (0,1)) OR (`linkedGuid` IN ({ids}) AND `linkType` IN (0,3))'),
    ('world', 'smart_scripts', ('entryorguid', 'source_type'), '`source_type`=0 AND `entryorguid` IN ({negative_ids})'),
    ('world', 'smart_scripts', ('target_type', 'target_param1'), '`target_type`=10 AND `target_param1` IN ({ids})'),
    ('world', 'conditions', ('ConditionTypeOrReference', 'ConditionValue1', 'ConditionValue3'),
     '`ConditionTypeOrReference`=31 AND `ConditionValue1`=3 AND `ConditionValue3` IN ({ids})'),
    ('characters', 'ai_agent_group_members', ('member_agent_id',), '`member_agent_id` IN ({ids})'),
    ('characters', 'ai_agent_groups', ('territory_map_id',), '`territory_map_id`=725'),
)


def _query(settings, sql):
    return manage.lab_mysql(settings, sql)


def _json_rows(settings, sql):
    try:
        rows = [json.loads(line) for line in _query(settings, sql).splitlines() if line.strip()]
    except (ValueError, RecursionError) as exc:
        raise ProfileError('Invalid reset database read-back') from exc
    require(len(rows) <= 20000 and all(isinstance(row, dict) for row in rows), 'Reset read-back exceeds its bounded scope')
    return rows


def _table(database, table):
    require(database in ('world', 'characters'), 'Reset cannot target another database')
    return templates._ident(database) + '.' + templates._ident(table)


def _schema_names():
    names = {
        'world': {'creature', 'creature_template', templates.LEDGER, 'ai_spawn_participation_defaults',
                  'quest_template', 'vehicle_template_accessory', 'vehicle_accessory',
                  *(r.table for r in templates.RESOURCES)},
        'characters': {'ai_agents', 'ai_long_term_memories', 'ai_agent_groups', 'ai_agent_group_members', 'respawn'},
    }
    for database, table, _, _ in SIDE_REFS:
        names[database].add(table)
    return names


def _schema_selector(alias='c'):
    return ' OR '.join('(' + alias + '.TABLE_SCHEMA=' + templates._literal(database) + ' AND ' + alias + '.TABLE_NAME IN (' +
                      ','.join(templates._literal(name) for name in sorted(tables)) + '))'
                      for database, tables in _schema_names().items())


def _schema(settings):
    rows = _json_rows(settings, "SELECT JSON_OBJECT('database',c.TABLE_SCHEMA,'table',c.TABLE_NAME,'column',c.COLUMN_NAME,"
        "'type',c.DATA_TYPE,'extra',c.EXTRA,'engine',t.ENGINE) FROM information_schema.COLUMNS c "
        'INNER JOIN information_schema.TABLES t ON t.TABLE_SCHEMA=c.TABLE_SCHEMA AND t.TABLE_NAME=c.TABLE_NAME '
        'WHERE ' + _schema_selector() + ' ORDER BY c.TABLE_SCHEMA,c.TABLE_NAME,c.ORDINAL_POSITION;')
    result = {}
    for row in rows:
        key = row['database'] + '.' + row['table']
        _table(row['database'], row['table']); templates._ident(row['column'])
        require(row['engine'] == 'InnoDB' and 'GENERATED' not in row['extra'], 'Reset requires ordinary transactional schema columns')
        result.setdefault(key, []).append([row['column'], row['type']])
    for key in ('world.creature', 'world.creature_template', 'world.ai_spawn_participation_defaults',
                'characters.ai_agents', 'characters.ai_long_term_memories', 'characters.ai_agent_group_members',
                'characters.ai_agent_groups', 'characters.respawn'):
        require(key in result, 'Reset database schema is incomplete')
    return result


def _columns(schema, database, table, needed):
    key = database + '.' + table
    require(key in schema and set(needed) <= {column for column, _ in schema[key]}, 'Reset reference schema changed')


def _read(settings, schema, database, table, predicate):
    columns = schema[database + '.' + table]
    expression = ','.join(templates._literal(column) + ',' + templates._ident(column) for column, _ in columns)
    rows = _json_rows(settings, 'SELECT JSON_OBJECT(' + expression + ') FROM ' + _table(database, table) + ' WHERE ' + predicate + ';')
    return sorted(rows, key=lambda row: json.dumps(row, ensure_ascii=False, sort_keys=True, allow_nan=False))


def _capture(settings, schema, database, table, predicate, *, delete=False, must_be_empty=False):
    rows = _read(settings, schema, database, table, predicate)
    require(not must_be_empty or not rows, 'Unexpected dependent rows prevent reviewed NPC retirement: ' + database + '.' + table)
    return dict(database=database, table=table, predicate=predicate, rows=rows, delete=delete)


def _memory_select(schema, predicate, owners):
    """A bounded ordered SHA256-of-SHA256 stream; memory prose never leaves SQL."""
    columns = schema['characters.ai_long_term_memories']
    expression = ','.join(templates._literal(column) + ',' + templates._ident(column) for column, _ in columns)
    row_hash = 'SHA2(CAST(JSON_OBJECT(' + expression + ') AS CHAR CHARACTER SET utf8mb4),256)'
    payload = "GROUP_CONCAT(" + row_hash + " ORDER BY `memory_id` SEPARATOR '')"
    return ("SELECT JSON_OBJECT('count',COUNT(*),'sha256',SHA2(COALESCE(" + payload + ",''),256),"
            "'bytes',COALESCE(OCTET_LENGTH(" + payload + "),0),'foreign_rows',COALESCE(SUM(`agent_id` NOT IN (" +
            _ids(owners) + ')),0)) FROM characters.ai_long_term_memories WHERE ' + predicate)


def _memory_fingerprint(settings, schema, predicate, owners):
    rows = _json_rows(settings, 'SET SESSION group_concat_max_len=' + str(MEMORY_CONCAT_BYTES) + ';\n' +
                      _memory_select(schema, predicate, owners) + ';')
    require(len(rows) == 1, 'Missing bounded memory fingerprint')
    fingerprint = rows[0]
    require(set(fingerprint) == {'count','sha256','bytes','foreign_rows'} and
            all(type(fingerprint[key]) is int and fingerprint[key] >= 0 for key in ('count','bytes','foreign_rows')) and
            fingerprint['count'] <= MEMORY_MAX_ROWS and fingerprint['bytes'] == fingerprint['count'] * 64 and
            isinstance(fingerprint['sha256'],str) and len(fingerprint['sha256']) == 64 and
            all(c in '0123456789abcdef' for c in fingerprint['sha256']),
            'Memory fingerprint exceeds its limit or was truncated')
    require(fingerprint['foreign_rows'] == 0, 'Other agents own memories referring to this population; review required')
    return fingerprint


def _ids(values):
    values = tuple(sorted(values))
    require(values and all(type(value) is int for value in values), 'Reset identity set is invalid')
    return ','.join(str(value) for value in values)


def _native_schema(schema):
    return {key.split('.', 1)[1]: columns for key, columns in schema.items() if key.startswith('world.')}


def _external_template_references(settings, schema, entries):
    if not entries:
        return []
    values = _ids(entries)
    refs = []
    for table, candidates in (
        ('creature_template', ('difficulty_entry_1', 'difficulty_entry_2', 'difficulty_entry_3', 'KillCredit1', 'KillCredit2')),
        ('quest_template', ('RequiredNpcOrGo1', 'RequiredNpcOrGo2', 'RequiredNpcOrGo3', 'RequiredNpcOrGo4')),
        ('creature_summon_groups', ('entry',)),
        ('vehicle_template_accessory', ('accessory_entry',)), ('vehicle_accessory', ('accessory_entry',)),
        ('conditions', ('ConditionValue1', 'ConditionValue2', 'ConditionValue3')),
        ('smart_scripts', tuple('action_param' + str(i) for i in range(1, 7)) + tuple('target_param' + str(i) for i in range(1, 4))),
    ):
        key = 'world.' + table
        if key not in schema:
            continue
        available = {column.lower(): column for column, _ in schema[key]}
        columns = [available[name.lower()] for name in candidates if name.lower() in available]
        if not columns:
            continue
        predicate = ' OR '.join(templates._ident(column) + ' IN (' + values + ')' for column in columns)
        refs.append(_capture(settings, schema, 'world', table, predicate, must_be_empty=True))
    return refs


def _delete_table_names():
    return {'world': {'creature','creature_template',templates.LEDGER,'ai_spawn_participation_defaults',
                      *(resource.table for resource in templates.RESOURCES)},
            'characters': {'ai_agents','ai_long_term_memories','respawn'}}


def _foreign_key_predicate():
    return ' OR '.join('(REFERENCED_TABLE_SCHEMA=' + templates._literal(database) + ' AND REFERENCED_TABLE_NAME IN (' +
                      ','.join(templates._literal(table) for table in sorted(tables)) + '))'
                      for database,tables in _delete_table_names().items())


def _foreign_keys(settings):
    # Existing reviewed schemas deliberately use logical references. A new FK
    # may introduce additional cascade effects; the migration must be reviewed.
    return _json_rows(settings, "SELECT JSON_OBJECT('database',TABLE_SCHEMA,'table',TABLE_NAME,'constraint',CONSTRAINT_NAME) "
        'FROM information_schema.KEY_COLUMN_USAGE WHERE ' + _foreign_key_predicate() + ';')


def _schema_guards(schema):
    guards = ['(SELECT COUNT(*)=' + str(sum(len(columns) for columns in schema.values())) +
              ' FROM information_schema.COLUMNS c WHERE ' + _schema_selector() + ')',
              '(SELECT COUNT(*)=0 FROM information_schema.KEY_COLUMN_USAGE WHERE ' + _foreign_key_predicate() + ')']
    for key, columns in schema.items():
        database, table = key.split('.', 1)
        where = 'TABLE_SCHEMA=' + templates._literal(database) + ' AND TABLE_NAME=' + templates._literal(table)
        guards.append('(SELECT COUNT(*)=1 FROM information_schema.TABLES WHERE ' + where + " AND ENGINE='InnoDB')")
        for column, kind in columns:
            guards.append('(SELECT COUNT(*)=1 FROM information_schema.COLUMNS WHERE ' + where + ' AND COLUMN_NAME=' +
                          templates._literal(column) + ' AND DATA_TYPE=' + templates._literal(kind) + " AND EXTRA NOT LIKE '%GENERATED%')")
    return guards


def plan(settings, data: Path = DEFAULT_DATA):
    """Read-only exact before-image; generate after stopping world writes."""
    require(settings.realm_id == 2 and settings.ai_profile == full_population.PROFILE,
            'NPC retirement requires the verified full-v8 realm 2 source profile')
    manage.full_preflight(data, settings)
    bindings = templates.verify(settings)
    schema = _schema(settings)
    require(not _foreign_keys(settings), 'Unreviewed foreign keys prevent NPC retirement')
    all_ids = _ids(REVIEWED_IDS)
    records = []
    spawns = _capture(settings, schema, 'world', 'creature', '`map`=725 OR `guid` IN (' + all_ids + ')', delete=True)
    actual = {row['guid'] for row in spawns['rows']}
    require(len(actual) == len(spawns['rows']) and ACTIVE_IDS <= actual and
            actual <= REVIEWED_IDS and actual & LEGACY_IDS in (frozenset(), LEGACY_IDS),
            'Reset requires the complete current population and optional complete masked legacy population')
    require(all(row['map'] == 725 for row in spawns['rows']), 'Reserved spawn identity belongs to another map')
    scope = _ids(actual)
    records.append(spawns)
    _columns(schema, 'characters', 'ai_agents', ('agent_id', 'spawn_id', 'map_id'))
    agents = _capture(settings, schema, 'characters', 'ai_agents',
        '`map_id`=725 OR `agent_id` IN (' + all_ids + ') OR `spawn_id` IN (' + all_ids + ')', delete=True)
    require({row['agent_id'] for row in agents['rows']} == actual and all(
        row['agent_id'] == row['spawn_id'] and row['map_id'] == 725 for row in agents['rows']),
        'Reset AI identities do not match the reviewed creature population')
    records.append(agents)
    records.append(_capture(settings, schema, 'world', 'ai_spawn_participation_defaults', '`spawn_id` IN (' + all_ids + ')', delete=True))
    _columns(schema, 'characters', 'ai_long_term_memories', ('agent_id', 'actor_agent_id', 'target_agent_id', 'actor_spawn_id', 'target_spawn_id'))
    memory_predicate = ' OR '.join('`' + key + '` IN (' + all_ids + ')' for key in
                                 ('agent_id', 'actor_agent_id', 'target_agent_id', 'actor_spawn_id', 'target_spawn_id'))
    records.append(dict(database='characters',table='ai_long_term_memories',predicate=memory_predicate,
                        fingerprint=_memory_fingerprint(settings,schema,memory_predicate,actual),owners=sorted(actual),delete=True))
    records.append(_capture(settings, schema, 'characters', 'respawn', '`type`=0 AND (`mapId`=725 OR `spawnId` IN (' + all_ids + '))', delete=True))
    require(all(row['spawnId'] in actual and row['mapId'] == 725 for row in records[-1]['rows']), 'Unexpected creature respawn belongs outside the reviewed population')
    for database, table, columns, predicate in SIDE_REFS:
        if database + '.' + table in schema:
            _columns(schema, database, table, columns)
            records.append(_capture(settings, schema, database, table,
                predicate.format(ids=all_ids, negative_ids=_ids(-value for value in REVIEWED_IDS)), must_be_empty=True))
    native_schema = _native_schema(schema)
    sources = []
    entries = {spec.entry for spec in bindings}
    reserved_entries = _ids(templates.ENTRY_BASE + identity - 901000 for identity in ACTIVE_IDS)
    unowned_predicate = '`entry` IN (' + reserved_entries + ')'
    if entries:
        unowned_predicate += ' AND `entry` NOT IN (' + _ids(entries) + ')'
    records.append(_capture(settings, schema, 'world', 'creature_template', unowned_predicate, must_be_empty=True))
    for base_entry in sorted({spec.base_entry for spec in bindings}):
        bundle = templates._bundle(settings, native_schema, base_entry)
        sources.append(dict(entry=base_entry, sha256=templates._digest(bundle)))
        records.append(_capture(settings, schema, 'world', 'creature_template', '`entry`=' + str(base_entry)))
        for resource in templates.RESOURCES:
            if resource.table in native_schema:
                records.append(_capture(settings, schema, 'world', resource.table,
                    templates._ident(resource.key) + '=' + str(base_entry) + ' AND (' + resource.predicate + ')'))
    for spec in bindings:
        require(spec.spawn_id in actual and spec.entry not in {s.base_entry for s in bindings}, 'Invalid dedicated template ownership')
        records.append(_capture(settings, schema, 'world', 'creature', '`id`=' + str(spec.entry) + ' AND `guid` NOT IN (' + scope + ')', must_be_empty=True))
        records.append(_capture(settings, schema, 'world', 'creature_template', '`entry`=' + str(spec.entry), delete=True))
        for resource in templates.RESOURCES:
            if resource.table in native_schema:
                records.append(_capture(settings, schema, 'world', resource.table,
                    templates._ident(resource.key) + '=' + str(spec.entry) + ' AND (' + resource.predicate + ')', delete=True))
    records.extend(_external_template_references(settings, schema, entries))
    if 'world.' + templates.LEDGER in schema:
        records.append(_capture(settings, schema, 'world', templates.LEDGER, '1', delete=True))
    body = dict(schema_version=1, operation='retire_preview_v8_npcs', realm_id=2, map_id=725,
                source_profile=full_population.PROFILE, spawn_ids=sorted(actual),
                legacy_spawn_ids=sorted(actual & LEGACY_IDS), template_entries=sorted(entries),
                sources=sources, schema=schema, records=records)
    return {**body, 'sha256': templates._digest(body)}


def _validate_plan(expected):
    require(isinstance(expected, dict) and isinstance(expected.get('sha256'), str), 'Missing reviewed reset plan')
    body = {key: value for key, value in expected.items() if key != 'sha256'}
    require(templates._digest(body) == expected['sha256'], 'Reset plan before-image hash changed')


def _record_guard(record, schema):
    if 'fingerprint' in record:
        expected = templates._literal(json.dumps(record['fingerprint'],sort_keys=True,separators=(',',':')))
        return 'JSON_CONTAINS((' + _memory_select(schema, record['predicate'], record['owners']) + '),CAST(' + expected + ' AS JSON))'
    table = record['table']
    guard = templates._guard(table, record['predicate'], record['rows'], schema[record['database'] + '.' + table])
    return guard.replace('FROM world.', 'FROM ' + templates._ident(record['database']) + '.')


def apply(settings, expected_plan, data: Path = DEFAULT_DATA):
    """Explicit offline apply; a stale/tampered plan cannot broaden the scope."""
    settlement.ensure_world_offline()
    _validate_plan(expected_plan)
    current = plan(settings, data)
    require(current['sha256'] == expected_plan['sha256'], 'NPC reset plan is stale; regenerate its offline before-image')
    settlement.ensure_world_offline()
    guards = _schema_guards(current['schema']) + [_record_guard(record, current['schema']) for record in current['records']]
    # Re-check optional reference absence and complete rows within one serializable
    # transaction. No DDL or external file changes happen in this transaction.
    sql = 'SET SESSION group_concat_max_len=' + str(MEMORY_CONCAT_BYTES) + ';\nSET TRANSACTION ISOLATION LEVEL SERIALIZABLE;\nSTART TRANSACTION;\n'
    sql += 'SET @population_reset_ready=(' + ' AND '.join(guards) + ');\n'
    for record in reversed(current['records']):
        if record['delete']:
            sql += 'DELETE FROM ' + _table(record['database'], record['table']) + ' WHERE (' + record['predicate'] + ') AND @population_reset_ready=1;\n'
    sql += 'COMMIT;\nSELECT @population_reset_ready;'
    require(_query(settings, sql).strip() == '1', 'NPC reset guard refused changed database state')
    verify_empty(settings)
    native = templates._schema(settings)
    require(not templates.verify(settings), 'Installed native template ownership remains after reset')
    for source in current['sources']:
        require(templates._digest(templates._bundle(settings, native, source['entry'])) == source['sha256'], 'Shared source template changed during reset')
    return dict(schema_version=1, operation='retire_preview_v8_npcs', status='complete', before_sha256=current['sha256'],
                removed_spawns=len(current['spawn_ids']), removed_legacy_spawns=len(current['legacy_spawn_ids']),
                removed_native_templates=len(current['template_entries']), source_templates_preserved=True)


def verify_empty(settings):
    """Read-only bounded postcondition usable by the empty authoring profile."""
    ids = _ids(REVIEWED_IDS)
    sql = "SELECT JSON_OBJECT('spawns',(SELECT COUNT(*) FROM world.creature WHERE map=725 OR guid IN (" + ids + ')),\n'
    sql += "'agents',(SELECT COUNT(*) FROM characters.ai_agents WHERE map_id=725 OR agent_id IN (" + ids + ') OR spawn_id IN (' + ids + ')),\n'
    sql += "'participation',(SELECT COUNT(*) FROM world.ai_spawn_participation_defaults WHERE spawn_id IN (" + ids + ')));'
    rows = _json_rows(settings, sql)
    require(rows == [dict(spawns=0, agents=0, participation=0)], 'Retired population or AI bindings remain in the empty authoring world')
    memory_predicate = ' OR '.join('`' + key + '` IN (' + ids + ')' for key in
        ('agent_id', 'actor_agent_id', 'target_agent_id', 'actor_spawn_id', 'target_spawn_id'))
    remainder = _json_rows(settings, "SELECT JSON_OBJECT('memories',(SELECT COUNT(*) FROM characters.ai_long_term_memories WHERE " + memory_predicate +
        "),'memberships',(SELECT COUNT(*) FROM characters.ai_agent_group_members WHERE member_agent_id IN (" + ids +
        ")),'groups',(SELECT COUNT(*) FROM characters.ai_agent_groups WHERE territory_map_id=725)," +
        "'respawns',(SELECT COUNT(*) FROM characters.respawn WHERE type=0 AND (mapId=725 OR spawnId IN (" + ids + '))));')
    require(remainder == [dict(memories=0,memberships=0,groups=0,respawns=0)], 'Retired NPC dependent state remains in the empty authoring world')
    return rows[0]


def summary(document):
    return dict(operation=document['operation'], sha256=document['sha256'], spawns=len(document['spawn_ids']),
                masked_legacy_spawns=len(document['legacy_spawn_ids']), native_templates=len(document['template_entries']))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('plan', 'apply', 'verify-empty'))
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--expected-plan', type=Path)
    parser.add_argument('--confirm-delete-reviewed-npcs', action='store_true')
    args = parser.parse_args(argv)
    try:
        settings = manage.Settings.load(os.environ)
        if args.command == 'plan':
            require(args.output is not None, 'A private before-image output path is required')
            document = plan(settings, args.data)
            descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
                json.dump(document, stream, ensure_ascii=False, indent=2, allow_nan=False)
                stream.write('\n')
            result = summary(document)
        elif args.command == 'apply':
            require(args.confirm_delete_reviewed_npcs and args.expected_plan is not None,
                    'Apply requires the reviewed private plan and explicit deletion confirmation')
            result = apply(settings, json.loads(args.expected_plan.read_text(encoding='utf-8')), args.data)
        else:
            result = verify_empty(settings)
        print(json.dumps(result, ensure_ascii=True))
    except (ProfileError, manage.SetupError, OSError, ValueError) as exc:
        print('Population reset refused: ' + (str(exc) if isinstance(exc, (ProfileError, manage.SetupError)) else 'invalid private plan or database access'))
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
