# Cinderbox

A deterministic multiplayer third-person physics sandbox, built with flecs, Box3D and (from M3)
ozz-animation. See [DESIGN.md](DESIGN.md) for the architecture and decisions.

## Status

| Milestone | State |
|---|---|
| M1: build, deterministic sim core, snapshots, determinism tests | done |
| M2: ENet server, raylib client, rollback netcode, join/leave/reconnect | done |
| M3: ozz animation, procedural box skeleton, locomotion blend | next |
| M4: replay tool, network simulator, bots, stress test | |

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
`--prop-lifetime`, `--props-per-player` and `--props-global`. The client options are `--rollback TICKS`,
`--width`, `--height`, and `--autoplay SECONDS [--screenshot FILE]` for an unattended smoke test.

Client controls:
- WASD moves, Shift sprints, Space jumps, and F spawns a prop.
- The mouse orbits the camera and the wheel zooms.
- Esc releases the mouse and F1 toggles the debug HUD.

The HUD shows the predicted and confirmed ticks, round-trip time, clock error, rollbacks, stalls and
checksum results.

Server and clients can be built with different compilers. The server rejects a client whose
simulation fingerprint differs from its own.

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
cmake/            float flags (Determinism.cmake), pinned dependencies (flecs, Box3D, ENet, raylib)
src/sim/          deterministic simulation shared by server and client
  types.h           inputs, input frames, config
  components.h      snapshotted ECS components (POD, no padding)
  simulation.*      the game: level, character mover, props, snapshots, hashing
  rollback.*        client prediction + rollback session
  physics_arena.*   Box3D allocator arena (makes the physics state copyable)
  box3d_shim.c      access to Box3D internals (world struct, portable serializer)
  detmath.h         deterministic trig and the yaw convention
  fingerprint.*     build fingerprint checked when a client connects
src/net/          wire protocol (protocol.*) and ENet wrapper (transport.*)
src/server/       authoritative GameServer (library) and cb_server
src/client/       GameClient core (no rendering) and the raylib app
  app/              presentation flecs world, rendering, camera, HUD
  app/scripts/      client scripts: spawn/destroy effects, player motion
tests/            determinism, rollback, gameplay and loopback network tests
scripts/          cross-compiler determinism check
```
