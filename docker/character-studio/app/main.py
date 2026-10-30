"""LAN character authoring: durable drafts and explicit runtime downloads only."""
from __future__ import annotations

import base64
import hashlib
import hmac
import io
import json
import mimetypes
import os
from pathlib import Path
import re
import secrets
import zipfile

import httpx
from fastapi import FastAPI, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import ValidationError

from .models import (Appearance, Card, DraftInput, DraftUpdate, GenerationInput, default_appearance,
                     import_profile, native_profile, plain, strict_json, validate_appearance)
from .storage import DraftStore, StoreError

APP_DIR = Path(__file__).parent
# Windows may associate .js with text/plain in its registry. ES modules must
# retain their JavaScript media type when this app is previewed on that host.
mimetypes.add_type("text/javascript", ".js")
MAX_REQUEST_BYTES = 65536
MAX_GENERATION_BYTES = 32768
COOKIE_NAME = "character_studio_session"


def create_app(*, data_dir: Path | None = None, catalog_path: Path | None = None,
               source_dir: Path | None = None, appearance_dir: Path | None = None,
               model_data_dir: Path | None = None, ai_url: str | None = None,
               ai_transport: httpx.AsyncBaseTransport | None = None, password: str | None = None) -> FastAPI:
    app = FastAPI(title="AI World Character Studio", version="1.0.0")
    # Compose explicitly supplies /data. A host-side import/test must not try
    # to create a privileged filesystem-root directory on Linux CI runners.
    development_root = APP_DIR.parents[2] if len(APP_DIR.parents) > 2 else Path.cwd()
    development_data = str(development_root / "runtime" / "character-studio")
    app.state.store = DraftStore(data_dir or Path(os.getenv("CHARACTER_STUDIO_DATA_DIR", development_data)))
    app.state.catalog_path = catalog_path or Path(os.getenv("CHARACTER_STUDIO_CATALOG", "/catalog/catalog.json"))
    app.state.source_dir = source_dir or Path(os.getenv("CHARACTER_STUDIO_SOURCE_DIR", "/source-profiles"))
    app.state.appearance_dir = appearance_dir or Path(os.getenv("CHARACTER_STUDIO_APPEARANCE_DIR", "/source-appearances"))
    app.state.ai_url = ai_url if ai_url is not None else os.getenv("CHARACTER_STUDIO_AI_URL", "http://ai-server:8000")
    app.state.ai_transport = ai_transport
    app.state.password = password if password is not None else os.getenv("CHARACTER_STUDIO_PASSWORD", "")
    app.state.csrf_secret = secrets.token_bytes(32)
    app.state.model_data_dir = model_data_dir or Path(os.getenv("CHARACTER_STUDIO_MODEL_DATA_DIR", "/model-data"))
    app.mount("/static", StaticFiles(directory=APP_DIR / "static", check_dir=False), name="static")
    if app.state.model_data_dir.is_dir():
        app.mount("/model-data", StaticFiles(directory=app.state.model_data_dir), name="model-data")

    def token_for(nonce: str):
        return hmac.new(app.state.csrf_secret, nonce.encode("ascii"), hashlib.sha256).hexdigest()

    def load_catalog():
        try:
            path = app.state.catalog_path
            if path.stat().st_size > 4 * 1024 * 1024:
                raise ValueError("catalog size")
            catalog = strict_json(path.read_bytes())
            if not isinstance(catalog, dict) or catalog.get("schema_version") != 1 or not catalog.get("races"):
                raise ValueError("catalog schema")
            return catalog
        except (OSError, ValueError, UnicodeError, RecursionError):
            raise HTTPException(503, "Ověřený katalog vzhledů není dostupný.") from None

    def check_draft(draft):
        try:
            validate_appearance(draft.appearance, load_catalog())
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from None

    @app.middleware("http")
    async def boundary(request: Request, call_next):
        if request.url.path != "/health" and app.state.password:
            supplied = request.headers.get("authorization", "")
            valid = False
            if supplied.startswith("Basic "):
                try:
                    username, separator, secret = base64.b64decode(supplied[6:], validate=True).decode("utf-8").partition(":")
                    valid = bool(separator) and secrets.compare_digest(username.encode("utf-8"), b"studio") and secrets.compare_digest(secret.encode("utf-8"), app.state.password.encode("utf-8"))
                except (ValueError, UnicodeError):
                    pass
            if not valid:
                return JSONResponse({"detail": "Přihlaste se do editoru účtem studio."}, status_code=401,
                                    headers={"WWW-Authenticate": 'Basic realm="Character Studio", charset="UTF-8"'})
        if request.method not in ("GET", "HEAD", "OPTIONS"):
            origin = request.headers.get("origin")
            expected = str(request.base_url).rstrip("/")
            nonce = request.cookies.get(COOKIE_NAME, "")
            token = request.headers.get("x-csrf-token", "")
            if (origin is not None and origin != expected
                    or request.headers.get("sec-fetch-site", "same-origin") not in ("same-origin", "none")
                    or not re.fullmatch(r"[a-f0-9]{64}", nonce)
                    or not secrets.compare_digest(token_for(nonce), token)):
                return JSONResponse({"detail": "Relace editoru vypršela. Obnovte stránku a zkuste to znovu."}, status_code=403)
        response = await call_next(request)
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "same-origin"
        response.headers["X-Frame-Options"] = "SAMEORIGIN"
        if request.url.path.startswith("/api/"):
            response.headers["Cache-Control"] = "no-store"
        return response

    @app.exception_handler(StoreError)
    async def store_error(_request, exc: StoreError):
        return JSONResponse({"detail": exc.message}, status_code=exc.status)

    async def payload(request: Request, model=None):
        if request.headers.get("content-type", "").split(";", 1)[0].strip().lower() != "application/json":
            raise HTTPException(415, "Požadavek musí obsahovat application/json.")
        body = bytearray()
        async for chunk in request.stream():
            body.extend(chunk)
            if len(body) > MAX_REQUEST_BYTES:
                raise HTTPException(413, "Požadavek přesahuje 64 KiB.")
        try:
            decoded = strict_json(bytes(body))
            return model.model_validate(decoded) if model is not None else decoded
        except (ValueError, UnicodeError, RecursionError):
            raise HTTPException(422, "Neplatná data postavy. Zkontrolujte povinná pole, textové limity a celočíselná ID.") from None

    def identity(value: str):
        if not re.fullmatch(r"[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}", value):
            raise HTTPException(404, "Postava nebyla nalezena.")
        return value

    def exports(value: str):
        doc = app.state.store.get(identity(value))
        try:
            card = Card.model_validate(doc["card"])
            appearance = Appearance.model_validate(doc["appearance"])
            validate_appearance(appearance, load_catalog())
            profile = native_profile(card)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from None
        sidecar = {"schema_version": 1, "spawn_id": profile["spawn_id"], "map_id": profile["map_id"],
                   "appearance": appearance.model_dump()}
        return doc, profile, sidecar

    def attachment(value, filename):
        body = json.dumps(value, ensure_ascii=False, indent=2).encode("utf-8") + b"\n"
        return Response(body, media_type="application/json", headers={"Content-Disposition": f'attachment; filename="{filename}"'})

    @app.get("/health")
    def health():
        return {"status": "ok"}

    @app.get("/", include_in_schema=False)
    def index():
        path = APP_DIR / "static" / "index.html"
        if not path.is_file():
            raise HTTPException(503, "Rozhraní editoru není dostupné.")
        return FileResponse(path)

    @app.get("/api/session")
    def session(request: Request, response: Response):
        nonce = request.cookies.get(COOKIE_NAME, "")
        if not re.fullmatch(r"[a-f0-9]{64}", nonce):
            nonce = secrets.token_hex(32)
        response.set_cookie(COOKIE_NAME, nonce, httponly=True, samesite="strict", secure=request.url.scheme == "https", max_age=43200)
        return {"csrf_token": token_for(nonce)}

    @app.get("/api/catalog")
    async def catalog():
        result = load_catalog()
        seeds = []
        try:
            paths = sorted(app.state.source_dir.glob("*.json"))[:128]
        except OSError:
            paths = []
        for path in paths:
            try:
                if path.is_symlink() or path.stat().st_size > 16384:
                    continue
                card = import_profile(strict_json(path.read_bytes()))
                appearance = default_appearance(result, gender=1 if card.spawn_id == "901036" else 0)
                origin = "default"
                sidepath = app.state.appearance_dir / f"{card.spawn_id}.json"
                try:
                    if sidepath.is_file() and not sidepath.is_symlink() and sidepath.stat().st_size <= 8192:
                        sidecar = strict_json(sidepath.read_bytes())
                        if (isinstance(sidecar, dict) and set(sidecar) == {"schema_version", "spawn_id", "map_id", "appearance"}
                                and type(sidecar["schema_version"]) is int and sidecar["schema_version"] == 1
                                and type(sidecar["spawn_id"]) is int and sidecar["spawn_id"] == int(card.spawn_id)
                                and type(sidecar["map_id"]) is int and sidecar["map_id"] == card.map_id):
                            candidate = Appearance.model_validate(sidecar["appearance"])
                            validate_appearance(candidate, result)
                            appearance = candidate
                            origin = "source"
                except (OSError, ValueError, UnicodeError, RecursionError):
                    # A separate invalid appearance does not erase an otherwise
                    # valid authored card from the draft creation picker.
                    pass
                seeds.append({"key": card.spawn_id, "card": card.model_dump(), "appearance": appearance.model_dump(), "appearance_source": origin})
            except (OSError, ValueError, UnicodeError, RecursionError):
                continue
        available = False
        if app.state.ai_url:
            try:
                async with httpx.AsyncClient(timeout=2, transport=app.state.ai_transport) as client:
                    response = await client.get(app.state.ai_url.rstrip("/") + "/health")
                    health = response.json()
                    available = response.status_code == 200 and isinstance(health, dict) and health.get("npc_model_enabled") is True and health.get("npc_model_configured") is True
            except (httpx.HTTPError, ValueError):
                pass
        return {**result, "source_profiles": seeds, "limits": {"name_characters": 32, "name_utf8_bytes": 80,
                "backstory_utf8_bytes": 8192, "personality_utf8_bytes": 2048, "native_profile_bytes": 16384, "max_characters": DraftStore.MAX_DRAFTS},
                "model": {"available": available}, "preview": {"available": (app.state.model_data_dir / "manifest.json").is_file()}}

    @app.get("/api/characters")
    def characters():
        return {"characters": app.state.store.list()}

    @app.get("/api/characters/{character_id}")
    def character(character_id: str):
        return app.state.store.get(identity(character_id))

    @app.post("/api/characters", status_code=201)
    async def create_character(request: Request):
        draft = await payload(request, DraftInput)
        check_draft(draft)
        return app.state.store.create(draft)

    @app.put("/api/characters/{character_id}")
    async def update_character(character_id: str, request: Request):
        identity(character_id)
        draft = await payload(request, DraftUpdate)
        check_draft(draft)
        return app.state.store.update(character_id, draft.revision, DraftInput.model_validate(draft.model_dump(exclude={"revision"})))

    @app.delete("/api/characters/{character_id}")
    def delete_character(character_id: str, revision: int = Query(ge=1, le=2**31 - 1)):
        app.state.store.delete(identity(character_id), revision)
        return {"deleted": True, "id": character_id}

    @app.post("/api/import-profile", status_code=201)
    async def import_native_profile(request: Request):
        raw = await payload(request)
        try:
            card = import_profile(raw)
            appearance = default_appearance(load_catalog(), gender=1 if card.spawn_id == "901036" else 0)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from None
        return app.state.store.create(DraftInput(card=card, appearance=appearance))

    @app.post("/api/import-draft", status_code=201)
    async def import_draft(request: Request):
        raw = await payload(request)
        keys = {"id", "revision", "created_at", "updated_at", "card", "appearance", "authoring", "schema_version"}
        if (not isinstance(raw, dict) or not set(raw).issubset(keys)
                or not {"card", "appearance"}.issubset(raw) or raw.get("schema_version", 1) != 1):
            raise HTTPException(422, "Soubor není podporovaný export charakterového draftu.")
        try:
            value = DraftInput.model_validate({key: raw[key] for key in ("card", "appearance", "authoring") if key in raw})
        except ValueError:
            raise HTTPException(422, "Draft obsahuje neplatné hodnoty.") from None
        check_draft(value)
        # Imported storage IDs/revisions are never trusted as update authority.
        return app.state.store.create(value)

    @app.get("/api/characters/{character_id}/draft")
    def draft_export(character_id: str):
        doc = app.state.store.get(identity(character_id))
        return attachment({"schema_version": 1, **doc}, f"character-{doc['id']}.json")

    @app.get("/api/characters/{character_id}/profile")
    def profile_export(character_id: str):
        _doc, profile, _appearance = exports(character_id)
        return attachment(profile, f"{profile['spawn_id']}.json")

    @app.get("/api/characters/{character_id}/appearance")
    def appearance_export(character_id: str):
        _doc, profile, appearance = exports(character_id)
        return attachment(appearance, f"{profile['spawn_id']}-appearance.json")

    @app.get("/api/characters/{character_id}/bundle")
    @app.get("/api/characters/{character_id}/bundle.zip")
    def bundle_export(character_id: str):
        doc, profile, appearance = exports(character_id)
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
            for folder, item in (("npc_agents", profile), ("npc_appearances", appearance)):
                bundle.writestr(f"{folder}/{profile['spawn_id']}.json", json.dumps(item, ensure_ascii=False, indent=2) + "\n")
            bundle.writestr("character-studio-draft.json", json.dumps(doc, ensure_ascii=False, indent=2) + "\n")
            bundle.writestr("README.txt", "CHARACTER STUDIO — export pro WoW 3.3.5a\n\n"
                "npc_agents/ obsahuje přesný šestipolový profil LLM.\n"
                "npc_appearances/ obsahuje samostatný vzhled stejného spawnu.\n"
                "Tyto soubory patří do odpovídajících data/ adresářů projektu.\n"
                "Před použitím ověřte skutečné spawn ID, mapu a registraci spravovaného NPC.\n"
                "Profil sám NPC nespawnuje ani nezapíná jeho řízení.\n"
                "Worldserver načítá profily a vzhled při spuštění; změna vyžaduje řízené nasazení a restart.\n"
                "Editor nic automaticky nemění v běžícím světě. Uchovejte charakterový draft pro další úpravy.\n")
        return Response(archive.getvalue(), media_type="application/zip",
                        headers={"Content-Disposition": f'attachment; filename="character-{profile["spawn_id"]}.zip"'})

    @app.post("/api/generate")
    async def generate(request: Request):
        draft = await payload(request, GenerationInput)
        check_draft(draft)
        if not draft.card.name.strip():
            raise HTTPException(422, "Před generováním vyplňte jméno postavy.")
        if not app.state.ai_url:
            raise HTTPException(503, "Pomocník pro psaní není nakonfigurovaný.")
        model_payload = {"profile": draft.card.model_dump(include={"name", "backstory", "personality"}),
                         "appearance": {**draft.appearance.model_dump(), "equipment": []}, "authoring": draft.authoring.model_dump(),
                         "instruction": draft.instruction}
        if len(json.dumps(model_payload, ensure_ascii=False).encode("utf-8")) > 32768:
            raise HTTPException(422, "Podklady pro model přesahují 32 KiB. Zkraťte autorské poznámky.")
        body = bytearray()
        try:
            async with httpx.AsyncClient(timeout=35, transport=app.state.ai_transport) as client:
                async with client.stream("POST", app.state.ai_url.rstrip("/") + "/character-studio/generate", json=model_payload) as response:
                    if response.status_code not in (200, 429, 503, 504):
                        raise HTTPException(502, "Model nevrátil platný návrh. Uložená postava zůstává beze změny.")
                    if response.status_code != 200:
                        messages = {429: "Model právě zpracovává jiný návrh. Zkuste to za chvíli.",
                                    503: "Model není zapnutý nebo nakonfigurovaný.", 504: "Model nestihl odpovědět v časovém limitu."}
                        raise HTTPException(response.status_code, messages[response.status_code])
                    async for chunk in response.aiter_bytes():
                        body.extend(chunk)
                        if len(body) > MAX_GENERATION_BYTES:
                            raise HTTPException(502, "Odpověď modelu je příliš velká.")
            proposal = strict_json(bytes(body))
            if not isinstance(proposal, dict) or set(proposal) != {"backstory", "personality", "suggestions"}:
                raise ValueError("proposal shape")
            candidate = draft.card.model_copy(update={"backstory": proposal["backstory"], "personality": proposal["personality"]})
            Card.model_validate(candidate.model_dump())
            if not candidate.backstory.strip() or not candidate.personality.strip():
                raise ValueError("empty proposal")
            if (not isinstance(proposal["suggestions"], list) or len(proposal["suggestions"]) > 4
                    or any(not isinstance(s, str) for s in proposal["suggestions"])):
                raise ValueError("suggestions")
            for suggestion in proposal["suggestions"]:
                plain(suggestion, 512, lines=False)
            return proposal
        except httpx.TimeoutException:
            raise HTTPException(504, "Model nestihl odpovědět v časovém limitu.") from None
        except httpx.HTTPError:
            raise HTTPException(503, "Spojení s pomocníkem pro psaní není dostupné.") from None
        except (ValueError, UnicodeError, RecursionError):
            raise HTTPException(502, "Model nevrátil platný návrh. Uložená postava zůstává beze změny.") from None

    return app


app = create_app()
