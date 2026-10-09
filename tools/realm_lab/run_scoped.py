#!/usr/bin/env python3
"""Run the lab observer/recorder with the versioned map's exact scope."""
from __future__ import annotations

import os
import sys

try:
    from .single_return import ProfileError, Scope
except ImportError:
    from single_return import ProfileError, Scope


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit("run_scoped.py requires a command")
    try:
        os.environ.update(Scope.load().observer_environment())
    except ProfileError as exc:
        raise SystemExit(f"Lab observer scope failed: {exc}") from exc
    os.execvp(sys.argv[1], sys.argv[1:])


if __name__ == "__main__":
    main()
