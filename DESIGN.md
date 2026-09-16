# Cinderbox — Design Spec

Multiplayer third-person physics sandbox. The look doesn't matter. The goals are determinism and a clean architecture.

## Stack
| Concern | Choice |
|---|---|
| Language / build | C++20, CMake + Ninja, Clang primary (MSVC and GCC also supported) |
| Dependencies | CMake FetchContent, pinned to exact commits, downloaded and built inside each build directory (never shared between compilers) |
| ECS | flecs 4.x |
| Physics | Box3D (erincatto/box3d), single-threaded, cross-platform determinism mode |
| Animation | ozz-animation 0.17.0, **scalar (non-SIMD) build**; gltf2ozz for asset conversion |
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

### Protocol (M2)
- **Channel 0** (reliable, ordered) carries all server messages, so a Welcome and the frames after it always arrive in order:
  - `Welcome`: config, slot, reconnect token, a portable snapshot of the state before tick S, and the inputs of frame S-1.
  - `Frame`: delta-coded against the previous frame.
  - `Checksum`: sent every 30 ticks.
  - `Reject`.
- **Channel 1** (unreliable) carries `Input`. Each packet repeats the last 12 ticks of input, so losing a packet costs nothing.
- **Handshake**: the client sends `Hello` with the protocol version, the build fingerprint (hash of a short scripted simulation run) and an optional reconnect token. Mismatched builds are rejected.
- **Welcome covers three cases**: joining, reconnecting, and recovering from a desync (the client sends `ResyncRequest` when a checksum does not match).
- **Clock sync**: the client aims to be `rtt/2 + jitter + 2 ticks` ahead of its estimate of the server's current tick, so its input for tick T arrives before the server simulates T.
  - It corrects small errors by running up to ±15% faster or slower.
  - If it falls more than 30 ticks behind, it catches up at up to 8 ticks per frame. If it gets more than 30 ticks ahead, it pauses.
  - Measured on loopback: 0 late inputs once settled. Late inputs only occur while a client catches up right after joining.
- **Latency limit**: with an 8-tick window at 60 Hz, a round-trip time above roughly 75 ms pins the client to the window (measured in M4, see Findings). Raise the window with `cb_client --rollback N`.
- **Disconnects**:
  - ENet detects a dead connection within 1–3 s.
  - The server keeps a disconnected player in the world with zeroed input for 10 s. If the client reconnects with its token in that time, it gets the same slot back through a Welcome. Otherwise the server issues a `Leave` event.
  - While disconnected, the client's time is frozen.

## State and snapshots
- **Two flecs worlds on the client**
  - Simulation world: POD components listed in a snapshot registry. Entities are addressed by a stable `NetId`, never by a flecs entity id.
  - Presentation world (`src/client/app/presentation.*`), which is never rolled back. Each frame it compares itself against the simulation:
    - new NetIds get visuals with a spawn effect, and vanished NetIds get a destroy effect;
    - poses are interpolated between the last two ticks;
    - position jumps caused by a rollback are faded out over about 80 ms.
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
- **Split (M3)**:
  - The simulation owns the animation state. `AnimState` (mode, previous mode, mode time, synchronized locomotion phase, idle time, smoothed ground speed) is a snapshotted component, advanced every tick by `UpdateAnimState` (`src/sim/anim_controller.*`).
  - ozz pose sampling (`src/anim/pose.*`) is a pure function of that state plus the clip data. It uses only IEEE arithmetic and the scalar ozz build, and a cross-compiler pose-hash check verifies it (`cb_tests --anim-hash`).
  - The client evaluates poses every frame from the state interpolated between ticks. The server does not sample poses yet; running 64 skeletons inside every rolled-back tick would cost CPU for nothing that gameplay uses today. Hitboxes or root motion can call the same evaluator on the server later.
- **Assets and the simulation**: the simulation never reads asset data. Clip lengths only affect rendering. The walk and run cycle lengths that drive the phase are simulation constants in `anim_tuning`. If poses ever feed into gameplay, the server must load the same asset files, and their hash should become part of the build fingerprint.
- **Locomotion**:
  - Idle, walk and run form a 1D blend on ground speed (0, 3 and 6.5 m/s). Walk and run share one phase, so the feet stay in sync.
  - JumpStart, Fall and Land are chosen from the grounded flag, the time since the last jump, and air time.
  - Modes crossfade over 0.15 s. A mode that is fading out holds its last pose.
- **Placeholder**: a procedural 27-joint rig with Mixamo joint names and procedural clips, built with deterministic trig. Drawn as one box per bone.
- **Real assets**: Mixamo FBX → Blender → glTF → `gltf2ozz` via `tools/convert_animations.*`, loaded through `assets/anim/anim.cfg`.
  - Centimetre rigs are detected automatically.
  - `lock_root_xz` cancels horizontal root motion.
  - A clip built for a different skeleton is rejected.
  - Bone names are used only to pick box sizes; no mapping file is needed until gameplay uses specific bones.
- **Preview**: `cb_client --anim-viewer` shows every clip plus a live speed sweep.
- **Client scripts**: the pose-evaluator creation observer, the per-frame pose evaluation system, and the spawn and destroy effects.

## Tooling
- **Determinism test**: replays a scripted input log and compares per-tick hashes, both between repeated runs and between different builds (`scripts/check_determinism.*`).
- **Replay**: `cb_server --record` writes every authoritative input frame plus a checksum every 60 ticks. `cb_replay verify` re-simulates the session headlessly, and `cb_client --replay` plays it with seeking (keyframes every 300 ticks).
- **Network simulator**: `cb_netsim` is a UDP relay with per-direction latency, jitter, loss and duplication, one upstream socket per client. ENet is not modified, so RTT measurement and retransmission behave as on a real network. The same code runs inside the lossy integration test.
- **Headless bots**: `cb_bot`.
  - "Full" bots run the real client and report its cost. Each gets its own thread, because a rollback can take several milliseconds and would otherwise delay the bots sharing its thread.
  - "Lite" bots keep pace and send input without simulating.
  - `scripts/stress_test.sh` runs a complete scenario.
- **Threading**: many simulations may run in one process. flecs and Box3D world creation and destruction are serialized by a process-wide mutex. The physics arena reserves address space and commits it in 16 MB steps.

## Measurements (M4, Clang Release, 32-thread desktop, everything on one machine)

Bots change their inputs almost every tick, which is a worst case for mispredictions. "Client work" is reconcile plus the predicted ticks, per rendered frame, for a full bot.

| Scenario | Server tick | Client work avg / max | Rollback window | Stalled | Late inputs (settled) | Desyncs |
|---|---|---|---|---|---|---|
| 32 bots, loopback | 0.56 ms | 0.66 / 7.3 ms | 8 | 0% | ~0% | 0 |
| 64 bots, loopback | 0.72 ms | 0.94 / 6.7 ms | 8 | 0% | ~0% | 0 |
| 64 bots, 25±5 ms each way, 1% loss (RTT 60 ms) | – | 2.05 / 12.6 ms | 8 | 1.8% | – | 0 |
| 64 bots, 45±10 ms, 2% loss (RTT 106 ms) | 0.75 ms | 1.98 / 17.1 ms | 8 | 7.9% | 4.5% | 0 |
| same | 0.78 ms | 1.63 / 15.5 ms | 16 | 1.1% | 0.17% | 0 |

- **Bandwidth**: with 64 players, about 115 kbit/s down and 47 kbit/s up per client. The server sends about 7.4 Mbit/s in total.
- **Join**: one portable snapshot per join, about 80–250 KB.
- **Replay**: `cb_replay verify` re-simulated a 64-player recording at about 0.4 ms per tick.

### Findings
- **Rollback window vs latency**: a client must stay about `RTT × tick rate + jitter + 2` ticks ahead of the confirmed state. With the chosen 8-tick window at 60 Hz, that caps the round trip at about 75 ms.
  - Above that, the client is pinned to the window. Every input it sends arrives late, the server repeats the previous one, and that player's own actions get corrected constantly.
  - At 106 ms RTT this showed up as 8% stalled time and about 5% late inputs server-wide, coming from 4 of 64 players.
  - A 16-tick window fixed it at the cost of deeper re-simulation.
- **Head-of-line blocking**: input frames travel on a reliable, ordered ENet channel. A lost frame delays every later frame until it is retransmitted (about RTT + 4 × variance, which is 9+ ticks at 106 ms), so confirmation stalls in bursts.
  - With 3% loss in the integration test, late inputs were about 80% with window 8 and about 11% with window 16.
  - The usual fix is to send frames unreliably, repeating every frame the client has not yet acknowledged, with the client's acknowledgement carried in its input packets. That is a pending decision (see below).
- **Clock estimate**: the client now takes the server clock from the fastest frame arrivals, decaying slowly, rather than from the latest arrival.
- **No spurious disconnects**: the 1–3 s ENet timeout caused none in a 2-minute, 64-player run at 2% loss.

### Open decisions
1. Frame transport: keep reliable ordered frames, or move to acknowledged, redundant unreliable frames.
2. Rollback window: keep 8, raise the default, or pick it automatically from the measured RTT.

## Milestones (check-in after each)
1. **M1** (done): build system, deterministic sim core (flecs + Box3D + mover + props), snapshot/restore, rollback session, determinism tests (Clang, GCC and MSVC verified identical).
2. **M2** (done): ENet server and raylib client, rollback netcode, join/leave/reconnect, loopback integration tests. Verified with an MSVC server and GCC and Clang clients in one session.
3. **M3** (done): ozz integration, procedural box skeleton, locomotion blend, glTF pipeline (script and docs, tested with generated Blender-style glTF files), animation viewer.
4. **M4** (done): replay recording, verification and playback, network simulator, bots (full and lite), stress-test script, lossy integration test, 64-player measurements.
