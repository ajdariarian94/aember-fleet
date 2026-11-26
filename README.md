# aember-fleet

Keeps the containers of a fleet of [aember](https://github.com/ajdariarian94/aember)
devices at the versions you choose: signed releases, rollout per group or
device, automatic rollback on a device when a new version doesn't stay up.
(Updates of aember itself come later.)

```
admin ── aember-fleet CLI ──► server (Docker: FastAPI + SQLite, TLS)
           signs releases          ▲  HTTPS (device login: aember-auth)
                                   │
device:  aember-auth ◄── socket ── aember-fleet-agent ── control socket ──► aember-containerd
         (device key, login)       (download, check, swap, watch)
```

- **server/**: the fleet server and the admin CLI (Python).
- the rest: **aember-fleet-agent**, the device side (C++23, an aember LXC
  container, ~2.7 MB). It reaches the server only through
  [aember-auth](https://github.com/ajdariarian94/aember-auth)'s socket, so it
  never holds the device key or a TLS stack.

## How an update goes

1. The admin uploads a container's **`.aember` package** (aember's
   [package format](https://github.com/ajdariarian94/aember/blob/main/doc/aember-format.md),
   what `aember-cli/run.py pack` builds) with a **release signature**
   (Ed25519 over a manifest: container, version, aember target, size,
   SHA-256). The private release key stays with the admin; the server only
   checks it, the devices check it again.
2. The admin assigns a version to a group (`lab`) or a device.
3. The agent polls (`GET /api/v1/device/desired`), checks the signature,
   downloads the package into aember-containerd's inbox
   (`/var/lib/aember/packages/<name>.aember`, renamed into place only once
   complete) and checks size and SHA-256.
4. It saves the update as pending and asks aember-containerd to install it
   (control socket, `install`): containerd stops the container, checks the
   package's own signature against the board's keys, unpacks it with the
   installed version kept as `<name>.prev` (one atomic swap), and starts
   it. The installed version is never missing, and a power loss at any step
   is rolled back by the next agent (an update interrupted twice counts as
   failed).
5. If the container isn't running at any moment of the health window
   (20 s), the agent asks for a `rollback` (back to `<name>.prev`) and marks
   that version failed (not tried again until another is assigned).
6. Every round ends with a status report the admin sees in `devices`.

## Server

```bash
docker build -t aember-fleet server
docker run --rm -u $(id -u) -v $PWD/fleet:/w aember-fleet \
    --data /w/data init --name fleet.example --ip 192.0.2.10 --release-key /w/release.key
docker run -d -p 8443:8443 -u $(id -u) -v $PWD/fleet/data:/data aember-fleet
```

`init` makes the TLS CA and server certificate (for the name and IPs), the
admin token, the token secret and the release key pair. Keep
`release.key` off the server.

Admin commands (`--data` gives the admin token and the CA,
`--server https://fleet.example:8443`):

```bash
aember-fleet join-token --group lab --uses 10
aember-fleet upload aember-echo.aember --container aember-echo --version 2 \
    --arch x64-poky-linux --release-key release.key
aember-fleet assign aember-echo 2 --group lab      # or --device <id>
aember-fleet devices                               # what they run, how it went
aember-fleet unassign aember-echo --group lab
```

`AEMBER_FLEET_POLL_INTERVAL` (seconds, default 60) is what agents are told.

## On a device

Install both containers into the board's overlay (`aember-cli/run.py install
-a <arch> ../aember` here and in aember-auth), then add:

- `etc/aember/auth/auth.json` and `ca.pem` for aember-auth (its README);
- `etc/aember/fleet/fleet.json`:

  ```json
  {"release_public_key": "<data/release.pub>", "arch": "x64-poky-linux",
   "health_seconds": 20}
  ```

`arch` is the board's aember target: releases are per target. The agent
updates containers that are in the board's `containers.json` (not new ones,
and not itself).

## Build and test

```bash
git submodule update --init
aember-cli/run.py test                 # agent unit tests (x64 SDK)
aember-cli/run.py build -a x64-poky-linux && aember-cli/run.py pack -a x64-poky-linux
docker build -f server/Dockerfile.test -t aember-fleet-test server && docker run --rm aember-fleet-test
```

Property-based tests check rules on many generated cases instead of a few
examples: `tests/test_properties.cpp` (base64, signed manifests, the
desired state, the state file, and the updater under random assignments,
agent stops and power losses against the TLA+ model's invariants) and
`server/tests/test_properties.py` (Hypothesis: tokens, manifests,
signatures, the API's answers to any upload or device report). A failing
C++ case prints the seed that reproduces it:
`AEMBER_PROPERTY_SEED=<seed> AEMBER_PROPERTY_RUNS=1 build/.../properties-unittest`.

`spec/tla/check.sh` model-checks the update with TLC (power loss at any
step, the assignment changing at any time; `spec/tla/README.md`).

The agent's inputs (both sockets, the desired state, manifests, fleet.json,
containers.json, its state file) have libFuzzer targets in `fuzz/`. On
Ubuntu 24.04, with `clang-18 llvm-18 libclang-rt-18-dev cmake ninja-build
pkg-config libmbedtls-dev nlohmann-json3-dev` installed: `fuzz/run.sh` (every
target for 60 s, as CI does); crashing inputs land in
`build/fuzz/crashes/<target>/`.

## End-to-end tests

`tests/e2e/fleet.py` runs the server (its Docker image) against an aember
device in QEMU with aember-auth and this agent installed, as aember system-test
scenarios (`aember systemtest --scenarios`):

- **fleet-update**: the device enrolls and logs in over TLS; a signed
  aember-echo v2 is installed and runs; a v3 that exits at once is rolled
  back to v2, marked failed and not tried again.
- **fleet-power-loss**: QEMU is killed while v2 is on trial; after the
  reboot the image is the old one or v2 (never missing), the agent rolls
  the interrupted update back, then installs v2 for good.

```bash
aember-cli/run.py build -a x64-poky-linux && aember-cli/run.py pack -a x64-poky-linux
(cd ../aember-auth && aember-cli/run.py build -a x64-poky-linux && aember-cli/run.py pack -a x64-poky-linux)
AEMBER_IMAGE=packed tools/e2e.sh            # -k fleet-update, --timeout-scale 3, ...
```

`tools/e2e.sh` (header: its inputs) builds the server image and runs the
scenarios in a `docker run` of aember's dev image with the sibling `../aember`
checkout's CLI. `AEMBER_IMAGE=packed` boots that checkout's `aember app
pack` output; the default, `release`, boots aember's latest release (it
needs aember-containerd's control socket, i.e. a release after v0.14.0).
The device's join token, CA and fleet.json are generated per run into a
copy of the image under `../aember/build/system-tests/`, never into a
board's overlay. CI job `e2e` builds aember main, aember-auth main and this
agent, then runs both scenarios (~2 min with KVM).
