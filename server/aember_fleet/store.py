"""The fleet's state in SQLite: join tokens, devices, login challenges,
signed artifacts and what each device or group should run."""

from __future__ import annotations

import json
import sqlite3
import threading
import time
import uuid
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS join_tokens (
    token TEXT PRIMARY KEY,
    grp TEXT NOT NULL,
    uses_left INTEGER NOT NULL,
    expires REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    public_key TEXT NOT NULL UNIQUE,
    grp TEXT NOT NULL,
    arch TEXT NOT NULL,
    info TEXT NOT NULL,
    enrolled REAL NOT NULL,
    last_seen REAL,
    status TEXT
);
CREATE TABLE IF NOT EXISTS challenges (
    nonce TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    expires REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS artifacts (
    id TEXT PRIMARY KEY,
    container TEXT NOT NULL,
    version TEXT NOT NULL,
    arch TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    size INTEGER NOT NULL,
    manifest TEXT NOT NULL,
    signature TEXT NOT NULL,
    created REAL NOT NULL,
    UNIQUE (container, version, arch)
);
-- What should run: per group, and per device (a device entry wins).
CREATE TABLE IF NOT EXISTS assignments (
    target_kind TEXT NOT NULL CHECK (target_kind IN ('group', 'device')),
    target TEXT NOT NULL,
    container TEXT NOT NULL,
    version TEXT NOT NULL,
    PRIMARY KEY (target_kind, target, container)
);
"""


class Store:
    def __init__(self, path: Path):
        self._db = sqlite3.connect(str(path), check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        self._lock = threading.Lock()
        with self._lock:
            self._db.executescript(SCHEMA)
            self._db.commit()

    def _q(self, sql: str, args: tuple = ()) -> list[sqlite3.Row]:
        with self._lock:
            rows = self._db.execute(sql, args).fetchall()
            self._db.commit()
            return rows

    # --- join tokens ------------------------------------------------------

    def add_join_token(self, token: str, group: str, uses: int, ttl: float) -> None:
        self._q(
            "INSERT INTO join_tokens VALUES (?, ?, ?, ?)",
            (token, group, uses, time.time() + ttl),
        )

    def use_join_token(self, token: str) -> str | None:
        """The token's group if it is valid (one use spent), else None."""
        with self._lock:
            row = self._db.execute(
                "SELECT grp, uses_left, expires FROM join_tokens WHERE token = ?",
                (token,),
            ).fetchone()
            if row is None or row["uses_left"] < 1 or row["expires"] < time.time():
                return None
            self._db.execute(
                "UPDATE join_tokens SET uses_left = uses_left - 1 WHERE token = ?",
                (token,),
            )
            self._db.commit()
            return row["grp"]

    # --- devices ----------------------------------------------------------

    def device_by_key(self, public_key: str) -> sqlite3.Row | None:
        rows = self._q("SELECT * FROM devices WHERE public_key = ?", (public_key,))
        return rows[0] if rows else None

    def add_device(
        self, name: str, public_key: str, group: str, arch: str, info: dict
    ) -> str:
        device_id = uuid.uuid4().hex[:16]
        self._q(
            "INSERT INTO devices (id, name, public_key, grp, arch, info, enrolled)"
            " VALUES (?, ?, ?, ?, ?, ?, ?)",
            (device_id, name, public_key, group, arch, json.dumps(info), time.time()),
        )
        return device_id

    def device(self, device_id: str) -> sqlite3.Row | None:
        rows = self._q("SELECT * FROM devices WHERE id = ?", (device_id,))
        return rows[0] if rows else None

    def devices(self) -> list[sqlite3.Row]:
        return self._q("SELECT * FROM devices ORDER BY enrolled")

    def set_device_group(self, device_id: str, group: str) -> bool:
        with self._lock:
            n = self._db.execute(
                "UPDATE devices SET grp = ? WHERE id = ?", (group, device_id)
            ).rowcount
            self._db.commit()
            return n > 0

    def set_device_arch(self, device_id: str, arch: str) -> None:
        self._q("UPDATE devices SET arch = ? WHERE id = ?", (arch, device_id))

    def report(self, device_id: str, status: dict) -> None:
        self._q(
            "UPDATE devices SET last_seen = ?, status = ? WHERE id = ?",
            (time.time(), json.dumps(status), device_id),
        )

    def seen(self, device_id: str) -> None:
        self._q("UPDATE devices SET last_seen = ? WHERE id = ?", (time.time(), device_id))

    # --- challenges -------------------------------------------------------

    def add_challenge(self, nonce: str, device_id: str, ttl: float = 60) -> None:
        now = time.time()
        self._q("DELETE FROM challenges WHERE expires < ?", (now,))
        self._q(
            "INSERT INTO challenges VALUES (?, ?, ?)", (nonce, device_id, now + ttl)
        )

    def take_challenge(self, nonce: str, device_id: str) -> bool:
        """True once for a live challenge of this device (then it's gone)."""
        with self._lock:
            row = self._db.execute(
                "SELECT device_id, expires FROM challenges WHERE nonce = ?", (nonce,)
            ).fetchone()
            self._db.execute("DELETE FROM challenges WHERE nonce = ?", (nonce,))
            self._db.commit()
        return (
            row is not None
            and row["device_id"] == device_id
            and row["expires"] >= time.time()
        )

    # --- artifacts --------------------------------------------------------

    def add_artifact(
        self,
        container: str,
        version: str,
        arch: str,
        sha256: str,
        size: int,
        manifest: str,
        signature: str,
    ) -> str:
        artifact_id = uuid.uuid4().hex[:16]
        self._q(
            "INSERT INTO artifacts VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                artifact_id,
                container,
                version,
                arch,
                sha256,
                size,
                manifest,
                signature,
                time.time(),
            ),
        )
        return artifact_id

    def artifact(self, artifact_id: str) -> sqlite3.Row | None:
        rows = self._q("SELECT * FROM artifacts WHERE id = ?", (artifact_id,))
        return rows[0] if rows else None

    def find_artifact(self, container: str, version: str, arch: str):
        rows = self._q(
            "SELECT * FROM artifacts WHERE container = ? AND version = ? AND arch = ?",
            (container, version, arch),
        )
        return rows[0] if rows else None

    def artifacts(self) -> list[sqlite3.Row]:
        return self._q("SELECT * FROM artifacts ORDER BY created")

    # --- assignments ------------------------------------------------------

    def assign(self, kind: str, target: str, container: str, version: str) -> None:
        self._q(
            "INSERT INTO assignments VALUES (?, ?, ?, ?) ON CONFLICT"
            " (target_kind, target, container) DO UPDATE SET version = excluded.version",
            (kind, target, container, version),
        )

    def unassign(self, kind: str, target: str, container: str) -> bool:
        with self._lock:
            n = self._db.execute(
                "DELETE FROM assignments WHERE target_kind = ? AND target = ?"
                " AND container = ?",
                (kind, target, container),
            ).rowcount
            self._db.commit()
            return n > 0

    def desired(self, device: sqlite3.Row) -> dict[str, str]:
        """container -> version for @p device: its group's, then its own."""
        want: dict[str, str] = {}
        for kind, target in (("group", device["grp"]), ("device", device["id"])):
            for row in self._q(
                "SELECT container, version FROM assignments"
                " WHERE target_kind = ? AND target = ?",
                (kind, target),
            ):
                want[row["container"]] = row["version"]
        return want

    def assignments(self) -> list[sqlite3.Row]:
        return self._q("SELECT * FROM assignments ORDER BY target_kind, target")
