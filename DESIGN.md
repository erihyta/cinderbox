# Cinderbox — Design

How Cinderbox works today, by subsystem. [README.md](README.md) is the manual (how to build, play
and make things); [ROADMAP.md](ROADMAP.md) is what comes next. The history is the list of
milestones at the end and the git log.

## The shape of it

A multiplayer third-person physics sandbox. The goals are determinism and a clean architecture;
the look does not matter.

```
inputs ──> server: simulation + mods ──> authoritative frames (inputs + mod commands) ──> every simulation
                                                                                              │
looks (workshop items, data only)  <── viewer (Godot) <── ViewFrame <── a source (live, replay, file)
```

| Principle | What it means here |
|---|---|
| One deterministic simulation | server and simulating clients run the same C++ and exchange inputs and commands, never state |
| Mechanisms in the engine, rules in mods | the engine moves bodies, holds items, runs state machines; what a pistol or a round is lives in a server mod |
| Clients present, they do not decide | a client never runs a rule; it draws what mods publish, with looks made of data |
| Authored in Godot, baked for the simulation | maps, characters, state machines and item bodies are Godot scenes; a bake turns them into small files the server reads |
| A viewer and its sources | the Godot viewer draws frames and does not know where they come from |
| Mods travel like workshop items | servers name items by SHA-256; the game connection never carries mod files |

## Stack

| Concern | Choice |
|---|---|
| Language / build | C++20, CMake + Ninja, Clang primary (MSVC and GCC also supported) |
| Dependencies | CMake FetchContent, pinned to exact commits, built inside each build directory (never shared between compilers) |
| ECS | flecs 4.x |
| Physics | Box3D (erincatto/box3d), single-threaded, cross-platform determinism mode, two local patches (`cmake/patches/`) |
| Animation | ozz-animation 0.17.0, scalar (non-SIMD) build; clips and skeletons baked from Godot scenes |
| Networking | ENet (UDP), dedicated headless server |
| Client | Godot 4.7 through GDExtensions (godot-cpp 4.5 API): a viewer and a peer |
| Game rules | C++ server mods compiled into `cb_server` (`server_mods/`), talking to the world only through commands |
| Looks | Godot scenes and resource packs with no scripts (reactions, predictions, HUD nodes) |
| Build directory | `%LOCALAPPDATA%/cinderbox-build/<project folder>/<preset>` (outside OneDrive, one per checkout) |

| Library | Holds | Links |
|---|---|---|
| `cb_expr` | the expression language: text to a small stack program, and its evaluator | nothing |
| `cb_sim_data` | components, events, the map format, the mod schema, the state machine compiler: what the data means | `cb_expr`; nothing that steps a world |
| `cb_sim` | the simulation and rollback | `cb_sim_data`, flecs, Box3D |
| `cb_anim` | skeletons, clips, the pose, hitboxes, retargeting | `cb_sim_data`, ozz |
| `cb_present`, `cb_capture` | the viewer protocol, the mirror, the frame codec; a simulation's state as a frame | `cb_anim` |
| `cb_net`, `cb_client_core` | protocol and transport; the live and replay sources | ENet |
| `cb_server_core`, `cb_server_mods` | the server, the mod API; the mods | everything above |

## Determinism

- The simulation is bit-exact across Windows x64, Linux x64 and macOS/ARM64, and across MSVC, Clang and GCC.
- IEEE floats only: no fast-math, `-ffp-contract=off`, `/fp:precise`, no x87.
- No libm trig in the simulation. `sin`, `cos` and `atan2` come from Box3D's cross-platform implementations (`detmath.h`). `sqrt` and basic arithmetic are correctly rounded by IEEE.
- No unordered iteration, pointer-keyed ordering, wall-clock time or `rand()`. Randomness is a seeded PRNG in the state.
- Inputs are integers: movement axes as int8, camera yaw and pitch as 16-bit angles, button and action bits.
- Entities are kept in a NetId-sorted list and players are processed in slot order. Flecs iteration order is never used.
- A per-tick state hash (FNV-1a over the canonical ECS image, with physics mirrored into it) backs the tests and runtime desync detection.
- Godot may change the FPU state of its threads: a source's simulation thread resets MXCSR before anything else and computes the build fingerprint there, so a wrong environment is rejected by the server instead of desyncing.

Two Box3D bugs found by the cross-platform checks are patched when it is fetched:

| Patch | Problem |
|---|---|
| `box3d-snapshot-padding.patch` | the snapshot writer copied struct padding and a heap address: joining clients received scraps of server memory, and no two saves were identical |
| `box3d-neon-minmax.patch` | ARM64 clamped to a zero of the other sign than x64 (`vmaxq`/`vminq` against SSE's `maxps`/`minps`) |

A third hole was ours: ozz chooses its scalar math with a definition it sets for its own sources only, and its math is mostly inline. Our code that includes its headers (the leg turn, the aim chain, hitboxes, retargeting) compiled the platform's SIMD versions, SSE on x64 and NEON on ARM64, which do not round alike. `ozz_base` now carries the definition to everything that links it. The pose hash found it once it covered the leg turn and the aim (`cb_tests --anim-hash-parts` says which step differs).

## The simulation

`src/sim`. One class steps the world; everything it knows is plain-data components on entities
addressed by a stable `NetId`.

| Part | How |
|---|---|
| World | static boxes, ramps, steps and platforms from a baked map; dynamic boxes, spheres and capsules |
| Player movement | a kinematic capsule mover (move-and-slide with Box3D's mover casts and plane solver); a pogo spring keeps it hovering, which carries it over steps; it pushes dynamic bodies |
| Controls | WASD relative to the camera, Shift, Space: the engine's. Every other control is an action a mod declares |
| Facing | freelook (the body turns toward where it walks) or camera-facing (`Facing` command); the legs follow the direction of travel either way (`AnimState::legYaw`, backwards past about 100 degrees) |
| Props | a lifetime and caps per player and globally, whoever spawned them |
| Death | a `Kill` command makes a player dead (no input, body disabled, not drawn) until a `Respawn`; it can leave a ragdoll |
| Ragdolls | eleven Box3D bodies from a fixed standing pose with cone-and-twist and hinge joints; no asset data enters the simulation |
| Items | entities with a kind, held in a socket, stowed, or lying in the world with a body |
| Animation | per player, an `AnimState`: how it moves, where it looks, the stances mods set, and where each layer of the character's state machine is |

### A tick

1. Join and leave events.
2. The frame's **commands** (from server mods), in order.
3. Movement and physics.
4. The animation state and the state machine, per player.
5. Events are recorded (impacts, footsteps, mod events, animation markers).

### State and snapshots

| Kind | What | Used for |
|---|---|---|
| Rollback snapshot | the ECS image plus the used part of Box3D's arena and its world struct (`PhysicsArena`, `box3d_shim.c`); raw pointers, same instance only | prediction: one per tick that may still be rolled back |
| Portable snapshot | the canonical ECS image plus Box3D's own world serializer; no pointers | joining, desync recovery; byte-identical across all six CI builds |

- Box3D ids in components are stored with the world slot zeroed; `BodyOf` / `ShapeOf` patch it back.
- A map is part of the state's meaning (later spawns come from it), so it travels with the welcome and in replay headers.

### Events

Events are **state**: small rings in `SimGlobals` with counts that only grow, hashed and rolled
back like everything else. Presentation compares counts, so a renderer that skipped ticks gets one
event per occurrence and a rollback un-counts what did not happen.

| Event | From |
|---|---|
| impacts | Box3D contact hit events above 1.5 m/s, sorted by strength then entities before they are recorded (so nothing depends on the engine's internal order) |
| footsteps | a stride counter on the character: distance walked while grounded |
| mod events | `Emit` commands, and markers a playing clip passes (the mod event of the marker's name) |
| jumped, landed | changes of `AnimState::mode`, seen by the mirror |

### The board

Mods publish values by name. A `Blackboard` (32 int slots per entity) and a global board are
hashed state written by `Set` commands; the **schema** (field names and types, event, action,
layer, stance and item kind names, the character's state machine, the items players need) goes
out in the welcome and in replay headers. The schema is not hashed: different mods on the same
build are fine.

**Private fields** (`BoardScope::Private`) are the exception to "everything is in the simulation":
a simulating client has the whole state, so what one player must not know about another cannot be
state. A private value is kept by the server per player slot (`GameServer::m_privates`), never
becomes a command, and is sent to its owner alone: `MsgPrivateFields` on the reliable channel
after the welcome and whenever a mod changes one. The viewer reads it by name for its own player and 0 for anyone else. It is not hashed,
not rolled back, not recorded, and a state machine cannot read it.

## Netcode

Authoritative server, client rollback (`src/net`, `src/client`).

- **Client**: sends its inputs each tick and simulates the whole world at once, predicting remote players by repeating their last input. When the server's frame for tick N differs from the prediction, it restores the snapshot at N and re-simulates, at most once per rendered frame.
- **Server**: never rolls back. It waits briefly for each input, reuses the last one when late, and broadcasts what it used.
- **Commands are not predicted**: a mod's effect shows when its frame arrives and rollback folds it in, like a remote player's input.
- **Window**: chosen from latency, 8 to 20 ticks (`RTT × rate + 2 × jitter × rate + 4`); beyond it the client waits.
- **Clock**: the client aims to be `rtt/2 + jitter + 2 ticks` ahead. Behind, it runs up to twice as fast (being behind costs input; being ahead only costs latency).
- **Desync**: periodic checksums; a mismatch asks for a new welcome.
- **Disconnects**: a dropped player stays 10 s with zeroed input and gets its slot back with its token.

| Channel | Carries |
|---|---|
| 0, reliable | `Hello` (protocol version, build fingerprint, name, token), `Welcome` (config, slot, map, schema, a portable snapshot), `Reject`, checksums, names, resync requests |
| 1, unreliable | client: the last 12 ticks of input and the tick it has frames up to. Server: every tick, all frames from that tick on, delta-chained; a lost batch costs nothing because the next repeats it |

- Frames encode a mask of players whose input changed, then only the changed fields; commands carry a field mask.
- ENet's throttle is off (it dropped unreliable packets after large reliable transfers, which stalled clients).
- **Replays** (`cb_server --record`): every authoritative frame plus checksums; `cb_replay verify` re-simulates headlessly.
- Protocol 19, replay version 5.

## The viewer protocol

`src/present`. A **viewer** draws frames and never asks where they came from; a **source**
produces them.

```
source  ──ViewFrame──>  viewer        the world at one tick, who is who, how the source is
source  <──input─────   viewer        the local player's input, for sources that play one
source  <──control───   viewer        named commands with a number ("pause" 1, "skip" -5)
```

| Source | Simulates | In |
|---|---|---|
| live (`LiveSource`) | yes: connection, prediction, rollback, on its own thread | peer extension |
| replay (`ReplaySource`) | yes: a recording re-simulated, followed as its player | peer extension |
| view file (`ViewFileSource`) | no: recorded frames played back | viewer extension |
| any object | whatever it likes: `take( whole ) -> PackedByteArray` | a script |

- **Two extensions**: `cinderbox` (the viewer: no simulation, no networking, all a mod's project needs) and `cinderbox_peer` (joins servers, plays recordings). Frames cross between them as bytes.
- **The mirror** (`mirror.*`): a presentation flecs world that interpolates between ticks, fades out rollback corrections, evaluates poses and turns count changes into visual events.
- **The codec** (`view_codec.*`, packets "CBV3"): whole frames or deltas against a frame both sides have.

| Precision | Positions | Rotations | Animation | Not sent |
|---|---|---|---|---|
| exact | floats that changed | floats | words that changed | nothing |
| compact | 1/512 m grid, small deltas | 32 bits (smallest three) | floats on a 1/1024 grid | velocity, inputs, a held item's transform |

- A **stride** sends fewer frames than ticks; lists and event rings are sent as what changed. 32 players went from 6.8 to 0.43 Mbit/s.
- **Every client simulates.** Streaming clients (M54: a client that was sent frames instead of inputs, about ten times the download, with fog of war decided by mods) were removed in M68. What is left of them is the compact codec and the stride, which view files use.

## Server mods

`src/server/mod_api.h`, `server_mods/`. C++ compiled into `cb_server`, running only there.

```
inputs ──> server: mods read the world + this tick's inputs ──> commands in the frame
            every simulation (server and clients) applies them, like inputs
```

- **Commands, not access**: `Set`, `Emit`, `SpawnProp`, `SpawnTemplate`, `Destroy`, `Push`, `Kill`, `Respawn`, `Freeze`, `Aim`, `Facing`, `SetStance`, `SwapLayer`, and the item commands. Mods need no determinism of their own: their decisions reach clients as values.
- **Targets** are NetIds, `SlotTarget( slot )` or `ItemTarget( slot, socket )` (resolved when the command runs).
- **Mods read the state before the tick**, so a hitscan is resolved against the world the shooter's input was predicted in.
- **Hit tests** (`CastRay`) pose each player near the ray from its `AnimState` and test its character's hitboxes; `RayHit::zone` names the zone. Server only: poses never enter the rolled-back simulation.
- **Mods cooperate by name**, never by call: board fields, item properties, and events (heard a tick later through `RecentEvents`).
- **Options**: `--mod-option NAME=VALUE`, read with `ctx.Option`.
- **Mod state** lives in one flecs world shared by the mods; it is not rolled back and not sent.

| Mod | Owns |
|---|---|
| `combat` | health, death, ragdolls, respawning, kills and deaths; hears `combat.damage`, `combat.heal`, `game.round_start`; says `combat.hurt`, `combat.killed`, `combat.respawned` |
| `pistol` | the gun: ammo, reload, hitscan with hit zones, the `mark` ray; a hit on a player is `combat.damage` |
| `melee` | the bat: a swing timed by the character's `melee.strike` marker, damage through `combat.damage` |
| `inventory` | what a player carries: slots, stowing, holsters; one owner of the hand |
| `pickup` | picking up and dropping items, hold-to-use progress on the board |
| `deathmatch` | rounds: scores `combat.killed`, freezes for the intermission, emits `game.round_start` |
| `props`, `expire`, `sneak` | throwing props; items that lie too long; a crouch layer from an animation pack |

## Workshop items and packs

A server mod has two halves: its **rules** (C++ in `cb_server`) and its **look** (a workshop item
players subscribe to). Characters are items too.

```
server_mods/pistol/client/  ──publish_mod.ps1──>  workshop: pistol/<sha256>.zip   (players have it)
                                     └──> client_item.cfg (sha256) ──build──> bin/items/pistol.item
cb_server announces "pistol <sha256>" in the schema; a client without that exact item leaves and says what is missing
```

| Defence | When | What |
|---|---|---|
| The pack validator | before a pack is loaded | an allowlist: known kinds of files in known folders, redirects that stay inside, no scripts, native libraries, nested packs or script types in resources |
| The scene guard (`cue_guard.*`) | before a scene is instantiated | listed node classes only, no scripts, no signal connections, node paths that stay in the scene, animations and reactions that call only listed methods, advance expressions cleared |
| Baked files | at pack time | `pack_mod.ps1` bakes item bodies and characters from the scenes that ship, so an item is never stale |

- **Identity is the content hash**: every player on a server sees the same look; an update is a new item.
- **Load order**: base game, the server's items, then the player's own mods (which keep the last word).
- **Honest limit**: neither check makes Godot's resource parsers or ozz's archive reader safe against deliberately malformed files.

## Characters and animation

One system poses every player: the character's **state machine**.

```
AnimationTree (Godot) ──bake──> graph.cfg + clips (.ozz) ──> the schema ──> every simulation runs it
                                                                                   │ AnimState
                                             ozz pose (clients draw it, the server hit-tests it) <──┘
```

| Step | Where | What |
|---|---|---|
| Author | Godot | a `CbCharacter` scene: the model on a humanoid-profile `Skeleton3D`, an `AnimationPlayer`, an `AnimationTree`, `CbHitbox` shapes, `CbSocket` nodes |
| Bake | saving the scene, the Bake button, packing the item | skeleton and clips sampled to ozz, `anim.cfg`, `graph.cfg` (states, transitions, layers, blend spaces, markers), `hitboxes.cfg`; writes a file only when its bytes change |
| Ship | the item (or the base game, for the mannequin) | the scene and the baked files |
| Run | every simulation | `UpdateAnimGraph`, per player, per layer, after movement; conditions are small stack programs over simulation values |
| Pose | clients and the server's hit tests | `PoseEvaluator`: each layer samples its state's clips and blends them through its mask, then the legs turn, the chest faces forward, the upper body bends with the camera's pitch, the aim chain points |

- **Why the simulation runs it**: which state plays decides the pose, the pose decides the hitboxes, and the server hit-tests those. So the machine runs the same everywhere and rolls back.
- **The placeholder rig** (`--character none`, tests): a procedural skeleton with six procedural clips and a one-layer machine built in, run like any other.
- **What a machine reads**: speed and direction, grounded and air time, `jumped`, aiming, stances by name, mod events (as triggers carrying their value), board fields, item kinds held.
- **Stances** are names: a mod sets one on a layer (`SetStance`), a character's machine reads it in conditions and layer weights. What a stance looks like is the character's.
- **Layers** are the tree's state machines stacked with `Blend2` nodes; a filter is the layer's bone mask. At most four.
- **Animation packs**: a mod's own layers (a crouch, a carry) baked into its item; `SwapLayer` plays a pack's layer instead of the character's of the same name, with clips retargeted to the character by profile bone names (`FitPack`). Items bring layers through wishes the server resolves per player.
- **The viewer's own player is led** (`present/anim_lead.*`): for unanswered predictions the viewer runs the machine's upper layers forward from the server's state, with the predicted stance or event put in at the press. The lead grows with the time since the press, stays when the answer arrives (so nothing jumps) and is given back at 15% of real time. An input the state does not have yet gets a tick of its own, as a command does in the simulation, which makes the led clock exactly the server's later one. The base layer is never led.
- **Looking** is part of the pose: while a character faces the camera (`FaceCamera`), its look chain (spine to head) turns by the camera's pitch, blended in over 0.2 s (`AnimState::look`).
- **Aiming** is part of the pose: the `Aim` command turns the character's aim chain toward where the player looks, drawn by clients and used by hit tests.
- **The ozz pose owns the body**: a `CbPoseModifier` re-applies it after any Godot animation; Godot animation only adds what the pose leaves alone.
- **An animation's other tracks** (particles, sounds, lights) stay in the Godot animation. The client copies them out of the character's `AnimationPlayer` when it is first drawn and a `CbTrackPlayer` plays them at the clip and time the pose is playing: values land exactly, method and audio keys fire once across rollbacks.
- **Retargeting onto imported characters**: rotations relative to each bone's own rest, rest postures bridged (arms-down to T-pose), hips scaled by height.
- **Ragdolls on screen**: any skeleton hangs off the eleven parts; the last pose blends in over 0.15 s.

## Items

| Piece | How |
|---|---|
| Kind | declared by a mod (`ItemKind( "melee.bat" )`); a condition name everywhere (true while held) |
| Held | an entity in a socket of its holder (`SpawnItem`, `HoldItem`); it has its own board and receives events |
| Sockets | `CbSocket` nodes in the character scene, in the item's frame; the client places them from the pose and parents the item as `Item` |
| Stowed | carried without being held, drawn in a holster socket |
| In the world | a body baked from its scene's `CbItemBody` (shape, mass, properties) into `items/<kind>.cfg`; dropped, picked up, expiring |
| Properties | named numbers on a kind (`inventory.slot`, `pickup.hold_seconds`): how mods agree about items |
| Look | a `CbItemLook` (kind -> scene) in the mod's reactions scene; the item's own reactions do the rest |

## Looks

Everything a player sees and hears beyond bodies is data in workshop items: no scripts.

| Node | Does |
|---|---|
| `CbDirector` | the World node: entities with kind, template and state as metadata; runs the reactions under it. Plain Godot: anything can drive it |
| `CbReaction` | on a **cue** (a mod event, a game event) or **while** conditions hold: an animation, a property, a listed method, a scene, a sound, a screen shake or flash, placed by the cue or a node |
| `CbPrediction` | says which cue the server will answer a press with (or a held action, again every `cooldown`: `while_held`); the cue plays at once with the same reactions, and the server's cue then plays only what waited |
| `CbItemLook` | which scene an item kind is drawn as |
| `CbFieldLabel`, `CbFieldBinding`, `CbEventFeed`, `CbScoreboard`, `CbPromptLabel` | HUD from fields and events |

- **Paths**: `^` (my entity), `^^` (its holder), `$at` / `$other` (who a cue names), `$local`, `$world`, `@field` (the entity a field names). No path leaves the World node.
- **Conditions and values**: one expression language (`src/expr`): names, comparisons, arithmetic, `and` / `or` / `not`, `?name`. Parsed once into a stack program with the names kept as text; each reader says what a name is.

| Reader | Names | Runs |
|---|---|---|
| State machine (`sim/anim_graph`) | resolved at bake against the schema to where the value lives | its own copy of the program, with `expr::Apply` for the arithmetic: no name lookups in the simulation |
| HUD nodes (`present/fields`) | the board, private fields, item kinds | `expr::Evaluate`, programs cached by text |
| Reactions, predictions (`godot/cue`) | the subject's state metadata, or another entity's through a path and a colon; `is_local`, `event.*` | `expr::Evaluate` |

- **Values**: `CbReaction.value_expression` sets a property to an expression's value (kept up to date while a While is on), `volume_expression` scales a sound; `CbFieldBinding.field` and `{...}` in a `CbFieldLabel` take expressions. HUD nodes stay their own classes: a HUD scene is not under the World node, so a reaction there has no director.
- **World reactions**: every `res://vfx/reactions*.tscn` is loaded once; files add to each other.
- **Prediction rule**: on a predicted cue a reaction waits for the server when it uses what only the server knows (the cue's point, end, value or other entity) or has `wait_for_server` on. Echoes are matched by name and order within a second.
- **Predicted state**: a prediction may also say what the server's answer changes for the viewer's own player. `changes` (`pistol.ammo -= 1`) are applied to the mirror's copy of the board after each frame's cues were heard, so the server's value and the prediction's are never both counted. A `stance` (or the cue itself, as an event) leads the body: see Characters and animation.
- **Cue Preview**: an editor panel that plays a scene's reactions on a small stage with stand-in players.

## Maps and templates

- A map is a Godot scene with marker nodes (`CbStatic`, `CbProp`, `CbSpawn`, `CbTemplate`, `CbEntity`) **baked** to a `.cbmap`: fixed-point values (1/1024 m, 1/4096 rad), so every platform reads the same floats. Scene-tree order is creation order, and so part of the format.
- The server sends the map in the welcome; clients draw the scene it was baked from (or the baked boxes when they do not have it).
- **The component registry** (`reflect.h`) is one description of what an author may attach to an entity, read by the Godot inspector, the baker and the simulation. Values are identified by name hash; unknown ones are dropped and the rest clamped, so a map cannot push the simulation outside the registry.
- **Templates** live in the map: initial values, never behaviour. A spawnable template is what the spawn action creates; anything a player spawns still expires and counts against the caps.

## The Godot client

`godot/`: the project the viewer runs in. `game.gd` is input, camera, HUD and joining; `boot.tscn`
loads mods; `workshop.gd` is where items are.

- **Threads**: a source simulates on its own thread and publishes frames; the main thread takes the newest and extrapolates the interpolation alpha. Input is latched until the source consumed it.
- **Nodes**: each visual is a prefab instance (checked by the scene guard); players are their character's scene, posed by a `CinderboxSkeleton`.
- **Camera**: third person, collides with the map; Godot yaw = simulation yaw − π.
- **The menu**: join by address, settings, an in-game menu; joining another server restarts the process so mounted packs are clean.
- **Jolt** is enabled for client-only effects; simulation entities never get Godot physics bodies.
- **Export**: `tools/export_client.ps1` with the "Windows" preset.

## Tests and CI

| Kind | Where | Covers |
|---|---|---|
| Unit and scenario tests | `tests/test_main.cpp` (`cb_tests`) | map format, commands, events, state machines, poses, hitboxes, items, the frame codec, snapshots, rollback |
| Network tests | `tests/net_test.cpp` (`cb_net_tests`) | sessions over loopback and a lossy link, mods end to end (combat, pistol, melee, inventory, pickup, deathmatch, sneak), sources |
| Headless Godot checks | `godot/addons/cinderbox_maps/check_*.gd` | reactions, predictions, the scene guard, the pack validator, the track player, the pose winning over Godot animation, retargeting, the menu |
| Tools | `cb_bot`, `cb_netsim`, `cb_replay` | bots (full ones run the real client), a UDP relay that degrades traffic, replay verification and view file summaries |

**CI** (`.github/workflows/determinism.yml`), on every push:

| Build | Compiler | CPU |
|---|---|---|
| windows-clang, windows-gcc, windows-msvc | Clang, MinGW GCC, MSVC | x64 |
| linux-gcc, linux-clang | GCC, Clang | x64 |
| macos-arm64-clang | Apple Clang | ARM64 |

- Each build runs all tests, then compares its per-tick hashes with `tests/reference_hashes.txt` and its pose hash with `tests/reference_anim_hash.txt`.
- **Cross-load**: the six portable snapshots must be byte-identical, and each OS loads each of them with each of its builds and runs on to the reference's final hash.
- Not covered: Linux ARM64, the Godot extensions, a Godot client in a session against a server built by another compiler.

## Known limits

| Area | Limit |
|---|---|
| Prediction | a reaction played on a wrong guess is not taken back; the led body holds what the state says about movement still over the lead; packs' layers are not checked for what they read; a predicted event's clock is one tick ahead on the frame its answer arrives |
| Animation tracks | behind latency a swing is first seen a little way in, and keys before that point do not fire; a state started over by an event (rapid fire) fires its keys again only when the transition has a crossfade (that is how the viewer tells a restart from a rollback); packs' non-bone tracks are not played |
| State machines | no nested machines, OneShot/Add/TimeScale nodes, `travel()`, or crossfade curves |
| Characters | one character per server; capsule size and speeds are not per character; the scene ships its animations' bone tracks next to the ozz clips |
| Mods | compiled into the server (no hot-loading); events between mods are a tick late; a board has 32 names per scope |
| Private fields | per player, not per entity; not in recordings or view files (they read 0 there); entities cannot be hidden from a client: each simulates the whole world, so there is no fog of war |
| Combat | no teams, no spectators |
| Aiming | no marker when the shot is blocked by something the camera sees past (cover in third person); the first-person camera does not lower when crouching (it follows the mover, and crouching is an animation); the bat's strike is rays in the look direction from the chest, not the bat's path through the pose; bots always use the camera behind |
| First-person body | the same mesh with the torso's bones collapsed, not a separate arms model: a large `view_offset` shows the arms' cut ends; the bat's stance is out of view; the shadow in first person is of the pinned pose |
| Items | one body shape per item; two kinds sharing a holster socket overlap |
| Packs | the checks do not make Godot's or ozz's parsers safe against malformed files |
| Menu | no server browser; no key rebinding page |
| Large files | `simulation.cpp` and `cinderbox_client.cpp` are about 2,000 lines each |

## Measurements (Clang Release, 32-thread desktop, everything on one machine)

Netcode numbers from when they were taken (M4, M5); frame sizes are in [The viewer protocol](#the-viewer-protocol).

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

## Milestones
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
35. **M35** (done): world reactions: `vfx/reactions*.tscn` scenes loaded once replace `CbEffect` / `CbEffectTable`; built-in events by name (and `pressed:<action>`, until M56); filters, cooldown, placement, sound and screen effects on `CbReaction`; `CbItemLook` as a node; all bindings converted.
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
54. **M54** (removed in M68): streaming clients: protocol 17 (`Hello.stream`, `StreamWelcome`, `View`, `StreamInput`), frames as acknowledged compact deltas, `ServerMod::Sees` and the `fog` mod, `StreamSource` and the `cinderbox_stream` extension with no simulation, `--stream`.
56. **M56** (done): the viewer predicts: `CbPrediction` (action, cue, conditions, cooldown) and `CbDirector.press`; the server's cue of the same name is the echo and plays only the reactions that waited for it; `pressed:<action>` cues removed; one reaction per cue in the pistol and melee looks. A streaming client predicts the same way: the viewer takes the press, whatever the source. (M55, mods run on clients that simulate, was tried on a branch and dropped for this.)
57. **M57** (done): the bat lights its own flames (a reaction on `melee.swing` instead of a playback key in the character's swing); `CbReaction.wait_for_server`.
58. **M58** (done): the pistol's `mark` action (right mouse): a ray, `pistol.scan` and `pistol.marked`; in the look a predicted click, a beam and a zone that follows the marked player for 2 seconds (README, "Example: a second action"); `net_pistol_mark`.
59. **M59** (done): no companion files: `CbCharacter.build_track_library` (the client copies an animation's non-bone tracks out of the character's `AnimationPlayer`), `CbTrackPlayer` (was `CbCompanionPlayer`), baking on scene save and at pack time, writes only when bytes change.
60. **M60** (done): the raylib client (`cb_client`, its anim and replay viewers) removed; raylib is no longer a dependency.
61. **M61** (done): the `combat` mod: health, death, ragdolls and respawning moved out of the pistol; weapons say `combat.damage`, it answers `combat.hurt` / `combat.killed` / `combat.respawned`; `combat.heal`; five server options; its own look (health bar, kill feed, scoreboard, hurt and death flashes); `net_combat`.
62. **M62** (done): one animation system: the placeholder rig gets a built-in state machine, so every pose comes from one; removed the built-in clip blending and stance clip tables (`EvaluateBuiltIn`, `StanceTable`, the six clip slots), `CinderboxAnimator` and its example mod, the non-tree character bake, the glTF conversion pipeline; `AnimState` loses five fields (protocol 18, replay 5, view packets CBV3); the robot example is an `AnimationTree`.
63. **M63** (done): this document by subsystem instead of by milestone; reference hashes regenerated for the M62 animation state. Found by the new pose hash on macOS: our own code compiled ozz's inline math as platform SIMD; `OZZ_BUILD_SIMD_REF` now reaches every target.
64. **M64** (done): predicted state in the viewer: `CbPrediction.changes` (fields of the viewer's own player) and `stance` / `stance_layer`; `LeadAnimState` runs the character's upper layers ahead for the local player (`present/anim_lead.*`, `AnimGraph::UpperLayersRead`, `PlayerAnim::shown`); `CbDirector.pending_predictions`; the pistol's ammo and recoil and the bat's swing and flames follow the click; `anim_lead` test.
65. **M65** (done): a bat's `melee.hot` is cleared by the item's NetId when its time is up, not through the hand: put away or dropped while hot, it stayed hot. `net_melee` puts it away hot.
66. **M66** (done): private fields: `BoardScope::Private`, kept by the server per player and sent to its owner alone (`MsgPrivateFields`, protocol 19); read by name in looks for the viewer's own player; the `secret` example mod; `net_private_fields`.
67. **M67** (done): one expression language (`cb_expr`): the state machine compiler, `present/fields` and the cue addon parse the same grammar (`and` / `or` / `not`, arithmetic, field against field, `?name`, paths with a colon) instead of three parsers; `value_expression` and `volume_expression` on `CbReaction`, expressions in `CbFieldBinding.field` and `CbFieldLabel`'s `{...}`; the state machine's programs and both reference hashes unchanged.
68. **M68** (done): streaming clients removed: `src/stream`, the `cinderbox_stream` extension, `Hello.stream` / `StreamWelcome` / `View` / `StreamInput` (protocol 20), `--stream` and `--stream-rate`, `ServerMod::Sees`, `present/visibility` and the `fog` mod. Every client simulates; view files keep the compact codec.
69. **M69** (done): the upper body follows the camera: `AnimState::look` (a byte that was reserved: 0 to 255, rising over 0.2 s while `Character::faceCamera`), the pose turns the character's look chain (`anim.cfg` `look`, `CbCharacter.look_chain`) about the side axis by `aimPitch` times it, before the aim chain; hitboxes follow; new reference hashes (the state and the pose hash now cover it).
70. **M70** (done): the bat's strike is pitched with the look (it was a level fan, so looking down hit nothing low); a first-person camera in `game.gd` on the posed head (`CinderboxSkeleton.hidden_bone` shrinks the viewer's own head after the pose is applied; the pose, sockets and hit tests are untouched).
71. **M71** (done): a mod event restarts the state it leads to (`AnimGraphState::restarts`, derived at compile from the event transitions into it): every shot of rapid fire plays its recoil, on the server, in the pose and in the viewer's lead alike.
72. **M72** (done): aiming by two traces: `PlayerInput::view` (the byte that was reserved; protocol 21) says which camera the player looks through; `Context::ViewPosition` is where that camera's line starts (the pivot, a shoulder, or the eye), `HeadPosition` the eye on the posed head (`HitTester::JointPosition`), and `CastAim` traces the line of sight for the target and then the shot from the eye to it. The pistol uses it; `CinderboxClient.get_view_position` places the camera on the same points.
73. **M73** (done): the first-person line of sight starts at `anim::EyeHeight` (the rest pose's head height) above the feet instead of on the posed head, on the server (`ViewPosition`) and in the viewer alike; `CastAim` always traces twice; `CinderboxSkeleton` casts the shadows from a whole copy of the skeleton and its meshes while a bone is hidden; `game.gd` places the camera on `RenderingServer.frame_pre_draw` (it was a frame behind the drawn body); the landing, hurt and shot shakes removed from the looks.
74. **M74** (done): the first-person body, viewer only: `CinderboxClient.first_person` puts the local player's pose from the spine up under the eye (the chest back to its rest orientation about the point between the shoulders, that point to its rest place under the eye, the whole turned by the look about the camera's pivot, the aimed arm put back on the line of sight, the arms moved by `CbItemLook.view_offset`) before it is applied, so sockets and held items follow; `CinderboxSkeleton.first_person_body` collapses the spine at the waist and the chest, neck and head between the shoulders and puts the shoulders back, and casts shadows from a whole copy. `anim::TranslateSubtree`, `RotateSubtreeAbout`.
75. **M75** (done): first person faces the camera: `FacesCamera( character, input )` (a mod's `FaceCamera`, or `PlayerInput::view` first person) decides the facing in the mover and the look blend in the animation controller, so the simulation reads the view byte; reference hashes unchanged (their inputs are third person).
76. **M76** (done): grips: `CbGrip` (a marker in the item's scene, baked as a `grip` line of `items/<kind>.cfg`) becomes `ItemShape::grip` in the schema (protocol 22, replay version 6); the pose ends with `anim::SolveGrip`: the item is in the carrying hand's socket frame, the other arm is bent at the elbow and turned at the shoulder so its wrist is on the grip (reach clamped, the elbow kept on its side), and the hand takes the grip's turn if asked. The server's hit tests (`HitTester`), the mirror (`HandGrips`) and the first-person body pass it. The item's frame is the carrying hand's socket, `AnimSet::HandSocketOf`: the character's `CbSocket` named for the hand, baked as `socket.RightHand` / `socket.LeftHand` in `anim.cfg`, or a built-in palm.
77. **M77** (done): a `CbGrip` for the carrying hand (`hand`): the item's frame is that marker's instead of the scene's origin. Bake only: `CbItemBody::bake` writes the centre and the other hand's grip relative to it; the viewer places an item's scene by the inverse of `CbGrip::CarryFrameUnder` (held, and lying in the world). The simulation, the schema and the pose are unchanged.
78. **M78** (done): `CbGrip.as_animated` (`ItemShape::grip` 3, protocol 23): no place is given; `anim::AsAnimated` reads where the other hand's socket is in the carrying hand's socket frame just before the aim chain moves the carrying arm, and `SolveGrip` puts it back there afterwards (and again in the first-person body, before the arm is aimed a second time). The first-person body's upper half is a second evaluation of the viewer's own state (`m_viewPose`: the base layer at its start state, no leg turn, not aimed; a placed grip solved, hands as animated left alone): its spine subtree replaces the real pose's, is turned as one piece (aiming: the arc that puts the aim joint's line to the tip on the line of sight; otherwise the yaw and what is left of the pitch) and moved so the aim joint (or the point between the shoulders) is at its rest place carried round the camera's pivot. Squaring the chest to its rest, or re-aiming the arm alone on the swaying real pose, pulled the hands apart or let the gun roll 10 degrees with each step.
79. **M79** (done): a newly created item node kept its scene root's saved transform until it first changed sockets (then it was placed by the carrying grip): it is now placed the same way at creation. The carrying grip applies in hand sockets and to the body lying in the world; any other socket (a holster) places the scene from its origin. `CbItemBody` warns when the scene root is moved or turned. The bat's carrying marker is turned a quarter (it is held across the fingers), its body with it.
80. **M80** (done): `ItemShape::turn` (protocol 24, replay version 7): the body's rotation in the frame the item is carried in, baked as a `turn` line. `PutItemInWorld` gives the body `rotation * turn`; the frame capture takes it out again, so presentation still gets the item's (the grip's) frame. The bake no longer asks for the body to be turned as the carrying grip is.
81. **M81** (done): the `rifle` server mod (a copy of the pistol whose trigger is `ctx.Held`: a shot every 6 ticks, one dry click and reload per empty magazine). `CbPrediction.while_held` and `CbDirector.hold( action )`: the viewer calls it every frame an action stays down, and a held prediction is due a cooldown after the last was due (not after the frame that showed it), so its count matches the server's. The rifle's flash and tracer are reactions in its own scene, under its `Muzzle` node. The characters' recoil state is entered on `pistol.fired or rifle.fired`. Not done: a rifle stance (it is held as a pistol, so the other hand is as animated); a replay viewer does not predict held actions (it has no local input).
