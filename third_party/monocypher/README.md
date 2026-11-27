# Monocypher 4.0.2 (vendored)

Ed25519 (RFC 8032) to verify release signatures: `monocypher.c/.h` and the optional
`monocypher-ed25519.c/.h`, unmodified.

- Source: https://monocypher.org/download/monocypher-4.0.2.tar.gz
  (sha256 `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`),
  checked against the `4.0.2` tag on GitHub (LoupVaillant/Monocypher): the
  files are the same apart from the version line.
- Licence: CC0-1.0 or BSD-2-Clause, at our choice (`LICENCE.md`). We use it
  under BSD-2-Clause, so the notice goes into the image's third-party
  notices.

It replaced wolfCrypt's Ed25519 in the agent (wolfSSL is GPL-2.0-only; see aember's
`doc/licensing/`). To update: replace the four files and this README's
version and hash together.
