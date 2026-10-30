"""All authored ownership is explicit, scoped and preserves the Defias world."""
import copy
import hashlib
import unittest
from unittest.mock import patch

from tools.realm_lab import defias_population as defias, full_population as full, single_return
from tools.realm_lab.tests import test_defias_population as defias_tests
from tools.realm_lab.tests.test_wildlife_population import write_json


def write_full_metadata(metadata, population):
    sha = hashlib.sha256((metadata / defias.MANIFEST).read_bytes()).hexdigest()
    with patch.object(full, "BASE_DEFIAS_SHA256", sha):
        write_json(metadata / full.MANIFEST, full.manifest_document(population))
    return sha


def snapshot(population, control=1, *, selected=None):
    rows = defias_tests.snapshot(population.defias_population, control)
    selected = full.RECRUIT_IDS if selected is None else set(selected)
    for actor in population.recruits:
        if actor.spawn_id in selected:
            rows.extend([
                dict(kind="agent", id=actor.spawn_id, spawn=actor.spawn_id, map=725,
                    type=actor.agent_type, control=control, home_map=725, home_x=actor.x,
                    home_y=actor.y, home_z=actor.z, home_o=actor.orientation,
                    work_map=None, faction=actor.world_faction),
                dict(kind="participation", spawn=actor.spawn_id, mode=0),
                dict(kind="full_work", id=actor.spawn_id, spawn=actor.spawn_id,
                    work_map=None, work_x=None, work_y=None, work_z=None, work_o=None)])
    return rows


class PublishedFullPopulationTests(unittest.TestCase):
    def test_published_full_profile_loads_with_canonical_lf_bytes_and_no_fixture_hash_overrides(self):
        defias_bytes = (full.METADATA / defias.MANIFEST).read_bytes()
        full_bytes = (full.METADATA / full.MANIFEST).read_bytes()
        self.assertNotIn(b"\r\n", defias_bytes)
        self.assertNotIn(b"\r\n", full_bytes)
        canonical_sha = hashlib.sha256(defias_bytes).hexdigest()
        self.assertEqual(full.BASE_DEFIAS_SHA256, canonical_sha)
        self.assertEqual(single_return.load_json(full.METADATA / full.MANIFEST)["defias_manifest_sha256"], canonical_sha)
        population = full.Population.load()
        self.assertEqual((population.profile, len(population.actors)), ("full-v8", 100))

    def test_all_hundred_keep_authored_geometry_services_and_existing_specialized_roles(self):
        population = full.Population.load()
        self.assertEqual(population.defias_population, defias.Population.load())
        self.assertEqual((len(population.actors), len(population.recruits), len(full.NATIVE_IDS)), (100, 45, 0))
        self.assertEqual({actor.spawn_id for actor in population.actors}, set(range(901000, 901100)))
        self.assertEqual(population.config(), {**population.defias_population.config(), 'AIWorld.LivingCaravans.Enabled': '1'})
        self.assertEqual(len(population.geometry_files), 61)
        originals = {actor.spawn_id: actor for actor in population.settlement.actors}
        for actor in population.recruits:
            self.assertEqual(tuple(getattr(actor, key) for key in ("entry", "label", "role", "x", "y", "z", "orientation")),
                tuple(getattr(originals[actor.spawn_id], key) for key in ("entry", "label", "role", "x", "y", "z", "orientation")))
        self.assertEqual(full.SERVICE_IDS, frozenset((*range(901008, 901014), 901040)))
        self.assertEqual({identity for identity, kind in full.RECRUIT_TYPES.items() if kind == 2}, full.SERVICE_IDS)
        self.assertEqual(full.RECRUIT_TYPES[901014], 0)
        self.assertEqual(full.RECRUIT_ROLES[901014], "CITY_CONSUMER")
        self.assertEqual({full.RECRUIT_FACTIONS[i] for i in range(901066, 901072)}, {4})
        self.assertEqual({full.RECRUIT_FACTIONS[i] for i in range(901092, 901100)}, {5})


class FullPopulationTests(unittest.TestCase):
    def setUp(self):
        self.fixture = defias_tests.DefiasPopulationTests("test_insert_only_offline_extension_preserves_every_parent_activation_guard")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.metadata, self.data = self.fixture.metadata, self.fixture.data
        sha = write_full_metadata(self.metadata, self.fixture.population)
        patcher = patch.object(full, "BASE_DEFIAS_SHA256", sha)
        patcher.start(); self.addCleanup(patcher.stop)
        self.population = full.Population.load(self.metadata)

    def test_manifest_cannot_expand_move_reclassify_remove_services_or_enable_implicit_llm(self):
        self.assertEqual(full.verify_data(self.data, self.metadata), self.population)
        original = single_return.load_json(self.metadata / full.MANIFEST)
        for mutate in (lambda d: d["actors"].pop(), lambda d: d["actors"][0].update(agent_type=True),
                       lambda d: d["actors"][8].update(agent_type=0),
                       lambda d: d["actors"][14]["home"].update(x=0),
                       lambda d: d["actors"][14].update(world_faction_id=2),
                       lambda d: d["actors"][14].update(living_role="RAIDER"),
                       lambda d: d.update(llm_cards_explicit_opt_in=False),
                       lambda d: d.update(runtime_raid_memory_only=False),
                       lambda d: d["owned_spawn_ids"].append(901100)):
            candidate = copy.deepcopy(original); mutate(candidate)
            write_json(self.metadata / full.MANIFEST, candidate)
            with self.subTest(mutate=mutate), self.assertRaises(full.ProfileError):
                full.Population.load(self.metadata)
        write_json(self.metadata / full.MANIFEST, original)
        with (self.metadata / defias.MANIFEST).open("ab") as stream: stream.write(b"\n")
        with self.assertRaises(full.ProfileError): full.Population.load(self.metadata)

    def test_every_remaining_binding_is_checked_before_parent_validation(self):
        full.validate_snapshot(snapshot(self.population), self.population)
        full.validate_snapshot(snapshot(self.population, selected=()), self.population, control=None, bootstrap=True)
        for kind, key, value in (("agent", "id", 999), ("agent", "spawn", 901015),
            ("agent", "type", 5), ("agent", "faction", 2), ("agent", "home_x", 0),
            ("agent", "control", True), ("agent", "work_map", 725),
            ("full_work", "work_o", 0), ("participation", "mode", 1)):
            rows = snapshot(self.population)
            row = next(row for row in rows if row["kind"] == kind and row.get("id", row.get("spawn")) == 901014)
            row[key] = value
            with self.subTest(kind=kind, key=key), self.assertRaises(full.ProfileError):
                full.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        for identity in (901008, 901014, 901066, 901092, 901050):
            rows = snapshot(self.population); rows.append(dict(kind="membership", group=1, member=identity))
            with self.subTest(identity=identity), self.assertRaises(full.ProfileError):
                full.validate_snapshot(rows, self.population)
        rows = snapshot(self.population)
        rows.append(copy.deepcopy(next(row for row in rows if row["kind"] == "full_work")))
        with self.assertRaises(full.ProfileError): full.validate_snapshot(rows, self.population)
        rows = [row for row in snapshot(self.population) if not (row["kind"] == "full_work" and row["id"] == 901014)]
        with self.assertRaises(full.ProfileError): full.validate_snapshot(rows, self.population)
        with self.assertRaises(full.ProfileError): defias.validate_snapshot(snapshot(self.population), self.population.defias_population)

    def test_one_guarded_transaction_extends_all_previous_guards_to_one_hundred(self):
        bootstrap, activation = full.bootstrap_sql(self.population), full.activation_sql(self.population)
        self.assertEqual(bootstrap.count("INSERT INTO characters.ai_agents"), 45)
        self.assertEqual(activation.count("UPDATE characters.ai_agents"), 1)
        for text in ("COUNT(*)=20", "COUNT(*)=3", "COUNT(*)=2", "COUNT(*)=12", "COUNT(*)=15", "COUNT(*)=45",
                     "@full_population_ready=1", "a.agent_type=0", "a.agent_type=1", "a.agent_type=2", "a.agent_type=5",
                     "agent_id IN (" + self.population.spawn_ids):
            self.assertIn(text, activation)
        for sql in (bootstrap, activation, full.deactivation_sql(self.population)):
            for forbidden in ("DELETE", "ALTER", "auth.", "UPDATE world.", "SET home_", "SET agent_type",
                              "food=", "money=", "resource=", "INSERT INTO characters.ai_agent_group_members"):
                self.assertNotIn(forbidden, sql)


if __name__ == "__main__": unittest.main()
