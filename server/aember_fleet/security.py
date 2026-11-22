"""Keys, signatures and tokens.

- Device login: the device signs ``aember-auth-v1\\n<device_id>\\n<nonce>``
  with its Ed25519 key (aember-auth); the server checks it with the public
  key from enrollment.
- Release signatures: Ed25519 over the exact bytes of a release manifest
  (``canonical_manifest``). The private release key stays with the admin
  (``aember-fleet sign``/``upload``); the server and the devices only have
  the public key, so a compromised server can't make devices run anything.
- Device access tokens: ``<payload>.<HMAC-SHA256>``, both base64url,
  payload ``{"d": device_id, "exp": unix_time}``.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import secrets
import time

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

CHALLENGE_PREFIX = "aember-auth-v1"


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


def unb64(text: str) -> bytes:
    return base64.b64decode(text, validate=True)


def _b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def _unb64url(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def public_key_from_b64(text: str) -> Ed25519PublicKey:
    raw = unb64(text)
    if len(raw) != 32:
        raise ValueError("an Ed25519 public key has 32 bytes")
    return Ed25519PublicKey.from_public_bytes(raw)


def verify(public_key_b64: str, message: bytes, signature_b64: str) -> bool:
    try:
        public_key_from_b64(public_key_b64).verify(unb64(signature_b64), message)
        return True
    except (InvalidSignature, ValueError):
        return False


def challenge_message(device_id: str, nonce: str) -> bytes:
    return f"{CHALLENGE_PREFIX}\n{device_id}\n{nonce}".encode()


def new_secret(nbytes: int = 32) -> str:
    return secrets.token_urlsafe(nbytes)


# --- release manifests ------------------------------------------------------


def canonical_manifest(
    container: str, version: str, arch: str, sha256: str, size: int
) -> str:
    """The bytes a release signature covers (and the agent checks)."""
    return json.dumps(
        {
            "arch": arch,
            "container": container,
            "sha256": sha256,
            "size": size,
            "version": version,
        },
        sort_keys=True,
        separators=(",", ":"),
    )


def sign_manifest(private_key: Ed25519PrivateKey, manifest: str) -> str:
    return b64(private_key.sign(manifest.encode()))


def generate_release_key() -> tuple[bytes, str]:
    """A new release key: (raw 32-byte private seed, base64 public key)."""
    key = Ed25519PrivateKey.generate()
    from cryptography.hazmat.primitives import serialization

    seed = key.private_bytes(
        serialization.Encoding.Raw,
        serialization.PrivateFormat.Raw,
        serialization.NoEncryption(),
    )
    pub = key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    return seed, b64(pub)


def release_key_from_seed(seed: bytes) -> Ed25519PrivateKey:
    return Ed25519PrivateKey.from_private_bytes(seed)


def sha256_file(path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --- device access tokens ---------------------------------------------------


def make_token(secret: str, device_id: str, ttl: int, now: float | None = None) -> str:
    exp = int((now if now is not None else time.time()) + ttl)
    payload = _b64url(json.dumps({"d": device_id, "exp": exp}).encode())
    mac = hmac.new(secret.encode(), payload.encode(), hashlib.sha256).digest()
    return f"{payload}.{_b64url(mac)}"


def check_token(secret: str, token: str, now: float | None = None) -> str | None:
    """The device id of a valid, unexpired token, else None."""
    try:
        payload, mac = token.split(".")
        want = hmac.new(secret.encode(), payload.encode(), hashlib.sha256).digest()
        if not hmac.compare_digest(want, _unb64url(mac)):
            return None
        data = json.loads(_unb64url(payload))
        if data["exp"] < (now if now is not None else time.time()):
            return None
        return str(data["d"])
    except (ValueError, KeyError, TypeError, json.JSONDecodeError):
        return None
