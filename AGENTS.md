# AGENTS.md

Guidance for AI coding agents working in **aember-fleet**.

## What this is

Container updates for aember fleets (README.md). Two parts:

- `server/`: Python 3.12, FastAPI + SQLite (`store.py`), `security.py`
  (Ed25519 via `cryptography`, HMAC device tokens, the canonical release
  manifest), `app.py` (the API), `cli.py` (`aember-fleet`: init, serve,
  admin commands). Tests: pytest with FastAPI's TestClient, run in Docker
  (`server/Dockerfile.test`); there is no Python venv with these packages
  in the dev container.
- the repository root: the device agent, C++23, built like aember-stats
  with the shared `aember-cli/` (`TESTS = "sdk"`, `RESOURCES_VERSION =
  "0.1.15"`: wolfSSL and nlohmann/json in the SDK). `updater.h` is the logic
  behind two interfaces (`FleetApi`, `ContainerControl`); `clients.h` has
  the real ones (aember-auth's socket, aember-containerd's control socket);
  the tests use fakes.

## Interfaces (change both sides together)

- Release manifest: `security.canonical_manifest` (sorted keys, no
  spaces), signed as is; the agent verifies the exact bytes
  (`manifest.cpp`), then container, version and arch, then size and
  SHA-256 of the download.
- Device API paths and JSON: `app.py` and aember-auth's `fleet-client`
  and the agent's `clients.cpp`. The agent sends `?arch=<aember target>`
  with `desired`; aember-auth only knows the CPU.
- aember-containerd's control socket (aember `services/containerd/
  include/.../control.h`).

## Rules

- The release private key never goes to the server: the server and the
  devices only verify. Uploads without a valid signature are refused.
- The agent never updates itself, and only containers in the device's
  `containers.json`.
- wolfSSL: `<wolfssl/options.h>` first; its compat macros rename words
  like `Sha256` (hence `Sha256Hasher`).
- Commit as Arian Ajdari; branches + PRs; merge when tests pass.
