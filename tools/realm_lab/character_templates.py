"""Offline, owned creature-template identities for authored character cards.

Every installed entry is a complete database clone of its reviewed native
template. Only its entry, name and subtitle change. Runtime verification reads
the installed ledger, never the mutable authoring files. No shared template,
home, agent state, inventory, memory or account row is changed here.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict, dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re

try:
    from . import full_population, settlement_population as settlement, single_return as base
except ImportError:
    import full_population
    import settlement_population as settlement
    import single_return as base

ROOT = Path(__file__).resolve().parents[2]
DIRECTORY = ROOT / 'data/npc_templates'
PROFILES = ROOT / 'data/npc_agents'
LEDGER = 'ai_character_templates'
ENTRY_BASE = 9800000
MAX_FILES = 100
LEDGER_FIELDS = ('spawn_id', 'entry', 'base_entry', 'map_id', 'name', 'title',
                 'source_sha256', 'clone_sha256', 'children_sha256')
SPEC_FIELDS = frozenset(('schema_version', 'spawn_id', 'entry', 'base_entry', 'map_id', 'name', 'title'))
ProfileError, require = base.ProfileError, base.require


@dataclass(frozen=True)
class TemplateSpec:
    spawn_id: int
    entry: int
    base_entry: int
    map_id: int
    name: str
    title: str = ''

    def document(self):
        return {'schema_version': 1, **asdict(self)}


@dataclass(frozen=True)
class Resource:
    table: str
    key: str
    predicate: str = '1'
    label: str = ''

    @property
    def identity(self): return self.label or self.table


# Shared referenced IDs (loot, gossip menus, trainers, waypoints, action lists)
# remain shared immutable resources. These tables instead use creature entry
# as the lookup key and therefore need independent copies.
RESOURCES = (
    Resource('creature_equip_template', 'CreatureID'),
    Resource('creature_template_addon', 'entry'),
    Resource('creature_template_movement', 'CreatureId'),
    Resource('creature_template_resistance', 'CreatureID'),
    Resource('creature_template_spell', 'CreatureID'),
    Resource('creature_template_locale', 'entry'),
    Resource('creature_default_trainer', 'CreatureId'),
    Resource('npc_vendor', 'entry'),
    Resource('npc_spellclick_spells', 'npc_entry'),
    Resource('creature_queststarter', 'id'),
    Resource('creature_questender', 'id'),
    Resource('creature_questitem', 'CreatureEntry'),
    Resource('creature_onkill_reputation', 'creature_id'),
    Resource('creature_text', 'CreatureID'),
    Resource('creature_text_locale', 'CreatureID'),
    Resource('quest_greeting', 'ID', '`Type`=0'),
    Resource('quest_greeting_locale', 'ID', '`Type`=0'),
    Resource('creature_summon_groups', 'summonerId', '`summonerType`=0'),
    Resource('smart_scripts', 'entryorguid', '`source_type`=0'),
    Resource('conditions', 'SourceGroup', '`SourceTypeOrReferenceId` IN (18,21,23)', 'entry_conditions_group'),
    Resource('conditions', 'SourceEntry', '`SourceTypeOrReferenceId`=16 OR (`SourceTypeOrReferenceId`=22 AND `SourceId`=0)', 'entry_conditions_entry'),
)


def strict_document(path: Path, maximum: int):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= maximum,
            'Character template authoring file is absent, linked or too large')
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate character template JSON field')
            result[key] = value
        return result
    try:
        return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=pairs,
                          parse_constant=lambda _value: (_ for _ in ()).throw(ValueError('constant')))
    except (OSError, UnicodeError, ValueError, RecursionError) as exc:
        raise ProfileError('Invalid character template authoring JSON') from exc


def plain(value, maximum, *, required=False, characters=None):
    require(isinstance(value, str) and len(value.encode('utf-8')) <= maximum and
            (not characters or len(value) <= characters) and (not required or bool(value.strip())) and
            all(ord(c) >= 32 and not 127 <= ord(c) <= 159 and c != '|' for c in value),
            'Invalid native character name or subtitle')
    return value


def spec_from_document(document: dict, population=None) -> TemplateSpec:
    population = population or full_population.Population.load()
    require(isinstance(document, dict) and set(document) == SPEC_FIELDS and
            type(document['schema_version']) is int and document['schema_version'] == 1,
            'Unsupported character template schema')
    require(all(type(document[key]) is int for key in ('spawn_id', 'entry', 'base_entry', 'map_id')),
            'Character template identities must be integer JSON numbers')
    actors = {actor.spawn_id: actor for actor in population.settlement.actors}
    spawn = document['spawn_id']
    require(spawn in full_population.SPAWN_IDS and spawn in actors and
            document['entry'] == ENTRY_BASE + spawn - 901000 and document['map_id'] == 725 and
            document['base_entry'] == actors[spawn].entry,
            'Character template does not match the reviewed native spawn')
    return TemplateSpec(spawn, document['entry'], document['base_entry'], 725,
        plain(document['name'], 80, required=True, characters=32), plain(document['title'], 80))


def load_descriptors(directory: Path = DIRECTORY, profiles_dir: Path = PROFILES, population=None) -> tuple[TemplateSpec, ...]:
    if not directory.exists(): return ()
    require(directory.is_dir() and not directory.is_symlink(), 'Invalid character template directory')
    files = sorted(directory.glob('*.json'))
    require(len(files) <= MAX_FILES, 'Character template catalog exceeds the reviewed population')
    population = population or full_population.Population.load()
    result, seen = [], set()
    for path in files:
        document = strict_document(path, 4096)
        spec = spec_from_document(document, population)
        require(path.name == f'{spec.spawn_id}.json' and spec.spawn_id not in seen,
                'Character template filename/identity is not canonical')
        card = strict_document(profiles_dir / path.name, 16384)
        require(isinstance(card, dict) and set(card) == {'agent_id', 'spawn_id', 'map_id', 'name', 'backstory', 'personality'} and
                all(type(card[key]) is int for key in ('agent_id', 'spawn_id', 'map_id')) and
                (card['agent_id'], card['spawn_id'], card['map_id'], card['name']) ==
                (spec.spawn_id, spec.spawn_id, spec.map_id, spec.name),
                'Character template and LLM card identity/name differ')
        for key, maximum in (('backstory', 8192), ('personality', 2048)):
            require(isinstance(card[key], str) and bool(card[key].strip()) and len(card[key].encode('utf-8')) <= maximum,
                    'Character template requires a complete native character card')
        result.append(spec); seen.add(spec.spawn_id)
    return tuple(result)


def _query(settings, sql):
    try:
        from .manage import lab_mysql
    except ImportError:
        from manage import lab_mysql
    return lab_mysql(settings, sql)


def _json_rows(settings, sql):
    result = _query(settings, sql)
    try:
        rows = [json.loads(line) for line in result.splitlines() if line.strip()]
    except (ValueError, RecursionError) as exc:
        raise ProfileError('Invalid character template database read-back') from exc
    require(all(isinstance(row, dict) for row in rows) and len(rows) <= 20000,
            'Character template database read-back is unbounded')
    return rows


def _ident(value):
    require(isinstance(value, str) and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', value), 'Unexpected character schema identifier')
    return '`' + value + '`'


def _literal(value):
    if value is None: return 'NULL'
    if type(value) is int: return str(value)
    if type(value) is float:
        require(math.isfinite(value), 'Nonfinite character database value')
        return repr(value)
    require(isinstance(value, str), 'Unsupported character database value')
    return "CONVERT(X'" + value.encode('utf-8').hex() + "' USING utf8mb4)"


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode()).hexdigest()


def _schema(settings):
    names = {'creature_template', 'creature', LEDGER, *(r.table for r in RESOURCES)}
    rows = _json_rows(settings, "SELECT JSON_OBJECT('table',c.TABLE_NAME,'column',c.COLUMN_NAME,'type',c.DATA_TYPE,'extra',c.EXTRA,'engine',t.ENGINE) "
        'FROM information_schema.COLUMNS c INNER JOIN information_schema.TABLES t ON t.TABLE_SCHEMA=c.TABLE_SCHEMA AND t.TABLE_NAME=c.TABLE_NAME '
        "WHERE c.TABLE_SCHEMA='world' AND c.TABLE_NAME IN (" + ','.join(_literal(n) for n in sorted(names)) + ') ORDER BY c.TABLE_NAME,c.ORDINAL_POSITION;')
    result = {}
    for row in rows:
        _ident(row['table']); _ident(row['column'])
        require(row['engine'] == 'InnoDB' and 'GENERATED' not in row['extra'],
                'Character identity mutation requires ordinary transactional schema columns')
        result.setdefault(row['table'], []).append((row['column'], row['type']))
    require('creature_template' in result and 'creature' in result and
            {'entry', 'name', 'subname'} <= {c for c, _ in result['creature_template']},
            'Native creature schema is unavailable')
    for resource in RESOURCES:
        if resource.table in result:
            require(resource.key in {c for c, _ in result[resource.table]}, 'Native child resource schema changed')
    if LEDGER in result:
        require({c for c, _ in result[LEDGER]} == set(LEDGER_FIELDS), 'Character template ownership ledger schema changed')
    return result


def _rows(settings, schema, table, predicate):
    columns = schema[table]
    expression = ','.join(_literal(column) + ',' + _ident(column) for column, _ in columns)
    return _json_rows(settings, f'SELECT JSON_OBJECT({expression}) FROM world.{_ident(table)} WHERE {predicate};')


def _bundle(settings, schema, entry):
    selected = [('template', 'creature_template', f'`entry`={entry}')]
    for resource in RESOURCES:
        if resource.table in schema:
            selected.append((resource.identity, resource.table,
                f'{_ident(resource.key)}={entry} AND ({resource.predicate})'))
    statements = []
    for identity, table, predicate in selected:
        expression = ','.join(_literal(column) + ',' + _ident(column) for column, _ in schema[table])
        statements.append(f"SELECT JSON_OBJECT('resource',{_literal(identity)},'data',JSON_OBJECT({expression})) "
                          f'FROM world.{_ident(table)} WHERE {predicate};')
    grouped = {identity: [] for identity, _, _ in selected}
    for row in _json_rows(settings, '\n'.join(statements)):
        require(set(row) == {'resource', 'data'} and row['resource'] in grouped and isinstance(row['data'], dict),
                'Invalid native character bundle read-back')
        grouped[row['resource']].append(row['data'])
    require(len(grouped['template']) == 1, 'A native or dedicated creature template is missing/duplicated')
    require(all(len(rows) <= 4096 for rows in grouped.values()), 'Native character child resource is unbounded')
    return {'template': grouped.pop('template')[0],
            'children': {key: sorted(rows, key=lambda r: json.dumps(r, sort_keys=True)) for key, rows in grouped.items()}}


def cloned_bundle(source, spec):
    template = {**source['template'], 'entry': spec.entry, 'name': spec.name, 'subname': spec.title}
    children = {}
    for resource in RESOURCES:
        if resource.identity not in source['children']: continue
        rows = []
        for row in source['children'][resource.identity]:
            copy = {**row, resource.key: spec.entry}
            if resource.table == 'creature_template_locale':
                copy.update(Name=spec.name, Title=spec.title)
            rows.append(copy)
        children[resource.identity] = sorted(rows, key=lambda r: json.dumps(r, sort_keys=True))
    return {'template': template, 'children': children}


def _ledger(settings, schema):
    if LEDGER not in schema: return []
    rows = _rows(settings, schema, LEDGER, '1')
    require(len(rows) <= MAX_FILES, 'Character identity ledger exceeds the reviewed population')
    population = full_population.Population.load()
    seen = set()
    for row in rows:
        spec = spec_from_document({'schema_version': 1, **{k: row[k] for k in ('spawn_id', 'entry', 'base_entry', 'map_id', 'name', 'title')}}, population)
        require(spec.spawn_id not in seen and all(isinstance(row[k], str) and re.fullmatch('[a-f0-9]{64}', row[k]) for k in
                ('source_sha256', 'clone_sha256', 'children_sha256')), 'Invalid installed character ownership ledger')
        seen.add(spec.spawn_id)
    return sorted(rows, key=lambda r: r['spawn_id'])


def _spec(row): return TemplateSpec(**{key: row[key] for key in ('spawn_id', 'entry', 'base_entry', 'map_id', 'name', 'title')})


def _spawn(settings, schema, spec, *, allow_restored):
    rows = _rows(settings, schema, 'creature', f'`guid`={spec.spawn_id}')
    require(len(rows) == 1, 'Authored character spawn is missing/duplicated')
    row = rows[0]
    actor = next(a for a in full_population.Population.load().settlement.actors if a.spawn_id == spec.spawn_id)
    allowed = (spec.base_entry, spec.entry) if allow_restored else (spec.entry,)
    require(row['id'] in allowed and row['map'] == 725 and row['StringId'] == actor.label and
            all(base.near(row[key], expected) for key, expected in
                (('position_x', actor.x), ('position_y', actor.y), ('position_z', actor.z), ('orientation', actor.orientation))),
            'Character spawn binding or immutable authored home changed')
    others = _rows(settings, schema, 'creature', f'`id`={spec.entry} AND `guid`<>{spec.spawn_id}')
    require(not others, 'Dedicated character template is used by another spawn')
    return row


def _verified(settings, schema, *, allow_restored):
    installed = _ledger(settings, schema)
    sources, targets, spawns = {}, {}, {}
    for row in installed:
        spec = _spec(row)
        source = _bundle(settings, schema, spec.base_entry)
        actual = _bundle(settings, schema, spec.entry)
        expected = cloned_bundle(source, spec)
        require(_digest(source) == row['source_sha256'] and actual == expected and
                _digest(actual) == row['clone_sha256'] and _digest(actual['children']) == row['children_sha256'],
                'Installed character template or its source/child resources changed')
        spawns[spec.spawn_id] = _spawn(settings, schema, spec, allow_restored=allow_restored)
        sources[spec.spawn_id], targets[spec.spawn_id] = source, actual
    return installed, sources, targets, spawns


def verify(settings, specs=None, *, allow_restored=False) -> tuple[TemplateSpec, ...]:
    """Verify installed state independently from currently editable authoring files."""
    schema = _schema(settings)
    installed, _, _, _ = _verified(settings, schema, allow_restored=allow_restored)
    if installed:
        require(settings.ai_profile == full_population.PROFILE, 'Installed character identities require full-v8')
    bindings = tuple(_spec(row) for row in installed)
    if specs is not None:
        current = {s.spawn_id: s for s in bindings}
        require(all(current.get(s.spawn_id) == s for s in specs), 'Desired character identities have not been installed')
    return bindings


def normalize_snapshot(rows, bindings):
    by_spawn = {s.spawn_id: s for s in bindings}
    result = []
    for row in rows:
        if row.get('kind') == 'spawn' and row.get('guid') in by_spawn:
            spec = by_spawn[row['guid']]
            require(row.get('entry') in (spec.base_entry, spec.entry), 'Character read-back entry does not match verified ownership')
            row = {**row, 'entry': spec.base_entry}
        result.append(row)
    return result


def _row_match(row, types=None):
    terms = []
    for column, value in row.items():
        expression = _ident(column)
        literal = _literal(value)
        if type(value) is float and (types or {}).get(column) == 'float':
            # JSON uses the shortest representation which round-trips to a
            # native FLOAT. Round that decimal back to FLOAT before MySQL
            # promotes both sides to DOUBLE for comparison.
            literal = 'CAST(' + literal + ' AS FLOAT)'
        if isinstance(value, str): expression, literal = 'BINARY ' + expression, 'BINARY ' + literal
        terms.append(f'({expression} <=> {literal})')
    return ' AND '.join(terms)


def _guard(table, predicate, rows, columns=()):
    clauses = [f'(SELECT COUNT(*)={len(rows)} FROM world.{_ident(table)} WHERE {predicate})']
    encoded = Counter(json.dumps(row, sort_keys=True) for row in rows)
    for encoded_row, count in encoded.items():
        clauses.append(f'(SELECT COUNT(*)={count} FROM world.{_ident(table)} WHERE ({predicate}) AND {_row_match(json.loads(encoded_row), dict(columns))})')
    return ' AND '.join(clauses)


def _bundle_guard(schema, spec, bundle, entry):
    parts = [_guard('creature_template', f'`entry`={entry}', [bundle['template']], schema['creature_template'])]
    for resource in RESOURCES:
        if resource.table in schema:
            parts.append(_guard(resource.table, f'{_ident(resource.key)}={entry} AND ({resource.predicate})', bundle['children'][resource.identity], schema[resource.table]))
    return ' AND '.join(parts)


def _insert(table, columns, values):
    return f'INSERT INTO world.{_ident(table)} (' + ','.join(_ident(c) for c in columns) + ') SELECT ' + ','.join(_literal(v) for v in values) + ' WHERE @character_templates_ready=1;'


def _copy(table, columns, overrides, predicate):
    # Copy original typed values inside MySQL. In particular FLOAT, DECIMAL,
    # NULL and future schema columns do not round-trip through Python SQL
    # literals when constructing a complete clone.
    expressions = [_literal(overrides[c]) if c in overrides else _ident(c) for c in columns]
    return f'INSERT INTO world.{_ident(table)} (' + ','.join(_ident(c) for c in columns) + ') SELECT ' + ','.join(expressions) + f' FROM world.{_ident(table)} WHERE ({predicate}) AND @character_templates_ready=1;'


def _transaction(settings, guards, statements):
    sql = 'START TRANSACTION;\nSET @character_templates_ready=(' + (' AND '.join(guards) or '1') + ');\n'
    sql += '\n'.join(statements) + '\nCOMMIT;\nSELECT @character_templates_ready;'
    require(_query(settings, sql).strip() == '1', 'Character template activation guard refused changed database state')


def restore(settings, specs=None):
    """Restore only owned spawn bindings for the ordinary reviewed bootstrap."""
    settlement.ensure_world_offline()
    schema = _schema(settings)
    installed, sources, targets, spawns = _verified(settings, schema, allow_restored=True)
    guards, updates = [], []
    for row in installed:
        spec = _spec(row)
        guards.extend((_guard(LEDGER, f'`spawn_id`={spec.spawn_id}', [row]),
            _bundle_guard(schema, spec, sources[spec.spawn_id], spec.base_entry),
            _bundle_guard(schema, spec, targets[spec.spawn_id], spec.entry),
            _guard('creature', f'`guid`={spec.spawn_id}', [spawns[spec.spawn_id]], schema['creature']),
            f'(SELECT COUNT(*)=0 FROM world.creature WHERE id={spec.entry} AND guid<>{spec.spawn_id})'))
        updates.append(f'UPDATE world.creature SET id={spec.base_entry} WHERE guid={spec.spawn_id} AND map=725 AND @character_templates_ready=1;')
    if installed:
        require(settings.ai_profile == full_population.PROFILE, 'Installed character identities require full-v8')
        settlement.ensure_world_offline()
        _transaction(settings, guards, updates)
        verify(settings, allow_restored=True)
    return tuple(_spec(row) for row in installed)


def apply(settings, specs=None):
    settlement.ensure_world_offline()
    specs = load_descriptors() if specs is None else tuple(specs)
    schema = _schema(settings)
    installed, sources, targets, spawns = _verified(settings, schema, allow_restored=True)
    if not specs and not installed: return ()
    require(settings.ai_profile == full_population.PROFILE, 'Native character templates require full-v8')
    _query(settings, f'''CREATE TABLE IF NOT EXISTS world.{LEDGER} (
 spawn_id BIGINT UNSIGNED NOT NULL PRIMARY KEY,entry INT UNSIGNED NOT NULL UNIQUE,
 base_entry INT UNSIGNED NOT NULL,map_id INT UNSIGNED NOT NULL,
 name VARCHAR(100) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
 title VARCHAR(100) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
 source_sha256 CHAR(64) CHARACTER SET ascii NOT NULL,clone_sha256 CHAR(64) CHARACTER SET ascii NOT NULL,
 children_sha256 CHAR(64) CHARACTER SET ascii NOT NULL) ENGINE=InnoDB;''')
    schema = _schema(settings)
    existing = {row['spawn_id']: row for row in installed}
    desired = {_spec(row).spawn_id: _spec(row) for row in installed}
    require(len(specs) == len({s.spawn_id for s in specs}), 'Duplicate desired character template')
    for spec in specs:
        validated = spec_from_document(spec.document())
        desired[spec.spawn_id] = validated
    guards, statements = [], []
    for spec in sorted(desired.values(), key=lambda s: s.spawn_id):
        source = sources.get(spec.spawn_id) or _bundle(settings, schema, spec.base_entry)
        expected = cloned_bundle(source, spec)
        spawn = spawns.get(spec.spawn_id) or _spawn(settings, schema, spec, allow_restored=True)
        guards.extend((_bundle_guard(schema, spec, source, spec.base_entry),
            _guard('creature', f'`guid`={spec.spawn_id}', [spawn], schema['creature']),
            f'(SELECT COUNT(*)=0 FROM world.creature WHERE id={spec.entry} AND guid<>{spec.spawn_id})'))
        old = existing.get(spec.spawn_id)
        if old:
            guards.extend((_guard(LEDGER, f'`spawn_id`={spec.spawn_id}', [old]),
                           _bundle_guard(schema, spec, targets[spec.spawn_id], spec.entry)))
            if _spec(old) != spec:
                statements.append(f'UPDATE world.creature_template SET name={_literal(spec.name)},subname={_literal(spec.title)} WHERE entry={spec.entry} AND @character_templates_ready=1;')
                if 'creature_template_locale' in schema:
                    statements.append(f'UPDATE world.creature_template_locale SET Name={_literal(spec.name)},Title={_literal(spec.title)} WHERE entry={spec.entry} AND @character_templates_ready=1;')
        else:
            guards.extend((_guard(LEDGER, f'`spawn_id`={spec.spawn_id} OR `entry`={spec.entry}', []),
                           _guard('creature_template', f'`entry`={spec.entry}', [])))
            for resource in RESOURCES:
                if resource.table in schema:
                    guards.append(_guard(resource.table, f'{_ident(resource.key)}={spec.entry} AND ({resource.predicate})', []))
            columns = [column for column, _ in schema['creature_template']]
            statements.append(_copy('creature_template', columns,
                dict(entry=spec.entry, name=spec.name, subname=spec.title), f'`entry`={spec.base_entry}'))
            for resource in RESOURCES:
                if resource.table in schema:
                    columns = [column for column, _ in schema[resource.table]]
                    overrides = {resource.key: spec.entry}
                    if resource.table == 'creature_template_locale':
                        overrides.update(Name=spec.name, Title=spec.title)
                    statements.append(_copy(resource.table, columns, overrides,
                        f'{_ident(resource.key)}={spec.base_entry} AND ({resource.predicate})'))
        record = {**asdict(spec), 'source_sha256': _digest(source), 'clone_sha256': _digest(expected),
                  'children_sha256': _digest(expected['children'])}
        if old:
            assignments = ','.join(_ident(key)+'='+_literal(record[key]) for key in LEDGER_FIELDS if key != 'spawn_id')
            statements.append(f'UPDATE world.{LEDGER} SET {assignments} WHERE spawn_id={spec.spawn_id} AND @character_templates_ready=1;')
        else:
            statements.append(_insert(LEDGER, LEDGER_FIELDS, [record[column] for column in LEDGER_FIELDS]))
        statements.append(f'UPDATE world.creature SET id={spec.entry} WHERE guid={spec.spawn_id} AND map=725 AND @character_templates_ready=1;')
    settlement.ensure_world_offline()
    _transaction(settings, guards, statements)
    return verify(settings, tuple(desired.values()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('plan', 'restore', 'apply', 'verify'))
    parser.add_argument('--directory', type=Path, default=DIRECTORY)
    parser.add_argument('--profiles', type=Path, default=PROFILES)
    args = parser.parse_args(argv)
    try:
        try:
            from .manage import Settings
        except ImportError:
            from manage import Settings
        settings = Settings.load(os.environ)
        if args.command == 'plan':
            specs = load_descriptors(args.directory, args.profiles)
            require(not specs or settings.ai_profile == full_population.PROFILE, 'Native character templates require full-v8')
            print(f'Native character template authoring validated: {len(specs)} requested identities; database unchanged.')
        elif args.command == 'restore':
            print(f'Owned character spawn bindings restored for reviewed bootstrap: {len(restore(settings))}.')
        elif args.command == 'apply':
            bindings = apply(settings, load_descriptors(args.directory, args.profiles))
            print(f'Complete independent native character templates installed and verified: {len(bindings)}.')
        else:
            print(f'Installed native character templates verified: {len(verify(settings))}.')
    except (ProfileError, ValueError, OSError) as exc:
        # Never expose authored lore, SQL, credential-bearing subprocess output
        # or raw paths in an authoring failure.
        print('Character template operation failed: ' + (str(exc) if isinstance(exc, ProfileError) else 'invalid private input or database access'))
        raise SystemExit(2)


if __name__ == '__main__': main()
