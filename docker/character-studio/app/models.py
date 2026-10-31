"""Authoring records are deliberately separate from native runtime profiles."""
from __future__ import annotations

import json
import re
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

UINT64_MAX = 2**64 - 1
UINT32_MAX = 2**32 - 1
SLOTS = ("head", "shoulder", "shirt", "chest", "waist", "legs", "feet", "wrist", "hands",
         "back", "tabard", "main_hand", "off_hand", "ranged")
AUTHOR_FIELDS = ("role", "faction", "age", "home", "traits", "values", "goals", "fears", "relationships", "notes")


def plain(value: str, byte_limit: int, *, lines: bool = True, nonempty: bool = False) -> str:
    value = value.replace("\r\n", "\n")
    if len(value.encode("utf-8")) > byte_limit:
        raise ValueError(f"Text přesahuje limit {byte_limit} UTF-8 bajtů.")
    if nonempty and not value.strip(" \n\t\u00a0"):
        raise ValueError("Text nesmí být prázdný.")
    for char in value:
        scalar = ord(char)
        if lines and char in "\n\t":
            continue
        if (scalar < 32 or 127 <= scalar <= 159 or char == "|" or 0xD800 <= scalar <= 0xDFFF
                or 0x200B <= scalar <= 0x200F or 0x2028 <= scalar <= 0x202E
                or 0x2060 <= scalar <= 0x206F or scalar == 0xFEFF):
            raise ValueError("Text obsahuje nepovolené řídicí znaky nebo herní značky.")
    return value


def decimal_identity(value: str, *, required: bool = False) -> str:
    if value == "" and not required:
        return value
    if not re.fullmatch(r"[1-9][0-9]{0,19}", value) or int(value) > UINT64_MAX:
        raise ValueError("ID musí být kladné celé číslo do 18446744073709551615.")
    return value


def strict_json(raw: bytes | str) -> Any:
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("Duplicitní klíč JSON.")
            result[key] = value
        return result

    def invalid_constant(_value):
        raise ValueError("Neplatné číslo JSON.")

    return json.loads(raw, object_pairs_hook=pairs, parse_constant=invalid_constant)


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)


class Card(StrictModel):
    agent_id: str = ""
    spawn_id: str = ""
    map_id: int = Field(default=725, ge=0, le=UINT32_MAX)
    name: str = Field(default="", max_length=32)
    backstory: str = ""
    personality: str = ""

    @field_validator("agent_id", "spawn_id")
    @classmethod
    def valid_identity(cls, value):
        return decimal_identity(value)

    @field_validator("name")
    @classmethod
    def valid_name(cls, value):
        return plain(value, 80, lines=False)

    @field_validator("backstory", "personality")
    @classmethod
    def valid_lore(cls, value, info):
        return plain(value, 8192 if info.field_name == "backstory" else 2048)


class Equipment(StrictModel):
    slot: Literal["head", "shoulder", "shirt", "chest", "waist", "legs", "feet", "wrist", "hands",
                  "back", "tabard", "main_hand", "off_hand", "ranged"]
    item_id: int = Field(gt=0, le=UINT32_MAX)
    display_id: int = Field(gt=0, le=UINT32_MAX)


class Appearance(StrictModel):
    race: int = Field(default=1, ge=1, le=11)
    gender: Literal[0, 1] = 0
    class_id: Literal[1] = 1
    skin: int = Field(default=0, ge=0, le=255)
    face: int = Field(default=0, ge=0, le=255)
    hair_style: int = Field(default=0, ge=0, le=255)
    hair_color: int = Field(default=0, ge=0, le=255)
    facial_hair: int = Field(default=0, ge=0, le=255)
    equipment: list[Equipment] = Field(default_factory=list, max_length=14)

    @field_validator("gender", "class_id", mode="before")
    @classmethod
    def actual_integer(cls, value):
        if type(value) is not int:
            raise ValueError("Volba vzhledu musí být celé číslo.")
        return value

    @model_validator(mode="after")
    def unique_slots(self):
        if len({item.slot for item in self.equipment}) != len(self.equipment):
            raise ValueError("Každá pozice vybavení smí být vybrána jen jednou.")
        return self


class Authoring(StrictModel):
    role: str = ""
    faction: str = ""
    age: str = ""
    home: str = ""
    traits: str = ""
    values: str = ""
    goals: str = ""
    fears: str = ""
    relationships: str = ""
    notes: str = ""

    @field_validator(*AUTHOR_FIELDS)
    @classmethod
    def valid_text(cls, value):
        return plain(value, 4096)


class DraftInput(StrictModel):
    card: Card = Field(default_factory=Card)
    appearance: Appearance = Field(default_factory=Appearance)
    authoring: Authoring = Field(default_factory=Authoring)


class DraftUpdate(DraftInput):
    revision: int = Field(ge=1, le=2**31 - 1)


class TargetIdentity(StrictModel):
    agent_id: str = ""
    spawn_id: str = ""
    map_id: int = Field(default=725, ge=0, le=UINT32_MAX)

    @field_validator("agent_id", "spawn_id")
    @classmethod
    def valid_identity(cls, value):
        return decimal_identity(value)


class GenerationInput(DraftInput):
    instruction: str = ""

    @field_validator("instruction")
    @classmethod
    def valid_instruction(cls, value):
        return plain(value, 4096)


def validate_appearance(appearance: Appearance, catalog: dict) -> None:
    race = next((r for r in catalog.get("races", []) if r.get("id") == appearance.race), None)
    variant = next((v for v in (race or {}).get("variants", []) if v.get("gender") == appearance.gender), None)
    if not variant:
        raise ValueError("Pro vybranou rasu a pohlaví nejsou dostupná klientská data.")
    for field, options in (("skin", "skins"), ("face", "faces"), ("hair_style", "hair_styles"),
                           ("hair_color", "hair_colors"), ("facial_hair", "facial_hairs")):
        if getattr(appearance, field) not in variant.get(options, []):
            raise ValueError(f"Volba vzhledu {field} není v klientu 3.3.5a dostupná.")
    face_options = variant.get("face_options", [])
    if face_options and not any(v.get("skin") == appearance.skin and appearance.face in v.get("faces", []) for v in face_options):
        raise ValueError("Vybraný obličej není dostupný pro tento odstín kůže.")
    hair_options = variant.get("hair_options", [])
    if hair_options and not any(v.get("style") == appearance.hair_style and appearance.hair_color in v.get("colors", []) for v in hair_options):
        raise ValueError("Vybraná barva není dostupná pro tento účes.")
    facial_options = variant.get("facial_options", [])
    if facial_options and not any(v.get("style") == appearance.facial_hair and appearance.hair_color in v.get("colors", []) for v in facial_options):
        raise ValueError("Vybraná barva není dostupná pro tento typ vousů či ozdob.")
    available = {(e.get("slot"), e.get("item_id"), e.get("display_id")) for e in catalog.get("equipment", [])}
    for item in appearance.equipment:
        if (item.slot, item.item_id, item.display_id) not in available:
            raise ValueError("Vybavení neodpovídá ověřenému katalogu klienta.")


def default_appearance(catalog: dict, *, gender: int = 0) -> Appearance:
    race = next((r for r in catalog.get("races", []) if r.get("id") == 1), None)
    variant = next((v for v in (race or {}).get("variants", []) if v.get("gender") == gender), None)
    if variant is None:
        raise ValueError("Katalog neobsahuje výchozí lidský vzhled.")
    return Appearance(race=1, gender=gender, skin=variant["skins"][0], face=variant["faces"][0],
                      hair_style=variant["hair_styles"][0], hair_color=variant["hair_colors"][0],
                      facial_hair=variant["facial_hairs"][0])


def native_profile(card: Card) -> dict:
    agent_id = int(decimal_identity(card.agent_id, required=True))
    spawn_id = int(decimal_identity(card.spawn_id, required=True))
    if agent_id != spawn_id:
        raise ValueError("Agent ID a spawn ID musí být stejné a odpovídat skutečnému NPC.")
    for field, limit in (("name", 80), ("backstory", 8192), ("personality", 2048)):
        plain(getattr(card, field), limit, lines=field != "name", nonempty=True)
    result = {"agent_id": agent_id, "map_id": card.map_id, "spawn_id": spawn_id,
              "name": card.name, "backstory": card.backstory, "personality": card.personality}
    if len(json.dumps(result, ensure_ascii=False, indent=2).encode("utf-8")) + 1 > 16384:
        raise ValueError("Celá charakterová karta přesahuje 16 KiB.")
    return result


def native_template(card: Card, authoring: Authoring, target: dict, population_profile: str) -> dict:
    """A reviewed recipe for a complete private clone, never client-selected SQL."""
    profile = native_profile(card)
    spawn_id = profile["spawn_id"]
    if population_profile != "full-v8":
        raise ValueError("Samostatná herní šablona vyžaduje aktivní profil full-v8. Kartu a vzhled lze exportovat samostatně.")
    if (not 901000 <= spawn_id <= 901099 or card.map_id != 725 or
            target.get("eligible") is not True or target.get("spawn_id") != card.spawn_id or
            target.get("agent_id") != card.agent_id or target.get("map_id") != card.map_id or
            type(target.get("entry")) is not int or not 0 < target["entry"] <= 0xFFFFFF):
        raise ValueError("Herní šablona vyžaduje ověřený původní model NPC z vlastní populace.")
    title = plain(authoring.role.strip(), 80, lines=False)
    return {"schema_version": 1, "spawn_id": spawn_id, "map_id": card.map_id,
            "entry": 9800000 + spawn_id - 901000, "base_entry": target["entry"],
            "name": card.name, "title": title}


def import_profile(raw: Any) -> Card:
    fields = {"agent_id", "spawn_id", "map_id", "name", "backstory", "personality"}
    if not isinstance(raw, dict) or set(raw) != fields:
        raise ValueError("Nativní profil musí obsahovat přesně šest podporovaných polí.")
    for field in ("agent_id", "spawn_id", "map_id"):
        if type(raw[field]) is not int:
            raise ValueError("Nativní ID musí být celočíselné číslo JSON.")
    card = Card(**{**raw, "agent_id": str(raw["agent_id"]), "spawn_id": str(raw["spawn_id"])})
    native_profile(card)
    return card
