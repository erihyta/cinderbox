# Cinderbox

A deterministic multiplayer third-person physics sandbox, built with flecs, Box3D, ENet and
ozz-animation, with a Godot 4 client (rendering, VFX, UI and mods) and a raylib debug viewer. See [DESIGN.md](DESIGN.md) for the architecture and decisions.

## Status

| Milestone | State |
|---|---|
| M1: build, deterministic sim core, snapshots, determinism tests | done |
| M2: ENet server, raylib client, rollback netcode, join/leave/reconnect | done |
| M3: ozz animation, procedural box skeleton, locomotion blend, asset pipeline | done |
| M4: replay tool, network simulator, bots, stress test | done |
| M5: unreliable frame batches, automatic prediction window | done |
| M6: Godot client (GDExtension), prefabs, VFX, HUD, mod packs, Windows export | done |
| M7: maps authored in the Godot editor, baked .cbmap format, map sent on join | done |
| M8: component registry, entity templates authored in the inspector, runtime spawning | done |
| M9: declarative effect bindings, so mods add effects as data | done |
| M10: sounds and screen effects in the same bindings | done |

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
prompt), `unix-clang-release`, `unix-gcc-release`, and `godot-export`.

`clang-release` and `unix-clang-release` also build the Godot extension (`CB_BUILD_GODOT`, via
godot-cpp) into `godot/bin/` as `template_debug`. `godot-export` builds only the `template_release`
extension used by exported games. The other presets skip Godot, so they never overwrite that DLL.

## Playing

The main client is the Godot project in `godot/` (Godot 4.7 or later). The raylib client `cb_client` is kept
as a debug viewer; it runs the same simulation and also plays recordings and previews animations.

```sh
# terminal 1
cb_server --port 7777
# terminal 2, 3, ...: the Godot client (after building clang-release)
godot --path godot -- --host=127.0.0.1 --port=7777
# or the raylib debug viewer
cb_client --host 127.0.0.1 --port 7777
```

Open `godot/` in the Godot editor to edit scenes, then press Play. Godot client options, given after `--`:
- `--host=H`, `--port=P`: the server address.
- `--rollback=N`: fixes the prediction window.
- `--animations=DIR`: a folder of converted clips.
- `--mods=DIR`: an extra mod folder.
- `--autoplay=SECONDS`, `--screenshot=FILE`: an unattended smoke test. The exit code is non-zero on a desync.

### Exporting the Godot client (Windows)

```powershell
cmake --preset godot-export; cmake --build --preset godot-export
powershell -ExecutionPolicy Bypass -File tools\export_client.ps1     # -> dist\Cinderbox\Cinderbox.exe
```

The export needs the Godot 4.7.2 export templates, installed either from the editor or extracted to
`%LOCALAPPDATA%\cinderbox-build\tools\godot\templates`. Packs in `mods\` are copied to `dist\Cinderbox\mods`.

`cb_server` and `cb_client` are in `<build dir>/bin`. The server options are `--tick-rate`, `--seed`, `--substeps`,
`--prop-lifetime`, `--props-per-player` and `--props-global`. The `cb_client` options are `--rollback TICKS` (fixes the prediction window, which is otherwise chosen from latency),
`--width`, `--height`, and `--autoplay SECONDS [--screenshot FILE]` for an unattended smoke test.

## Mods

Mods are cosmetic Godot resource packs (`.zip`). A mod can replace or add:
- entity visuals in `prefabs/`;
- effects in `vfx/`, and effect bindings as `vfx/bindings_<name>.tres`;
- sounds and other shared files in `assets/`;
- the HUD in `ui/`;
- map visuals in `maps/` (the scene named after the map the server runs);

Mods cannot contain code. The game refuses a pack that contains scripts, native libraries or files
outside these folders, and one whose resources reference a script. Gameplay stays in the simulation
and on the server, so a mod cannot change it.

1. Create a Godot project under `mods_src/<name>`. Copy `mods_src/example_neon` as a starting point.
2. Put your files at the same paths the game uses, for example `vfx/prop_spawn.tscn`.
3. List them in the project's "Mod" export preset.
4. Run `tools\pack_mod.ps1 -Project mods_src\<name>` to create `mods\<name>.zip`.

The game loads packs from these places, in this order:
1. `<game folder>/mods`
2. `user://mods`
3. each `--mods=DIR`

Within a folder, packs load in alphabetical order, and a later pack overrides an earlier one.
[mods_src/README.md](mods_src/README.md) lists the files the game looks up.

## Maps

A map is a Godot scene with three marker nodes in it, baked into a `.cbmap` file the server loads.
The markers carry the collision the simulation needs; everything else in the scene is the look.

| Node | What it becomes |
|---|---|
| `CbStatic` | A solid box: floor, wall, ramp, step, platform. `size` is the full size in metres. |
| `CbProp` | A dynamic box or sphere the level starts with. |
| `CbSpawn` | Where players appear. One per map. |
| `CbTemplate` | A named entity built from components (see Entities below). |
| `CbComponent` | One component on a template, with the fields the simulation defines. |
| `CbEntity` | Places a template in the level, or defines one inline from its own components. |

1. Make a scene in `godot/maps/`, for example `arena.tscn` (copy `example_arena.tscn` to start).
2. Place `CbStatic` boxes for the collision, and your own meshes, lights and effects for the look.
3. Press **Bake Map** in the 3D toolbar, or run `tools\bake_map.ps1 -Scene res://maps/arena.tscn`.
4. Run the server on it: `cb_server --map godot\maps\arena.cbmap`.

The server sends the baked map to every client when it joins, so clients always play the server's
level and can never disagree about it. Clients then look for `res://maps/<name>.tscn` to draw it; if
they do not have that scene, they draw the baked collision boxes instead, which is what the
built-in sandbox does.

Baked values, including authored component fields, are rounded to fixed-point (1/1024 m,
1/4096 rad) so a map is identical on every platform, and the order of the nodes in the scene is
the order entities are created in, which is part of the map's identity. See `src/sim/map.h` for
the format and `src/sim/reflect.h` for the component registry.

## Entities and components

An entity is described by attaching components to it in the inspector, the same components the
simulation uses. Add a `CbTemplate`, give it `CbComponent` children, pick a component in each one,
and its fields appear:

| Component | Fields |
|---|---|
| `Shape` | `kind` (Box, Sphere, Capsule), `size`, `radius`, `height` |
| `Body` | `type` (Static, Kinematic, Dynamic), `gravity_scale`, `linear_damping`, `angular_damping` |
| `Material` | `density`, `friction`, `restitution` |
| `Velocity` | `linear`, `angular` the entity starts with |
| `Prop` | `lifetime_seconds` (0 keeps it forever); makes it count against the prop caps |

Those fields come from the simulation's own registry (`src/sim/reflect.h`), so adding a field there
makes it appear in the editor with no Godot-side code. A component an author does not attach is
left at the engine's default, which is also how a map baked before a field existed still loads.

- `visual` on a template names the prefab clients draw for it: `res://prefabs/<visual>.tscn`.
  Without it, the shape's default prefab is used.
- `spawnable` marks the one template the spawn button (F) creates. Anything a player spawns still
  expires and counts against the prop caps, whatever the template says.
- A `CbEntity` with a `template_name` places that template. A `CbEntity` with its own
  `CbComponent` children defines a template just for itself, and identical ones are shared.
- A template with `Body.type = Static` becomes level geometry: it never falls and the kill plane
  ignores it.

Templates are part of the map, so they travel to clients with it and the server can spawn them at
runtime. They are initial values only: a template says what an entity starts as, never how it
behaves. Behaviour stays in the simulation.

## Effects

What plays when is data, not code. A binding says "on this event, for this entity, play this
scene", and the client loads every `res://vfx/bindings*.tres` it can find. A mod adds effects by
shipping a file of its own, so two mods can add effects without fighting over one list.

| Field | Meaning |
|---|---|
| `event` | Spawned, Destroying, Jumped or Landed |
| `template_name` | Only for entities from this map template; empty matches any |
| `kind` | `any`, `prop`, `player` or `static` |
| `scene` | The effect scene to play |
| `offset` | Moves it relative to the entity |
| `lifetime` | Seconds before it is freed |
| `follow` | Parent it to the entity so it travels with it, instead of staying put |
| `who` | Anyone, only the local player, or only other players |
| `cooldown` | Shortest gap between two plays, so a busy event does not stack twenty sounds |
| `sound` | A `.wav`/`.ogg` played at the event, with `volume_db`, `pitch_scale`, `pitch_jitter`, `bus` and `max_distance` |
| `shake`, `shake_time` | Camera shake for the viewer |
| `flash_color`, `flash_time` | A full-screen flash; the colour's alpha is its strength |

A binding can carry a scene, a sound, a screen effect, or any combination. Screen effects are what
the viewer feels, so they usually go with `who = Local player`.

Every binding that matches plays, so bindings add to each other. When nothing matches, the older
convention still applies: `res://vfx/<event>.tscn`, one of `prop_spawn`, `prop_destroy`, `jump`
or `land`.

`godot/vfx/bindings.tres` is the game's own set; `mods_src/example_neon/vfx/bindings_neon.tres`
shows a mod adding three more, including its own sound. Both are edited in the Godot inspector.

The sounds in `godot/assets/sfx/` are placeholders in the same spirit as the procedural rig: short,
synthetic, and meant to be replaced. `tools/make_sfx.py` regenerates them.

## Animations

Players are drawn as one box per bone of an ozz skeleton. Until you add clips, a procedural
placeholder rig with Mixamo joint names is used. To add your own clips:

1. Put `idle`, `walk`, `run`, `jump_start`, `fall` and `land` `.glb` files in a folder.
2. Run `tools\convert_animations.ps1 -Source <folder>` (or `tools/convert_animations.sh <folder>`).
3. Preview them with `cb_client --anim-viewer`.
4. For the Godot client, pass `--animations=<build dir or assets/anim>`. Clips are loaded from disk, not from the Godot pack.

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
| `cb_server --map FILE.cbmap` | Runs an authored map instead of the built-in sandbox |
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
  map.*             baked map format (.cbmap): fixed-point collision, templates and spawn data
  reflect.*         the authorable component registry the editor and the baker both read
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
src/client/       GameClient core (no rendering, also "lite" mode), bot brain, and the raylib debug viewer
  app/              raylib rendering, camera, HUD over the presentation mirror
  app/anim_viewer.* offline clip preview (--anim-viewer)
  app/replay_viewer.* recording playback (--replay)
src/present/      engine-independent presentation, shared by Godot and raylib
  frame.*           PresentationFrame: a copy of what the simulation shows at one tick
  mirror.*          presentation flecs world: interpolation, error smoothing, visual events
  scripts/          spawn/destroy effects, player pose evaluation
src/godot/        GDExtension: CinderboxClient (simulation thread, prefabs, signals), CinderboxSkeleton,
                  map and entity authoring nodes, effect bindings (cinderbox_effects.*)
godot/            Godot client project: boot (mod loader), game (input, camera, HUD, VFX), prefabs, vfx, ui
  maps/             map scenes and their baked .cbmap files
  addons/cinderbox_maps/  editor plugin: the Bake Map button and the headless baker
mods_src/         mod projects (example_neon)
tests/            determinism, rollback, gameplay, animation and loopback network tests
scripts/          cross-compiler determinism check, stress test
tools/            animation conversion (convert_animations.*), test glTF generator, pack_mod.ps1,
                  export_client.ps1, bake_map.ps1, make_sfx.py (placeholder sounds)
```
