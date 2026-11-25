#!/usr/bin/env bash
# End-to-end tests (tests/e2e/fleet.py): the fleet server (Docker) and an
# aember device in QEMU with aember-auth and this agent.
#
#   tools/e2e.sh [-k SCENARIO]... [systemtest options]
#
# Inputs (environment; defaults for the aember_ws layout of sibling repos):
#   AEMBER_DIR    aember checkout (its CLI drives QEMU)           ../aember
#   AEMBER_IMAGE  "release" (default): aember's latest x64 Release image
#                 from its GitHub release, unpacked into the checkout's
#                 build/fleet-e2e/<tag>/; "packed": the checkout's own
#                 build/package/x64-poky-linux/Release (aember app pack)
#   AUTH_BUNDLE   packed aember-auth (aember-cli/run.py pack there)
#                 ../aember-auth/build/aember-out/x64-poky-linux/aember-auth
#   AGENT_BUNDLE  packed agent      build/aember-out/x64-poky-linux/aember-fleet-agent
#   AEMBER_DEV_IMAGE  the image to run in (QEMU, guestfish, squashfs-tools)
#
# Runs in a privileged `docker run` of aember's dev image like aember's CI
# (tools/ci/run.sh): aember at /aember_ws/aember (guestfish's `docker run`
# mounts HOST_ROOT_PATH there), the host's network (QEMU's 10.0.2.2 is the
# host, where the server listens) and Docker socket, /dev/kvm when there.
# Consoles: $AEMBER_DIR/build/system-tests/x64-poky-linux/fleet-*.log.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
aember="$(cd "${AEMBER_DIR:-$here/../aember}" && pwd)"
auth="$(cd "${AUTH_BUNDLE:-$here/../aember-auth/build/aember-out/x64-poky-linux/aember-auth}" && pwd)"
agent="$(cd "${AGENT_BUNDLE:-$here/build/aember-out/x64-poky-linux/aember-fleet-agent}" && pwd)"
image="${AEMBER_DEV_IMAGE:-ghcr.io/ajdariarian94/aember-dev:latest}"
server_image="aember-fleet-e2e"
arch=x64-poky-linux

for b in "$auth" "$agent"; do
  [ -f "$b/container.json" ] || { echo "no packed container in $b" >&2; exit 1; }
done

if [ "${AEMBER_IMAGE:-release}" = release ]; then
  # The image devices run: aember's latest release (kernel, initramfs,
  # rootfs image), unpacked once per tag.
  url="https://github.com/ajdariarian94/aember/releases/latest/download"
  tag="$(curl -fsSLI -o /dev/null -w '%{url_effective}' \
    https://github.com/ajdariarian94/aember/releases/latest | sed 's|.*/tag/||')"
  package="$aember/build/fleet-e2e/$tag"
  echo "aember image: release $tag"
  if [ ! -f "$package/aember-rootfs.img" ]; then
    mkdir -p "$package"
    curl -fsSL "$url/aember-app-$arch-Release-$tag.tar.gz" \
      | tar xz -C "$package" kernel aember-initramfs.cpio.gz aember-rootfs.img
  fi
else
  package="$aember/build/package/$arch/Release"
  echo "aember image: $package (packed)"
fi
for f in kernel aember-initramfs.cpio.gz aember-rootfs.img; do
  [ -f "$package/$f" ] || { echo "missing $package/$f" >&2; exit 1; }
done

docker build -q -t "$server_image" "$here/server" >/dev/null

args=(--rm --privileged --net=host
  -v "$aember:/aember_ws/aember" -w /aember_ws/aember
  -v "$here:/e2e/fleet:ro" -v "$auth:/e2e/auth:ro" -v "$agent:/e2e/agent:ro"
  -e HOST_ROOT_PATH="$aember"
  -e GITHUB_STEP_SUMMARY=/aember_ws/aember/build/ci-summary.md
  -v /var/run/docker.sock:/var/run/docker.sock
  --group-add "$(stat -c %g /var/run/docker.sock)")
if [ -e /dev/kvm ]; then
  args+=(--group-add "$(stat -c %g /dev/kvm)")
fi

# printf %q keeps the arguments intact through bash -c (none: nothing).
extra=""
if [ $# -gt 0 ]; then extra="$(printf '%q ' "$@")"; fi

docker run "${args[@]}" "$image" bash -c "
  set -euo pipefail
  export PATH=/venv/bin:\$PATH
  pip install -q --disable-pip-version-check -r requirements.txt
  git config --global --add safe.directory '*'
  python3 aember-cli/run.py systemtest --architecture $arch \
    --scenarios /e2e/fleet/tests/e2e/fleet.py \
    --input auth=/e2e/auth --input agent=/e2e/agent \
    --package-dir /aember_ws/aember/${package#"$aember"/} \
    --input server-image=$server_image $extra"
