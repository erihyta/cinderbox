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
| Client rendering / input | Godot 4.7 through a GDExtension (godot-cpp 4.5 API); raylib as a debug viewer |
| Client "scripts" | Small C++ flecs systems and observers (engine-independent, `src/present`) |
| Client content and mods | Godot scenes and resource packs (VFX, materials, meshes, UI) |
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
- **Rollback window**: chosen automatically from latency, between 8 and 20 ticks (M5). Beyond the window, the client stops predicting and waits.
- **Server**: never rolls back. It waits briefly for each player's input. If an input is late, it reuses that player's last input and broadcasts the inputs it actually used.
- **Server → client**: authoritative input frames plus periodic state checksums. A full snapshot is sent on join, and again when a checksum mismatch shows a desync.
- **Connection loss**: the client freezes its own time, reconnects, and resynchronizes from a snapshot.
- **Target**: 32+ players, so re-simulation must be cheap. This is verified with the bot stress test.
  - M1 measurement (Clang Release, 64 players, 277 props): 0.55 ms per step, 0.12 ms per save, 0.09 ms per load, about 2.1 MB per snapshot. A worst-case 8-tick rollback costs about 5.5 ms.

### Protocol (version 2, M5)
- **Channel 0** (reliable, ordered): `Hello`, `Welcome`, `Reject`, `Checksum` (every 30 ticks) and `ResyncRequest`.
  - `Welcome` carries the config, slot, reconnect token, a portable snapshot of the state before tick S, and the inputs of frame S-1.
- **Channel 1** (unreliable), which is never blocked by retransmissions:
  - **Client → server**: `Input` repeats the last 12 ticks of input and carries `ackTick`, meaning the client has every frame before that tick.
  - **Server → client**: every tick, a `FrameBatch` with all frames from the client's acknowledged tick to the newest. Frames are delta-chained from the acknowledged frame.
    - A lost batch costs nothing: the next one carries the same frames.
    - The server keeps 256 frames. A client that falls further behind gets a new `Welcome`.
    - Clients with the same acknowledgement share one encoded batch.
    - Large batches are sent as unreliable fragments.
- **Frame encoding**: a mask of the players whose input changed, then per player only the changed fields. A small camera turn takes one byte.
- **Handshake**: `Hello` carries the protocol version, the build fingerprint (hash of a short scripted simulation run) and an optional reconnect token. Mismatched builds are rejected.
- **Welcome covers three cases**: joining, reconnecting, and recovering from a desync (the client sends `ResyncRequest` when a checksum does not match).
- **Clock sync**: the client aims to be `rtt/2 + jitter + 2 ticks` ahead of the server's current tick, so its input for tick T arrives before the server simulates T.
  - The server clock is estimated from the fastest frame arrivals, decaying slowly.
  - Small errors are corrected by running up to ±15% faster or slower. Beyond 30 ticks the client catches up (8 ticks per frame) or pauses.
- **Prediction window (automatic)**: the client needs about `RTT × rate + 2 × jitter × rate + 4` ticks between the confirmed state and its prediction.
  - It grows the window at once, up to 20 ticks, and shrinks it one tick at a time after 3 s of lower latency. The minimum is 8.
  - `cb_client --rollback N` / `cb_bot --rollback N` fix the window. The HUD shows it.
- **Disconnects**:
  - ENet declares a connection dead after 2–6 s. Time freezes sooner: without frames the prediction window fills and the client holds its clock.
  - The server keeps a disconnected player in the world with zeroed input for 10 s. If the client reconnects with its token in that time, it gets the same slot back through a Welcome. Otherwise the server issues a `Leave` event.

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

## Godot client (M6)
Godot is only the presentation layer. The simulation, prediction, rollback, networking and animation
evaluation are the same C++ code the raylib client and the server use; none of it runs through Godot.

```
server ──ENet──> GameClient + Simulation (sim thread) ──PresentationFrame──> Mirror (main thread)
                                                                              │ events, poses
                                                            CinderboxClient ──┴─> prefab nodes, signals ──> game.gd (VFX, HUD)
```

- **Layers**:
  - `src/present` holds what both clients share:
    - `CaptureFrame` copies the visible state of one tick (a `PresentationFrame`);
    - the `Mirror` flecs world interpolates between ticks, smooths rollback corrections and evaluates ozz poses;
    - the mirror emits visual events (spawned, destroying, removed, jumped, landed).
  - The raylib viewer and the Godot extension only draw the mirror.
- **Simulation thread**: `CinderboxClient` runs `GameClient` on its own thread and publishes a frame whenever the tick, the state or a rollback changes.
  - The main thread takes the newest frame and extrapolates the interpolation alpha from the time it was published.
  - Input is latched: a jump or spawn press is kept until the simulation thread has consumed it.
- **Floating-point environment**: Godot is free to change the FPU state of its own threads.
  - The simulation thread resets MXCSR to the default (round-to-nearest, no denormals-as-zero) before doing anything.
  - It computes the build fingerprint on that thread, so a wrong environment would be rejected by the server instead of desyncing. The HUD statistics report `fp_environment_ok`.
  - The template_release extension (`-O3`, `Release`) produces the same fingerprint as the native builds.
- **Nodes**:
  - Each visual gets a node instantiated from `prefab_dir` (`res://prefabs`): `static_box`, `prop_box`, `prop_sphere`, `player`.
  - Boxes are unit scenes scaled to the entity's size. Players are positioned at the feet.
  - `CinderboxSkeleton` draws bone boxes with one MultiMesh and can drive a `Skeleton3D` by Mixamo bone name.
    This makes an imported character mesh a drop-in replacement; an AnimationTree-to-ozz binding is future work.
- **Signals**: `visual_spawned`, `visual_destroying`, `visual_removed`, `player_jumped`, `player_landed` and `connection_state_changed`.
  - `game.gd` turns them into VFX: `res://vfx/<event>.tscn`, one-shot `GPUParticles3D`.
  - The HUD is `res://ui/hud.tscn`.
- **Non-deterministic physics**: Jolt is enabled for client-only effects. Simulation entities never get Godot physics bodies.
- **Camera**: yaw uses the simulation convention internally; Godot yaw = simulation yaw − π (Godot cameras look down −Z).
- **Build**:
  - `CB_BUILD_GODOT` fetches godot-cpp (tag godot-4.5-stable, the newest API tag; Godot 4.7 loads it through `compatibility_minimum`).
  - It builds `godot/bin/libcinderbox.<platform>.<target>.<arch>`.
  - On Windows, everything uses the DLL C runtime. Clang with the GNU driver needs `TYPED_METHOD_BIND`.
- **Export**:
  - `godot/export_presets.cfg` has a "Windows" preset, and `tools/export_client.ps1` runs it with the 4.7.2 templates.
  - An exported client is a 109 MB engine executable, a 3 MB extension and a 53 KB pack.

## Mods (M6)
- **Content**: mods are Godot resource packs (`.zip`, made with `--export-pack`) that replace or add files under `prefabs/`, `vfx/`, `ui/`, `maps/` and `assets/`.
  - They hold Godot's runtime formats (binary scenes, compressed textures), so they are small and load fast.
  - `boot.tscn` loads them before the game scene is opened. The load order is: the game folder, then `user://mods`, then `--mods=`, alphabetically within each.
- **No code**: the loader refuses a pack that
  - contains scripts, native libraries, extension files, nested packs or files outside those folders; or
  - contains a resource mentioning a script type or GDExtension.

  The pack tool removes the mod project's `project.binary` and caches, which would otherwise replace the game's own.
- **Cosmetic only**: gameplay, collision and timing live in the simulation, so a mod cannot change them. The server needs no knowledge of mods.
- **Maps** (M7): authored in Godot, baked to `.cbmap` and sent on join. See the Maps section.
- **Animations**: the Godot client currently loads ozz clips from a folder on disk (`--animations=`).
  - Loading `.ozz` files from packs requires reading them through Godot's `FileAccess` into an ozz memory stream.
  - This is future work, as are clips shipped by mods.
  - Clips shipped by mods also need a skeleton compatibility check, and gameplay-relevant clips must stay the server's.

## Maps (M7)
A map is authored in the Godot editor and **baked offline** into a `.cbmap` the server loads. Godot
is the editor, never a runtime dependency of the server: `cb_server` is a headless C++ binary and
stays one. The bake is the same kind of build step as `gltf2ozz` for animations and `--export-pack`
for mods.

```
maps/arena.tscn ──bake──> arena.cbmap ──> cb_server ──welcome──> every client's simulation
      │                                                                    │
      └── meshes, lights, particles ─────────> res://maps/arena.tscn ──> what the client draws
```

- **Authoring**: three marker nodes, registered by the extension and inert at runtime:
  `CbStatic` (a solid box: floor, wall, ramp, step, platform), `CbProp` (a dynamic box or sphere)
  and `CbSpawn` (where players appear).
  - They are not Godot physics bodies. Collision belongs to Box3D, on the server and inside the
    client's prediction; Godot's own physics never sees a simulation entity.
  - Everything else in the scene is the look, and the mapper builds it however they like.
- **Baking** (`tools/bake_map.ps1`, or the Bake Map button):
  - The baker walks the scene and accumulates transforms itself, so it works headless and is
    always relative to the map root.
  - Values are quantized to fixed-point: 1/1024 m for positions and extents, 1/4096 rad for angles.
    Both grids are powers of two, so quantizing and dequantizing are exact; a map produces the same
    floats on every platform without depending on anyone's decimal parsing or libm.
  - A static box keeps yaw and pitch only. Roll is dropped, and the baker says so.
  - Scene-tree order is the order the simulation creates entities in, so it decides flecs ids,
    Box3D body order and every state hash. It is part of the format.
- **Distribution**: the map is sent in the welcome, next to the state snapshot.
  - A client cannot play the wrong level, so there is nothing to negotiate.
  - It is needed even though the snapshot already has the level: entities created *later*, such as
    a player spawning, come from the map, and a mismatch would desync from that moment on.
  - The built-in sandbox is serialized the same way, so there is one code path.
  - A baked map is small: the sandbox is 1.6 KB and the example arena is 0.5 KB.
- **Visuals**: a client draws `res://maps/<name>.tscn`, the scene the map was baked from, and then
  does not draw the baked boxes. Without that scene it draws the boxes, so a client is never left
  in the void. Mods replace map visuals like any other scene.
- **Replays** carry the map in their header, so a recording of an authored map still verifies.

## Entity templates and the component registry (M8)
The registry (`src/sim/reflect.h`) is one description of what an author may attach to an entity,
read by three things: the Godot inspector, the baker, and the simulation that applies the values.
Adding a field to it makes the field appear in the editor with no code on the Godot side, which is
what keeps authoring drag-and-drop instead of scripting.

- **A schema, not storage.** Some entries map to flecs components (`Velocity`, `Prop`), others
  describe how the entity is created (`Body` and `Material` feed Box3D's body and shape
  definitions). All of them are initial values; none are runtime state.
- **The editor builds itself from it.** `CbComponent` is one node whose inspector comes from
  `_get_property_list()` over the registry: pick "Body" and the body fields appear, with enum
  dropdowns and ranges out of the same table.
- **Values are identified by name hash**, not by position, so the registry can grow and be
  reordered without invalidating baked maps. Unknown components and fields are dropped when a map
  is read, and everything else is clamped to its range, so a map file can never push the simulation
  outside what the registry allows.
- **Templates live in the map**, which means they are already hashed and already sent to clients on
  join. A template carries a `visual` name so clients know which prefab to draw, and entities made
  from one carry a `TemplateRef` so presentation can look it up.
- **Runtime spawning**: a map can mark one template `spawnable`, and the spawn button creates that
  instead of the built-in random prop. Anything a player spawns is still given a `Prop` component
  if the template did not, so it expires and counts against the caps; a map cannot let players fill
  the world.
- **Absent means default.** A component that was not attached leaves Box3D's own defaults in place.
  That is also the compatibility rule: a map baked before a field existed keeps working.

## Effect bindings (M9)
The last piece of presentation that was still code is *what plays when*. It is now data:
`CbEffect` is one binding, `CbEffectTable` is a list of them saved as a `.tres`, and the client
loads every `res://vfx/bindings*.tres`.

- **Additive, not exclusive.** Every binding that matches an event plays, and files are loaded by
  name, so a mod ships `bindings_<name>.tres` and adds to the game's set instead of replacing it.
  Two mods can add effects without either one winning.
- **Matching** narrows an event by map template, by kind of entity, and by whether it is the local
  player's. That is enough for "this template sparks when it spawns" or "only I see my own landing
  puff" without a line of script.
- **`follow`** parents the effect to the entity's node, so a trail travels with the thing it
  belongs to and dies with it.
- **Compatibility**: when no binding matches, the old `res://vfx/<event>.tscn` convention still
  applies, so mods written before this keep working.
- **Why a resource and not a text format**: mods may not ship code, and the mod validator refuses
  `.json`. A Godot resource is inspector-editable, binary in the exported pack, and carries no
  script. Authoring one needs the extension present, so `tools/pack_mod.ps1` copies it into a mod
  project while packing and keeps it out of the pack.
- **The simulation is untouched.** Events come from the mirror as before; bindings only decide what
  presentation does with them.

**M10 added the next rows to the same table** instead of inventing new systems for them:
- **Sound**: a binding names a stream and its volume, pitch, jitter, bus and falloff distance. It
  plays positionally at the event. Pitch jitter is random on purpose — presentation never has to be
  repeatable, only the simulation does.
- **Screen effects**: camera shake and a full-screen flash, both decaying over their own time. They
  are what the viewer feels rather than what the world does, so they normally pair with
  `who = Local player`.
- **Cooldown**: the shortest gap between two plays of one binding. Twenty props landing at once is
  a visual event but should not be twenty overlapping sounds, and that is a decision for whoever
  authored the binding, not for the code.

The placeholder sounds in `godot/assets/sfx/` are generated by `tools/make_sfx.py`, the same
approach as the procedural animation rig: something plain and obviously temporary, so the pipeline
can be finished before the assets exist.

## Tooling
- **Determinism test**: replays a scripted input log and compares per-tick hashes, both between repeated runs and between different builds (`scripts/check_determinism.*`).
- **Replay**: `cb_server --record` writes every authoritative input frame plus a checksum every 60 ticks. `cb_replay verify` re-simulates the session headlessly, and `cb_client --replay` plays it with seeking (keyframes every 300 ticks).
- **Network simulator**: `cb_netsim` is a UDP relay with per-direction latency, jitter, loss and duplication, one upstream socket per client. ENet is not modified, so RTT measurement and retransmission behave as on a real network. The same code runs inside the lossy integration test.
- **Headless bots**: `cb_bot`.
  - "Full" bots run the real client and report its cost. Each gets its own thread, because a rollback can take several milliseconds and would otherwise delay the bots sharing its thread.
  - "Lite" bots keep pace and send input without simulating.
  - `scripts/stress_test.sh` runs a complete scenario.
- **Threading**: many simulations may run in one process. flecs and Box3D world creation and destruction are serialized by a process-wide mutex. The physics arena reserves address space and commits it in 16 MB steps.

## Measurements (Clang Release, 32-thread desktop, everything on one machine)

"Client work" is reconcile plus the predicted ticks, per rendered frame, for a full bot. There are two kinds of bot:
- **Realistic bots** hold directions and sprint, turn the camera smoothly now and then, and jump and spawn props occasionally.
- **Chaotic bots** (`--chaotic`) change every input field every tick. They are the worst case for mispredictions and bandwidth.

### M5 (unreliable frame batches, automatic window), 64 bots, 4 of them full

| Link | Bots | Window | Server tick | Client work avg / max | Stalled | Late inputs | Down / up per client | Server out |
|---|---|---|---|---|---|---|---|---|
| loopback | realistic | 8 | 0.80 ms | 0.86 / 8.9 ms | 0% | 0% | 42 / 45 kbit/s | 2.6 Mbit/s |
| 25±5 ms each way, 1% loss (RTT 61 ms) | realistic | 10 | 0.88 ms | 0.93 / 8.8 ms | 0% | 0% | 162 / 45 kbit/s | 10.5 Mbit/s |
| 45±10 ms, 2% loss (RTT 106 ms) | realistic | 13 | 0.94 ms | 2.48 / 16.4 ms | 0% | 0.01% | 254 / 44 kbit/s | 17 Mbit/s |
| 45±10 ms, 2% loss (RTT 106 ms) | chaotic | 13 | 1.08 ms | 1.17 / 24.2 ms | 0% | 0% | 1.4 Mbit / 44 kbit/s | 90 Mbit/s |
| loopback | chaotic | 8 | 0.81 ms | 1.18 / 10.7 ms | 0% | 0% | 196 / 45 kbit/s | 12.5 Mbit/s |

- **Bandwidth**: download grows with round trip, because each batch repeats the frames still in flight (about RTT × rate + 1 of them).
- **Integration test** (3% loss and 1% duplication each way, RTT about 74 ms): 0% late inputs once settled, with windows of 10–11. Before M5 it was about 80%.
- **Mixed build**: MSVC server, GCC network simulator and bots, and a Clang client over a lossy link. No desyncs, and the GCC build verified the MSVC server's recording.

### M4 (reliable ordered frames, fixed window, chaotic bots) for comparison

| Link | Window | Server tick | Client work avg / max | Stalled | Late inputs | Down per client |
|---|---|---|---|---|---|---|
| loopback, 64 bots | 8 | 0.72 ms | 0.94 / 6.7 ms | 0% | ~0% | 118 kbit/s |
| RTT 106 ms, 2% loss | 8 | 0.75 ms | 1.98 / 17.1 ms | 7.9% | 4.5% | 115 kbit/s |
| RTT 106 ms, 2% loss | 16 | 0.78 ms | 1.63 / 15.5 ms | 1.1% | 0.17% | 120 kbit/s |

- **Join**: one portable snapshot per join, about 80–250 KB.
- **Replay**: `cb_replay verify` re-simulates at 0.15–0.4 ms per tick.

### Findings
- **Window vs latency**: with a fixed 8-tick window at 60 Hz, a round trip above about 75 ms pins the client to the window.
  - Every input it sends then arrives late, and that player's own actions get corrected constantly.
  - The automatic window fixes this, at the cost of deeper re-simulation when latency is high (client work up to about 16–24 ms in the worst frames at 106 ms with 64 very active players).
- **Head-of-line blocking** (M4): with reliable ordered frames, one lost frame stalled confirmation until it was retransmitted. The M5 unreliable batches removed it.
- **Test harness pitfalls**:
  - A full bot sharing a thread with lite bots made their inputs late; each full bot now runs on its own thread.
  - Debug builds cannot keep real time with a server and four simulating clients on one thread, so the late-input thresholds are only checked in optimized builds.
- **Rare reconnect**: an occasional client reconnect (about one per several minutes of 64 bots at 2% loss) was seen with the 1–3 s ENet timeout. The timeout is now 2–6 s; no reconnects occurred in the M5 runs.

### Possible next steps
- **Less download at high latency**: skip frames that are probably still in flight and resend them only after a timeout. This trades bandwidth for a slower recovery from loss.
- **Camera**: add camera collision in the client (it can currently clip into walls).
- **Godot**:
  - an AnimationTree-to-ozz binding;
  - ozz clips loaded from packs;
  - Linux and macOS exports (the extension builds with `unix-clang-release`; not yet tested).
- **Maps, next steps**:
  - shapes beyond boxes, spheres and capsules, and a way to author them without one node per box;
  - static geometry left out of the portable snapshot: it is immutable and both sides build it from
    the map, so a large map should not pay for it on every join;
  - templates shared between maps, instead of one copy per map file.
- **Presentation sandbox** (M9, M10): effects, sounds and screen effects are data. What is missing
  now is not more rows but more events: impacts, footsteps and scrapes all need the simulation to
  report them deterministically first, which is a gameplay change rather than a presentation one.
- **Marker nodes on the client**: a map's visual scene still carries its authoring nodes, which
  cost about 0.3 ms per 1000 at load and nothing per frame. If maps ever get large, the bake can
  write a visual-only copy with the markers replaced by plain Node3D.

## Milestones (check-in after each)
1. **M1** (done): build system, deterministic sim core (flecs + Box3D + mover + props), snapshot/restore, rollback session, determinism tests (Clang, GCC and MSVC verified identical).
2. **M2** (done): ENet server and raylib client, rollback netcode, join/leave/reconnect, loopback integration tests. Verified with an MSVC server and GCC and Clang clients in one session.
3. **M3** (done): ozz integration, procedural box skeleton, locomotion blend, glTF pipeline (script and docs, tested with generated Blender-style glTF files), animation viewer.
4. **M4** (done): replay recording, verification and playback, network simulator, bots (full and lite), stress-test script, lossy integration test, 64-player measurements.
5. **M5** (done): unreliable acknowledged frame batches, per-field input encoding, automatic prediction window, realistic and chaotic bots, re-measured.
6. **M6** (done): engine-independent presentation layer (`src/present`), Godot GDExtension client on its own simulation thread, prefab/VFX/HUD scenes, mod packs with a no-code validator and an example mod, Windows export. Verified: same fingerprint as native builds (editor and exported release), no desyncs with bots.
7. **M7** (done): maps authored in the Godot editor (CbStatic / CbProp / CbSpawn), a fixed-point `.cbmap` bake, the map sent to clients on join, map visuals drawn from the authored scene, `cb_server --map`, and maps in replays.
8. **M8** (done): the authorable component registry, `CbTemplate` / `CbComponent` / `CbEntity` authoring with an inspector generated from the registry, templates and instances in the map format, entities spawned from templates at runtime, and per-template visuals on the client.
9. **M9** (done): effect bindings as data (`CbEffect` / `CbEffectTable`), additive binding files so mods add effects without replacing the game's, matching by template, kind and local player, effects that follow their entity, and a mod that ships its own bindings.
10. **M10** (done): sounds, camera shake, screen flash and per-binding cooldowns as further fields of the same effect bindings, placeholder sound effects and a generator for them, and an example mod that ships its own sound.
