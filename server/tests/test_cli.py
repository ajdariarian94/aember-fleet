"""The admin CLI end to end against the app: Admin.call goes to a
TestClient instead of HTTPS, the rest is the real CLI."""

import json

import pytest
from fastapi.testclient import TestClient

from aember_fleet import cli, security
from aember_fleet.app import create_app

ARCH = "x64-poky-linux"


@pytest.fixture
def run(tmp_path, monkeypatch, capsys):
    data = tmp_path / "data"
    key = tmp_path / "release.key"
    assert cli.main(["--data", str(data), "init", "--name", "fleet.test",
                     "--ip", "127.0.0.1", "--release-key", str(key)]) == 0
    capsys.readouterr()
    client = TestClient(create_app(data))
    seen = []

    def call(self, method, path, body=None, raw=None, headers=None):
        h = {"Authorization": f"Bearer {self.token}", **(headers or {})}
        seen.append((self.server, method, path, body))
        r = client.request(method, path, json=body if raw is None else None,
                           content=raw, headers=h)
        if r.status_code >= 400:
            raise SystemExit(f"{method} {path}: {r.status_code} {r.text}")
        return r.json()

    monkeypatch.setattr(cli.Admin, "call", call)

    def run_cli(*args):
        assert cli.main(["--data", str(data), "--server", "https://f:1", *args]) == 0
        return json.loads(capsys.readouterr().out)

    run_cli.data, run_cli.key, run_cli.client, run_cli.seen = data, key, client, seen
    return run_cli


def test_join_token_devices_and_group(run):
    out = run("join-token", "--group", "lab", "--uses", "2", "--ttl-hours", "1")
    assert out["token"]
    assert run.seen[-1] == ("https://f:1", "POST", "/api/v1/admin/join-tokens",
                            {"group": "lab", "uses": 2, "ttl_hours": 1.0})
    r = run.client.post("/api/v1/enroll", json={
        "join_token": out["token"], "public_key": security.b64(b"k" * 32),
        "name": "a", "info": {"arch": ARCH}})
    device_id = r.json()["device_id"]
    assert [d["group"] for d in run("devices")["devices"]] == ["lab"]
    run("group", device_id, "kitchen")
    assert run.seen[-1][1:3] == ("PUT", f"/api/v1/admin/devices/{device_id}/group")
    assert [d["group"] for d in run("devices")["devices"]] == ["kitchen"]


def test_upload_signs_with_the_release_key_and_assign(run, tmp_path):
    img = tmp_path / "echo.squashfs"
    img.write_bytes(b"image bytes")
    out = run("upload", str(img), "--container", "echo", "--version", "2",
              "--arch", ARCH, "--release-key", str(run.key))
    assert out["artifact"]
    arts = run("artifacts")["artifacts"]
    assert [(a["container"], a["version"], a["arch"]) for a in arts] == [("echo", "2", ARCH)]
    pub = (run.data / "release.pub").read_text().strip()
    manifest = security.canonical_manifest(
        "echo", "2", ARCH, security.sha256_file(img), img.stat().st_size)
    import sqlite3

    db = sqlite3.connect(run.data / "fleet.db")
    stored, signature = db.execute("SELECT manifest, signature FROM artifacts").fetchone()
    assert stored == manifest
    assert security.verify(pub, manifest.encode(), signature)

    run("assign", "echo", "2", "--group", "lab")
    assert run.seen[-1][2:] == ("/api/v1/admin/assign",
                                {"container": "echo", "group": "lab", "device": None,
                                 "version": "2"})
    run("unassign", "echo", "--group", "lab")
    assert run.seen[-1][2:] == ("/api/v1/admin/unassign",
                                {"container": "echo", "group": "lab", "device": None})
    with pytest.raises(SystemExit):  # nothing assigned any more
        run("unassign", "echo", "--group", "lab")


def test_admin_token_and_ca_come_from_the_data_dir(run, tmp_path):
    a = cli.Admin("https://f:1/", "t", None)
    assert a.server == "https://f:1" and a.ctx is None
    ca = cli.Admin("https://f:1", "t", str(run.data / "tls" / "ca.crt"))
    assert ca.ctx is not None
    run("devices")
    token = (run.data / "admin.token").read_text().strip()
    assert run.client.get("/api/v1/admin/devices",
                          headers={"Authorization": f"Bearer {token}"}).status_code == 200
    # Another token on the command line wins.
    with pytest.raises(SystemExit):
        cli.main(["--data", str(run.data), "--token", "wrong", "devices"])
