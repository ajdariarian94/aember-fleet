# TLA+ model of the update

`FleetUpdate.tla` models aember-fleet-agent updating one container
(`src/updater.cpp`), checked with [TLC](https://github.com/tlaplus/tlaplus):
every interleaving of the agent's steps with power losses (or the agent
being stopped) at any point and the server changing the assignment at any
time.

```bash
spec/tla/check.sh          # both models, a few seconds
spec/tla/check.sh Fleet    # one
```

TLC runs in Docker (`eclipse-temurin:21-jre`, `-Xmx3g`, `--memory 4g`);
`tla2tools.jar` v1.7.4 is downloaded once into `~/.cache/aember-tla` and
checked against its SHA-256. CI job `tla`. aember has the same setup for
its start/restart/shutdown model (aember `spec/tla/`).

## The model

Disk: the image containerd starts, `<image>.prev`, `<image>.new` and the
agent's state file. A power loss keeps the disk and loses the agent's
memory; the next agent starts from its state file.

| Step | Code |
|------|------|
| `Recover`, `Restore`: a pending update is counted as interrupted (saved), then rolled back to `.prev` | `Updater::Recover` |
| `Poll`: nothing to do, a failed version, or update | `RunOnce`, `Update` |
| `Download`: `.new`, size and SHA-256 checked | `Install` |
| `LinkPrev`: `.prev` = hard link of the image (via `.prev.tmp` + rename) | `Install` |
| `SavePending`: the version being installed goes into the state file | `Install` |
| `Stop`, `Swap2` (`rename(.new, image)`, atomic), `Health` | `Install`, `Check` |
| `Rollback`: `rename(.prev, image)`, version marked failed | `RollBack` |
| `Save` | `SaveState` |
| `PowerLoss`, `Assign(v)`: the environment, bounded | |

Constants: `Versions`, `Good` (versions that stay up), `Initial` (the
shipped image), `MaxLosses`, `MaxChanges`, `MaxInterrupts`
(`kMaxInterrupts`, 2), `Fixed` (the agent now, or before this model).

Properties:

- `ImageAlwaysThere`: containerd always has an image to start.
- `GoodKept`: a version that stays up is always on disk (image or `.prev`).
- `RecordedIsImage`: the version in the state file is the image.
- `Settles` (liveness): once the assignment and the power stay put, the
  device runs the assigned version, or (crashing / failed) a good one.
- `BadNeverStays` (liveness): a crashing version never stays.

| Model | |
|-------|---|
| `Fleet` | the agent now: all properties hold (v1 shipped, v2 good, v3 crashes; 2 power losses, 2 assignment changes) |
| `FleetBefore` | **expected to fail**: the agent before this model |

## Found by the model (fixed)

The agent before renamed the image to `.prev`, then `.new` to the image,
and saved its state only at the end of a round:

1. **No image** (`ImageAlwaysThere`, `Settles`): a power loss between the
   two renames left only `.prev`. containerd couldn't start the container,
   and every later update failed at the first rename ("cannot keep the
   previous image"): the container never came back. Trace: Poll → Download
   → Stop → Swap1 → PowerLoss → Recover → Poll → … → Swap1 fails → Save →
   Poll → (forever).
2. **The good image lost** (`GoodKept`): a power loss during the health
   check of a crashing version, before the state was saved. The next agent
   didn't know the update had happened, installed the same version again
   and moved the crashing image (now in place) to `.prev`, overwriting the
   good one; the rollback then restored the crashing image. Trace: Assign v3
   → Poll → Download → Stop → Swap1 → Swap2 → PowerLoss → Recover → Poll →
   Download → Stop → Swap1 (prev = v3, image none).
3. **Stopped agent decides health** (not a separate model step: it is a
   power loss in the health window): `Sleep` returned at once after
   SIGTERM, so the health check ran its 20 samples in milliseconds and
   could keep a version that crashes a few seconds later.

The agent now: `.prev` is a hard link (copy where links fail) put in place
with one rename, so the image never goes away; the update is saved as
`pending` (file and directory fsync'd) before the container is stopped; `.new` replaces the image
with one rename; a health check cut short by SIGTERM decides nothing;
a restarted agent rolls a pending update back first, and a version
interrupted `kMaxInterrupts` (2) times counts as failed. It also restores an
image left only as `.prev` by the older agent.

## Where the code differs

- One container; the agent handles containers one after another, the same
  way.
- Health is one step: a good version stays up, a bad one doesn't. On a
  device it is sampled once a second for `health_seconds`: a version that
  crashes and is restarted by containerd between two samples can pass.
- Stopping and starting the container are folded into the steps around
  them; their failures end the round with an error and no change on disk.
- Each step's write is durable at once in the model. On a device that
  takes an fsync of the file and of its directory (`SyncDir` after every
  rename the model relies on): without the directory's, a power cut lost
  the `pending` mark, which the `fleet-power-loss` end-to-end test caught.
