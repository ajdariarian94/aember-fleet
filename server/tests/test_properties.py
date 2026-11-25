"""Property-based tests (Hypothesis): tokens, release manifests and
signatures, and the API's answers to anything a device or an upload sends.

A failing example is printed with the seed that reproduces it
(``--hypothesis-seed``); Hypothesis also keeps it in .hypothesis/ and tries
it first next time.
"""

import base64
import json

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from fastapi.testclient import TestClient
import pytest
from hypothesis import assume, example, given, settings
from hypothesis import strategies as st

from aember_fleet import cli, security
from aember_fleet.app import create_app

from test_api import ARCH, Device, join_token

text = st.text(max_size=40)
hex64 = st.text(alphabet="0123456789abcdef", min_size=64, max_size=64)
# Any JSON value Python's json module reads, NaN and the infinities too.
json_values = st.recursive(
    st.none() | st.booleans() | st.integers() | st.floats() | text,
    lambda inner: st.lists(inner, max_size=4) | st.dictionaries(text, inner, max_size=4),
    max_leaves=12,
)
# The API tests share one fleet (a module fixture: every example talks to
# the same server) and take longer than Hypothesis' default deadline.
api = settings(deadline=None)


# --- device tokens ------------------------------------------------------------


@given(secret=text, device=text, ttl=st.integers(1, 10**6), now=st.integers(0, 2**40),
       later=st.integers(0, 2 * 10**6))
def test_token_valid_until_it_expires(secret, device, ttl, now, later):
    token = security.make_token(secret, device, ttl, now=now)
    got = security.check_token(secret, token, now=now + later)
    assert got == (device if later <= ttl else None)


@given(secret=text, device=text, data=st.data())
def test_a_changed_payload_is_refused(secret, device, data):
    payload, mac = security.make_token(secret, device, 60, now=0).split(".")
    i = data.draw(st.integers(0, len(payload) - 1))
    c = data.draw(st.sampled_from("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"))
    assume(c != payload[i])
    forged = payload[:i] + c + payload[i + 1:]
    assert security.check_token(secret, f"{forged}.{mac}", now=0) is None


@given(token=text)
def test_check_token_never_raises(token):
    got = security.check_token("s3cret", token)
    assert got is None or isinstance(got, str)


@given(raw=st.binary(max_size=64))
def test_check_token_never_raises_on_payloads_with_a_valid_mac(raw):
    """Only the server makes tokens, but a payload it can't read must still
    be a refusal, not an exception."""
    import hashlib
    import hmac

    payload = base64.urlsafe_b64encode(raw).rstrip(b"=").decode()
    mac = hmac.new(b"s3cret", payload.encode(), hashlib.sha256).digest()
    token = f"{payload}.{base64.urlsafe_b64encode(mac).rstrip(b'=').decode()}"
    got = security.check_token("s3cret", token, now=0)
    assert got is None or isinstance(got, str)


# --- release manifests --------------------------------------------------------

KEY = Ed25519PrivateKey.generate()
PUB = security.b64(KEY.public_key().public_bytes_raw())


@given(container=text, version=text, arch=text, sha=hex64, size=st.integers(0, 2**63))
def test_manifest_is_canonical_json_of_its_fields(container, version, arch, sha, size):
    m = security.canonical_manifest(container, version, arch, sha, size)
    fields = json.loads(m)
    assert fields == {"container": container, "version": version, "arch": arch,
                      "sha256": sha, "size": size}
    assert security.canonical_manifest(**{
        "container": fields["container"], "version": fields["version"],
        "arch": fields["arch"], "sha256": fields["sha256"], "size": fields["size"],
    }) == m
    assert m.isascii()  # the same bytes whatever the encoding on the way


@given(version=text, data=st.data())
def test_any_changed_byte_breaks_the_signature(version, data):
    m = security.canonical_manifest("echo", version, ARCH, "a" * 64, 5).encode()
    sig = base64.b64decode(security.sign_manifest(KEY, m.decode()))
    assert security.verify(PUB, m, security.b64(sig))
    i = data.draw(st.integers(0, len(m) - 1))
    x = data.draw(st.integers(1, 255))
    changed = m[:i] + bytes([m[i] ^ x]) + m[i + 1:]
    assert not security.verify(PUB, changed, security.b64(sig))
    j = data.draw(st.integers(0, 63))
    bad = sig[:j] + bytes([sig[j] ^ x]) + sig[j + 1:]
    assert not security.verify(PUB, m, security.b64(bad))


@given(key=text, message=st.binary(max_size=64), signature=text)
def test_verify_never_raises(key, message, signature):
    assert security.verify(key, message, signature) in (True, False)


@given(device=st.text(max_size=20).filter(lambda s: "\n" not in s),
       nonce=st.text(max_size=20).filter(lambda s: "\n" not in s))
def test_challenge_message_has_its_parts(device, nonce):
    assert security.challenge_message(device, nonce).decode().split("\n") == [
        security.CHALLENGE_PREFIX, device, nonce]


# --- the API --------------------------------------------------------------------


@pytest.fixture(scope="module")
def fleet(tmp_path_factory):
    tmp_path = tmp_path_factory.mktemp("fleet")
    data = tmp_path / "data"
    key = tmp_path / "release.key"
    cli.init(data, "fleet.test", ["127.0.0.1"], key)
    client = TestClient(create_app(data))
    admin = {"Authorization": "Bearer " + (data / "admin.token").read_text().strip()}
    seed = security.unb64(key.read_text().strip())
    return client, admin, security.release_key_from_seed(seed)


@api
@given(fields=st.dictionaries(st.sampled_from(
           ["container", "version", "arch", "sha256", "size", "extra"]), json_values),
       canonical=st.booleans(), sign=st.booleans(), body=st.binary(max_size=16))
# Regression: a size of 1e400 (inf) raised OverflowError in int(): a server
# error instead of a 422.
@example(fields={"arch": "a", "container": "c", "sha256": "x", "size": float("inf"),
                 "version": "1"}, canonical=True, sign=False, body=b"")
def test_upload_takes_only_a_signed_canonical_matching_manifest(
        fleet, fields, canonical, sign, body):
    """Anything else is refused with a 4xx, never a server error."""
    client, admin, key = fleet
    manifest = (json.dumps(fields, sort_keys=True, separators=(",", ":")) if canonical
                else json.dumps(fields))
    sig = security.sign_manifest(key, manifest) if sign else security.b64(b"\0" * 64)
    r = client.put("/api/v1/admin/artifacts", content=body,
                   headers={**admin, "X-Aember-Manifest": security.b64(manifest.encode()),
                            "X-Aember-Signature": sig})
    assert r.status_code in (200, 403, 409, 422), r.text
    if r.status_code == 200:
        assert sign
        assert manifest == security.canonical_manifest(
            fields["container"], fields["version"], fields["arch"], fields["sha256"],
            fields["size"])



def post_json(client, path, body, headers=None):
    """As a device may send it: NaN and Infinity too (httpx's json= refuses
    them, Python's json reads them)."""
    return client.post(path, content=json.dumps(body),
                       headers={"Content-Type": "application/json", **(headers or {})})


@settings(api, max_examples=50)
@given(status=st.lists(st.dictionaries(text, json_values, max_size=4), max_size=3),
       info=st.dictionaries(text, json_values, max_size=4))
def test_whatever_a_device_reports_the_admin_can_list_devices(fleet, status, info):
    """A device's info and reports are taken (200) or refused (422); either
    way the admin's device list keeps working."""
    client, admin, _ = fleet
    device = Device(client)
    r = post_json(client, "/api/v1/enroll", {
        "join_token": join_token(client, admin, group="lab"), "public_key": device.pub,
        "name": "dev", "info": info})
    assert r.status_code in (200, 422), r.text
    if r.status_code == 200:
        device.id = r.json()["device_id"]
        r = post_json(client, "/api/v1/device/status", {"containers": status},
                      device.login())
        assert r.status_code in (200, 422), r.text
    r = client.get("/api/v1/admin/devices", headers=admin)
    assert r.status_code == 200, r.text
