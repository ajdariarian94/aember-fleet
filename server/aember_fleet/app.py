"""The HTTP API (FastAPI).

Devices (aember-auth on the device does enroll and login; the aember-fleet
agent reaches the device endpoints through aember-auth's socket):

    POST /api/v1/enroll                 {join_token, public_key, name, info}
    POST /api/v1/auth/challenge         {device_id}            -> {nonce}
    POST /api/v1/auth/token             {device_id, nonce, signature}
    GET  /api/v1/device/desired?arch=T  what to run (signed manifests); T is
                                        the device's aember target
    GET  /api/v1/device/artifacts/<id>  an assigned artifact's image
    POST /api/v1/device/status          what runs, and how it went

Admin (Authorization: Bearer <data/admin.token>):

    POST /api/v1/admin/join-tokens      {group, uses, ttl_hours}
    GET  /api/v1/admin/devices
    PUT  /api/v1/admin/devices/<id>/group  {group}
    PUT  /api/v1/admin/artifacts        image body; X-Aember-Manifest
                                        (base64), X-Aember-Signature
    GET  /api/v1/admin/artifacts
    POST /api/v1/admin/assign           {container, version, group | device}
    POST /api/v1/admin/unassign         {container, group | device}
"""

from __future__ import annotations

import hmac
import json
import os
import re
import tempfile
from pathlib import Path

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field

from . import security
from .store import Store

TOKEN_TTL = 900  # s: device access tokens
ARCH_RE = re.compile(r"[a-z0-9_]+(-[a-z0-9_]+)*")
# s: how often agents ask (AEMBER_FLEET_POLL_INTERVAL; tests use less)
POLL_INTERVAL = int(os.environ.get("AEMBER_FLEET_POLL_INTERVAL", "60"))


class DataDir:
    """Everything the server keeps: data/fleet.db, data/token.secret,
    data/admin.token, data/release.pub, data/artifacts/."""

    def __init__(self, path: Path):
        self.path = Path(path)
        self.artifacts = self.path / "artifacts"
        self.artifacts.mkdir(parents=True, exist_ok=True)
        self.store = Store(self.path / "fleet.db")
        self.token_secret = self._read_or_create("token.secret")
        self.admin_token = self._read_or_create("admin.token")
        release_pub = self.path / "release.pub"
        if not release_pub.is_file():
            raise FileNotFoundError(
                f"{release_pub}: the release public key (`aember-fleet init`)"
            )
        self.release_pub = release_pub.read_text().strip()
        security.public_key_from_b64(self.release_pub)  # valid?

    def _read_or_create(self, name: str) -> str:
        p = self.path / name
        if not p.is_file():
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(security.new_secret() + "\n")
        return p.read_text().strip()


# --- request bodies -----------------------------------------------------------


class Enroll(BaseModel):
    join_token: str
    public_key: str
    name: str = Field(min_length=1, max_length=64)
    info: dict = {}


class Challenge(BaseModel):
    device_id: str


class TokenRequest(BaseModel):
    device_id: str
    nonce: str
    signature: str


class Status(BaseModel):
    containers: list[dict] = []


class JoinTokenRequest(BaseModel):
    group: str = "default"
    uses: int = Field(1, ge=1)
    ttl_hours: float = Field(24, gt=0)


class GroupRequest(BaseModel):
    group: str = Field(min_length=1)


class Assign(BaseModel):
    container: str = Field(min_length=1)
    version: str | None = None
    group: str | None = None
    device: str | None = None


# --- the app -------------------------------------------------------------------


def create_app(data_path: Path) -> FastAPI:
    data = DataDir(data_path)
    store = data.store
    app = FastAPI(title="aember-fleet", version="0.1.0")
    app.state.data = data

    def bearer(authorization: str | None) -> str:
        if not authorization or not authorization.startswith("Bearer "):
            raise HTTPException(401, "missing bearer token")
        return authorization.removeprefix("Bearer ").strip()

    def device_auth(authorization: str | None = Header(None)):
        device_id = security.check_token(data.token_secret, bearer(authorization))
        device = store.device(device_id) if device_id else None
        if device is None:
            raise HTTPException(401, "invalid or expired token")
        return device

    def admin_auth(authorization: str | None = Header(None)) -> None:
        if not hmac.compare_digest(bearer(authorization), data.admin_token):
            raise HTTPException(401, "not the admin token")

    def target(body) -> tuple[str, str]:
        if (body.group is None) == (body.device is None):
            raise HTTPException(422, "give a group or a device")
        if body.device is not None and store.device(body.device) is None:
            raise HTTPException(404, f"no device {body.device}")
        return ("group", body.group) if body.group else ("device", body.device)

    @app.get("/api/v1/health")
    def health():
        return {"ok": True}

    # --- device: enrollment and login --------------------------------------

    @app.post("/api/v1/enroll")
    def enroll(body: Enroll):
        try:
            security.public_key_from_b64(body.public_key)
        except ValueError:
            raise HTTPException(422, "public_key: base64 of 32 bytes")
        existing = store.device_by_key(body.public_key)
        if existing is not None:  # the same device again (lost its state file)
            return {"device_id": existing["id"]}
        group = store.use_join_token(body.join_token)
        if group is None:
            raise HTTPException(403, "invalid, used up or expired join token")
        arch = str(body.info.get("arch", "unknown"))
        device_id = store.add_device(body.name, body.public_key, group, arch, body.info)
        return {"device_id": device_id}

    @app.post("/api/v1/auth/challenge")
    def challenge(body: Challenge):
        if store.device(body.device_id) is None:
            raise HTTPException(404, "unknown device")
        nonce = security.new_secret(24)
        store.add_challenge(nonce, body.device_id)
        return {"nonce": nonce}

    @app.post("/api/v1/auth/token")
    def token(body: TokenRequest):
        device = store.device(body.device_id)
        if device is None or not store.take_challenge(body.nonce, body.device_id):
            raise HTTPException(401, "unknown device or challenge")
        message = security.challenge_message(body.device_id, body.nonce)
        if not security.verify(device["public_key"], message, body.signature):
            raise HTTPException(401, "bad signature")
        store.seen(body.device_id)
        return {
            "token": security.make_token(data.token_secret, body.device_id, TOKEN_TTL),
            "expires_in": TOKEN_TTL,
        }

    # --- device: desired state, artifacts, status --------------------------

    def desired_entries(device) -> list[dict]:
        entries = []
        for container, version in sorted(store.desired(device).items()):
            art = store.find_artifact(container, version, device["arch"])
            if art is None:
                continue  # no build for this architecture: nothing to do
            entries.append(
                {
                    "name": container,
                    "version": version,
                    "artifact": art["id"],
                    "manifest": art["manifest"],
                    "signature": art["signature"],
                }
            )
        return entries

    @app.get("/api/v1/device/desired")
    def desired(arch: str | None = None, device=Depends(device_auth)):
        # The agent knows the device's aember target (x64-poky-linux, its
        # -musl variant, ...), aember-auth only the CPU: releases are per
        # target, so the agent's word counts.
        if arch is not None:
            if not ARCH_RE.fullmatch(arch):
                raise HTTPException(422, "arch: an aember target like x64-poky-linux")
            if arch != device["arch"]:
                store.set_device_arch(device["id"], arch)
                device = store.device(device["id"])
        store.seen(device["id"])
        return {"poll_interval": POLL_INTERVAL, "containers": desired_entries(device)}

    @app.get("/api/v1/device/artifacts/{artifact_id}")
    def artifact(artifact_id: str, device=Depends(device_auth)):
        if artifact_id not in {e["artifact"] for e in desired_entries(device)}:
            raise HTTPException(404, "not an artifact of this device")
        path = data.artifacts / f"{artifact_id}.squashfs"
        return FileResponse(path, media_type="application/octet-stream")

    @app.post("/api/v1/device/status")
    def status(body: Status, device=Depends(device_auth)):
        store.report(device["id"], body.model_dump())
        return {"ok": True}

    # --- admin ---------------------------------------------------------------

    @app.post("/api/v1/admin/join-tokens", dependencies=[Depends(admin_auth)])
    def join_token(body: JoinTokenRequest):
        token = security.new_secret(18)
        store.add_join_token(token, body.group, body.uses, body.ttl_hours * 3600)
        return {"token": token, "group": body.group, "uses": body.uses}

    def device_view(row) -> dict:
        return {
            "id": row["id"],
            "name": row["name"],
            "group": row["grp"],
            "arch": row["arch"],
            "enrolled": row["enrolled"],
            "last_seen": row["last_seen"],
            "status": json.loads(row["status"]) if row["status"] else None,
            "desired": store.desired(row),
        }

    @app.get("/api/v1/admin/devices", dependencies=[Depends(admin_auth)])
    def devices():
        return {"devices": [device_view(d) for d in store.devices()]}

    @app.put("/api/v1/admin/devices/{device_id}/group", dependencies=[Depends(admin_auth)])
    def set_group(device_id: str, body: GroupRequest):
        if not store.set_device_group(device_id, body.group):
            raise HTTPException(404, f"no device {device_id}")
        return {"ok": True}

    @app.put("/api/v1/admin/artifacts", dependencies=[Depends(admin_auth)])
    async def upload(
        request: Request,
        x_aember_manifest: str = Header(...),
        x_aember_signature: str = Header(...),
    ):
        try:
            manifest = security.unb64(x_aember_manifest).decode()
            fields = json.loads(manifest)
            container, version, arch = fields["container"], fields["version"], fields["arch"]
            sha256, size = fields["sha256"], int(fields["size"])
        except (ValueError, KeyError, TypeError):
            raise HTTPException(422, "X-Aember-Manifest: base64 of a release manifest")
        if manifest != security.canonical_manifest(container, version, arch, sha256, size):
            raise HTTPException(422, "manifest is not in canonical form")
        if not security.verify(data.release_pub, manifest.encode(), x_aember_signature):
            raise HTTPException(403, "release signature doesn't verify")
        if store.find_artifact(container, version, arch) is not None:
            raise HTTPException(409, f"{container} {version} {arch} exists already")

        fd, tmp = tempfile.mkstemp(dir=data.artifacts, suffix=".part")
        got = 0
        import hashlib

        h = hashlib.sha256()
        with os.fdopen(fd, "wb") as f:
            async for chunk in request.stream():
                f.write(chunk)
                h.update(chunk)
                got += len(chunk)
        if got != size or h.hexdigest() != sha256:
            os.unlink(tmp)
            raise HTTPException(422, "upload doesn't match the manifest's size/sha256")
        artifact_id = store.add_artifact(
            container, version, arch, sha256, size, manifest, x_aember_signature
        )
        os.replace(tmp, data.artifacts / f"{artifact_id}.squashfs")
        return {"artifact": artifact_id}

    @app.get("/api/v1/admin/artifacts", dependencies=[Depends(admin_auth)])
    def artifacts():
        return {
            "artifacts": [
                {k: a[k] for k in ("id", "container", "version", "arch", "size", "created")}
                for a in store.artifacts()
            ]
        }

    @app.post("/api/v1/admin/assign", dependencies=[Depends(admin_auth)])
    def assign(body: Assign):
        if not body.version:
            raise HTTPException(422, "version is missing")
        if not any(
            a["container"] == body.container and a["version"] == body.version
            for a in store.artifacts()
        ):
            raise HTTPException(404, f"no artifact {body.container} {body.version}")
        kind, tgt = target(body)
        store.assign(kind, tgt, body.container, body.version)
        return {"ok": True}

    @app.post("/api/v1/admin/unassign", dependencies=[Depends(admin_auth)])
    def unassign(body: Assign):
        kind, tgt = target(body)
        if not store.unassign(kind, tgt, body.container):
            raise HTTPException(404, "no such assignment")
        return {"ok": True}

    return app


def app_from_env() -> FastAPI:
    """For uvicorn: AEMBER_FLEET_DATA (default /data)."""
    return create_app(Path(os.environ.get("AEMBER_FLEET_DATA", "/data")))
