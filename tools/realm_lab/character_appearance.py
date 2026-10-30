"""Build the Character Studio catalog from the installed 3.3.5a client DBCs.

The catalog contains numeric metadata only. No models or textures are copied.
Customization combinations follow Player::ValidateAppearance with class 1 and
creation options; equipment comes from actual CharStartOutfit/Item rows.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

RACES = {1: "Člověk", 2: "Ork", 3: "Trpaslík", 4: "Noční elf", 5: "Nemrtvý",
         6: "Tauren", 7: "Gnóm", 8: "Troll", 10: "Krvavý elf", 11: "Draenei"}
CLASSES = {1: "bojovník", 2: "paladin", 3: "lovec", 4: "rogue", 5: "kněz",
           6: "rytíř smrti", 7: "šaman", 8: "mág", 9: "černokněžník", 11: "druid"}
SLOTS = {"head": (1,), "shoulder": (3,), "shirt": (4,), "chest": (5, 20),
         "waist": (6,), "legs": (7,), "feet": (8,), "wrist": (9,),
         "hands": (10,), "back": (16,), "tabard": (19,),
         "main_hand": (13, 17, 21), "off_hand": (13, 14, 22, 23),
         "ranged": (15, 25, 26, 28)}
SLOT_NAMES = {"head": "Hlava", "shoulder": "Ramena", "shirt": "Košile", "chest": "Hruď",
              "waist": "Pas", "legs": "Nohy", "feet": "Chodidla", "wrist": "Zápěstí",
              "hands": "Ruce", "back": "Plášť", "tabard": "Tabard", "main_hand": "Pravá ruka",
              "off_hand": "Levá ruka", "ranged": "Zbraň na dálku"}
FILES = ("ChrRaces.dbc", "CharSections.dbc", "CharacterFacialHairStyles.dbc",
         "CharStartOutfit.dbc", "Item.dbc")


def read_dbc(path: Path, expected_fields: int, expected_size: int | None = None):
    raw = path.read_bytes()
    if len(raw) < 20:
        raise ValueError("Truncated DBC header")
    magic, records, fields, size, strings = struct.unpack_from("<4s4I", raw)
    if (magic != b"WDBC" or fields != expected_fields or size != (expected_size or fields * 4)
            or records > 100000 or strings > 32 * 1024 * 1024
            or len(raw) != 20 + records * size + strings):
        raise ValueError("Unsupported or corrupt 3.3.5a DBC")
    return [struct.unpack_from("<" + "I" * (size // 4), raw, 20 + index * size)
            for index in range(records)]


def build_catalog(directory: Path) -> dict:
    race_rows = {row[0]: row for row in read_dbc(directory / FILES[0], 69)}
    sections = read_dbc(directory / FILES[1], 10)
    facial = read_dbc(directory / FILES[2], 8)
    outfits = read_dbc(directory / FILES[3], 77, 296)
    items = {row[0]: row for row in read_dbc(directory / FILES[4], 8)}
    races = []
    for race_id, name in RACES.items():
        race = race_rows[race_id]
        variants = []
        for gender in (0, 1):
            rows = [r for r in sections if r[1] == race_id and r[2] == gender
                    and r[7] & 1 and not r[7] & 4]
            skins = sorted({r[9] for r in rows if r[3] == 0 and r[8] == 0})
            face_options = [{"skin": skin, "faces": sorted({r[8] for r in rows
                             if r[3] == 1 and r[9] == skin})} for skin in skins]
            hair_options = [{"style": style, "colors": sorted({r[9] for r in rows
                             if r[3] == 3 and r[8] == style})}
                            for style in sorted({r[8] for r in rows if r[3] == 3})]
            facial_hairs = sorted({r[2] for r in facial if r[0] == race_id and r[1] == gender})
            exclude_facial_texture = race_id in (6, 11) or (gender == 1 and race_id not in (4, 5))
            facial_options = [{"style": style, "colors": sorted({r[9] for r in rows
                              if r[3] == 2 and r[8] == style}) if not exclude_facial_texture
                              else sorted({r[9] for r in rows if r[3] == 3})}
                              for style in facial_hairs]
            # Omit combinations that cannot satisfy all native checks.
            hair_options = [{"style": h["style"], "colors": [color for color in h["colors"]
                             if any(color in f["colors"] for f in facial_options)]}
                            for h in hair_options]
            hair_options = [h for h in hair_options if h["colors"]]
            if not skins or not face_options or not hair_options or not facial_hairs:
                raise ValueError("Missing player appearance options")
            variants.append({"gender": gender, "skins": skins,
                             "faces": sorted({f for row in face_options for f in row["faces"]}),
                             "hair_styles": [row["style"] for row in hair_options],
                             "hair_colors": sorted({c for row in hair_options for c in row["colors"]}),
                             "facial_hairs": facial_hairs, "face_options": face_options,
                             "hair_options": hair_options, "facial_options": facial_options})
        races.append({"id": race_id, "name": name,
                      "gender_models": {"0": race[4], "1": race[5]}, "variants": variants})

    equipment = {}
    for outfit in outfits:
        race_id, class_id, gender = outfit[1] & 255, (outfit[1] >> 8) & 255, (outfit[1] >> 16) & 255
        if race_id not in RACES:
            continue
        for item_id, display_id in zip(outfit[2:26], outfit[26:50]):
            item = items.get(item_id)
            if not item or display_id in (0, 0xFFFFFFFF) or item[5] != display_id:
                continue
            for slot, allowed in SLOTS.items():
                if item[6] not in allowed:
                    continue
                key = (slot, item_id, display_id)
                if key not in equipment:
                    equipment[key] = {"slot": slot, "item_id": item_id, "display_id": display_id,
                                      "inventory_type": item[6],
                                      "label": f"{SLOT_NAMES[slot]} #{item_id}", "sources": []}
                label = f"{RACES[race_id]} · {CLASSES.get(class_id, str(class_id))}"
                if label not in equipment[key]["sources"]:
                    equipment[key]["sources"].append(label)
    # Creation outfits have almost no hats, capes or tabards. Supply a bounded
    # selection of real classic item rows for every visual slot as well.
    # Labels intentionally use IDs: item names do not exist in Item.dbc.
    for slot, allowed in SLOTS.items():
        displays = {row["display_id"] for row in equipment.values() if row["slot"] == slot}
        for item_id, item in sorted(items.items()):
            if len(displays) >= 8:
                break
            if item[6] not in allowed or not item[5] or item[5] in displays:
                continue
            equipment[(slot, item_id, item[5])] = {
                "slot": slot, "item_id": item_id, "display_id": item[5],
                "inventory_type": item[6], "label": f"{SLOT_NAMES[slot]} #{item_id}",
                "sources": ["Item.dbc · výběr vzhledů"]}
            displays.add(item[5])
    gear = sorted(equipment.values(), key=lambda row: (list(SLOTS).index(row["slot"]), row["item_id"]))
    return {"schema_version": 1, "client_build": 12340,
            "source": {"kind": "local-client-dbc", "files": [
                {"name": name, "sha256": hashlib.sha256((directory / name).read_bytes()).hexdigest()}
                for name in FILES]},
            "equipment_slots": [{"id": slot, "name": SLOT_NAMES[slot], "inventory_types": list(types)}
                                for slot, types in SLOTS.items()],
            "races": races, "equipment": gear}


def apply_item_names(catalog: dict, names: dict) -> None:
    """Enrich labels from a read-only item_template export without changing gear."""
    if not isinstance(names, dict) or len(names) > 100000:
        raise ValueError("Item names must be an ID-to-name JSON object")
    for identity, name in names.items():
        if (not isinstance(identity, str) or not re.fullmatch(r"[1-9][0-9]{0,9}", identity)
                or int(identity) > 0xFFFFFFFF or not isinstance(name, str) or not name.strip()
                or len(name.encode("utf-8")) > 512 or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in name)):
            raise ValueError("Invalid item name metadata")
    matched = set()
    missing = set()
    for row in catalog["equipment"]:
        name = names.get(str(row["item_id"]))
        if name is None:
            missing.add(row["item_id"])
            continue
        row["label"] = name
        matched.add(row["item_id"])
    canonical = json.dumps(names, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    catalog["source"]["item_names"] = {"kind": "world.item_template", "sha256": hashlib.sha256(canonical).hexdigest(),
                                        "matched_item_ids": len(matched), "missing_item_ids": sorted(missing)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dbc-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--names-json", type=Path, help="Optional read-only item_template name snapshot: {item_id: name}")
    args = parser.parse_args()
    catalog = build_catalog(args.dbc_directory)
    if args.names_json is not None:
        if args.names_json.stat().st_size > 16 * 1024 * 1024:
            raise ValueError("Item names snapshot too large")
        apply_item_names(catalog, json.loads(args.names_json.read_text(encoding="utf-8")))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"races": len(catalog["races"]), "equipment": len(catalog["equipment"]),
                      "output": str(args.output)}))


if __name__ == "__main__":
    main()
