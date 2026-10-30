"""Bounded SQLite draft store; every mutation checks the saved revision."""
from __future__ import annotations

from datetime import datetime, timezone
from contextlib import contextmanager
import json
from pathlib import Path
import sqlite3
import uuid

from .models import DraftInput


class StoreError(Exception):
    def __init__(self, status: int, message: str):
        self.status = status
        self.message = message


class DraftStore:
    MAX_DRAFTS = 512

    def __init__(self, directory: Path):
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.path = directory / "characters.sqlite3"
        with self.connect() as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("CREATE TABLE IF NOT EXISTS drafts (id TEXT PRIMARY KEY, revision INTEGER NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL, payload TEXT NOT NULL)")

    @contextmanager
    def connect(self):
        database = sqlite3.connect(self.path, timeout=5, isolation_level=None)
        try:
            yield database
        finally:
            database.close()

    @staticmethod
    def document(row):
        return {"id": row[0], "revision": row[1], "created_at": row[2], "updated_at": row[3], **json.loads(row[4])}

    def list(self):
        with self.connect() as db:
            return [self.document(row) for row in db.execute("SELECT * FROM drafts ORDER BY updated_at DESC, id ASC")]

    def get(self, identity: str):
        with self.connect() as db:
            row = db.execute("SELECT * FROM drafts WHERE id=?", (identity,)).fetchone()
        if row is None:
            raise StoreError(404, "Postava nebyla nalezena.")
        return self.document(row)

    def create(self, draft: DraftInput):
        identity = str(uuid.uuid4())
        now = datetime.now(timezone.utc).isoformat()
        payload = json.dumps(draft.model_dump(), ensure_ascii=False)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            if db.execute("SELECT COUNT(*) FROM drafts").fetchone()[0] >= self.MAX_DRAFTS:
                db.rollback()
                raise StoreError(409, "Úložiště obsahuje nejvýše 512 postav. Nejprve některou archivujte exportem a odstraňte.")
            db.execute("INSERT INTO drafts VALUES (?,1,?,?,?)", (identity, now, now, payload))
            db.commit()
        return self.get(identity)

    def update(self, identity: str, revision: int, draft: DraftInput):
        now = datetime.now(timezone.utc).isoformat()
        payload = json.dumps(draft.model_dump(), ensure_ascii=False)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT revision FROM drafts WHERE id=?", (identity,)).fetchone()
            if row is None:
                db.rollback()
                raise StoreError(404, "Postava nebyla nalezena.")
            if row[0] != revision:
                db.rollback()
                raise StoreError(409, "Postavu mezitím změnil jiný editor. Načtěte uloženou verzi a své změny porovnejte.")
            db.execute("UPDATE drafts SET revision=revision+1,updated_at=?,payload=? WHERE id=?", (now, payload, identity))
            saved = db.execute("SELECT * FROM drafts WHERE id=?", (identity,)).fetchone()
            db.commit()
        return self.document(saved)

    def delete(self, identity: str, revision: int):
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT revision FROM drafts WHERE id=?", (identity,)).fetchone()
            if row is None:
                db.rollback()
                raise StoreError(404, "Postava nebyla nalezena.")
            if row[0] != revision:
                db.rollback()
                raise StoreError(409, "Postava má novější verzi. Nejprve ji znovu načtěte.")
            db.execute("DELETE FROM drafts WHERE id=?", (identity,))
            db.commit()
