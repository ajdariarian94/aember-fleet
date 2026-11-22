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
