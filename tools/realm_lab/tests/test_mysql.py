"""Real MySQL collision/grant tests, enabled only against disposable CI MySQL."""
from dataclasses import replace
import os
import unittest

from tools.realm_lab.manage import (
    Settings, SetupError, auth_user_sql, mysql, register_sql, verify_registration,
)


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class MySQLIntegrationTests(unittest.TestCase):
    def setUp(self):
        # Only CI enables this class; never point these fixtures at a live auth.
        self.admin = Settings.load({
            "TC_DB_USER": "lab_world", "TC_DB_PASSWORD": "lab-world-pass",
            "TC_AUTH_DB_HOST": "127.0.0.1", "TC_AUTH_DB_PORT": "3306",
            "TC_AUTH_DB_USER": "root",
            "TC_AUTH_DB_PASSWORD": os.environ["LAB_TEST_MYSQL_ROOT_PASSWORD"],
            "LAB_REALM_ADDRESS": "192.168.0.248",
            "LAB_REALM_LOCAL_ADDRESS": "192.168.0.248",
        })
        mysql(self.admin, """
DROP TABLE IF EXISTS realmcharacters, realmlist, account;
CREATE TABLE realmlist (
  id INT PRIMARY KEY, name VARCHAR(32) UNIQUE, address VARCHAR(255),
  localAddress VARCHAR(255), localSubnetMask VARCHAR(255), port INT,
  flag INT, gamebuild INT
) ENGINE=InnoDB;
CREATE TABLE account (id INT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE realmcharacters (
  realmid INT, acctid INT, numchars INT, PRIMARY KEY (realmid, acctid)
) ENGINE=InnoDB;
INSERT INTO account VALUES (100), (101);
INSERT INTO realmlist VALUES (1, 'Elwynn', 'original', 'original', '255.255.255.0', 8085, 0, 12340);
INSERT INTO realmcharacters VALUES (1, 100, 3);
""")
        self.original = mysql(self.admin, "SELECT * FROM realmlist WHERE id=1;")

    def assert_original_unchanged(self):
        self.assertEqual(mysql(self.admin, "SELECT * FROM realmlist WHERE id=1;"), self.original)
        self.assertEqual(mysql(self.admin, "SELECT numchars FROM realmcharacters WHERE realmid=1 AND acctid=100;"), "3")

    def test_repeat_registration_keeps_original_realm_and_character_counts(self):
        verify_registration(self.admin, mysql(self.admin, register_sql(self.admin)))
        mysql(self.admin, "UPDATE realmcharacters SET numchars=4 WHERE realmid=2 AND acctid=100;")
        verify_registration(self.admin, mysql(self.admin, register_sql(self.admin)))
        self.assertEqual(mysql(self.admin, "SELECT numchars FROM realmcharacters WHERE realmid=2 AND acctid=100;"), "4")
        self.assertEqual(mysql(self.admin, "SELECT COUNT(*) FROM realmcharacters WHERE realmid=2;"), "2")
        self.assert_original_unchanged()

    def test_id_collision_does_not_modify_another_realm(self):
        mysql(self.admin, "INSERT INTO realmlist VALUES (2, 'Existing realm', 'other', 'other', '255.255.255.0', 9000, 0, 12340);")
        before = mysql(self.admin, "SELECT * FROM realmlist WHERE id=2;")
        with self.assertRaises(SetupError):
            verify_registration(self.admin, mysql(self.admin, register_sql(self.admin)))
        self.assertEqual(mysql(self.admin, "SELECT * FROM realmlist WHERE id=2;"), before)
        self.assertEqual(mysql(self.admin, "SELECT COUNT(*) FROM realmcharacters WHERE realmid=2;"), "0")
        self.assert_original_unchanged()

    def test_name_collision_does_not_retarget_a_different_realm(self):
        mysql(self.admin, "INSERT INTO realmlist VALUES (3, 'AI World Lab', 'other', 'other', '255.255.255.0', 9000, 0, 12340);")
        before = mysql(self.admin, "SELECT * FROM realmlist WHERE id=3;")
        with self.assertRaises(SetupError):
            verify_registration(self.admin, mysql(self.admin, register_sql(self.admin)))
        self.assertEqual(mysql(self.admin, "SELECT * FROM realmlist WHERE id=3;"), before)
        self.assert_original_unchanged()

    def test_shared_auth_user_can_register_but_cannot_run_ddl_or_access_world(self):
        mysql(self.admin, """DROP USER IF EXISTS 'realm_lab_ci_auth'@'%';
CREATE DATABASE IF NOT EXISTS world;
CREATE TABLE IF NOT EXISTS world.some_table (id INT PRIMARY KEY);
INSERT IGNORE INTO world.some_table VALUES (1);
""")
        lab = replace(self.admin, auth_user="realm_lab_ci_auth", auth_password="quote'and&dollar$")
        mysql(self.admin, auth_user_sql(lab))
        verify_registration(lab, mysql(lab, register_sql(lab)))
        with self.assertRaises(SetupError):
            mysql(lab, "CREATE TABLE forbidden_lab_auth_migration (id INT);")
        with self.assertRaises(SetupError):
            mysql(lab, "SELECT * FROM world.some_table;")
        self.assert_original_unchanged()


if __name__ == "__main__":
    unittest.main()
