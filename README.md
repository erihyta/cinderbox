# Cinderbox

A deterministic multiplayer third-person physics sandbox, built with flecs, Box3D, ENet,
ozz-animation and raylib. See [DESIGN.md](DESIGN.md) for the architecture and decisions.

## Status

| Milestone | State |
|---|---|
| M1: build, deterministic sim core, snapshots, determinism tests | done |
| M2: ENet server, raylib client, rollback netcode, join/leave/reconnect | done |
| M3: ozz animation, procedural box skeleton, locomotion blend, asset pipeline | done |
| M4: replay tool, network simulator, bots, stress test | done |
| M5: unreliable frame batches, automatic prediction window | done |

## Building

Requirements: CMake 3.24+, Ninja, and one of Clang, GCC or MSVC. Dependencies are downloaded at
configure time and pinned in `cmake/Dependencies.cmake`.

Build output goes outside the source tree (`%LOCALAPPDATA%\cinderbox-build\<preset>` on Windows,
`~/.cache/cinderbox-build/<preset>` elsewhere), which keeps it out of OneDrive.

```sh
cmake --preset clang-release
cmake --build --preset clang-release
ctest --preset clang-release
```

Presets: `clang-debug`, `clang-release`, `gcc-release`, `msvc-release` (run from a VS developer
prompt), `unix-clang-release`, `unix-gcc-release`.

## Playing

```sh
# terminal 1
cb_server --port 7777
# terminal 2, 3, ...
cb_client --host 127.0.0.1 --port 7777
```

Both executables are in `<build dir>/bin`. The server options are `--tick-rate`, `--seed`, `--substeps`,
`--prop-lifetime`, `--props-per-player` and `--props-global`. The client options are `--rollback TICKS` (fixes the prediction window, which is otherwise chosen from latency),
`--width`, `--height`, and `--autoplay SECONDS [--screenshot FILE]` for an unattended smoke test.

## Animations

Players are drawn as one box per bone of an ozz skeleton. Until you add clips, a procedural
placeholder rig with Mixamo joint names is used. To add your own clips:

1. Put `idle`, `walk`, `run`, `jump_start`, `fall` and `land` `.glb` files in a folder.
2. Run `tools\convert_animations.ps1 -Source <folder>` (or `tools/convert_animations.sh <folder>`).
3. Preview them with `cb_client --anim-viewer`.

[assets/anim/README.md](assets/anim/README.md) has the Mixamo → Blender steps and the `anim.cfg`
reference. `--assets DIR` selects a different asset folder, and `--procedural-anim` forces the placeholder.

Client controls:
- WASD moves, Shift sprints, Space jumps, and F spawns a prop.
- The mouse orbits the camera and the wheel zooms.
- Esc releases the mouse and F1 toggles the debug HUD.

The HUD shows the predicted and confirmed ticks, round-trip time, clock error, rollbacks, stalls and
checksum results.

Server and clients can be built with different compilers. The server rejects a client whose
simulation fingerprint differs from its own.

## Testing tools

All tools are in `<build dir>/bin`.

| Tool | What it does |
|---|---|
| `cb_server --record FILE` | Records the whole session (input frames plus checksums) |
| `cb_replay info\|verify FILE` | Summarizes a recording, or re-simulates it and checks every checksum |
| `cb_client --replay FILE [--replay-start S]` | Watches a recording |
| `cb_netsim --listen P --target HOST:PORT --latency MS --jitter MS --loss % [--duplicate %]` | UDP relay that degrades traffic (latency is added in each direction) |
| `cb_bot --port P --count N --full M --duration S [--chaotic]` | Headless players; the M "full" bots run prediction and rollback and report its cost; `--chaotic` changes every input every tick |
| `scripts/stress_test.sh --bots N --full M --latency MS --jitter MS --loss % --rollback T` | Starts a server, the simulator and the bots, and prints a summary |

Replay viewer controls:
- Space pauses, Up/Down change the speed, Left/Right seek 5 s, and `,` / `.` step one tick.
- Home restarts, Tab follows the next player, and Backspace switches to a free camera.
- The right mouse button or Esc orbits the camera.

```sh
# a lossy 64-player session, recorded and verified afterwards
scripts/stress_test.sh --bots 64 --full 4 --latency 25 --jitter 5 --loss 1 --duration 60 --record session.cbr
```

## Determinism checks

```powershell
# Windows: builds with Clang, GCC and MSVC and cross-checks all of them
powershell -ExecutionPolicy Bypass -File scripts\check_determinism.ps1 -Reference tests\reference_hashes.txt
```

```sh
# Linux / macOS
scripts/check_determinism.sh tests/reference_hashes.txt
```

`tests/reference_hashes.txt` holds the per-tick state hashes of the reference scenario. Any change to
simulation code or tuning legitimately changes them. Regenerate the file with
`cb_tests --dump tests/reference_hashes.txt` and commit it together with the change.

## Layout

```
cmake/            float flags (Determinism.cmake), pinned dependencies (flecs, Box3D, ENet, ozz, raylib)
assets/anim/      your converted animation clips (see its README)
src/sim/          deterministic simulation shared by server and client
  types.h           inputs, input frames, config
  components.h      snapshotted ECS components (POD, no padding)
  simulation.*      the game: level, character mover, props, snapshots, hashing
  rollback.*        client prediction + rollback session
  physics_arena.*   Box3D allocator arena (makes the physics state copyable)
  box3d_shim.c      access to Box3D internals (world struct, portable serializer)
  detmath.h         deterministic trig and the yaw convention
  fingerprint.*     build fingerprint checked when a client connects
  anim_controller.* deterministic locomotion state machine (AnimState)
src/anim/         ozz: procedural rig, asset loading (anim_set.*), pose evaluation (pose.*)
src/net/          wire protocol, ENet wrapper, network simulator (netsim.*), replay files (replay.*)
src/server/       authoritative GameServer (library) and cb_server
src/tools/        cb_netsim, cb_replay, cb_bot
src/client/       GameClient core (no rendering, also "lite" mode), bot brain, and the raylib app
  app/              presentation flecs world, rendering, camera, HUD
  app/anim_viewer.* offline clip preview (--anim-viewer)
  app/replay_viewer.* recording playback (--replay)
  app/scripts/      client scripts: spawn/destroy effects, player pose evaluation
tests/            determinism, rollback, gameplay, animation and loopback network tests
scripts/          cross-compiler determinism check, stress test
tools/            animation conversion (convert_animations.*), test glTF generator
```
