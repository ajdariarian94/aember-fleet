from aember_fleet import security


def test_token_round_trip_and_expiry():
    t = security.make_token("s3cret", "dev-1", ttl=60, now=1000)
    assert security.check_token("s3cret", t, now=1030) == "dev-1"
    assert security.check_token("s3cret", t, now=1061) is None  # expired
    assert security.check_token("other", t, now=1030) is None  # wrong secret
    payload, mac = t.split(".")
    forged = security.make_token("s3cret", "dev-2", ttl=60, now=1000).split(".")[0]
    assert security.check_token("s3cret", f"{forged}.{mac}", now=1030) is None
    for junk in ("", "a.b", "no-dot", "x.y.z"):
        assert security.check_token("s3cret", junk) is None


def test_manifest_is_canonical_and_signed():
    m = security.canonical_manifest("echo", "2", "x64-poky-linux", "ab" * 32, 10)
    assert m == (
        '{"arch":"x64-poky-linux","container":"echo","sha256":"'
        + "ab" * 32
        + '","size":10,"version":"2"}'
    )
    seed, pub = security.generate_release_key()
    sig = security.sign_manifest(security.release_key_from_seed(seed), m)
    assert security.verify(pub, m.encode(), sig)
    assert not security.verify(pub, (m + " ").encode(), sig)
    assert not security.verify(pub, m.encode(), "AAAA")
    assert not security.verify("bm90IGEga2V5", m.encode(), sig)


def test_base64_is_strict():
    assert security.unb64("YWJj") == b"abc"
    for junk in ("YW Jj", "YWJj!", "YWJ"):  # non-alphabet bytes, bad padding
        try:
            security.unb64(junk)
        except ValueError:
            continue
        raise AssertionError(f"{junk!r} was accepted")


def test_tokens_of_every_length_round_trip_without_padding():
    # base64url without "=": the padding back on decode must fit each length.
    for n in range(1, 12):
        device_id = "d" * n
        t = security.make_token("s", device_id, ttl=60, now=1000)
        assert "=" not in t
        assert security.check_token("s", t, now=1000) == device_id


def test_a_token_is_valid_up_to_its_expiry_second():
    t = security.make_token("s", "dev", ttl=60, now=1000)
    assert security.check_token("s", t, now=1060) == "dev"
    assert security.check_token("s", t, now=1060.5) is None


def test_sha256_of_a_file_bigger_than_one_read(tmp_path):
    import hashlib

    data = bytes(range(256)) * (5 * 4096 + 3)  # > 1 MiB, not a multiple
    p = tmp_path / "img"
    p.write_bytes(data)
    assert security.sha256_file(p) == hashlib.sha256(data).hexdigest()
    (tmp_path / "empty").write_bytes(b"")
    assert security.sha256_file(tmp_path / "empty") == hashlib.sha256(b"").hexdigest()
