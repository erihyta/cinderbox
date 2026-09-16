# Cinderbox — Design Spec

Multiplayer third-person physics sandbox. The look doesn't matter. The goals are determinism and a clean architecture.

## Stack
| Concern | Choice |
|---|---|
| Language / build | C++20, CMake + Ninja, Clang primary (MSVC and GCC also supported) |
| Dependencies | CMake FetchContent, pinned to exact commits |
| ECS | flecs 4.x |
| Physics | Box3D (erincatto/box3d), single-threaded, cross-platform determinism mode |
| Animation | ozz-animation 0.9.x, **scalar (non-SIMD) build** |
| Networking | ENet (UDP), dedicated headless server |
| Client rendering / input | raylib |
| Client "scripts" | Small C++ flecs systems and observers |
| Build directory | `%LOCALAPPDATA%/cinderbox-build` (outside OneDrive) |

## Determinism
- The simulation must be bit-exact across Windows x64, Linux x64 and macOS/ARM64, and across MSVC, Clang and GCC.
- IEEE floats only: no fast-math, `-ffp-contract=off`, `/fp:precise`, and no x87.
- No libm trig in the simulation. `sin`, `cos` and `atan2` come from Box3D's cross-platform implementations, wrapped in `detmath.h`. `sqrt` and basic arithmetic are fine because IEEE requires them to be correctly rounded.
- No unordered iteration, pointer-keyed ordering, wall-clock time or `rand()` in the simulation. Randomness comes from a seeded PRNG stored in the simulation state.
- Inputs are quantized to integers: movement axes as int8, camera yaw as uint16, plus button bits.
- A per-tick state hash (world + physics) backs the determinism tests and runtime desync detection.

## Netcode: authoritative server + client rollback
- **Tick rate** is configurable (default 60 Hz). The server announces it when a client joins.
- **Client**: sends its own inputs each tick and immediately simulates the full world, predicting remote players by repeating their last input. When the server's authoritative input frame for tick N differs from the prediction, the client restores the snapshot at N and re-simulates to the present, at most once per rendered frame.
- **Rollback window**: 8 ticks. Beyond that, the client stops predicting and waits.
- **Server**: never rolls back. It waits briefly for each player's input. If an input is late, it reuses that player's last input and broadcasts the inputs it actually used.
- **Server → client**: authoritative input frames plus periodic state checksums. A full snapshot is sent on join, and again when a checksum mismatch shows a desync.
- **Connection loss**: the client freezes its own time, reconnects, and resynchronizes from a snapshot.
- **Target**: 32+ players, so re-simulation must be cheap. This is verified with the bot stress test.
  - M1 measurement (Clang Release, 64 players, 277 props): 0.55 ms per step, 0.12 ms per save, 0.09 ms per load, about 2.1 MB per snapshot. A worst-case 8-tick rollback costs about 5.5 ms.

## State and snapshots
- **Two flecs worlds on the client**
  - Simulation world: POD components listed in a snapshot registry. Entities are addressed by a stable `NetId`, never by a flecs entity id.
  - Presentation world: meshes, effects, ozz poses and interpolation.
  - The server runs only the simulation world.
- **Rollback snapshots (in-process, fast)**: Box3D allocates from a per-world arena (`PhysicsArena`). A snapshot copies the used part of the arena plus the `b3World` struct from Box3D's static world table (`box3d_shim.c`). These snapshots contain raw pointers, so they only load back into the same `Simulation` instance.
- **Portable snapshots (join / desync recovery)**: the canonical ECS image plus Box3D's own world serializer (the one its replay system uses). No pointers. Verified to continue bit-identically across Clang, GCC and MSVC builds. Box3D rejects the image if struct layouts differ.
- **Box3D ids in components** are stored with the world slot zeroed, because the slot differs between processes. `Simulation::BodyOf` / `ShapeOf` patch it back in.
- **Canonical order**: entities are kept in a NetId-sorted list. Players are processed in slot order. Flecs iteration order is never used for simulation.
- **Hash**: FNV-1a over the canonical ECS image. Prop poses and velocities are mirrored from Box3D every tick, so the hash covers physics too.
- **Ring buffer**: a snapshot for each tick that might still be rolled back, plus hashes of confirmed states so they can be checked against server checksums.

## Gameplay
- **World**: flat ground, enclosing walls, ramps, steps, platforms and dynamic boxes/spheres.
- **Player movement**: a kinematic capsule mover (move-and-slide with Box3D's mover casts and plane solver). A pogo spring keeps the capsule hovering, which carries it over steps. There is no explicit slope limit yet. The mover pushes dynamic bodies with impulses and turns to face its movement direction.
- **Controls**: WASD moves relative to the camera, Shift sprints, Space jumps, the mouse orbits the third-person camera, and one key spawns a prop.
- **Props**: spawned in front of the player, with a 20 s lifetime and at most 10 per player (the oldest despawns first), plus a global cap. All values are configurable.
- **Players**: spawn on join and despawn on leave. A player who falls below the kill-Y respawns.

## Animation (visual now, gameplay-ready)
- ozz is evaluated inside the deterministic tick using the scalar build, so root motion and hitboxes can be added later without breaking determinism.
- Animation time is derived from ticks, not wall-clock time.
- **Locomotion states**:
  - Idle, walk and run form a 1D blend by ground speed, with synchronized cycle phase.
  - Jump start, fall and land are driven by the grounded flag and vertical velocity.
- **Placeholder**: until real assets arrive, a procedural humanoid ozz skeleton with a procedural walk cycle, drawn as one box per bone.
- **Real assets**: Mixamo rig. FBX is exported to glTF with Blender, then converted with `gltf2ozz`. Bone names are mapped in a config file.
- The client scripts own the blend weights and the spawn and destroy visual effects.

## Tooling
- **Determinism test**: replays a scripted input log and compares per-tick hashes, both between repeated runs and between different builds.
- **Replay**: the server records its input log, and a tool plays it back headless or in the client.
- **Network simulator**: adds latency, jitter and packet loss.
- **Headless bot clients**: random inputs, for the 32+ player stress test.

## Milestones (check-in after each)
1. **M1** (done): build system, deterministic sim core (flecs + Box3D + mover + props), snapshot/restore, rollback session, determinism tests (Clang, GCC and MSVC verified identical).
2. **M2**: ENet server and raylib client, rollback netcode, join/leave/reconnect.
3. **M3**: ozz integration, procedural box skeleton, locomotion blend, glTF pipeline docs.
4. **M4**: replay tool, network simulator, bots, 32-player stress test.
