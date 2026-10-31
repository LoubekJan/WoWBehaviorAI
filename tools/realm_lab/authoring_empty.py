"""Read-only contract for an empty authored map while its final layout is built.

This profile never creates or deletes actors. The explicit, backed-up reset is
a separate offline operation. Existing population profiles remain unchanged.
"""
from __future__ import annotations

from pathlib import Path

try:
    from . import settlement_population as settlement
except ImportError:
    import settlement_population as settlement

PROFILE = 'authoring-empty'
ROOT = Path(__file__).resolve().parents[2]


def config() -> dict[str, str]:
    bounds = settlement.BOUNDS
    return {
        'AIWorld.ScopeMapId': '725', 'AIWorld.ScopeZoneIds': '"4988"',
        'AIWorld.ScopeBoundsEnabled': '1',
        'AIWorld.ScopeMinX': str(bounds['min_x']), 'AIWorld.ScopeMaxX': str(bounds['max_x']),
        'AIWorld.ScopeMinY': str(bounds['min_y']), 'AIWorld.ScopeMaxY': str(bounds['max_y']),
        'AIWorld.ScopeSpawnIds': '""', 'AIWorld.ScopeRestrictAgents': '0',
        'AIWorld.ScopeAlwaysActive': '0',
    }


def verify_empty(settings, *, root: Path = ROOT) -> dict[str, int]:
    try:
        from .manage import SetupError, lab_mysql
    except ImportError:
        from manage import SetupError, lab_mysql
    if settings.realm_id != 2 or settings.ai_profile != PROFILE or settings.npc_agents_enabled:
        raise SetupError('Empty authoring requires realm 2, authoring-empty and the NPC controller disabled')
    reserved = lambda column: f'({column} BETWEEN 900725 AND 900824 OR {column} BETWEEN 901000 AND 901099)'
    queries = {
        'creatures': 'SELECT COUNT(*) FROM world.creature WHERE map=725',
        'agents': 'SELECT COUNT(*) FROM characters.ai_agents WHERE map_id=725 OR ' + reserved('spawn_id') + ' OR ' + reserved('agent_id'),
        'participation': 'SELECT COUNT(*) FROM world.ai_spawn_participation_defaults WHERE ' + reserved('spawn_id'),
        'memberships': 'SELECT COUNT(*) FROM characters.ai_agent_group_members WHERE ' + reserved('member_agent_id'),
        'memories': 'SELECT COUNT(*) FROM characters.ai_long_term_memories WHERE ' + reserved('agent_id'),
        'respawns': 'SELECT COUNT(*) FROM characters.respawn WHERE type=0 AND mapId=725',
    }
    counts = {}
    for name, sql in queries.items():
        raw = lab_mysql(settings, sql)
        if not raw.isascii() or not raw.isdigit():
            raise SetupError('Invalid empty authoring database read-back')
        counts[name] = int(raw)
    installed = lab_mysql(settings, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='world' AND table_name='ai_character_templates'")
    if installed not in ('0', '1'):
        raise SetupError('Invalid character-template ledger schema read-back')
    templates = lab_mysql(settings, 'SELECT COUNT(*) FROM world.ai_character_templates') if installed == '1' else '0'
    if not templates.isascii() or not templates.isdigit():
        raise SetupError('Invalid installed character-template count')
    counts['templates'] = int(templates)
    for directory in ('npc_agents', 'npc_appearances', 'npc_templates'):
        counts[directory] = len(tuple((root / 'data' / directory).glob('*.json')))
    if any(counts.values()):
        raise SetupError('Empty authoring still contains NPCs, AI state or installed character inputs; use the explicit offline reset')
    return counts


def preflight(data: Path, settings):
    population = settlement.verify_data(data)
    verify_empty(settings)
    return population
