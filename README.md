# Cinderbox

A deterministic multiplayer third-person physics sandbox, built with flecs, Box3D and (from M3)
ozz-animation. See [DESIGN.md](DESIGN.md) for the architecture and decisions.

## Status

| Milestone | State |
|---|---|
| M1: build, deterministic sim core, snapshots, determinism tests | done |
| M2: ENet server, raylib client, rollback netcode | next |
| M3: ozz animation, procedural box skeleton, locomotion blend | |
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
cmake/            float flags (Determinism.cmake), pinned dependencies
src/sim/          deterministic simulation shared by server and client
  types.h           inputs, input frames, config
  components.h      snapshotted ECS components (POD, no padding)
  simulation.*      the game: level, character mover, props, snapshots, hashing
  rollback.*        client prediction + rollback session
  physics_arena.*   Box3D allocator arena (makes the physics state copyable)
  box3d_shim.c      access to Box3D internals (world struct, portable serializer)
  detmath.h         deterministic trig and the yaw convention
tests/            determinism, rollback and gameplay tests
scripts/          cross-compiler determinism check
```
