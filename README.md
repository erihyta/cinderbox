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
| M11: impacts and footsteps reported by the simulation | done |
| M12: Godot AnimationTree driven by the simulation's animation state | done |
| M13: humanoid-profile bone names and retargeting onto any character | done |
| M14: server gameplay mods (C++), commands in the frame, board and mod events, deterministic ragdolls, data-driven mod presentation, pistol demo | done |
| M15: workshop items (mods' looks, announced by servers, never sent), hardened pack validator, HUD from fields (bars, kill feed, scoreboard), player names, fast clock catch-up | done |
| M16: deathmatch rounds as a server mod, `Freeze` command, mod options (`--mod-option`) | done |
| M17: CI on GitHub Actions: six compilers on Windows, Linux and macOS ARM64 must agree bit for bit | done |
| M18: Box3D snapshots no longer carry stale memory; byte-identical across all builds | done |
| M19: characters as workshop items (baked in the editor: ozz skeleton, clips, hitboxes), `cb_server --character`, hit zones for mods | done |
| M20: one pose for players: aiming is part of the ozz pose (so hitboxes follow the raised arm), Godot animation on players is cosmetic only | done |
| M21: facing modes for mods: freelook (default) or camera-facing, with legs that walk where the body goes | done |
| M22: animation layers and stances chosen by mods (the pistol on the upper body, a new melee bat on the whole body) | done |
| M23: companion tracks: VFX, sounds, lights and props keyed in a character's Godot animations play in step with the ozz pose | done |
| M24: a real default character: the Universal Animation Library's mannequin ships with the game; held items sit the same on every rig | done |
| M25: state machines authored in Godot: a character's AnimationTree is baked and run by the simulation, markers become mod events | done |
| M26: strafing with real clips: 2D blend spaces, body-frame velocity, per-character leg turning; a local-only character from the paid animation pack | done |
| M27: the chest faces the camera while strafing (`face_forward`), though the strafe clips turn the torso | done |
| M28: the bat burns while it swings, as the melee mod's own look (not the character's) | done |

## Building

Requirements: CMake 3.24+, Ninja, and one of Clang, GCC or MSVC. Dependencies are downloaded at
configure time and pinned in `cmake/Dependencies.cmake`.

Build output goes outside the source tree (`%LOCALAPPDATA%\cinderbox-build\<project folder>\<preset>` on
Windows, `~/.cache/cinderbox-build/<project folder>/<preset>` elsewhere), which keeps it out of OneDrive.
The project folder's name is part of the path, so two checkouts never build into each other.

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
- `--name=NAME`: your name (otherwise the one saved from the name field, which Esc shows).
- `--workshop=DIR`: where subscribed workshop items are (default `user://workshop`).
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
`--prop-lifetime`, `--props-per-player`, `--props-global`, and `--mods A,B` / `--mods none` / `--list-mods`
(every compiled server mod runs by default, see [Server mods](#server-mods)). The `cb_client` options are `--rollback TICKS` (fixes the prediction window, which is otherwise chosen from latency),
`--width`, `--height`, and `--autoplay SECONDS [--screenshot FILE]` for an unattended smoke test.

## Server mods

The game's rules live in C++ mods compiled into `cb_server` (`server_mods/<name>/<name>.cpp`). They run
**only on the server**. Clients never run gameplay code and never learn what a pistol is.

| What a mod does | How |
|---|---|
| Reads the world | the state before this tick: players, positions, board values, ray casts |
| Reads input | this tick's inputs; `Pressed()` / `Held()` on actions it declared |
| Changes the world | **commands** added to the tick's authoritative frame |
| Keeps its own state | a flecs world shared by all mods (never rolled back, never sent) |
| Talks to presentation | board fields and mod events, by name |

Every client applies the frame's commands exactly like inputs, so the world stays deterministic
and mods need no determinism of their own.

| Command | Effect |
|---|---|
| `Set` | writes a board field of an entity, or of the game (global) |
| `Emit` | announces a mod event (`"pistol.fired"`): two entities, a value, a point, a vector |
| `SpawnProp` / `SpawnTemplate` | creates a prop, owned by a player or the level |
| `Destroy` | removes an entity (never a player) |
| `Push` | an impulse or a velocity change; knockback for characters |
| `Kill` | a player dies, optionally leaving a ragdoll (lifetime and cap chosen by the mod) |
| `Respawn` / `RespawnAt` | brings a dead player back |
| `Freeze` | stops a player moving and acting (it still looks around), or releases it |
| `Aim` | turns the player's aim chain (its character's arm, by default) toward where it looks, or lets it go |
| `Facing` | the body faces where the camera looks, or turns toward where it walks (freelook, the default) |
| `Stance` | plays a stance on one of the player's animation layers, or clears it |

```cpp
// server_mods/jumper/jumper.cpp: a jump boost on Q, the whole mod.
class JumperMod final : public cb::mods::ServerMod {
	cb::mods::ActionHandle m_boost;
	cb::mods::EventHandle m_boosted;
public:
	const char* Name() const override { return "jumper"; }
	void Declare( cb::mods::Declarations& d ) override {
		m_boost = d.Action( "boost", "Q" );       // clients bind Q to it
		m_boosted = d.Event( "jumper.boosted" );  // bindings can play something on it
	}
	void Tick( cb::mods::Context& ctx ) override {
		for ( int i = 0; i < cb::kMaxPlayers; ++i )
			if ( ctx.InWorld( i ) && ctx.Pressed( i, m_boost ) ) {
				ctx.Push( cb::SlotTarget( i ), {}, { 0, 9, 0 }, cb::ImpulseVelocity );
				ctx.Emit( m_boosted, cb::SlotTarget( i ) );
			}
	}
};
std::unique_ptr<cb::mods::ServerMod> CreateMod_jumper() { return std::make_unique<JumperMod>(); }
```

Add the folder, re-run CMake, and the mod is in `cb_server --list-mods`. The server sends every client
a **schema** on join: field names and types, event names, and action names with suggested keys. The
Godot client binds those keys (InputMap actions `cb_<name>`), and bindings refer to fields and events
by name.

The mods that ship:

| Mod | Declares | Rules |
|---|---|---|
| `loadout` | `loadout.slot`; actions `slot_1` (1), `slot_2` (2), `slot_3` (3) | 1 is empty hands (and freelook), 2 the pistol, 3 the bat |
| `melee` | layer `full`, stances `melee`, `melee_swing`; events `melee.swing`, `melee.hit`, `combat.damage` | the bat: a full-body stance while it is out; left mouse swings (0.45 s, every 0.6 s), a fan of 1.8 m rays from the chest at the strike, 40 damage through `combat.damage` |
| `props` | action `spawn_prop` (F) | F with empty hands throws a prop (the map's spawnable template, or a random box or sphere) |
| `pistol` | `combat.*`, `pistol.*` fields; `fire` (left mouse), `reload` (R); events `pistol.fired`, `pistol.hit`, `pistol.reload`, `pistol.dry`, `combat.killed` | hitscan from the camera pivot, 25 damage, 12 rounds, 1.5 s reload; the `pistol` stance on the `upper` layer while it is out; keeps health, so it also applies other mods' `combat.damage`; death leaves a ragdoll (10 s, at most 16); respawn after 3 s; falling out of the world counts as a death |
| `deathmatch` | `deathmatch.score` per player; `deathmatch.phase`, `.seconds`, `.round`, `.winner`, `.kill_limit` for the game; events `deathmatch.round_end`, `game.round_start` | rounds: first to 10 kills, or the best score after 300 s; falling costs a point; everyone is frozen for a 6 s intermission, then the world is cleared, everyone respawns and scores reset |

Mods cooperate through the board: `props` and `pistol` read the `loadout.slot` that `loadout`
publishes. They also cooperate through events: `deathmatch` scores the pistol's `combat.killed`, and
the pistol refills health and ammo on `game.round_start`.

Server operators tune mods with `--mod-option NAME=VALUE` (repeatable); a mod reads them with
`ctx.Option( "deathmatch.kills", 10 )`.

| Option | Default |
|---|---|
| `deathmatch.kills` | 10 kills to win a round |
| `deathmatch.round_seconds` | 300 |
| `deathmatch.pause_seconds` | 6 (the intermission) |
| `deathmatch.fall_penalty` | 1 point lost for falling out of the world |
| `pistol.zone.<zone>` | damage multiplier for a hit zone of the server's character: `head` 2, anything else 1 |

```bash
cb_server --port 7777 --mod-option deathmatch.kills=5 --mod-option deathmatch.round_seconds=120
```

### Workshop items

A mod's **look** (bindings, HUD, meshes, sounds) is a workshop item, like a Steam Workshop or
Counter-Strike mod. **Game servers never send it**: a server only announces which item each of its
mods needs, and players must already have that exact item.

| Piece | Where |
|---|---|
| The item's source | `server_mods/<mod>/client/`, a Godot project next to the mod's code, with a "Mod" export preset |
| Publishing | `tools\publish_mod.ps1 -Mod <mod>`: packs it, names it by its SHA-256, installs it into the local workshop, writes `server_mods/<mod>/client_item.cfg` (commit it) |
| The workshop | for now a folder: `user://workshop/<mod>/<sha256>.zip` (`%APPDATA%\Godot\app_userdata\Cinderbox\workshop`); `godot/workshop.gd` is the one place a real workshop would plug in |
| What the server announces | the hash from `bin/items/<mod>.item` (the build copies `client_item.cfg` there; `--items DIR` to use another folder) |

Joining a server:
1. The server's schema lists the items its mods need.
2. Anything missing or different: the client leaves and says exactly which items to get.
3. Otherwise items load after the base game and **before your own mods**, which load again last, so you
   can still restyle what an item ships. Their `ui/hud_<mod>.tscn` is laid over the game's HUD.

After changing a mod's client project, publish it again and rebuild: the new hash is what servers
announce, and older copies stay in the workshop for servers that still announce them.

## Client mods

Client mods are cosmetic Godot resource packs (`.zip`). A mod can replace or add:
- entity visuals in `prefabs/`;
- effects in `vfx/`, and effect bindings as `vfx/bindings_<name>.tres`;
- sounds and other shared files in `assets/`;
- the HUD in `ui/`;
- map visuals in `maps/` (the scene named after the map the server runs);

Client mods and workshop items cannot contain code. Every pack is checked against an allowlist
before it loads: only known kinds of files in these folders (and Godot's converted copies of them),
redirects that stay inside the pack, no compressed resources, and no resource that names a script
type or a script file. `godot/addons/cinderbox_maps/check_mod_validator.gd` checks real packs and a
set of hostile ones. Gameplay stays in the simulation and in the server's mods, so a pack cannot
change it. What no validator can promise is that Godot's own parsers are safe against a deliberately
malformed file, so packs are still something to take from people you trust.

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
| `event` | Spawned, Destroying, Jumped, Landed, Footstep, Impact, **Mod event** or **Action** |
| `name` | Mod event or action: which one (`pistol.fired`, `fire`) |
| `conditions` | Board conditions on the subject, all must hold (see below) |
| `subject` | Mod events: entity A (who it is about) or B (the other one). `who`, `kind`, `template`, `conditions` and `bone` are checked on it |
| `value_filter` | Mod events: any, value > 0 (a hit that did damage), or value == 0 |
| `bone` | Play at a joint of the subject's character (`RightHand`, `Head`) |
| `at_end` | Mod events: play at the event's vector (where a shot ended) instead of its point |
| `beam` | Stretch a one-metre scene from where it plays to the event's end, like a tracer |
| `template_name` | Only for entities from this map template; empty matches any |
| `kind` | `any`, `prop`, `player` or `static` |
| `scene` | The effect scene to play |
| `offset` | Moves it relative to the entity |
| `lifetime` | Seconds before it is freed |
| `follow` | Parent it to the entity so it travels with it, instead of staying put |
| `who` | Anyone, only the local player, or only other players |
| `cooldown` | Shortest gap between two plays, so a busy event does not stack twenty sounds |
| `min_strength` | Impacts only: ignore anything approaching slower than this, in m/s |
| `sound` | A `.wav`/`.ogg` played at the event, with `volume_db`, `pitch_scale`, `pitch_jitter`, `bus` and `max_distance` |
| `shake`, `shake_time` | Camera shake for the viewer |
| `flash_color`, `flash_time` | A full-screen flash; the colour's alpha is its strength |

A binding can carry a scene, a sound, a screen effect, or any combination. Screen effects are what
the viewer feels, so they usually go with `who = Local player`.

Footsteps and impacts come from the simulation, not from the renderer guessing:
- A **footstep** is a stride, counted by distance walked, so the rate follows the speed on its own.
- An **impact** is a collision the physics engine reported above 1.5 m/s, carrying where it
  happened, both entities and how fast they were approaching. Two bindings with different
  `min_strength` give a soft hit and a hard one different effects.

Both are part of the simulation's state, so they are identical on every machine, survive rollback,
and a client that skipped frames still sees them.

An **Action** binding plays the moment the local player presses a mod action, before the server
answers. That is where feedback that cannot wait a round trip goes (a muzzle flash); its conditions
say whether the server will accept the press (`pistol.ammo > 0`). Other players' shots arrive as mod
events.

Conditions read the server mods' **board** by name:

| Condition | True when |
|---|---|
| `name` | the field is not zero |
| `!name` | the field is zero |
| `?name` | the server declared the field (its mod is running) |
| `!?name` | the server did not declare it (e.g. hide the pistol's scoreboard when deathmatch shows its own) |
| `name == 2`, `!=`, `>`, `>=`, `<`, `<=` | the comparison holds (`true` / `false` count as 1 / 0) |

A field the server did not declare reads as zero, so bindings for a mod that is not running never
match.

**State bindings** (`CbStateBinding`, the table's `states`) hold while their conditions do:

| Field | Meaning |
|---|---|
| `conditions`, `kind`, `who` | when and for whom |
| `attach_scene`, `attach_bone`, `attach_offset`, `attach_rotation` | a scene kept at a joint (a held item) |
| `tree_parameter` | an AnimationTree parameter set to whether the conditions hold |

The HUD reads the board too, through script-free nodes any HUD scene can use:

| Node | Does |
|---|---|
| `CbFieldLabel` | a Label with a `text_format` (`"AMMO {pistol.ammo} / 12"`), shown while its `conditions` hold |
| `CbFieldBinding` | writes a field into any property of its `target` (default: its parent), `value = field * multiply + add`; with conditions it hides the target while they fail. A `ProgressBar`'s `value` and `max_value`, a panel's `visible`, a colour |
| `CbEventFeed` | a line per mod event, `"{a}  >  {b}"` with player names, fading after `line_seconds` (a kill feed) |
| `CbScoreboard` | players as rows: `cells` like `"{name}"`, `"{combat.kills}"`, sorted by `sort_field`, shown while Tab is held and its `conditions` hold |

In formats, `{field}` is the local player's field, `{name}` a player's name, and `{name:field}` the
name of the player a field points at (`"{name:deathmatch.winner} WINS"`).

The pistol's HUD (`server_mods/pistol/client/ui/hud_pistol.tscn`) is built from these: a health bar
(`ProgressBar` from `combat.health` and `combat.max_health`), ammo, reloading, crosshair, kills and
deaths, the kill feed and the scoreboard. None of it is script, so a client mod can restyle all of it.

Every binding that matches plays, so bindings add to each other. When nothing matches, the older
convention still applies: `res://vfx/<event>.tscn`, one of `prop_spawn`, `prop_destroy`, `jump`
or `land`.

`godot/vfx/bindings.tres` is the game's own set; the pistol's look (predicted shots, tracers, hits,
hurt and death feedback, reload, the held pistol) is `vfx/bindings_pistol.tres` in its
workshop item;
`mods_src/example_neon/vfx/bindings_neon.tres` shows a mod adding three more, including its own sound.
All are edited in the Godot inspector.

The sounds in `godot/assets/sfx/` are placeholders in the same spirit as the procedural rig: short,
synthetic, and meant to be replaced. `tools/make_sfx.py` regenerates them.

## Animations

Players are posed by ozz and drawn as their character (by default the mannequin, see
[Characters](#characters)). With `--character none`, a procedural placeholder rig is drawn as one box
per bone, with the bone names of Godot's `SkeletonProfileHumanoid`: `Hips`, `Spine`,
`Chest`, `UpperChest`, `Neck`, `Head`, `Left/RightShoulder`, `UpperArm`, `LowerArm`, `Hand`,
`UpperLeg`, `LowerLeg`, `Foot`, `Toes`. That is the profile Godot retargets imported characters
onto, so a character imported the normal way can be driven with no mapping of our own. Clips whose
joints still carry Mixamo names are recognised through an alias table.

To add your own clips:

1. Put `idle`, `walk`, `run`, `jump_start`, `fall` and `land` `.glb` files in a folder.
2. Run `tools\convert_animations.ps1 -Source <folder>` (or `tools/convert_animations.sh <folder>`).
3. Preview them with `cb_client --anim-viewer`.
4. For the Godot client, pass `--animations=<build dir or assets/anim>`. Clips are loaded from disk, not from the Godot pack.

### Godot animation on players: cosmetic only

**One rule:** the ozz pose, computed from the simulation's state, is the only thing that places a
player's body. It is the pose the server poses hitboxes with, so a body drawn any other way would
be shot where it is not. Godot animation may add to a player what the pose leaves alone: faces,
fingers, props, materials, effects.

This is enforced, not just advised. A `CinderboxSkeleton` driving a `Skeleton3D` gives it a
`CbPoseModifier` as its first skeleton modifier, which applies the ozz pose again after any
`AnimationPlayer` or `AnimationTree` has run. Modifiers after it (look-at, spring bones) run on top of
the pose; the character bake warns about them and about an `AnimationTree`, naming the bones that
have hitboxes.

To drive cosmetic animation from the game, put a `CinderboxAnimator` in a player prefab next to an
`AnimationTree` and point it at the tree:

| It sets | From |
|---|---|
| the state machine's state | the simulation's mode: locomotion, jump, fall, land |
| the blend position | the smoothed ground speed, in m/s, so the blend points sit at 3.0 and 6.5 |
| the clip time | the simulation's locomotion phase, resynced when the tree drifts past `sync_threshold` |

The phase is shared by walk and run, so feet line up between the two clips and between clients.
Transitions use `travel()`, so the transitions authored in the tree are respected.

`mods_src/example_animtree` is a mod that shows the pattern: the body is the ozz pose (bone boxes
from a `CinderboxSkeleton`), and a state machine over a 1D blend space animates a jetpack whose
flames follow the same states (idle flicker, walk and run by speed, a burst on jump). It is a mod, so
it needs no code.

```sh
# rebuild the example prefab (it is an ordinary scene; edit it in the editor instead if you prefer)
godot --headless --path godot --script res://addons/cinderbox_maps/make_animtree_example.gd -- --out=<abs path>/mods_src/example_animtree/prefabs/player.tscn
# check that the ozz pose wins over Godot animation on a driven skeleton
godot --headless --path godot --script res://addons/cinderbox_maps/check_pose_wins.gd
# check that a prefab's tree follows the simulation, without joining a server
godot --headless --path godot --script res://addons/cinderbox_maps/check_animtree.gd -- mods/example_animtree.zip
```

A prefab may contain a `CinderboxSkeleton`, a `CinderboxAnimator`, or both; whichever it has is
driven. `event_parameters` on the animator maps mod events to tree parameters it fires (set to 1, the
request of an `AnimationNodeOneShot`), e.g. `"pistol.fired": "parameters/shoot/request"` for a recoil
clip.

### Ragdolls

A `Kill` command can leave a ragdoll: eleven Box3D bodies joined by cone-and-twist and hinge joints,
built from a fixed standing pose in the simulation (`src/sim/ragdoll.h`). It is simulation state,
identical on every machine, pushable, shootable, and it piles up with props and other ragdolls.

Clients draw it with the player's own prefab (or `prefabs/ragdoll.tscn` if there is one): every joint
of the skeleton follows the nearest body part, so any character works, and the pose the player was
last drawn in is blended into the ragdoll over 0.15 s, so there is no snap. A prefab animated only by
an AnimationTree needs a `CinderboxSkeleton` for its ragdoll to be posed. The ozz pose is still evaluated for every player even when only the AnimationTree is used,
which costs a little work no one reads.

### Driving an imported character with ozz

Point a `CinderboxSkeleton` at a `Skeleton3D` and it retargets onto it: each bone is rotated
relative to **its own rest**, so the character keeps its proportions, and the hips move by an
amount scaled to its height. Our placeholder rests with its arms down while the humanoid profile
rests in a T-pose, and the difference between those two postures is bridged when the skeleton is
bound, so arms end up down rather than sticking out.

Turn `retarget` off to force every bone to exactly where our rig has it, which only makes sense for
a character built to our proportions.

```sh
# a humanoid with long legs and short arms, posed from the simulation's animation state
godot --headless --path godot --script res://addons/cinderbox_maps/check_retarget.gd
```

[assets/anim/README.md](assets/anim/README.md) has the Mixamo → Blender steps and the `anim.cfg`
reference. `--assets DIR` selects a different asset folder, and `--procedural-anim` forces the placeholder.
These folders are for the raylib viewer and for development; in the game, a character comes from its
workshop item (see [Characters](#characters)).

Client controls:
- WASD moves, Shift sprints and Space jumps: the engine's own controls.
- Everything else comes from the server's mods, bound to the keys they suggest. With the shipped mods:
  1 and 2 switch hands and pistol, the left mouse button fires, R reloads, and F spawns a prop.
- Tab shows the scoreboard, the mouse orbits the camera and the wheel zooms.
- Esc releases the mouse (and shows the name field; Enter rejoins with the new name), F1 toggles the debug HUD.

The HUD shows the predicted and confirmed ticks, round-trip time, clock error, rollbacks, stalls and
checksum results.

Server and clients can be built with different compilers. The server rejects a client whose
simulation fingerprint differs from its own.

## Characters

Everyone on a server plays as one character, chosen by the server:

```bash
cb_server                      # the default: mannequin, shipped with the game
cb_server --character robot    # a workshop item [--workshop DIR]
cb_server --character none     # the procedural placeholder rig
```

| Kind | Lives in | Server reads | Players need |
|---|---|---|---|
| shipped with the game (`mannequin`) | `godot/characters/<name>/` | `bin/characters/<name>/` (the build copies the baked files there) | nothing: it is in the base game |
| workshop item (`robot`) | `characters/<name>/client/` | the item's zip in the workshop | the exact item (by SHA-256) |

### The default character: the mannequin

The [Universal Animation Library](https://quaternius.com) mannequin by Quaternius (CC0), in
`godot/characters/mannequin/`:

- **Source**: `source/UAL1_Standard.glb`, the in-place version (the `_RM` files carry root motion,
  which the simulation does not want: it moves the player itself). Its import retargets the
  UE-style bones (`pelvis`, `spine_01`, `upperarm_r`) onto `SkeletonProfileHumanoid` with
  `source/bone_map.tres`.
- **State machine**: an ordinary `AnimationTree` in `character.tscn` (see
  [State machines](#state-machines)): locomotion, jumps, and an upper-body layer for the pistol
  (with a shot on `pistol.fired`) and the bat (with a swing that strikes on a marker; the bat's
  fire is the melee mod's own look).
- **Hitboxes**: capsules along the spine, arms and legs sized from the bone lengths, a head
  sphere, a hips box.
- **Edit it** in the editor like any scene and press **Bake character**. The generator that made
  it only re-bakes an existing scene; `-- --force` builds it from scratch (and discards edits):

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_mannequin.gd
```

Items held in a hand (the pistol, the bat) use a hand frame that is the same on every rig
(`AnimSet::AttachFrame`), so a binding's `attach_offset` / `attach_rotation` work on the
mannequin, the robot and the placeholder rig alike.

### The paid animation pack (local only)

The library's paid **Source** version has 120 animations, among them jogs in eight directions. It
must never reach GitHub (the repository is public), so neither it nor anything baked from it is
committed:

| Where | What | In git |
|---|---|---|
| `Universal Animation Library[Source]/` | the purchase | ignored |
| `godot/characters/ual_mannequin/` | `source/UAL1.glb`, the scene, the baked clips | ignored |
| `make_mannequin.gd --pack=source` | how to build it | committed (names only) |

To build it: copy `Unreal-Godot/UAL1.glb` to `godot/characters/ual_mannequin/source/`, give its
import the same settings as `mannequin/source/UAL1_Standard.glb.import` (the bone map, and
`Sword_Attack` saved to `res://characters/ual_mannequin/animations/`), then:

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_mannequin.gd -- --pack=source --force
```

Its locomotion is a 2D blend space: idle in the middle, the eight jogs on a circle at the game's
jog speed (3 m/s), a walk inside and the sprint ahead; `turn_legs` is off and `face_forward` on. `cb_server` picks
`ual_mannequin` by default where it is built, and `mannequin` everywhere else; its test
(`ual_mannequin`) skips itself when the character is not there, as on CI.

### Workshop characters

A workshop character is a **workshop item**, like a mod's look: the server announces it by name and SHA-256
and players must already have that exact item (a missing one refuses the join). It is shipped in
Godot's runtime format, and **nothing is imported or converted when a player joins**: the item
already holds what the game needs, baked in the editor.

| In the item, under `characters/<name>/` | Made by | Read by |
|---|---|---|
| `character.tscn`: the model, a `CbCharacter` at the root, a `CinderboxSkeleton`, `CbHitbox` zones | the author | clients (the player prefab) |
| `skeleton.ozz`, `idle.ozz`, `walk.ozz`, `run.ozz`, `jump_start.ozz`, `fall.ozz`, `land.ozz`, `anim.cfg` | the Bake button | clients (poses) and the server (hit tests) |
| `hitboxes.cfg` | the Bake button | the server |

With `--character none`, players use the procedural placeholder rig, which has default zones
(head, torso, arm, leg).

### Making a character

1. Make an item project: `characters/<name>/client/` with `project.godot` and a "Mod" export
   preset (copy `characters/robot/client`). `tools\pack_mod.ps1` copies the Cinderbox extension into
   it; do that once (or copy `godot/cinderbox.gdextension` and `godot/bin`) before opening it in
   the editor.
2. Import the model with Godot's humanoid retargeting: in the import dialog, Skeleton3D → Retarget
   → Bone Map with `SkeletonProfileHumanoid`, and the rest fixer's Apply Node Transforms and
   Normalize Position Tracks. Bones then have profile names (`Hips`, `Head`, `LeftUpperArm`...).
3. Make `res://characters/<name>/character.tscn`:
   - a `CbCharacter` root: `character_name`, the paths to the `Skeleton3D` and the
     `AnimationPlayer`, and which animation plays each clip (idle, walk, run, jump_start, fall,
     land);
   - the imported model under it, with the `Skeleton3D` at the root's origin, unrotated and
     unscaled (the bake checks this);
   - a `CinderboxSkeleton` whose `skeleton_path` points at the `Skeleton3D`, with `retarget` off
     (the baked skeleton is that skeleton) and `draw_bone_boxes` off;
   - `BoneAttachment3D` nodes with `CbHitbox` children: sphere, capsule or box shapes, each with a
     `zone` ("head", "torso", "arm", "leg", or your own);
   - the aim chain on the `CbCharacter`: `aim_chain` (bones with weights, turned in order, e.g.
     `UpperChest:0.3 RightUpperArm:1`) and `aim_tip` (the bone that ends up on the line of sight).
     The default, `RightUpperArm:1` to `RightHand`, points the right arm;
   - the stances it supports: `stance_clips` maps stance clip names (`pistol`, `melee_walk`) to
     animations, and `masks` maps layers to bones (`upper` → `Spine`).
4. Select the `CbCharacter` and press **Bake character** in the inspector. It writes the `.ozz` files,
   `anim.cfg` and `hitboxes.cfg` next to the scene (clips are sampled at `sample_rate`, 30 Hz).
5. List `character.tscn` in the preset's `export_files` and the baked files in its
   `include_filter` (see the robot's preset), then publish:

```powershell
powershell -ExecutionPolicy Bypass -File tools\publish_mod.ps1 -Character <name>
```

`characters/robot` is a complete example: a rigid robot, taller than the built-in rig, generated by
a script so a fresh clone can rebuild it (`make_character_example.gd`), then baked by the same code
as the button:

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_character_example.gd -- --out=<abs path>/characters/robot/client/characters/robot
```

### Hit zones

Gameplay mods cast rays with `ctx.CastRay`. Players are hit by their character's hitboxes, posed
from the simulation's animation state at that tick, and `hit.zone` names the zone. Only the server
does this (mods run there), so hit tests cost clients nothing and never enter the rolled-back
simulation. The pistol multiplies its damage by `pistol.zone.<zone>`.

### Aiming

Aiming is part of the pose. A mod sends `ctx.Aim( player, true )` (the pistol does while it is out),
and the pose turns the character's aim chain toward where the player looks, relative to its body.
Every client draws that and every server hit test uses it, so a raised arm can be hit where it is
seen. Respawning keeps the aim; only the mod lets it go.

### Facing

| Mode | The body | Set by |
|---|---|---|
| Freelook (default) | turns toward where the player walks; the camera looks around freely | nothing |
| Camera-facing | faces where the camera looks, every tick (a shooter's stance) | `ctx.FaceCamera( player, true )` |

The pistol switches to camera-facing while it is out, together with aiming. In camera-facing the
legs still walk where the player goes: the hips turn toward the direction of travel (up to 90
degrees) and the spine turns back, and moving away from the facing plays the walk cycle backwards.
This works with any character's six clips, no strafe clips needed.

### Layers and stances

Mods choose what a player's body plays, by layer:

| Mods declare | Characters provide | Example |
|---|---|---|
| a **layer** (`d.Layer( "upper" )`), applied in declaration order | its bone mask: `mask.upper = Spine` in `anim.cfg`, set on the `CbCharacter`'s `masks` | `full` is every bone; `upper` defaults to the spine up |
| a **stance** (`d.Stance( "pistol" )`) | its clips: `<stance>_<clip>` for any of the six (`melee_walk`) or one looping `<stance>` clip, set on the `CbCharacter`'s `stance_clips` | the pistol's `pistol` loop; the bat's `melee_idle`, `melee_walk`, `melee_run` and `melee_swing` |

`ctx.SetStance( player, layer, stance )` plays it (a default `StanceHandle` clears it). The pose
blends each layer's stance over the ones below by the mask's per-joint weights, fading over 0.2 s;
whatever a stance lacks falls back to the default clips, and a layer the character cannot mask does
nothing (both are reported when the character is loaded). A single-clip stance plays from the moment
it was set, which is how a swing is made. Stances are part of the simulation's animation state, so
everyone draws them and the server's hit tests use them. The built-in rig and the robot ship the
pistol and bat stances.

### Companion tracks

A character's animations are ordinary Godot animations: bone tracks next to any other track. The
bake splits them:

| Tracks | Become | Played by |
|---|---|---|
| bone position / rotation / scale | ozz clips | the pose (drawn by clients, hit-tested by the server) |
| everything else: value (`emitting`, `visible`, `light_energy`, material colours), method, audio, animation | `companion.tres`, same clip names | each client, in step with the pose |

So a flame on the swing is two keys on the swing animation (`Flame:emitting` on at 0.1 s, off at
0.34 s), and a whoosh is an audio key, all in Godot's animation editor. At runtime a
`CbCompanionPlayer` plays, per channel (the base locomotion, each stance layer), the clip the pose is
playing at the time it is at: values land exactly, method and audio keys fire once (a rollback that
replays a moment does not fire it again, a long jump such as a join fires nothing), and a channel
whose clip has no companion tracks returns to the `RESET` animation's values. No scripts: tracks call
built-in methods (`restart`, `play`) or set properties. The robot's bat swing has a fire trail and a
whoosh made this way.

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/check_companion.gd
```

Put the companion's scene nodes (particles, lights, an `AudioStreamPlayer3D`) in the character
scene, and list `companion.tres` and the sounds in the item's export preset.

Companion tracks reach only what is in the character scene. Effects on something a mod hands the
character (a weapon) belong to that mod's look instead: the melee mod publishes `melee.swinging`
while a swing lasts, and its item holds `bat_fire.tscn` in the right hand exactly like the bat
while `loadout.slot == 3` and `melee.swinging`. Any character swinging the bat burns the same.

### State machines

A character's animation logic is authored as a Godot `AnimationTree`, the normal way, and baked.
The simulation runs the baked machine every tick, so the server's hit tests and every screen agree
and rollback replays it exactly; the tree itself never runs in the game.

| In the tree | Baked as |
|---|---|
| root: a state machine, or a blend tree of state machines stacked with `Blend2` nodes | layers (at most 4); a `Blend2`'s filter is the layer's bone mask |
| states: `Animation` nodes, `BlendSpace1D`, `BlendSpace2D` (points are animations, play mode forward or backward) | clip states, blend states (phase-synced, so feet stay in step; 2D blends inside Godot's triangles) |
| transitions: Auto advance, advance condition, advance expression, priority, crossfade, Immediate / At End | the same (Sync switching becomes Immediate; crossfades are linear) |
| markers on animations | the mod event of the same name, from the player, when the clip passes it |
| every other track (particles, sounds, lights) | companion tracks, played by clients in step |

Set it up on the `CbCharacter`:

- `animation_tree_path`: the tree. Its `root_node` should be the model (tracks' paths start there).
- `graph_inputs`: what drives the tree's numbers, by parameter path:
  `"Base/Locomotion/blend_position": "forward_speed"`, `"UpperBlend/blend_amount": "pistol or melee"`.
  A 2D blend space takes two expressions, x then y: `"move_right, move_forward"`.
- `turn_legs`: on (the default), the hips turn toward the direction of travel so a forward walk
  goes sideways. Turn it off for a character with its own directional clips (strafes).
- `face_forward`: off by default. On, the spine is turned back by however much the clips turned the
  hips, so the chest faces where the body faces (strafe clips often turn the torso toward the
  travel); the head keeps looking ahead. The legs still run where they run.

Conditions and expressions read simulation values, never scripts:

| Name | Value |
|---|---|
| `speed`, `forward_speed` | smoothed ground speed (m/s); negative forward_speed while backing up |
| `move_forward`, `move_right` | smoothed ground velocity in the body's frame (m/s): for directional blend spaces |
| `vertical_speed`, `grounded`, `airborne_time`, `jumped` | the body's movement (`jumped`: on the tick of a jump) |
| `aiming`, `backward`, `state_time` | a mod's Aim; walking backwards; seconds in the current state |
| a stance's name (`pistol`, `melee`) | true while any layer has it (mods' `SetStance`) |
| a mod event's name (`pistol.fired`) | true on the tick it is emitted at this player: a trigger |
| a board field's name (`loadout.slot`) | the player's value (or the global one) |

Operators: `and or not && || ! == != < <= > >= + - * /` and parentheses. A name no mod declares
reads as 0 (the server logs it). The bake fails with a reason for anything it cannot run (nested
state machines, other blend nodes, a missing animation).

A mod times its effect by the animation with `ctx.AnimationEmits( event )`: the melee mod hits on
the mannequin's `melee.strike` marker, and on its own timer for characters without one. To edit an
imported animation (add a marker or a track), save it to a file in the import settings (Save to
File, Keep Custom Tracks), as the mannequin does with `Sword_Attack`. Check markers after a
reimport: a reimport in Godot 4.7.1 kept the added track but dropped the marker (4.7.2 kept both).
The mannequin's generator puts its marker back whenever it bakes.

## Testing tools

All tools are in `<build dir>/bin`.

| Tool | What it does |
|---|---|
| `cb_server --map FILE.cbmap` | Runs an authored map instead of the built-in sandbox |
| `cb_server --record FILE` | Records the whole session (input frames plus checksums) |
| `cb_replay info\|verify FILE` | Summarizes a recording, or re-simulates it and checks every checksum |
| `cb_client --replay FILE [--replay-start S]` | Watches a recording |
| `cb_netsim --listen P --target HOST:PORT --latency MS --jitter MS --loss % [--duplicate %]` | UDP relay that degrades traffic (latency is added in each direction) |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_mod_validator.gd -- PACK.zip...` | Checks the pack validator: the named packs pass, built-in hostile packs are refused |
| `cb_bot --port P --count N --full M --duration S [--chaotic] [--shoot] [--melee]` | Headless players; the M "full" bots run prediction and rollback and report its cost; `--chaotic` changes every input every tick; `--shoot` makes full bots take out the pistol and fire at the nearest player; `--melee` makes them close in with the bat and swing |
| `godot --path godot -- --autoplay=S --screenshot=F.png --screenshot-every=S2` | Unattended client; also saves `F_1.png`, `F_2.png`, ... and prints the mod events it saw |
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

### Continuous integration

Every push runs `.github/workflows/determinism.yml` on GitHub Actions:

| Job | Runs |
|---|---|
| `windows-clang`, `windows-gcc`, `windows-msvc` | Windows x64: Clang 20, MinGW GCC 16, MSVC 19.44 |
| `linux-gcc`, `linux-clang` | Ubuntu 24.04 x64: GCC 13, Clang 18 |
| `macos-arm64-clang` | macOS 15 on Apple silicon: Apple Clang 17, ARM64 |
| `cross-load on windows / linux / macos` | after all builds: all hash dumps and portable snapshots are byte-identical, and every build of that OS continues every build's snapshot |

Each build job (`scripts/ci_check.sh <preset> <name> <out-dir>`, also usable locally):

- builds the simulation, server, mods and tests (no Godot, no raylib);
- runs every test (`ctest`, network sessions included);
- compares the per-tick hashes with `tests/reference_hashes.txt` and the pose hash with
  `tests/reference_anim_hash.txt`;
- uploads its hash dump, a portable snapshot and `cb_tests` for the cross-load jobs
  (`scripts/ci_cross.sh`), which also check that all dumps and snapshots are byte-identical.

Box3D is fetched with two local patches in `cmake/patches/` (see DESIGN.md, M18):
`box3d-snapshot-padding.patch` (snapshots carried uninitialized padding bytes and a heap pointer) and
`box3d-neon-minmax.patch` (ARM64 clamps returned -0 where x64 returns +0).

## Layout

```
cmake/            float flags (Determinism.cmake), pinned dependencies (flecs, Box3D, ENet, ozz, raylib)
assets/anim/      your converted animation clips (see its README)
src/sim/          deterministic simulation shared by server and client
  types.h           inputs, commands, input frames, config
  mod_schema.*      names of the mods' board fields, events and actions (sent on join)
  ragdoll.h         the ragdoll's bodies and joints
  map.*             baked map format (.cbmap): fixed-point collision, templates and spawn data
  reflect.*         the authorable component registry the editor and the baker both read
  components.h      snapshotted ECS components (POD, no padding)
  simulation.*      the engine: level, character mover, props, commands, ragdolls, snapshots, hashing
  rollback.*        client prediction + rollback session
  physics_arena.*   Box3D allocator arena (makes the physics state copyable)
  box3d_shim.c      access to Box3D internals (world struct, portable serializer)
  detmath.h         deterministic trig and the yaw convention
  fingerprint.*     build fingerprint checked when a client connects
  anim_controller.* deterministic locomotion state machine (AnimState)
src/anim/         ozz: procedural rig, asset loading (anim_set.*), pose evaluation (pose.*), joint names (profile.*)
src/net/          wire protocol, ENet wrapper, network simulator (netsim.*), replay files (replay.*)
src/server/       authoritative GameServer (library), the mod API (mod_api.*) and cb_server
server_mods/      gameplay mods compiled into cb_server: loadout, props, pistol, deathmatch
godot/characters/ characters shipped with the game (mannequin: source glb, bone map, scene, baked files)
characters/       character items: <name>/client is the item's Godot project, <name>/client_item.cfg its hash
  <mod>/client/     a mod's look as a Godot project, published as a workshop item
  <mod>/client_item.cfg  the published item's SHA-256, which servers announce
src/tools/        cb_netsim, cb_replay, cb_bot
src/client/       GameClient core (no rendering, also "lite" mode), bot brain, and the raylib debug viewer
  app/              raylib rendering, camera, HUD over the presentation mirror
  app/anim_viewer.* offline clip preview (--anim-viewer)
  app/replay_viewer.* recording playback (--replay)
src/present/      engine-independent presentation, shared by Godot and raylib
  frame.*           PresentationFrame: a copy of what the simulation shows at one tick
  mirror.*          presentation flecs world: interpolation, error smoothing, visual and mod events
  fields.*          board fields and conditions by name
  pose_tools.*      ragdoll poses, pose blending
  scripts/          spawn/destroy effects, player pose evaluation, ragdoll poses
src/godot/        GDExtension: CinderboxClient (simulation thread, prefabs, signals, state bindings),
                  CinderboxSkeleton, map and entity authoring nodes, effect and state bindings
                  (cinderbox_effects.*), HUD labels (cinderbox_hud.*)
godot/            Godot client project: boot (player mods, pack validator), game (input, camera, HUD, VFX,
                  joining with workshop items), workshop.gd (where items are), prefabs, vfx, ui
  maps/             map scenes and their baked .cbmap files
  assets/sfx/       placeholder sounds (tools/make_sfx.py)
  addons/cinderbox_maps/  editor and dev tooling: the Bake Map button, the headless baker,
                    the AnimationTree example generator and its check, the pack validator check
mods_src/         client mod projects (example_neon, example_animtree)
tests/            determinism, rollback, gameplay, animation and loopback network tests
scripts/          cross-compiler determinism check, stress test
tools/            animation conversion (convert_animations.*), test glTF generator, pack_mod.ps1, publish_mod.ps1,
                  export_client.ps1, bake_map.ps1, make_sfx.py (placeholder sounds)
```

## Credits

- Default character and its animations: [Universal Animation Library](https://quaternius.com) by
  Quaternius, CC0 (`godot/characters/mannequin/source/LICENSE.txt`).
