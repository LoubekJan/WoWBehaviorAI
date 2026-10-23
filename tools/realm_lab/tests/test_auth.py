from contextlib import redirect_stderr
from io import StringIO
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab.auth import AuthSettings, main, render_auth_config, run_auth
from tools.realm_lab.manage import SetupError


ROOT = Path(__file__).resolve().parents[3]


def environment():
    return {"TC_AUTH_SERVER_USER": "world_auth", "TC_AUTH_SERVER_PASSWORD": "owner-pass"}


class AuthSetupTests(unittest.TestCase):
    def test_real_template_uses_private_auth_endpoint_and_schema_owner(self):
        rendered = render_auth_config((ROOT / "deploy/authserver.conf").read_text(), AuthSettings.load(environment()))
        self.assertIn('LoginDatabaseInfo = "aiworld-auth-db;3306;world_auth;owner-pass;auth"\n', rendered)
        self.assertIn('LogsDir = "/runtime/logs/auth"\n', rendered)
        self.assertIn('SourceDirectory = "/workspace"\n', rendered)
        self.assertIn("Updates.EnableDatabases = 1\n", rendered)
        self.assertNotIn("__TC_", rendered)
        self.assertNotIn(";world\"", rendered)
        self.assertNotIn(";characters\"", rendered)

    def test_missing_or_invalid_credentials_fail_without_printing_secret(self):
        for secret in ("", "change-me-owner", "secret;auth", 'secret"', "secret\\", "secret\n"):
            with self.subTest(secret=secret), self.assertRaises(SetupError) as caught:
                AuthSettings.load({**environment(), "TC_AUTH_SERVER_PASSWORD": secret})
            if secret:
                self.assertNotIn(secret, str(caught.exception))
        with self.assertRaises(SetupError):
            AuthSettings.load({"TC_AUTH_DB_USER": "world_dml", "TC_AUTH_DB_PASSWORD": "not-an-owner"})

    def test_connection_has_explicit_validated_endpoint_and_preserves_literal_password(self):
        settings = AuthSettings.load({**environment(), "TC_AUTH_DB_HOST": "auth.example.local",
                                      "TC_AUTH_DB_PORT": "3307", "TC_AUTH_SERVER_PASSWORD": "don't&$rewrite/"})
        rendered = render_auth_config((ROOT / "deploy/authserver.conf").read_text(), settings)
        self.assertIn("auth.example.local;3307;world_auth;don't&$rewrite/;auth", rendered)
        for key, bad in (("TC_AUTH_DB_HOST", "bad host"), ("TC_AUTH_DB_PORT", "0"),
                         ("TC_AUTH_DB_PORT", "65536"), ("TC_AUTH_SERVER_USER", "owner';DROP")):
            with self.subTest(key=key), self.assertRaises(SetupError):
                AuthSettings.load({**environment(), key: bad})

    def test_template_drift_fails_closed(self):
        template = (ROOT / "deploy/authserver.conf").read_text()
        for corrupted in (template.replace("Updates.EnableDatabases = 1", "# missing"),
                          template + "\nLoginDatabaseInfo = \"other\"\n",
                          template + "\nUnexpected = __TC_UNKNOWN__\n"):
            with self.subTest(), self.assertRaises(SetupError):
                render_auth_config(corrupted, AuthSettings.load(environment()))

    def test_run_passes_config_path_to_binary_and_never_uses_shell(self):
        # Substitute only the fixed /tmp target so this test works on Windows.
        with tempfile.TemporaryDirectory(dir=ROOT) as temporary:
            target = Path(temporary) / "world-authserver.conf"
            original_path = Path
            with patch("tools.realm_lab.auth.Path", side_effect=lambda path: target if str(path) == "/tmp/world-authserver.conf" else original_path(path)), \
                 patch("tools.realm_lab.auth.os.execv") as execute:
                run_auth(ROOT / "deploy/authserver.conf", Path("/build/bin/authserver"), environment())
            execute.assert_called_once_with(str(Path("/build/bin/authserver")),
                                            [str(Path("/build/bin/authserver")), "-c", str(target)])
            self.assertEqual(target.read_text(), render_auth_config((ROOT / "deploy/authserver.conf").read_text(), AuthSettings.load(environment())))

    def test_cli_does_not_emit_os_error_details(self):
        output = StringIO()
        with patch("sys.argv", ["auth.py", "run-auth", "template", "binary"]), \
             patch("tools.realm_lab.auth.run_auth", side_effect=OSError("private rendered password")), \
             redirect_stderr(output), self.assertRaises(SystemExit) as caught:
            main()
        self.assertEqual(caught.exception.code, 2)
        self.assertNotIn("private rendered password", output.getvalue())

    def test_init_is_auth_only_and_separates_schema_and_dml_privileges(self):
        script = (ROOT / "deploy/auth/mysql/01-init-users.sh").read_text()
        self.assertIn("CREATE DATABASE IF NOT EXISTS auth", script)
        self.assertIn("GRANT ALL PRIVILEGES ON auth.* TO '${TC_AUTH_SERVER_USER}'", script)
        self.assertIn("GRANT SELECT, INSERT, UPDATE, DELETE ON auth.* TO '${TC_AUTH_DB_USER}'", script)
        self.assertIn("NO_BACKSLASH_ESCAPES", script)
        self.assertNotIn("world.*", script)
        self.assertNotIn("characters.*", script)
        self.assertNotIn("CREATE DATABASE IF NOT EXISTS world", script)
        self.assertNotIn("GRANT OPTION", script)

    @unittest.skipIf(os.name == "nt", "Execute init shell contract on the Linux CI host")
    def test_init_shell_escapes_literal_passwords_and_rejects_bad_user_before_mysql(self):
        with tempfile.TemporaryDirectory(dir=ROOT) as temporary:
            directory = Path(temporary)
            client = directory / "mysql"
            client.write_text('#!/bin/sh\ncat > "$AUTH_SQL_CAPTURE"\n')
            client.chmod(0o700)
            captured = directory / "captured.sql"
            env = {**os.environ, "PATH": f"{directory}:{os.environ['PATH']}", "AUTH_SQL_CAPTURE": str(captured),
                   "MYSQL_ROOT_PASSWORD": "test-root", "TC_AUTH_SERVER_USER": "test_owner",
                   "TC_AUTH_SERVER_PASSWORD": "don't&$rewrite/", "TC_AUTH_DB_USER": "test_world",
                   "TC_AUTH_DB_PASSWORD": "world'pass"}
            result = subprocess.run(["bash", str(ROOT / "deploy/auth/mysql/01-init-users.sh")],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            sql = captured.read_text()
            self.assertIn("IDENTIFIED BY 'don''t&$rewrite/'", sql)
            self.assertIn("IDENTIFIED BY 'world''pass'", sql)
            self.assertNotIn("test-root", sql)
            captured.unlink()
            env["TC_AUTH_DB_USER"] = "bad';DROP"
            result = subprocess.run(["bash", str(ROOT / "deploy/auth/mysql/01-init-users.sh")],
                                    env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(captured.exists())
            self.assertNotIn(env["TC_AUTH_SERVER_PASSWORD"], result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
