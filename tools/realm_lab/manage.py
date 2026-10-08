#!/usr/bin/env python3
"""Realm lab config and one-time setup. No third-party Python dependencies.

The lab owns world/characters on lab-mysql. Shared auth is used for DML only;
its schema is maintained by the original realm, never by lab CD.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import ipaddress
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Mapping


class SetupError(ValueError):
    pass


def value(env: Mapping[str, str], key: str, default: str | None = None) -> str:
    result = env.get(key, default)
    if not result or any(ord(c) < 32 for c in result):
        raise SetupError(f"{key} must be set without control characters")
    return result


def number(env: Mapping[str, str], key: str, default: str, low: int, high: int) -> int:
    raw = value(env, key, default)
    if not re.fullmatch(r"[0-9]+", raw) or not low <= int(raw) <= high:
        raise SetupError(f"{key} must be an integer in {low}..{high}")
    return int(raw)


def credential(env: Mapping[str, str], key: str, *, username: bool = False) -> str:
    result = value(env, key)
    if result.startswith("change-me"):
        raise SetupError(f"Replace the example value for {key}")
    if username:
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,31}", result):
            raise SetupError(f"{key} must be a simple MySQL username")
    elif any(c in result for c in ';"\\'):
        raise SetupError(f"{key} contains a character unsupported by TrinityCore connection strings")
    return result


def address(env: Mapping[str, str], key: str, default: str | None = None) -> str:
    result = value(env, key, default)
    if len(result) > 255 or not re.fullmatch(r"[A-Za-z0-9_.:-]+", result):
        raise SetupError(f"{key} must be an IP address or hostname")
    return result


@dataclass(frozen=True)
class Settings:
    realm_id: int
    name: str
    address: str
    local_address: str
    subnet: str
    port: int
    db_user: str
    db_password: str
    auth_host: str
    auth_port: int
    auth_user: str
    auth_password: str

    @classmethod
    def load(cls, env: Mapping[str, str]) -> Settings:
        name = value(env, "LAB_REALM_NAME", "AI World Lab")
        if len(name) > 32:
            raise SetupError("LAB_REALM_NAME must fit realmlist.name (32 characters)")
        subnet = value(env, "LAB_REALM_LOCAL_SUBNET_MASK", "255.255.255.0")
        try:
            ipaddress.IPv4Network(f"0.0.0.0/{subnet}")
        except ValueError as exc:
            raise SetupError("LAB_REALM_LOCAL_SUBNET_MASK must be a valid IPv4 mask") from exc
        port = number(env, "LAB_WORLD_PORT", "8086", 1, 65535)
        if port == 8085:
            raise SetupError("LAB_WORLD_PORT must differ from the original realm's 8085")
        return cls(
            number(env, "LAB_REALM_ID", "2", 2, 255), name,
            address(env, "LAB_REALM_ADDRESS"), address(env, "LAB_REALM_LOCAL_ADDRESS"),
            subnet, port, credential(env, "TC_DB_USER", username=True),
            credential(env, "TC_DB_PASSWORD"),
            address(env, "TC_AUTH_DB_HOST", "aiworld-auth-db"),
            number(env, "TC_AUTH_DB_PORT", "3306", 1, 65535),
            credential(env, "TC_AUTH_DB_USER", username=True),
            credential(env, "TC_AUTH_DB_PASSWORD"),
        )


def config_overrides(settings: Settings) -> dict[str, str]:
    auth = f"{settings.auth_host};{settings.auth_port};{settings.auth_user};{settings.auth_password};auth"
    local = f"lab-mysql;3306;{settings.db_user};{settings.db_password}"
    return {
        "RealmID": str(settings.realm_id),
        "WorldServerPort": "8085",  # External host port is advertised in realmlist.
        "LoginDatabaseInfo": f'"{auth}"',
        "WorldDatabaseInfo": f'"{local};world"',
        "CharacterDatabaseInfo": f'"{local};characters"',
        "DataDir": '"/runtime/data"',
        "LogsDir": '"/runtime/logs"',
        "SourceDirectory": '"/workspace"',
        "Updates.EnableDatabases": "6",  # WORLD=4 + CHARACTER=2; no AUTH=1.
        "AIWorld.Enable": "0",
        "AIWorld.ElwynnAlwaysActive": "0",
        "AIWorld.EnableSpawnReconciliation": "0",
        "AIWorld.EnableZoneControlActivation": "0",
        "AIWorld.TelemetryEnabled": "0",
        "AIWorld.LivingWolvesEnabled": "0",
        "AIWorld.LivingRolesEnabled": "0",
        "AIWorld.LocalRecoverySpawnId": "0",
        "AIWorld.WolfGroupAutoFormation": "0",
        "AIWorld.DefiasGroupAutoFormation": "0",
        "AIWorld.RecoveryAdviceEnabled": "0",
        "SOAP.Enabled": "0",
    }


def render_config(template: str, settings: Settings) -> str:
    overrides = config_overrides(settings)
    seen: set[str] = set()
    lines: list[str] = []
    for line in template.splitlines():
        match = re.match(r"^\s*([A-Za-z][A-Za-z0-9_.]*)\s*=", line)
        key = match.group(1) if match else None
        if key in overrides:
            if key in seen:
                raise SetupError(f"Duplicate config entry: {key}")
            seen.add(key)
            line = f"{key} = {overrides[key]}"
        lines.append(line)
    missing = overrides.keys() - seen
    if missing:
        raise SetupError(f"Base config lacks required entries: {', '.join(sorted(missing))}")
    rendered = "\n".join(lines) + "\n"
    if "__TC_DB_" in rendered:
        raise SetupError("Unresolved DB placeholder in rendered config")
    return rendered


def sql_literal(text: str) -> str:
    # Every mysql invocation sets NO_BACKSLASH_ESCAPES first.
    return "'" + text.replace("'", "''") + "'"


def mysql(settings: Settings, sql: str, *, admin: bool = False) -> str:
    env = dict(os.environ)
    user = settings.auth_user
    password = settings.auth_password
    if admin:
        user = value(env, "LAB_AUTH_ADMIN_USER", "root")
        password = value(env, "LAB_AUTH_ADMIN_PASSWORD")
    env["MYSQL_PWD"] = password
    # Credentials and SQL never go in command-line arguments or error output.
    result = subprocess.run(
        ["mysql", "--batch", "--raw", "--skip-column-names", "--default-character-set=utf8mb4",
         f"--host={settings.auth_host}", f"--port={settings.auth_port}", f"--user={user}", "auth"],
        input="SET SESSION sql_mode = 'NO_BACKSLASH_ESCAPES';\n" + sql,
        env=env, text=True, encoding="utf-8", capture_output=True, check=False,
    )
    if result.returncode:
        # mysql diagnostics can echo SQL containing passwords; suppress them.
        raise SetupError("Shared auth SQL failed; check connectivity, grants and realm ID/name conflicts")
    return result.stdout.strip()


def auth_user_sql(settings: Settings) -> str:
    account = f"{sql_literal(settings.auth_user)}@'%'"
    return f"""CREATE USER {account} IDENTIFIED BY {sql_literal(settings.auth_password)};
GRANT SELECT, INSERT, UPDATE, DELETE ON auth.* TO {account};
"""


def register_sql(settings: Settings) -> str:
    rid, name = settings.realm_id, sql_literal(settings.name)
    fields = (
        f"address={sql_literal(settings.address)}, localAddress={sql_literal(settings.local_address)}, "
        f"localSubnetMask={sql_literal(settings.subnet)}, port={settings.port}"
    )
    # Avoid ON DUPLICATE KEY UPDATE: a colliding unique name can target another
    # realm. Both insert and update are constrained to our own ID AND name.
    return f"""START TRANSACTION;
INSERT INTO realmlist (id, name, address, localAddress, localSubnetMask, port, flag, gamebuild)
SELECT {rid}, {name}, {sql_literal(settings.address)}, {sql_literal(settings.local_address)},
       {sql_literal(settings.subnet)}, {settings.port}, 2, 12340
WHERE NOT EXISTS (SELECT 1 FROM realmlist WHERE id={rid} OR name={name});
UPDATE realmlist SET {fields} WHERE id={rid} AND name={name};
INSERT IGNORE INTO realmcharacters (realmid, acctid, numchars)
SELECT {rid}, account.id, 0 FROM account
WHERE EXISTS (SELECT 1 FROM realmlist WHERE id={rid} AND name={name});
COMMIT;
SELECT id, name, address, localAddress, localSubnetMask, port FROM realmlist WHERE id={rid};
"""


def verify_registration(settings: Settings, row: str) -> None:
    expected = [str(settings.realm_id), settings.name, settings.address,
                settings.local_address, settings.subnet, str(settings.port)]
    if row.split("\t") != expected:
        raise SetupError("Realm registration read-back differs; choose an unused ID and unique name")


def preflight(data: Path, settings: Settings) -> None:
    for filename in ("Map.dbc", "AreaTable.dbc", "Faction.dbc", "FactionTemplate.dbc"):
        path = data / "dbc" / filename
        if not path.is_file() or path.stat().st_size < 20:
            raise SetupError(f"Missing/empty extracted DBC: {filename}")
        with path.open("rb") as stream:
            if stream.read(4) != b"WDBC":
                raise SetupError(f"Invalid 3.3.5 DBC signature: {filename}")
    for folder, suffix in (("maps", ".map"), ("vmaps", ".vmtree"), ("mmaps", ".mmap")):
        if not any((data / folder).glob(f"*{suffix}")):
            raise SetupError(f"No extracted/generated {suffix} files in {data / folder}")
    row = mysql(settings, f"SELECT id, name, address, localAddress, localSubnetMask, port FROM realmlist WHERE id={settings.realm_id};")
    verify_registration(settings, row)
    print("Lab data presence and shared-auth registration verified; custom geometry/AI are not certified.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    world = sub.add_parser("run-world")
    world.add_argument("template", type=Path)
    world.add_argument("binary", type=Path)
    sub.add_parser("provision-auth")
    sub.add_parser("register-realm")
    check = sub.add_parser("preflight")
    check.add_argument("data", type=Path)
    args = parser.parse_args()
    try:
        settings = Settings.load(os.environ)
        if args.command == "run-world":
            rendered = render_config(args.template.read_text(encoding="utf-8"), settings)
            dest = Path("/tmp/lab-worldserver.conf")
            fd = os.open(dest, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            os.chmod(dest, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                stream.write(rendered)
            os.execv(str(args.binary), [str(args.binary), "-c", str(dest)])
        elif args.command == "provision-auth":
            # Never adopt/modify an existing user with possibly wider grants.
            existing = mysql(settings, f"SELECT COUNT(*) FROM mysql.user WHERE User={sql_literal(settings.auth_user)};", admin=True)
            if existing != "0":
                raise SetupError("Shared-auth user already exists; choose a new lab user or inspect its grants manually")
            mysql(settings, auth_user_sql(settings), admin=True)
            mysql(settings, "SELECT 1 FROM realmlist LIMIT 1;")
            print("Dedicated lab shared-auth user created (DML on auth only).")
        elif args.command == "register-realm":
            verify_registration(settings, mysql(settings, register_sql(settings)))
            print(f"Realm {settings.realm_id} registration verified on port {settings.port}.")
        elif args.command == "preflight":
            preflight(args.data, settings)
    except (SetupError, OSError) as exc:
        print(f"Lab setup failed: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc


if __name__ == "__main__":
    main()
