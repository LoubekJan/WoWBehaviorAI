"""The unchanged 45 authored identities certified against preview-v12 geometry."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

try:
    from . import map_bundle, residents_v11 as previous
except ImportError:
    import map_bundle
    import residents_v11 as previous

PROFILE = "residents-v12"
SOURCE_REVISION = "preview-v12"
METADATA = map_bundle.METADATA / SOURCE_REVISION
MANIFEST, POSITION_PROOF, FACTION_PROOF = previous.MANIFEST, previous.POSITION_PROOF, previous.FACTION_PROOF
SPAWN_IDS, HOUSEHOLD_IDS, DEFIAS_IDS = previous.SPAWN_IDS, previous.HOUSEHOLD_IDS, previous.DEFIAS_IDS
HOUSE_IDS, INN_IDS, HALL_IDS = previous.HOUSE_IDS, previous.INN_IDS, previous.HALL_IDS
TEMPLATES, DEFIAS_SMART_SCRIPTS = previous.TEMPLATES, previous.DEFIAS_SMART_SCRIPTS
BASE_TEMPLATE, DEFIAS_TEMPLATE = previous.BASE_TEMPLATE, previous.DEFIAS_TEMPLATE
BASE_ENTRY, ENTRY_BASE, DEFIAS_BASE_ENTRY = previous.BASE_TEMPLATE["entry"], previous.ENTRY_BASE, previous.DEFIAS_BASE_ENTRY
require, ProfileError, geometry_digest = previous.require, previous.ProfileError, previous.geometry_digest
expected_entry, placement = previous.expected_entry, previous.placement
snapshot_sql, validate_snapshot, bootstrap_sql, validate_telemetry = (
    previous.snapshot_sql, previous.validate_snapshot, previous.bootstrap_sql, previous.validate_telemetry)


def baseline_population():
    return previous.Population.load()


@dataclass(frozen=True)
class Population(previous.Population):
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA, geometry=None):
        population = super().load(metadata, geometry, profile=PROFILE, revision=SOURCE_REVISION)
        require(population.actors == baseline_population().actors,
                "A geometry-only v12 upgrade must preserve all 45 v11 identities and homes")
        return population


def verify_data(data: Path, metadata: Path = METADATA):
    geometry = map_bundle.verify_data(data, metadata.parent, revision=SOURCE_REVISION)
    population = Population.load(metadata, geometry)
    previous.verify_factions(data, metadata, profile=PROFILE, revision=SOURCE_REVISION)
    return population
