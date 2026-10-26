#!/usr/bin/env python3
"""Run the lab observer/recorder with the versioned map's exact scope."""
from __future__ import annotations

import os
import sys

try:
    from .single_return import ProfileError, Scope
    from . import settlement_population, supply_population, terrain_profile, wildlife_population
except ImportError:
    from single_return import ProfileError, Scope
    import settlement_population, supply_population, terrain_profile, wildlife_population


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit("run_scoped.py requires a command")
    try:
        profile = os.environ.get("LAB_AI_PROFILE", "disabled")
        if profile not in ("disabled", "single-return", "hunt-cycle", "hunt-100", terrain_profile.PROFILE, settlement_population.PROFILE, wildlife_population.PROFILE, supply_population.PROFILE):
            raise ProfileError("Unknown reviewed observer lab profile")
        if profile == supply_population.PROFILE:
            scope = supply_population.Population.load()
        elif profile == wildlife_population.PROFILE:
            scope = wildlife_population.Population.load()
        elif profile == settlement_population.PROFILE:
            scope = settlement_population.Population.load()
        else:
            scope = terrain_profile.load_scope() if profile == terrain_profile.PROFILE else Scope.load()
        os.environ.update(scope.observer_environment())
    except ProfileError as exc:
        raise SystemExit(f"Lab observer scope failed: {exc}") from exc
    os.execvp(sys.argv[1], sys.argv[1:])


if __name__ == "__main__":
    main()
