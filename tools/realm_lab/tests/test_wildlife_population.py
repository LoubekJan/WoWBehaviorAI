"""Pinned ownership overlay and fail-closed wildlife deployment contracts."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import settlement_population as settlement, single_return, wildlife_population as wildlife
from tools.realm_lab.tests.test_settlement_population import legacy_snapshot, snapshot as native_snapshot

ROOT = Path(__file__).resolve().parents[3]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf-8", newline="\n")


def fixture(root):
    """Real reviewed actors/contracts with small synthetic native geometry."""
    metadata, data = root / "metadata", root / "data"
    metadata.mkdir()
    native = single_return.load_json(settlement.METADATA / settlement.MANIFEST)
    proof = single_return.load_json(settlement.METADATA / settlement.POSITION_PROOF)
    server = single_return.load_json(settlement.METADATA / "server-manifest.json")
    overlay = single_return.load_json(settlement.METADATA / wildlife.MANIFEST)
    files = []
    for record in native["geometry_files"]:
        path = data / record["path"]
        path.parent.mkdir(parents=True, exist_ok=True)
        content = ("wildlife-fixture:" + record["path"]).encode("ascii")
        path.write_bytes(content)
        files.append({"path": record["path"], "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
    proof["geometry_files"] = native["geometry_files"] = server["files"] = files
    native["legacy_snapshot_sha256"] = settlement.legacy_snapshot_digest(legacy_snapshot())
    write_json(metadata / settlement.POSITION_PROOF, proof)
    proof_sha = hashlib.sha256((metadata / settlement.POSITION_PROOF).read_bytes()).hexdigest()
    native["placement_proof_sha256"] = overlay["placement_proof_sha256"] = proof_sha
    write_json(metadata / settlement.MANIFEST, native)
    native_sha = hashlib.sha256((metadata / settlement.MANIFEST).read_bytes()).hexdigest()
    overlay["settlement_manifest_sha256"] = native_sha
    write_json(metadata / "server-manifest.json", server)
    write_json(metadata / wildlife.MANIFEST, overlay)
    return metadata, data, native_sha, proof_sha


def snapshot(population, control=1, *, legacy=True, owned=None):
    rows = native_snapshot(population.settlement, legacy=legacy)
    actors = {actor.spawn_id: actor for actor in population.settlement.actors}
    for row in rows:
        if row["kind"] == "spawn" and row["guid"] in actors:
            row["orientation"] = actors[row["guid"]].orientation
    selected = wildlife.SPAWN_IDS if owned is None else set(owned)
    for actor in population.actors:
        if actor.spawn_id not in selected:
            continue
        rows.extend([
            {"kind": "agent", "id": actor.spawn_id, "spawn": actor.spawn_id, "map": 725,
             "type": actor.agent_type, "control": control, "home_map": 725,
             "home_x": actor.x, "home_y": actor.y, "home_z": actor.z,
             "home_o": actor.orientation, "work_map": None, "faction": 0},
            {"kind": "participation", "spawn": actor.spawn_id, "mode": 0},
            {"kind": "wildlife_work", "id": actor.spawn_id, "spawn": actor.spawn_id,
             "work_map": None, "work_x": None, "work_y": None, "work_z": None, "work_o": None},
        ])
    rows.extend({"kind": "type", "entry": entry, "type": wildlife.ROLE_TYPES[role]}
                for role, entry in wildlife.ROLE_ENTRIES.items())
    return rows


class PublishedWildlifeTests(unittest.TestCase):
    def test_real_overlay_pins_repository_bytes_and_preserves_all_homes(self):
        population = wildlife.Population.load()
        self.assertEqual(hashlib.sha256((settlement.METADATA / settlement.MANIFEST).read_bytes()).hexdigest(),
                         wildlife.BASE_MANIFEST_SHA256)
        self.assertEqual(hashlib.sha256((settlement.METADATA / settlement.POSITION_PROOF).read_bytes()).hexdigest(),
                         wildlife.BASE_PLACEMENT_PROOF_SHA256)
        self.assertEqual(len(population.actors), 20)
        self.assertEqual({a.spawn_id for a in population.actors}, set(range(901072, 901092)))
        self.assertEqual({a.spawn_id for a in population.actors if a.role == "predator"},
                         {901076, 901081, 901086, 901091})
        native = {a.spawn_id: a for a in population.settlement.actors}
        for actor in population.actors:
            self.assertEqual((actor.entry, actor.label, actor.x, actor.y, actor.z, actor.orientation),
                             tuple(getattr(native[actor.spawn_id], field) for field in
                                   ("entry", "label", "x", "y", "z", "orientation")))


class WildlifePopulationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data, manifest_sha, proof_sha = fixture(Path(self.tmp.name))
        for name, value in (("BASE_MANIFEST_SHA256", manifest_sha), ("BASE_PLACEMENT_PROOF_SHA256", proof_sha)):
            patcher = patch.object(wildlife, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.population = wildlife.Population.load(self.metadata)
        self.path = self.metadata / wildlife.MANIFEST
        self.document = single_return.load_json(self.path)

    def test_same_sixty_one_native_files_are_verified_before_ownership_work(self):
        self.assertEqual(wildlife.verify_data(self.data, self.metadata), self.population)
        path = self.data / "mmaps/7253130.mmtile"
        path.write_bytes(b"changed navigation")
        with self.assertRaisesRegex(wildlife.ProfileError, "geometry hash mismatch"):
            wildlife.verify_data(self.data, self.metadata)

    def test_scope_observes_all_hundred_but_owns_only_twenty_natural_living_roles(self):
        config = self.population.config()
        self.assertEqual(config["AIWorld.ScopeSpawnIds"], self.population.settlement.config()["AIWorld.ScopeSpawnIds"])
        self.assertEqual(len(config["AIWorld.ScopeSpawnIds"].strip('"').split(",")), 100)
        self.assertEqual(len(self.population.spawn_ids.split(",")), 20)
        self.assertEqual(self.population.observer_environment(), self.population.settlement.observer_environment())
        for key in ("Enable", "LivingRolesEnabled", "LivingNeedEvolutionEnabled", "LivingRoleExtensionsEnabled",
                    "TelemetryEnabled", "TelemetryNativeCreaturesEnabled"):
            self.assertEqual(config["AIWorld." + key], "1", key)
        for key in ("RemoteInferenceEnabled", "GroupCoarseSimulationEnabled", "EnableSpawnReconciliation",
                    "EnableZoneControlActivation", "LivingWolvesEnabled", "WolfGroupAutoFormation",
                    "DefiasGroupAutoFormation", "CoalitionMaintenance", "GroupCoordination", "DynamicTaskEnable"):
            self.assertEqual(config["AIWorld." + key], "0", key)
        self.assertEqual(config["AIWorld.NeedsHungerRatePerSecond"], "0.003")
        self.assertEqual(config["AIWorld.NeedsFatigueRatePerSecond"], "0")

    def test_modified_base_or_proof_cannot_be_adopted_by_editing_overlay_hash(self):
        for filename, field in ((settlement.MANIFEST, "settlement_manifest_sha256"),
                                (settlement.POSITION_PROOF, "placement_proof_sha256")):
            path = self.metadata / filename
            original = path.read_bytes()
            try:
                path.write_bytes(original + b" ")
                document = copy.deepcopy(self.document)
                document[field] = hashlib.sha256(path.read_bytes()).hexdigest()
                write_json(self.path, document)
                with self.subTest(file=filename), self.assertRaisesRegex(wildlife.ProfileError, "unchanged"):
                    wildlife.Population.load(self.metadata)
            finally:
                path.write_bytes(original)
                write_json(self.path, self.document)

    def test_actor_roles_homes_labels_and_control_whitelist_are_immutable(self):
        mutations = (
            lambda d: d.update(profile="settlement-v8"),
            lambda d: d.update(map_id=0),
            lambda d: d.update(realm_id=1),
            lambda d: d.update(owned_spawn_ids=list(range(901071, 901091))),
            lambda d: d["native_spawn_ids"].pop(),
            lambda d: d["need_rates"].update(hunger_per_second=1000),
            lambda d: d["actors"].pop(),
            lambda d: d["actors"][0].update(spawn_id=901000),
            lambda d: d["actors"][0].update(spawn_id=901073),
            lambda d: d["actors"][0].update(entry=1186, role="predator"),
            lambda d: d["actors"][0].update(label="new_home"),
            lambda d: d["actors"][0]["home"].update(x=0),
            lambda d: d["actors"][0]["home"].update(z=float("nan")),
            lambda d: d["actors"][0].update(home={}),
        )
        for mutation in mutations:
            document = copy.deepcopy(self.document)
            mutation(document)
            write_json(self.path, document)
            with self.subTest(mutation=mutation), self.assertRaises(wildlife.ProfileError):
                wildlife.Population.load(self.metadata)

    def test_active_and_observe_phases_accept_exact_overlay_without_changing_input(self):
        for legacy in (True, False):
            for control in (0, 1):
                rows = snapshot(self.population, control, legacy=legacy)
                before = copy.deepcopy(rows)
                wildlife.validate_snapshot(rows, self.population, control=control)
                self.assertEqual(rows, before)
                # The original native validator remains strict for this input.
                with self.assertRaises(settlement.ProfileError):
                    settlement.validate_snapshot([r for r in rows if r["kind"] in settlement.FIELDS], self.population.settlement)

    def test_only_explicit_offline_bootstrap_accepts_partial_bindings_and_mixed_control(self):
        rows = snapshot(self.population, owned={901072, 901076})
        next(row for row in rows if row["kind"] == "agent" and row["id"] == 901072)["control"] = 0
        wildlife.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        complete = snapshot(self.population)
        next(row for row in complete if row["kind"] == "agent" and row["id"] == 901072)["control"] = 0
        wildlife.validate_snapshot(complete, self.population, control=None)
        for phase in (0, 1, None):
            with self.subTest(phase=phase), self.assertRaises(wildlife.ProfileError):
                wildlife.validate_snapshot(rows, self.population, control=phase)
        incomplete_native = [row for row in rows if row.get("guid") != 901000]
        wildlife.validate_snapshot(incomplete_native, self.population, control=None, bootstrap=True)
        with self.assertRaises(wildlife.ProfileError):
            wildlife.validate_snapshot(incomplete_native, self.population)

    def test_every_owned_agent_field_is_validated_before_filtering_native_contract(self):
        changes = {"id": 999, "spawn": 999, "map": 0, "type": 6, "control": 2, "home_map": 0,
                   "home_x": 0, "home_y": 0, "home_z": 0, "home_o": 0,
                   "work_map": 725, "faction": 1}
        for field, value in changes.items():
            rows = snapshot(self.population)
            next(row for row in rows if row["kind"] == "agent" and row["id"] == 901072)[field] = value
            with self.subTest(field=field), self.assertRaises(wildlife.ProfileError):
                wildlife.validate_snapshot(rows, self.population, control=None, bootstrap=True)

    def test_duplicate_participation_classification_work_and_group_drift_fail_closed(self):
        for kind in ("agent", "participation", "wildlife_work", "type"):
            rows = snapshot(self.population)
            selected = next(row for row in rows if row["kind"] == kind and
                            row.get("id", row.get("spawn", 901072)) not in settlement.LEGACY_IDS)
            with self.subTest(kind=kind), self.assertRaises(wildlife.ProfileError):
                wildlife.validate_snapshot(rows + [copy.deepcopy(selected)], self.population)
        for field in ("work_map", "work_x", "work_y", "work_z", "work_o"):
            rows = snapshot(self.population)
            next(row for row in rows if row["kind"] == "wildlife_work")[field] = 0
            with self.subTest(field=field), self.assertRaises(wildlife.ProfileError):
                wildlife.validate_snapshot(rows, self.population)
        for row in ({"kind": "participation", "spawn": 901072, "mode": 1},
                    {"kind": "type", "entry": 883, "type": 6},
                    {"kind": "membership", "group": 1, "member": 901072}):
            rows = snapshot(self.population)
            rows = [existing for existing in rows if not (existing["kind"] == row["kind"] and
                    existing.get("spawn", existing.get("entry")) == row.get("spawn", row.get("entry")))]
            with self.subTest(row=row), self.assertRaises(wildlife.ProfileError):
                wildlife.validate_snapshot(rows + [row], self.population)

    def test_base_native_spawns_templates_smartai_and_legacy_are_not_exempted(self):
        for kind, identity, field, value in (("spawn", "guid", "x", 0),
                                            ("template", "entry", "faction", 999),
                                            ("agent", "id", "control", 1)):
            rows = snapshot(self.population)
            row = next(r for r in rows if r["kind"] == kind and
                       (r[identity] in settlement.LEGACY_IDS if kind == "agent" else True))
            row[field] = value
            with self.subTest(kind=kind), self.assertRaises(wildlife.ProfileError):
                wildlife.validate_snapshot(rows, self.population)
        for extra in (
            {"kind": "membership", "group": 3, "member": 901000},
            {"kind": "participation", "spawn": 901000, "mode": 0},
            dict(next(r for r in snapshot(self.population) if r["kind"] == "agent" and r["id"] == 901072),
                 id=999, spawn=999, map=0),
            dict(next(r for r in snapshot(self.population) if r["kind"] == "agent" and r["id"] == 901072),
                 id=901000, spawn=901000, control=0),
        ):
            with self.subTest(extra=extra), self.assertRaises(wildlife.ProfileError):
                wildlife.validate_snapshot(snapshot(self.population) + [extra], self.population)
        rows = [r for r in snapshot(self.population) if r.get("guid") != 900725]
        with self.assertRaisesRegex(wildlife.ProfileError, "legacy population is incomplete"):
            wildlife.validate_snapshot(rows, self.population)

    def test_truncated_or_unknown_readback_cannot_disappear_during_filtering(self):
        rows = snapshot(self.population)
        for extra in ({"kind": "agent", "id": 901072}, {"kind": "wildlife_work"},
                      {"kind": "unknown"}, {"kind": "participation", "spawn": True, "mode": 0}):
            with self.subTest(extra=extra), self.assertRaises(wildlife.ProfileError):
                wildlife.validate_snapshot(rows + [extra], self.population, control=None, bootstrap=True)
        with self.assertRaisesRegex(wildlife.ProfileError, "workplace read-back incomplete"):
            wildlife.validate_snapshot([r for r in rows if not (r["kind"] == "wildlife_work" and r["id"] == 901072)], self.population)

    def test_sql_is_additive_bootstrap_and_narrow_idempotent_activation(self):
        bootstrap = wildlife.bootstrap_sql(self.population)
        activation = wildlife.activation_sql(self.population)
        self.assertEqual(bootstrap.count("INSERT INTO characters.ai_agents"), 20)
        self.assertEqual(bootstrap.count("INSERT INTO world.ai_spawn_participation_defaults"), 20)
        self.assertEqual(bootstrap.count("INSERT INTO world.ai_agent_type_entry_defaults"), 2)
        self.assertNotIn("UPDATE", bootstrap)
        self.assertEqual(activation.count("UPDATE characters.ai_agents"), 1)
        self.assertIn("COUNT(*)=20", activation)
        self.assertIn("@wildlife_population_ready=1", activation)
        self.assertIn("ct.unit_flags2=2048", activation)
        self.assertIn("ct.flags_extra=66", activation)
        self.assertIn("c.guid=901000", activation)
        self.assertIn("c.guid=901099", activation)
        for sql in (bootstrap, activation, wildlife.deactivation_sql(self.population)):
            for forbidden in ("auth.", "DELETE", "ALTER", "UPDATE world.", "INSERT INTO world.creature (", "SET home_", "hunger"):
                self.assertNotIn(forbidden, sql)
        self.assertTrue(wildlife.snapshot_sql(self.population).startswith(settlement.snapshot_sql(self.population.settlement)))


if __name__ == "__main__":
    unittest.main()
