from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[3]


class ComposeLoader(yaml.SafeLoader):
    pass


ComposeLoader.add_constructor("!override", lambda loader, node: loader.construct_sequence(node))
ComposeLoader.add_constructor("!reset", lambda loader, node: None)


def document(path):
    return yaml.load((ROOT / path).read_text(), Loader=ComposeLoader)


class RealmIsolationTests(unittest.TestCase):
    def test_mutable_volumes_and_runtime_mounts_are_disjoint(self):
        primary = document("compose.yml")
        lab = document("compose.lab.yml")
        primary_names = {v["name"] for v in primary["volumes"].values()}
        lab_names = {v["name"] for v in lab["volumes"].values()}
        self.assertTrue(primary_names.isdisjoint(lab_names))
        self.assertEqual(lab["name"], "aitc-lab")
        self.assertNotIn("authserver", lab["services"])
        for name, service in lab["services"].items():
            for mount in service.get("volumes", []):
                source = mount.split(":")[0]
                if source.startswith("./runtime/"):
                    self.assertTrue(source.startswith("./runtime/lab/"), (name, mount))
        self.assertIn("./runtime/lab/data:/runtime/data:ro", lab["services"]["worldserver"]["volumes"])
        self.assertEqual(lab["services"]["worldserver"]["ports"], ["${LAB_WORLD_PORT:-9086}:8085"])

    def test_only_worldserver_and_setup_tools_reach_shared_auth_network(self):
        lab = document("compose.lab.yml")
        for name, service in lab["services"].items():
            if name in ("worldserver", "tc-dev"):
                self.assertIn("shared-auth", service["networks"])
            else:
                self.assertNotIn("shared-auth", service.get("networks", []))

    def test_lab_observer_and_recorder_derive_scope_from_readonly_metadata(self):
        lab = document("compose.lab.yml")
        self.assertEqual(lab["services"]["worldserver"]["environment"]["LAB_AI_PROFILE"], "${LAB_AI_PROFILE:-disabled}")
        for service in ("worldserver", "world-viewer", "aiworld-recorder"):
            mounts = lab["services"][service]["volumes"]
            self.assertIn("./data/realm_lab:/workspace/data/realm_lab:ro", mounts)
        for service in ("world-viewer", "aiworld-recorder"):
            self.assertIn("/workspace/tools/realm_lab/run_scoped.py", lab["services"][service]["command"])
        self.assertIn("./docker/world-viewer/app/scope.py:/scope.py:ro", lab["services"]["aiworld-recorder"]["volumes"])

    def test_ci_volumes_do_not_overlap_runtime_and_need_no_auth_network(self):
        lab = document("compose.lab.yml")
        ci = document("compose.lab.ci.yml")
        for volume in ("build-data", "ccache-data"):
            self.assertNotEqual(ci["volumes"][volume]["name"], lab["volumes"][volume]["name"])
        self.assertEqual(ci["services"]["tc-dev"]["networks"], ["default"])
        self.assertIsNone(ci["networks"]["shared-auth"])

    def test_cd_is_branch_bound_and_never_restarts_primary_services(self):
        workflow = document(".github/workflows/realm-lab.yml")
        deploy = workflow["jobs"]["deploy"]
        self.assertIn("refs/heads/AI-World-lab", deploy["if"])
        self.assertIn("github.event_name == 'push'", deploy["if"])
        self.assertIn("REALM_LAB_DEPLOY_ENABLED", deploy["if"])
        self.assertIn("realm-lab-deploy", deploy["runs-on"])
        scripts = (ROOT / "docker/scripts/deploy-lab.sh").read_text()
        for forbidden in ("make build", "restart authserver", "configure-realm.sh", "provision-auth", "register-realm"):
            self.assertNotIn(forbidden, scripts)

    def test_original_ci_cannot_build_manual_lab_ref_into_original_volumes(self):
        workflow = document(".github/workflows/ci.yml")
        self.assertIn("github.ref == 'refs/heads/ai-world'", workflow["jobs"]["build-server"]["if"])


if __name__ == "__main__":
    unittest.main()
