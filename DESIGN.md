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
| Game rules | C++ server mods compiled into `cb_server` (`server_mods/`), talking to the world only through commands |
| Client "scripts" | Small C++ flecs systems and observers (engine-independent, `src/present`); mod presentation as data bindings |
| Client content and mods | Godot scenes and resource packs (VFX, materials, meshes, UI) |
| Build directory | `%LOCALAPPDATA%/cinderbox-build/<project folder>/<preset>` (outside OneDrive, one per checkout) |

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
The engine provides the mechanisms below; the rules on top (what spawns props, what hurts, when a
player dies) are server mods (M14).

- **World**: flat ground, enclosing walls, ramps, steps, platforms and dynamic boxes/spheres.
- **Player movement**: a kinematic capsule mover (move-and-slide with Box3D's mover casts and plane solver). A pogo spring keeps the capsule hovering, which carries it over steps. There is no explicit slope limit yet. The mover pushes dynamic bodies with impulses and turns to face its movement direction.
- **Controls**: WASD moves relative to the camera, Shift sprints, Space jumps and the mouse orbits the third-person camera. Every other control is an action a server mod declares.
- **Props**: the `props` mod throws one in front of the player; the engine keeps the budget: a 20 s lifetime and at most 10 per player (the oldest despawns first), plus a global cap. All values are configurable.
- **Players**: spawn on join and despawn on leave. A player who falls below the kill-Y is put back at a spawn point and the fall is counted (`Character::fallCount`) for mods to see. A `Kill` command makes a player dead (no input, body disabled, not drawn) until a `Respawn`.

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
a view source (its own thread) ──ViewFrame──> Mirror (main thread)
  live: server ──ENet──> GameClient + Simulation                 │ events, poses
  replay: a recording ──> Simulation          CinderboxClient ──┴─> prefab nodes, signals ──> game.gd (VFX, HUD)
```

- **Layers**:
  - `src/present` holds what both clients share:
    - `CaptureFrame` copies the visible state of one tick (a `PresentationFrame`);
    - the `Mirror` flecs world interpolates between ticks, smooths rollback corrections and evaluates ozz poses;
    - the mirror emits visual events (spawned, destroying, removed, jumped, landed).
  - The raylib viewer and the Godot extension only draw the mirror.
- **Simulation thread**: the view source (see [The viewer protocol](#the-viewer-protocol-m49)) simulates on its own thread and publishes a frame whenever the tick, the state or a rollback changes. `CinderboxClient` only draws frames.
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
- **Signals**: `visual_spawned`, `visual_destroying`, `visual_removed`, `player_jumped`, `player_landed` and `source_state_changed`.
  - `game.gd` turns them into VFX: `res://vfx/<event>.tscn`, one-shot `GPUParticles3D`.
  - The HUD is `res://ui/hud.tscn`.
- **Non-deterministic physics**: Jolt is enabled for client-only effects. Simulation entities never get Godot physics bodies.
- **Camera**: yaw uses the simulation convention internally; Godot yaw = simulation yaw − π (Godot cameras look down −Z).
- **Build**:
  - `CB_BUILD_GODOT` fetches godot-cpp (tag godot-4.5-stable, the newest API tag; Godot 4.7 loads it through `compatibility_minimum`).
  - It builds `godot/bin/libcinderbox.<platform>.<target>.<arch>` (the viewer) and `libcinderbox_peer.<...>` (the peer): see [Two extensions](#two-extensions-m51).
  - On Windows, everything uses the DLL C runtime. Clang with the GNU driver needs `TYPED_METHOD_BIND`.
- **Export**:
  - `godot/export_presets.cfg` has a "Windows" preset, and `tools/export_client.ps1` runs it with the 4.7.2 templates.
  - An exported client is a 109 MB engine executable, a 3.4 MB extension and a 147 KB pack (M14: with the pistol content and its sounds).

## Client mods (M6)
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

## Impacts and footsteps (M11)
Effects could already be chosen by data, but the only events to choose from were the ones
presentation could see for itself. Impacts and footsteps have to come from the simulation, which
makes them a determinism problem rather than a presentation one.

- **Impacts** come from Box3D's contact hit events (`enableHitEvents` on everything that moves, a
  1.5 m/s threshold on the world). They land in a small ring in `SimGlobals`: both entities, the
  point, the approach speed, and a count that only grows.
  - The ring is **state**, so it is hashed, snapshotted and restored by rollback like everything
    else. A predicted impact that gets rolled back is un-counted with the state it belonged to.
  - Box3D reports events in its own order, so they are **sorted by strength, then by the entities
    involved** before being recorded. What ends up in the state cannot depend on the engine's
    internal ordering, and when more impacts happen in one tick than fit, the loudest are kept.
  - Shapes are mapped back to entities by shape index, rebuilt only on ticks that produced a hit.
- **Footsteps** are a stride phase on the character: distance walked while grounded, with a counter
  that ticks over every 1.6 m. A distance rather than a timer means walking and sprinting both
  sound right with no second parameter, and the count is ordinary character state.
- **Presentation reads counters, not streams.** The mirror compares the counts it last saw with the
  ones in the frame, so a renderer that skipped ticks gets one event per step instead of one per
  frame, and a count that moved backwards (a rollback or a reset) resyncs silently. This is the
  same shape as Jumped and Landed, which are diffs of the animation mode.
- **Verified across compilers.** Because the ring is hashed, the Clang/GCC/MSVC determinism check
  now covers contact events as well; all three agree.

## Godot animation driven by the simulation (M12)
Until now a character was posed by ozz from the simulation's animation state. M12 adds the other
option: hand that state to Godot's own animation system, so a character can be built the way a
Godot artist expects — imported clips, blend spaces, state machines, retargeting, IK — while its
timing still comes from the simulation.

- **`CinderboxAnimator`** sits in a player prefab beside an `AnimationTree`. Each frame it sets the
  state machine's state from the mode, the blend position from the ground speed, and advances the
  tree by hand (the tree is switched to manual callbacks: it runs on the simulation's clock, not
  Godot's).
- **Phase, not just state.** The simulation's `locomotionPhase` is shared by walk and run so their
  feet line up. The animator turns it back into seconds using the clip lengths the simulation was
  tuned with, and resyncs when the tree drifts more than `sync_threshold`, wrapping the comparison
  for looping clips. Catching up by advancing preserves the transition blend, which restarting the
  state would throw away.
- **The blend space is in metres per second**, the same units the simulation smooths, so its blend
  points sit exactly on the walk and run speeds rather than on invented normalized values.
- **Both paths coexist.** A prefab may have a `CinderboxSkeleton`, a `CinderboxAnimator` or both,
  and the client drives whichever it finds. ozz stays the default and remains the path that can
  move server-side once animation affects gameplay.
- **`apply_state()` is exposed to scripts**, which is what makes the whole thing checkable without
  a server: `check_animtree.gd` drives a prefab through every mode and asserts the tree follows.
- **The example is a mod.** `example_animtree` ships only `prefabs/player.tscn`, and replacing how
  characters animate needs no code, which is the clearest demonstration so far that presentation is
  fully replaceable from outside.

## Humanoid bone names and retargeting (M13)
The rig's joints are named after Godot's `SkeletonProfileHumanoid`. That profile is what Godot's
importer retargets any character onto — Mixamo, Rigify, Ready Player Me — so adopting its names
means an imported character can be driven without a per-character mapping written by us. Clips that
still carry Mixamo names are matched through a small alias table at bind time, so existing assets
keep working.

Naming alone is not enough, and this is the part that decides whether it works at all:

- **Rotation relative to rest, not position.** `CinderboxSkeleton` used to push model-space poses
  into `set_bone_global_pose`, which forces every bone to where *our* rig has it and tears apart any
  character with different limb lengths. It now sets local rotations and lets the target's own rest
  supply the bone lengths, so proportions are the character's own.
- **Rest postures are bridged.** Our placeholder rests with its arms down; the profile's rest is a
  T-pose. Read naively, "our rest" and "their rest" would be treated as the same posture and every
  retargeted arm would stick out sideways. At bind time each mapped joint gets a constant that
  carries our rest posture onto the target's rest, so a pose that means "arms down" arrives as arms
  down on a T-posed character.
- **Hips are the exception that also translates**, scaled by the ratio of hip heights, so a taller
  character crouches by proportionally more.
- **Bones we do not drive keep their rest** and follow whatever drives their parent, which is what
  lets a character have bones our rig has never heard of.
- **`apply_anim_state()` is exposed to scripts**, so a rig can be posed without a server. That is
  what `check_retarget.gd` uses: it builds a humanoid with 1.35x legs and 0.7x arms, poses it, and
  asserts the arms come out of the T-pose, every bone keeps its rest length, and the legs swing.

None of this touches the simulation: bone names and poses are presentation, and the simulation only
ever deals in the animation mode, its time and the ground speed.

## Server gameplay mods (M14)
The rules of the game now live in **server mods**: C++ compiled into `cb_server`, running only on the
server. Clients stay presentation only: they simulate the engine (movement, physics, props,
ragdolls) and draw what the mods publish, but they never run a rule and never learn what a pistol is.

```
inputs ──> server: mods read the world + this tick's inputs ──> commands in the frame
                                                                      │
            every simulation (server and clients) applies them ◄──────┘  like inputs, deterministic
```

- **Commands, not access.** A mod never touches the simulation. It adds commands to the tick's
  authoritative frame (`Set`, `Emit`, `SpawnProp`, `Destroy`, `Push`, `Kill`, `Respawn`), and every
  simulation applies them in order after join/leave events and before movement.
  - Mods need no determinism of their own: their randomness and decisions reach clients as the
    values in the commands.
  - Clients predict inputs, not commands. A mod's effect shows up when its frame arrives and rollback
    folds it in, the same way a remote player's input does.
  - Mods read the state *before* the tick, so a hitscan is resolved against the world the shooter's
    input was predicted in, minus other players' mispredictions. No lag compensation is needed yet.
- **Targets** are NetIds, or a player slot (`SlotTarget`), which also works for a player joining in
  the same frame. A target that no longer exists is ignored the same way everywhere.
- **Board and events.** `Blackboard` (16 int slots per entity) and a global board are hashed state;
  mod events go into a ring like impacts. The **schema** (field names and types, event names, action
  names and suggested keys) goes out in the Welcome and in replay headers. It is never hashed:
  different mods on the same build are fine.
- **Actions.** `PlayerInput` gained camera pitch and 16 action bits (10 bytes). The engine keeps only
  jump and sprint; the spawn button became the `props` mod's `spawn_prop` action.
- **Mod state** lives in one flecs world shared by all mods (the `pistol` mod keeps a `Gunner` per
  player, plus `Dead` and `Reloading` while those last). This is where flecs earns its keep: queries
  over gameplay state, not the simulation's storage.
- **Protocol v4.** Commands travel in frames with a field mask each (a board write costs about
  10 bytes). The server drops non-finite commands before sending; the simulation also ignores them.

### Ragdolls
- `Kill` with a ragdoll builds eleven Box3D bodies from a **fixed standing pose** (`src/sim/ragdoll.h`),
  turned to the player's facing, carrying its velocity plus the hit's velocity change on the nearest
  part. No asset data enters the simulation.
- Joints: cone-and-twist for spine, neck, shoulders and hips; limited hinges for elbows and knees
  (checked: knees only fold backwards, elbows only forwards, no joint separates).
- Hashed, snapshotted, portable, and verified identical across Clang, GCC and MSVC with the rest of
  the scenario. The mod chooses lifetime and cap; the oldest go first.
- Presentation hangs any skeleton off the parts: each joint follows its nearest ragdoll part and keeps
  its own rest offset from that part's joint. The pose the player was last drawn in blends into the
  ragdoll over 0.15 s.

### Presentation as data
- **Bindings on names.** `CbEffect` gained mod events and local **action presses** (feedback that
  cannot wait a round trip, with conditions that say whether the server will accept it), board
  conditions, subject A or B, value filters, bones and beams.
- **State bindings** hold while conditions do: a held item at a joint, an arm aimed where the player
  looks, an AnimationTree parameter. **`CbFieldLabel`** puts fields in a HUD scene.
- Everything is resolved through the schema, so a binding for a mod that is not running never matches,
  and none of it is script: client mods can restyle all of it.

### Transport: ENet's throttle
ENet's packet throttle drops *unreliable* packets whenever the round trip rises. A Welcome (a large
reliable transfer) could make it drop every frame batch to that client for about half a second; the
client stalled at its prediction window, fell ~20 ticks behind, and every input it sent in the ~3 s
of rate correction arrived late, so the server discarded its presses. Frame batches repeat everything
unacknowledged, so dropping them never saved anything: throttle deceleration is now 0 on every peer.
(Found by the M14 network test; seen in about 1 run in 6 on this branch and never on master, for
reasons not pinned down beyond timing.)

### Measurements (Clang Release, loopback, 32 bots, 4 full ones firing the pistol, props on)
| Server tick | Late inputs | Full client work avg / max | Down / up per client | Server out | Desyncs |
|---|---|---|---|---|---|
| 0.7 ms (max 7) | 0% | 0.7–1.2 / 16 ms | ~55 / 68 kbit/s | 1.7–1.9 Mbit/s | 0 |

- Upload grew from 45 to 68 kbit/s: inputs are 10 bytes and each packet repeats 12 of them.
- Commands cost little: a kill is about 8 commands (fields, event, kill), resent until acknowledged.

### Honest limits
- Mods are compiled in; hot-loading or DLL mods would need a C API over `mod_api.h`.
- Hit detection uses the player capsule only (no head or limbs).
- A prefab animated only by an AnimationTree needs a `CinderboxSkeleton` for its ragdoll.
- The raylib viewer draws ragdolls and hides the dead; it has none of the new bindings.

## Workshop items, HUD from fields, names (M15)

### Mods are distributed like workshop items
A server mod has two halves. Its **rules** are C++ compiled into `cb_server`, built by whoever runs the
server. Its **look** is a workshop item that players subscribe to, like a Steam Workshop or
Counter-Strike mod. **The game connection never carries mod files.**

```
server_mods/pistol/client/  ──publish_mod.ps1──>  workshop: pistol/<sha256>.zip   (players have it)
                                     │
                                     └──> client_item.cfg (sha256) ──build──> bin/items/pistol.item
                                                                                    │
cb_server announces "pistol <sha256>" in the schema  <───────────────────────────────┘
client: has that exact item? load it, else leave and say what is missing
```

- **Identity is the content hash** (SHA-256 of the pack). Unambiguous: every player on a server sees
  the same look, and an update is a new item that servers opt into by announcing its hash.
- **Load order**: base game, then the server's items, then the player's own mods (loaded again, so
  they keep the last word). Bindings and `ui/hud_<mod>.tscn` overlays reload after items load.
- **A missing item refuses the join** with the list of items to get; half a look is worse than none.
- **The workshop is a folder for now** (`user://workshop/<mod>/<sha256>.zip`). `godot/workshop.gd` is
  the single place a real one (Steamworks UGC) plugs in.
- **The validator became an allowlist**, since items come from other people: known files in known
  places, redirects that stay in the pack, no compressed resources (they hid their contents from the
  old byte scan), and no script types or script paths in any resource. Honest limit: it cannot make
  Godot's own resource parsers safe against deliberately malformed files.

### HUD from fields
The health display used to be a label in the base game. It is now the pistol item's own 2D HUD, and
any mod can build one the same way, from ordinary controls plus four script-free nodes:
`CbFieldLabel` (text), `CbFieldBinding` (a field into any property of any node: a `ProgressBar`'s
`value` from `combat.health` and `max_value` from `combat.max_health`), `CbEventFeed` (a kill feed from
`combat.killed`) and `CbScoreboard` (players by name with field columns, while Tab is held).

### Names
Hello carries the name a player asks for; the server keeps it printable, trims it to 24 bytes on a
character boundary, makes duplicates unique ("Sam (2)") and sends the roster to everyone on the
reliable channel. Names are presentation: never simulated, never hashed. Protocol 5.

### Clock sync: catch up fast
A client a few ticks behind (a join, a hitch) used to speed up by at most 15%, so its inputs arrived
late for seconds and the server dropped its presses. Being behind costs input while being ahead only
costs latency, so beyond 1.5 ticks behind it now runs up to twice as fast. A 0.4 s stall went from 60
late inputs in the next second to 0; joins went from ~90 late inputs to ~8.

## Deathmatch rounds (M16)
The game loop is a server mod like any other (`server_mods/deathmatch`), so a server that wants no
rounds leaves it out. It needed three engine additions, none of them about rounds:

| Addition | Why |
|---|---|
| `Freeze` command (`Character::frozen`, hashed) | the intermission: players stop moving and acting but still look around, identically on every client |
| `Context::Option` and `--mod-option NAME=VALUE` | operators tune mods without recompiling |
| `RecentEvents`, `SpawnedProps`, `Ragdolls` | mods react to each other's events and clear the world between rounds |

| Phase | What happens |
|---|---|
| playing | `combat.killed` scores +1 for the killer; a world death (no killer) costs the fall penalty; the round ends at the kill limit or when time runs out |
| intermission | winner, round and seconds published; `deathmatch.round_end` emitted; everyone frozen |
| next round | spawned props and ragdolls destroyed, everyone respawned and unfrozen, scores reset, `game.round_start` emitted |

- **Mods cooperate by event, not by call.** The pistol knows nothing about rounds; it refills on
  `game.round_start`, which any game mode can emit.
- **Events are one tick late for other mods.** `RecentEvents` returns the previous tick's events, so a
  refill lands a tick after the respawn. Nobody can act in that tick, so this is harmless here.
- **Another mod's respawn is visible only through state.** The pistol now records the tick a player
  died and treats "alive again after that tick" as a respawn by someone else. Without that, a player
  killed in the same tick the pistol ran never came back (found by the deathmatch net test).
- **The look is a workshop item** (round HUD, winner banner, scoreboard with scores, two chimes). The
  pistol's scoreboard hides itself with the new `!?deathmatch.score` condition when the deathmatch
  one is there; `{name:deathmatch.winner}` shows the winner's name from a field holding a NetId.
- **Verified:** a net test (duel, first to 2) checks scoring, the frozen intermission, the winner and
  round 2 with no desyncs; a rendered session with 3 shooting bots saw 3 round ends and no desyncs;
  the scenario now freezes and releases players, and Clang, GCC and MSVC agree on all 1201 hashes.
- **Limits:** no teams and no spectators; a player joining mid-round just plays (one joining in the
  intermission waits frozen); every round puts each player back at its slot's spawn point.

## Continuous integration (M17)
`.github/workflows/determinism.yml` runs on every push. Determinism is the whole netcode's premise
(clients only exchange inputs), so CI checks it on every compiler and CPU family we can get:

| Build | Compiler | CPU |
|---|---|---|
| windows-clang | Clang 20 | x64 |
| windows-gcc | MinGW GCC 16 (MSYS2 UCRT64) | x64 |
| windows-msvc | MSVC 19.44 | x64 |
| linux-gcc | GCC 13 | x64 |
| linux-clang | Clang 18 | x64 |
| macos-arm64-clang | Apple Clang 17 | ARM64 (Apple silicon) |

- **Each build** runs all tests, then compares its 1201 per-tick hashes with
  `tests/reference_hashes.txt` and its pose hash with `tests/reference_anim_hash.txt`. A failure names
  the first tick that differs.
- **Cross-load**: all six portable snapshots must be byte-identical, and one job per OS loads each
  of them with each of its builds and runs them 900 ticks to the reference's final hash (30 load pairs
  in all; a binary only runs on its own OS).
- **First result**: everything agreed on the first run, including ARM64. This is the first check
  that is not x86: the flags that matter (`-ffp-contract=off`, no fast-math) already hold on ARM64.
- **Found and fixed (M18): Box3D wrote stale memory into snapshots.** Its snapshot writer copies
  structs out raw, padding included, and struct copies from stack temporaries carry stack bytes into
  that padding. Shapes also carried their hull's heap address. The simulation never reads those bytes,
  so hashes were unaffected, but every joining client received scraps of server memory and a heap
  address, and no two saves were byte-identical. `cmake/patches/box3d-snapshot-padding.patch`, applied
  when Box3D is fetched, zeroes the padding, the union bytes past the active member and the geometry
  union in each written copy; the image format is unchanged, so old and new builds interoperate. The
  holes were found from Clang's record layouts (`-Xclang -fdump-record-layouts`) and are named by the
  fields around them, so a Box3D update that renames a field fails to compile. `cb_tests
  portable_bytes` runs the scenario twice with the stack filled with different bytes and requires
  identical snapshots (330 bytes differed before), and CI requires all six builds' snapshots to be
  identical. Worth sending upstream.
- **Found and fixed (M18): ARM64 clamped to a zero of the other sign.** With the snapshots finally
  comparable, macOS differed from x64 in 37 bytes, all the sign bit of a manifold's `twistImpulse`.
  Box3D's `b3SymClampW` is `min( max( -b, a ), b )`; SSE's `maxps`/`minps` return the second operand
  when the values compare equal, NEON's `vmaxq`/`vminq` order -0 below +0, so with a zero friction
  limit x64 stored +0 and ARM64 -0. The hashes never saw it (warm-started zeros add nothing), but a
  signed zero can flip later through `atan2` or a division. `cmake/patches/box3d-neon-minmax.patch`
  gives NEON the SSE semantics (compare and select), which also matches SSE for NaN inputs.
- **Cost**: one run takes about 7 minutes of wall time. On a private repository macOS minutes count
  ten times and Windows twice, about 150 billed minutes per push.
- **Not covered yet**: Linux ARM64, the Godot extension build, and a Godot client in a session against
  a server built by another compiler (the fingerprint check covers the build flags, not a live session).

## Characters and hit zones (M19)
A character is a workshop item, and everything in it is baked at authoring time. Joining only
mounts packs that are already on disk: no import, no conversion, nothing sent by the server.

| Step | Where | What |
|---|---|---|
| Import | editor | Godot's own importer, retargeted to `SkeletonProfileHumanoid` |
| Author | editor | `CbCharacter` root, `CinderboxSkeleton`, `CbHitbox` shapes under `BoneAttachment3D` |
| Bake | editor (the Bake button) | ozz skeleton and six clips sampled from the AnimationPlayer, `anim.cfg`, `hitboxes.cfg` |
| Ship | `publish_mod.ps1 -Character` | the scene and the baked files, one hashed zip |
| Play | server | `--character NAME` opens the same zip, checks its SHA-256, reads the baked files |
| Play | client | mounts the item, `use_character()` reads `res://characters/<name>/` through `FileAccess` |

- **One source of truth.** The server reads the item players mount, and the hash guarantees
  identical bytes. The server never sends content: the schema only names the character, which is
  also one of the announced items (protocol 6).
- **Hitboxes are server-only.** Gameplay mods (and so hit tests) run only on the server. A ray is
  cast against the world with players left out, then each live player it passes near is posed from
  its `AnimState` at that tick and its hitboxes are tested; the closest hit in front of the world
  wins, and `RayHit::zone` names it. This costs clients nothing and keeps poses out of the
  rolled-back, hashed simulation. It stays deterministic anyway: the pose is a pure function of
  simulation state and the hashed item, with the scalar ozz build.
- **Spaces.** Hitboxes, poses and visuals share one space: the character's root at the feet, facing
  +Z. The bake refuses a `Skeleton3D` that is moved, turned or scaled relative to the
  `CbCharacter`, and the client drives the skeleton with `retarget` off, since the baked skeleton
  is that skeleton.
- **Mods stay character-agnostic.** They see zone names, not bones; the pistol's damage per zone is
  a server option (`pistol.zone.head=2`).
- **Validator.** `.ozz` and `.cfg` are allowed in packs only under `characters/`. They are data read
  by our loaders, never loaded as Godot resources. ozz's archive reader does little validation of
  its input, so a malformed `.ozz` in a pack someone installed could still crash the client (the
  same caveat as Godot's own resource parsers).
- **Verified:** unit tests for zones, misses and turning; the committed robot bake loads and its
  head is where the head zone is; a character item read from a zip (hash mismatch and missing
  hitboxes refused); a headshot session (two head hits kill, no desyncs); a rendered session with
  three bots playing as the robot (no desyncs).
- **Not done yet:**
  - capsule size and movement speeds per character (they change the simulation, so they need exact
    values on every client);
  - per-player character choice;
  - the visual aim offsets that state bindings add (an arm raised to aim) do not move hitboxes
    (done in M20: aiming is in the pose);
  - an imported, skinned character has not been through the whole path yet (the robot is generated
    rigid parts; the importer route is the standard Godot one but untested here).

## One pose for players (M20)
Since M19 the server hits players where their ozz pose puts them. Anything else that moved a
player's body on screen would be a lie: shots would land where nothing is drawn, or miss what is.
So the rule is: **the ozz pose places a player's body; Godot animation only adds.**

| Before | Now |
|---|---|
| The pistol's arm was raised by a client-only state binding (`aim_bone`) | `AnimState` carries `aiming` and the look direction relative to the body; mods set `aiming` with an `Aim` command; `PoseEvaluator` turns the character's aim chain, so clients draw it and hit tests use it |
| A `CinderboxAnimator` + `AnimationTree` could pose the whole body | A `CbPoseModifier` (first skeleton modifier) re-applies the ozz pose after any animation mixer; the tree only animates what the pose leaves alone |
| The example AnimationTree mod animated a box body | It animates a jetpack's flames; the body is the ozz pose |

- **Aim chain per character**: `aim` and `aim_tip` in `anim.cfg`, written by the bake from the
  `CbCharacter`, default the right arm. Several joints with weights are turned in order, so a chest
  can lean part of the way while the arm ends exactly on the line.
- **Mods decide when**: aiming is a gameplay rule, so the engine only offers the command. The pistol
  aims while it is out. Placing a character (respawn, falling out) keeps the flag, because the mod,
  not the engine, lets it go (found in a rendered session: a respawned player's arm hung down).
- **Determinism**: the aim angles come from the inputs and the body's facing inside the simulation
  (`WrapAngle`, `detmath::CosSin`), so they are hashed and identical everywhere; `AnimState` grew
  from 20 to 28 bytes (protocol 7).
- **A bug the checks caught**: `CinderboxSkeleton` looked for its modifier among the skeleton's
  ordinary children, but had added it as an internal one, so it added a new modifier every frame; a
  robot session fell to 40 FPS. `check_pose_wins.gd` now requires exactly one modifier, and exact
  driving writes local poses parents-first instead of `set_bone_global_pose` per bone.
- **Verified**: aim through `Evaluate` (straight, turned, a two-joint chain), a hitbox that is hit
  only while aiming, the robot's hand an arm's length in front of its shoulder while aiming, the Aim
  command and respawn in `commands`, the shooter aiming in `mods_session`, `check_pose_wins.gd`
  (with a negative run that must fail), the AnimationTree example still following the simulation,
  and a rendered session.
- **Not done**: a chain per weapon (one chain per character for now); IK for the second hand.

## Facing modes (M21)
A mod chooses how a player's body turns: freelook (the default: toward the direction of travel) or
camera-facing (toward the camera's yaw, snapped every tick, like third-person shooters). It is a
`Facing` command setting `Character::faceCamera`, read by the mover, so it is simulation state:
hashed, predicted and identical everywhere. The pistol uses it while it is out.

- **Legs follow movement.** `AnimState::legYaw` turns the hips toward the direction of travel,
  relative to the facing, clamped to 90 degrees and turned at 10 rad/s; the pose rotates the hips
  by it and the spine back, so only the legs turn. Past ~100 degrees away from the facing the legs
  walk backwards (`legsBackward`, the walk and run cycles reversed); below ~80 they walk forwards
  again, so walking exactly sideways does not flip every tick.
- **Why procedural legs.** Strafe clips (8-way blends) look better but every character would need
  more clips; this works with the six every character has. Upper/lower body layers chosen by mods
  (a pistol changes the upper body, a sword both) are the planned next step, and would replace it.
- **Verified**: `commands` (camera-facing snaps to the camera, strafing turns the legs 90 degrees,
  backing up reverses them, freelook turns the body back around), `pose_tools` (turned legs move the
  feet, not the head or hands), reference hashes regenerated (protocol 8), a rendered session.
- **Not done**: a freelook key while camera-facing (hold to look around without turning); strafe
  or layered clips.

## Layers and stances (M22)
Mods dictate what the body plays; characters provide the motion. A pistol changes the upper body, a
bat the whole body, and neither mod knows what a character looks like.

| Piece | Where | What |
|---|---|---|
| Layer | declared by mods (schema), at most 4 | a name; its bone mask comes from the character (`mask.<layer>`), `full` = every bone, `upper` defaults to `Spine` |
| Stance | declared by mods (schema) | a name; the character's clips `<stance>_<clip>` or one `<stance>` loop, falling back to the defaults |
| State | `AnimState` (hashed, 56 bytes) | per layer: stance, the one it replaced, seconds since set |
| Pose | `PoseEvaluator` with a `StanceTable` | base locomotion, then each layer's stance blended by per-joint mask weights (ozz joint weights), crossfading 0.2 s; then leg turn and aim |

- **Resolved per character**: `BuildStanceTable` turns the schema's names into masks and clips for
  one character, on the server (hit tests) and on each client (once the schema and character are
  known). Missing masks and clips are warnings, not errors: the body keeps its default motion there.
- **Swings are stances**: a single-clip stance plays by the layer's clock, which restarts when the
  stance is set, so a mod plays a one-shot by setting it and setting the ready stance back after.
- **Mods cooperate by event**: health stays in the pistol mod; the melee mod emits `combat.damage`,
  which the pistol applies (kills credited, deathmatch scores them). Facing has one owner per
  transition: a weapon turns camera-facing on when it comes out, the loadout turns it off with empty
  hands, so switching weapons never races two mods ticking in a fixed order.
- **Verified**: `stances` (the upper layer moves hands but not feet, the full layer moves feet, half
  way through a fade is about half way, missing stances and masks do nothing and are reported, no
  table means no stances), `commands` (the Stance command, out-of-range layers ignored, respawn
  keeps stances), `robot_character` (its baked stances resolve cleanly), the `melee` net session
  (ready and swing stances on the server, a kill by the bat, no desyncs), reference hashes
  regenerated (protocol 9), and a rendered session with the bat.
- **Not done**: the stance clips are placeholders (rigid robot, procedural box rig); no per-layer
  additive blending (layers replace, weighted); a freelook key while camera-facing.

## Companion tracks (M23)
Godot animations already carry VFX and sound: value tracks toggle `emitting`, audio tracks play
streams, method tracks call built-in methods. The bake used to keep only the bone tracks. Now it
splits each animation: bones to ozz (the shared pose), the rest to `companion.tres`, an
AnimationLibrary with the same clip names. The body stays the ozz pose's (hitboxes exact); the rest
is presentation, timed by the same simulation clocks.

- **Which clip, when**: `anim::ActiveClips` (pure, tested) gives per channel the clip and time the
  pose is playing: the dominant base clip, each layer's own stance clip.
- **Playing**: `CbCompanionPlayer` runs one hidden `AnimationPlayer` per channel in manual mode.
  Forward steps under 0.25 s `advance()` (keys fire); back steps, long jumps and clip changes
  `seek( t, true, true )` (values only). A high-water mark per clip makes a rollback's replay fire
  nothing twice. Deterministic mode is off, because channels share a library and would overwrite
  each other; when a channel resets to `RESET`, the others re-apply their values.
- **Godot facts found on the way**: animation players fire method keys only once they have been
  through a frame; `seek( t, true )` runs method and audio keys, `seek( t, true, true )` does not.
- **Verified**: `check_companion.gd` (value timing, one fire through a rollback, none on a long
  jump, loops every cycle, resets without clobbering), `stances` (ActiveClips), a rendered session
  with the robot's fire trail mid-swing, no desyncs.
- **Known**: an item's resources refer to its other files by an item-local uid:// that the game
  cannot resolve (the item's uid cache is stripped from its pack so it cannot replace the game's), so
  Godot warns once and uses the path. Harmless; cleaning it needs a pack-time rewrite.
- **Not done**: companion tracks for the partial weights of blends (only the dominant clip per
  channel plays); presentation state machines driven the same way.

## The default character (M24)
Players were blocks until now: the procedural rig. The default is now a real, skinned character
that ships with the game, the Universal Animation Library's mannequin (Quaternius, CC0).

- **Shipped, not downloaded**: `godot/characters/mannequin/` is in the base pack, so no item is
  announced for it. The build copies its baked files next to `cb_server` (`bin/characters/`), which
  reads them with the same loader as a workshop zip (`LoadCharacterFolder`). The schema no longer
  requires the character to be an item. Protocol 10.
- **Import**: the in-place glb, retargeted onto the humanoid profile by a `BoneMap` (UE names to
  profile names) with the rest fixer, so it is a character like any other: profile bone names,
  `Skeleton3D` at identity, baked by the ordinary bake.
- **Held items**: bindings were tuned on the placeholder rig's hand, whose `-Y` runs along the
  fingers; a profile hand points `+Y`, so the pistol pointed backwards. `AnimSet::AttachFrame`
  swings the placeholder's frame onto each rig's rest bone direction (shortest arc), which is
  identity on the placeholder and right for a T-pose with palms down. Only item attachment uses
  it; poses and hitboxes are unchanged.
- **Verified**: `mannequin_character` (loads without warnings, stands on the ground, the right zones
  are hit, aiming raises the arm, the held item points along the line of sight on both rigs), a
  rendered session with shooting bots and no desyncs, reference hashes unchanged.
- **Known**: the simulation's walk and run cycles (1.0 s, 0.7 s) are not the clips' lengths
  (`Walk` is 1.33 s), so the feet slide a little; the bat has no walk clip of its own (the ordinary
  walk plays); the aim chain turns the right arm only, so the left hand of `Pistol_Idle` can
  drift off the grip while aiming up or down.

## State machines from Godot (M25)
The goal from M23 was a presentation-only client with animations, tracks and state machines all
authored in Godot. M23 did the tracks; M25 does the state machine: a character's `AnimationTree`
is baked into data the simulation runs, as ozz already does for the bones.

- **Why the simulation**: which state plays decides the pose, the pose decides the hitboxes, and
  the server hit-tests those. So the machine must run the same everywhere and roll back:
  `UpdateAnimGraph` runs in `MoveCharacters` after the movement, per player, per layer.
- **Format** (`graph.cfg`, text): clips (length, loop, markers), layers (mask, weight expression),
  states (a clip, or a 1D blend space on an expression), transitions. Numbers go through
  `ParseAnimFloat`, plain double arithmetic, never the C library's locale-dependent parser.
- **Expressions**: compiled once to a small stack program; names resolve against the schema
  (built-ins, board fields, stances, events). Events are triggers: true on the tick a mod emits
  them at the player (commands apply before movement, so the same tick).
- **Markers** record the mod event of their name, from the player, as a command would. Mods see
  them next tick (`RecentEvents`), effect bindings play them, a rollback un-counts them.
- **State**: `AnimGraphLayerState` (32 bytes, per layer) in `AnimState`: state and previous, their
  clocks, time in state, crossfade length, the layer's eased weight, blend inputs. AnimState grew
  from 56 to 184 bytes; reference hashes were regenerated.
- **Transport**: the graph text rides in the schema (like the map in the welcome), so every client,
  bot and replay runs exactly the server's; a client whose copy differs warns and plays the
  server's. Protocol 11. Clips stay local (ozz files in the pack), found by animation name.
- **Pose**: `PoseEvaluator::SetGraph` samples each layer's state (blend points, crossfade from the
  previous state) and blends layers through their masks; the leg twist and the aim chain still
  run after it.
- **Bake**: reads the tree through Godot's API; unsupported pieces fail with a reason. In the game
  the tree is switched off before it enters the scene.
- **Found on the way**: the M24 `character.tscn` had lost its hitboxes (nodes under an instanced
  glb need Editable Children to be saved), and a script-instanced scene needs
  `GEN_EDIT_STATE_INSTANCE` or saving it copies the whole glb in (3.9 MB instead of 23 KB).
- **Verified**: `anim_graph` (numbers, expressions, a walk/jump/punch graph in a simulation, a
  marker once), `mannequin_character` (the baked tree end to end: idle, blend, jump, pistol draw and
  shot, swing marker once at 0.4 s), a rendered session of melee bots against the real server
  (hits landed on the marker, no desyncs), all hashes identical across builds.
- **The mannequin's feet**: M24's sliding is mostly gone: its blend space puts Walk, Jog and
  Sprint at the speeds they cover (about 0.9, 3 and 5 m/s, measured from the foot travel) and
  plays each at its own rate. Sprinting at 6.5 m/s still outruns the Sprint clip a little.
- **Not done**: nested state machines, 2D blend spaces, OneShot/Add/TimeScale nodes, `travel()`
  (only Auto transitions), per-transition reset memory (a state always restarts), crossfade curves,
  markers inside blend spaces, and previewing expressions in Godot's own editor (its Expression
  cannot read simulation values; conditions can be toggled by hand).

## Strafing with real clips (M26)
The mannequin walked sideways by turning its hips 90 degrees over a forward walk (the M21 leg
twist). The library's paid Source version has jogs in eight directions, so a character can now
blend real directional clips instead.

- **2D blend spaces**: Godot's `BlendSpace2D` bakes as points plus triangles and blends like Godot:
  barycentric inside a triangle, the nearest hull edge outside. Auto triangles are computed by the
  bake with Godot's own `Geometry2D.triangulate_delaunay` (Godot builds them in a deferred call).
- **Inputs**: `move_forward` / `move_right`, the ground velocity in the body's frame, smoothed like
  the speed, kept while airborne. Only camera-facing players really strafe; in freelook the body
  turns toward where it goes.
- **turn_legs** (anim.cfg, `CbCharacter`): off keeps the hips straight; the simulation still
  computes the leg yaw (it is state), the pose ignores it.
- **Paid content stays local**: the pack and `godot/characters/ual_mannequin/` are git-ignored;
  the generator (`--pack=source`) holds only names. The server prefers `ual_mannequin` where it is
  built, the game export includes it there, and clones and CI keep the CC0 `mannequin`.
- **Verified**: `anim_blend2d` (Godot's weights inside and outside the triangles, `move_right`
  from a real strafe), `ual_mannequin` locally (a right strafe plays `Jog_Right`, hips straight;
  skipped on CI), a rendered session against the server with pistol bots, no desyncs. AnimState
  grew to 224 bytes; reference hashes regenerated.
- **Not done**: walk and sprint have no side or back versions in the pack, so slow sideways
  movement blends idle with the sideways jog; clips play at their own rate, so moving faster or
  slower than a point's speed slides the feet a little.

## Facing forward while strafing (M27)
The paid pack's strafe jogs turn the hips 29-45 degrees toward the travel and the chest up to 50,
with only the head looking ahead: facing the camera, the character still looked off to the side.

- **face_forward** (anim.cfg, `CbCharacter`): after the layers, the pose measures how far the hips
  turned from their rest about the vertical and turns the spine subtree back by it; the neck
  subtree gets the turn back in proportion to how much of the neck the base layer set (the strafe
  clips already point the head ahead; an upper layer's clip, like the pistol's, did not).
- **Measured** (per direction, averaged over a second, since a jog twists the chest with every
  stride): chest within 14 degrees of the facing, head within 7, hips as the clip has them.
  It was 26-50 degrees for the chest.
- It is part of the pose, so the server's hit tests see the same torso; nothing in the simulation
  changed (hashes identical).
- **Freelook is unchanged**: without a weapon out, the body turns toward where it walks (the
  default the game chose in M21), so there is nothing to strafe; with the pistol or bat it faces
  the camera and strafes.

## The bat's fire belongs to the bat (M28)
The fire had been a companion track on the character's swing animation (its right hand burned). A
companion track can only reach nodes of the character scene, and the bat is the melee mod's: it is
attached by the mod's client item. So the effect moved to where the bat is.

- **Server**: the melee mod publishes `melee.swinging` (bool, per player) from the swing's start to
  its end; like any board value it is simulation state, so everyone agrees and rollback covers it.
- **Look**: a state binding in the item holds `bat_fire.tscn` in the right hand with the bat's
  offset and rotation while `loadout.slot == 3` and `melee.swinging`. Flames spawn along the barrel
  and stay in the world, so the swing draws a trail; any character gets it.
- **Characters**: the mannequins lost their hand fire (node and track); the swing keeps the
  `melee.strike` marker the mod hits on.
- **Timing**: the fire follows the mod's swing window (0.45 s), not the animation's keys. That is
  the trade: the mod owns the swing, the character owns the motion.
- **Verified**: the melee network test (the field is on during swings and off after, kills, no
  desyncs), a close-up of the swing with the bat placed as the client places it, a rendered
  session with melee bots. The melee item was republished.

## Held items and sockets (M29)
The rule for a presentation-only client: one small piece of state from the server, everything else
authored as data. A character's animations could reach only nodes in the character scene, and a
weapon was a mod's scene attached from outside, so "the sword's trail on this frame of the swing"
could not be authored; M28 fell back to a board flag with the timing in the mod.

- **Items are entities** (`HeldItem`: holder, kind, socket). `SpawnItem` replaces what the socket
  held; the item follows its holder and goes when the holder leaves. It has its own board and
  receives events like any entity. `ItemTarget( slot, socket )` resolves to it inside the
  simulation, so a mod addresses an item it spawned in the same tick, without its NetId.
- **Sockets** are `CbSocket` nodes in the character scene, in the item's frame; the client places
  them from the pose every frame (the same joint transforms the server's hit tests use) and parents
  the item under them as `Item`. RightHand and LeftHand are built in: a character without them gets
  them at the hands in the frame items were already made in.
- **Composition**: the character's animation keys *when* (an Animation Playback track on
  `.../RightHand/Item/AnimationPlayer`, baked into the companion tracks), the item's scene says *what*
  (its `slash`). An item's own state drives its AnimationTree (board fields as advance conditions),
  events sent to it play its animations.
- **Verified**: `held_items` (spawn and address in the same tick, events, following, snapshot and
  hash, replacement, leaving), the swing firing a stand-in item's `slash` at its 0.2 s key through
  the companion player, quiet empty sockets, the melee network test (the bat item held, hot after a
  hit, no desyncs), a rendered session with the bat glowing and trailing flames mid-swing. Reference
  hashes unchanged; protocol 12.
- **Not done**: the pistol still uses its attach binding (a held item since M33); items cannot be dropped into the world or
  handed over (only spawned and destroyed); rollback does not undo an item animation that already
  played (as with companion tracks, a replay does not fire twice, but a mispredicted swing that
  never happened has already shown).

## One event, resolved by what is held (M30)
"Attack" should look like a bat swing with a bat, a slash with a sword, a punch with empty hands,
from the one small command the server already sends. The resolver is the character's state
machine and the items' own animations; they only had to be able to see each other.

- **Item kinds are condition names**: true while the player holds one (any socket). The simulation
  collects who holds what once per tick for the graphs that run.
- **Events carry their value into conditions**: a fired event reads as its value, or 1 when that is
  0, so `attack` stays a trigger and `attack == 2` picks a heavy attack.
- **Events at a holder reach its items**: an event sent to a player plays each held item's
  animation of that name. Verified in a session: 23 of 23 `melee.hit` events played on the bat in
  the attacker's `RightHand` socket.
- **No game content in the engine**: the placeholder rig's pistol and melee stance clips are gone
  (the stance test uses the robot's), and the event feed has no default event. What remains built
  in is the humanoid-sandbox vocabulary: aim, facing, stances, ragdolls, sockets and held items.
- **Verified**: `attack_resolve` (the same event: Punch empty-handed, BatSwing holding a bat, Heavy
  on value 2), all suites, reference hashes unchanged.

## Animation packs (M32)
Mods author AnimationTree layers and swap a player's own for them. Decisions: swap by layer name (the
rest of the tree stays), packs live in the mod's workshop item, every character follows Godot's
humanoid profile.

- **Simulation**: each layer's state records its `source` (0: the character's; n: pack n-1). A
  `SwapLayer` command sets it and restarts the layer; `ResolveLayer` gives the layer that plays and
  the graph that owns its clips, shared by the simulation and the pose so they cannot disagree.
- **Transport**: the server loads each pack from its mod's item (SHA-checked zip, `anim/<pack>/`),
  puts its graph in the schema (like the character's) and compiles it; clients, bots and replays
  compile the same text. Protocol 13.
- **Retargeting** (`retarget.*`): per target joint, the source joint of the same profile name; its
  turn from rest (in its own frame) applied to the target's rest; the hips' movement scaled by the
  hips' heights; other bones keep the target's lengths. A clip for its own skeleton comes back exact
  (0.0000 m off); the same skeleton skips it. `ProfileName` learned every humanoid-profile name
  (the fingers were missing).
- **Fitting** (`FitPack`): once per character, shared by every player's pose and the hit tests.
- **Authoring**: `CbAnimPack` is a `CbCharacter` without hitboxes that bakes to `res://anim/`;
  items may carry `anim/<pack>/` data (the validator allows .ozz and .cfg there, never scripts).
- **Verified**: `retarget`, `layer_swap` (head 1.60 m, 1.18 m swapped, 1.60 m restored), the `sneak`
  network test (the server's pose: head 1.61 m, 0.88 m crouched, restored, no desyncs), a rendered
  session with sneaking bots, reference hashes unchanged.
- **Not done**: a pack's companion tracks (VFX keyed in its clips) are not played yet; packs replace
  layers the character has, they cannot add new ones; the placeholder rig does not conform to the
  profile's rest shape, so packs look off on it; crouching does not slow movement (speed is not a
  mod control yet).

## Reactions as nodes (M33)
Godot's `MultiplayerSynchronizer` copies node properties from an authority to peers. That does not
fit here: the server has no Godot scene (it runs the simulation), and what reaches clients is small
simulation state that has to *mean* something on screen. The missing piece was on the receiving
side: a way for a scene to say what it does with that state, without scripts.

- **`CbReaction`**: a node in any entity's scene. When: on a mod event, or while board conditions
  hold. Subject: the entity, or a held item's holder. Does: plays an animation (and an off
  animation), sets a property (sub-paths into materials; put back when a While ends), calls a
  built-in method with no arguments, adds a scene (freed after a lifetime, or when a While ends).
- **Client**: collects each visual's reactions when its node is made; checks every While each
  frame against the subject's board (and the global board); fires event reactions whose subject the
  event is at. Presentation only.
- **One mechanism instead of conventions**: gone are the item rules of M29/M30 (item board fields
  as AnimationTree conditions; events playing same-named animations on items and on a holder's
  items) and `CbStateBinding` (a scene kept at a joint, a tree parameter). Effect bindings stay for
  one-shots in the world and on screen.
- **The pistol is a held item** (`pistol.gun`, slot 2, gone when dead). Its scene moved into the
  socket frame at exactly the old attach offset (checked in Godot). The bat's glow and hit sparks
  are two reactions; its barrel material is local to the scene, or one hot bat lit every bat.
- **Found on the way**: taking your item away with `Destroy( ItemTarget(...) )` destroyed the
  *other* mod's item on a same-tick swap (melee spawns the bat, then the pistol's destroy resolves
  to it). Both mods now destroy the NetId `HeldItem()` gave. The melee network test swaps pistol ->
  bat and fails on the old code.
- **Verified**: `check_reactions.gd` (10 checks), all suites, reference hashes unchanged; a
  rendered session with `cb_bot --melee`: 14 hit-spark and 10 glow reactions fired, both item looks
  loaded, no desyncs.
- **Not done**: reactions to the built-in events (jumped, landed, footstep, impact); methods with
  arguments; an event reaction is not taken back when rollback removes its event.

## Entity paths (M34)
A reaction could only watch its own entity or its holder, and its node paths only reached its own
scene. Godot's node tree cannot be the way out: entity nodes are named by NetId (`player_52`), items
are reparented into sockets, players are rebuilt when the character changes. So entities are named
through the simulation's relations instead.

- **Paths** (`src/present/entity_path.*`, engine-independent): `self`, `holder`, `item:<socket>`,
  `event.a`, `event.b`, `local`, `world`, chained with `/`. Parsing and resolving are unit-tested;
  resolving takes callbacks (holder of, item in), so tests need no mirror.
- **Reactions**: `subject` is a path; `event_side` says whether the event names the subject as A,
  B or either; `act_on` is a path to the scene node paths resolve in; conditions take a
  `path:` prefix (`!holder:combat.dead`). The client keeps an index of held items per frame.
- **Limits** (the user's call: read anything, write only presentation, never outside entity
  scenes): node lookups outside the scene they resolve in return nothing; `free`, `queue_free`
  and `script` are refused. A While whose `act_on` moves to another scene ends in the old one first.
- **Verified**: `entity_paths` (parsing, errors, chains, empty hands, events, path conditions),
  `check_reactions.gd` (15 checks: `act_on` roots, restoring there, containment, refusals), a
  session with temporary reactions on the bat: acting on `event.b` found the victim's scene on each
  hit, side B fired when the holder was hit, `!holder:combat.dead` acting on the holder turned off
  at death; no desyncs. Reference hashes unchanged.
- **Not done**: reactions still live only in entity scenes (world reactions and replacing
  `CbEffect` are M35); paths cannot name "the nearest player" or an entity by template.

## World reactions (M35)
Effect bindings (`CbEffect` in a `CbEffectTable`, M9-M15) and reactions (M33-M34) did the same job
two ways: "when this happens, do that". Now there is one: `CbReaction`.

- **World scenes**: every `res://vfx/reactions*.tscn` is loaded once and given to the client
  (`add_world_scene`), which keeps it under a `World` node. Its reactions have no self; they name
  their subject from the event. Its `CbItemLook` nodes (now nodes, not resources) say how items look.
- **Events**: the simulation's own are names now (`spawned`, `destroying`, `jumped`, `landed`,
  `footstep`, `impact`) next to mod events, plus `pressed:<action>` for the local player's press.
  Entity scenes hear them too (a player's scene can react to its own footsteps).
- **What bindings had, as reaction fields**: kind and template filters, cooldown, placement (event
  point or end, a beam from the point or a bone, a bone, following the subject), sound, camera
  shake and flash (emitted to the game as `screen_effect`). `who` and `value_filter` and
  `min_strength` became conditions: `is_local`, `event.value > 0`, `event.strength >= 8`.
- **Converted**: all 29 bindings of the game, pistol, melee, deathmatch and the neon example, by a
  one-off script; the `res://vfx/<event>.tscn` fallback is gone (the game's own reactions name
  those scenes). `game.gd` lost its effect director; it loads scenes and applies screen effects.
- **Verified**: every reactions scene loads in its project; `check_reactions.gd`; all suites and
  reference hashes (`lossy_session`'s timing check fails on this machine right now, and did so with
  a binary from before M33 as well; it passes in CI); a rendered session with three pistol bots:
  126 impacts, 86 footsteps, 37 jumps, 35 landings, 14 spawns, 93 tracers (82 remote shots and 11
  predicted local ones), 77 player hits, 17 surface hits, 20 hurt flashes, 5 death flashes, no
  desyncs.
- **Not done**: a While in a world scene cannot place things at an event (it has none); screen
  effects are still applied by `game.gd`, so a mod replacing the camera must handle the signal.

## Reactions on the scene tree (M36)
M34 named entities through the simulation's relations (`holder/item:LeftHand`) because the client's
scene tree was not stable: nodes were named by NetId, sockets sat at a rig-dependent depth, and
nodes were rebuilt. The better fix was to make the tree stable and address it directly, the Roblox
way, and to split the reaction system out of Cinderbox.

- **Stable tree**: everything lives under the client's `World` node. Players are `player_<slot>`
  (the user's call: stable while connected), other entities `<kind>_<net id>`, the map `Map`, held
  items `Item` in their socket. A node being replaced gives up its name first.
- **Sockets are children of the entity** in the game: `CollectSockets` moves them there (placement
  was already from the pose, in world space), and `Head` joined the built-in hands. The companion
  tracks that reached an item through a socket's authored place are rewritten once per loaded
  library (`Armature/Skeleton3D/At_RightHand/RightHand/Item/AnimationPlayer` ->
  `../RightHand/Item/AnimationPlayer`); a live session showed the bat still burning mid-swing.
- **Anchors** (the user left the syntax to me): `^` my entity, `^^` the one above, `$at`, `$other`,
  `$local`, `$world`, then an ordinary NodePath. Anchors compose with Godot paths, so a new
  relation needs no new vocabulary; presets would have covered only what was foreseen.
- **Standalone addon** (`src/godot/cue`, library `cb_cue`, godot-cpp only): `CbDirector` holds
  entities (metadata: kind, template, `state`), the world state and the local entity, indexes the
  reactions that register themselves under it, fires them on `cue()` and re-checks Whiles on
  `update()`. The client is an adapter: `add_entity` on create, `set_state` when a board changes
  (hashed, so unchanged boards cost nothing), `cue` for every event, `set_local`.
- **Replaced**: M34's entity paths and `act_on` (every path can be anchored), `bone` (a socket
  path: `$at/RightHand`), the client's reaction bookkeeping. `CbItemLook` stays Cinderbox's.
- **Verified**: `check_reactions.gd` drives a bare director (18 checks: state and Whiles, `$local`
  following `set_local`, `^^`, `^/Timer`, side A and B, `$other:` conditions, `event.value`,
  placing at `$other/Head`, `is_local` and `screen_effect`, containment, refusal, lifetimes); all
  suites and reference hashes (`lossy_session`'s timing check still fails on this machine only);
  a rendered melee session: the companion path rewritten, 22 hits and 22 `SparksOnHit` through `^^`,
  footsteps, jumps, impacts, pistol tracers and predicted shots, no desyncs.
- **Not done**: an editor preview dock (pick a node, fire a cue, toggle state) comes next; the addon
  is its own library but still ships inside the Cinderbox extension.

## Cue Preview (M37)
Reactions were only visible in a running game (a server, bots, the right moment). The addon already
ran without Cinderbox, so the preview is a stage for it inside the editor.

- **A C++ editor plugin in the cue addon** (godot-cpp only), registered at the editor level with
  `EditorPlugins::add_by_type`: every project that has the extension gets it, including the mod
  client projects where items and reaction scenes are authored. A GDScript addon would have needed
  a copy in each of them.
- **The stage**: a SubViewport with its own world (floor, light, camera), a `CbDirector`, two
  stand-in players with sockets, and a copy of the edited scene (`duplicate()`, so unsaved edits
  show and the scene itself is never touched) as a held item, a character or world reactions.
- **Controls**: cue name (listed from the scene's reactions plus the game's), `$at` / `$other`,
  value, strength; one field per state name the scene's conditions read, per entity; the local
  player. Screen effects drive a flash overlay and a camera shake.
- **Verified in the real editor** (a temporary hook opened the panel, set state, fired a cue and saved
  the viewport): the bat glowed with `melee.hot` = 1, sparked on `melee.hit` and went back to wood;
  `reactions_pistol.tscn` put a hit puff at player_1's chest and the hit marker at `$at/Head`; the
  mannequin stood in as player_0. A hidden bottom panel does not draw, which first looked like a
  bug (the Animation panel takes over when a character opens).
- **Not done**: the stand-ins do not animate (a character's clips, and so its companion tracks, do
  not play); there is no timeline to script a sequence of cues; the camera cannot be moved.

## Reaction polish (M38)
A review of `CbReaction` before anything else is built on it.

- **Fixed**: a While leaving the tree while on (a world scene reloaded, an item put away) left its
  property set and its scene behind; a method needing arguments failed on every firing; `call`,
  `callv`, `propagate_call`, `set` and others could do what `queue_free` is refused for; a cue scene
  with `scene_lifetime` 0 was never freed, silently; a reaction that did not parse was skipped
  silently in the game; a While's looping animation kept playing after it ended.
- **New**: `method_args`; a Timing group (`delay`, `chance`, `cooldown`); `blend_time` (a tween, so
  the bat's glow can fade as its old AnimationTree crossfade did); `CbDirector.explain()`, which the
  Cue Preview uses to say what every listening reaction did or why not; screen effects are emitted
  by the reaction itself (after its delay).
- **Help**: a class reference (`doc_classes/*.xml`, compiled in with godot-cpp's
  `target_doc_sources`) for hover texts and F1; an inspector plugin that opens every group with an
  info line and a button for more; info buttons on the Cue Preview's controls. A missing colon in
  a path condition (`^^combat.health`) is now a warning with the fix.
- **Found on the way**: an unescaped `default=""` made the XML invalid and Godot dropped the whole
  reference without a word; the enum hint "A: ..." was read as `name:value`.
- **Verified**: `check_reactions.gd` (28 checks, 10 new: explain, method_args, the refused `call`,
  delay, chance 0, blend_time, a While undoing itself on leaving the tree); in the editor, the info
  rows, the popup and the class page (captured with a temporary hook).
- **Ideas not built**: a value that follows state continuously (a light's energy from
  `combat.health`); a reaction that sends a cue of its own (chains); a timeline of cues in the
  preview; text fields with anchor completion instead of the NodePath picker.

## Items in the world (M39)
Decisions (the user's): a dropped item is a physics body in the simulation; its shape is declared
by the mod in C++ for now (authored in Godot and baked is a later milestone); pickup rules and the
prompt are a mod, with the engine providing verbs and a few general pieces.

- **Simulation**: `HeldItem.holder == 0` means "lying in the world": the same entity (NetId, board)
  with a dynamic body of its kind's shape (`ItemShape`: box or sphere, centre in the grip's frame,
  mass). `DropItem` and `PickUpItem` move it between hand and world; `SpawnItem` with no holder puts
  one on the floor, and `SpawnItem` into a taken socket drops what was there instead of destroying
  it. Shapes travel in the schema (MCB8) and are set on every simulation (server, clients, replays).
  Commands carry a rotation as a unit quaternion's x, y, z. Protocol 14. Reference hashes unchanged.
- **Presentation**: the frame hands out the item's grip (its body sits at the shape's centre); the
  client moves the node between `World/item_<id>` and `<holder>/<socket>/Item`, and clears the
  holder's companion caches when it leaves a hand.
- **Mods**: melee and pistol now look at what is in the hand, not at the loadout slot: any
  `melee.bat` swings, any `pistol.gun` fires; a slot gives an item when the hand has none and takes
  back only its own. The new **pickup** mod: nearest item within 1.5 m along the ground (its
  distance was first measured from the body's centre, 1.4 m up, which put a bat at the feet out of
  reach), E to pick up, G to throw, drop on death, `pickup.spawn_each` to seed the map.
- **The prompt as data**: the director keeps ids for entities (`add_entity(..., id)`) and cue paths
  gained `<anchor>@field` (the entity a state field names); a "while" whose `scene_parent` resolves
  to another node starts over there; `CbPromptLabel` (upright, billboarded, fixed size); formats
  gained `{key:action}` and `{look:field}`; `CbItemLook.display_name`.
- **Found on the way**: the protocol refused command types above `SwapLayer` (so the first drop
  vanished between server and simulation); a prompt leaving an item drew the next item's name for
  one frame.
- **Verified**: `world_items` (fall and rest, rollback replays the fall exactly, pick up, drop,
  spawn into a taken hand drops, rotations sanitised or refused); the `pickup` network test (a bot
  throws its bat, walks after it and picks it up; two clients simulate the throw with no desyncs);
  `check_reactions.gd` (31 checks; `@field` follows and moves); a rendered session with items on the
  floor and the prompt reading "[E]  Pick up Bat" / "Pistol" on the right items.
- **Not done**: shapes authored in Godot; items walking their holder differently (their own
  layers); a hold-to-use duration on prompts; items expiring when nobody picks them up.

## Looks follow what is held (M40)
After M39 the rules followed the hand but the looks still asked for the loadout slot: a picked-up
pistol drew no HUD, and a bat held while slot 2 was out fired the pistol's predicted muzzle flash.

- **Item kinds are conditions** everywhere presentation reads state, as they already were in the
  state machines: `pistol.gun` is true while the player holds one. The client indexes held items per
  frame; `check_conditions` (the HUD nodes) answers item kind names through the conditions' extra
  names hook, and every player's reaction state carries a true/false per item kind.
- **The pistol's look** (HUD ammo, reloading, crosshair; the predicted shot and dry click) asks
  `pistol.gun` instead of `loadout.slot == 2`. No look reads the loadout slot any more.
- **Verified**: a rendered session with 80 items on the floor and a scripted player pressing E
  during its pistol phase: while it held a bat with slot 2 out, `pistol.gun` read false; all 45
  predicted-shot shakes across three runs happened with a gun in hand. No desyncs.

## One item per life, and expiry (M41)
M39's rule "a slot gives an item when the hand has none" let a player drop the bat, switch away and
back, and get another: unlimited items.

- **Inventory rule** (the user's choice, as in Counter-Strike): a slot gives its item once per life.
  Put away and taken out it returns; once it leaves the hand any other way while the slot is out
  (dropped, thrown, swapped for a pick-up), the slot is spent until the player dies. In melee and
  pistol: `spent`, set when the given item is no longer the one in the hand, cleared while dead.
- **Expiry is a mod of its own** (the user's call): `expire` watches every item each tick
  (`ctx.Items()`); one that has been held and then lies in the world for `expire.seconds` (60) is
  destroyed, and picking it up resets the clock. Never-held items are left alone, so seeded and map
  items stay. A first draft looked only every 15 ticks and could miss an item held between looks.
- **Verified**: the `inventory` network test (take the bat, throw it, switch to hands and back: no
  second bat, never more than one item; with `expire.seconds=2` the thrown bat is gone at the end;
  no desyncs), all suites, reference hashes unchanged.

## Item bodies authored in Godot (M42)
M39 declared an item's world body in C++ (`BoxItem(...)`), away from the scene it had to match.
Characters already author their hit zones in Godot and bake them; items now do the same.

- **`CbItemBody`**: a `CollisionShape3D` with a `mass`, in the item's scene (as `CbHitbox` is one
  with a zone), so Godot's own gizmo shows and edits it. Box or sphere, unrotated; its position is
  the shape's centre from the grip.
- **Bake**: `bake_items.gd`, run inside the mod's client project, follows every `CbItemLook` to its
  scene and writes `items/<kind>.cfg` (shape, half extents, centre, mass). `pack_mod.ps1` runs it
  before packing, the preset's `include_filter` ships `items/*.cfg`, and the client's pack validator
  accepts `items/` as data.
- **Server**: `loadItemShape( mod, kind )` reads the file from the declaring mod's item (hash
  checked, like animation packs) and replaces the declared or default shape before the schema goes
  out; clients still get shapes from the schema, so nothing changes for them. Declarations remember
  which mods declared each kind.
- **Mods**: the bat and the pistol have a `CbItemBody` and no `BoxItem` line; their baked files are
  committed. `BoxItem` / `SphereItem` stay for mods without a look.
- **Verified**: the bake reproduced the old C++ values from the scenes; `item_shapes` (parsing,
  refusals, the shipped files); the pickup test checks the bat's body is the baked one; the real
  server logged both bodies read from the published items; the pack validator check; all suites,
  reference hashes unchanged.
- **Not done**: a Bake button in the editor (baking happens on publish or by the command);
  capsules; several shapes per item.

## Hold-to-use prompts (M43)
Decisions (the user's): the hold time is a pickup option with a per-kind override from the item's
mod; progress lives on the player's board so every screen agrees.

- **Item properties**: a general way for mods to agree about items, like the board is for state. A
  mod declares a named number on an item kind (`ItemProperty( kind, "pickup.hold_seconds", 0.5 )`)
  and any mod reads it (`ctx.ItemProperty`). Server-side only: nothing goes to clients.
- **Pickup**: a press on the item in reach starts a hold; it counts while E stays down on that same
  item, and takes it after the item's hold time (0: the same tick, a tap as before). `pickup.hold`
  (seconds the item in reach needs) and `pickup.progress` (0..1) are on the board.
- **Look**: `CbPromptLabel.progress_field` draws a bar under the text (two quads with the label's
  billboard, fixed size and no depth test; the fill's mesh grows from the left). The pickup look has
  two reactions, chosen by `pickup.hold`: a tap prompt and a "Hold" prompt with the bar.
- **Cost**: progress is a command every tick of a hold, which clients cannot predict: the pickup
  test's rollbacks per client went from about 20 to about 85 over 8 seconds. Rollbacks are cheap
  (the stress test replays 8 ticks in a few milliseconds), so it stays simple; a start tick the
  client animates from would avoid it.
- **Verified**: the pickup network test (taps never take the bat, `pickup.hold` says it needs
  holding, progress passes through the middle, a held key takes it; no desyncs); a rendered session
  with both prompts on screen ("[E]  Pick up Pistol", "Hold [E]  Pick up Bat" with the bar at 47%);
  all suites; reference hashes unchanged.
- **Not done**: a ring instead of a bar; hold times authored in Godot; holding for other verbs.

## Items bring layers (M44)
Holding a bat should change how its holder stands and walks, wherever the bat came from, and stop
when it is dropped, without each mod writing swap code.

- **No new simulation state**: a layer's `source` stays a plain byte set by `SwapLayer` commands.
  The server decides which command to send. Mods' `SwapLayer` / `RestoreLayer` calls are now wishes
  it keeps per player and layer (`LayerWishes`); after every mod has ticked, `ResolveLayers` takes
  the mod's wish, else the pack of the first held item that has that layer
  (`Declarations::ItemLayers`), else none, compares it with the source the simulation shows, and
  sends a command only where they differ. Clients see ordinary commands, so nothing changed for
  them or for rollback, and the comparison with the real state heals itself (a respawn, a rejoin).
- **Order of say**: a mod's swap over an item's layers. The sneak mod's crouch replaces the bat's
  carry while the key is held and the carry returns after.
- **The bat's pack** (`melee.carry`, baked by `make_carry_pack.gd` from the CC0 clips: `Sword_Idle`
  made to loop, `Walk_Formal`, `Jog_Fwd`): a `Base` layer, shipped in the melee item.
- **Verified**: the `item_layers` network test (carry with the bat out, crouch over it, carry again,
  the player's own after the throw; never the wrong one; no desyncs); the sneak test, now finding
  its pack by name among several; the real server loading the pack from the published item; a
  rendered session with melee bots and no desyncs; all suites; reference hashes unchanged.
- **Not done**: how the carry looks was not judged by eye beyond "it plays" (the clips are
  placeholders from the free pack); packs still cannot add layers a character lacks; movement speed
  is not tied to the item.

## Joining from a menu (M45)
Until now the client took its server from the command line and showed "Connecting..." forever when
nobody was there. A stranger needs a place to type an address and an answer when it does not work.

| Piece | How |
|---|---|
| Menu | `res://ui/menu.tscn`, a scene with no script (packs cannot carry one), so mods can restyle it. `menu.gd` finds its nodes by unique name and skips missing ones; `game.gd` decides which panel shows |
| Remembered | `user://player.cfg`: name, settings (sensitivity, volume, fullscreen), the last 6 servers with their map |
| Why a join failed | `game.gd` watches the attempt: an address that names nothing (`GameClient::Stats::connectFailures`, counted only when `Transport::Resolves` says so), no answer in 10 s, the server's reject reason, a missing or refused workshop item |
| Leaving | The game scene is reloaded. The client node, its thread, the mirror and every cache start again, so no state of one server can reach the next (schema generations, entity nodes, held kinds) |
| Other mods next | Resource packs cannot be unloaded. If a server does not use an item this process has loaded, the game restarts itself with `--host` / `--port` (`OS.set_restart_on_exit`) rather than show that item's HUD and reactions |
| Camera | `present::CameraFreeDistance`: a ray against the frame's static entities (boxes grown by the camera's radius, spheres, capsules), on the main thread with the camera's current angles, so there is no frame of lag. In at once, out at 12 m/s |

- **Why not ask the simulation for the camera ray**: it lives on its own thread and is rolled back and
  re-simulated; a query from the main thread would need a lock or a frame of delay. The frame already
  carries every static shape, and the math agrees with `Simulation::CastRay` exactly on the built-in
  level (30 rays, difference 0).
- **Verified**:
  - `camera_collision` unit test (walls, a turned wall, spheres, capsules, props ignored, the level against the simulation's own rays).
  - `check_menu.gd` against a live server, windowed and headless: 41 checks (bad address, unknown host, dead port, cancel, join, name reaches the server, camera above the floor, Esc menu, settings, leave, recent list, rejoin with the items' HUDs back) and the restart into a server with other mods.
  - The autoplay smoke test (`--port`, no menu) with bots: no desyncs. A debug export: joins with autoplay, and opens in the menu without errors.
  - Unit tests and reference hashes unchanged. Network tests: all pass except `loopback_session` and `lossy_session`, whose timing checks failed on this machine that day with a binary built before M45 as well.
- **Found on the way**: after leaving and rejoining, an item's HUD was missing, because HUDs were only
  loaded together with new packs. They are now loaded once per scene.
- **Not done**:
  - No server browser: there is no master server, and LAN discovery was left out. Addresses are IPv4 or host names (ENet).
  - The restart into another server was not tried in an exported build, only from the editor binary.
  - Very close to a wall the camera ends up near the player's head, and the character is not faded out.
  - No key rebinding in settings (mod actions already are InputMap actions, so a page for it is possible).

## The inventory (M46)
**The bug.** Picking up an item sometimes dropped another slot's item, and switching away from a
picked-up item dropped it. Nothing in the client was involved: three server mods shared one hand
with no shared idea of what a player carries.

| Who | What it believed |
|---|---|
| `loadout` | only a number (`loadout.slot`) |
| `pistol`, `melee` | "my slot came out and the hand has none of mine: spawn one" |
| `pickup` | "E puts it in the hand; what was there drops" |
| simulation | `SpawnItem` into a taken socket drops what was there (M39) |

So slot 2 coming out threw a picked-up bat on the floor, and picking up with slot 2 out dropped
that slot's pistol and marked the slot spent (M41).

**The fix** gives items a place to be when they are not in a hand, and the slots one owner.

- **Engine: stowed.** `HeldItem::stowed` (the byte that was reserved, so snapshots and hashes keep
  their layout) with `socket` = a holster socket or `kNoSocket`. `HeldItemOf` only answers items in
  use, so everything that asked "what is in the hand" (mods, item layers, state machine conditions)
  ignores stowed items without a change. New command `MoveItem` (stow / take in use); `SpawnItem`
  and `PickUpItem` take `value = 1` for "stowed". A leaving player's stowed items go with it.
  Protocol 15.
- **Mod: `inventory`** replaces `loadout`. It reconciles every tick instead of trusting events:
  slots whose item is no longer carried are emptied, carried items in no slot are arrivals and get
  their kind's slot (pushing out what was there), then the slot that is out is made true in the hand
  with `StowItem` / `HoldItem`. That one rule covers a pick-up, a throw, an expired item and another
  mod handing an item over.
- **`pickup`** only makes the player carry the item (`PickUpStowed`) when an inventory runs; the
  old one-hand swap stays for servers without it.
- **`pistol`, `melee`** lost their give / take back / spent code. They declare three item
  properties (`inventory.slot`, `.start`, `.holster`) and otherwise only look at the hand.
- **Holsters** are a property with a socket (`ItemProperty( kind, name, SocketHandle )`, stored as
  socket + 1) and a `CbSocket` in the character scene. The engine has no notion of a holster: a
  stowed item is drawn in its socket if the character has it, else hidden. On the client a hidden
  one gives up the socket's `Item` name, so `^^/RightHand/Item` is always the one in use.
- **Client**: item-kind conditions (`pistol.gun`) count items in use only, so a holstered pistol
  shows no ammo HUD.

**Verified**
- `stowed_items` unit test: give stowed, the full-hand refusal, swap in one frame, a holster is not
  a hand, spawning into the hand never drops a stowed item, snapshot round trip, pick up to stowed,
  drop, leave.
- `inventory` network test, a bot steered to a lying bat: starts with 2 stowed; switching 2 and 3
  drops nothing; the pick-up swaps bats and keeps the pistol; switching away from the picked-up bat
  and back keeps it; a thrown bat leaves slot 3 empty; 6 items from start to end; no desyncs.
- `headshot` test: a death leaves nothing lying, the dead carry nothing, the next life has 2 items.
- All unit and network suites pass; the 1201 reference hashes are unchanged; reactions check passes.
- A rendered session with bots: bat across the back, pistol on the hip, pistol out with the bat
  still on the back, no desyncs.

**Not done**
- **No slot HUD** (done in M48: the board had 16 names per entity and all 16 were used).
- Holster positions were placed by numbers and checked in two screenshots, not tuned by eye. The
  paid mannequin got the same two sockets locally (it is not in the repository).
- Two kinds sharing one holster socket would be drawn on top of each other.
- A death was seen in one screenshot with the body upright in a T-pose at the instant of the kill;
  not looked into, and nothing here touches ragdolls.

## Authoring loose ends (M47)
Three leftovers from the item milestones.

| What | Before | Now |
|---|---|---|
| Hold progress | `pickup.progress`, a float the server set every tick of a hold | `pickup.since`: the tick the hold began. `CbPromptLabel` (`since_field`, `duration_field`) fills the bar from the client's own clock (`get_tick_time`, `get_tick_rate`) |
| Hold time | a C++ line in the item's mod | `properties` on the item's `CbItemBody`, baked as `property <name> <number>` lines into `items/<kind>.cfg` |
| Baking a body | command line, or publishing | also a **Bake item body** button on the node |

- **Why a start tick**: every board change is a command in the authoritative frame that a client
  could not have predicted, so a field that changes every tick makes every tick a rollback. In the
  pickup test the field now changes 8 times, and clients roll back 23 and 30 times over the run
  where M43 measured about 85.
- **Properties are generic**: the engine class knows no mod. `ParseItemShape` returns them next to
  the shape, and the server writes them over the declared item properties: what the scene says wins
  over what the code says. The bat's `pickup.hold_seconds` moved from `melee.cpp` to `bat.tscn`.
- **The button** finds the kinds by looking for `CbItemLook`s in `res://vfx/reactions*.tscn` whose
  scene is the edited one, the same rule `bake_items.gd` uses from the other side.
- **Verified**: `item_shapes` (property lines parsed, bad ones refused, the bat's file carries its
  hold time), `pickup` (taps still do nothing, progress seen from the two fields, few changes), the
  button pressed from a script writes the same file as the command line bake and refuses a property
  name with a space; a rendered client walked to a bat and held E: the bar fills; all suites pass,
  reference hashes unchanged.
- **Not done**: the button was pressed from a script, not clicked in the editor. A bake reaches
  servers only when the mod is published again (the item's hash changes), which the button says.

## A bigger board (M48)
Every field a mod publishes has a name, and a server's mods share 16 names per scope (entity,
global). M46 used the last entity name: one more field from any mod and the server refused to start.

- **`kBoardSlots` 16 -> 32.** One constant sizes the `Blackboard` component, the global board in
  `SimGlobals`, the presentation frame and the schema check. It is part of the snapshot layout, so
  every state hash changed: `tests/reference_hashes.txt` was written again (`cb_tests --dump`), and
  the protocol is 16. A board is 128 bytes instead of 64, on entities that have one (players, items).
- **Why not more, or dynamic**: a fixed array keeps the component a plain block of bytes (snapshots
  and rollback copy it as is). 32 leaves room (19 entity names are used now) without making every
  snapshot pay for space nobody uses.
- **The inventory's slots on the board**: `inventory.item_2` .. `item_4` (NetIds), and a HUD made of
  them (`hud_inventory.tscn`: two `CbFieldLabel`s per slot, one shown while the slot is out).
- **Verified**: all unit and network suites with the new size; the new reference hashes agree
  across the CI compilers; the `inventory` test checks the board's slots at the start, after the
  swap and after the throw; a rendered session shows the row ("1 Hands, 2 Pistol, 3 Bat, 4").
- **Not done**: the row shows names only (no icons, no ammo); when a server reaches 32 names the
  same wall is back, with the same one-constant fix.

## The viewer protocol (M49)
The Godot client was one class that both played (connection, prediction, rollback) and drew. It is
now a **viewer** that draws frames, and **sources** that produce them. [ROADMAP.md](ROADMAP.md)
has what this is for.

```
source  ──ViewFrame──>  viewer        the world at one tick, who is who, how the source is
source  <──input─────   viewer        the local player's input, for sources that play one
source  <──control───   viewer        named commands with a number ("pause" 1, "skip" -5)
```

| Part of a `ViewFrame` (`src/present/view.h`) | What |
|---|---|
| `state`, `stats` | how the source is: `"connecting"` .. `"playing"`, `"rejected"`; named numbers and words for a debug HUD and tests |
| `mapHash`, `mapName`, templates, `schema`, `names` | the session; generations say when they changed |
| `frame` | the `PresentationFrame`: entities, boards, animation states, ragdolls, event rings, the local player |
| `publishedAt`, `alphaAtPublish`, `rate` | where between two ticks the source was, and how fast it moves on (0 frozen, 1 playing, 2 a recording at double speed) |
| `localPressed` | the local player's presses, from a source that plays someone else's input |

- **A frame is self-contained.** A viewer that skips frames needs only the newest. Events are
  counters with a ring of the most recent, so nothing plays twice and nothing is lost.
- **Sources** (`src/client`, no rendering, no Godot):

  | Source | Does | Input | Controls |
  |---|---|---|---|
  | `LiveSource` | `GameClient` on a thread: joins, predicts, rolls back | the viewer's | none |
  | `ReplaySource` | `ReplayPlayer` on a thread: re-simulates a `.cbr`, checks its checksums | none | `pause`, `speed`, `seek`, `skip`, `step`, `follow`, `follow_next` |

  Both stand on `ThreadedSource`: the thread, the simulation's floating-point environment, and the
  newest frame handed across (a rollback or presses in a frame the viewer skipped are carried into
  the next).
- **The viewer knows no source.** `CinderboxClient` holds a `ViewSource` (since M51 one it is
  handed: see [Two extensions](#two-extensions-m51)). `get_stats()` is the source's stats,
  whatever they are; `control( name, value )` passes a command through.
- **A recording is watched as its player.** The followed player is the frame's local player, so
  the HUD, `is_local` reactions and the camera show what that player saw. Its presses arrive in
  `localPressed`, so `pressed:<action>` reactions play as they did live. The recording names the
  workshop items it needs; the client loads them as for a server.
- **Stats are generic** (`ViewStat`: a name and an int, float, bool or text) so the viewer does not
  include the client's types. The live source keeps the names the HUD and the checks already read
  (`rtt_ms`, `desyncs`, `checksums_verified`); the replay source reports recorded checksums it
  could not reproduce as `desyncs`, so an unattended run fails the same way.
- **`ReplayPlayer`** left the raylib viewer (`src/client/replay_player.*`); both viewers use it.

**Verified**
- `net_view_sources`: a `LiveSource` joins a server, plays for 3 s (ticks advance, serials grow, its
  input spawns a prop, no desyncs); the `ReplaySource` of that session follows slot 0, reaches the
  end at 16x with every checksum reproduced and the spawn presses reported; seek, pause, step and
  follow; a missing file is rejected with a reason.
- A Godot client on a server with four bots for 10 s: no desyncs, exit code 0. The same session
  watched from its recording: the six workshop items load, the followed player's HUD and kill feed
  show, no desyncs, exit code 0. A missing file comes back with the reason.
- All unit and network suites (Clang); GCC builds; the reference hashes are unchanged (the
  simulation was not touched). MSVC and the CI compilers have not built this yet.
- Six tests that existed but were never registered with `ctest` now are: `stowed_items`,
  `camera_collision`, `net_item_shapes`, `net_pickup`, `net_inventory`, `net_item_layers`.

**Not done**
- The playback keys in the Godot client (Space, arrows, `,` `.`, Home, N) were not pressed by hand;
  the controls behind them are what the test drives.
- Recordings carry no names: a watched player is "Player N".
- The menu has no "watch a recording" entry; it is `--replay=FILE`.
- The raylib client still has its own live and replay paths (it shares `ReplayPlayer` only).
- On the first full `ctest` run after this change `net_loopback_session` and `net_lossy_session`
  failed; alone, repeated three times each, and in a second full run they passed. They check
  real-time thresholds, and nothing they run was changed; not looked into further.

## Frames as bytes (M50)
A `ViewFrame` was a C++ object, so a source and its viewer had to be one library. It is now also a
packet of bytes (`src/present/view_codec.*`), which is what lets them be two libraries, a file, or
a network apart.

- **One packet is one frame**: whole, or a delta against a frame both sides have (the base, named
  by its serial). The encoder and decoder are two functions with the base passed in, so the same
  code serves a library boundary (the base is the frame before), a file (a whole frame now and
  then) and, later, a network (the base is the frame the receiver acknowledged).
- **Deltas are per 32-bit word.** An entity is laid out as a block of words with no padding
  (identity, shape, transform, velocity, the whole animation state, the whole board); a bit per
  word says which differ from the base, and only those travel.

  | What changed | Costs |
  |---|---|
  | nothing about an entity | 1 bit |
  | a board write | 1 word, plus the entity's 14-byte mask |
  | a prop that moved | its transform and velocity words |
  | a tick in which nothing changed, 155 entities | 106 bytes in all |

  The global board, the inputs and the two event rings are blocks of the same kind; the session
  (map, schema, names) and the stats are only in a packet when they differ from the base's.
- **Time travels as an age**, not a timestamp: sender and receiver have no clock in common.
- **Never trusted.** Counts are bounded, a short packet fails, slots and ragdoll indices are
  clamped. Every truncation of a packet is refused; 3000 packets with flipped bits either fail or
  decode to some frame, none crash.
- **View files** (`src/present/view_file.*`): `"CBVF"`, then packets, every 300th whole for seeking.
  `cb_server --record-view FILE` writes one (the world after each tick, names included);
  `ViewFileSource` plays it with the replay source's controls and no simulation, so it plays on any
  build. `godot -- --view=FILE` watches one; `cb_replay view FILE` summarizes one.
- **`ByteWriter` / `ByteReader`** moved from `src/net` to `src/sim/bytes.h`: the codec must not need
  the network library.

**Measured** (a server with bots that shoot, 21 s each, `cb_replay view`):

| Players | Entities | Whole frame | Delta frame, average / most | At 60 frames a second |
|---|---|---|---|---|
| 4 | 85 | 13.7 KB | 1.7 / 3.3 KB | 0.8 Mbit/s |
| 32 | 409 | 34.6 KB | 14.2 / 20.6 KB | 6.8 Mbit/s |
| 64 | 504 | 43.9 KB | 20.6 / 27.6 KB | 9.9 Mbit/s |

Fine between two libraries and for a file (1.2 MB a second at 64 players); too much for a stream.
Whatever moves sends exact floats every tick, and word deltas cannot shrink a float that changed.
ROADMAP.md says what a stream needs first.

**Verified**
- `view_codec`: 600 frames of the scripted session (joins, leaves, ragdolls, props, board writes),
  whole and as deltas: every one decodes to the same bytes it was; the session, stats, presses and
  age arrive; a delta is refused without its base or with another; truncated and damaged packets.
- `view_file`: 700 frames written and played: seek across the whole frames and back, step, skip,
  follow (next, a slot, nobody), 16x to the end with the followed player's presses, a file cut
  short, a missing file.
- A Godot client watching a 4-bot session from its view file: the workshop items load, names, HUD,
  inventory and kill feed show, exit code 0. Replays and live play still work.

**Not done**
- The frame sizes above were not broken down by what they are spent on.
- A view file is recorded by the server only; a client cannot record what it saw.
- On this machine the two late-input tests (`net_loopback_session`, `net_lossy_session`) failed in
  this session's last runs, the unchanged M49 build's as much as this one's: the machine, not the
  change. CI is the judge.
- Unattended Godot runs sometimes end with "ObjectDB instances were leaked at exit" (seen on
  recordings and view files, roughly every other run); not looked into.

## Two extensions (M51)
The Godot client is two GDExtensions with bytes between them. The simulation and the networking
are in one; the other cannot link them.

| Extension | Holds | Class | Who needs it |
|---|---|---|---|
| `cinderbox` (`libcinderbox.*`) | the viewer, the cue addon, HUD nodes, authoring and bake nodes, view file playback | `CinderboxClient` and the `Cb*` nodes | the game, and every mod's client project |
| `cinderbox_peer` (`libcinderbox_peer.*`) | the live and replay sources: `GameClient`, `Simulation`, rollback, ENet | `CinderboxPeer` | the game, to join servers and play recordings |

```
CinderboxPeer (peer library)                         CinderboxClient (viewer library)
  LiveSource / ReplaySource on a thread                ObjectSource: decodes, keeps the last frame
  take( whole ) ──── PackedByteArray: a packet ────>   as the next delta's base
  set_input( bytes ) <──── a PlayerInput, 10 bytes ──  set_input( move, yaw, pitch, ... )
  control( name, value ) <───────────────────────────  control( name, value )
```

- **A source is any object** with `take( whole ) -> PackedByteArray` (and optionally `takes_input`,
  `set_input`, `control`): `client.set_source( object )`. `CinderboxPeer` is one; a script is
  another (`check_object_source.gd` reads a view file's packets in 20 lines of GDScript).
- **Deltas across the wall.** The peer encodes each frame against the one it handed over before;
  the viewer decodes against the one it decoded before. `whole` asks for a packet that stands
  alone: the first time, and after bytes that did not decode, so the two cannot stay out of step.
- **Time across the wall.** Each library has its own clock, so a packet carries the frame's age at
  hand-over, and the viewer interpolates from there.
- **Why the viewer cannot simulate**: the libraries it links do not contain a simulation.

  | Library | Was | Now |
  |---|---|---|
  | `cb_sim_data` | part of `cb_sim` | components, events, the map format, the schema, the baked state machines: what the data means |
  | `cb_sim` | everything | `Simulation`, rollback, the physics arena, the fingerprint; links `cb_sim_data` |
  | `cb_present` | linked `cb_sim` (for `CaptureFrame`) | links `cb_sim_data` and `cb_anim` only |
  | `cb_capture` | part of `cb_present` | `CaptureFrame`: a simulation's state as a frame. Sources, the server and tests link it |

  The viewer links `cb_present`; the peer links `cb_client_core` (which links `cb_capture`,
  `cb_net`, `cb_sim`). `ImpactRecord` and `ModEventRecord` moved to `sim/events.h` and the flecs
  world-creation lock to `sim/world_lifetime.*`, so presentation needs neither `simulation.h`.
- **What the viewer still does itself**: poses (ozz and the baked state machines run from the
  animation state where it is drawn) and view files (no simulation involved).
- **The game** (`game.gd`) makes a `CinderboxPeer` when the class exists, sets `host`, `port`,
  `player_name` and the rollback window on it (they left `CinderboxClient`), and hands it to the
  viewer. Without the peer extension the game still watches view files and says why it cannot join.
- **Mod projects get the viewer only**: `tools/pack_mod.ps1` copies `libcinderbox.*`, never the peer.

**Verified**
- The viewer library holds neither the simulation's nor the rollback's abort messages; the peer
  library holds both (a search of the two DLLs). Its link line names no `cb_sim`, `cb_net` or
  `cb_client_core`.
- A Godot client through both extensions: 10 s on a server with four bots (no desyncs, exit 0),
  the recording of that session (checksums reproduced, exit 0), its view file (exit 0).
- The melee mod's client project, packed with the viewer alone: `CinderboxClient`, `CbReaction`,
  `CbItemBody`, `CbCharacter` exist, `CinderboxPeer` does not, the bat's scene loads, the pack is made.
- `check_object_source.gd`: a GDScript source feeds the viewer a view file's packets; state, world,
  map and mods arrive; after garbage bytes the viewer asks for a whole frame and plays on.
- With no peer extension registered, joining comes back with "this copy of the game has no peer
  extension" (seen before the project was re-imported).
- All unit and network suites but `net_lossy_session`, which fails on this machine today for the
  M49 build too (see M50); the reference hashes are unchanged.

**Not done**
- The release extensions build (viewer 3.7 MB, peer 2.5 MB); an exported game was not made and
  run with them.
- Linux and macOS builds of the peer extension are untested (CI builds no Godot extension).
- A project that already had the old single extension needs one `godot --path godot --import` (or
  opening the editor) so Godot registers `cinderbox_peer.gdextension`.
- `check_menu.gd` follows the renames but was not run.
- A frame is copied twice more per drawn frame than before (encode, decode); not measured, and not
  visible in the frame rate of the runs above.

## Packs are contained (M52)
The pack validator refused scripts, and a scene with no script could still act: any built-in node
could be in it, and a reaction called any method not on a short list of forbidden ones. A pack
could carry an `HTTPRequest` and a reaction that calls `request` on it.

The fix is a second check at the other end: not what a pack's files are, but what a scene is made
of, just before it is instantiated (`src/godot/cue/cue_guard.*`, in the cue library, read from the
scene's `SceneState` without creating a node).

| A scene is refused for | Because |
|---|---|
| a node whose class is not on the list | the list is what looks are made of; everything else (network, windows, cameras, viewports, timers) is out |
| a script on a node or on a resource a node holds | second line behind the validator's byte scan |
| any signal connection | a wired signal is a method call nobody listed |
| a `NodePath` property that climbs above the scene's root, or is absolute | a look reaches nothing outside itself |
| an animation method track that calls an unlisted method | Godot runs these itself; they never pass through `CbReaction` |
| an animation track whose path has `..` or starts at the root, or that sets `script` | the same containment for what animations move |
| any of these in a scene instanced inside it | all the way down |

- **One method list** for reactions and animation method tracks (`restart`, `play`, `stop`, `show`,
  `hide`, a few setters). It replaced `CbReaction`'s list of forbidden names: what is not listed is
  not called.
- **Advance expressions are cleared** in the game. An `AnimationTree` evaluates them against a
  node, so they can call its methods; the baked state machines the simulation runs do not use the
  tree at all. Not in the editor, where the scene being edited is the author's own.
- **Where it runs**: every place a moddable scene becomes nodes. The viewer's prefabs, items,
  characters and map scene; a reaction's `scene`; the HUD, item HUDs, world reactions and the menu
  (`CbDirector.instantiate( scene )` from scripts); a character's companion animations
  (`CheckResource`). A refused scene is not instantiated, a warning names what was found, and the
  game shows what it shows for a missing one.
- **Own nodes by prefix**: a class named `Cb*` or `Cinderbox*` that exists is allowed. A pack
  cannot define a class, so the names cannot be borrowed.

**Verified**
- `check_guard.gd`: the game's 16 scenes pass; refused: `HTTPRequest`, `Window`, `Camera3D`,
  `SubViewport`, `LinkButton`, `Timer`, one nested in an instanced scene, a scripted node, a wired
  signal, a path climbing out, a path from the root, method tracks calling `queue_free` and
  `call_deferred`, tracks climbing out or from the root, a track setting `script`; accepted: a
  method track calling `restart`, a value track, skinned meshes with a sibling path, a reaction and
  its target. A reaction does not call `queue_free` or `propagate_call`, calls `hide`, and adds
  nothing when its scene is refused.
- A live session with the six workshop items and the local character: nothing refused, no desyncs.

**Not done**
- The check runs when a scene is used, not when a pack is loaded: a bad scene in a pack is found
  the first time the game wants it. The pack tool does not run it for authors yet.
- The robot character and the two example mods were not run through it (their projects hold old
  copies of the extension).
- An `AnimationPlayer` at a scene's root keeps Godot's default `root_node` (`..`), one level above
  the scene: its tracks can reach its parent's other children, no further.
- `CbFieldBinding` writes a number into any property of its target; the target path is contained
  like every path, the property is not on a list.
- What the allowed classes do by themselves is not judged: a sound that plays at full volume on
  load, a UI panel over the whole screen.

## Smaller frames (M53)
An exact frame is what two libraries of one process hand each other. A stream cannot afford it:
6.8 Mbit/s for 32 players (M50). Two things make frames that a network can carry, and a view file
can now be recorded with both, which is how they are measured and watched before a stream exists.

- **Compact packets** (`ViewPrecision::Compact`). The picture, not the state:

  | Part of an entity | Exact | Compact |
  |---|---|---|
  | position | 3 floats when any changed | how far it moved on a 1/512 m grid: 1 or 2 bytes an axis |
  | rotation | 4 floats | 32 bits (the largest component named, the other three in 10 bits) |
  | velocity | 3 floats | not sent (only rollback smoothing reads it) |
  | animation state | the words that changed | floats on a 1/1024 grid, as how far they moved; the rest as it is |
  | an item in a hand | its holder's transform, every tick | no transform: it is drawn in the socket |
  | inputs | all 64 players' | not sent (nothing draws them) |

  Whatever lands on the same grid point as before is not sent, so a body that settles stops
  costing anything.
- **The two sides stay on the same grid.** The sender keeps exact frames, the receiver only what it
  decoded, and a delta is "how many grid steps from the base". They agree because a value that
  came through a compact packet is exactly on the grid (an integer below 2^24 over a power of two
  is exact in a float), so both compute the same grid point for the base. Rotations are not
  deltas: the code is sent when it differs from the base's code, which only the sender computes.
- **Fewer frames than ticks** (`ViewFrame::stride`). A source may send every third tick; the
  viewer draws from the frame before to this one over that many ticks. What happened in between
  is not lost: events are counters with rings. `cb_server --record-view FILE --view-rate 20`.
- **Also smaller, in both precisions**: when entities come and go, only those are named (it was
  every NetId: 1.4 KB of a 3.9 KB frame for 32 players); an event ring sends the records that are
  new, not a mask over all its words.
- **`cb_replay view FILE`** says where an average delta frame's bytes go.

**Measured** (a server with bots that shoot, 21 s each):

| Players | Exact, 60 a second (M50) | Compact, 60 a second | Compact, 20 a second |
|---|---|---|---|
| 4 | 829 kbit/s | 161 | 67 |
| 32 | 6809 kbit/s | 1072 | 429 |
| 64 | 9909 kbit/s | 1648 | 671 |

Where a 32-player compact frame at 20 a second goes (2684 bytes): animation 768, positions 619,
rotations 575, events 243, which entities and which changed 229, header and stats 92, ragdolls 72,
identity and step counts 62. The aim was 300 kbit/s for 32 players; it is 429. ROADMAP.md lists
what is left to take.

**How far the picture is from the truth**: at most 1.7 mm and 0.22 degrees, over the scripted
session.

**Verified**
- `view_codec`: 600 compact deltas in a row, the sender on exact frames and the receiver on what it
  decoded: every frame is bit for bit what one whole compact packet of the true frame decodes to
  (no drift); the error bound above; 3000 damaged compact deltas, none crash; exact packets still
  round-trip to identical bytes.
- `view_file`: every third tick, compact: a third of the frames, each lasting three ticks; seek and
  step land on the right tick with the true world on the grid.
- A Godot client watching a compact 20-a-second view file of a 4-bot session: items, HUD, names,
  kill feed, exit code 0; the script source check passes on the same file.

**Not done**
- Whether 20 frames a second looks smooth was not judged by eye: a still frame looks right.
- A compact view file has no inputs, so `pressed:` reactions do not play for the player it follows.
- View files written before this (packet "CBV1", file version 1) no longer read.
- The header is 75 bytes a frame of fixed-width fields; nothing was done about it.

## Tooling
- **Determinism test**: replays a scripted input log and compares per-tick hashes, both between repeated runs and between different builds (`scripts/check_determinism.*` locally, CI on every push).
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
The ordered plan for the client is in [ROADMAP.md](ROADMAP.md). These are loose ideas beside it.
- **Less download at high latency**: skip frames that are probably still in flight and resend them only after a timeout. This trades bandwidth for a slower recovery from loss.
- **Camera**: fade the character out when the map pushes the camera up against it (collision itself is in, M45).
- **Godot**:
  - ozz clips loaded from packs;
  - a real imported character: retargeting is checked against a synthetic humanoid, but no
    skinned model with a mesh has been through the pipeline yet;
  - Linux and macOS exports (the extension builds with `unix-clang-release`; not yet tested).
- **Maps, next steps**:
  - shapes beyond boxes, spheres and capsules, and a way to author them without one node per box;
  - static geometry left out of the portable snapshot: it is immutable and both sides build it from
    the map, so a large map should not pay for it on every join;
  - templates shared between maps, instead of one copy per map file.
- **Presentation sandbox** (M9-M11): effects, sounds and screen effects are data, and impacts and
  footsteps are reported by the simulation. Scrapes (sustained sliding contact) would need contact
  persistence rather than one-off hit events, and surfaces (what was hit, not just how hard) would
  need a material id in the map's material component.
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
11. **M11** (done): impacts from Box3D contact events and footsteps from stride distance, both part of the hashed simulation state and both rollback-safe, exposed to presentation as counters and bound to effects by `min_strength`.
12. **M12** (done): `CinderboxAnimator`, which drives a Godot AnimationTree from the simulation's animation mode, ground speed and locomotion phase, with drift resync; a generated example prefab shipped as a mod, and a headless check that a prefab's tree follows the simulation.
13. **M13** (done): the rig renamed to Godot's humanoid profile with Mixamo aliases, and `CinderboxSkeleton` retargeting by rotation onto a character's own rest, bridging the arms-down and T-pose rest postures, verified on a deliberately differently proportioned humanoid.
14. **M14** (done): server gameplay mods in C++ with commands in the authoritative frame, a board and mod events, the mod schema sent on join, pitch and mod actions in the input, deterministic ragdolls, data-driven presentation of mod state (conditional effects, predicted action feedback, held items, aimed arms, HUD labels), a pistol demo (loadout, props, pistol mods), `cb_bot --shoot`, and ENet's throttle drops disabled. Verified identical across Clang, GCC and MSVC.
15. **M15** (done): mods' looks as workshop items announced by hash and never sent (publish tool, local workshop, join refusal, load order), the pistol's look moved into its item, an allowlist pack validator with a hostile-pack check, HUD nodes driven by fields and events (health bar, kill feed, scoreboard), player names, and a fast clock catch-up.
16. **M16** (done): deathmatch rounds as a server mod with its own workshop item, the `Freeze` command, mod options, event and world queries in the mod API, the `!?field` condition, `{name:field}`, scoreboard conditions, and a deathmatch net test. Verified identical across Clang, GCC and MSVC.
17. **M17** (done): CI on GitHub Actions: Windows (Clang, MinGW GCC, MSVC), Linux (GCC, Clang) and macOS ARM64 (Apple Clang) each run every test and match the reference hashes, and every OS continues every build's portable snapshot. Identical on the first run.
18. **M18** (done): Box3D snapshots zero padding, stale union bytes and geometry pointers (a patch applied at fetch), so they no longer leak server memory to joining clients; ARM64 min/max match x64 for signed zeros (a second patch); snapshots are byte-identical across all six builds, checked by `portable_bytes` and CI.
19. **M19** (done): characters as workshop items baked in the editor (`CbCharacter` Bake button: ozz skeleton and clips, `hitboxes.cfg` from `CbHitbox` zones), `cb_server --character` reading the same zip players mount (SHA-256 checked, miniz), the client playing as it from the pack, server-side hit tests against posed hitboxes with zones for mods, the pistol's damage per zone, the robot example item, and tests.
20. **M20** (done): aiming in the ozz pose (`Aim` command, aim chain per character, hit tests follow it), `CbPoseModifier` so Godot animation on players is cosmetic only, the AnimationTree example turned cosmetic (a jetpack), bake warnings for animation that could move hitbox bones, and the modifier-per-frame bug fixed.
21. **M21** (done): facing modes chosen by mods (`Facing` command: freelook by default, camera-facing for the pistol), legs that turn toward the direction of travel with the spine turned back and a reversed walk when backing up.
22. **M22** (done): animation layers (bone masks from the character) and stances (clip sets with fallback) declared by mods and set with a `Stance` command, blended per joint with fades, in the pose the server hit-tests; the pistol's upper-body stance, a melee mod with a full-body stance and swing, `combat.damage` between mods, bake support (`stance_clips`, `masks`), the robot's stance clips, and tests.
23. **M23** (done): companion tracks: the bake keeps every non-bone track of a character's animations in `companion.tres`, and `CbCompanionPlayer` plays them per channel in step with the ozz pose (values exact, keys once through rollbacks, RESET between clips); the robot's bat swing gets a fire trail and a whoosh as ordinary tracks.
24. **M24** (done): the default character: the Universal Animation Library's mannequin retargeted onto the humanoid profile, shipped with the game and read by `cb_server` from `bin/characters/` (no item needed), and a hand frame for held items that is the same on every rig.
25. **M25** (done): state machines authored in Godot: a character's `AnimationTree` (state machines, Blend2 layers, 1D blend spaces, Godot's transitions with conditions and expressions) is baked to `graph.cfg` and run by the simulation, travelling in the schema; markers emit mod events (the melee swing strikes on one); the mannequin's tree with a flaming swing; `cb_bot --melee`.
26. **M26** (done): strafing with real clips: 2D blend spaces (Godot's triangles), `move_forward` / `move_right`, `turn_legs` per character; `ual_mannequin`, a local-only character built from the paid Source pack with eight-way jogs (the pack and its bakes are git-ignored), preferred by the server where it exists.
27. **M27** (done): `face_forward`: the chest (and head) face where the body faces while strafe clips turn the hips; on for `ual_mannequin`.
28. **M28** (done): the bat's fire is the melee mod's look: `melee.swinging` on the board during a swing, `bat_fire.tscn` held like the bat while it holds; the characters' hand fire is gone.
29. **M29** (done): held items as entities with their own state (`SpawnItem`, `ItemTarget`), sockets (`CbSocket`, built-in hands), item looks (`CbItemLook`), item boards as AnimationTree conditions and events as animations, characters' animations playing the held item's animations; the melee bat converted.
30. **M30** (done): one event resolved by what the player holds: item kinds and event values in state machine conditions, events at a player played by its held items (the bat's hit sparks); the engine's placeholder rig and HUD lose their game content.
31. **M31** (done): item swaps clear the holder's companion track caches, so a bat taken out again still slashes; `cb_bot --melee` swaps weapons.
32. **M32** (done): animation packs: mods ship AnimationTree layers (`CbAnimPack`) and swap a player's layer for them by name (`SwapLayer` / `RestoreLayer`), in the simulation and every pose; clips retargeted by humanoid-profile names; the `sneak` mod's crouch.
33. **M33** (done): `CbReaction` nodes (on event / while, self / holder; animation, property, method, scene); the implicit item rules and `CbStateBinding` removed; the pistol as a held item; the bat's glow and sparks as reactions; item swaps across mods destroy by NetId.
34. **M34** (done): entity paths (`self`, `holder`, `item:<socket>`, `event.a`, `event.b`, `local`, `world`) for a reaction's subject, conditions and `act_on`; `event_side`; lookups contained in the scene they resolve in, `free` / `queue_free` / `script` refused.
35. **M35** (done): world reactions: `vfx/reactions*.tscn` scenes loaded once replace `CbEffect` / `CbEffectTable`; built-in events and `pressed:<action>` by name; filters, cooldown, placement, sound and screen effects on `CbReaction`; `CbItemLook` as a node; all bindings converted.
36. **M36** (done): the scene tree as the address space: a stable World tree (`player_<slot>`, sockets as entity children, companion tracks rewritten), anchors (`^`, `^^`, `$at`, `$other`, `$local`, `$world`) on ordinary NodePaths; `CbDirector` + `CbReaction` as a standalone addon (`cb_cue`) driven by cues and entity state; M34's entity paths replaced.
37. **M37** (done): Cue Preview, an editor bottom panel in the cue addon: the edited scene on a stage with stand-in players, cues fired and state set by hand, screen effects shown; verified on the bat, the pistol's world reactions and the mannequin.
38. **M38** (done): reaction polish: fixes (a While undoes itself when it leaves the tree, a wider refused list, warnings for leaks and parse errors), `method_args`, `delay` / `chance`, `blend_time`, `explain()` in the Cue Preview, and in-editor help (class reference, info rows, info buttons).
39. **M39** (done): items in the world: physics bodies of declared shapes, DropItem / PickUpItem / SpawnItem on the floor, spawn into a taken hand drops; melee and pistol follow what is in the hand; the pickup mod (E, G, drop on death, spawn_each) with a data-only proximity prompt (`CbPromptLabel`, `$local@field`, `{key:}`, `{look:}`); protocol 14.
40. **M40** (done): looks follow what is held: item kind names are conditions in the HUD and in reactions; the pistol's look asks `pistol.gun`, not the loadout slot.
41. **M41** (done): a loadout slot gives one item per life (dropping no longer duplicates); the `expire` mod removes items that were held and then left lying (`expire.seconds`); `ctx.Items()`.
42. **M42** (done): item bodies authored in Godot: `CbItemBody` in the item's scene, `bake_items.gd` (run by packing) writes `items/<kind>.cfg`, the server reads it from the mod's item; the bat and pistol converted.
43. **M43** (done): hold-to-use prompts: item properties (`ItemProperty`), the pickup mod's hold (`pickup.hold_seconds`, `pickup.hold`, `pickup.progress`), `CbPromptLabel.progress_field` and its bar, a second prompt scene.
44. **M44** (done): held items bring layers: `ItemLayers( kind, pack )`, mods' swaps kept as wishes and resolved with item layers into `SwapLayer` commands (a mod's swap wins); the bat's `melee.carry` pack.
45. **M45** (done): join menu (address, recent servers), Esc menu and settings in a script-free scene; failed joins come back with the reason; leaving reloads the scene, other mods restart the game; camera collision against the frame's static shapes.
46. **M46** (done): stowed items in the engine (`MoveItem`, `HeldItem::stowed`), an `inventory` mod owning slots 1 to 4 in place of `loadout`, item properties for slot / start / holster, holster sockets on the mannequin; fixes items dropping on switch and on pick-up.
47. **M47** (done): item properties authored on the `CbItemBody` and baked with the body (the bat's hold time), a Bake button on it, and `pickup.since` (a start tick) in place of a progress field set every tick.
48. **M48** (done): `kBoardSlots` 32 (new reference hashes, protocol 16); the inventory publishes its slots and shows them on a HUD row.
49. **M49** (done): the viewer protocol: `ViewFrame` / `ViewSource` (`src/present/view.h`), `LiveSource` and `ReplaySource` in `src/client`, `CinderboxClient` as a viewer that knows no source, recordings watched in the Godot client (`--replay=FILE`) as the followed player; ROADMAP.md.
50. **M50** (done): frames as bytes: `EncodeView` / `DecodeView` with per-word deltas against a base, view files (`cb_server --record-view`, `ViewFileSource`, `--view=FILE`, `cb_replay view`), measured sizes; `bytes.h` moved to `src/sim`.
51. **M51** (done): two extensions: the viewer (`cinderbox`: no simulation, no networking) and the peer (`cinderbox_peer`: `CinderboxPeer`, the live and replay sources) with packets between them; `set_source( object )`; `cb_sim_data` and `cb_capture` split out so the viewer cannot link a simulation; mod projects get the viewer only.
52. **M52** (done): packs are contained: the scene guard (node class list, no scripts, no connections, contained paths, method list for animations and reactions, advance expressions cleared), run wherever a moddable scene is instantiated; `check_guard.gd`.
53. **M53** (done): smaller frames: compact packets (grid positions and animation values as small deltas, 32-bit rotations, no velocity or inputs), a stride for fewer frames than ticks, entity lists and event rings sent as what changed, `--view-rate` / `--view-compact`, a size breakdown in `cb_replay view`; 32 players from 6.8 to 0.43 Mbit/s.
