#!/usr/bin/env python3
"""Render the canonical world's authserver configuration without a shell."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import sys
from typing import Mapping

try:
    from .manage import SetupError, address, credential, number
except ImportError:  # Direct invocation inside the server container.
    from manage import SetupError, address, credential, number


@dataclass(frozen=True)
class AuthSettings:
    host: str
    port: int
    user: str
    password: str

    @classmethod
    def load(cls, env: Mapping[str, str]) -> AuthSettings:
        return cls(
            address(env, "TC_AUTH_DB_HOST", "aiworld-auth-db"),
            number(env, "TC_AUTH_DB_PORT", "3306", 1, 65535),
            credential(env, "TC_AUTH_SERVER_USER", username=True),
            credential(env, "TC_AUTH_SERVER_PASSWORD"),
        )


def render_auth_config(template: str, settings: AuthSettings) -> str:
    connection = f"{settings.host};{settings.port};{settings.user};{settings.password};auth"
    overrides = {
        "LoginDatabaseInfo": f'"{connection}"',
        "SourceDirectory": '"/workspace"',
        "LogsDir": '"/runtime/logs/auth"',
        "Updates.EnableDatabases": "1",
    }
    seen: set[str] = set()
    lines: list[str] = []
    for line in template.splitlines():
        match = re.match(r"^\s*([A-Za-z][A-Za-z0-9_.]*)\s*=", line)
        key = match.group(1) if match else None
        if key in overrides:
            if key in seen:
                raise SetupError(f"Duplicate auth config entry: {key}")
            seen.add(key)
            line = f"{key} = {overrides[key]}"
        lines.append(line)
    missing = overrides.keys() - seen
    if missing:
        raise SetupError(f"Auth template lacks required entries: {', '.join(sorted(missing))}")
    rendered = "\n".join(lines) + "\n"
    if "__TC_" in rendered:
        raise SetupError("Unresolved auth config placeholder")
    return rendered


def run_auth(template: Path, binary: Path, env: Mapping[str, str]) -> None:
    settings = AuthSettings.load(env)
    rendered = render_auth_config(template.read_text(encoding="utf-8"), settings)
    target = Path("/tmp/world-authserver.conf")
    descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.chmod(target, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(rendered)
    os.execv(str(binary), [str(binary), "-c", str(target)])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run-auth")
    run.add_argument("template", type=Path)
    run.add_argument("binary", type=Path)
    args = parser.parse_args()
    try:
        run_auth(args.template, args.binary, os.environ)
    except (SetupError, OSError) as exc:
        # Do not emit rendered connection strings or raw process diagnostics.
        message = str(exc) if isinstance(exc, SetupError) else "Unable to prepare or start authserver"
        print(f"Auth setup failed: {message}", file=sys.stderr)
        raise SystemExit(2)


if __name__ == "__main__":
    main()
