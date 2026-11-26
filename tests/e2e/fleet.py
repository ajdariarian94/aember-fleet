"""
End-to-end tests of aember-fleet: the fleet server (its Docker image) and a
device (aember's packed image in QEMU) with aember-auth and the
aember-fleet-agent installed.

They are aember system-test scenarios, loaded by aember's CLI:

    aember systemtest --scenarios <this file> \\
        --input auth=<aember-auth bundle> --input agent=<agent bundle> \\
        --input server-image=aember-fleet

`tools/e2e.sh` builds the inputs and runs this (README "End-to-end tests").
The bundles are what `aember-cli/run.py pack -a x64-poky-linux` writes
(build/aember-out/<arch>/<name>/). QEMU reaches the server on the host at
10.0.2.2, so the server runs in the host's network namespace (`--network
host`, like the container this runs in).

`FleetServer` and `device_image` are also what a fleet demo needs: a server
and an enrolled device in one go.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from aember.steps.testing.system.scenarios import System, scenario, wait_all_running
from aember.steps.testing.system.vm import Vm
from aember.utils import aember_package

ARCH = "x64-poky-linux"
GROUP = "e2e"
# The certificate's name: the device checks it (SNI + name), but connects
# to QEMU's host address (auth.json "connect").
SERVER_NAME = "fleet.test"
HOST_FROM_QEMU = "10.0.2.2"
POLL_INTERVAL = 5  # s; the server tells the agents
HEALTH_SECONDS = 10  # how long a new version must stay up

ECHO = "aember-echo"
ECHO_IMAGE = f"/var/lib/aember/{ECHO}/image.squashfs"  # installed package
ECHO_SCRIPT = "echo-alpine.sh"  # what aember-echo's config.json runs
AGENT_LOG = "/var/log/aember-container-aember-fleet-agent.log"
AGENT_STATE = "/var/lib/aember-fleet/state.json"
AUTH_DEVICE = "/var/lib/aember-auth/device.json"

UP_TIMEOUT = 120
ENROLL_TIMEOUT = 90
UPDATE_TIMEOUT = 120


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class FleetServer:
    """The fleet server in a Docker container on the host network: `init`
    (CA, server certificate, admin token, release key) and `serve`. Admin
    commands run in the container (`aember-fleet ...`), the release key
    never leaves it."""

    def __init__(self, image: str, poll_interval: int = POLL_INTERVAL) -> None:
        self.image = image
        self.port = _free_port()
        self.name = f"aember-fleet-e2e-{os.getpid()}-{self.port}"
        self.poll_interval = poll_interval

    @property
    def url(self) -> str:
        return f"https://{SERVER_NAME}:{self.port}"

    def __enter__(self) -> FleetServer:
        script = (
            "mkdir -p /keys && aember-fleet --data /data init "
            f"--name {SERVER_NAME} --ip 127.0.0.1 --ip {HOST_FROM_QEMU} "
            "--release-key /keys/release.key >/dev/null && "
            f"exec aember-fleet --data /data serve --port {self.port}"
        )
        subprocess.run(
            ["docker", "run", "-d", "--rm", "--name", self.name]
            + ["--network", "host", "--entrypoint", "sh"]
            + ["-e", f"AEMBER_FLEET_POLL_INTERVAL={self.poll_interval}"]
            + [self.image, "-c", script],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 60
        while True:
            try:
                self.admin("devices")
                return self
            except (subprocess.CalledProcessError, json.JSONDecodeError):
                if time.monotonic() > deadline:
                    raise AssertionError(
                        f"fleet server didn't come up:\n{self.logs()}"
                    ) from None
                time.sleep(1)

    def __exit__(self, *exc: object) -> None:
        subprocess.run(
            ["docker", "rm", "-f", self.name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def logs(self) -> str:
        out = subprocess.run(
            ["docker", "logs", "--tail", "50", self.name],
            capture_output=True,
            text=True,
        )
        return out.stdout + out.stderr

    def exec(self, *args: str) -> str:
        return subprocess.run(
            ["docker", "exec", self.name, *args],
            check=True,
            capture_output=True,
            text=True,
        ).stdout

    def admin(self, *args: str) -> dict:
        out = self.exec(
            "aember-fleet",
            "--data",
            "/data",
            "--server",
            f"https://127.0.0.1:{self.port}",
            *args,
        )
        return json.loads(out)

    def file(self, path: str) -> str:
        return self.exec("cat", path)

    def join_token(self, uses: int = 1) -> str:
        return self.admin("join-token", "--group", GROUP, "--uses", str(uses))["token"]

    def upload(self, package: Path, container: str, version: str) -> None:
        """Upload a release: the container's .aember package."""
        inside = f"/tmp/{container}-{version}.aember"
        subprocess.run(
            ["docker", "cp", str(package), f"{self.name}:{inside}"],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        self.admin(
            "upload",
            inside,
            "--container",
            container,
            "--version",
            version,
            "--arch",
            ARCH,
            "--release-key",
            "/keys/release.key",
        )

    def assign(self, container: str, version: str) -> None:
        self.admin("assign", container, version, "--group", GROUP)

    def devices(self) -> list[dict]:
        return self.admin("devices")["devices"]

    def container_status(self, container: str) -> dict:
        """What the (only) device last reported for @p container."""
        for device in self.devices():
            for c in (device.get("status") or {}).get("containers", []):
                if c.get("name") == container:
                    return c
        return {}


def device_image(system: System, server: FleetServer, label: str) -> Path:
    """A copy of the packed image with aember-auth and the agent installed
    and set up for @p server (a join token for one device)."""
    files = {
        "/etc/aember/auth/auth.json": json.dumps(
            {
                "server": server.url,
                "connect": HOST_FROM_QEMU,
                "ca": "/etc/aember-auth/ca.pem",
                "join_token": server.join_token(),
                "name": f"e2e-{label}",
            },
            indent=2,
        ),
        "/etc/aember/auth/ca.pem": server.file("/data/tls/ca.crt"),
        "/etc/aember/fleet/fleet.json": json.dumps(
            {
                "release_public_key": server.file("/data/release.pub").strip(),
                "arch": ARCH,
                "health_seconds": HEALTH_SECONDS,
            },
            indent=2,
        ),
    }
    bundles = [Path(system.input("auth")), Path(system.input("agent"))]
    return system.image_with_containers(bundles, files, label)


@dataclass
class EchoVersion:
    package: Path  # the release: <work>/<version>/aember-echo.aember
    image: Path  # the SquashFS in it (what the device installs)


def echo_versions(system: System, versions: dict[str, str]) -> dict[str, EchoVersion]:
    """aember-echo packages derived from the installed one, each with its
    own echo-alpine.sh (version -> script); config.json and
    lxc.container.conf stay (it still runs `/bin/sh /echo-alpine.sh`).
    Unsigned: aember's boards allow that."""
    work = system.log_dir / f"{system.name}-echo"
    shutil.rmtree(work, ignore_errors=True)
    original = system.image_copy_out(ECHO_IMAGE, work / "original.squashfs")
    config = system.container_config(ECHO)
    lxc = system.image_file(f"/var/lib/aember/{ECHO}/lxc.container.conf") or ""
    root = work / "root"
    subprocess.run(
        ["unsquashfs", "-no-xattrs", "-d", str(root), str(original)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    out = {}
    for version, script in versions.items():
        (root / ECHO_SCRIPT).write_text(script)
        image = work / f"{ECHO}-{version}.squashfs"
        subprocess.run(
            ["mksquashfs", str(root), str(image), "-all-root", "-noappend"]
            + ["-comp", "xz", "-quiet"],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        package = work / version / f"{ECHO}.aember"
        package.parent.mkdir(parents=True)
        aember_package.write_package(package, config, lxc, image)
        out[version] = EchoVersion(package, image)
    return out


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


ECHO_GOOD = """#!/bin/sh
# aember-echo {version} (aember-fleet end-to-end test)
while true; do
    echo "$(date '+%Y-%m-%d %H:%M:%S') - echo {version}" >> /tmp/echo-output.txt
    sleep 1
done
"""
ECHO_CRASHES = """#!/bin/sh
# aember-echo {version}: exits at once (the agent must roll it back)
exit 1
"""


def device_echo_sha(vm: Vm) -> str:
    return vm.sh(f"sha256sum {ECHO_IMAGE}").split()[0]


def running_echo_version(vm: Vm) -> str:
    """The version line of the running aember-echo's script (its rootfs is
    the mounted image)."""
    code, out = vm.run(f"head -2 /var/lib/aember/containers/{ECHO}/{ECHO_SCRIPT}")
    return out if code == 0 else ""


def wait_enrolled(server: FleetServer, vm: Vm) -> dict:
    """The device enrolled, logged in and reported (the agent's status
    arrives through aember-auth)."""

    def reported() -> dict | None:
        devices = server.devices()
        if len(devices) == 1 and devices[0].get("status"):
            return devices[0]
        return None

    try:
        return vm.wait_until(reported, ENROLL_TIMEOUT, "device enrolled and reported")
    except AssertionError as e:
        raise AssertionError(f"{e}\n--- fleet server ---\n{server.logs()}") from None


def wait_status(server: FleetServer, vm: Vm, version: str, states: set[str]) -> dict:
    """The device reports aember-echo at @p version in one of @p states."""

    def check() -> dict | None:
        c = server.container_status(ECHO)
        if c.get("version") == version and c.get("state") in states:
            return c
        return None

    try:
        return vm.wait_until(
            check,
            UPDATE_TIMEOUT,
            f"{ECHO} {version} {'/'.join(sorted(states))} on the server",
        )
    except AssertionError as e:
        raise AssertionError(f"{e}\n{diagnostics(server, vm)}") from None


def diagnostics(server: FleetServer, vm: Vm) -> str:
    """What the server and the agent say, for a failure message."""
    _, log = vm.run(f"tail -n 30 {AGENT_LOG}")
    _, state = vm.run(f"cat {AGENT_STATE}")
    devices = json.dumps(server.devices(), indent=1)
    return (
        f"--- agent log ---\n{log}\n--- agent state ---\n{state}\n"
        f"--- server devices ---\n{devices}\n--- server log ---\n{server.logs()}"
    )


def boot_up(system: System, vm: Vm) -> None:
    """Up: the packed services and containers, and auth and the agent
    (added to this image's containers.json) run."""
    vm.wait_ready(UP_TIMEOUT)
    vm.open_shell()
    wait_all_running(system, vm)
    vm.wait_until(
        lambda: all(
            vm.states().get(n) == "running"
            for n in ("aember-auth", "aember-fleet-agent")
        ),
        UP_TIMEOUT,
        "aember-auth and aember-fleet-agent running",
    )


@scenario(
    "fleet-update",
    "Enroll over TLS; a signed aember-echo v2 is installed; a v3 that exits "
    "at once is rolled back to v2 and marked failed",
)
def fleet_update(system: System) -> None:
    images = echo_versions(
        system,
        {"2": ECHO_GOOD.format(version="v2"), "3": ECHO_CRASHES.format(version="v3")},
    )
    with FleetServer(system.input("server-image")) as server:
        image = device_image(system, server, system.name)
        with system.boot(rootfs=image) as vm:
            boot_up(system, vm)
            device = wait_enrolled(server, vm)
            if vm.run(f"test -s {AUTH_DEVICE}")[0] != 0:
                raise vm.fail(f"no {AUTH_DEVICE} after enrolling")
            if device["group"] != GROUP or device["arch"] != ARCH:
                raise vm.fail(f"enrolled as {device}")

            # v2: downloaded, checked, swapped in, stays up.
            server.upload(images["2"].package, ECHO, "2")
            server.assign(ECHO, "2")
            wait_status(server, vm, "2", {"updated", "current"})
            if device_echo_sha(vm) != sha256(images["2"].image):
                raise vm.fail("the device's aember-echo image isn't v2")
            vm.wait_until(
                lambda: "aember-echo v2" in running_echo_version(vm),
                30,
                "aember-echo runs v2's script",
            )
            vm.wait_until(
                lambda: vm.states().get(ECHO) == "running", 60, f"{ECHO} running"
            )

            # v3 exits at once: rolled back to v2, never tried again.
            server.upload(images["3"].package, ECHO, "3")
            server.assign(ECHO, "3")
            status = wait_status(server, vm, "2", {"rolled-back", "failed"})
            if "3" not in status.get("failed_versions", []):
                raise vm.fail(f"v3 isn't marked failed: {status}")
            if device_echo_sha(vm) != sha256(images["2"].image):
                raise vm.fail("after the rollback the image isn't v2")
            vm.wait_until(
                lambda: vm.states().get(ECHO) == "running",
                60,
                f"{ECHO} running again (v2)",
            )
            log = vm.sh(f"cat {AGENT_LOG}")
            if "3 didn't stay up: rolling back" not in log:
                raise vm.fail(f"no rollback in the agent's log:\n{log}")
            # Next round: still v2, v3 refused without another try.
            wait_status(server, vm, "2", {"failed"})


@scenario(
    "fleet-power-loss",
    "Power is cut while a new aember-echo is on trial; after the reboot the "
    "agent rolls back the half-done update, the image is never lost, and the "
    "update then goes through",
)
def fleet_power_loss(system: System) -> None:
    images = echo_versions(system, {"2": ECHO_GOOD.format(version="v2")})
    with FleetServer(system.input("server-image")) as server:
        # Its own disk, booted twice: the second boot sees what the first
        # one left (no -snapshot).
        image = device_image(system, server, system.name)
        original = None
        with system.boot(rootfs=image, snapshot=False) as vm:
            boot_up(system, vm)
            wait_enrolled(server, vm)
            original = device_echo_sha(vm)
            server.upload(images["2"].package, ECHO, "2")
            server.assign(ECHO, "2")

            # The update is pending (saved before the container stops) and
            # v2 on trial for HEALTH_SECONDS: cut the power now.
            def pending() -> bool:
                code, out = vm.run(f"cat {AGENT_STATE}")
                return code == 0 and '"pending": "2"' in out

            vm.wait_until(pending, UPDATE_TIMEOUT, "update to v2 pending", 0.3)
        # (leaving the with kills QEMU: a power loss)

        with system.boot(rootfs=image, snapshot=False) as vm:
            vm.wait_ready(UP_TIMEOUT)
            vm.open_shell()
            # The image is there at every moment: the old one or v2.
            sha = device_echo_sha(vm)
            if sha not in (original, sha256(images["2"].image)):
                raise vm.fail("aember-echo's image is neither the old one nor v2")
            vm.wait_until(
                lambda: "update was interrupted (1), rolling back"
                in vm.run(f"cat {AGENT_LOG}")[1],
                UP_TIMEOUT,
                "the agent rolls the interrupted update back",
            )
            # Interrupted once: tried again, and this time it stays.
            wait_status(server, vm, "2", {"updated", "current"})
            if device_echo_sha(vm) != sha256(images["2"].image):
                raise vm.fail("after the retry the image isn't v2")
            vm.wait_until(
                lambda: vm.states().get(ECHO) == "running", 60, f"{ECHO} running"
            )
