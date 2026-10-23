"""Canonical registration checks against the disposable CI MySQL only."""
import os
import unittest

from tools.realm_lab import manage, promotion, world_setup


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

    def cutover_fixture(self):
        manage.mysql(self.settings, """
ALTER TABLE account ADD username VARCHAR(32), ADD salt BINARY(32), ADD verifier BINARY(32), ADD expansion INT DEFAULT 1;
ALTER TABLE account_access ADD Comment VARCHAR(255), ADD PRIMARY KEY (AccountID,RealmID);
ALTER TABLE rbac_account_permissions ADD granted TINYINT DEFAULT 1, ADD PRIMARY KEY (accountId,permissionId,realmId);
INSERT INTO account(id) VALUES (102);
UPDATE account SET username=CONCAT('USER',id),salt=UNHEX(REPEAT('11',32)),verifier=UNHEX(REPEAT('22',32));
UPDATE account SET expansion=2 WHERE id=101;
UPDATE realmlist SET name='Elwynn' WHERE id=1;
INSERT INTO realmlist VALUES (2,'AI World Lab','192.168.0.248','192.168.0.248','255.255.255.0',9086,0,12340);
UPDATE realmcharacters SET numchars=3 WHERE realmid=1;
INSERT INTO realmcharacters VALUES (2,100,4),(2,101,2);
INSERT INTO account_access (AccountID,SecurityLevel,RealmID,Comment) VALUES
 (100,3,1,'old administrator'),(100,1,2,'current lab'),(101,1,1,'old moderator'),
 (102,1,1,'old player'),(102,3,2,'current administrator');
INSERT INTO rbac_account_permissions(accountId,permissionId,granted,realmId) VALUES
 (100,1,0,1),(100,2,1,1),(100,2,0,2),(100,3,1,-1);
""")

    def test_cutover_preserves_credentials_character_counts_and_permissions_without_ambiguous_columns(self):
        self.cutover_fixture()
        identity_sql = "SELECT id,username,HEX(salt),HEX(verifier) FROM account ORDER BY id;"
        before = manage.mysql(self.settings, identity_sql)
        counts = manage.mysql(self.settings, "SELECT acctid,numchars FROM realmcharacters WHERE realmid=2 ORDER BY acctid;")
        manage.mysql(self.settings, promotion.cutover_sql())
        self.assertEqual(manage.mysql(self.settings, identity_sql), before)
        self.assertEqual(manage.mysql(self.settings, "SELECT acctid,numchars FROM realmcharacters WHERE realmid=2 ORDER BY acctid;"), counts)
        self.assertEqual(manage.mysql(self.settings, "SELECT GROUP_CONCAT(id ORDER BY id) FROM realmlist;"), "2")
        self.assertEqual(manage.mysql(self.settings, "SELECT COUNT(*) FROM account WHERE expansion=2;"), "3")
        self.assertEqual(manage.mysql(self.settings, "SELECT GROUP_CONCAT(CONCAT(AccountID,':',RealmID,':',SecurityLevel) ORDER BY AccountID,RealmID) FROM account_access;"),
                         "100:-1:3,100:2:3,101:2:1,102:2:3")
        self.assertEqual(manage.mysql(self.settings, "SELECT granted FROM rbac_account_permissions WHERE accountId=100 AND permissionId=1 AND realmId=2;"), "0")
        self.assertEqual(manage.mysql(self.settings, "SELECT granted FROM rbac_account_permissions WHERE accountId=100 AND permissionId=2 AND realmId=2;"), "0")
        self.assertEqual(manage.mysql(self.settings, "SELECT COUNT(*) FROM rbac_account_permissions WHERE realmId=-1;"), "1")
        self.assertEqual(manage.mysql(self.settings, "SELECT COUNT(*) FROM realmcharacters WHERE realmid=1;"), "0")
        manage.mysql(self.settings, promotion.cutover_sql())
        self.assertEqual(manage.mysql(self.settings, identity_sql), before)

    def test_cutover_sql_error_rolls_back_prior_permission_merges(self):
        self.cutover_fixture()
        before = manage.mysql(self.settings, "SELECT * FROM account_access ORDER BY AccountID,RealmID;")
        manage.mysql(self.settings, "DROP TABLE rbac_account_permissions;")
        with self.assertRaises(manage.SetupError):
            manage.mysql(self.settings, promotion.cutover_sql())
        self.assertEqual(manage.mysql(self.settings, "SELECT * FROM account_access ORDER BY AccountID,RealmID;"), before)
        self.assertEqual(manage.mysql(self.settings, "SELECT GROUP_CONCAT(id ORDER BY id) FROM realmlist;"), "1,2")
        self.assertEqual(manage.mysql(self.settings, "SELECT COUNT(*) FROM account WHERE expansion=1;"), "2")


if __name__ == "__main__":
    unittest.main()
