import json

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from fastapi.testclient import TestClient

from aember_fleet import cli, security
from aember_fleet.app import create_app

ARCH = "x64-poky-linux"


@pytest.fixture
def fleet(tmp_path):
    data = tmp_path / "data"
    key = tmp_path / "release.key"
    cli.init(data, "fleet.test", ["127.0.0.1"], key)
    client = TestClient(create_app(data))
    admin = {"Authorization": "Bearer " + (data / "admin.token").read_text().strip()}
    return client, admin, key, data


class Device:
    """A device as aember-auth does it: Ed25519 key, enroll, login."""

    def __init__(self, client):
        self.client = client
        self.key = Ed25519PrivateKey.generate()
        self.pub = security.b64(
            self.key.public_key().public_bytes(
                serialization.Encoding.Raw, serialization.PublicFormat.Raw
            )
        )
        self.id = None

    def enroll(self, join_token, name="dev"):
        r = self.client.post(
            "/api/v1/enroll",
            json={"join_token": join_token, "public_key": self.pub, "name": name,
                  "info": {"arch": ARCH}},
        )
        if r.status_code == 200:
            self.id = r.json()["device_id"]
        return r

    def login(self):
        nonce = self.client.post(
            "/api/v1/auth/challenge", json={"device_id": self.id}).json()["nonce"]
        sig = security.b64(self.key.sign(security.challenge_message(self.id, nonce)))
        r = self.client.post(
            "/api/v1/auth/token",
            json={"device_id": self.id, "nonce": nonce, "signature": sig})
        assert r.status_code == 200, r.text
        return {"Authorization": "Bearer " + r.json()["token"]}


def join_token(client, admin, **kw):
    r = client.post("/api/v1/admin/join-tokens", json=kw, headers=admin)
    assert r.status_code == 200
    return r.json()["token"]


def upload(client, admin, key, tmp_path, container, version, data=b"image", arch=ARCH,
           tamper=None):
    img = tmp_path / f"{container}-{version}.squashfs"
    img.write_bytes(data)
    seed = security.unb64(key.read_text().strip())
    manifest = security.canonical_manifest(
        container, version, arch, security.sha256_file(img), len(data))
    sig = security.sign_manifest(security.release_key_from_seed(seed), manifest)
    body = data if tamper != "body" else data + b"!"
    if tamper == "signature":
        sig = security.b64(b"\0" * 64)
    return client.put(
        "/api/v1/admin/artifacts", content=body,
        headers={**admin, "X-Aember-Manifest": security.b64(manifest.encode()),
                 "X-Aember-Signature": sig})


def test_enrollment_and_login(fleet):
    client, admin, _, _ = fleet
    token = join_token(client, admin, group="lab", uses=1)
    d = Device(client)
    assert d.enroll("wrong").status_code == 403
    assert d.enroll(token).status_code == 200
    # The same key again: the same device, no join token use needed.
    again = Device(client)
    again.key, again.pub = d.key, d.pub
    assert again.enroll("whatever").json()["device_id"] == d.id
    # The token is used up.
    assert Device(client).enroll(token).status_code == 403
    headers = d.login()
    assert client.get("/api/v1/device/desired", headers=headers).status_code == 200
    devices = client.get("/api/v1/admin/devices", headers=admin).json()["devices"]
    assert [(x["id"], x["group"], x["arch"]) for x in devices] == [(d.id, "lab", ARCH)]


def test_login_needs_the_device_key_and_a_fresh_nonce(fleet):
    client, admin, _, _ = fleet
    d = Device(client)
    d.enroll(join_token(client, admin))
    nonce = client.post("/api/v1/auth/challenge", json={"device_id": d.id}).json()["nonce"]
    other = Ed25519PrivateKey.generate()
    bad = security.b64(other.sign(security.challenge_message(d.id, nonce)))
    r = client.post("/api/v1/auth/token",
                    json={"device_id": d.id, "nonce": nonce, "signature": bad})
    assert r.status_code == 401
    good = security.b64(d.key.sign(security.challenge_message(d.id, nonce)))
    r = client.post("/api/v1/auth/token",  # the nonce went with the failed try
                    json={"device_id": d.id, "nonce": nonce, "signature": good})
    assert r.status_code == 401
    assert client.post("/api/v1/auth/challenge", json={"device_id": "nope"}).status_code == 404
    for h in ({}, {"Authorization": "Bearer junk"}, {"Authorization": "Basic x"}):
        assert client.get("/api/v1/device/desired", headers=h).status_code == 401


def test_uploads_need_a_valid_release_signature(fleet, tmp_path):
    client, admin, key, _ = fleet
    assert upload(client, admin, key, tmp_path, "echo", "1", tamper="signature").status_code == 403
    assert upload(client, admin, key, tmp_path, "echo", "1", tamper="body").status_code == 422
    assert upload(client, admin, key, tmp_path, "echo", "1").status_code == 200
    assert upload(client, admin, key, tmp_path, "echo", "1").status_code == 409
    assert client.put("/api/v1/admin/artifacts", content=b"x").status_code in (401, 422)
    arts = client.get("/api/v1/admin/artifacts", headers=admin).json()["artifacts"]
    assert [(a["container"], a["version"]) for a in arts] == [("echo", "1")]


def test_rollout_to_a_group_and_a_device(fleet, tmp_path, ):
    client, admin, key, data = fleet
    pub = (data / "release.pub").read_text().strip()
    a = Device(client)
    b = Device(client)
    a.enroll(join_token(client, admin, group="lab"), name="a")
    b.enroll(join_token(client, admin, group="lab"), name="b")
    ha, hb = a.login(), b.login()
    upload(client, admin, key, tmp_path, "echo", "1", b"one")
    upload(client, admin, key, tmp_path, "echo", "2", b"two")
    upload(client, admin, key, tmp_path, "echo", "3", b"three", arch="aarch64-poky-linux")

    assert client.post("/api/v1/admin/assign", headers=admin,
                       json={"container": "echo", "version": "9", "group": "lab"}).status_code == 404
    assert client.post("/api/v1/admin/assign", headers=admin,
                       json={"container": "echo", "version": "1"}).status_code == 422
    assert client.post("/api/v1/admin/assign", headers=admin,
                       json={"container": "echo", "version": "1", "group": "lab"}).status_code == 200
    assert client.post("/api/v1/admin/assign", headers=admin,
                       json={"container": "echo", "version": "2", "device": b.id}).status_code == 200

    da = client.get("/api/v1/device/desired", headers=ha).json()["containers"]
    db = client.get("/api/v1/device/desired", headers=hb).json()["containers"]
    assert [(c["name"], c["version"]) for c in da] == [("echo", "1")]
    assert [(c["name"], c["version"]) for c in db] == [("echo", "2")]  # device wins
    for c in da + db:  # what the agent checks
        assert security.verify(pub, c["manifest"].encode(), c["signature"])
        assert json.loads(c["manifest"])["arch"] == ARCH

    # Only an assigned artifact can be downloaded, by its device.
    r = client.get(f"/api/v1/device/artifacts/{da[0]['artifact']}", headers=ha)
    assert r.status_code == 200 and r.content == b"one"
    assert client.get(f"/api/v1/device/artifacts/{db[0]['artifact']}", headers=ha).status_code == 404

    # Version 3 exists only for aarch64: an x64 device gets nothing.
    client.post("/api/v1/admin/assign", headers=admin,
                json={"container": "echo", "version": "3", "group": "lab"})
    assert client.get("/api/v1/device/desired", headers=ha).json()["containers"] == []

    assert client.post("/api/v1/admin/unassign", headers=admin,
                       json={"container": "echo", "device": b.id}).status_code == 200
    assert client.post("/api/v1/admin/unassign", headers=admin,
                       json={"container": "echo", "device": b.id}).status_code == 404


def test_the_agent_sets_the_target(fleet, tmp_path):
    client, admin, key, _ = fleet
    d = Device(client)
    r = client.post("/api/v1/enroll", json={
        "join_token": join_token(client, admin), "public_key": d.pub, "name": "cpu",
        "info": {"arch": "x86_64"}})  # what aember-auth knows
    d.id = r.json()["device_id"]
    h = d.login()
    upload(client, admin, key, tmp_path, "echo", "1", b"one")
    client.post("/api/v1/admin/assign", headers=admin,
                json={"container": "echo", "version": "1", "group": "default"})
    assert client.get("/api/v1/device/desired", headers=h).json()["containers"] == []
    got = client.get(f"/api/v1/device/desired?arch={ARCH}", headers=h).json()
    assert [c["version"] for c in got["containers"]] == ["1"]
    devices = client.get("/api/v1/admin/devices", headers=admin).json()["devices"]
    assert devices[0]["arch"] == ARCH
    assert client.get("/api/v1/device/desired?arch=../x", headers=h).status_code == 422


def test_status_reports(fleet):
    client, admin, _, _ = fleet
    d = Device(client)
    d.enroll(join_token(client, admin))
    h = d.login()
    status = {"containers": [{"name": "echo", "version": "1", "state": "running"}]}
    assert client.post("/api/v1/device/status", json=status, headers=h).status_code == 200
    dev = client.get("/api/v1/admin/devices", headers=admin).json()["devices"][0]
    assert dev["status"] == status and dev["last_seen"] is not None


def test_admin_needs_the_admin_token(fleet):
    client, _, _, _ = fleet
    for method, path in (("GET", "/api/v1/admin/devices"),
                         ("POST", "/api/v1/admin/join-tokens"),
                         ("GET", "/api/v1/admin/artifacts")):
        r = client.request(method, path, json={}, headers={"Authorization": "Bearer nope"})
        assert r.status_code == 401
