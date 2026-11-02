"""Create missing reviewed resident drafts without replacing an author's edits."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from .models import DraftInput, default_appearance, import_profile, strict_json, validate_appearance
from .storage import DraftStore
from .targets import TargetRegistry


def seed(*, project_root: Path, source_dir: Path, catalog_path: Path, data_dir: Path,
         profile: str) -> dict[str, int]:
    if profile != "residents-v10":
        raise ValueError("Resident drafts require the active reviewed resident profile")
    registry = TargetRegistry.load(project_root, profile)
    identities = set(range(902000, 902022))
    if not registry.available or {target.spawn_id for target in registry.targets} != identities:
        raise ValueError("Reviewed resident target registry is unavailable")
    paths = sorted(source_dir.glob("*.json"))
    if {path.name for path in paths} != {f"{identity}.json" for identity in identities}:
        raise ValueError("Resident source cards do not match the reviewed population")
    catalog = strict_json(catalog_path.read_bytes())
    appearance = default_appearance(catalog)
    validate_appearance(appearance, catalog)
    drafts = []
    for path in paths:
        if path.is_symlink() or path.stat().st_size > 16384:
            raise ValueError("Invalid resident source card")
        card = import_profile(strict_json(path.read_bytes()))
        if path.name != f"{card.spawn_id}.json" or not registry.validate(card.agent_id, card.spawn_id, card.map_id)["eligible"]:
            raise ValueError("Resident source card is not bound to its reviewed spawn")
        house = (int(card.spawn_id) - 902000) // 2 + 1
        drafts.append(DraftInput(card=card, appearance=appearance, authoring={
            "role": f"Obyvatel domu {house}", "faction": "Stormwind", "home": f"Dům {house}",
            "notes": "Ve hře používá původní vzhled obyvatele. Vlastní vzhled v editoru je návrh pro další nasazení."}))
    store = DraftStore(data_dir)
    existing = {}
    for doc in store.list():
        card = doc.get("card", {})
        spawn = card.get("spawn_id")
        if spawn in {str(identity) for identity in identities}:
            if card.get("map_id") != 725 or spawn in existing:
                raise ValueError("Conflicting resident drafts require an explicit author decision")
            existing[spawn] = doc
    created = 0
    for draft in drafts:
        if draft.card.spawn_id not in existing:
            store.create(draft)
            created += 1
    return {"created": created, "preserved": len(existing), "residents": len(drafts)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, default=Path(os.getenv("CHARACTER_STUDIO_PROJECT_DIR", "/workspace")))
    parser.add_argument("--source", type=Path, default=Path(os.getenv("CHARACTER_STUDIO_SOURCE_DIR", "/source-profiles")))
    parser.add_argument("--catalog", type=Path, default=Path(os.getenv("CHARACTER_STUDIO_CATALOG", "/catalog/catalog.json")))
    parser.add_argument("--data", type=Path, default=Path(os.getenv("CHARACTER_STUDIO_DATA_DIR", "/data")))
    parser.add_argument("--profile", default=os.getenv("LAB_AI_PROFILE", ""))
    args = parser.parse_args()
    result = seed(project_root=args.project_root, source_dir=args.source, catalog_path=args.catalog,
                  data_dir=args.data, profile=args.profile)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
