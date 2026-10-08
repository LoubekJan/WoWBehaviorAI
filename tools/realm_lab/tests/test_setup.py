from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab.manage import (
    Settings, SetupError, auth_user_sql, config_overrides, preflight,
    register_sql, render_config, verify_registration,
)

ROOT = Path(__file__).resolve().parents[3]


def environment():
    return {
        "TC_DB_USER": "lab_world", "TC_DB_PASSWORD": "world-pass",
        "TC_AUTH_DB_USER": "lab_auth", "TC_AUTH_DB_PASSWORD": "auth-pass",
        "LAB_REALM_ADDRESS": "192.168.0.248",
        "LAB_REALM_LOCAL_ADDRESS": "192.168.0.248",
    }


class RealmSetupTests(unittest.TestCase):
    def setUp(self):
        self.settings = Settings.load(environment())

    def test_real_template_routes_databases_and_disables_shared_auth_migrations(self):
        rendered = render_config((ROOT / "deploy/worldserver.conf").read_text(), self.settings)
        self.assertIn('LoginDatabaseInfo = "aiworld-auth-db;3306;lab_auth;auth-pass;auth"', rendered)
        self.assertIn('WorldDatabaseInfo = "lab-mysql;3306;lab_world;world-pass;world"', rendered)
        self.assertIn('CharacterDatabaseInfo = "lab-mysql;3306;lab_world;world-pass;characters"', rendered)
        self.assertIn("RealmID = 2\n", rendered)
        self.assertIn("WorldServerPort = 8085\n", rendered)
        self.assertIn("Updates.EnableDatabases = 6\n", rendered)
        self.assertNotIn("__TC_DB_", rendered)
        self.assertIn("AIWorld.ElwynnAlwaysActive = 0\n", rendered)
        self.assertIn("AIWorld.Enable = 0\n", rendered)
        self.assertIn("AIWorld.LocalRecoverySpawnId = 0\n", rendered)

    def test_realm_one_and_original_host_port_are_rejected(self):
        for key, val in (("LAB_REALM_ID", "1"), ("LAB_WORLD_PORT", "8085"), ("LAB_REALM_ID", "-1")):
            with self.subTest(key=key, value=val), self.assertRaises(SetupError):
                Settings.load({**environment(), key: val})

    def test_invalid_credentials_do_not_appear_in_errors(self):
        for secret in ('password;auth', 'password"', "password\\", "change-me-world", "bad\nsecret"):
            with self.subTest(secret=secret), self.assertRaises(SetupError) as ctx:
                Settings.load({**environment(), "TC_DB_PASSWORD": secret})
            self.assertNotIn(secret, str(ctx.exception))

    def test_sql_and_conf_handle_quotes_without_sed_replacement(self):
        settings = Settings.load({**environment(), "TC_AUTH_DB_PASSWORD": "don't&$rewrite/", "LAB_REALM_NAME": "Jan's Lab"})
        self.assertIn("IDENTIFIED BY 'don''t&$rewrite/'", auth_user_sql(settings))
        self.assertIn("'Jan''s Lab'", register_sql(settings))
        rendered = render_config((ROOT / "deploy/worldserver.conf").read_text(), settings)
        self.assertIn("don't&$rewrite/;auth", rendered)

    def test_auth_user_has_no_ddl_or_original_realm_data_grants(self):
        sql = auth_user_sql(self.settings)
        self.assertIn("GRANT SELECT, INSERT, UPDATE, DELETE ON auth.*", sql)
        self.assertNotIn("ALL PRIVILEGES", sql)
        self.assertNotIn("world.*", sql)
        self.assertNotIn("characters.*", sql)

    def test_registration_does_not_upsert_a_colliding_unique_name(self):
        sql = register_sql(self.settings)
        self.assertNotIn("ON DUPLICATE KEY UPDATE", sql)
        self.assertIn("WHERE id=2 AND name='AI World Lab'", sql)
        self.assertIn("WHERE id=2 OR name='AI World Lab'", sql)
        self.assertIn("INSERT IGNORE INTO realmcharacters", sql)
        self.assertNotIn("DELETE", sql)

    def test_registration_requires_matching_readback(self):
        row = "2\tAI World Lab\t192.168.0.248\t192.168.0.248\t255.255.255.0\t9086"
        verify_registration(self.settings, row)
        for bad in ("", row.replace("9086", "8085"), row.replace("AI World Lab", "Other realm")):
            with self.assertRaises(SetupError):
                verify_registration(self.settings, bad)

    def test_config_drift_fails_instead_of_leaving_original_credentials(self):
        template = (ROOT / "deploy/worldserver.conf").read_text()
        for corrupted in (template.replace("Updates.EnableDatabases = 7", "# removed"), template + "\nRealmID = 1\n"):
            with self.assertRaises(SetupError):
                render_config(corrupted, self.settings)

    def test_preflight_rejects_empty_data_and_wrong_auth_registration(self):
        with tempfile.TemporaryDirectory(dir=ROOT) as tmp:
            data = Path(tmp)
            with self.assertRaises(SetupError):
                preflight(data, self.settings)
            (data / "dbc").mkdir()
            for filename in ("Map.dbc", "AreaTable.dbc", "Faction.dbc", "FactionTemplate.dbc"):
                (data / "dbc" / filename).write_bytes(b"WDBC" + b"\0" * 16)
            for directory, filename in (("maps", "0000000.map"), ("vmaps", "000.vmtree"), ("mmaps", "000.mmap")):
                (data / directory).mkdir()
                (data / directory / filename).write_bytes(b"test")
            with patch("tools.realm_lab.manage.mysql", return_value="1\tElwynn"):
                with self.assertRaises(SetupError):
                    preflight(data, self.settings)


if __name__ == "__main__":
    unittest.main()
