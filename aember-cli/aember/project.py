"""What makes this repository different from its siblings that share the CLI
(aember-stats, aember-monitor, aember-auth): the container name and
what it needs."""

from __future__ import annotations

NAME = "aember-fleet-agent"
# aember-resources release with the SDKs this repo builds with; v0.1.15 adds
# wolfSSL (TLS client, Ed25519) to the SDKs.
RESOURCES_VERSION = "0.1.15"
RESOURCES_REPO = "ajdariarian94/aember-resources"
# Unit tests: "host" or "sdk" (built with the x64 SDK and run through its
# loader: they need the SDK's wolfSSL).
TESTS = "sdk"
