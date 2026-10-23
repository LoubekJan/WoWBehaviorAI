"""Canonical registration checks against the disposable CI MySQL only."""
import os
import unittest

from tools.realm_lab import manage, world_setup


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class CanonicalRegistrationMySQLTests(unittest.TestCase):
    def setUp(self):
        self.settings = manage.Settings.load({
            "TC_DB_USER": "lab_world", "TC_DB_PASSWORD": "lab-world-pass",
            "TC_AUTH_DB_HOST": "127.0.0.1", "TC_AUTH_DB_PORT": "3306",
            "TC_AUTH_DB_USER": "root", "TC_AUTH_DB_PASSWORD": os.environ["LAB_TEST_MYSQL_ROOT_PASSWORD"],
            "LAB_REALM_ADDRESS": "192.168.0.248", "LAB_REALM_LOCAL_ADDRESS": "192.168.0.248",
            "LAB_AI_PROFILE": "settlement-v8",
        })
        manage.mysql(self.settings, """
DROP TABLE IF EXISTS realmcharacters, realmlist, account_access, rbac_account_permissions, uptime, account;
CREATE TABLE realmlist (
 id INT PRIMARY KEY, name VARCHAR(32) UNIQUE, address VARCHAR(255), localAddress VARCHAR(255),
 localSubnetMask VARCHAR(255), port INT, flag INT, gamebuild INT
) ENGINE=InnoDB;
CREATE TABLE account (id INT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE realmcharacters (realmid INT, acctid INT, numchars INT, PRIMARY KEY(realmid,acctid)) ENGINE=InnoDB;
CREATE TABLE account_access (AccountID INT, SecurityLevel INT, RealmID INT) ENGINE=InnoDB;
CREATE TABLE rbac_account_permissions (accountId INT, permissionId INT, realmId INT) ENGINE=InnoDB;
CREATE TABLE uptime (realmid INT, starttime INT) ENGINE=InnoDB;
INSERT INTO account VALUES (100),(101);
INSERT INTO realmlist VALUES (1,'Trinity','127.0.0.1','127.0.0.1','255.255.255.0',8085,2,12340);
INSERT INTO realmcharacters VALUES (1,100,0);
INSERT INTO account_access VALUES (100,3,-1);
""")

    def test_fresh_registration_removes_placeholder_preserves_accounts_and_is_repeatable(self):
        world_setup.register_world(self.settings)
        self.assertEqual(manage.mysql(self.settings, "SELECT GROUP_CONCAT(id ORDER BY id) FROM realmlist;"), "2")
        self.assertEqual(manage.mysql(self.settings, "SELECT COUNT(*) FROM realmcharacters WHERE realmid=1;"), "0")
        self.assertEqual(manage.mysql(self.settings, "SELECT COUNT(*) FROM realmcharacters WHERE realmid=2;"), "2")
        self.assertEqual(manage.mysql(self.settings, "SELECT COUNT(*) FROM account;"), "2")
        self.assertEqual(manage.mysql(self.settings, "SELECT COUNT(*) FROM account_access WHERE RealmID=-1;"), "1")
        manage.mysql(self.settings, "UPDATE realmcharacters SET numchars=3 WHERE realmid=2 AND acctid=100;")
        world_setup.register_world(self.settings)
        self.assertEqual(manage.mysql(self.settings, "SELECT numchars FROM realmcharacters WHERE realmid=2 AND acctid=100;"), "3")

    def test_existing_named_realm_is_not_removed(self):
        manage.mysql(self.settings, "UPDATE realmlist SET name='Elwynn' WHERE id=1;")
        with self.assertRaises(manage.SetupError):
            world_setup.register_world(self.settings)
        self.assertEqual(manage.mysql(self.settings, "SELECT name FROM realmlist WHERE id=1;"), "Elwynn")

    def test_default_with_characters_is_not_removed(self):
        manage.mysql(self.settings, "UPDATE realmcharacters SET numchars=1 WHERE realmid=1;")
        with self.assertRaises(manage.SetupError):
            world_setup.register_world(self.settings)
        self.assertEqual(manage.mysql(self.settings, "SELECT numchars FROM realmcharacters WHERE realmid=1;"), "1")

    def test_default_with_realm_permissions_or_world_history_is_not_removed(self):
        for insert, cleanup in (
            ("INSERT INTO account_access VALUES (101,3,1)", "DELETE FROM account_access WHERE RealmID=1"),
            ("INSERT INTO rbac_account_permissions VALUES (100,1,1)", "DELETE FROM rbac_account_permissions"),
            ("INSERT INTO uptime VALUES (1,1)", "DELETE FROM uptime"),
        ):
            manage.mysql(self.settings, insert + ";")
            with self.subTest(insert=insert), self.assertRaises(manage.SetupError):
                world_setup.register_world(self.settings)
            self.assertEqual(manage.mysql(self.settings, "SELECT COUNT(*) FROM realmlist WHERE id=1;"), "1")
            manage.mysql(self.settings, cleanup + ";")


if __name__ == "__main__":
    unittest.main()
