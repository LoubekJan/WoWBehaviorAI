"""Authoring inputs cannot overwrite a shared or unrelated template identity."""
from dataclasses import replace
from pathlib import Path
import copy
import json
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import character_templates as templates, full_population


def document(spawn=901014, name='Šaša', title='Obyvatelka'):
    population = full_population.Population.load()
    actor = next(a for a in population.settlement.actors if a.spawn_id == spawn)
    return dict(schema_version=1, spawn_id=spawn, entry=9800000+spawn-901000,
                base_entry=actor.entry, map_id=725, name=name, title=title)


class CharacterTemplateTests(unittest.TestCase):
    def test_descriptors_require_reviewed_target_and_matching_native_card(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profiles, descriptors = root/'profiles', root/'templates'
            profiles.mkdir(); descriptors.mkdir()
            source = document()
            card = dict(agent_id=901014, spawn_id=901014, map_id=725, name='Šaša', backstory='Příběh', personality='Osobnost')
            (profiles/'901014.json').write_text(json.dumps(card), encoding='utf-8')
            path = descriptors/'901014.json'
            path.write_text(json.dumps(source), encoding='utf-8')
            result = templates.load_descriptors(descriptors, profiles)
            self.assertEqual(result[0].document(), source)
            for key, value in (('entry', 11328), ('entry', True), ('base_entry', 295),
                               ('spawn_id', 901100), ('map_id', 0), ('name', 'Běla'),
                               ('title', 'x\nA'), ('title', '   '),
                               ('name', '|cff000000Šaša'), ('title', 'á'*41)):
                candidate = {**source, key: value}
                path.write_text(json.dumps(candidate), encoding='utf-8')
                with self.subTest(key=key, value=value), self.assertRaises(templates.ProfileError):
                    templates.load_descriptors(descriptors, profiles)
            path.write_text(json.dumps(source).replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1'), encoding='utf-8')
            with self.assertRaises(templates.ProfileError): templates.load_descriptors(descriptors, profiles)
            self.assertEqual(templates.load_descriptors(root/'missing', profiles), ())

    def test_clone_copies_every_field_and_child_preserving_source(self):
        source = dict(template=dict(entry=11328, name='Eastvale Peasant', subname=None,
            npcflag=128, NewFutureColumn='unchanged', HealthModifier=2.75, ScriptName='', lootid=11328),
            children=dict(npc_vendor=[dict(entry=11328, item=123, maxcount=2)],
                creature_template_locale=[dict(entry=11328, locale='deDE', Name='Bauer', Title=None)],
                creature_equip_template=[dict(CreatureID=11328, ID=1, ItemID1=17)],
                entry_conditions_group=[dict(SourceGroup=11328, SourceTypeOrReferenceId=23, SourceEntry=123)]))
        before = copy.deepcopy(source)
        spec = templates.spec_from_document(document())
        actual = templates.cloned_bundle(source, spec)
        self.assertEqual(source, before)
        self.assertEqual(actual['template'], {**source['template'], 'entry': 9800014, 'name': 'Šaša', 'subname': 'Obyvatelka'})
        self.assertEqual(actual['children']['npc_vendor'][0], dict(entry=9800014, item=123, maxcount=2))
        self.assertEqual(actual['children']['creature_template_locale'][0]['Name'], 'Šaša')
        self.assertEqual(actual['children']['entry_conditions_group'][0]['SourceGroup'], 9800014)

    def test_normalization_is_binding_specific_and_does_not_mutate_database_readback(self):
        spec = templates.spec_from_document(document())
        original = [dict(kind='spawn', guid=901014, entry=9800014), dict(kind='spawn', guid=901015, entry=11328),
                    dict(kind='agent', id=901014, type=0)]
        rows = templates.normalize_snapshot(original, (spec,))
        self.assertEqual(rows[0]['entry'], 11328)
        self.assertEqual(original[0]['entry'], 9800014)
        self.assertEqual(rows[1:], original[1:])
        self.assertEqual(templates.normalize_snapshot(rows, (spec,)), rows)
        with self.assertRaises(templates.ProfileError):
            templates.normalize_snapshot([dict(kind='spawn', guid=901014, entry=9800035)], (spec,))

    def test_read_only_verification_never_reads_current_authoring_files(self):
        with patch.object(templates, '_schema', return_value={}), \
             patch.object(templates, '_verified', return_value=([], {}, {}, {})), \
             patch.object(templates, 'load_descriptors', side_effect=AssertionError('must not load authoring files')):
            self.assertEqual(templates.verify(object()), ())

    def test_sql_labels_are_encoded_and_whole_operation_uses_one_guard(self):
        name="X'); DROP TABLE world.creature; --"
        sql=templates._insert('creature_template', ['entry','name'], [9800014,name])
        self.assertNotIn(name, sql)
        self.assertIn('CONVERT(X\'', sql)
        self.assertIn('@character_templates_ready=1', sql)
        with patch.object(templates, '_query', return_value='0') as query:
            with self.assertRaises(templates.ProfileError): templates._transaction(object(), ['1=0'], [sql])
        self.assertEqual(query.call_count, 1)
        statement=query.call_args.args[1]
        self.assertTrue(statement.startswith('START TRANSACTION;'))
        self.assertIn('COMMIT;', statement)
        self.assertNotIn('DELETE', statement.replace(name,''))


if __name__ == '__main__': unittest.main()
