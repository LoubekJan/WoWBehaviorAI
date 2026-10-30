#!/usr/bin/env python3
"""One-time, backed-up retirement of the original Elwynn realm.

Run on the trusted deployment host. Auth is copied, never recreated from
accounts supplied by a client. Original containers remain available until
the canonical stack passes its post-deploy acceptance checks.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
STATE = ROOT / "runtime/lab/receipts/world-promotion.json"
OLD_PROJECT = "wowbehaviorai"
COMPOSE = ["docker", "compose", "--env-file", ".env", "-f", "compose.yml"]
OLD_SERVICES = {"worldserver", "authserver", "account-web", "world-viewer", "ai-server", "aiworld-recorder", "mysql"}


class PromotionError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PromotionError(message)


def run(args: list[str], data: bytes | None = None, *, timeout: int = 120) -> bytes:
    result = subprocess.run(args, cwd=ROOT, input=data, capture_output=True, timeout=timeout)
    require(result.returncode == 0, f"Deployment operation failed: {args[0]} {args[1] if len(args) > 1 else ''}")
    return result.stdout


def inspect(container: str) -> dict:
    return json.loads(run(["docker", "inspect", container]))[0]


def project_containers(project: str) -> dict[str, dict]:
    ids = run(["docker", "ps", "-aq", "--filter", f"label=com.docker.compose.project={project}"]).decode().split()
    result = {}
    for identity in ids:
        row = inspect(identity)
        service = row["Config"]["Labels"].get("com.docker.compose.service")
        if service in OLD_SERVICES:
            require(service not in result, "Ambiguous original realm service inventory")
            result[service] = row
    return result


def environment(container: dict) -> dict[str, str]:
    return dict(value.split("=", 1) for value in container["Config"]["Env"] if "=" in value)


def read_env(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.lstrip().startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            value = value.strip()
            if value.startswith(("'", '"')):
                require(len(value) >= 2 and value[-1] == value[0], "Unclosed dotenv quotation")
                value = value[1:-1]
            values[key.strip()] = value
    return values


def private_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.chmod(path, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def sql(container: str, statement: str) -> str:
    # Password is read inside the database container, never put in argv/logs.
    program = 'export MYSQL_PWD="$MYSQL_ROOT_PASSWORD"; exec mysql -uroot --batch --raw --skip-column-names --default-character-set=utf8mb4'
    return run(["docker", "exec", "-i", container, "sh", "-ec", program], statement.encode()).decode().strip()


def account_identity(container: str, maximum_id: int | None = None) -> dict:
    # Only an aggregate digest leaves this function; account names and SRP6
    # material never enter deployment logs or the public acceptance report.
    where = f" WHERE id<={int(maximum_id)}" if maximum_id is not None else ""
    raw = sql(container, f"SELECT id,username,HEX(salt),HEX(verifier) FROM auth.account{where} ORDER BY id;")
    maximum = max((int(row.split("\t", 1)[0]) for row in raw.splitlines()), default=0)
    return {"count": len(raw.splitlines()) if raw else 0, "maximum_id": maximum,
            "identity_sha256": hashlib.sha256(raw.encode()).hexdigest()}


def dump(container: str, databases: tuple[str, ...], target: Path) -> None:
    require(all(name in ("auth", "world", "characters") for name in databases), "Unexpected backup database")
    program = ('export MYSQL_PWD="$MYSQL_ROOT_PASSWORD"; exec mysqldump -uroot --single-transaction '
               '--routines --events --triggers --hex-blob --set-gtid-purged=OFF --databases ' + " ".join(databases))
    descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        process = subprocess.Popen(["docker", "exec", container, "sh", "-ec", program], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        with gzip.GzipFile(fileobj=output, mode="wb", mtime=0) as archive:
            while block := process.stdout.read(1024 * 1024):
                archive.write(block)
        process.stdout.close()
        diagnostics = process.stderr.read()
        process.stderr.close()
        require(process.wait() == 0 and not diagnostics, "Private database backup failed")
    require(target.stat().st_size > 50, "Empty database backup")
    with gzip.open(target, "rb") as stream:
        while stream.read(1024 * 1024):
            pass


def import_dump(container: str, source: Path) -> None:
    program = 'export MYSQL_PWD="$MYSQL_ROOT_PASSWORD"; exec mysql -uroot --default-character-set=utf8mb4'
    with gzip.open(source, "rb") as stream:
        process = subprocess.Popen(["docker", "exec", "-i", container, "sh", "-ec", program], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        while block := stream.read(1024 * 1024):
            process.stdin.write(block)
        process.stdin.close()
        require(process.wait(timeout=120) == 0, "Auth import failed; original database backup retained")


def auth_copy_digest(container: str) -> str:
    program = ('export MYSQL_PWD="$MYSQL_ROOT_PASSWORD"; exec mysqldump -uroot --no-create-info '
               '--skip-extended-insert --order-by-primary --compact --skip-comments '
               '--skip-add-locks --skip-disable-keys --skip-lock-tables --hex-blob auth')
    return hashlib.sha256(run(["docker", "exec", container, "sh", "-ec", program])).hexdigest()


def cutover_sql() -> str:
    return """SET SESSION sql_mode='NO_BACKSLASH_ESCAPES';
START TRANSACTION;
UPDATE auth.account_access AS destination
INNER JOIN auth.account_access AS source ON source.AccountID=destination.AccountID AND source.RealmID=1
SET destination.SecurityLevel=GREATEST(destination.SecurityLevel,source.SecurityLevel)
WHERE destination.RealmID=2;
INSERT INTO auth.account_access (AccountID,SecurityLevel,RealmID,Comment)
SELECT source.AccountID,source.SecurityLevel,2,source.Comment FROM auth.account_access AS source
LEFT JOIN auth.account_access AS destination ON destination.AccountID=source.AccountID AND destination.RealmID=2
WHERE source.RealmID=1 AND destination.AccountID IS NULL;
INSERT IGNORE INTO auth.rbac_account_permissions (`accountId`,`permissionId`,`granted`,`realmId`)
SELECT source.`accountId`,source.`permissionId`,source.`granted`,2
FROM auth.rbac_account_permissions AS source WHERE source.`realmId`=1;
DELETE FROM auth.account_access WHERE RealmID=1;
DELETE FROM auth.rbac_account_permissions WHERE realmId=1;
DELETE FROM auth.realmcharacters WHERE realmid=1;
DELETE FROM auth.realmlist WHERE id=1;
UPDATE auth.account SET expansion=2 WHERE expansion<2;
COMMIT;
"""


def wait_healthy(service: str) -> str:
    for attempt in range(60):
        cid = run(COMPOSE + ["ps", "-q", service]).decode().strip()
        if cid:
            row = inspect(cid)
            if row["State"].get("Health", {}).get("Status") == "healthy":
                return cid
            require(row["State"]["Running"], f"Service stopped during promotion: {service}")
        time.sleep(2)
    raise PromotionError(f"Service did not become healthy: {service}")


def configure_env(old: dict[str, dict]) -> None:
    source = ROOT / "deploy/lab/.env"
    target = ROOT / ".env"
    require(source.is_file(), "Existing authored-world configuration is missing")
    values = read_env(target if target.exists() else source)
    require(values.get("LAB_REALM_ID", "2") == "2" and values.get("LAB_AI_PROFILE") in ("settlement-v8", "wildlife-v8", "supply-v8", "consumers-v8", "raids-v8", "fields-v8", "defias-v8", "full-v8"),
            "Only the verified authored realm 2 can be promoted")
    values.setdefault("AUTH_MYSQL_ROOT_PASSWORD", secrets.token_urlsafe(32))
    values.setdefault("TC_AUTH_SERVER_USER", "world_auth")
    values.setdefault("TC_AUTH_SERVER_PASSWORD", secrets.token_urlsafe(32))
    values.update(TC_AUTH_DB_HOST="aiworld-auth-db", LAB_SOAP_ENABLED="1", AUTHSERVER_PORT="3724", ACCOUNT_WEB_PORT="9088")
    if "account-web" in old:
        prior = environment(old["account-web"])
        for key in ("TC_SOAP_USER", "TC_SOAP_PASSWORD"):
            require(bool(prior.get(key)), "Original account management credential is not configured")
            if not values.get(key):
                values[key] = prior[key]
    require(all("\n" not in value and "\r" not in value and "#" not in value and "$" not in value
                for value in values.values()), "Environment contains values requiring explicit dotenv quoting")
    descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.chmod(target, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        for key, value in values.items():
            stream.write(f"{key}={value}\n")
    logs = ROOT / "runtime/lab/logs/auth"
    logs.mkdir(parents=True, exist_ok=True)
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        owner = source.stat()
        os.chown(target, owner.st_uid, owner.st_gid)
        os.chown(logs, owner.st_uid, owner.st_gid)


def retire_workflow() -> None:
    """Prevent an old-branch push from recreating the removed world."""
    runner = inspect("runner-runner-1")
    labels = runner["Config"]["Labels"]
    require(labels.get("com.docker.compose.service") == "runner", "Unexpected legacy deploy runner")
    token = environment(runner).get("ACCESS_TOKEN")
    require(bool(token), "Legacy workflow management credential is unavailable")
    url = "https://api.github.com/repos/LoubekJan/WoWBehaviorAI/actions/workflows/ci.yml/disable"
    # The owner may already have disabled it using their normal Git login.
    # Public metadata can verify that state without broadening the runner PAT.
    status_request = urllib.request.Request(url.removesuffix("/disable") + f"?audit={time.time_ns()}",
                                            headers={"User-Agent": "aiworld-retirement"})
    with urllib.request.urlopen(status_request, timeout=30) as response:
        workflow = json.load(response)
    if workflow.get("state") != "disabled_manually":
        request = urllib.request.Request(url, method="PUT", headers={
            "Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
            "User-Agent": "aiworld-retirement", "X-GitHub-Api-Version": "2022-11-28"})
        with urllib.request.urlopen(request, timeout=30) as response:
            require(response.status == 204, "Original deployment workflow was not disabled")
    run(["docker", "update", "--restart=no", runner["Id"]])
    if runner["State"]["Running"]:
        run(["docker", "stop", "--time", "30", runner["Id"]])


def prepare_graveyard() -> None:
    try:
        from .graveyard import build_server
    except ImportError:
        from graveyard import build_server
    data = ROOT / "runtime/lab/data"
    base = ROOT / "runtime/lab/dbc-base/WorldSafeLocs.dbc"
    expected = "9a6e6280ae4d0c226b06df3bee1a4aa90d84a2a029bf2175b8a0b5520a9b89ef"
    if not base.exists():
        raw = (data / "dbc/WorldSafeLocs.dbc").read_bytes()
        require(hashlib.sha256(raw).hexdigest() == expected,
                "Server graveyard base differs from the verified client; provide matching immutable base")
        base.parent.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(base, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(raw)
    require(hashlib.sha256(base.read_bytes()).hexdigest() == expected, "Unexpected graveyard base")
    build_server(data, base, ROOT / "runtime/lab/client-patch/patch-X.MPQ")


def adopt_canonical_installation() -> None:
    """Recognize a completed fresh installation without importing any database."""
    try:
        from . import full_population, defias_population, field_population, raid_population, consumer_population, manage, settlement_population, single_return, supply_population, wildlife_population
    except ImportError:
        import full_population, defias_population, field_population, raid_population, consumer_population, manage, settlement_population, single_return, supply_population, wildlife_population
    configuration = ROOT / ".env"
    require(configuration.is_file(), "No original realm exists and canonical credentials are missing")
    try:
        settings = manage.Settings.load(read_env(configuration))
        require(settings.realm_id == 2 and settings.ai_profile in (settlement_population.PROFILE, wildlife_population.PROFILE, supply_population.PROFILE, consumer_population.PROFILE, raid_population.PROFILE, field_population.PROFILE, defias_population.PROFILE, full_population.PROFILE) and
                settings.auth_host == "aiworld-auth-db" and settings.auth_port == 3306,
                "Only the canonical realm 2 settlement with its local auth may be adopted")
        databases = {}
        for service, volume in (("auth-mysql", "aitc_auth_mysql-data"), ("lab-mysql", "aitc_lab_mysql-data")):
            identities = run(COMPOSE + ["ps", "-q", service]).decode().split()
            require(len(identities) == 1, "Canonical database container is missing or ambiguous")
            container = inspect(identities[0])
            labels = container["Config"]["Labels"]
            mounts = [mount for mount in container["Mounts"] if mount.get("Destination") == "/var/lib/mysql"]
            require(container["State"]["Running"] and labels.get("com.docker.compose.project") == "aitc-lab" and
                    labels.get("com.docker.compose.service") == service and len(mounts) == 1 and
                    mounts[0].get("Type") == "volume" and mounts[0].get("Name") == volume,
                    "Canonical database identity or named volume differs from the reviewed installation")
            databases[service] = identities[0]
        auth = databases["auth-mysql"]
        require(sql(auth, "SELECT GROUP_CONCAT(id ORDER BY id) FROM auth.realmlist;") == "2",
                "Fresh installation must contain only canonical realm 2")
        profile = {wildlife_population.PROFILE: wildlife_population, supply_population.PROFILE: supply_population,
                   consumer_population.PROFILE: consumer_population, raid_population.PROFILE: raid_population, field_population.PROFILE: field_population, defias_population.PROFILE: defias_population, full_population.PROFILE: full_population}.get(settings.ai_profile, settlement_population)
        population = profile.verify_data(ROOT / "runtime/lab/data")
        rows = single_return.parse_snapshot(sql(databases["lab-mysql"], profile.snapshot_sql(population)))
        profile.validate_snapshot(rows, population)
    except (manage.SetupError, settlement_population.ProfileError, OSError, ValueError, KeyError, TypeError) as exc:
        raise PromotionError("Fresh canonical installation did not pass configuration, database and native-population checks") from exc
    managed_count = 0 if profile is settlement_population else len(profile.SPAWN_IDS)
    state = {"schema_version": 1, "status": "auth_ready", "mode": "adopted_canonical_installation",
             "original": {}, "running_before": {}, "backups": {},
             "account_identity": account_identity(auth),
             "native_spawn_count": 100 - managed_count,
             "managed_spawn_count": managed_count,
             "native_files_verified": len(population.geometry_files),
             "prepared_utc": datetime.now(timezone.utc).isoformat()}
    private_json(STATE, state)
    print("Existing canonical realm 2 installation verified; no database was imported or reset.")


def prepare() -> None:
    if STATE.exists():
        state = json.loads(STATE.read_text())
        if state["status"] in ("auth_ready", "complete"):
            require((ROOT / ".env").is_file(), "Canonical credentials missing after promotion")
            print("Canonical auth already prepared; original realm remains retired.")
            return
        raise PromotionError("Previous cutover requires inspection; protected backup and original containers retained")
    old = project_containers(OLD_PROJECT)
    if not old:
        adopt_canonical_installation()
        return
    require({"mysql", "authserver", "worldserver", "account-web"}.issubset(old), "Original realm inventory is incomplete")
    source_mysql = old["mysql"]["Id"]
    if not old["mysql"]["State"]["Running"]:
        run(["docker", "start", source_mysql])
        for attempt in range(30):
            if inspect(source_mysql)["State"].get("Health", {}).get("Status") == "healthy":
                break
            time.sleep(2)
        else:
            raise PromotionError("Original database did not become available for backup")
    require(sql(source_mysql, "SELECT GROUP_CONCAT(id ORDER BY id) FROM auth.realmlist;") == "1,2",
            "Unexpected realm inventory; refusing automatic retirement")
    require(sql(source_mysql, "SELECT COUNT(*) FROM auth.realmlist WHERE id=1 AND port=8085;") == "1",
            "Original realm identity differs from the reviewed deployment")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    backup = ROOT / f"runtime/lab/backups/retired-elwynn-{stamp}-{secrets.token_hex(4)}"
    backup.mkdir(parents=True, mode=0o700)
    retire_workflow()
    # Record any existing canonical configuration before replacing it.
    if (ROOT / ".env").exists():
        private = backup / "canonical-before.env"
        private.write_bytes((ROOT / ".env").read_bytes())
        private.chmod(0o600)
    configure_env(old)
    run(COMPOSE + ["config", "--quiet"])
    state = {"schema_version": 1, "status": "freezing", "backup": str(backup.relative_to(ROOT)),
             "original": {key: value["Id"] for key, value in old.items()}, "old_world_volume": "aitc_mysql-data"}
    state["running_before"] = {key: value["State"]["Running"] for key, value in old.items()}
    private_json(STATE, state)
    # Stop writers before the frozen backup. Never remove a container here.
    for service in ("account-web", "worldserver", "authserver", "aiworld-recorder", "ai-server", "world-viewer"):
        if service in old:
            run(["docker", "stop", "--time", "30", old[service]["Id"]])
    # Existing world container retains its old configuration until CD recreates it.
    prior_world = run(["docker", "ps", "-aq", "--filter", "label=com.docker.compose.project=aitc-lab",
                       "--filter", "label=com.docker.compose.service=worldserver"]).decode().split()
    for cid in prior_world:
        prior = inspect(cid)
        state["prior_world"] = {"id": cid, "running": prior["State"]["Running"],
                                "address": prior["NetworkSettings"]["Networks"].get("aitc-lab_default", {}).get("IPAddress", "")}
        private_json(STATE, state)
        run(["docker", "stop", "--time", "30", cid])
    state["account_identity"] = account_identity(source_mysql)
    state["auth_copy_sha256"] = auth_copy_digest(source_mysql)
    state["realm2_characters"] = sql(source_mysql, "SELECT acctid,numchars FROM auth.realmcharacters WHERE realmid=2 ORDER BY acctid;")
    dump(source_mysql, ("auth",), backup / "auth.sql.gz")
    dump(source_mysql, ("world", "characters"), backup / "elwynn-world-characters.sql.gz")
    lab_mysql = run(COMPOSE + ["ps", "-q", "lab-mysql"]).decode().strip()
    require(bool(lab_mysql), "Existing authored-world database is missing")
    dump(lab_mysql, ("world", "characters"), backup / "authored-world-characters.sql.gz")
    for filename in (".env", "deploy/lab/.env"):
        content = (ROOT / filename).read_bytes()
        target = backup / ("canonical.env" if filename == ".env" else "previous-lab.env")
        descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
    state["backups"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in backup.iterdir() if path.is_file()}
    state["status"] = "backed_up"
    private_json(STATE, state)
    prepare_graveyard()
    run(COMPOSE + ["up", "-d", "auth-mysql"])
    destination = wait_healthy("auth-mysql")
    require(sql(destination, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='auth';") == "0",
            "New auth database is not empty; refusing overwrite")
    import_dump(destination, backup / "auth.sql.gz")
    require(auth_copy_digest(destination) == state["auth_copy_sha256"], "Auth copy is not byte-equivalent before policy changes")
    sql(destination, cutover_sql())
    require(account_identity(destination) == state["account_identity"], "Account identity or SRP6 credentials changed")
    require(sql(destination, "SELECT acctid,numchars FROM auth.realmcharacters WHERE realmid=2 ORDER BY acctid;") == state["realm2_characters"],
            "Authored-world character counts changed")
    require(sql(destination, "SELECT GROUP_CONCAT(id ORDER BY id) FROM auth.realmlist;") == "2", "Retired realm is still registered")
    run(COMPOSE + ["up", "-d", "authserver"])
    wait_healthy("authserver")
    state.update(status="auth_ready", prepared_utc=datetime.now(timezone.utc).isoformat())
    private_json(STATE, state)
    print("Auth copied and verified; realm 2 retained, original realm 1 retired, backups secured.")


def finalize() -> None:
    state = json.loads(STATE.read_text())
    require(state["status"] in ("auth_ready", "complete"), "Auth promotion has not passed acceptance")
    for service in ("auth-mysql", "authserver", "lab-mysql", "worldserver", "world-viewer", "ai-server", "account-web"):
        wait_healthy(service)
    destination = run(COMPOSE + ["ps", "-q", "auth-mysql"]).decode().strip()
    require(sql(destination, "SELECT GROUP_CONCAT(id ORDER BY id) FROM auth.realmlist;") == "2", "Unexpected active realm after cutover")
    if state["status"] == "complete":
        require(not project_containers(OLD_PROJECT), "Retired original-realm containers reappeared")
        print("Canonical world and preserved accounts verified; original realm already retired.")
        return
    # Credential preservation is a one-time cutover assertion. Passwords and
    # account membership may legitimately change after promotion completes.
    require(account_identity(destination, state["account_identity"]["maximum_id"]) == state["account_identity"],
            "Final original account credentials differ from the backed-up original")
    # The retired database volume is an offline rollback artifact. No volumes
    # are pruned and no original directory or shared client data is deleted.
    for service, cid in state["original"].items():
        require(service in OLD_SERVICES, "Unexpected retirement target")
        present = run(["docker", "ps", "-aq", "--filter", f"id={cid}"]).decode().strip()
        if not present:
            # A prior finalization may have stopped after removing some targets.
            replacements = project_containers(OLD_PROJECT)
            require(service not in replacements, "Original realm service was replaced during retirement")
            continue
        row = inspect(cid)
        require(row["Config"]["Labels"].get("com.docker.compose.project") == OLD_PROJECT and
                row["Config"]["Labels"].get("com.docker.compose.service") == service, "Container identity changed")
        run(["docker", "update", "--restart=no", cid])
        if row["State"]["Running"]:
            run(["docker", "stop", "--time", "30", cid])
        run(["docker", "rm", cid])
        state.setdefault("removed", []).append(cid)
        private_json(STATE, state)
    state.update(status="complete", completed_utc=datetime.now(timezone.utc).isoformat())
    private_json(STATE, state)
    print("Canonical world verified; original realm services removed, protected rollback backups retained.")


def rollback_preparation() -> None:
    """Keep a failed auth cutover reversible while original containers exist."""
    if not STATE.exists():
        return
    state = json.loads(STATE.read_text())
    if state.get("status") in ("auth_ready", "complete"):
        return
    # Remove the new alias from DNS before restarting a legacy world that was
    # connected to both networks. Never discard either database volume.
    for service in ("authserver", "auth-mysql"):
        ids = run(COMPOSE + ["ps", "--all", "-q", service]).decode().split()
        for cid in ids:
            run(["docker", "stop", "--time", "30", cid])
    for service, cid in state.get("original", {}).items():
        if state.get("running_before", {}).get(service):
            run(["docker", "start", cid])
    prior = state.get("prior_world", {})
    if prior.get("running"):
        run(["docker", "start", prior["id"]])
    before = ROOT / state["backup"] / "canonical-before.env"
    if before.exists():
        (ROOT / ".env").write_bytes(before.read_bytes())
        (ROOT / ".env").chmod(0o600)
    state["status"] = "rolled_back"
    private_json(STATE, state)


def record_failure(error: BaseException) -> None:
    """Persist only a fixed operation category; exception payloads stay private."""
    operation = "validation_or_operation"
    timeout = None
    if isinstance(error, subprocess.TimeoutExpired):
        operation = "subprocess_timeout"
        command = error.cmd
        if isinstance(command, (list, tuple)) and len(command) >= 2 and command[0] == "docker":
            if command[1] in ("inspect", "stop", "start", "ps", "rm", "update", "exec", "compose"):
                operation = "docker_" + command[1]
        if type(error.timeout) in (int, float) and 0 <= error.timeout <= 86400:
            timeout = error.timeout
    private_json(STATE.with_name(STATE.stem + "-failure.json"), {
        "schema_version": 1, "captured_utc": datetime.now(timezone.utc).isoformat(),
        "exception_type": type(error).__name__, "operation": operation, "timeout_seconds": timeout,
    })


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "finalize"))
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            prepare()
        else:
            finalize()
    except (PromotionError, OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        try:
            record_failure(error)
        except Exception:
            print("Private promotion diagnostics could not be saved.", file=sys.stderr)
        if args.command == "prepare":
            try:
                rollback_preparation()
            except Exception:
                print("Automatic rollback needs inspection; original containers and all backups remain available.", file=sys.stderr)
        print("World promotion failed; inspect the protected promotion receipt. Original backups were not removed.", file=sys.stderr)
        raise SystemExit(2)


if __name__ == "__main__":
    main()
