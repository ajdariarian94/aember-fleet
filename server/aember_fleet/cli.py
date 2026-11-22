"""`aember-fleet`: set up and run the server, and manage the fleet.

    aember-fleet init --data DIR --name fleet.example [--ip 10.0.2.2]
                      [--release-key FILE]
    aember-fleet serve --data DIR [--port 8443]
    aember-fleet join-token [--group G] [--uses N] [--ttl-hours H]
    aember-fleet devices
    aember-fleet group DEVICE GROUP
    aember-fleet upload IMAGE --container NAME --version V --arch ARCH
                        --release-key FILE
    aember-fleet artifacts
    aember-fleet assign CONTAINER VERSION (--group G | --device ID)
    aember-fleet unassign CONTAINER (--group G | --device ID)

Admin commands talk to --server (default https://localhost:8443) with the
admin token and CA from --data (or --token / --ca).
"""

from __future__ import annotations

import argparse
import datetime
import ipaddress
import json
import os
import ssl
import sys
import urllib.error
import urllib.request
from pathlib import Path

from . import security


def _write_private(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(data)


def init(data: Path, name: str, ips: list[str], release_key: Path) -> None:
    """TLS CA + server certificate, release key pair, admin token."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    tls = data / "tls"
    if (tls / "ca.crt").exists():
        raise SystemExit(f"{data} is set up already")
    now = datetime.datetime.now(datetime.timezone.utc)

    def pem_key(key) -> bytes:
        return key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )

    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "aember-fleet CA")])
    ca = (
        x509.CertificateBuilder()
        .subject_name(ca_name)
        .issuer_name(ca_name)
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(
            x509.KeyUsage(False, False, False, False, False, True, True, False, False),
            critical=True,
        )
        .sign(ca_key, hashes.SHA256())
    )
    srv_key = ec.generate_private_key(ec.SECP256R1())
    sans: list[x509.GeneralName] = [x509.DNSName(name)]
    sans += [x509.IPAddress(ipaddress.ip_address(ip)) for ip in ips]
    srv = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)]))
        .issuer_name(ca_name)
        .public_key(srv_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=825))
        .add_extension(x509.SubjectAlternativeName(sans), critical=False)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]),
            critical=False,
        )
        .sign(ca_key, hashes.SHA256())
    )
    _write_private(tls / "ca.key", pem_key(ca_key))
    (tls / "ca.crt").write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    _write_private(tls / "server.key", pem_key(srv_key))
    (tls / "server.crt").write_bytes(
        srv.public_bytes(serialization.Encoding.PEM)
        + ca.public_bytes(serialization.Encoding.PEM)
    )

    seed, pub = security.generate_release_key()
    if release_key.exists():
        raise SystemExit(f"{release_key} exists already")
    _write_private(release_key, security.b64(seed).encode() + b"\n")
    (data / "release.pub").write_text(pub + "\n")

    from .app import DataDir

    DataDir(data)  # creates fleet.db, token.secret, admin.token
    print(f"fleet data:        {data}")
    print(f"server name:       {name} {' '.join(ips)}")
    print(f"devices' CA:       {tls / 'ca.crt'}  (into the board's /etc/aember/auth/ca.pem)")
    print(f"release public key {pub}  (into the board's /etc/aember/fleet/fleet.json)")
    print(f"release key:       {release_key}  (keep it off the server)")
    print(f"admin token:       {data / 'admin.token'}")


def serve(data: Path, port: int, host: str) -> None:
    import uvicorn

    os.environ["AEMBER_FLEET_DATA"] = str(data)
    uvicorn.run(
        "aember_fleet.app:app_from_env",
        factory=True,
        host=host,
        port=port,
        ssl_certfile=str(data / "tls" / "server.crt"),
        ssl_keyfile=str(data / "tls" / "server.key"),
    )


class Admin:
    def __init__(self, server: str, token: str, ca: str | None):
        self.server = server.rstrip("/")
        self.token = token
        self.ctx = ssl.create_default_context(cafile=ca) if ca else None

    def call(self, method: str, path: str, body=None, raw: bytes | None = None,
             headers: dict | None = None):
        data = raw if raw is not None else (
            json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(self.server + path, data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.token}")
        if raw is None and body is not None:
            req.add_header("Content-Type", "application/json")
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, context=self.ctx) as resp:
                return json.loads(resp.read() or b"{}")
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")
            raise SystemExit(f"{method} {path}: {e.code} {detail}") from None
        except urllib.error.URLError as e:
            raise SystemExit(f"cannot reach {self.server}: {e.reason}") from None


def upload(admin: Admin, image: Path, container: str, version: str, arch: str,
           release_key: Path) -> dict:
    seed = security.unb64(release_key.read_text().strip())
    key = security.release_key_from_seed(seed)
    sha = security.sha256_file(image)
    manifest = security.canonical_manifest(
        container, version, arch, sha, image.stat().st_size)
    return admin.call(
        "PUT", "/api/v1/admin/artifacts", raw=image.read_bytes(),
        headers={
            "X-Aember-Manifest": security.b64(manifest.encode()),
            "X-Aember-Signature": security.sign_manifest(key, manifest),
            "Content-Type": "application/octet-stream",
        })


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="aember-fleet", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", type=Path, default=Path(os.environ.get("AEMBER_FLEET_DATA", "data")))
    ap.add_argument("--server", default=os.environ.get("AEMBER_FLEET_SERVER", "https://localhost:8443"))
    ap.add_argument("--token", help="admin token (default: DATA/admin.token)")
    ap.add_argument("--ca", help="CA certificate (default: DATA/tls/ca.crt)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("init")
    p.add_argument("--name", required=True, help="the server's DNS name (certificate)")
    p.add_argument("--ip", action="append", default=[], help="also valid for this IP")
    p.add_argument("--release-key", type=Path, default=Path("release.key"))
    p = sub.add_parser("serve")
    p.add_argument("--port", type=int, default=8443)
    p.add_argument("--host", default="0.0.0.0")
    p = sub.add_parser("join-token")
    p.add_argument("--group", default="default")
    p.add_argument("--uses", type=int, default=1)
    p.add_argument("--ttl-hours", type=float, default=24)
    sub.add_parser("devices")
    p = sub.add_parser("group")
    p.add_argument("device")
    p.add_argument("group")
    p = sub.add_parser("upload")
    p.add_argument("image", type=Path)
    p.add_argument("--container", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--arch", required=True)
    p.add_argument("--release-key", type=Path, required=True)
    sub.add_parser("artifacts")
    for name in ("assign", "unassign"):
        p = sub.add_parser(name)
        p.add_argument("container")
        if name == "assign":
            p.add_argument("version")
        g = p.add_mutually_exclusive_group(required=True)
        g.add_argument("--group")
        g.add_argument("--device")
    a = ap.parse_args(argv)

    if a.cmd == "init":
        init(a.data, a.name, a.ip, a.release_key)
        return 0
    if a.cmd == "serve":
        serve(a.data, a.port, a.host)
        return 0
    token = a.token or (a.data / "admin.token").read_text().strip()
    ca = a.ca or (str(a.data / "tls" / "ca.crt") if (a.data / "tls" / "ca.crt").exists() else None)
    admin = Admin(a.server, token, ca)
    if a.cmd == "join-token":
        out = admin.call("POST", "/api/v1/admin/join-tokens",
                         {"group": a.group, "uses": a.uses, "ttl_hours": a.ttl_hours})
    elif a.cmd == "devices":
        out = admin.call("GET", "/api/v1/admin/devices")
    elif a.cmd == "group":
        out = admin.call("PUT", f"/api/v1/admin/devices/{a.device}/group", {"group": a.group})
    elif a.cmd == "upload":
        out = upload(admin, a.image, a.container, a.version, a.arch, a.release_key)
    elif a.cmd == "artifacts":
        out = admin.call("GET", "/api/v1/admin/artifacts")
    else:
        body = {"container": a.container, "group": a.group, "device": a.device}
        if a.cmd == "assign":
            body["version"] = a.version
        out = admin.call("POST", f"/api/v1/admin/{a.cmd}", body)
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
