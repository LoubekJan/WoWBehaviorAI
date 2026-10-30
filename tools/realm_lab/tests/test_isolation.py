"""Canonical runtime boundaries and deployment entry-point contracts."""
from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[3]


def document(path):
    return yaml.safe_load((ROOT / path).read_text(encoding="utf-8"))


def triggers(workflow):
    # YAML 1.1 treats GitHub's unquoted `on` key as the boolean True.
    return workflow.get("on", workflow.get(True))


class RealmIsolationTests(unittest.TestCase):
    def test_studio_checks_the_world_profile_without_mutation_credentials(self):
        services = document("compose.yml")["services"]
        studio = services["character-studio"]
        self.assertEqual(studio["environment"]["LAB_AI_PROFILE"], services["worldserver"]["environment"]["LAB_AI_PROFILE"])
        self.assertIn("./data/realm_lab:/workspace/data/realm_lab:ro", studio["volumes"])
        self.assertIn("./tools/realm_lab:/workspace/tools/realm_lab:ro", studio["volumes"])
        self.assertFalse(any(key.startswith(("TC_DB_", "TC_AUTH_", "TC_SOAP_", "AI_NPC_MODEL_")) for key in studio["environment"]))

    def test_auth_and_world_storage_are_distinct_and_preserve_live_world_identity(self):
        world = document("compose.yml")
        self.assertEqual(world["name"], "aitc-lab")
        self.assertIn("authserver", world["services"])
        self.assertIn("auth-mysql", world["services"])
        self.assertIn("lab-mysql", world["services"])
        self.assertNotIn("mysql", world["services"])
        names = {key: value["name"] for key, value in world["volumes"].items()}
        self.assertEqual(len(set(names.values())), len(names))
        self.assertEqual(names["auth-mysql-data"], "aitc_auth_mysql-data")
        self.assertEqual(names["mysql-data"], "aitc_lab_mysql-data")
        self.assertEqual(names["build-data"], "aitc_lab_build-data")
        self.assertNotIn("aitc_mysql-data", names.values())
        self.assertIn("auth-mysql-data:/var/lib/mysql", world["services"]["auth-mysql"]["volumes"])
        self.assertIn("mysql-data:/var/lib/mysql", world["services"]["lab-mysql"]["volumes"])
        self.assertNotIn("ports", world["services"]["auth-mysql"])
        self.assertNotIn("ports", world["services"]["lab-mysql"])

    def test_auth_owner_is_not_exposed_to_worldserver_and_soap_stays_internal(self):
        world = document("compose.yml")
        services = world["services"]
        self.assertIn("TC_AUTH_SERVER_PASSWORD", services["authserver"]["environment"])
        self.assertNotIn("TC_AUTH_SERVER_PASSWORD", services["worldserver"]["environment"])
        self.assertIn("TC_AUTH_DB_PASSWORD", services["worldserver"]["environment"])
        self.assertEqual(services["auth-mysql"]["networks"]["default"]["aliases"], ["aiworld-auth-db"])
        self.assertFalse(world.get("networks"))
        for service in services.values():
            self.assertFalse(any("7878" in str(port) for port in service.get("ports", [])))
        self.assertEqual(services["account-web"]["environment"]["TC_SOAP_URL"], "http://worldserver:7878")
        self.assertEqual(services["account-web"]["environment"]["TC_ACCOUNT_EXPANSION"], "2")
        self.assertEqual(services["worldserver"]["depends_on"]["authserver"]["condition"], "service_healthy")

    def test_world_and_observer_use_authored_scope_and_readonly_game_data(self):
        services = document("compose.yml")["services"]
        self.assertEqual(services["worldserver"]["environment"]["LAB_AI_PROFILE"], "${LAB_AI_PROFILE:-settlement-v8}")
        self.assertIn("./runtime/lab/data:/runtime/data:ro", services["worldserver"]["volumes"])
        self.assertEqual(services["worldserver"]["ports"], ["${LAB_WORLD_PORT:-9086}:8085"])
        for name, service in services.items():
            for mount in service.get("volumes", []):
                source = mount.split(":")[0]
                if source.startswith("./runtime/"):
                    self.assertTrue(source.startswith("./runtime/lab/"), (name, mount))
        for name in ("worldserver", "world-viewer", "aiworld-recorder"):
            self.assertIn("./data/realm_lab:/workspace/data/realm_lab:ro", services[name]["volumes"])
        for name in ("world-viewer", "aiworld-recorder"):
            self.assertIn("/workspace/tools/realm_lab/run_scoped.py", services[name]["command"])
        self.assertIn("./docker/world-viewer/app/scope.py:/scope.py:ro", services["aiworld-recorder"]["volumes"])

    def test_legacy_entry_points_select_the_same_stack(self):
        compatibility = document("compose.lab.yml")
        self.assertEqual(compatibility["name"], "aitc-lab")
        self.assertEqual(compatibility["include"], [{"path": "compose.yml"}])
        self.assertNotIn("services", compatibility)
        self.assertEqual((ROOT / ".env.example").read_bytes(), (ROOT / "deploy/lab/.env.example").read_bytes())
        self.assertIn("include Makefile", (ROOT / "Makefile.lab").read_text())
        makefile = (ROOT / "Makefile").read_text()
        self.assertIn("WORLD_ENV_FILE ?= .env", makefile)
        self.assertNotIn("reset-db:", makefile)
        self.assertNotIn("docker volume rm", makefile)

    def test_ci_volumes_are_disjoint_from_runtime_and_need_no_external_network(self):
        runtime = document("compose.yml")
        ci = document("compose.lab.ci.yml")
        runtime_names = {volume["name"] for volume in runtime["volumes"].values()}
        ci_names = {volume["name"] for volume in ci["volumes"].values()}
        self.assertTrue(runtime_names.isdisjoint(ci_names))
        self.assertEqual(set(runtime["volumes"]), set(ci["volumes"]))
        self.assertFalse(ci.get("networks"))
        self.assertEqual(ci["name"], "aitc-lab-ci")

    def test_cd_is_trusted_branch_bound_with_isolated_builds(self):
        workflow = document(".github/workflows/realm-lab.yml")
        deploy = workflow["jobs"]["deploy"]
        self.assertIn("refs/heads/AI-World-lab", deploy["if"])
        self.assertIn("github.event_name == 'push'", deploy["if"])
        self.assertIn("REALM_LAB_DEPLOY_ENABLED", deploy["if"])
        self.assertIn("realm-lab-deploy", deploy["runs-on"])
        self.assertEqual(set(deploy["needs"]), {"contracts", "build-server"})
        build = workflow["jobs"]["build-server"]
        self.assertIn("refs/heads/AI-World-lab", build["if"])
        build_commands = "\n".join(step.get("run", "") for step in build["steps"])
        self.assertIn("-f compose.yml -f compose.lab.ci.yml", build_commands)
        self.assertNotIn("-f compose.lab.yml -f compose.lab.ci.yml", build_commands)
        all_commands = "\n".join(step.get("run", "") for job in workflow["jobs"].values() for step in job["steps"])
        self.assertNotIn("git fetch origin ai-world", all_commands)

    def test_retired_elwynn_workflow_cannot_automatically_deploy(self):
        retired = document(".github/workflows/ci.yml")
        self.assertEqual(set(triggers(retired)), {"workflow_dispatch"})
        for job in retired["jobs"].values():
            self.assertNotIn("self-hosted", job["runs-on"])
            for step in job["steps"]:
                command = step.get("run", "")
                self.assertNotIn("docker ", command)
                self.assertNotIn("make ", command)
                self.assertNotIn("ssh ", command)

    def test_runner_models_keep_ci_identity_and_only_canonical_deployment(self):
        ci = document("deploy/runner/compose.yml")
        deploy = document("deploy/runner/compose.lab.yml")
        self.assertEqual(ci["name"], "runner")
        self.assertEqual(set(ci["services"]), {"ci-runner"})
        self.assertEqual(deploy["name"], "aitc-lab-runners")
        self.assertEqual(set(deploy["services"]), {"lab-runner"})
        ci_labels = set(ci["services"]["ci-runner"]["environment"]["LABELS"].split(","))
        deploy_labels = set(deploy["services"]["lab-runner"]["environment"]["LABELS"].split(","))
        self.assertIn("ci", ci_labels)
        self.assertIn("realm-lab-deploy", deploy_labels)
        self.assertNotIn("deploy", ci_labels | deploy_labels)
        mounts = deploy["services"]["lab-runner"]["volumes"]
        self.assertIn("/home/voslik/WoWBehaviorAI-lab:/home/voslik/WoWBehaviorAI-lab", mounts)
        self.assertNotIn("/home/voslik/WoWBehaviorAI:/home/voslik/WoWBehaviorAI", mounts)


if __name__ == "__main__":
    unittest.main()
