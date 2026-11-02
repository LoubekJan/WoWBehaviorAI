"""Preview-v12 geometry with the unchanged living-v11 runtime behavior/catalog.

V11 evidence remains immutable. New native paths certify the same compiled
interaction coordinates against the new map, without rewriting any actor,
template, memory or Studio draft.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path

try:
    from . import living_v11 as previous, residents_v12 as native, single_return as base, settlement_population as settlement
except ImportError:
    import living_v11 as previous
    import residents_v12 as native
    import single_return as base
    import settlement_population as settlement

PROFILE = "living-v12"
METADATA = native.METADATA
MANIFEST, POSITION_PROOF, LOCATIONS_HEADER = previous.MANIFEST, previous.POSITION_PROOF, previous.LOCATIONS_HEADER
SPAWN_IDS, DEFIAS_IDS, HUMAN_IDS = previous.SPAWN_IDS, previous.DEFIAS_IDS, previous.HUMAN_IDS
require, ProfileError = base.require, base.ProfileError
agent_type, world_faction, capability, depot_id = previous.agent_type, previous.world_faction, previous.capability, previous.depot_id
runtime_catalog, position_contract = previous.runtime_catalog, previous.position_contract
snapshot_sql, validate_snapshot, bootstrap_sql, validate_telemetry = (
    previous.snapshot_sql, previous.validate_snapshot, previous.bootstrap_sql, previous.validate_telemetry)


def compiled_contract():
    baseline = previous.Population.load()
    document = base.load_json(previous.METADATA / MANIFEST)
    require(document["runtime_catalog_sha256"] == settlement.digest(runtime_catalog(baseline.positions)),
            "The v11 compiled interaction catalog no longer matches its reviewed baseline")
    return baseline, document


def manifest_document(population, native_manifest_sha256, contract=None):
    if contract is None: _proof, contract = position_contract(population)
    _baseline, original = compiled_contract()
    return {**previous.manifest_document(population, native_manifest_sha256, contract,
                profile=PROFILE, native_profile=native.PROFILE, source_revision=native.SOURCE_REVISION),
            "behavior_profile": previous.PROFILE,
            "compiled_position_proof_sha256": original["position_proof_sha256"]}


@dataclass(frozen=True)
class Population(previous.Population):
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA, geometry=None):
        population = native.Population.load(metadata, geometry)
        proof, contract = position_contract(population)
        baseline, original = compiled_contract()
        require(runtime_catalog(proof) == runtime_catalog(baseline.positions) and
                contract["runtime_catalog_sha256"] == original["runtime_catalog_sha256"] and
                contract["locations_header_sha256"] == original["locations_header_sha256"],
                "V12 geometry must not change the v11 runtime interaction catalog or compiled header")
        previous.verify_positions(proof, population, contract, profile=PROFILE,
            source_revision=native.SOURCE_REVISION, compiled_proof_sha256=original["position_proof_sha256"])
        sha = hashlib.sha256((metadata / native.MANIFEST).read_bytes()).hexdigest()
        require(settlement.digest(base.load_json(metadata / MANIFEST)) ==
                settlement.digest(manifest_document(population, sha, contract)),
                "V12 living identity, geometry, compiled catalog or behavior contract changed")
        return cls(population, proof)


def verify_data(data: Path, metadata: Path = METADATA):
    return Population.load(metadata, native.verify_data(data, metadata).geometry)
