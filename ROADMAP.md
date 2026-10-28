# Cinderbox — Roadmap

What comes next, in order, and why. [DESIGN.md](DESIGN.md) says how things work today;
[docs/HISTORY.md](docs/HISTORY.md) is what is done. **Rewritten after M234** from a review of the
whole project, with the decisions taken after it ([step 0](#0-decisions)). The last version's steps 1
to 11 (M188 to M232) are done; that version is in git history, and what it left that still holds is
below ([Later](#later), [Parked](#parked), [Leftovers](#leftovers-by-area),
[Decided earlier](#decided-earlier-still-holds)).

## Where it stands (M234)

| Area | What there is |
|---|---|
| The simulation | deterministic on six compilers and three OSes, checked in CI with cross-loaded snapshots and a pose hash; rollback, prediction, a lead for the viewer's own player |
| Netcode | an authoritative server; every client simulates the whole world from everyone's inputs; replays, view files, bots, a network simulator, a chaos test |
| Bodies | a character per body, any skeleton, possession (of a body or a prop), NPCs a mod drives on the server's navigation mesh, the `waves` mod |
| Animation | Godot AnimationTrees baked to a deterministic subset (blend spaces, TimeScale, OneShot, Add2, nested machines); packs and overlays an item brings |
| Rules | C++ server mods; sheets the simulation runs for what moves bodies (brawl, dash, flight, grapple, frag, grenade, cannon). The bat and the guns are still C++ with hand-written predictions |
| Looks | reactions, predictions and HUD lines in sheets; the scene guard and the pack validator |
| The world | props, destructible walls, items, containers; maps baked from Godot scenes |
| Modding | a Godot project per mod made from the SDK, Test mod, a look republished while the server runs; the editor knows the names after a server build |

## How we work from now

| Rule | Why |
|---|---|
| A milestone is done when shipped content uses it and it survived a playtest, not when its test passes | M230 to M232 are tested, and nothing shipped uses them |
| Features wait until [the slice](#4-the-slice) needs them | an engine with no game makes every feature generic, and a generic feature is never finished |
| One fact, one place | the bat's swing length is in three places (the clip, the sheet, the C++) |
| The pack is the source of truth | names declared in C++ make the editor wait for a server build |
| Standard words in the modders' docs | modders come from Roblox, Unity and Godot: client-side prediction, replicated field, ability, cooldown, hitscan, not "a lead late" or "a body nobody drives" |

## What the review found

### Keep

- The determinism CI: six builds, cross-loaded snapshots, a pose hash. Rare, and it holds.
- The viewer and its sources: a viewer never knows where its frames come from.
- Mods act on the world only through commands.
- Replays, view files, bots, `cb_netsim`, the chaos test.
- **The brawl model**: skills fully predicted from sheets, the server deciding only damage. Most
  Roblox fighting games either trust the client's hits or show the server's late.

### Problems

| Problem | Evidence | Fixed by |
|---|---|---|
| Speed with no playtest gate | 234 milestones from Sep 16 to Oct 27; M221 to M234 in under 15 hours. CI is green, but nothing checks the features in play together; M223 was crash fixes for M220 to M222 | [step 1](#1-stabilize-m215-to-m234), [the rules above](#how-we-work-from-now) |
| Features nothing ships | TimeScale, crossfade curves, OneShot, Add2, nested machines (M230 to M232); `CbCurve` and `curve()` (M207) | steps [1](#1-stabilize-m215-to-m234), [6](#6-sheets-improved) |
| A migration left half done | brawl, dash, flight, grapple, frag, grenade and cannon are sheets; the bat and the guns are C++ plus `predict` lines | [step 5](#5-every-item-a-sheet) |
| Dead code | melee.cpp sets `melee` and `melee_swing` on a layer `full`, gun.cpp `gun` on `upper`; no state machine reads them (the mannequin's layers are FullBody and UpperBody; the packs start on events). bat.rules predicts `stance melee_swing on full` | steps [2](#2-small-fixes-from-the-review), [5](#5-every-item-a-sheet) |
| Stale docs | characters.md gives the mannequin an upper-body layer for the pistol and the bat (its graph has `UpperBody` at weight 0, one `Rest` state) and uses those stances as examples; the SDK README lists the unmaintained motion nodes as Favorites | [step 2](#2-small-fixes-from-the-review) |
| No game | the README says physics sandbox, the notes say Roblox fighting games, the review asked about hundreds of players: three different engines | steps [0](#0-decisions), [3](#3-two-network-models), [4](#4-the-slice) |
| One fact in three places | the bat: `melee.done` at 1.1 s in the clip, `cooldown 1.1` in bat.rules, `kCooldownSeconds 0.6` in melee.cpp | steps [5](#5-every-item-a-sheet), [7](#7-timing-from-the-clip) |
| Names declared in C++ | the editor knows a mod's names only after cb_server is built (it writes `cinderbox_names.cfg` into every project) | [step 6](#6-sheets-improved) |
| 18 Godot projects, one `res://` | files collide already: `vfx/blast.tscn` in frag and grenade (two files that differ), `assets/sfx/bat_hit.wav` and `swing.wav` in melee and brawl. The SDK's rule (name files after the mod) is broken by the shipped mods | steps [2](#2-small-fixes-from-the-review), [9](#9-cinderbox-studio) |
| Rules need a compiler | a modder builds the engine (CMake, Ninja, Clang); a host cannot run two authors' mods without a build. The workshop covers looks; rules mean forking the server | [step 10](#10-server-rules-in-luau) |
| Four languages in one file | `motion` / `use` (simulated, indenting counts), `on` / `while` (the viewer's reactions, indenting "for the eye"), `predict` (the viewer's guesses), `show` / `text` / `bind` (HUD). The fist's sheet is all the first kind, the bat's has none of it | [step 6](#6-sheets-improved) |

### Weak points

| Weak point | Why it matters | Where |
|---|---|---|
| The player cap | every client steps the whole world, and again for its rollback window: 64 players at a 200 ms round trip take 87% of a core (M139) | [step 3](#3-two-network-models) |
| Cheating | every client has the whole world: ESP and wallhacks are trivial, and there is no fog of war | [step 3](#3-two-network-models) (the server-state model sends only what is near), [before launch](#before-launch) |
| Untrusted Godot files | workshop items are scenes loaded on every player's machine; the guard and the validator check what is in them, not Godot's parsers (DESIGN.md says so) | [before launch](#before-launch) |
| Mods in the server's process | one mod's crash takes the server down | [step 10](#10-server-rules-in-luau) |
| Packs cannot be unloaded | joining another server restarts the game; per-player avatars as packs would make it worse | [step 11](#11-avatars) |
| Windows-only tools, no prebuilt SDK | the scripts are PowerShell; the extension is built from source | [step 9](#9-cinderbox-studio) |
| Big files | `simulation.cpp` about 6,000 lines, `tests/test_main.cpp` about 12,500 | [housekeeping](#housekeeping) |

## Next, in order

| # | What | In one line |
|---|---|---|
| 0 | [Decisions](#0-decisions) | **taken**: the server-state model the default, today's a server's choice; `.cbsheet`; Luau; the standard skeleton only |
| 1 | [Stabilize M215 to M234](#1-stabilize-m215-to-m234) | a written playtest checklist, run by bots and by hand with two games under latency and loss; every feature since M215 used by shipped content or removed |
| 2 | [Small fixes from the review](#2-small-fixes-from-the-review) | **done (M243)**: dead stances, colliding files, stale docs, two Jolt areas, an old replay, the unmaintained nodes out of sight |
| 3 | [Two network models](#3-two-network-models) | the server's state, sent by what is near, the default; everyone simulating the whole world kept as a choice; the server says which on join. **M244, M245 done**: states and stand-ins, on the wire (`--net-model state`); the default stays `inputs` until a two-game playtest |
| 4 | [The slice](#4-the-slice) | one game, played by people; it starts here and runs beside steps 5 to 8, each done when the slice uses it |
| 5 | [Every item a sheet](#5-every-item-a-sheet) | the bat and the guns the way the fist is: predicted from their sheets, the C++ only deciding damage |
| 6 | [Sheets improved](#6-sheets-improved) | a resource on the node it is about (CbRules goes), `.cbsheet`, names and parameters in the sheet, readable cooldowns, the warts fixed |
| 7 | [Timing from the clip](#7-timing-from-the-clip) | windows keyed on the animation (hit, invulnerable, cancel), and a hit shape that follows a bone |
| 8 | [Body parts with priorities](#8-body-parts-with-priorities) | animation asks for parts at a priority, instead of two named layers and overlays |
| 9 | [Cinderbox Studio](#9-cinderbox-studio) | the Godot editor made the mod tool: prebuilt SDK, feature profile, a Cinderbox tab, gizmos, export and debugger plugins, one Play, one authoring project |
| 10 | [Server rules in Luau](#10-server-rules-in-luau) | scripts in the mod's item, sandboxed and reloaded on save; C++ for engine mechanisms |
| 11 | [Avatars](#11-avatars) | one standard humanoid body; the appearance is the player's, swapped on the client |
| 12 | [Godot's own, used](#12-godots-own-used) | spring bones, instance uniforms, threaded loading, MultiMesh, translations; cosmetic physics decided |

- **Why stabilize first**: the last twenty milestones went in "features first, bugs after"; this is
  the after. Each built on the one before (characters per body, possession, NPCs, NPC items, waves),
  so a bug found later sits under three layers that changed since.
- **Why the network model before the slice**: it is the default now, and what the sheets predict
  and how the slice plays both depend on it.
- **Why the slice before the tools**: steps 5 to 8 are what a fighting game leans on (predicted
  skills, timing, full-body moves); the slice says which parts matter. Steps 9 to 11 are for
  modders and players, and are worth more once there is a game to mod.

### 0. Decisions

**Taken (2026-10-27):**

| Decision | Taken | Means |
|---|---|---|
| **The network model** | the **server-state model** (the client rolls back to the server's state) is the default; today's model (everyone simulates the whole world from everyone's inputs) stays as a choice. The server picks one and tells each player when it joins | [step 3](#3-two-network-models), right after the fixes |
| **The sheet's file name** | **`.cbsheet`** (a language name gets taken: Ember is a programming language) | [step 6](#6-sheets-improved) |
| **How server-only rules are written** | **Luau** | [step 10](#10-server-rules-in-luau) |
| **Avatars' proportions** | **the standard skeleton only**: no body scales | [step 11](#11-avatars) |

**Open:**

| Decision | Options | Leaning | Decides |
|---|---|---|---|
| Cosmetic physics | Jolt with a static copy of the level; no Jolt at all | decided later | [step 12](#12-godots-own-used) |
| The slice's game, and the players per server it is made for | an arena brawler like a Roblox fighting game; a big sandbox | the brawler (the notes say Roblox fighting games) | [step 4](#4-the-slice) |

The file name and `.res`: making a sheet a Godot resource does not make it a `.res`. `.res` and
`.tres` are Godot's generic containers (binary and text) for any resource; Godot's own Shader and
GDScript are resources kept as their own plain-text files (`.gdshader`, `.gd`) by loaders of their
own. A sheet is the same: a loader and a saver in the extension for `.cbsheet`
([step 6](#6-sheets-improved)).

### 1. Stabilize M215 to M234

A written checklist ([docs/playtest.md](docs/playtest.md)), run by hand with two games on one
machine (no other testers yet) and unattended, at a 150 ms round trip with 2% loss:
`tools\playtest.ps1` starts the server, `cb_netsim` and the two games, records the session and
verifies its replay at the end (`-Autoplay SEC`: the games play by themselves). What breaks is fixed
with a test each.

| Milestones | Check |
|---|---|
| M215 (the motion lines) | F1 says why a skill did not start, for every shipped skill |
| M216 to M218 (characters per body, eyes) | a mod changes a body's character mid-game; first person only with a camera in the scene; shots start at the eyes |
| M219 (any skeleton) | the critter walks, dies without a ragdoll, keeps its own layer when a pack does not fit |
| M220, M221, M225, M226 (bodies, props) | leave a body holding an item and take it back; drive a crate through a doorway, along walls, turning in a corner; the camera's distance by size |
| M222, M224, M228 (NPCs) | an NPC walks, is shot, dies, respawns, jabs with the fist; on screen without jumps |
| M227 (finding the way) | around obstacles; after a wall breaks (the mesh is not mended: known) |
| M229 (waves) | 32 NPCs: the client's frame time; a player joining mid-wave; a replay and a view file of it watched to the end |
| M230 to M232 (the tree, wider) | each used by a shipped character or pack, or removed. Ideas: the bat's swing in 0.6 s (TimeScale), a flinch on `combat.hurt` (OneShot), a nested machine for a combo |
| M223 (the chaos test) | a long soak with every mod on, under loss |

**Done when** the checklist passes and every feature since M215 is used by shipped content or gone.

**So far (M235)**: the launcher and the checklist; unattended sessions (the default mods, the
critter, waves, waves of 32 NPCs) clean after two fixes.

| Found | State |
|---|---|
| Both games of a waves session crashed at once: a rollback gave a NetId to something else, and the viewer wrote a ragdoll's pose into a prop's visual | fixed (M235) |
| A body taken back lost what its life started with: its death dropped its starting items as loot beside the next life's | fixed (M235) |
| The local paid `ual_mannequin` predated M218: no first-person camera (first person did nothing), an upper layer reading `pistol or melee or melee_swing` | regenerated (M235) |
| A grenade is never thrown by a skeleton its arm's pack does not fit (the critter): the throw waits for the pack's `frag.release` marker, and that pack is not played | fixed (M238): the throw waits for the marker or a moment, whichever comes first |
| The waves mod takes any body nobody drives within 0.5 m of a spot it asked for one (a player's left body, say) | fixed (M242): only bodies that were not there when it asked |
| `combat.hurt` was said at the attacker: a victim's state machine could not react to it (a flinch) | fixed (M242): said at the victim, the attacker as its other entity. A hurt animation per weapon would need the weapon in `combat.damage`, passed on with the hurt: later |
| 32 NPCs: the server sends 1 to 2 Mbit/s to two players (their inputs) | to watch ([step 3](#3-two-network-models)) |
| With brawl on, the unattended player never reaches the pistol or the bat (the kit takes the first slots) | the checklist covers them by hand |
| The pistol and the rifle did not hurt NPCs (only a target with a player's slot counted) | fixed (M236) |
| The regenerated `ual_mannequin` turned held items 90 degrees (its hand sockets tuned by hand before) | the old sockets put back (M236) |
| The fist hit and shoved on the click, before the drawn punch reached anyone | fixed (M237): it hits on the clip's marker, or a moment later without one |
| With the fist out the walk's arm swing does not show (the pack owns the upper body) | the user's own clips to come |
| HUD lines overlap at half-screen windows (the debug lines over the skills, the hotbar over the keys' line and the health bar) | not a priority (the user plays full screen), kept |
| Spamming use breaks the bat's and the guns' animations: their hand-written `predict` lines guess a use every 1.1 s / 0.19 s from the click, the server allows one 1.1 s after its own swing started / every 0.2 s from when the press arrives; guesses outrun it, the drawn swing restarts on a guess the server refuses and snaps back when it is taken back (measured with `--spam-use`, M239) | the fix is [step 5](#5-every-item-a-sheet): a sheet's use is predicted exactly, no guess |
| Spamming the fist restarts its punch every 0.3 s (its cooldown), before a 0.87 to 1.33 s clip plays out; drawn exactly as simulated (no guess) | fixed (M240): a recovery marker (`brawl.fist.ready`) in each punch; the next starts from there |
| In third person the fist hit where the camera looked but shoved along the body's facing | fixed (M241): the brawl kit hits and shoves where the body faces (`cast ... along facing`, new); the camera is the default only for what is aimed (the hook) |
| A motion's wait (`then when EVENT`) or `if` never heard an animation's marker (said after the motions had run); the fist and the throws always took their timeouts | fixed (M240): they hear it the tick after, as a trigger does |

**M230 to M232 in use**: TimeScale and crossfade curves are the fist's (M236: `brawl.fists`, the
finisher); OneShot is the mannequin's flinch on `combat.hurt` (M242: said at the victim now); the
nested state machine is the fist's `Guard` (M242). **Add2: kept for later** (decided after M241): a
lean or a breathing sway, once there are clips for it.

**Next in step 1**: the checklist by hand (two games).

### 2. Small fixes from the review

| Fix | What |
|---|---|
| Dead stances | **done (M243)**: the gun's `gun` on `upper` gone (the gun aims the holder; its swap test checks the aim); the bat's go with [step 5](#5-every-item-a-sheet) |
| Colliding files | **done (M243)**: `vfx/frag_blast.tscn`, `vfx/grenade_blast.tscn`, `assets/sfx/brawl_hit.wav`, `brawl_swing.wav`, `melee_bat_hit.wav`, `melee_swing.wav` (brawl's and melee's sounds also shared an import UID: imported again). None collide now, nor with the base game's. [Step 9](#9-cinderbox-studio) makes the folders a rule |
| Stale docs | **done (M243)**: characters.md (the mannequin's layers, the stances' examples, "the default characters still have pistol and bat states"), server-mods.md (the gun's stance); the SDK README's Favorites |
| The unmaintained motion nodes | `CbMotion`, `CbProbe`, `CbImpulse`, `CbForce`, `CbLink`, `CbMove`, `CbLaunch`: **out of the Favorites (M243)**, taken out once from projects that had them; deleted in [step 6](#6-sheets-improved) (no shipped mod uses them) |
| Two areas in Jolt | **done (M243)**: every `Area3D` of a look is made quiet when the game instantiates it (no monitoring: `cue::Instantiate`, beside taking out cameras); the bake still reads its shapes |
| `mygame.cbr` in the checkout | **deleted (M243)**: a replay from Sep 17 (`CBRP`, version 2; today's is 47), git-ignored, nothing could play it |
| An uncommitted file | `godot/addons/cinderbox_maps/check_anim_tree.gd.uid` (M230's check): **committed (M235)** |

**What Jolt does today**: nothing of the simulation's. Godot makes a physics object only for a
`CollisionShape3D` under a physics node; every Cinderbox authoring node (`CbStatic`, `CbProp`,
`CbItem`, `CbCharacter`, `CbBreakable`) is a plain `Node3D`, and `CbHitbox` says so in code. The
shapes are data the bakes read; collision is Box3D's, the camera's too (a Box3D copy of the level).
The two areas above are the only things in Jolt. Cosmetic physics: [step 12](#12-godots-own-used).

### 3. Two network models

**Decided**: the server-state model is the default; today's stays as a choice. The server picks
(`cb_server --net-model state|inputs`, `state` when not said), the welcome says which, and the game
makes the matching source. Everything else is shared: the simulation, the sheets, the mods, the
viewer.

| | Inputs (today's) | Server state (the default) |
|---|---|---|
| What a client gets | every player's input, every tick | the state of what is near or relevant to it, at a rate by priority |
| What a client simulates | the whole world, from inputs | its own body, its skills and what it touches; the rest is drawn from the server's states, a little in the past |
| A disagreement | back to its snapshot, everyone's inputs run again | back to the server's state for what it predicts, its own inputs run again |
| Other players' effects on you (a shove, a slam) | predicted from their last input | seen when the server's state arrives (a round trip) |
| Fog of war, ESP | impossible: every client has everything | what is not sent cannot be seen |
| Determinism | required (a desync is a resync) | lowers corrections; still checked in CI (the inputs model and replays need it) |
| For | small servers (a fighting game's 10 to 30) | the default; many players |

**What is in the way of many players today**: every client steps the whole world, and steps it
again for the whole rollback window whenever a remote player's input was not what it guessed:
nearly every tick, since mice turn every tick.

| | 64 players (M139) | 200 or more (an estimate) |
|---|---|---|
| One step | about 0.8 ms | about 2.5 ms or more |
| At a 200 ms round trip | about 16 steps again each tick: 87% of a core | about 40 ms of work in a 16.7 ms tick |
| The server | fine (it never rolls back) | still fine |
| Bandwidth | every input to every client | grows with the players squared, at the server |

**Measured first (M244)**, on a recording of 32 chaotic bots (710 entities):

| | Bytes |
|---|---|
| The whole simulation state, as a welcome sends it | 760 KB |
| of which the entities | 90 KB |
| of which the physics engine's internals (contacts, trees, solver caches) | 670 KB |
| The whole state as a change against two ticks before | 410 KB (it shifts: a XOR finds little) |

- **So no physics engine state crosses the network.** The server sends its entities and a
  description of each body (its shapes, read from the physics engine); a client builds a body from
  the description when it has none or the server's changed. What is not the client's own is a
  **stand-in**: a kinematic body placed where the server has it, which nothing in the client's step
  drives. Destruction is then the server's alone, and needs no determinism.

**The milestones**, in order:

| Milestone | What |
|---|---|
| **M244** (done) | Offline, no network: `Simulation::SaveState` and `ApplyState`, stand-ins, `StateSession` (the client's prediction: the newest state, then its own inputs again). Applied at once a state is the server's world, entity for entity (every state of a 32-bot session); the client's step costs a sixth to a half of the server's, less the more players there are (stand-ins are not simulated). `cb_replay state` plays the model against any recording |
| **M245** (done) | The wire: `cb_server --net-model state\|inputs` (protocol 75: the welcome and the server's info say which); a state every second tick, compact (positions, rotations and velocities rounded), sent as a change against the newest the client acknowledged, lined up entity by entity and deflated (`net/state_codec.h`); the client's `StateSession` beside `RollbackSession`; `playtest.ps1 -NetModel state`. Bots on one machine: client work 1.2 ms a frame against 9.1 at 4 players, 3.8 against 37 at 16; download 450 against 387 kbit/s at 4, 1149 against 441 at 16. An unattended two-game session through the simulator: clean. **The default turns to `state` once it plays well in the hand checklist** ([playtest.md, 5](docs/playtest.md#5-the-state-network-model)) |
| M246 | What the player touches, predicted (prediction switching, as Unity's Netcode for Entities calls it): a loose body near the player is simulated, not placed, until it settles. Moved up by M244's numbers: a stand-in crate stops the player outright where the server's gives way (the mover shoves only what is dynamic) |
| M247 | Only what is near is sent, by priority (near every state, far a few times a second, coarser); joining sends what is near, not the whole world; the rest drawn a little in the past (the mirror smooths already) |
| M248 | Hits rewind other players to where the shooter saw them (lag compensation), within a limit; private fields and hidden entities (what is not sent cannot be seen) |
| Measured | players per server, bandwidth per client, the server's tick, the client's frame: targets set once M245 plays |

**M244's numbers** (`cb_replay state`, states 6 ticks late, every second tick):

| Session | A state, raw | Deflated change | The client's step / the server's | Own body within 5 cm |
|---|---|---|---|---|
| 4 bots walking (158 entities) | 42 KB | 4 KB (1 Mbit/s) | 0.045 / 0.106 ms | 96% of ticks (1.6 cm on average) |
| 4 chaotic bots | 44 KB | 9.6 KB (2.3 Mbit/s) | 0.047 / 0.157 ms | 79% (17 cm): the others' shoves and props are the server's, seen a state later |
| 32 chaotic bots | 143 KB | 36 KB (8.7 Mbit/s at 30 a second: M245's compact state is needed) | 0.19 / 1.09 ms | 45% (0.4 m): a crowd shoving, and crates that do not give way (M246) |

- **Mods do not change**: they read the server's world and send commands into its simulation in
  either model. Recordings do not either: the server's simulation is deterministic, so its inputs
  and commands still re-simulate.
- **Physics stays one shared world, the server's**: a ragdoll falls the same on every screen.
- **Open**: how often state goes out (the simulation's 60 Hz or 30); whether other players within
  arm's reach are predicted from their inputs (sent with the state) for a fighting game's shoves.
  How a client's simulation runs with only part of the world is answered (M244): what it has are
  stand-ins, what it lacks is not there.

**Where the numbers come from** (for the targets):

| Players in one place | What works | Examples |
|---|---|---|
| up to about 32 | everyone simulating the whole world from inputs (the inputs model) | fighting games; For Honor's 4v4 |
| about 50 to 200 | the server's state, sent by what is near, one server | Fortnite, PUBG, Apex |
| hundreds in one fight | the same, with far players sent rarely and coarsely, fewer collisions | PlanetSide 2 |
| thousands in one world | many servers per world: zones, copies of a zone, meshing | WoW, EVE (time dilation), Star Citizen |

- **What MMOs add for their numbers** is not a better prediction: sending only what is near,
  cheaper players (no collision between players, movement checked rather than simulated, 10 to 20
  ticks a second), hits decided by the server with lag compensation, and the world split across
  servers.
- **Shared physics is what makes Cinderbox dear at scale**: MMOs mostly have no props or
  destruction. The busiest spot (everyone in one square) sets the bandwidth.
- **The cheap way to many players** is many servers of 50 to 100 per game (how Roblox scales a
  game), not one server of 1,000. One world across servers is [later](#one-world-across-servers).
- **The inputs model, tuned** for small servers: `reconcile_every 2`; a better guess of a remote
  player's input (keep its mouse turning, rather than repeating the last input).

### 4. The slice

One game made with the engine and played by people. Leaning ([step 0](#0-decisions)): **an arena
brawler**: one map, one mode (rounds and a score), the brawl kit and the bat, a ten-minute session
that is fun.

- Built from now on, beside steps 5 to 8: each of those is done when the slice uses it.
- Played every week, with latency (by hand with two games until there are testers). What it runs
  into is the next milestone.
- **Done when** strangers play a session and want another.

### 5. Every item a sheet

The fist's way for every item: the item's sheet runs its use in every simulation (predicted), and
the mod's C++ turns its hit events into damage (brawl.cpp is the model, 143 lines).

**The bat, today**:

| | |
|---|---|
| The swing | melee.cpp (272 lines): five rays fanned out from the chest at the clip's `melee.strike` marker; the hit test poses the target (hit zones) |
| The look's guess | `predict use as melee.swing`, `cooldown 1.1` copied from the clip, `stance melee_swing on full` (read by nothing) |
| `melee.hot` | a field on the bat, set by the server when a swing hits a player (`ctx.Set`), cleared by the mod's own timer 2 s later. Not predicted; a prop hit does not heat it |
| NPCs | cannot swing it: melee.cpp loops over player slots. The fist works for NPCs because sheets run for every driver (M228) |
| The two sheets | `bat.rules` sits in the bat's scene (paths from the bat, `^^` its holder: the glow, the flames, the sparks, the guess); `reactions_melee.rules` in a world reactions scene loaded once (paths from `$at`: the sounds). Same format, different place, different job; both converted by a script from the old nodes (the `# NodeName` headings, `# FlamesOnSwing` heading nothing, tabs and spaces mixed) |

**The bat, after** (a sketch, not tried):

```
use
	until melee.done          # the clip's marker ends it: one place for the swing's length
	emit melee.swing          # the pack's Swing state starts on it, predicted: no predict line

motion Strike on melee.strike
	cast sphere 0.25 range 1.8
		emit melee.hit
		impulse 7 along look
```

- melee.cpp turns `melee.hit` into `combat.damage`: about 60 lines. NPCs swing it.
- **The glow**: only a look, so no field: `on melee.hit` in the bat's sheet sets the emission and
  sets it back after 2 s, at once, because the hit is predicted. If heat should matter to the game
  (a hot bat hurts more), the language needs `change` on the item and a timer on a carried item
  ([step 6](#6-sheets-improved)).
- **A sheet's cast hits capsules**: the simulation has no pose, the hit test's hit zones are the
  server's. A bat can live with that, as the fist does; the follow-the-bat shape is
  [step 7](#7-timing-from-the-clip).
- **The guns**: the use (ammo, the rate of fire, the cue) moves into the sheet, so their `predict`
  lines go; the shot's hit stays the server's (hit zones need the posed hitboxes) and hears the
  sheet's event. A cast that poses what it passes near is possible later: the pose is
  deterministic (CI checks its hash).

### 6. Sheets improved

The language is right: bounded, deterministic, run by every simulation, so skills are predicted
(Overwatch does the same with a visual language, Statescript). What changes is how it is attached,
written and checked.

| Change | Why |
|---|---|
| **A resource on the node it is about**: `CbSheet`, read and written by the extension's own loader and saver, plain text in a `.cbsheet` file, as `.gd` and `.gdshader` are. A property on the node: `CbItem.sheet`, `CbMotionSet.sheet`, the world reactions root, the HUD root; the node says which blocks it allows. It may be saved inside the scene, as a built-in script is. **CbRules goes** | `file` is a plain string today: no UID, a rename breaks it, Godot does not see the dependency. Saved inside the scene, the notes stay with the item (Godot drops a scene's comments) |
| **Not Godot's script slot** | the security story is "every script is refused"; "every script but ours" is a rule someone will probe, and a script language through GDExtension is a large, fragile API for two features (errors in the gutter, completion) |
| **Where each block runs, said**: the simulated blocks and the viewer's in marked sections (or files of their own), one indenting rule (it counts) | four languages in one file today, with two indenting rules |
| **Names in the sheet**: `field brawl.combo int`, `event brawl.jab.hit`; the server reads them from the pack | the editor waits for a server build today |
| **Parameters**: `param damage 8`, shown in the item's inspector, read by the server's rules | numbers written twice (the bat) |
| **A motion's state readable**: its cooldown left, its charges, whether it is on, in conditions and HUD lines | brawl rebuilds cooldowns: five fields declared in C++, five `while` motions counting them down, an `if` and a `change` per skill, while `cooldown` goes unused |
| **Rates said**: `change x -= 1 per second` | `change x -= 1` is once in a step and per second in a `while` |
| **One rule for expressions**: one word bare, anything longer in brackets | an expression ends at "the next option word" today (`impulse (distance < frag.radius) * ... along away` works because `along` is one) |
| **`let NAME = EXPRESSION`** inside a motion | the frag's falloff written out twice |
| **A cast and the owner's own effects in one step** | dash_strike.rules: `use if brawl.cd_dash <= 0` and `motion Hit on use if brawl.cd_dash <= 0`, two motions with a copied condition |
| **`change` on the item; a timer on a carried item** | the bat's heat, if it is ever more than a look |
| **Curves as resources**: `curve(res://falloff.tres, reach)` or a curves dictionary on the node; the frag's blast falloff the first user. The `CbCurve` node goes | no shipped sheet uses `curve()`; a node found by its name from text |
| **The planned words**: `$target:FIELD`, `at POINT`, a cast in a later step ([the list](docs/rules-language.md#planned)) | |
| **Tools**: a formatter; errors in the editor as you type; sheet tests run headless (a motion on a stand-in, then what moved) | |
| **The unmaintained motion nodes deleted** | the sheet's bake is the only one |
| **`.rules` becomes `.cbsheet`**: 40 sheets and about 80 files that name `.rules`; `docs/rules-language.md` with it; a `.gitattributes` line and a small VS Code grammar | the name says what the file is, one family with `.cbmap` and `.cbreplay` |

**The dash strike, after** (proposed, not valid today):

```
use
	cooldown 3
	emit brawl.dash_strike.started
	impulse 16 replace horizontal
	move for 0.25 friction 0
	cast ray range 4
		emit brawl.dash_strike.hit
		impulse 10 on target along move
```

Its HUD line reads the cooldown itself: `text Line "DASH STRIKE {cooldown.brawl.dash_strike}s" if
cooldown.brawl.dash_strike > 0`. brawl's `cd_dash` field and its `CoolDash` motion go.

**Considered and not taken**: a visual graph ([routes not taken](#routes-not-taken)); a
general deterministic VM ([later](#later)).

### 7. Timing from the clip

The sheet says what; the clip says when. Unreal's ability system works this way (montages with
notify windows); so do fighting games' frame data.

| On the animation | Baked to | Read as |
|---|---|---|
| a marker (today) | an event at a time | `on melee.strike` |
| a window (hit, invulnerable, cancel, armor): two markers, its start and its end (Godot's timeline has points, not spans) | a span of the clip | the sheet forms the interval: what happens between the start's marker and the end's (`then when x.open`, then `when x.close`); a condition `window.hit` only if sheets keep needing it |
| a hit shape on a bone (a `CollisionShape3D` under a `BoneAttachment3D`) | swept from tick to tick along the pose while its window is open | a cast that follows the bat |

- The simulation poses one chain of the owner's bones while a window is open, not the whole body
  (the pose is deterministic: CI checks its hash). Under the ozz pose, as everything that hits.
- Gives: the bat's arc, a kick's foot, the critter's tail. Answers the language's open question "a
  shape that follows a bone (a sword's arc)".
- Removes: numbers copied from clips (`cooldown 1.1`).

### 8. Body parts with priorities

**Today**: two named layers (`FullBody`, `UpperBody`) a pack replaces by name; up to three overlay
slots a player fills in the order its items are carried; bone filters that are on or off; two
special rules ("what the hands hold wins the arms", "a hand an overlay names lets go").

**Short for a fighting game**: most of its skills are full-body (an uppercut, a slam, a dash
strike), and today a full-body move is an overlay, ordered by which item was picked up first. A
filter's hard edge at the spine shows when a swing plays over a run. The critter has only `FullBody`.

**After** (Unreal's montage slots and per-bone blending, in the simulation):

| | |
|---|---|
| Parts | a character has body parts: a humanoid gets Legs, Torso, Head, LeftArm and RightArm from its bone profile; another skeleton authors its own (Tail, Wings) |
| Requests | every animation asks for parts, at a priority, with fade times, replacing or added |
| Who wins | on each part, the highest priority; added ones (a recoil, a flinch) on top |
| Priorities | locomotion, then the held item, then a skill, then a hit reaction or a stun |
| Legs | a request keeps the legs walking (a jab on the run) or takes them (a heavy swing roots you) |
| Borders | weights rising along the spine, not a cut |
| The two rules | one rule: the held item asks for its arms above an overlay; a throw asks for its arm above that |
| Authoring | still an AnimationTree per pack; the pack's root says its parts and priority instead of matching layer names |

In the shared pose, so hitboxes follow; its state per part is small (which request, its weight).

### 9. Cinderbox Studio

Godot's editor is the studio (Roblox Studio exists because Roblox owns its engine; Godot's editor is
ours). The extension makes it the mod tool:

| Godot's hook | Use |
|---|---|
| A prebuilt extension from CI (Windows, Linux, macOS) | making a look needs no compiler |
| `EditorFeatureProfile`, shipped with the SDK | hides script creation, the Asset Library, and every node class the scene guard refuses: a modder cannot build what the game will reject |
| A main-screen plugin | a Cinderbox tab: the mod's items, names, problems, Publish, Test |
| `EditorNode3DGizmoPlugin` | everything authored as a number, drawn: a find's shape, a cast's range and radius, a rope's length, sockets, hitboxes, the capsule, the eyes |
| `EditorExportPlugin` | bake, check and pack on export: replaces `pack_mod.ps1` and `publish_mod.ps1`, on every platform |
| `EditorDebuggerPlugin` | the F1 motion lines, fields, events and rollbacks in the editor while the test game runs |
| `EditorScenePostImportPlugin` | a dropped `.glb` becomes a `CbCharacter`: bone map, hitboxes, sockets, a starter tree (`make_mannequin.gd` does it as a script) |

| Also | What |
|---|---|
| One authoring project | every mod in a folder of its own (`res://mods/<id>/`); the validator refuses files outside it. Ends the collisions of step 2 and the 18 projects |
| One Play | save, bake, start a local server and the game with this mod, reload on save: Test mod, on every platform |
| Starters by kind | an ability, a weapon, a game mode, a character, an avatar, instead of one starter with everything |
| From "authoring, finished" | names offered inside expressions and sheets; an item used in its preview; the preview with someone else in it (a hit) and the character's own mesh; a change the server reads restarts the test |
| The modders' docs | in standard words (see [How we work from now](#how-we-work-from-now)) |

### 10. Server rules in Luau

**Decided**: Luau. What was parked as "rules without a relink" comes back: today the workshop
carries a mod's look, and its rules mean building the engine.

- **Luau**: what Roblox developers write; made to be sandboxed (no files, no OS, a limit on
  instructions); fast. Rules run on the server only, so they need no determinism.
- The same verbs as today's C++ (`Declare`, `Start`, `Tick`, the `Context`'s reads and commands),
  from a script in the mod's item: a server runs items from different authors without a build, and
  a script reloads on save.
- A script's error stops its mod, not the server.
- **C++ stays** for engine mechanisms and a mod too heavy for a script (compiled in, as now).
- **What is predicted stays in sheets**: a Lua VM's memory cannot be copied cheaply for rollback; a
  sheet's state is a 28-byte record.
- The shipped mods move over in the order the slice needs them.

### 11. Avatars

The body and its appearance kept apart:

| | The body: one for everyone | The avatar: one per player |
|---|---|---|
| What | the standard humanoid `CbCharacter`: the skeleton with Godot's humanoid bone names, the state machine, hitboxes, capsule, sockets, camera | meshes skinned to the same bone names, material colors, accessories |
| Who knows it | the server and every simulation | clients only |

- **Spawn the standard scene and swap the model and the materials**: its default mesh hidden, the
  avatar's `MeshInstance3D`s added with their skins bound to the same `Skeleton3D`, accessories on
  sockets and `BoneAttachment3D`s, colors as instance uniforms.
- **The avatar never changes what the server sees**: everyone's hitboxes are the same (fair), and
  one set of clips and poses serves everyone.
- **Decided: the standard skeleton only**, no body scales (Roblox's R15 way: held items and hitboxes
  match the drawing). An avatar made on another rig is retargeted to it when it is published
  (assets ship baked).
- **An avatar never blocks joining**: today a missing item refuses the join. An avatar is fetched
  in the background, and the mannequin is drawn until it arrives.
- **Budgets checked when publishing**: triangles, materials, texture sizes.
- **Loading**: packs cannot be unloaded and share `res://`, so avatars as packs would grow memory
  and collide. To prototype: each avatar one self-contained baked scene (meshes, materials,
  textures inside), loaded from `user://` by its hash, not mounted; still baked, and freed when
  nobody wears it.
- **Which avatar a player wears**: a value per player for looks only: not in the simulation's hash,
  sent as names are.

### 12. Godot's own, used

| Godot's | For |
|---|---|
| `SpringBoneSimulator3D` | tails, hair, capes, after the pose modifier: cosmetic, nothing that can be shot. The critter's tail first |
| Instance uniforms (`instance uniform` in a shader) | a look per entity (heat, a team color, a hit flash) without a material per entity: the bat copies its material to glow |
| Threaded loading (`ResourceLoader.load_threaded_request`) | packs and characters while joining |
| `MultiMeshInstance3D`, visibility ranges | debris, crowds of NPCs |
| Animation detail by distance | far bodies posed less often: drawing only, hit tests unchanged |
| Translations | a sheet's HUD text is English in the file |
| `Theme` resources | client mods restyling the UI |
| **Cosmetic physics** (decided later) | Jolt has no level (step 2), so a cosmetic shell casing would fall through the floor: Jolt gets a static copy of the level when the map loads, from the same shapes; or Jolt goes, with `RigidBody3D` and `StaticBody3D` out of the scene guard's list |
| `PhysicalBone3D` | ragdolls for any skeleton ([later](#later)) |

## Before launch

| What | Why |
|---|---|
| The pack loaders fuzzed, files limited in size | workshop items are Godot files on every player's machine; the guard checks their content, not Godot's parsers |
| Cheating, decided | on a server running the inputs model every client holds the whole world, so ESP is trivial there; the server-state model sends only what is near |
| A mod's crash | takes the server down until step 10 |
| How players find servers | [parked](#parked) |
| Translations, tools on every platform | steps 12 and 9 |

## Later

In the order they would likely come back.

### Our own animation authoring, apart from Godot's

Decided after M231 instead of layers that are the look's alone (a face, breathing, a tail's wag on
the client): when the time comes, the animation the simulation runs gets an authoring mechanism of
its own, apart from Godot's, and Godot's `AnimationTree`s are left for what is only seen. Only the
skeletal bones would be replicated on the server. Much later; it needs more decisions first.
[Body parts with priorities](#8-body-parts-with-priorities) would be its core.

### Ragdolls for any skeleton

Today a skeleton without a humanoid's bones falls as no ragdoll.

| Piece | What |
|---|---|
| Authored on the scene | Godot's own way: `PhysicalBone3D` nodes on the skeleton (their shapes and joints), baked into a ragdoll the simulation builds for that character, as it builds the humanoid's eleven bodies today |
| In the schema | each character's parts and joints, so every simulation makes the same ragdoll |

### Vehicles

| Piece | What |
|---|---|
| Joints in templates | a template of several bodies (each its shapes) joined by Box3D's joints (revolute, prismatic, weld, wheel), authored as nodes in the scene |
| A seat | a player sits: the mover is off, the player is carried by the body and drawn in its pose (it keeps its body; the vehicle reads its keys) |
| Input to motors | as a motion routes input to the mover, a vehicle routes it to its joints' motors and its forces; the rules (who may drive, damage) are a mod's |
| Platform carry | standing on a moving deck carries the player (its velocity and turn) |
| A body's own controls | every body says how it is driven: the character controller (the mover), a vehicle's (its motors and forces). Possessing a body takes its controls with it |

### Building

Players placing pieces into the world (walls, ramps, floors), the way a base is built.

| Piece | What |
|---|---|
| Placing | a sheet statement that places a template on a grid in front of the player (snapped, turned in steps), so the placer's own simulation shows it on the press, as a launch does |
| The preview | the ghost of the piece where it would go, drawn by the viewer from the same cast the placement uses |
| Holding together | pieces joined where they touch (glue's weld joints, M202), each breakable; what loses its hold falls |
| Rules | what a piece costs, who may build where, how many: a mod's |
| Staying | a server mod saves and loads what was built (server only) |
| The cost | every piece is state in every snapshot: a cap per player and per server, and static pieces where possible |

### A deterministic VM for heavy predicted logic

WebAssembly, for what a sheet cannot say and must still be predicted (an NPC's brain in the
simulation): any source language, its memory cheap to copy for rollback, deterministic with NaNs
made canonical and no threads; bounded by an instruction budget. Modders would need a compiler and
debugging is harder: an escape hatch, not the main way.

### One world across servers

A seamless world split across server processes (meshing). A project the size of a company (Star
Citizen took about a decade); many servers per game ([step 3](#3-two-network-models)) comes first.

## Parked

Decided to wait; each says what it waits for.

| What | Waits for |
|---|---|
| How players find servers | undecided: Steam, a website's "play" link that starts the game, a game's own menu or a desktop shortcut. Until then: join by address, Test locally, a server's mods listed before joining |
| Steam Workshop as the store | the decision above; the store is a folder today |
| Ships | Box3D growing what they lack (buoyancy) rather than more patches (two today: `cmake/patches/`). In the simulation, so everyone predicts them: platform carry (vehicles), a buoyancy component with a wave function the water's look shares |
| **The player's body, for motions** | a mod that wants it; each a word of the sheet language ([its planned list](docs/rules-language.md#planned)), in the simulation, so a motion using it is predicted. In the order they would likely be wanted: |
| · Touching a wall | a condition (`wall`: touching one, beside or ahead) and a frame along it for forces and impulses: wallrunning, wall jumps, a ledge grab |
| · Teleport | a motion effect that sets where the player is: to the target at an offset in its frame (behind it), or to a cast's point. Blink, "behind you", swapping places |
| · What the body passes through | movement parameters `pass_players`, `pass_props`, `pass_level`: a dash through the target, phasing through crowds, a spectator flying through walls |
| · Not hittable | a movement parameter `hittable`: shots, casts and swings pass the player's hitboxes. A dodge's invulnerable frames (or a [window](#7-timing-from-the-clip)) |
| · The capsule's size | movement parameters for its height and radius, so a crouch fits under a low gap; the feet stay, and it does not stand up where the full height would not fit |
| · Keep speed, steer | a motion effect that turns the velocity without changing its size: a glide, a grind rail, a ricochet |
| · Hidden from others | a field the viewer reads to fade a body (and what it holds) for everyone but its player: invisibility, a cloak, a spectator |

## Leftovers, by area

Not scheduled. A row moves up into "Next" when the slice wants it.

| Area | What |
|---|---|
| Items | a state machine or a motion reading the hand's fields; a lifetime for an item that was not launched; a fuse that starts when the pin is pulled (a cook); bots that use what a mod adds; a bot count in the menu |
| The inventory | a split of one; a slot's item shown while dragged over; a key for the bag's slots; dropping or moving part of a stack; a mod taking one off a stack without throwing it; a corpse's bag (undecided: the ragdoll carries it, or a box where it fell); predicted answers to a mod's intents; numbers wider than a byte in an intent |
| Sheets and HUDs | a sheet for a list's rows; an item's HUD drawn on its slot |
| Motions | a lasting force as a C++ command; a test of one player hooked to another; a knocked-down ragdoll state; carrying a player |
| Animation | a slow-motion finisher (a state's speed is in, M230); the dash's and the hook's poses are placeholders |
| Feet on the ground | foot IK in the shared pose, beside the aim chain: each foot's ground found by a ray in the simulation, the hips lowered to the lower foot, two-bone IK per leg, so the legs' hitboxes are where the feet are drawn. Not Godot's IK nodes: they run after the pose (seen only), and Godot's physics has no level |
| The client | keys the server says: a server maps keys to what they do, an item's own key included, sent once at join; a player may still rebind (per server). Cameras a mod places (a round's end, a kill cam) |
| Games | teams and spectators (mods, once private fields and the board cover them); lists on one entity (array fields, or a `CbList` over a field family) |
| Finding the way | one mesh per character size; a mesh mended when a wall breaks; off-mesh links (a jump, a ladder) |
| NPCs | NPCs of a character of their own; a brain in a sheet (predicted exactly, at a rollback cost) |

## Decided earlier (still holds)

| Decision | Why |
|---|---|
| Possession, not an NPC type | a player is a controller (its keys, name, score); what it drives is a body. NPCs, spectators, Prop Hunt and mind control fall out of it |
| The controller is a part, not a kind | a body nobody drives costs what a prop costs; the controller's pass meets only driven bodies |
| What a body's scene says decides how it is driven | its capsule, a first-person camera, a skeleton and state machine, its movement numbers |
| The mover stays C++ | the hottest loop (every player, every tick, again in every rollback); sheets are bounded on purpose |
| Walls static until broken; debris players cannot push (`CatDebris`); settling by Box3D's own sleeping | a wall costs nothing in rollback; turning sleeping pieces static would save next to nothing |
| The ozz pose owns the body | hit tests use it; Godot animation only adds what cannot be shot |
| Physics players react to is shared | ragdolls and falling walls are the same on every screen; cosmetic physics is for what nobody could compare (capes, spring bones, casings) |
| The UI stays presentation-only | a screen shows fields and sends intents; what it cannot say becomes a widget the engine gives, not a script |

## Routes not taken

| Route | Why not |
|---|---|
| Server mods also run on clients | tried as M55 and dropped: rules would ship to clients and every mod would need to be deterministic. The sheet language took its place |
| A built-in dash, a built-in grapple | rules in the engine; mods could only tune what the engine thought of |
| The viewer guesses a command (`CbPrediction` with a push) | the viewer has no simulation: a guessed push cannot collide, and a pull would fight every frame's correction |
| Separate physics for the map and the game | there is one world; a map's bodies and a mod's are authored the same way (M185) |
| Godot's physics at runtime | collision is Box3D's on the server and in every client's prediction; Godot's `CollisionShape3D` is only how shapes are authored |
| Hitboxes as physics bodies | each bone moved every tick, copied in every snapshot, shoving props with its limbs; hitboxes are shapes on the shared pose that hit tests ask |
| Our own editor | Godot's editor is the studio; rebuilding its viewport, inspector, animation editors and importers would take one person years ([step 9](#9-cinderbox-studio) extends it instead) |
| A fork of Godot | every Godot release merged by hand; the extension's editor hooks cover step 9 |
| Sheets in Godot's script slot | "every script is refused" is the security story; a script language through GDExtension is large and fragile for two features ([step 6](#6-sheets-improved)) |
| Sheets as `.res` or `.tres` | Godot's generic containers would hold the sheet as one quoted string: poor diffs, no editing as text. A loader of its own keeps it plain text, as `.gd` and `.gdshader` are |
| A visual graph for skills | hard to diff and review; Godot dropped VisualScript for lack of users |
| A three-letter extension | gains nothing (Godot's own are long: `.gdshader`, `.gdextension`); the `cb` three-letter names are taken: comic archives (`.cbr`, `.cbz`, `.cbt`, `.cb7`), Code::Blocks (`.cbp`), COBOL (`.cbl`) |
| Naming the sheet language Ember | already a programming language's name |
| Body scales for avatars | the standard skeleton only (decided after the review): held items and hitboxes match the drawing, nothing to clamp |
| Everyone simulating the whole world as the default | the rollback window's cost grows with every body; kept as a server's choice for small servers ([step 3](#3-two-network-models)) |

## Housekeeping

Independent of the path; each is small.

| What | Note |
|---|---|
| The rest of `simulation.cpp` in modules | props, ragdolls, items and animation; the mover went first (M92) |
| `tests/test_main.cpp` in files by area | about 12,500 lines in one file |
| Replays' extension | `.cbr` is also the comic-book archive (a double-click opens a comic reader): `.cbreplay` |
| A motion's field and a mod's | both may write one (commands first, then motions); the server could warn when a mod sets every tick a field a motion writes |
| Slots for what is worn | the dash boots take a slot: selected, they are held like a tool |
| `reconcile_every` 2 as the default | halves a client's rollback work with many players and latency (M139); others are seen up to 2 ticks later: to be looked at in play first |
| The characters' part of a step | as much as the physics at 64 players (0.4 ms, M140) and not yet looked into |
| Physics threads beside drawing | on by default (M141), not yet watched in the game: `--physics-threads=0` if frames suffer |
| The old `build/` folder in the checkout | builds go outside the source tree; the folder is ignored and can be deleted |
| `brawl.cpp`'s top comment | names the old roadmap's step 5 |
| Old copies of the extension in `godot/bin` | `~libcinderbox...TMP` files, left by rebuilds while Godot had the library open: about 500 MB, git-ignored, can be deleted |
