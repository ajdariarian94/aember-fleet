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
  with the shared CLI (`aember-cli/run.py` → aember-build-helper's
  `project-cli/`; `TESTS = "sdk"`, `RESOURCES_VERSION =
  "0.1.15"`: wolfSSL and nlohmann/json in the SDK). `updater.h` is the logic
  behind two interfaces (`FleetApi`, `ContainerControl`); `clients.h` has
  the real ones (aember-auth's socket, aember-containerd's control socket);
  the tests use fakes. `fuzz/`: libFuzzer targets for the agent.

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

## TLA+ model

`spec/tla/FleetUpdate.tla` is the update's design (power loss at any step,
assignment changes). When the updater's steps change, change the model,
run `spec/tla/check.sh` (Docker, seconds) and keep its README's "Where the
code differs" current. Invariants to keep: the image is never missing (no
rename of the image away), the update is saved as pending before the
container is stopped, a pending update is rolled back at start.

## Rules

- The release private key never goes to the server: the server and the
  devices only verify. Uploads without a valid signature are refused.
- The agent never updates itself, and only containers in the device's
  `containers.json`.
- wolfSSL: `<wolfssl/options.h>` first; its compat macros rename words
  like `Sha256` (hence `Sha256Hasher`).
- A test that pins down a fixed bug starts with a comment
  `Regression: <the bug>` (`grep -rn "Regression:"` lists them). Every bug
  fix gets one.
- Fuzzing: `fuzz/run.sh [SECONDS] [TARGET...]` (CI: 60 s per target, the
  `fuzz` job). It is a host build with clang 18 and Ubuntu's wolfSSL and
  nlohmann-json (the SDK has no libFuzzer). Targets: `fuzz_clients` (both
  sockets answering anything), `fuzz_desired`, `fuzz_manifest` (also signed
  with a test key, to reach the JSON checks), `fuzz_agent_config`
  (fleet.json), `fuzz_agent_files` (containers.json and state.json, then a
  round), `fuzz_base64`. A crash gets a fix, a `Regression:` test and its
  input in `fuzz/corpus/<target>/`.
- nlohmann/json: `value()` and `get<T>()` throw on another type, and a
  double converted to int out of range is undefined behaviour. JSON from
  outside the process (sockets, the server, files) is read with type
  checks (`Text()`, `Number()`, `IsTrue()` in updater.cpp/clients.cpp); a
  broken entry is skipped, never fatal.
- Commit as Arian Ajdari; branches + PRs; merge when tests pass.
