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

1. The admin uploads a container image with a **release signature**
   (Ed25519 over a manifest: container, version, aember target, size,
   SHA-256). The private release key stays with the admin; the server only
   checks it, the devices check it again.
2. The admin assigns a version to a group (`lab`) or a device.
3. The agent polls (`GET /api/v1/device/desired`), checks the signature,
   downloads the image next to the current one and checks size and SHA-256.
4. It stops the container through aember-containerd's control socket,
   keeps the current image as `<image>.prev`, puts the new one in place and
   starts it.
5. If the container isn't running at any moment of the health window
   (20 s), the agent restores `.prev`, starts it again, and marks that
   version failed (not tried again until another is assigned).
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
aember-fleet upload aember-echo.squashfs --container aember-echo --version 2 \
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
aember-cli/run.py test                 # agent unit tests (x64 SDK) + CLI tests
aember-cli/run.py build -a x64-poky-linux && aember-cli/run.py pack -a x64-poky-linux
docker build -f server/Dockerfile.test -t aember-fleet-test server && docker run --rm aember-fleet-test
```

Tested end to end in aember's QEMU x64: enrollment and login over TLS, an
update of aember-echo to a signed v2, and the rollback of a v3 that exits at
once.
