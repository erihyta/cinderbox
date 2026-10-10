# Cinderbox — Design

How Cinderbox works today, by subsystem. [README.md](README.md) and [docs/](docs/) are the manual (how to build,
play and make things); [ROADMAP.md](ROADMAP.md) is what comes next;
[docs/HISTORY.md](docs/HISTORY.md) is every milestone so far.

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
| Player movement | a kinematic capsule mover (move-and-slide with Box3D's mover casts and plane solver); a pogo spring keeps it hovering, which carries it over steps. It has a mass (a movement parameter): where it meets a dynamic body the two trade momentum by what they weigh, so a crate is kicked away and a heavy block stops it. In a file of its own (`mover.*`) |
| Motions | what mods add to movement (a dash, a double jump): `CbMotion` nodes baked to text, sent in the schema, compiled against it and run for every player before the mover (`motions.*`). They happen on a press, on a mod event at the player, or hold while their conditions do (which can read the keys held). A motion is a trigger with parts: a **probe** (a ray along the look that has to find a point of the world, of a prop or of a player, and holds on to it after its flight: `MotionHold`, a component), and **effects** on a target (the player, what the probe found, or the entity a field names): an impulse once, a force while it is on (an acceleration, newtons, or toward a speed; ramped in; reacting on the other end), a link (a rope whose tension the two share by weight). The input is what a client already simulates ahead, so a player's own are predicted and rolled back. `MotionState` (a slot per motion: last use, uses, on until) is on players only where a server has motions |
| Movement parameters | walk and sprint speed, acceleration, friction, air control, gravity, jump speed, turn rate, a fall limit, air friction, whether the movement input goes along the ground or the camera, whether the character is in the air whatever is under it, and what it weighs: values, not constants. The server's set is in `SimConfig::move` (its options, then its character's values); a mod's `SetMove` command gives one player its own (`MoveOverrides`, a component only players a mod touched have) |
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
3. Per player: its **motions** (what mods add to movement, by this tick's input), then the mover. Then physics.
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

Mods publish values by name. A `Blackboard` (an int per field the mods declared, per entity) and a global board are
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
- Protocol 37, replay version 20 (`src/net/protocol.h`, `replay.cpp`).

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
- **The codec** (`view_codec.*`, packets "CBV4"): whole frames or deltas against a frame both sides have.

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

- **Commands, not access**: `Set`, `Emit`, `SpawnProp`, `SpawnTemplate`, `Destroy`, `Push`, `Kill`, `Respawn`, `Freeze`, `Aim`, `Facing`, `SetStance`, `SwapLayer`, `SetMove`, and the item commands. Mods need no determinism of their own: their decisions reach clients as values.
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
| `inventory` | the rules of what a player carries: how many slots, what a life starts with, what a death drops (the slots are the engine's) |
| `pickup` | picking up and dropping items, hold-to-use progress on the board |
| `deathmatch` | rounds: scores `combat.killed`, freezes for the intermission, emits `game.round_start` |
| `props`, `expire`, `sneak` | throwing props; items that lie too long; a crouch layer from an animation pack, and its speed |
| `dash` | the charges of a dash; the dash and a double jump themselves are its motion set, run by every simulation |
| `flight` | a player's first tank; flight, the jetpack, its fuel and the glide are its motion set |
| `grapple` | three names; the grappling hook is its motion (a probe, a force and a link), its rope a `CbLinkLook` |

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
- **The editor's node list**: the extension's editor plugin adds the Cinderbox nodes to the Create New Node dialog's Favorites (the dialog's own `favorites.Node` file), once per project and node.
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
| In the world | a body baked from its scene's `CbItem` (the shape under it, mass, properties) into `items/<kind>.cfg`; dropped, picked up, expiring |
| Slots | what a player carries, in numbered places: an item's slot (`HeldItem::slot`), a player's selected one (`Slots`), `SimConfig::slots`. Select, move and drop are intents in `PlayerInput` (kind, two slots, a count), carried out by `Simulation::StepSlots` before anything asks what is held, once per count: predicted and rolled back like movement |
| Using | the use button (`BtnUse`, the engine's, like jump) uses the selected slot's item; a kind whose `use` says so is used by its slot's key instead, where it is. `StepSlots` records the kind's `<kind>.used` event on that tick where a mod declared it; the item's mod asks `Context::Used` / `Using` on the same tick |
| Properties | named numbers on a kind (`slot`, `holster`, `use`, `pickup.hold_seconds`): how mods agree about items; the engine reads the first three |
| Look | the item's own scene, whose root is its `CbItem`: the baked `items/<kind>.cfg` names the scene, and the game reads every one it finds; the scene's own reactions do the rest |

## Looks

Everything a player sees and hears beyond bodies is data in workshop items: no scripts.

| Node | Does |
|---|---|
| `CbDirector` | the World node: entities with kind, template and state as metadata; runs the reactions under it. Plain Godot: anything can drive it |
| `CbReaction` | on a **cue** (a mod event, a game event) or **while** conditions hold: an animation, a property, a listed method, a scene, a sound, a screen shake or flash, placed by the cue or a node |
| `CbPrediction` | says which cue the server will answer a press with (or a held action, again every `cooldown`: `while_held`); the cue plays at once with the same reactions, and the server's cue then plays only what waited |
| `CbItem` | the root of an item's scene: kind, name, mass, properties, first-person view; where the hands hold it (two markers it names); with the `CollisionShape3D` under it, baked to `items/<kind>.cfg` for the server (body, grip, properties) and the game (scene, name, view) |
| `CbLinkLook` | which scene the line of a motion's probe is drawn as: stretched by the viewer from the player's socket to the line's end, which every frame carries (`FrameEntity::linkEnd`) |
| `CbMotionSet`, `CbMotion`, `CbProbe`, `CbImpulse`, `CbForce`, `CbLink` | not looks: authoring nodes for what a mod adds to movement, one family (`CbMotionPart`; the effects share `CbMotionEffect`), baked to `motions/<set>.cfg` for the simulation (docs/motions.md) |
| `CbFieldLabel`, `CbFieldBinding`, `CbPromptLabel` | HUD from fields, for their subject: the local player, or the entity of the list row they are in |
| `CbList`, `CbKey` | a row per player, item, slot or event that happened, copied from the row designed as its child, filtered and sorted by expressions (a scoreboard is a scene, not a node); a key of the viewer's own that keeps a `ui.` value at 1 while it is on (whoever reads the value shows something), with the cursor free if it says so |
| `CbClick` | what a click on a control does: sets `ui.` values (the viewer's own, known to no simulation) and asks an intent of the player's slots. With slots in the frame (`FrameEntity::slotIndex`, `slotCount`), a list of slots and a free cursor, an inventory screen is a scene |

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
| Unit and scenario tests | `tests/test_main.cpp` (`cb_tests`) | map format, commands, movement parameters, motions, events, state machines, poses, hitboxes, items, the frame codec, snapshots, rollback |
| Network tests | `tests/net_test.cpp` (`cb_net_tests`) | sessions over loopback and a lossy link, mods end to end (combat, pistol, melee, inventory, pickup, deathmatch, sneak), sources |
| Headless Godot checks | `godot/addons/cinderbox_maps/check_*.gd` | reactions, predictions, the scene guard, the pack validator, the track player, the pose winning over Godot animation, retargeting, a character's movement, motion bakes, the menu |
| Tools | `cb_bot`, `cb_netsim`, `cb_replay` | bots (full ones run the real client), a UDP relay that degrades traffic, replay verification and view file summaries |

**CI** (`.github/workflows/determinism.yml`), on every push:

| Build | Compiler | CPU |
|---|---|---|
| windows-clang, windows-gcc, windows-msvc | Clang, MinGW GCC, MSVC | x64 |
| linux-gcc, linux-clang | GCC, Clang | x64 |
| macos-arm64-clang | Apple Clang | ARM64 |

- Each build runs all tests, then compares its per-tick hashes (the reference scenario, then a run with motions) with `tests/reference_hashes.txt` and its pose hash with `tests/reference_anim_hash.txt`.
- **Cross-load**: the six portable snapshots must be byte-identical, and each OS loads each of them with each of its builds and runs on to the reference's final hash.
- Not covered: Linux ARM64, the Godot extensions, a Godot client in a session against a server built by another compiler.

## Source layout

```
cmake/            float flags (Determinism.cmake), pinned dependencies (flecs, Box3D, ENet, ozz)
src/expr/         the one expression language (cb_expr): conditions and values as text, a parser and an evaluator, no dependencies
src/sim/          deterministic simulation shared by server and client, as two libraries: cb_sim_data
                  (what the data means: no world is stepped) and cb_sim (the simulation itself)
  events.h          the rings of recent impacts and mod events (part of the state and of every frame)
  bytes.h           minimal binary reader and writer
  world_lifetime.*  the lock around creating flecs and Box3D worlds
  types.h           inputs, commands, input frames, config
  mod_schema.*      names of the mods' board fields, events and actions (sent on join)
  ragdoll.h         the ragdoll's bodies and joints
  map.*             baked map format (.cbmap): fixed-point collision, templates and spawn data
  reflect.*         the authorable component registry the editor and the baker both read
  components.h      snapshotted ECS components (POD, no padding)
  simulation.*      the engine: level, props, commands, ragdolls, snapshots, hashing
  mover.*           the character mover: walking, jumping, falling, pushing, by input and parameters
  move_params.*     the movement parameters: names, defaults, ranges
  motions.*         what mods add to movement: the baked text, its compiler and the runner
  rollback.*        client prediction + rollback session
  physics_arena.*   Box3D allocator arena (makes the physics state copyable)
  box3d_shim.c      access to Box3D internals (world struct, portable serializer)
  detmath.h         deterministic trig and the yaw convention
  fingerprint.*     build fingerprint checked when a client connects
  anim_controller.* what a state machine reads about how a player moves (speed, legs, aim, the air)
  anim_graph.*      a character's state machine: its text, the compiler and the runner
src/anim/         ozz: the placeholder rig and baked characters (anim_set.*), the pose from a state machine (pose.*), joint names (profile.*)
src/net/          wire protocol, ENet wrapper, network simulator (netsim.*), replay files (replay.*)
src/server/       authoritative GameServer (library), the mod API (mod_api.*) and cb_server
server_mods/      gameplay mods compiled into cb_server: inventory, props, pistol, melee, pickup, deathmatch, ...
godot/characters/ characters shipped with the game (mannequin: source glb, bone map, scene, baked files)
characters/       character items: <name>/client is the item's Godot project, <name>/client_item.cfg its hash
  <mod>/client/     a mod's look as a Godot project, published as a workshop item
  <mod>/client_item.cfg  the published item's SHA-256, which servers announce
src/tools/        cb_netsim, cb_replay, cb_bot
src/client/       GameClient core (no rendering, also "lite" mode), the view sources, the bot brain
  live_source.*     a view source that plays on a server (GameClient on its own thread)
  replay_source.*   a view source that plays a recording (replay_player.* on its own thread)
src/present/      engine-independent presentation, what the Godot extensions draw from
  source_thread.*   what sources with a thread share: the thread, the float environment, the newest frame
  view.h            the viewer protocol: ViewFrame (what a viewer is told), ViewSource (who tells it)
  view_codec.*      a ViewFrame as bytes: whole, or a delta against a frame both sides have
  view_file.*       view files: frames recorded as bytes, and the source that plays them
  frame.h           PresentationFrame: a copy of what the simulation shows at one tick
  capture.*         CaptureFrame: a simulation's state as a frame (its own library, cb_capture)
  mirror.*          presentation flecs world: interpolation, error smoothing, visual and mod events
  fields.*          board fields by name: conditions, values and "{field}" text
  pose_tools.*      ragdoll poses, pose blending
  scripts/          spawn/destroy effects, player pose evaluation, ragdoll poses
src/godot/        the viewer GDExtension (cinderbox): CinderboxClient (draws a view source's frames as prefabs,
                  signals, items; the adapter that drives the World director), CinderboxSkeleton, map and entity authoring nodes,
                  CbItem, HUD labels (cinderbox_hud.*), CbMotion (cinderbox_motion.*). No simulation, no networking
  object_source.*   a source that is a Godot object handing over packets (the peer, or a script)
  peer/             the peer GDExtension (cinderbox_peer): CinderboxPeer, the sources that simulate
  packet_handoff.*  what both hand a viewer: the newest frame as a packet
  cue/            the reaction addon, godot-cpp only: CbDirector, CbReaction, cue paths and conditions
godot/            Godot client project: boot (player mods, pack validator), game (input, camera, HUD, VFX,
                  joining with workshop items), workshop.gd (where items are), prefabs, vfx, ui
  maps/             map scenes and their baked .cbmap files
  assets/sfx/       placeholder sounds (tools/make_sfx.py)
  addons/cinderbox_maps/  editor and dev tooling: the Bake Map button, the headless bakers,
                    the character generators, the headless checks
mods_src/         client mod projects (example_neon)
tests/            determinism, rollback, gameplay, animation and loopback network tests
scripts/          cross-compiler determinism check, stress test
sdk/              the Godot project a mod's look is made from (sdk/README.md)
tools/            sdk.ps1, pack_mod.ps1, publish_mod.ps1, export_client.ps1, bake_map.ps1, make_sfx.py (placeholder sounds)
```

## Known limits

| Area | Limit |
|---|---|
| Prediction | a reaction played on a wrong guess is not taken back; the led body holds what the state says about movement still over the lead; packs' layers are not checked for what they read; a predicted event's clock is one tick ahead on the frame its answer arrives |
| Animation tracks | behind latency a swing is first seen a little way in, and keys before that point do not fire; a state started over by an event (rapid fire) fires its keys again only when the transition has a crossfade (that is how the viewer tells a restart from a rollback); packs' non-bone tracks are not played |
| State machines | no nested machines, OneShot/Add/TimeScale nodes, `travel()`, or crossfade curves |
| Characters | one character per server; the capsule's size is not per character; the scene ships its animations' bone tracks next to the ozz clips |
| Movement | a mod's `SetMove` and `Push` are commands, so they are not predicted (a motion is). The controller is kinematic: its mass decides what contacts, forces and ropes exchange, but a player is not knocked over. An effect on a player with a lower slot is felt on its next tick. A probe's line starts at the same point in every view (not at the eye in first person). One probe per player. A While motion's per-second changes need a Float field and are not clamped (a tank fills to a little over full). Another player's motion is seen when its input arrives. At most 16 motions per server |
| Mods | compiled into the server (no hot-loading); a mod is switched off by a `disabled` file in its folder (compiled in, run only when `--mods` names it), a part of one by a server option its conditions read; events between mods are a tick late; a board has 32 names per scope |
| Private fields | per player, not per entity; not in recordings or view files (they read 0 there); entities cannot be hidden from a client: each simulates the whole world, so there is no fog of war |
| Combat | no teams, no spectators |
| Aiming | no marker when the shot is blocked by something the camera sees past (cover in third person); the first-person camera does not lower when crouching (it follows the mover, and crouching is an animation); the bat's strike is rays in the look direction from the chest, not the bat's path through the pose; bots always use the camera behind |
| First-person body | the same mesh with the torso's bones collapsed, not a separate arms model: a large `view_offset` shows the arms' cut ends; the shadow in first person is of the pinned pose. An item with a `view_camera` floats with no arms at all: it is drawn in the world like everything else, with the game's field of view, so it can pass into a wall it is held against; its own animation there is the item's, not the body's |
| Items | one body shape per item; two kinds sharing a holster socket overlap |
| Packs | the checks do not make Godot's or ozz's parsers safe against malformed files |
| Menu | no server browser; no key rebinding page |
| Large files | `simulation.cpp` is about 2,900 lines (the mover left it in M92); `cinderbox_client.cpp` is in four parts since M117, the largest 1,250 |

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
- **Integration test** (3% loss and 1% duplication each way, RTT about 74 ms): 0% late inputs once settled, with windows of 13–16. Before M5 it was about 80%. (The test steps its four clients on a thread each: one after another they take longer than a tick, since each resimulates its window for every frame, at about 0.6 ms a tick with every mod running.)
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
