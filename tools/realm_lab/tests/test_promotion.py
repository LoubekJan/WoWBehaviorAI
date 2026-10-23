"""Retirement boundary tests: no Docker daemon or production data is used."""
from contextlib import redirect_stdout
import gzip
import hashlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import promotion


ROOT = Path(__file__).resolve().parents[3]


def original(service):
    return {"Id": f"original-{service}",
            "Config": {"Labels": {"com.docker.compose.project": promotion.OLD_PROJECT,
                                   "com.docker.compose.service": service},
                       "Env": ["TC_SOAP_USER=soap-admin", "TC_SOAP_PASSWORD=soap-test-password"]},
            "State": {"Running": True, "Health": {"Status": "healthy"}},
            "NetworkSettings": {"Networks": {"aitc-lab_default": {"IPAddress": "172.28.0.5"}}}}


class WorldPromotionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.state = self.directory / "runtime/lab/receipts/world-promotion.json"
        self.root_patch = patch.object(promotion, "ROOT", self.directory)
        self.state_patch = patch.object(promotion, "STATE", self.state)
        self.root_patch.start()
        self.state_patch.start()
        self.addCleanup(self.root_patch.stop)
        self.addCleanup(self.state_patch.stop)

    def source_environment(self, extra=""):
        source = self.directory / "deploy/lab/.env"
        source.parent.mkdir(parents=True)
        source.write_text("LAB_REALM_ID=2\nLAB_AI_PROFILE=settlement-v8\n"
                          "TC_DB_USER=lab_world\nTC_DB_PASSWORD=keep-world-password\n"
                          "TC_AUTH_DB_USER=lab_auth\nTC_AUTH_DB_PASSWORD=keep-login-password\n"
                          "WORLD_VIEWER_TELEMETRY_TOKEN=keep-token\n" + extra)
        return source

    def acceptance_state(self):
        return {"schema_version": 1, "status": "auth_ready", "original": {"mysql": "original-mysql"},
                "account_identity": {"count": 2, "identity_sha256": "digest", "maximum_id": 2},
                "source_account_max_id": 2}

    def test_process_failure_never_exposes_database_diagnostics(self):
        result = SimpleNamespace(returncode=1, stdout=b"private-account-password", stderr=b"secret-SQL")
        with patch.object(promotion.subprocess, "run", return_value=result):
            with self.assertRaises(promotion.PromotionError) as caught:
                promotion.run(["docker", "exec", "private-target"])
        self.assertNotIn("private-account-password", str(caught.exception))
        self.assertNotIn("secret-SQL", str(caught.exception))

    def test_identity_returns_digest_and_count_without_credential_material(self):
        raw = "1\tALICE\tAA\tBB\n2\tBOB\tCC\tDD"
        with patch.object(promotion, "sql", return_value=raw):
            result = promotion.account_identity("source")
        self.assertEqual(result["count"], 2)
        self.assertEqual(result["identity_sha256"], hashlib.sha256(raw.encode()).hexdigest())
        self.assertNotIn("ALICE", json.dumps(result))

    def test_environment_preserves_world_identity_and_copies_old_soap_credential(self):
        self.source_environment()
        promotion.configure_env({"account-web": original("account-web")})
        values = promotion.read_env(self.directory / ".env")
        self.assertEqual(values["TC_DB_PASSWORD"], "keep-world-password")
        self.assertEqual(values["TC_AUTH_DB_PASSWORD"], "keep-login-password")
        self.assertEqual(values["WORLD_VIEWER_TELEMETRY_TOKEN"], "keep-token")
        self.assertEqual(values["TC_SOAP_USER"], "soap-admin")
        self.assertEqual(values["TC_SOAP_PASSWORD"], "soap-test-password")
        self.assertEqual(values["LAB_REALM_ID"], "2")
        self.assertEqual(values["TC_AUTH_DB_HOST"], "aiworld-auth-db")
        self.assertTrue(values["AUTH_MYSQL_ROOT_PASSWORD"])

    def test_empty_existing_soap_fields_are_filled_from_the_working_original_service(self):
        source = self.source_environment("TC_SOAP_USER=\nTC_SOAP_PASSWORD=\n")
        (self.directory / ".env").write_bytes(source.read_bytes())
        promotion.configure_env({"account-web": original("account-web")})
        values = promotion.read_env(self.directory / ".env")
        self.assertEqual(values["TC_SOAP_USER"], "soap-admin")
        self.assertEqual(values["TC_SOAP_PASSWORD"], "soap-test-password")

    def test_incompatible_realm_or_dotenv_credentials_fail_before_writing_new_config(self):
        source = self.source_environment()
        for text in (source.read_text().replace("LAB_REALM_ID=2", "LAB_REALM_ID=1"),
                     source.read_text() + "UNSUPPORTED=secret$dollar\n"):
            source.write_text(text)
            with self.subTest(), self.assertRaises(promotion.PromotionError):
                promotion.configure_env({"account-web": original("account-web")})
            self.assertFalse((self.directory / ".env").exists())

    def test_cutover_copies_realm_permissions_and_only_deletes_old_realm_references(self):
        sql = promotion.cutover_sql()
        self.assertIn("FROM auth.account_access WHERE RealmID=1", sql)
        self.assertIn("GREATEST(SecurityLevel", sql)
        self.assertIn("INSERT IGNORE INTO auth.rbac_account_permissions", sql)
        self.assertIn("DELETE FROM auth.realmlist WHERE id=1", sql)
        self.assertIn("DELETE FROM auth.realmcharacters WHERE realmid=1", sql)
        self.assertIn("UPDATE auth.account SET expansion=2 WHERE expansion<2", sql)
        self.assertNotRegex(sql, r"DELETE\s+FROM\s+auth\.account\s")
        self.assertNotIn("DROP DATABASE", sql)
        self.assertNotIn("DROP USER", sql)
        self.assertNotIn("DELETE FROM characters.", sql)
        self.assertNotIn("DELETE FROM world.", sql)

    def test_dump_streams_valid_gzip_and_refuses_unexpected_database(self):
        content = b"-- private test fixture\nCREATE DATABASE auth;\nINSERT INTO auth.account VALUES (1,'TEST');\n" * 100
        process = SimpleNamespace(stdout=io.BytesIO(content), stderr=io.BytesIO(), wait=lambda: 0)
        target = self.directory / "auth.sql.gz"
        with patch.object(promotion.subprocess, "Popen", return_value=process) as launch:
            promotion.dump("source", ("auth",), target)
        self.assertEqual(gzip.decompress(target.read_bytes()), content)
        command = launch.call_args.args[0]
        self.assertNotIn("--all-databases", " ".join(command))
        with self.assertRaises(promotion.PromotionError):
            promotion.dump("source", ("mysql",), self.directory / "forbidden.sql.gz")

    def test_prepare_refuses_unknown_realm_inventory_before_any_service_stop(self):
        old = {service: original(service) for service in ("mysql", "authserver", "worldserver", "account-web")}
        with patch.object(promotion, "project_containers", return_value=old), \
             patch.object(promotion, "sql", return_value="1,2,3"), \
             patch.object(promotion, "run") as run:
            with self.assertRaises(promotion.PromotionError):
                promotion.prepare()
        run.assert_not_called()
        self.assertFalse(self.state.exists())

    def test_partial_cutover_requires_inspection_instead_of_reimporting_auth(self):
        promotion.private_json(self.state, {"status": "backed_up"})
        with patch.object(promotion, "project_containers") as containers, patch.object(promotion, "import_dump") as restore:
            with self.assertRaises(promotion.PromotionError):
                promotion.prepare()
        containers.assert_not_called()
        restore.assert_not_called()

    def test_prepare_refuses_nonempty_destination_after_backups_without_importing_or_removing_original(self):
        self.source_environment()
        old = {service: original(service) for service in ("mysql", "authserver", "worldserver", "account-web")}

        def commands(arguments, *args, **kwargs):
            if arguments[:3] == ["docker", "ps", "-aq"]:
                return b"authored-world"
            if arguments[:len(promotion.COMPOSE)] == promotion.COMPOSE and arguments[-3:] == ["ps", "-q", "lab-mysql"]:
                return b"authored-mysql"
            return b""

        def database(container, statement):
            if "information_schema.tables" in statement:
                return "1"  # A previous database must never be adopted as an empty destination.
            if "GROUP_CONCAT" in statement:
                return "1,2"
            if "COUNT(*) FROM auth.realmlist" in statement:
                return "1"
            if "FROM auth.realmcharacters" in statement:
                return "100\t1"
            self.fail(f"Unexpected SQL operation before refusing the existing destination: {statement}")

        def backup(container, databases, path):
            path.write_bytes(gzip.compress(b"-- protected test backup\n", mtime=0))

        with patch.object(promotion, "project_containers", return_value=old), \
             patch.object(promotion, "retire_workflow"), \
             patch.object(promotion, "prepare_graveyard"), \
             patch.object(promotion, "run", side_effect=commands) as run, \
             patch.object(promotion, "inspect", return_value=original("worldserver")), \
             patch.object(promotion, "sql", side_effect=database), \
             patch.object(promotion, "account_identity", return_value=self.acceptance_state()["account_identity"]), \
             patch.object(promotion, "auth_copy_digest", return_value="auth-digest"), \
             patch.object(promotion, "dump", side_effect=backup), \
             patch.object(promotion, "wait_healthy", return_value="destination"), \
             patch.object(promotion, "import_dump") as restore:
            with self.assertRaisesRegex(promotion.PromotionError, "not empty"):
                promotion.prepare()
        restore.assert_not_called()
        self.assertTrue(all(call.args[0][1] != "rm" for call in run.call_args_list))
        state = json.loads(self.state.read_text())
        self.assertEqual(state["status"], "backed_up")
        backup_directory = self.directory / state["backup"]
        self.assertTrue((backup_directory / "auth.sql.gz").is_file())
        self.assertTrue((backup_directory / "elwynn-world-characters.sql.gz").is_file())
        self.assertTrue((backup_directory / "authored-world-characters.sql.gz").is_file())
        self.assertEqual(state["backups"]["auth.sql.gz"],
                         hashlib.sha256((backup_directory / "auth.sql.gz").read_bytes()).hexdigest())

    def test_finalize_does_not_remove_any_original_service_before_all_healthchecks_pass(self):
        promotion.private_json(self.state, self.acceptance_state())
        with patch.object(promotion, "wait_healthy", side_effect=promotion.PromotionError("world is unhealthy")), \
             patch.object(promotion, "run") as run:
            with self.assertRaises(promotion.PromotionError):
                promotion.finalize()
        run.assert_not_called()
        self.assertEqual(json.loads(self.state.read_text())["status"], "auth_ready")

    def test_finalize_checks_original_project_label_before_removing_container(self):
        state = self.acceptance_state()
        promotion.private_json(self.state, state)
        row = original("mysql")
        row["Config"]["Labels"]["com.docker.compose.project"] = "unrelated-project"
        with patch.object(promotion, "wait_healthy", return_value="healthy"), \
             patch.object(promotion, "run", return_value=b"destination") as run, \
             patch.object(promotion, "account_identity", return_value=state["account_identity"]), \
             patch.object(promotion, "sql", return_value="2"), \
             patch.object(promotion, "inspect", return_value=row):
            with self.assertRaises(promotion.PromotionError):
                promotion.finalize()
        self.assertTrue(all(call.args[0][1] not in ("stop", "rm", "update") for call in run.call_args_list))

    def test_completed_finalization_is_repeatable_and_checks_only_original_account_prefix(self):
        state = self.acceptance_state()
        state["status"] = "complete"
        promotion.private_json(self.state, state)
        with patch.object(promotion, "wait_healthy", return_value="healthy"), \
             patch.object(promotion, "run", return_value=b"destination") as run, \
             patch.object(promotion, "account_identity", return_value=state["account_identity"]) as identity, \
             patch.object(promotion, "sql", return_value="2"), \
             patch.object(promotion, "project_containers", return_value={}), \
             patch.object(promotion, "inspect") as inspect:
            promotion.finalize()
        identity.assert_called_once_with("destination", 2)
        inspect.assert_not_called()
        self.assertTrue(all(call.args[0][1] not in ("stop", "rm", "update") for call in run.call_args_list))

    def test_partial_finalization_resumes_without_touching_an_already_removed_identity(self):
        state = self.acceptance_state()
        state["original"]["worldserver"] = "original-worldserver"
        promotion.private_json(self.state, state)

        def commands(arguments, *args, **kwargs):
            if arguments[:len(promotion.COMPOSE)] == promotion.COMPOSE:
                return b"destination"
            if arguments[:3] == ["docker", "ps", "-aq"]:
                return b"" if arguments[-1] == "id=original-mysql" else b"original-worldserver"
            return b""

        with patch.object(promotion, "wait_healthy", return_value="healthy"), \
             patch.object(promotion, "run", side_effect=commands) as run, \
             patch.object(promotion, "account_identity", return_value=state["account_identity"]), \
             patch.object(promotion, "sql", return_value="2"), \
             patch.object(promotion, "project_containers", return_value={"worldserver": original("worldserver")}), \
             patch.object(promotion, "inspect", return_value=original("worldserver")) as inspect:
            promotion.finalize()
        inspect.assert_called_once_with("original-worldserver")
        removals = [call.args[0] for call in run.call_args_list if call.args[0][:2] == ["docker", "rm"]]
        self.assertEqual(removals, [["docker", "rm", "original-worldserver"]])
        self.assertEqual(json.loads(self.state.read_text())["status"], "complete")

    def test_partial_finalization_refuses_a_replacement_original_service(self):
        state = self.acceptance_state()
        promotion.private_json(self.state, state)

        def commands(arguments, *args, **kwargs):
            return b"destination" if arguments[:len(promotion.COMPOSE)] == promotion.COMPOSE else b""

        with patch.object(promotion, "wait_healthy", return_value="healthy"), \
             patch.object(promotion, "run", side_effect=commands) as run, \
             patch.object(promotion, "account_identity", return_value=state["account_identity"]), \
             patch.object(promotion, "sql", return_value="2"), \
             patch.object(promotion, "project_containers", return_value={"mysql": original("mysql")}), \
             patch.object(promotion, "inspect") as inspect:
            with self.assertRaisesRegex(promotion.PromotionError, "replaced"):
                promotion.finalize()
        inspect.assert_not_called()
        self.assertTrue(all(call.args[0][1] not in ("stop", "rm", "update") for call in run.call_args_list))

    def test_rollback_stops_canonical_auth_before_restarting_original_world_and_restores_config(self):
        backup = self.directory / "runtime/lab/backups/test"
        backup.mkdir(parents=True)
        (backup / "canonical-before.env").write_text("PRIVATE=previous-configuration\n")
        (self.directory / ".env").write_text("PRIVATE=new-configuration\n")
        promotion.private_json(self.state, {"status": "backed_up", "backup": "runtime/lab/backups/test",
                                          "original": {"mysql": "original-mysql", "worldserver": "original-world"},
                                          "running_before": {"mysql": True, "worldserver": True},
                                          "prior_world": {"id": "authored-world", "running": True}})

        def commands(arguments, *args, **kwargs):
            if arguments[:len(promotion.COMPOSE)] == promotion.COMPOSE and arguments[-1] in ("authserver", "auth-mysql"):
                return f"canonical-{arguments[-1]}".encode()
            return b""

        with patch.object(promotion, "run", side_effect=commands) as run:
            promotion.rollback_preparation()
        operations = [call.args[0] for call in run.call_args_list]
        stop_positions = [index for index, args in enumerate(operations) if args[:2] == ["docker", "stop"]]
        start_positions = [index for index, args in enumerate(operations) if args[:2] == ["docker", "start"]]
        self.assertEqual(len(stop_positions), 2)
        self.assertEqual(len(start_positions), 3)
        self.assertLess(max(stop_positions), min(start_positions))
        self.assertEqual((self.directory / ".env").read_text(), "PRIVATE=previous-configuration\n")
        self.assertEqual(json.loads(self.state.read_text())["status"], "rolled_back")
        self.assertTrue(all(args[1] != "rm" for args in operations))

    def test_rollback_never_restarts_old_services_after_auth_acceptance(self):
        promotion.private_json(self.state, self.acceptance_state())
        with patch.object(promotion, "run") as run:
            promotion.rollback_preparation()
        run.assert_not_called()

    def test_failed_prepare_cli_attempts_rollback_and_reports_a_sanitized_error(self):
        with patch("sys.argv", ["promotion.py", "prepare"]), \
             patch.object(promotion, "prepare", side_effect=promotion.PromotionError("sensitive-account-data")), \
             patch.object(promotion, "rollback_preparation") as rollback, \
             patch("sys.stderr", new_callable=io.StringIO) as output, \
             self.assertRaises(SystemExit) as caught:
            promotion.main()
        self.assertEqual(caught.exception.code, 2)
        self.assertNotIn("sensitive-account-data", output.getvalue())
        rollback.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
