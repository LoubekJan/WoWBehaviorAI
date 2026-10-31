"""Read-only NPC bindings from the same reviewed populations as world startup.

This is an authoring compatibility check, not live database certification and
never an enrollment mechanism. A draft may name any future identity; installing
a native profile is allowed only for a controlled, participating known-role
actor in the configured population. No SQL or runtime control changes live here.
"""
from __future__ import annotations

from dataclasses import dataclass
import importlib
from pathlib import Path
import sys

from .models import decimal_identity

SUPPORTED_PROFILES = {"consumers-v8": "consumer_population", "raids-v8": "raid_population",
                      "fields-v8": "field_population", "defias-v8": "defias_population",
                      "full-v8": "full_population"}
LIVING_ROLES = {0: "CIVILIAN", 1: "GUARD", 2: "SERVICE", 5: "COMBATANT", 6: "PREDATOR", 7: "PREY"}
ROLE_NAMES = {"resident": "Obyvatel", "worker": "Pracovník", "guard": "Strážný",
              "hunter": "Lovec", "raider": "Nájezdník", "combatant": "Bojovník",
              "predator": "Dravec", "prey": "Zvěř", "merchant": "Obchodník",
              "farmer": "Farmář", "consumer": "Obyvatel", "supplier": "Zásobovač",
              "camp_guard": "Táborová stráž"}
FACTION_NAMES = {"stormwind": "Stormwind", "defias": "Defias", "gnolls": "Gnollové",
                 "murlocs": "Murloci", "kobolds": "Koboldové", "wildlife": "Divočina"}


@dataclass(frozen=True)
class Target:
    spawn_id: int
    map_id: int
    name: str
    role: str
    living_role: str
    entry: int
    eligible: bool
    reason: str
    control_mode: str = ""

    def public(self):
        return {"agent_id": str(self.spawn_id), "spawn_id": str(self.spawn_id), "map_id": self.map_id,
                "name": self.name, "role": self.role, "living_role": self.living_role, "entry": self.entry,
                "eligible": self.eligible, "reason": self.reason,
                "control_mode": self.control_mode or ("AI_WORLD_CONTROLLED" if self.eligible else "OBSERVE_ONLY")}


@dataclass(frozen=True)
class TargetRegistry:
    available: bool
    profile: str
    targets: tuple[Target, ...] = ()
    reason: str = ""

    @classmethod
    def load(cls, project_root: Path, profile: str):
        if profile == "authoring-empty":
            return cls(False, profile, reason="Mapa se připravuje v režimu authoring-empty a nemá registrovaná NPC. "
                       "Návrh lze uložit a zálohovat; export do hry je do nové registrace NPC vypnutý.")
        if profile not in SUPPORTED_PROFILES:
            return cls(False, profile, reason="Aktivní provozní profil nepodporuje ověřené vazby LLM postav. "
                       "Návrh lze uložit; export do hry zatím není dostupný.")
        try:
            project_root = project_root.resolve()
            module_name = SUPPORTED_PROFILES[profile]
            source = project_root / "tools" / "realm_lab" / (module_name + ".py")
            metadata = project_root / "data" / "realm_lab" / "aiworldlab" / "preview-v8"
            present = source.is_file() and metadata.is_dir()
        except (OSError, RuntimeError):
            present = False
        if not present:
            return cls(False, profile, reason="Ověřená metadata provozní populace nejsou dostupná. "
                       "Návrh lze uložit, ale herní export je zablokovaný.")
        try:
            # Only an operator-configured checkout supplies modules. Client
            # requests cannot choose paths, modules, profiles or a trust root.
            root = str(project_root)
            if root not in sys.path:
                sys.path.insert(0, root)
            module = importlib.import_module("tools.realm_lab." + module_name)
            if Path(module.__file__).resolve() != source.resolve():
                raise ValueError("population module provenance")
            population = module.Population.load(metadata)
            all_actors = {actor.spawn_id: actor for actor in population.settlement.actors}
            owned = {actor.spawn_id: actor for actor in population.actors}
            if len(all_actors) != 100 or len(owned) != len(population.actors) or not owned.keys() <= all_actors.keys():
                raise ValueError("population identities")
            scope = population.config()
            map_id = int(scope["AIWorld.ScopeMapId"])
            scope_ids = {int(value) for value in scope["AIWorld.ScopeSpawnIds"].strip('"').split(",")}
            if map_id != 725 or scope["AIWorld.ScopeRestrictAgents"] != "1" or not owned.keys() <= scope_ids:
                raise ValueError("population scope")
            targets = []
            for spawn_id, actor in sorted(all_actors.items()):
                owner = owned.get(spawn_id)
                kind = getattr(owner, "agent_type", None)
                eligible = owner is not None and type(kind) is int and kind in LIVING_ROLES
                role = getattr(owner, "role", actor.role)
                display_role = ROLE_NAMES.get(role, role)
                faction = FACTION_NAMES.get(actor.faction, actor.faction)
                if eligible:
                    reason = f"NPC {spawn_id} je řízené AIWorld v ověřeném profilu {profile}."
                elif owner is not None:
                    reason = f"NPC {spawn_id} nemá podporovanou roli pro LLM řízení."
                else:
                    reason = f"NPC {spawn_id} v profilu {profile} používá běžné herní AI a není registrované pro LLM řízení."
                targets.append(Target(spawn_id, map_id, f"{display_role} · {faction}", role,
                                      LIVING_ROLES.get(kind, "NONE"), actor.entry, eligible, reason,
                                      "AI_WORLD_CONTROLLED" if owner is not None else "OBSERVE_ONLY"))
            return cls(True, profile, tuple(targets))
        except (ImportError, OSError, ValueError, TypeError, KeyError, AttributeError):
            # Do not return raw metadata paths or file/exception content.
            return cls(False, profile, reason="Metadata aktivní populace se nepodařilo ověřit. "
                       "Návrh zůstává uložitelný; herní export je do opravy metadat zablokovaný.")

    def metadata(self):
        return {"available": self.available, "profile": self.profile,
                "source": "reviewed-population", "reason": self.reason}

    def validate(self, agent_id: str, spawn_id: str, map_id: int):
        result = {"eligible": False, "registry_available": self.available, "target": None,
                  "reason": self.reason}
        if not self.available:
            return result
        try:
            agent = int(decimal_identity(agent_id, required=True))
            spawn = int(decimal_identity(spawn_id, required=True))
        except ValueError:
            return {**result, "reason": "Vyplňte platné kladné Agent ID a Spawn ID."}
        if agent != spawn:
            return {**result, "reason": "Agent ID a Spawn ID musí být shodné."}
        target = next((target for target in self.targets if target.spawn_id == spawn), None)
        if target is None:
            return {**result, "reason": "Toto NPC není součástí ověřené populace aktivního profilu. "
                    "Nejprve je nutná samostatná registrace; změna čísla v kartě ji neprovede."}
        result["target"] = target.public()
        if type(map_id) is not int or map_id != target.map_id:
            return {**result, "reason": f"Vybrané NPC patří na mapu {target.map_id}; mapa v kartě se neshoduje."}
        return {**result, "eligible": target.eligible, "reason": target.reason}
