# Cinderbox — Design

How Cinderbox works today, by subsystem, and the rules that must always hold.

| Read with | For |
|---|---|
| [README.md](README.md), [docs/](docs/) | the manual: building, playing, making things |
| [ROADMAP.md](ROADMAP.md) | what comes next |
| [docs/HISTORY.md](docs/HISTORY.md) | how each part came to be, milestone by milestone |
| [docs/scaling.md](docs/scaling.md) | every measurement: players, bandwidth, processor time |

## The shape of it

A multiplayer third-person physics sandbox. The goals are determinism and a clean architecture;
the look does not matter.

```
inputs ──> server: simulation + mods ──> authoritative frames (inputs + mod commands) ──> every simulation
           (the hybrid: commands + physics events in the frames, the characters as states)    │
looks (workshop items, no scripts) <── viewer (Godot) <── ViewFrame <── a source (live, replay, file)
```

| Principle | What it means here |
|---|---|
| One deterministic simulation | The server and every client run the same C++. The physics world is never sent as state: a client steps it from everyone's inputs and the mods' commands (the inputs model), or from the server's commands and physics events (the hybrid, the default). In the hybrid the characters are sent as state ([Netcode](#netcode)) |
| Mechanisms in the engine, rules in mods | The engine moves bodies, holds items and runs state machines; what a pistol or a round is lives in a server mod |
| Clients present, they do not decide | A client never runs a rule; it draws what mods publish, with looks made of data |
| Authored in Godot, baked for the simulation | Maps, characters, state machines and item bodies are Godot scenes; a bake turns them into small files the server reads |
| A viewer and its sources | The Godot viewer draws frames and does not know where they come from |
| Mods travel like workshop items | Servers name items by SHA-256; the game connection never carries mod files. Today an item is a Godot pack; decided (2026-11-17): cooked items read by our own reader ([ROADMAP.md](ROADMAP.md#6b-cooked-items)) |
| Nodes never grow with players | Every visual that comes with an entity is drawn through the RenderingServer; nodes are a fixed set (the HUD's frame, menus, the camera) and fixed pools. Decided 2026-11-17: bodies since M260; items, effects, lists and the map are still nodes ([ROADMAP.md](ROADMAP.md#6b-cooked-items)) |

## Stack

| Concern | Choice |
|---|---|
| Language, build | C++20, CMake + Ninja; Clang first, MSVC and GCC supported |
| Dependencies | FetchContent, pinned to exact commits, built inside each build directory (never shared between compilers): `cmake/Dependencies.cmake` |
| ECS | flecs 4.x |
| Physics | Box3D (erincatto/box3d) in its cross-platform determinism mode, stepped on several threads, five local patches (`cmake/patches/`) |
| Animation | ozz-animation 0.17.0, scalar (non-SIMD) build; skeletons and clips baked from Godot scenes |
| Networking | ENet (UDP); a dedicated headless server |
| Pathfinding | Recast & Detour 1.6.0, server only (a navigation mesh built at start; not part of the simulation) |
| Client | Godot 4.7 through two GDExtensions (godot-cpp 4.5 API): a viewer and a peer |
| Game rules | C++ server mods compiled into `cb_server` (`server_mods/`); they change the world only through commands |
| Looks | Godot scenes in resource packs; scripts are refused by two checks (the first tightened in M262: [Workshop items and packs](#workshop-items-and-packs)) |
| Build output | `%LOCALAPPDATA%/cinderbox-build/<project folder>/<preset>`: outside OneDrive, one per checkout |

| Library | Holds | Links |
|---|---|---|
| `cb_expr` | the expression language: text to a small stack program, and its evaluator | nothing |
| `cb_sim_data` | components, events, the map format, the mod schema, the state machine compiler: what the data means | `cb_expr`; nothing that steps a world |
| `cb_sim` | the simulation and the client sessions (rollback, hybrid) | `cb_sim_data`, flecs, Box3D |
| `cb_anim` | skeletons, clips, the pose, hitboxes, retargeting | `cb_sim_data`, ozz |
| `cb_present`, `cb_capture` | the viewer protocol, the mirror, the frame codec; a simulation's state as a frame | `cb_anim` |
| `cb_net`, `cb_client_core` | protocol and transport; the live and replay sources | ENet |
| `cb_server_core`, `cb_server_mods` | the server, the mod API, the navigation mesh; the mods | everything above, Recast & Detour |

## Determinism

**Rules that must always hold:**

- Bit-exact across Windows x64, Linux x64 and macOS ARM64, and across MSVC, Clang and GCC.
- IEEE floats only: no fast-math, `-ffp-contract=off`, `/fp:precise`, no x87.
- No libm trig in the simulation: `sin`, `cos` and `atan2` are Box3D's cross-platform versions
  (`detmath.h`). `sqrt` and basic arithmetic are correctly rounded by IEEE.
- No unordered iteration, pointer-keyed ordering, wall-clock time or `rand()`. Randomness is a
  seeded PRNG kept in the state.
- Inputs are integers: movement axes as int8, camera yaw and pitch as 16-bit angles, button and
  action bits.
- Entities are processed in a NetId-sorted list and players in slot order; flecs iteration order
  is never used.
- A per-tick state hash (FNV-1a over the canonical ECS image, physics mirrored into components)
  backs the tests and runtime desync detection.
- Godot may change a thread's FPU state: a source's simulation thread resets MXCSR first and
  computes the build fingerprint there, so a wrong environment is refused by the server instead of
  desyncing.

**Box3D patches**, applied at fetch (`cmake/patches/`, listed in `cmake/Dependencies.cmake`), each
found by the cross-platform checks or the snapshot comparisons:

| Patch | Problem |
|---|---|
| `box3d-snapshot-padding.patch` | the snapshot writer copied struct padding and a heap address: joining clients got scraps of server memory, and no two saves were identical |
| `box3d-neon-minmax.patch` | ARM64 clamped to the other signed zero than x64 (`vmaxq`/`vminq` against SSE's `maxps`/`minps`) |
| `box3d-snapshot-sections.patch` | a snapshot records where each section starts, so a test can say which part of two snapshots differs (`cbx_SnapSections`) |
| `box3d-snapshot-free-contacts.patch` | a free contact slot kept a freed mesh contact's triangle-cache address (CI's macOS build, M247) |
| `box3d-mesh-cache-zero.patch` | a new mesh contact's triangle cache was zeroed only in its union's first member; the rest held stack bytes (CI's Linux Clang, M250) |

**ozz's scalar math**: ozz picks scalar math with a definition set for its own sources only, and its
math is mostly inline, so our code that includes its headers (leg turn, aim chain, hitboxes,
retargeting) compiled SSE on x64 and NEON on ARM64, which round differently. `ozz_base` now passes
the definition to everything that links it. `cb_tests --anim-hash-parts` says which pose step differs.

## The simulation

`src/sim`. One class, `Simulation`, steps the world; all state is plain-data components on entities
addressed by a stable `NetId`.

| Part | How |
|---|---|
| World | static level pieces of any shape (boxes, hulls, triangle meshes, height fields) from a baked map; dynamic bodies of one or several shapes |
| Player movement (`mover.*`) | a kinematic capsule: move-and-slide with Box3D's mover casts and plane solver, held up by a pogo spring that carries it over steps. The ground is found by a sphere cast down (0.9 of the radius; the normal from a short ray, like Unreal's floor check), so gaps and ledges hold it. It has a mass. It **reads the physics world and never writes it** (M246): solid bodies stop it, a loose body pushes it out by their weight ratio; bodies [pushed by characters](docs/maps.md#who-pushes-what) it walks through (its pusher shoves them) and stands on from above when they are heavy enough. Its capsule touches nothing in the physics step and never sleeps (a sleeping body would reorder Box3D's bodies) |
| Motions (`motions.*`) | what mods add to movement (a dash, a grapple, a slam): the motion blocks of a sheet ([docs/rules-language.md](docs/rules-language.md)), baked to text, sent in the schema, compiled against it, run for every driver before the mover. Started by a press, a mod event at the player, or held while conditions are true. Steps, finds and casts, probes that fly and hold on (`MotionHold`), and effects (impulses, forces, links, moves, launches). They run on input the client already predicts, so a player's own motions are predicted and rolled back. `MotionState` (per motion: last use, uses, on until) exists only on players, only when the server has motions |
| Movement parameters (`move_params.*`) | values, not constants: speeds, acceleration, friction, air control, gravity, jump speed, turn rate, fall limit, air friction, move frame (ground or camera), airborne, mass. The server's set is `SimConfig::move` (its options, then its character's values); a mod's `SetMove` gives one player its own (`MoveOverrides`) |
| Controls | WASD (relative to the camera), Shift and Space are the engine's; every other control is an action a mod declares |
| Facing | freelook (the body turns toward where it walks) or camera-facing (the `Facing` command); the legs follow the travel direction either way (`AnimState::legYaw`, walking backwards past about 100°) |
| Props | a lifetime, and caps per player and globally, whoever spawned them |
| Death | `Kill` makes a player dead (no input, body disabled, not drawn) until `Respawn`; it may leave a ragdoll |
| Ragdolls | eleven Box3D bodies from a fixed standing pose, cone-twist and hinge joints; no asset data enters the simulation |
| Items | entities with a kind: held in a socket, stowed, or lying in the world with a body |
| Animation | per player an `AnimState`: how it moves, where it looks, the stances mods set, and where each layer of its state machine is |

### A tick

1. Join and leave events.
2. The frame's **commands** (from server mods), in order.
3. Every driver's **motions** (by this tick's input), each against the world as the tick found it;
   what they do to other players lands after the targets' own. Then every driver's **mover**. Then
   **physics**. Two passes, so a player's slot changes nothing (M212).
4. Each player's animation state and state machine.
5. Events are recorded (impacts, footsteps, mod events, animation markers).

### State and snapshots

| Kind | What | Used for |
|---|---|---|
| Rollback snapshot | the ECS image, plus the used part of Box3D's arena and its world struct (`PhysicsArena`, `box3d_shim.c`); raw pointers, same instance only | prediction: one per tick that may still be rolled back |
| Portable snapshot | the canonical ECS image plus Box3D's own world serializer; no pointers | joining, desync recovery; byte-identical across all six CI builds |

- Box3D ids in components are stored with the world slot zeroed; `BodyOf` and `ShapeOf` patch it back.
- The map is part of the state's meaning (later spawns come from it), so it travels with the
  welcome and in replay headers.

### Events

Events are **state**: small rings in `SimGlobals` whose counts only grow, hashed and rolled back like
everything else. Presentation compares counts, so a renderer that skipped ticks still gets every
event once, and a rollback un-counts what did not happen.

| Event | From |
|---|---|
| impacts | Box3D contact hit events above 1.5 m/s, sorted by strength, then by entity (no dependence on Box3D's internal order) |
| footsteps | a stride counter per character: distance walked while grounded |
| mod events | `Emit` commands, and markers a playing clip passes (the mod event of the marker's name) |
| jumped, landed | changes of `AnimState::mode`, seen by the mirror |

### The board

Mods publish values by name. A `Blackboard` (an int per declared field, per entity) and a global
board are hashed state written by `Set` commands. The **schema** (field names and types; event,
action, layer, stance and item kind names; the characters' state machines; the workshop items
players need) goes out in the welcome and in replay headers. It is not hashed: different mods on
the same build are fine.

**Private fields** (`BoardScope::Private`) are the exception to "everything is in the simulation":
a simulating client has the whole state, so what one player must not know cannot be state. The
server keeps them per player slot (`GameServer::m_privates`) and sends them to their owner alone
(`MsgPrivateFields`, reliable, after the welcome and on every change). The viewer reads them for its
own player, and 0 for anyone else. Not hashed, not rolled back, not recorded; a state machine cannot
read them.

## Netcode

An authoritative server with two network models, chosen by the server for everyone
(`cb_server --net-model hybrid|inputs`; the welcome and the server info say which). Side by side, with
the hybrid's accepted limitations: [docs/scaling.md](docs/scaling.md#the-two-network-models).

| | Inputs model | Hybrid (the default since 2026-11-01) |
|---|---|---|
| Physics | every client simulates it from everyone's inputs, rolled back | every client steps it once per tick from the server's events, a little in the past; never rolled back |
| Characters | every client simulates all of them, rolled back | the server's: your own predicted, the others drawn from the server's states |
| Client session | `RollbackSession` (`sim/rollback.*`) | `HybridSession` (`sim/hybrid_session.*`) |

### The physics world's inputs (both models)

`physics_inputs.cpp` (M246, M247). Characters read the physics world and never write it, so it is a
function of its inputs alone:

- **Pushers**: a kinematic capsule per character (`CatPusher`), placed every tick from a list of
  places on a 1/1024 m grid that the server makes from its characters near something pushable.
  They shove bodies pushed by characters and ragdolls, and touch nothing else.
- **Physics events** (`PhysicsEvent`): every write to a body that depends on a character (a
  motion's push or break, an item launched or let go, a kill's ragdoll) is queued and applied at
  one of two points in the step (after the characters move; after the items' own motions). Each is
  self-contained, and nothing waits for a later tick (snapshots do not hold the queue). A mod's
  command that carries all its numbers stays a direct command: every simulation has the frame's
  commands.
- **Two Box3D worlds in one arena**: the physics world and the characters' (their capsules, a
  driven prop). A stored body handle says which (`BodyOf`). The mover reads the physics world and
  meets other characters in theirs; queries ask both. Bodies cross only by events (`TakeOver`,
  `Enter`, `Idle`; `Remove`, `Spawn`). A character's body never sleeps.
- **NetIds from two counters**: the physics world's from 1, the characters' from `0x10000000`. A
  simulation without characters still names every physics body as the server does.
- **Following another's physics** (`SetFollowsPhysics`): the simulation takes pushers and events
  from its frame and drops its own. `PhysicsMade()` is what a step made; `PhysicsHash()` hashes the
  physics world without the characters. Test: `net_physics_alone` (a recorded session replayed with
  every player standing still and the server's physics inputs matches the server's physics world
  on every tick).

### The hybrid

| Mechanism | How |
|---|---|
| **Physics from the server's events** | Frame batches carry no inputs: each tick's commands, physics events and pushers' places, and now and then the physics checksum (`PhysicsHash`). `HybridSession` steps its physics a few ticks behind the newest frame (twice the jitter, plus two ticks, plus a packet interval: M256), each tick once, checked by checksum; a mismatch asks for the world again. Its clock follows the server's as the frames' best arrivals show it, one tick per tick, never past the frames it has |
| **Frames carry the physics only** (M256) | Commands a client would skip are left out by the server's own step (`Simulation::FollowerSkipped`), except an item into a container (it uses up a physics NetId there too). Frames go with the states, one datagram every `stateEvery` ticks, each frame in two datagrams. Pushers' places are varint changes against where each would be going on. A state's header holds only the slots' owners and the global board: a client's event rings are its own |
| **The client's physics reads no character** (`Simulation::FollowerSkips`) | Of a tick's commands it runs only what the physics world alone decides (a break, an item into the world, a push on a body, fields, events). A command about a character (a kill, a possession, a pickup, anything aimed at a slot) is skipped: its physics part arrives as self-contained events (`Idle` and `Enter` release the controller too; `Enter` names the prop's owner and end), its character part in the states. The physics world's own rules count only what is in it (prop caps: a driven prop is out). Why: states arrive when the link allows, so a command reading them could diverge (one crashed the client). Tested with characters scrambled before each physics step, states held back, and `cb_replay joins` |
| **Characters are the server's** | Every second tick, the characters' side (every entity not in the physics world; `Simulation::SaveState`, compact) as a delta against the newest state the client acknowledged (`net/state_codec.h`). Other characters are **stand-ins** placed from the newest state not ahead of the physics, so one standing on debris stands on it here too. **A state never touches the physics world** |
| **Sent by priority** (M250, M257; `--every-character` turns it off) | `net::EncodeUpdates`. A client's own character and what it holds go every state. Each other character's priority for that client grows every state by how much it matters (`GameServer::Matters`: near, in its camera's view, just did something, aiming at the client) and resets when sent; the most owed go first, each with what it holds, until the client's budget (`--client-budget`, 256 kbit/s of states after packing) or `stateBudget` entities (512). Records are deltas against what the client acknowledged (or, if that is too old, what it has in its newest state: `m_recordVersions`); what is left out the client keeps; what is gone is named until acknowledged; a record the client already has (by hash) is not sent. Identical messages are shared between clients |
| **Made while the next tick runs** (M258) | `SendStates` reads what the plans need from the simulation; the state threads build every client's plan and message (`MakeStates`) while the main thread runs the next tick; they go out once made (`FinishStates`), with that state's frames, in one datagram. Puppets, views and hashes are made on the state threads too |
| **Events go with their characters** (M257) | A mod event at a character or what it holds (`Simulation::StepEvents`) goes with that character's update, not in the frames, for half a second (`kEventWindow`): a far one may not be heard. A client hears each once (by serial), in a ring of its own (`HybridSession::HeardEvents`). Its own character is sent only what mods said; it predicts the rest. An event declared for everyone (`Declarations::Event`'s `everyone`: a kill feed, a round's end), or at nothing, stays in the frames |
| **Other characters are puppets** (M253, M254; `sim/puppet.h`) | A puppet (`Simulation::PuppetOf`) is a few integers: position, facing, look, flags, and each state machine layer's state and start tick. Every client but the driver's gets it as a delta (`net::EncodePuppet`), plus the record and held items without what the client keeps itself (`Simulation::PuppetView`), and no input. `Simulation::ApplyPuppets` places it (interpolated when sent less often) and runs its animation locally, following the puppet's layers (`UpdateAnimGraph`'s `follow`: states and start times are the server's, clocks and blends local). Its markers arrive as the server's events |
| **Fields others see** (M255) | An entity field belongs to its player alone unless shared (`BoardField::shared`; a field a state machine reads is shared automatically). A puppet's view zeroes the rest, and a `SetField` on a character or what it holds goes in the states, not the frames. Names are sent as changes after the first list |
| **Lag compensation** (M250; `--lag-compensation MS`, 500 by default) | Each input says how many ticks back the player saw the others (`PlayerInput::seenBack`; about 20 at a 100 ms round trip), no further than its round trip explains (M265, `GameServer::SeenBackLimit`: the round trip the server measures and four times its spread, two states, 150 ms; a claim beyond is cut to it). Hits meet the others as that player saw them, and the world as it is. **Mods' casts** (`Context::CastRay`): the server tests hitboxes at the characters' past places and poses (`GameServer::RecordPast`). **Motions' finds and probes**, in the simulation (`SimConfig::compensation`; 0 in the inputs model): the past capsules (`Simulation::RecordPast`, kept in snapshots, not portable images) are tested instead of the present ones (`CastSeen`); a hold attaches where the target is now |
| **Your own character, predicted** (`Simulation::StepCharacters`) | Runs ahead of the server against the physics world as it is (in the past); each state and the inputs since, run again, make it the server's again. While the physics steps a past tick, it stands where the server had it then. Entities a client makes on the characters' side take NetIds from `0x70000000` |
| **Your own throws drawn at once** (M249, `HybridSession::OwnLaunches`) | What your motions launch while predicted is drawn from the press (`Simulation::FlyItem`), never in the physics; the server's copy takes over when it arrives, carried forward to your tick while it flies |

**Rules that must hold in the hybrid:**

- The client's physics reads no character, and a state never touches the physics world.
- Physics events are self-contained; nothing waits for a later tick.
- While the state threads make a state's messages, nothing they read changes: acknowledgements and
  events at characters are held (`m_heldAcks`, `m_heldEvents`); keeping a state, a snapshot, a client
  joining or leaving, or any message but an input waits for `FinishStates`.

The constants and the reasons behind each mechanism are in the code's comments and in
[docs/HISTORY.md](docs/HISTORY.md) (M244 to M261, with the costs measured when it was built); today's
numbers are in [docs/scaling.md](docs/scaling.md).

### The inputs model

- **Client**: sends its inputs every tick and simulates the whole world, predicting remote players
  by repeating their last input. When the server's frame for tick N differs from the prediction, it
  restores the nearest snapshot at or before N and re-simulates, at most once per rendered frame.
- **Snapshots are few**: one before each tick predicted for the first time, and one during each
  re-simulation where the confirmed frames end (where the next misprediction almost always is).
- **`reconcile_every`** (`CinderboxPeer`, `--reconcile-every=N`, default 1): a misprediction is fixed
  only every N ticks; remote players are seen up to N ticks later.
- **Server**: never rolls back. It waits briefly for each input, reuses the last one when late, and
  broadcasts what it used.
- **Commands are not predicted**: a mod's effect shows when its frame arrives, folded in by rollback
  like a remote input.
- **Window**: 8 to 20 ticks, from latency (`RTT × rate + 2 × jitter × rate + 4`); beyond it the
  client waits.
- **Clock**: the client aims to be `rtt/2 + jitter + 2 ticks` ahead; when behind it runs up to twice
  as fast (being behind costs input, being ahead only latency).
- **Desync**: periodic checksums; a mismatch asks for a new welcome.
- **Disconnects**: a dropped player stays 10 s with zeroed input and gets its slot back with its token.

### Threads (both models)

| | How |
|---|---|
| **Physics** (`src/sim/physics_threads.h`) | Every world has 4 workers (`kPhysicsWorkers`); how many threads run them is each machine's choice and changes nothing simulated (default: a quarter of the machine's threads, at most 3 extra; `--physics-threads`). One pool per process; its threads spin a millisecond after the last task. A task allocates inside its world's arena, under a lock; where things land varies, and nothing reads that. Box3D aims to be the same for any worker count but does not promise it: the `physics_threads` test checks 1 to 8 workers on 1 to 8 threads. Cost: the workers' memory is in every snapshot (2.7 to 3.5 MB at 64 players); gain: 10 to 20% off the physics in a game |
| **Characters** (M258, `Simulation::SetParallelFor`; a server gives its own pool, others none) | Movers and the characters' animation run apart, one driver each. A mover reads the world as the tick found it, so each driver's results are kept (`MovedCharacter`) and written after, in slot order. A driven prop moves at its turn on the simulation's thread. The motions' pass stays on one thread. The `character_threads` test: the same hash on 1 to 8 threads and with players taken backwards |
| **State threads** (the hybrid server) | "Made while the next tick runs", above |

### Channels and encoding

| Channel | Carries |
|---|---|
| 0, reliable | `Hello` (protocol version, build fingerprint, name, token), `Welcome` (config, slot, map, schema, a portable snapshot), `Reject`, checksums, names, private fields, typed text, resync requests |
| 1, unreliable | Client: its newest inputs, each in as many packets as the server's count of its lost packets asks (4 at least; always 12 before M261), the tick it has frames up to, and in the hybrid the state it keeps as a base. Server, inputs model: every tick, all frames from the client's acknowledgement on, delta-chained; a lost batch costs nothing because the next repeats it. Hybrid: frames with the states (above) |

- Inputs model frames go out in three packets each (the hybrid's in two, with its states), one per
  update; packets sent in one update share a datagram and are lost together (M142). A client keeps
  a batch that starts after what it has until the gap is filled. The oldest unacknowledged frame is
  re-sent only once it has been out longer than an answer takes.
- Frames encode a mask of the players whose input changed, then only the changed fields; commands
  carry a field mask.
- ENet's throttle is off (it dropped unreliable packets after large reliable transfers, stalling
  clients).
- **Replays** (`cb_server --record`): every authoritative frame plus checksums; `cb_replay verify`
  re-simulates headlessly.
- **Versions**: `kProtocolVersion` in `src/net/protocol.h`, `kVersion` in `src/net/replay.cpp` and
  `src/present/view_file.cpp`. The server refuses any other protocol version and any other build
  fingerprint. The code is the place to read the numbers; [docs/versions.md](docs/versions.md) says
  what each version changed (a row added with each raise).

## The viewer protocol

`src/present`. A **viewer** draws frames and never asks where they come from; a **source** produces
them.

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

- **Two extensions**: `cinderbox` (the viewer: no simulation, no networking; all a mod's project
  needs) and `cinderbox_peer` (joins servers, plays recordings). Frames cross between them as bytes.
- **The mirror** (`mirror.*`): a presentation flecs world that interpolates between ticks, fades out
  rollback corrections, evaluates poses and turns count changes into visual events.
- **Poses by distance and view** (M259; `EvaluatePlayerPose`, `Mirror::SetView`): in view, every
  frame within 15 m, every 2nd within 30, 4th within 60, 8th beyond (distance scaled by zoom); out
  of view, every 4th within 15 m (its shadow may show), else never; staggered by NetId. The viewer's
  own every frame. A pose not remade is not reapplied (`PlayerAnim::posed`); its catch-up covers the
  time since its last.
- **The codec** (`view_codec.*`, packets `CBV7`): whole frames, or deltas against a frame both sides
  have. A **stride** sends fewer frames than ticks; lists and event rings are sent as changes. 32
  players went from 6.8 to 0.43 Mbit/s.

| Precision | Positions | Rotations | Animation | Not sent |
|---|---|---|---|---|
| exact | floats that changed | floats | words that changed | nothing |
| compact | 1/512 m grid, small deltas | 32 bits (smallest three) | floats on a 1/1024 grid | velocity, inputs, a held item's transform |

- **Every client simulates**; none is sent frames to draw. In the inputs model a client simulates the
  whole world; in the hybrid, the physics world and its own character. (Streaming clients, sent
  frames instead of inputs, were M54 and removed in M68; the compact codec and the stride remain,
  used by view files.)

## Server mods

`src/server/mod_api.h`, `server_mods/`. C++ compiled into `cb_server`, running only there.

```
inputs ──> server: mods read the world + this tick's inputs ──> commands in the frame
            every simulation (server and clients) applies them, like inputs
```

- **Commands, not access**: `Set`, `Emit`, `SpawnProp`, `SpawnTemplate`, `Destroy`, `Push`, `Kill`,
  `Respawn`, `Freeze`, `Aim`, `Facing`, `SetStance`, `SwapLayer`, `SetMove`, and the item commands.
  Mods need no determinism of their own: their decisions reach clients as values.
- **Targets** are NetIds, `SlotTarget( slot )` or `ItemTarget( slot, socket )`, resolved when the
  command runs.
- **Mods read the state before the tick**, so a hitscan is resolved against the world the shooter's
  input was predicted in.
- **A frame holds 1024 commands** (`kMaxCommandsPerFrame`); more wait for the next tick, first and in
  order, and the server logs it.
- **Hit tests** (`CastRay`) pose each player near the ray from its `AnimState` and test its
  character's hitboxes; `RayHit::zone` names the zone. Server only: poses never enter the
  rolled-back simulation.
- **Mods cooperate by name, never by call**: board fields, item properties, and events (heard a tick
  later through `RecentEvents`).
- **Options**: `--mod-option NAME=VALUE`, read with `ctx.Option`.
- **Mod state** lives in one flecs world the mods share; not rolled back, not sent.

| Mod | Owns |
|---|---|
| `combat` | health, death, ragdolls, respawning, kills and deaths; hears `combat.damage`, `combat.heal`, `game.round_start`; says `combat.hurt`, `combat.killed`, `combat.respawned` |
| `gun` | the pistol and the rifle: ammo, reload, hitscan with hit zones; a hit on a player is `combat.damage` (`mark`: the ray a second use marks with) |
| `melee` | the bat: a swing timed by the character's `melee.strike` marker, damage through `combat.damage` |
| `inventory` | what a player carries: how many slots, what a life starts with, what a death drops (the slots are the engine's) |
| `pickup` | picking up and dropping items, hold-to-use progress on the board |
| `deathmatch` | rounds: scores `combat.killed`, freezes for the intermission, emits `game.round_start` |
| `props`, `expire`, `sneak` | throwing props; removing items left lying; a crouch layer from an animation pack, and its speed |
| `dash` | the dash's charges; the dash and the double jump are its motion set |
| `flight` | a player's first tank; flight, the jetpack, its fuel and the glide are its motion set |
| `grapple` | three names; the hook is its motion (a probe, a force, a link), its rope a `CbLinkLook` |
| `brawl` | the brawler's kit: a fist and four skills whose sheets are their moves; turns their hits into `combat.damage` (a guard takes a quarter) |
| `frag`, `grenade` | the fused grenade (a stack of three, set off by its sheet); the impact grenade (off) |
| `chat`, `shop`, `chest` | typed text; buying with an intent; containers placed around the spawn |
| `waves`, `possess`, `cannon` | NPCs in waves on the navigation mesh; taking over the nearest body; a cannon whose balls break walls (all off) |
| `item`, `secret` | the cues an item's use says, whatever the item; a private field (the example) |

Every mod, its names and options: [docs/server-mods.md](docs/server-mods.md).

## Workshop items and packs

A server mod has two halves: its **rules** (C++ in `cb_server`) and its **look** (a workshop item
players subscribe to). Characters are items too.

```
server_mods/gun/client/  ──publish_mod.ps1──>  workshop: gun/<sha256>.zip   (players have it)
                                     └──> client_item.cfg (sha256) ──build──> bin/items/gun.item
cb_server announces "gun <sha256>" in the schema; a client without that exact item leaves and says what is missing
```

| Defence | When | What |
|---|---|---|
| The pack validator | before a pack is loaded | an allowlist: known kinds of files in known folders; binary resources only (no text resource: Godot decodes escapes in them); redirects with no escape and no object, each to a converted file of the pack named for its own file; every converted file named by one; no scripts, native libraries, nested packs, script types or script classes in resources; a resource's external resources plain files in the moddable folders, by path and by id |
| The scene guard (`cue_guard.*`) | before a scene is instantiated | listed node classes only, no scripts, no signal connections, node paths that stay in the scene, animations and reactions that call only listed methods, advance expressions cleared |
| Baked files | at pack time | `pack_mod.ps1` bakes item bodies and characters from the scenes that ship, so an item is never stale |

- **Identity is the content hash**: every player on a server sees the same look; an update is a new
  item.
- **Load order**: base game, the server's items, then the player's own mods (which have the last word).
  Every pack is loaded with `replace_files`: an item may replace the base game's files in the moddable
  folders, and another item's.
- **The validator decides before anything loads**: the scene guard comes after the load
  (`game.gd`'s HUD, `presentation.gd`'s reactions, `cinderbox_client.cpp`'s prefabs all load first),
  and Godot runs a built-in script inside `ResourceLoader.load`. Until M262 an escaped `GDScript`
  type in a text resource passed the validator's search of the bytes (found in M261.9), an escaped
  `res://` passed its redirect check, and any file under `.godot/exported` and `.godot/imported` was
  accepted. Since M262 it refuses text resources and escapes, reads a binary resource's external
  resources (Godot follows an id before a path), and ties each converted file to the redirect of the
  file it is a copy of (named by the MD5 of that file's path), so a pack replaces only what it may
  replace anyway. What no byte check can promise is that Godot's parsers are safe against a
  malformed binary file: cooked items, which no Godot loader reads, end that
  ([ROADMAP.md](ROADMAP.md#6b-cooked-items)).
- **What the guard does hold**: a scene with no script still cannot act (no `HTTPRequest`, `Timer`
  or wired signal is instantiated): every scene the game shows is instantiated through it
  (`cue::Instantiate`). The editor's item preview instantiates the mod's own reactions without it.
- **Honest limit**: neither check makes Godot's resource parsers or ozz's archive reader safe
  against deliberately malformed files; nor is what a client reads from a server checked that way
  yet ([ROADMAP.md](ROADMAP.md#before-launch)).

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
| Bake | on save, the Bake button, when packing | skeleton and clips sampled to ozz, `anim.cfg`, `graph.cfg` (states, transitions, layers, blend spaces, markers), `hitboxes.cfg`; a file is written only when its bytes change |
| Ship | the item (or the base game, for the mannequin) | the scene and the baked files |
| Run | every simulation | `UpdateAnimGraph`, per player, per layer, after movement; conditions are small stack programs over simulation values |
| Pose | clients and the server's hit tests | `PoseEvaluator`: each layer samples its state's clips and blends through its mask; then the legs turn, the chest faces forward, the upper body bends with the camera's pitch, the aim chain points |

- **Why the simulation runs it**: the playing state decides the pose, the pose decides the hitboxes,
  and the server hit-tests those. So the machine runs the same everywhere and rolls back.
- **The ozz pose owns the body**: what the game draws is exactly the pose hit tests use; since M260
  nothing of a character's scene animates a body in the game. In the editor, a `CbPoseModifier`
  re-applies the pose after any Godot animation (`CinderboxSkeleton`).
- **The placeholder rig** (`--character none`, tests): a procedural skeleton with six procedural
  clips and a built-in one-layer machine, run like any other.
- **What a machine reads**: speed and direction, grounded and air time, `jumped`, aiming, stances by
  name, mod events (as triggers carrying their value), board fields, item kinds held.
- **Stances** are names a mod sets on a layer (`SetStance`); the character's machine reads them in
  conditions and layer weights. What a stance looks like is the character's business.
- **Layers** are the tree's state machines stacked with `Blend2` nodes; a filter is the layer's bone
  mask. At most four.
- **Animation packs**: a mod's own layers (a crouch, a carry) baked into its item; `SwapLayer` plays a
  pack's layer instead of the character's layer of the same name, with clips retargeted by profile
  bone names (`FitPack`). Items bring layers through wishes the server resolves per player.
- **The viewer's own player is led** (`present/anim_lead.*`): for unanswered predictions the viewer
  runs the machine's upper layers forward from the server's state, with the predicted stance or
  event put in at the press. The lead grows with time since the press, holds when the answer
  arrives (nothing jumps), and is given back at 15% of real time. An input the state does not have
  yet gets a tick of its own, as a command does in the simulation, so the led clock is exactly the
  server's later one. The base layer is never led.
- **Looking and aiming** are part of the pose: while a character faces the camera (`FaceCamera`), its
  spine-to-head chain turns by the camera's pitch, blended over 0.2 s (`AnimState::look`); the `Aim`
  command turns its aim chain toward where the player looks. Clients draw it; hit tests use it.
- **An animation's other tracks** (particles, sounds, lights) stay in the Godot animation: the client
  copies them out of the character's `AnimationPlayer` when first drawn, and a `CbTrackPlayer` plays
  them at the clip and time the pose is playing; values land exactly, method and audio keys fire
  once across rollbacks.
- **Retargeting** onto imported characters: rotations relative to each bone's rest, rest postures
  bridged (arms down to T-pose), hips scaled by height.
- **Ragdolls on screen**: any skeleton hangs off the eleven parts; the last pose blends in over 0.15 s.
- **The editor's node list**: the extension adds the Cinderbox nodes to the Create New Node dialog's
  Favorites, once per project and node.

## Items

| Piece | How |
|---|---|
| Kind | declared by a mod (`ItemKind( "melee.bat" )`), or by its look (`ItemsOfLook`); a condition name everywhere, true while held |
| Held | an entity in a socket of its holder (`SpawnItem`, `HoldItem`), with its own board; it receives events |
| Sockets | `CbSocket` nodes in the character's scene; the client places them from the pose and parents the item as `Item` |
| Stowed | carried without being held, drawn in a holster socket |
| In the world | a body baked from the item scene's `CbItem` (the shape under it, mass, properties) into `items/<kind>.cfg`; dropped, picked up, expiring |
| Slots | numbered places a player carries things in: an item's slot (`HeldItem::slot`), the selected one (`Slots`), `SimConfig::slots`. Select, move and drop are intents in `PlayerInput` (kind, two slots, a count), carried out by `Simulation::StepSlots` before anything asks what is held, once per count: predicted and rolled back like movement |
| Using | the use button (`BtnUse`) uses the selected slot's item; a kind whose `use` says so is used by its slot's key, wherever it is. `StepSlots` records `<kind>.used` on that tick where a mod declared it; the item's mod asks `Context::Used` / `Using` on the same tick |
| Properties | named numbers on a kind (`slot`, `holster`, `use`, `pickup.hold_seconds`, ...): how mods agree about items; the engine reads the first three |
| Look | the item's own scene, rooted at its `CbItem`; the baked `items/<kind>.cfg` names the scene, and the scene's reactions do the rest |

## Looks

Everything a player sees and hears beyond bodies is data in workshop items: no scripts (what the
checks hold and do not: [Workshop items and packs](#workshop-items-and-packs)).

| Node | Does |
|---|---|
| `CbDirector` | the World node: entities with kind, template and state as metadata; runs the reactions under it. Plain Godot: anything can drive it |
| `CbReaction` | on a **cue** (a mod event, a game event) or **while** conditions hold: an animation, a property, a listed method, a scene, a sound, a screen shake or flash, placed by the cue or a node |
| `CbPrediction` | which cue the server will answer a press with (or a held action, every `cooldown`: `while_held`); the cue plays at once, and the server's cue then plays only what waited |
| `CbRules` | a sheet (`.rules`) beside the scene: its rules become reactions, predictions and HUD lines when the scene is used; its motion blocks are baked ([docs/rules-language.md](docs/rules-language.md)) |
| `CbItem` | the root of an item's scene: kind, name, mass, properties, first-person view, where the hands hold it (two markers); with the `CollisionShape3D` under it, baked to `items/<kind>.cfg` |
| `CbLinkLook` | the scene a motion probe's line is drawn as, stretched from the player's socket to the line's end (`FrameEntity::linkEnd`) |
| `CbMotionSet` | not a look: a mod's motions, as a sheet's motion blocks, baked to `motions/<set>.cfg` ([docs/motions.md](docs/motions.md)). The motion nodes (`CbMotion`, `CbProbe`, `CbImpulse`, `CbForce`, `CbLink`, `CbMove`, `CbLaunch`) still bake but no content uses them ([ROADMAP.md](ROADMAP.md#3c-groundwork) deletes them) |
| `CbFieldLabel`, `CbFieldBinding`, `CbPromptLabel` | HUD from fields, for their subject: the local player, or the entity of the list row they are in |
| `CbList`, `CbKey` | a row per player, item, slot or event, copied from the row designed as its child, filtered and sorted by expressions (a scoreboard is a scene, not a node); a viewer key that holds a `ui.` value at 1 while on, freeing the cursor if it says so |
| `CbClick` | what a click on a control does: sets `ui.` values (the viewer's own, unknown to any simulation) and sends an intent for the player's slots. With slots in the frame, a list of slots and a free cursor, an inventory screen is a scene |

- **Paths**: `^` (my entity), `^^` (its holder), `$at` / `$other` (who a cue names), `$local`,
  `$world`, `@field` (the entity a field names). No path leaves the World node.
- **One expression language** (`src/expr`): names, comparisons, arithmetic, `and` / `or` / `not`,
  `?name`. Parsed once into a stack program with the names kept as text; each reader says what a
  name is:

| Reader | Names | Runs |
|---|---|---|
| State machine (`sim/anim_graph`) | resolved at bake against the schema | its own copy of the program, with `expr::Apply` for the arithmetic: no name lookups in the simulation |
| HUD nodes (`present/fields`) | the board, private fields, item kinds | `expr::Evaluate`, programs cached by text |
| Reactions, predictions (`godot/cue`) | the subject's state metadata, or another entity's through a path and a colon; `is_local`, `event.*` | `expr::Evaluate` |

- **Values**: `CbReaction.value_expression` sets a property to an expression (kept current while a
  While holds); `volume_expression` scales a sound; `CbFieldBinding.field` and `{...}` in a
  `CbFieldLabel` take expressions. HUD scenes are not under the World node, so a reaction there has
  no director.
- **World reactions**: every `res://vfx/reactions*.tscn` is loaded once; files add to each other.
- **The prediction rule**: on a predicted cue, a reaction waits for the server when it uses what only
  the server knows (the cue's point, end, value or other entity) or has `wait_for_server` on. Echoes
  are matched by name and order within a second.
- **Predicted state**: a prediction may say what the server's answer changes for the viewer's own
  player. `changes` (`gun.ammo -= 1`) are applied to the mirror's copy of the board after each
  frame's cues are heard, so the server's value and the prediction's are never both counted. A
  `stance` (or the cue as an event) leads the body (see Characters and animation).
- **Cue Preview**: an editor panel that plays a scene's reactions on a small stage with stand-in
  players.

## Maps and templates

- A map is a Godot scene with marker nodes (`CbStatic`, `CbProp`, `CbSpawn`, `CbTemplate`, `CbEntity`)
  **baked** to a `.cbmap` of fixed-point values (1/1024 m, 1/4096 rad), so every platform reads the
  same floats. Scene-tree order is creation order, and so part of the format.
- The server sends the map in the welcome; clients draw the scene it was baked from (or the baked
  shapes when they do not have it).
- **The component registry** (`reflect.h`) is the one description of what an author may attach to an
  entity, read by the Godot inspector, the baker and the simulation. Values are identified by name
  hash; unknown ones are dropped and the rest clamped, so a map cannot push the simulation outside
  the registry.
- **Templates** live in the map: initial values, never behaviour. A spawnable template is what the
  spawn action creates; anything a player spawns still expires and counts against the caps.

## The Godot client

`godot/`: the project the viewer runs in. `game.gd` is joining, input and the HUD (its parts:
`presentation.gd`, `view_camera.gd`, `keys.gd`, `autoplay.gd`); `boot.tscn` loads mods; `workshop.gd`
knows where items are.

- **Threads**: a source simulates on its own thread and publishes frames; the main thread takes the
  newest and extrapolates the interpolation alpha. Input is latched until the source consumed it.
- **Nodes**: each visual is a prefab instance (checked by the scene guard). A player or a ragdoll is
  an entity node with its sockets, its body drawn by its character's look through the
  RenderingServer and posed by the ozz pose (`look.h`, M260). Another player's item that only looks
  is its meshes; one put away out of sight is neither sent nor drawn.
- **The rule (2026-11-17): nodes never grow with players.** Today bodies keep it; an item with
  reactions keeps its scene, effects and sounds are nodes, a list makes a row per entry, and the
  map is its scene: each moves to the RenderingServer, fixed pools or rows in view with the cooked
  items ([ROADMAP.md](ROADMAP.md#6b-cooked-items)). `CharacterLook` and `ItemLook` (`look.*`) read a
  scene the guard instantiated when the game first needs it; the cooker will do that when an item
  is published.
- **Camera**: third person, collides with the map; Godot yaw = simulation yaw − π.
- **The menu**: join by address, settings, an in-game menu. Joining another server restarts the
  process, so mounted packs are clean.
- **Jolt** is enabled for client-only effects; simulation entities never get Godot physics bodies.
- **Export**: `tools/export_client.ps1` with the "Windows" preset.

## Tests and CI

| Kind | Where | Covers |
|---|---|---|
| Unit and scenario tests | `tests/test_main.cpp` (`cb_tests`) | map format, commands, movement, motions, events, state machines, poses, hitboxes, items, the frame codec, snapshots, rollback, the hybrid without a network |
| Network tests | `tests/net_test.cpp` (`cb_net_tests`) | sessions over loopback and a lossy link, both models, mods end to end, sources, the crowd bench |
| Headless Godot checks | `godot/addons/cinderbox_maps/check_*.gd` (29) | reactions, predictions, the scene guard, the pack validator, the track player, the pose winning over Godot animation, retargeting, movement, motion bakes, the menu |
| Tools | `cb_bot`, `cb_netsim`, `cb_replay` | bots (full ones run the real client), a UDP relay that degrades traffic, replay verification, view file summaries |

**CI** (`.github/workflows/determinism.yml`), on every push:

| Build | Compiler | CPU |
|---|---|---|
| windows-clang, windows-gcc, windows-msvc | Clang, MinGW GCC, MSVC | x64 |
| linux-gcc, linux-clang | GCC, Clang | x64 |
| macos-arm64-clang | Apple Clang | ARM64 |

- Each build runs `ctest`, then compares its per-tick hashes (the reference scenario, then a run with
  motions) with `tests/reference_hashes.txt`, and its pose hash with `tests/reference_anim_hash.txt`.
- **Cross-load**: the six portable snapshots must be byte-identical, and each OS loads each of them
  with each of its builds and runs on to the reference's final hash.
- **Not covered by CI**: Linux ARM64; the Godot extensions and the Godot checks; a Godot client
  against a server built by another compiler; sanitizers. Every line of `cb_tests`' and
  `cb_net_tests`' tables is registered (M263). Details:
  [docs/testing.md](docs/testing.md).

## Source layout

| Folder | Holds |
|---|---|
| `cmake/` | float flags (`Determinism.cmake`), pinned dependencies (`Dependencies.cmake`), Box3D's patches |
| `src/expr/` | the expression language (`cb_expr`): no dependencies |
| `src/sim/` | the deterministic simulation (`cb_sim_data`, `cb_sim`). `simulation.*` the engine (level, props, commands, slots, motions' effects, ragdolls, snapshots, hashing); `mover.*`, `move_params.*` movement; `motions.*` motion sets (compiler and runner); `anim_controller.*`, `anim_graph.*` what a state machine reads, and the machine; `physics_inputs.cpp` pushers and physics events; `stand_ins.cpp`, `puppets.cpp`, `puppet.h` the hybrid's states and puppets; `rollback.*`, `hybrid_session.*` the two client sessions; `physics_arena.*`, `box3d_shim.c` Box3D's memory and internals; `physics_threads.*`; `map.*`, `level.h` the map format and layout; `reflect.*` the component registry; `components.h`, `types.h`, `events.h` the data; `mod_schema.*`; `ragdoll.h`; `detmath.h`, `util.h` (hashes, PRNG), `bytes.h`, `small_list.h`; `fingerprint.*`; `world_lifetime.*` (the lock around making flecs and Box3D worlds); `bot_brain.h` |
| `src/anim/` | ozz: the placeholder rig and baked characters (`anim_set.*`), the pose (`pose.*`), joint names (`profile.*`), hitboxes, retargeting, joint math |
| `src/net/` | the protocol, the ENet transport, the hybrid's state codec (`state_codec.*`), the network simulator, replay files |
| `src/server/` | `GameServer`, the mod API, hit tests, the navigation mesh, character and look items, the worker pool, the crowd bench, `cb_server`'s `main.cpp` |
| `server_mods/` | the gameplay mods compiled into `cb_server`; `<mod>/client/` is a mod's look as a Godot project, `<mod>/client_item.cfg` its published item's SHA-256 |
| `characters/` | character items: `<name>/client` the item's Godot project, `<name>/client_item.cfg` its hash |
| `src/client/` | `GameClient` (no rendering; also a "lite" mode), the live and replay sources, replay playback, asking a server before joining, the level's static shapes for the camera |
| `src/present/` | engine-independent presentation: the viewer protocol (`view.h`), the frame codec, view files, `PresentationFrame` and capture, the mirror, fields, the led and caught-up animations, event news, camera collision, pose tools, `scripts/` (spawn effects, player poses, ragdolls) |
| `src/godot/` | the viewer extension (`cinderbox`): `CinderboxClient` (`cinderbox_client*.cpp`, five parts), the look (`look.*`), authoring nodes (map, entity, character, item, motion, HUD, text), the editor's skeleton, the track player, the autobake, the sheet editor; `cue/` the reaction addon (director, reactions, predictions, paths, the scene guard, names, rules sheets, Cue Preview); `peer/` the peer extension (`cinderbox_peer`) |
| `src/tools/` | `cb_netsim`, `cb_replay`, `cb_bot` |
| `godot/` | the game: boot, game scripts, menu, workshop, prefabs, vfx, ui; `maps/` map scenes and `.cbmap` files; `characters/` the shipped mannequin; `assets/sfx/` placeholder sounds; `addons/cinderbox_maps/` the editor tooling, bakers, generators and headless checks |
| `sdk/` | the Godot project a mod's look is made from ([sdk/README.md](sdk/README.md)) |
| `mods_src/` | client mod projects (`example_neon`) |
| `tests/`, `scripts/`, `tools/` | the tests; the determinism and stress scripts; the PowerShell tools (SDK, pack, publish, test a mod, export, bake a map, playtest, crowd client) and `make_sfx.py` |

## Known limits

| Area | Limit |
|---|---|
| Prediction | a reaction played on a wrong guess is not taken back; the led body holds what the state says about movement over the lead; packs' layers are not checked for what they read; a predicted event's clock is one tick ahead on the frame its answer arrives |
| Animation tracks | behind latency a swing is first seen a little way in, and keys before that point do not fire; a state restarted by an event (rapid fire) fires its keys again only when the transition has a crossfade (that is how the viewer tells a restart from a rollback); packs' non-bone tracks are not played |
| State machines | the baked subset of Godot's tree ([docs/characters.md](docs/characters.md#state-machines)): no `travel()`, Sync switching or transitions to End at the top; a OneShot's autorestart and fade curves are not baked; a grouped machine bakes as a nested one; other blend nodes are refused |
| Characters | the third-person camera is the same for every body; first person only for a character with a first-person camera in its scene; a non-humanoid skeleton plays without a ragdoll, without packs that do not fit it, and without first-person arms; no character without a skeleton; the scene ships its animations' bone tracks next to the ozz clips; a mod's `Destroy` of a body someone drives releases its driver first. What bodies, possession and NPCs can do: [docs/characters.md](docs/characters.md) |
| Players with latency (inputs model) | every client simulates the whole world and re-simulates its window when a remote input differs from its guess (almost always: a mouse turns every tick). 64 players at a 200 ms round trip: about 16 steps a tick at 0.8 ms each, 87% of a core here (M139); `reconcile_every` 2 halves it. The step itself is the cost (snapshots 5%); physics follows what is awake (0.02 ms with 2 players, 0.3 to 0.4 ms with 300 props in the air: M140). The hybrid's costs: [docs/scaling.md](docs/scaling.md) |
| Movement | a mod's `SetMove` and `Push` are commands, not predicted (a motion is). The controller is kinematic: its mass decides what contacts, forces and ropes exchange, but a player is never knocked over. Pushes from several players in a tick add up in slot order. A probe's line starts at the same point in every view (not the eye in first person); one probe per player. A While motion's per-second changes need a Float field and are not clamped (a tank fills to a little over full). Another player's motion is seen when its input arrives (hybrid: with its update and events; not run locally). In the hybrid your own motion's effect on a body or another player comes back with the server's events (a throw is drawn at once) |
| Mods | compiled into the server (no hot-loading); switched off by a `disabled` file in the mod's folder (compiled in, run only when `--mods` names it), a part of one by a server option its conditions read; events between mods are a tick late; a board has as many names per scope as mods declare (16-bit field numbers), each one state on every entity that has any. A look published again is taken up live only when it changes nothing the simulation reads (bodies, properties, packs, motions need a restart); a player without the new item keeps the old look; files the new look dropped stay loaded |
| Private fields | per player, not per entity; not in recordings or view files (they read 0). No fog of war: in the inputs model each client simulates the whole world; in the hybrid each has the whole physics world and every character, far ones less often, without others' unshared fields and their items put away |
| Combat | no teams, no spectators |
| Aiming | no marker when the shot is blocked by something the camera sees past (cover in third person); the first-person camera does not lower when crouching (crouching is an animation); the bat's strike is rays along the look from the chest, not the bat's path; bots always use the camera behind |
| First-person body | the world is the third-person one; first person only shows copies: the body casts its shadow without being drawn, legs and arms copies are made for the eye (three poses of one skeleton), and the arms and held item are drawn by a second camera (the sun's shadow map drawn once more). Legs slide back when looking down; the view's pose is the standing upper body; most items need a `view_offset` and `view_rotation`; fingers are the clip's; a held item's shadow is of its meshes at rest; what is stowed on the body is not drawn |
| Items | two kinds sharing a holster socket overlap |
| Packs and servers | the pack validator reads bytes and Godot's binary format's start, not every resource Godot would build ([Workshop items and packs](#workshop-items-and-packs)); neither the checks nor the client make Godot's, ozz's or the network decoders safe against malformed files or a malicious server ([ROADMAP.md](ROADMAP.md#before-launch)) |
| Menu | no server browser |
| Large files | `simulation.cpp` about 7,000 lines; `cinderbox_client.cpp` in five parts, the largest 2,150; `tests/test_main.cpp` about 13,800 ([ROADMAP.md](ROADMAP.md#housekeeping)) |

The old netcode measurements (M4, M5: the inputs model with 64 bots) are in
[docs/scaling.md](docs/scaling.md#m4-and-m5-the-inputs-model-64-bots).
