"""Transactional JSON document store, with atomic read/modify/write."""
import json
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from fastapi import HTTPException

def now():
    return datetime.now(timezone.utc).isoformat()

class Store:
    def __init__(self, path):
        self.path = path

    def initialize(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.connection() as db:
            db.execute('PRAGMA journal_mode=WAL')
            db.execute('CREATE TABLE IF NOT EXISTS documents (kind TEXT, id TEXT, body TEXT NOT NULL, PRIMARY KEY(kind,id))')

    @contextmanager
    def connection(self):
        db = sqlite3.connect(self.path, timeout=15)
        try:
            with db:
                yield db
        finally:
            db.close()

    def get(self, kind, id):
        with self.connection() as db:
            row = db.execute('SELECT body FROM documents WHERE kind=? AND id=?', (kind, id)).fetchone()
        if not row:
            raise HTTPException(404, f'{kind.title()} not found')
        return json.loads(row[0])

    def list(self, kind):
        with self.connection() as db:
            rows = db.execute('SELECT body FROM documents WHERE kind=? ORDER BY rowid DESC', (kind,)).fetchall()
        return [json.loads(row[0]) for row in rows]

    def put(self, kind, doc):
        with self.connection() as db:
            db.execute('INSERT INTO documents VALUES (?,?,?) ON CONFLICT(kind,id) DO UPDATE SET body=excluded.body', (kind, doc['id'], json.dumps(doc, ensure_ascii=False)))
        return doc

    def update(self, kind, id, mutate):
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT body FROM documents WHERE kind=? AND id=?', (kind, id)).fetchone()
            if not row:
                raise HTTPException(404, f'{kind.title()} not found')
            doc = json.loads(row[0])
            mutate(doc)
            doc['updated_at'] = now()
            db.execute('UPDATE documents SET body=? WHERE kind=? AND id=?', (json.dumps(doc, ensure_ascii=False), kind, id))
        return doc
