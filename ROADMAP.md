# Cinderbox — Roadmap

The road to 1.0: what comes next, in order, and why.

| Read with | |
|---|---|
| [DESIGN.md](DESIGN.md) | how it works today |
| [docs/HISTORY.md](docs/HISTORY.md) | every milestone done, M1 to M261.6 |
| [docs/scaling.md](docs/scaling.md) | how many players a server holds, and every measurement behind it |
| [docs/hytale-comparison.md](docs/hytale-comparison.md) | what Cinderbox could take from Hytale's mods and plugins: [what was taken](#from-the-hytale-comparison) |

**Rewritten after M261** (2026-11-17, M261.6; an outside review's findings added in M261.7). Every statement here about what exists was read in
the code at M261.5's merge (`d3387be`); what is planned says so. The roadmap before it grew from
the rewrite after M234, from a review of the whole project (`c7c09bd`, 2026-10-27); what that
review found and still holds is kept below, in the step it belongs to.

## Where it stands (M261)

| Area | What there is |
|---|---|
| The simulation | flecs and Box3D, deterministic on six builds (Windows: Clang, GCC, MSVC; Linux: GCC, Clang; macOS arm64: Clang). CI compares every build's snapshots and animation pose hash, and loads each system's snapshots on the others |
| Netcode | an authoritative server and two network models, the server's choice. **Hybrid** (`cb_server`'s default since 2026-11-01): the physics simulated by every client from the server's events, never sent as state; other characters as puppets of a few bytes; your own predicted; lag compensation. **Inputs**: every client simulates everything from everyone's inputs, for small servers. Replays, view files, bots, a network simulator, a chaos test |
| Scale | 1023 players a server. 255 fighting in one crowd hold 60 Hz on the server, and the game draws them at 47 frames a second; 1023 in one crowd run, below 60 Hz ([docs/scaling.md](docs/scaling.md)) |
| The client | Godot 4.7 with the engine as a GDExtension. It draws whatever a source hands it (a server, a replay, a view file) and runs no mod's code. Every body is drawn one way, from its character's look, through the RenderingServer (M260) |
| Bodies | a character per body, any skeleton, possession of a body or a prop, NPCs a mod drives on the server's navigation mesh |
| Animation | Godot AnimationTrees baked to a deterministic subset (blend spaces, TimeScale, crossfade curves, OneShot, Add2, nested machines) that the simulation runs; packs and overlays an item brings; far bodies posed less often (M259) |
| Rules | 24 server mods in C++, compiled into the server, 18 of them on by default. Sheets that every simulation runs, so they are predicted, for what moves bodies: brawl, dash, flight, grapple, frag, grenade, cannon. The bat and the guns are still C++ with hand-written predictions |
| Looks | reactions, predictions and HUD lines in sheets; a scene guard and a pack validator for workshop items |
| The world | props, destructible walls, items, containers; maps baked from Godot scenes |
| Modding | a Godot project per mod, made from the SDK; Test mod (build, publish and play in one step); a look published again is taken up without restarting the server or the game; workshop items found by hash, never sent over the game connection; the editor knows a mod's names after a server build |
| Tests | 174 tests in `ctest` (the simulation, the network, the tools) and 29 Godot checks (`check_*.gd`); the determinism CI after every push |

## What 1.0 means

**Decided (2026-11-17).** 1.0 is the version people outside the project can build on: players
find a server and play a game made with Cinderbox, wearing avatars of their own, and modders make
and publish mods, rules included, without building the engine. Strangers play before it, in
[the alpha](#4b-the-alpha), right after the slice: what they run into shapes the rest.

**1.0 is done when:**

| | Step |
|---|---|
| Strangers play the slice, and want another session | [4](#4-the-slice), [the alpha](#4b-the-alpha) |
| A mod's look and rules are made, tested and published with the prebuilt SDK, and run on any server without a build | [9](#9-cinderbox-studio), [10](#10-server-rules-in-luau) |
| The formats modders depend on are settled: the sheet, an item, the rules' API. After 1.0 a change to one is versioned and said | [6](#6-sheets-improved), [10](#10-server-rules-in-luau), [before launch](#before-launch) |
| Joining a stranger's server is safe | [before launch](#before-launch), closed |
| Players find servers without typing an address | [parked](#parked) today: how, decided later |
| A player wears an avatar of their own | [11](#11-avatars) |
| The manual says what the code does, in the words modders use | [before launch](#before-launch) |

**Each step, in 1.0 or not:**

| Step | In 1.0 | Why |
|---|---|---|
| 4 The slice | yes | it is the test of everything else |
| 4b The alpha | yes, before the rest | outside players early, not at the end |
| 5 Every item a sheet | yes | the slice's bat; one way for every item before modders copy one |
| 6 Sheets improved | yes | the sheet's format is settled at 1.0: it changes before, not after |
| 7 Timing from the clip | as far as the slice needs | a fighting game's feel; the slice decides |
| 8 Body parts with priorities | as far as the slice needs | the same |
| 9 Cinderbox Studio | yes | modding without a compiler |
| 10 Server rules in Luau | yes | rules without a compiler; a mod's crash stops the mod, not the server |
| 11 Avatars | yes (decided 2026-11-17) | a player's identity; the owner brings a few models when the step starts |
| 12 Godot's own, used | the parts the slice and launch need (translations, themes) | the rest is polish |
| [A thousand in one crowd](#a-thousand-in-one-crowd) | no | 255 in one fight hold 60 Hz today |

## How we work

| Rule | Why |
|---|---|
| A milestone is done when shipped content uses it and it survived a playtest, not when its test passes. From step 4 on, a playtest by hand every week (two games, 150 ms) is the gate | M230 to M232 were tested and nothing shipped used them until M236 and M242; the checklist by hand has waited since M235 |
| Features wait until [the slice](#4-the-slice) needs them | an engine with no game makes every feature generic, and a generic feature is never finished |
| One fact, one place | the bat's swing length is in three places (the clip, the sheet, the C++) |
| The pack is the source of truth | names declared in C++ make the editor wait for a server build |
| Standard words in the modders' docs | modders come from Roblox, Unity and Godot: client-side prediction, replicated field, ability, cooldown, hitscan |
| The docs say what the code does | a statement about today is read in the code before it is written; where they differ, the code wins (M261.5 fixed 19 stale lines, M261.6 more) |
| CI after every push | the determinism CI was red from M136 to M161 and nobody saw |

## The road

| # | Step | State |
|---|---|---|
| 0 | [Decisions](#0-decisions) | twelve taken (four after the review, eight since); eight open |
| 1 | [Stabilize M215 to M234](#1-stabilize-m215-to-m234) | unattended: clean (M235 to M242). **By hand: to do** |
| 2 | [Small fixes from the review](#2-small-fixes-from-the-review) | done (M243); the motion nodes' deletion goes to 3c |
| 3 | [Two network models](#3-two-network-models) | done (M244 to M250) |
| 3b | [Many players](#3b-many-players) | done (M251 to M261): 255 in one fight at 60 Hz |
| 3c | [Groundwork](#3c-groundwork) | **next**: from an outside review (2026-11-17), about a day each |
| 4 | [The slice](#4-the-slice) | **next**: an arena brawler (decided 2026-11-17) |
| 4b | [The alpha](#4b-the-alpha) | after 4: strangers on one official server |
| 5 | [Every item a sheet](#5-every-item-a-sheet) | next, beside the slice |
| 6 | [Sheets improved](#6-sheets-improved) | after 5 |
| 7 | [Timing from the clip](#7-timing-from-the-clip) | when the slice asks |
| 8 | [Body parts with priorities](#8-body-parts-with-priorities) | when the slice asks |
| 9 | [Cinderbox Studio](#9-cinderbox-studio) | after the alpha |
| 10 | [Server rules in Luau](#10-server-rules-in-luau) | after the alpha |
| 11 | [Avatars](#11-avatars) | after the alpha; in 1.0 (decided 2026-11-17) |
| 12 | [Godot's own, used](#12-godots-own-used) | one row done (M259) |
| | [Before launch](#before-launch) | open |

- **Why groundwork first**: four tests never ran, a malformed welcome aborts the client, and
  thread safety is held by comments. Each is about a day, and every later step leans on them.
- **Why the slice now**: steps 5 to 8 are what a fighting game leans on (predicted skills, timing,
  full-body moves), and the slice says which parts of them matter. Steps 9 to 11 are for modders
  and players, and are worth more once there is a game to mod and strangers who played it.
- **Why the slice came late** (said by the review): steps 3 and 3b (M244 to M261) went to network
  models and to 255 and 1023 players while the slice is an arena of a few to tens. Chosen on
  purpose (2026-10-28, 2026-11-03), and it bought headroom; from here the slice decides.
- **Why stabilizing is not finished**: unattended sessions catch crashes, desyncs and a replay
  that does not verify; what only a person sees (a swing that looks late, a camera that jumps)
  needs two games played by hand.

### 0. Decisions

**Taken:**

| Decision | Taken | Means |
|---|---|---|
| **The network model** (2026-10-28; the default since 2026-11-01) | the **hybrid** is the default: the physics deterministic, simulated by every client from the server's events, never sent as state; characters the server's, your own predicted, the rest drawn from puppets. The inputs model stays a server's choice | [step 3](#3-two-network-models) |
| **The sheet's file name** (2026-10-27) | **`.cbsheet`** (a language name gets taken: Ember is a programming language) | [step 6](#6-sheets-improved) |
| **How server-only rules are written** (2026-10-27) | **Luau** | [step 10](#10-server-rules-in-luau) |
| **Avatars' proportions** (2026-10-27) | **the standard skeleton only**: no body scales | [step 11](#11-avatars) |
| **One way to draw every character** (2026-11-06) | a look made from the character's scene, drawn through the RenderingServer, the viewer's own included; only what the look takes is drawn. Built in M260 | [Where it stands](#where-it-stands-m261) |
| **Stay on Godot** (2026-11-08) | the expensive parts are the engine's own C++; Godot is the renderer, the UI and the modders' free editor. Looked at again only if a measured target fails with the time in Godot's renderer and a GPU crowd cannot fix it | [routes not taken](#routes-not-taken) |
| **Every item drawn as data** (2026-11-14) | an item's look made once per kind; its reactions run from the compiled sheet; no node per item | [step 6](#6-sheets-improved) |
| **Pushers stay exact** (2026-11-06) | a pusher's place every tick, not every other tick filled in: that would change how they push | [docs/scaling.md](docs/scaling.md#the-plan-for-many-players-step-3b-2026-11-03) |
| **1.0's scope** (2026-11-17) | as proposed, with avatars in it | [What 1.0 means](#what-10-means) |
| **The slice's game** (2026-11-17) | an arena brawler | [step 4](#4-the-slice) |
| **Avatars in 1.0** (2026-11-17) | yes; the owner brings a few models when the step starts | [step 11](#11-avatars) |
| **The Hytale comparison's proposals** (2026-11-17) | 1 and 7 before launch; 5 in step 6, 3 with it; 2, 4 and 6 postponed and kept open | [From the Hytale comparison](#from-the-hytale-comparison) |

**Open:**

| Decision | Options | Leaning | Decides |
|---|---|---|---|
| **How players find servers** | Steam; a website's "play" link that starts the game; the game's own list; a desktop shortcut | decided later (the owner, 2026-11-17); needed for 1.0 | [parked](#parked) |
| **The players an arena is made for** | a few (a duel, 2v2) to tens | decided with the slice's map | [step 4](#4-the-slice) |
| **Cosmetic physics** | Jolt with a static copy of the level; no Jolt at all | decided later | [step 12](#12-godots-own-used) |
| **The slice's network model** | the hybrid (the default); the inputs model | chosen by playing both at 150 ms (`playtest.ps1 -NetModel inputs`) | [step 4](#4-the-slice) |
| **Both network models after the alpha** | keep both (the inputs model for small servers); keep one | after the alpha, from what the slice chose. Two models cost every feature twice (three client sessions: `game_client.h`; NetIds used up by hand to follow the server: `Simulation::ApplyCommand`) | [step 3](#3-two-network-models) |
| **Which servers 1.0 joins** | any; official or listed ones only | none yet: any server needs every decoder of server data fuzzed first | [before launch](#before-launch) |
| **Players' own avatars at 1.0** | uploaded by players; a curated set (the owner's models) | none yet: uploads need terms, moderation and budgets | [step 11](#11-avatars) |
| **The repository out of OneDrive** | a plain folder, GitHub as the backup; stay | none yet: `.git` and the worktrees sync through OneDrive (no damage found on 2026-11-17: `git fsck` clean) | [housekeeping](#housekeeping) |

`.cbsheet` and `.res`: a sheet made a Godot resource is not a `.res`. `.res` and `.tres` are
Godot's generic containers (binary and text) for any resource; Godot's own Shader and GDScript are
resources kept as plain-text files of their own (`.gdshader`, `.gd`) by loaders of their own. A
sheet is the same: a loader and a saver in the extension for `.cbsheet`.

### 1. Stabilize M215 to M234

A written checklist ([docs/playtest.md](docs/playtest.md)) of everything M215 to M234 added,
played together under latency and loss: `tools\playtest.ps1` starts the server, `cb_netsim` (a 150
ms round trip, 2% lost) and two games, records the session and verifies its replay at the end.
`-Autoplay SEC` lets the games play by themselves.

| | State |
|---|---|
| Unattended sessions (the default mods, the critter, waves, waves of 32 NPCs) | clean, after the fixes of M235 to M242 ([history](docs/HISTORY.md)); run on the inputs model, the default then |
| M230 to M232 in use | TimeScale and crossfade curves: the fist (M236). OneShot: the mannequin's flinch on `combat.hurt` (M242). A nested machine: the fist's guard (M242). Add2: kept for later (decided after M241: a lean or a breathing sway, once there are clips) |
| **The checklist by hand, two games** | **to do**, on the hybrid (`playtest.ps1`'s default since 2026-11-01) |

**Still open from the sessions:**

| Found | Goes with |
|---|---|
| Spamming use breaks the bat's and the guns' animations: their hand-written `predict` lines guess a use more often than the server allows one | [step 5](#5-every-item-a-sheet) |
| With brawl on, the unattended player never reaches the pistol or the bat (the kit takes the first slots) | the checklist, by hand |
| With the fist out the walk's arm swing does not show (the fist's pack owns the upper body) | the owner's own clips |
| HUD lines overlap in half-screen windows (the debug lines, the hotbar, the keys' line) | not a priority: the game is played full screen |

**Done when** the checklist passes by hand and every feature since M215 is used by shipped content
or gone (`CbCurve`, M207, is unused: deleted in [3c](#3c-groundwork); [step 6](#6-sheets-improved)
makes curves resources).

### 2. Small fixes from the review

**Done (M243)**: the gun's dead stance, colliding file names between mods, stale docs, the
unmaintained motion nodes out of the editor's favorites, every `Area3D` of a look made quiet in the
game, an old replay deleted. **Left**: the motion nodes themselves (`CbMotion`, `CbProbe`,
`CbImpulse`, `CbForce`, `CbLink`, `CbMove`, `CbLaunch`; no shipped mod uses them) are deleted in
[3c](#3c-groundwork); the bat's dead stance goes with [step 5](#5-every-item-a-sheet).

### 3. Two network models

**Done (M244 to M250).** The hybrid is `cb_server`'s default since 2026-11-01; the inputs model is
a server's choice (`--net-model inputs|hybrid`), and the welcome says which.

- **What it is**: characters read the physics world and never write it; what a character does to a
  body is a physics event; the physics world runs on every client from those events and the
  characters' pushers, a little in the past, each tick once, checked by its checksum. Characters are
  the server's: your own predicted, the rest drawn from its states. [DESIGN.md](DESIGN.md#netcode)
  has how; [docs/scaling.md](docs/scaling.md#the-two-network-models) has the two models side by side
  and the limitations accepted with the hybrid.
- **Lag compensation** (M250): a mod's cast from a player (a shot, a swing) and the player's own
  finds and probes (a punch, a hook) meet the others as that player saw them.
- **M244 and M245's server-state model was dropped**: it streamed the physics world as state
  ([routes not taken](#routes-not-taken)).

### 3b. Many players

**Done (M251 to M261).** The most players a hybrid server can hold, bandwidth first, then the
server's and the game's processor time. All of it, with every measurement, is in
[docs/scaling.md](docs/scaling.md).

| Players in one fight | The server's thread a tick | Each player, down | The game, all of them in view |
|---|---|---|---|
| 255 | 15 to 17.5 ms of 16.7 | 0.51 Mbit/s | 47 frames a second (a recording) |
| 1023 | 55 ms | 1.7 to 1.9 Mbit/s | 5 frames a second (a recording) |

- **255 fighting players fit**; each sends 34 kbit/s up (M261).
- **The targets set for 1000 in one area are not met** (the table in
  [docs/scaling.md](docs/scaling.md#where-it-stands-m261)); what is left is
  [later](#a-thousand-in-one-crowd), not in 1.0.
- **No fog of war**: every character is sent to every player, far ones about every 12th state.
  What each player is not sent: others' fields that are not shared, others' items put away, others'
  inputs. Cheating is decided [before launch](#before-launch).

### 3c. Groundwork

From an outside review of the code (2026-11-17), checked against it. Each is about a day.

| What | Why |
|---|---|
| The four tests that never ran registered (`motion_order`, `motion_regular`, `motion_amounts`, `motion_inspector`); CMake's list made from the C++ table, so the two cannot drift again | they are in `test_main.cpp`'s table and not in `tests/CMakeLists.txt`: `ctest` and CI never ran them |
| Two CI jobs on Linux: ASan and UBSan running every test; TSan running the `net_*` tests | thread safety is held by comments: a new thread for each state (`GameServer`'s `m_making`), `FinishStates( true )` in seven places, `ItemsOf` rebuilding a `mutable` index inside a const method |
| Reading a snapshot or a server's image fails with an error, never `std::abort()` | a malformed welcome aborts the client: `Simulation::LoadPortable` reads through `Reader::Take` and `DeserializeEcs`, which abort on short or wrong bytes |
| A `.clang-format` and a `.clang-tidy` (the `bugprone-*` checks); `-Werror` for our own targets in CI | |
| The motion nodes (`CbMotion`, `CbProbe`, `CbImpulse`, `CbForce`, `CbLink`, `CbMove`, `CbLaunch`) and `CbCurve` deleted; `BakeSheetMotion` moved to a file of its own first | no shipped content uses them; `BakeSheetMotion` (the sheet's bake) lives in `cinderbox_motion.cpp` beside them |
| The protocol's changelog out of `protocol.h` (`kProtocolVersion`'s comment) into the history | the code says what is; the history says how it came to be |

**Not here**: the splits the review asked for (`Simulation`, about 200 methods and a 7,009-line
`.cpp`; `GameServer`, 3,211 lines; functions of 400 to 1,200 lines) are made where steps 5 to 8
touch the code, not as one refactor ([housekeeping](#housekeeping)).

### 4. The slice

One game made with the engine and played by people. **Decided (2026-11-17): an arena brawler**,
like a Roblox fighting game: one map, one mode (rounds and a score), the brawl kit and the bat, a
ten-minute session that is fun. How many players an arena is made for is decided with its map.

| | Today |
|---|---|
| The mode | the `deathmatch` mod, on by default: a round ends at 10 kills or 5 minutes; a scoreboard |
| The kit | the `brawl` mod: fist, dash strike, uppercut, ground slam, guard, as hotbar skills; all predicted from sheets |
| The bat | the `melee` mod: each life starts with one (the shop sells another); C++ with a hand-written prediction ([step 5](#5-every-item-a-sheet)) |
| The map | `test_ground` and `example_arena` are test maps; no map is made for the slice yet |
| Players | by hand with two games on one machine; no other testers yet |
| The network model | not chosen for the slice. The hybrid is the default; [docs/scaling.md](docs/scaling.md#the-two-network-models) calls the inputs model the fighting games' (10 to 30 players). In the hybrid your punch moves another player a round trip and the buffer late; in the inputs model at once, every client simulating everyone |

- **First**: both network models played by hand at 150 ms (`tools\playtest.ps1 -NetModel inputs`,
  then the default), the brawl kit against each other; the slice takes the one that feels right.
- Built from now on, beside steps 5 to 8: each of those is done when the slice uses it.
- Played every week, with latency. What it runs into is the next milestone.
- **Done when** it is fun in our own sessions by hand; then [the alpha](#4b-the-alpha).

### 4b. The alpha

Strangers play the slice on one official server, every week. Steps 9 to 11 come after it.

| Needs | Why |
|---|---|
| A build players download, and a way to get the next one | the server refuses any other protocol version (`kProtocolVersion`, 92 so far): every change to the protocol locks out the old game |
| The official server's address handed out (a link, or typed) | how players find servers is decided later; one server needs no list |
| Only the official server joined | a stranger's server is not safe to join until [before launch](#before-launch)'s decoders are fuzzed |
| A place to hear back from players | what they run into is the next milestone |

**Done when** strangers play sessions and come back for more.

### 5. Every item a sheet

The fist's way for every item: the item's sheet runs its use in every simulation (predicted), and
the mod's C++ turns its hit events into damage (`brawl.cpp` is the model, 158 lines).

**The bat, today**:

| | |
|---|---|
| The swing | `melee.cpp` (272 lines): five rays fanned out from the chest at the clip's `melee.strike` marker; the hit test poses the target (hit zones) |
| The look's guess | `predict use as melee.swing`, `cooldown 1.1` copied from the clip, `stance melee_swing on full` (read by nothing) |
| Its length | three places: `melee.done` at 1.1 s in the clip, `cooldown 1.1` in `bat.rules`, `kCooldownSeconds 0.6` in `melee.cpp` |
| `melee.hot` | a field on the bat, set by the server when a swing hits a player (`ctx.Set`), cleared by the mod's own timer 2 s later. Not predicted; a prop hit does not heat it |
| NPCs | cannot swing it: `melee.cpp` loops over player slots. The fist works for NPCs because sheets run for every driver (M228) |
| Its sheets | `bat.rules` in the bat's scene (the glow, the flames, the sparks, the guess); `reactions_melee.rules` in a world reactions scene (the sounds) |

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

- `melee.cpp` turns `melee.hit` into `combat.damage`: about 60 lines. NPCs swing it.
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
written and checked, and, since its format is settled at 1.0, it changes here.

| Change | Why |
|---|---|
| **First: the sheet compiler a plain C++ library**, with no Godot types, as `src/expr` is. Today a sheet is read twice: `ParseRules` (1,198 lines, `src/godot/cue/cue_rules.cpp`) reads it into Godot Dictionaries in the editor, `BakeSheetMotion` (`cinderbox_motion.cpp`) writes a second text format, and every simulation parses that (`CompileMotionSet`, 957 lines, `src/sim/motions.cpp`) | the server then reads sheets itself (and their names: the pack is the source of truth); sheet tests run headless; the formatter and errors as you type share the one parser |
| **A resource on the node it is about**: `CbSheet`, read and written by the extension's own loader and saver, plain text in a `.cbsheet` file, as `.gd` and `.gdshader` are. A property on the node: `CbItem.sheet`, `CbMotionSet.sheet`, the world reactions root, the HUD root; the node says which blocks it allows. It may be saved inside the scene, as a built-in script is. **`CbRules` goes** | `CbRules.file` is a plain string: no UID, a rename breaks it, Godot does not see the dependency. Saved inside the scene, the notes stay with the item (Godot drops a scene's comments) |
| **Not Godot's script slot** | the security story is "every script is refused"; "every script but ours" is a rule someone will probe, and a script language through GDExtension is a large, fragile API for two features (errors in the gutter, completion) |
| **Where each block runs, said**: the simulated blocks and the viewer's in marked sections (or files of their own), one indenting rule (it counts) | four languages in one file: `motion` and `use` (simulated, indenting counts), `on` and `while` (the viewer's reactions, indenting for the eye), `predict` (the viewer's guesses), `show`, `text` and `bind` (HUD) |
| **Names in the sheet**: `field brawl.combo int`, `event brawl.jab.hit`; the server reads them from the pack | the editor waits for a server build |
| **Parameters**: `param damage 8`, shown in the item's inspector, read by the server's rules | numbers written twice (the bat) |
| **One sheet for many items** (from the Hytale comparison, proposal 5): a sheet shared across items, each item with its own parameters or replacing named parts of it | `fist.rules` writes the same `find` block four times |
| **Words for how skills meet** (proposal 5): `blocked by` (which motions block or interrupt which), a combo with a window, a charge (how long it is held decides what it does), tap against hold, a cooldown shared by name | the fist guards two motions with `not motion.brawl.fist.Finisher`; brawl's combo window is a field a `while` motion counts down |
| **Text and rows from the server** (proposal 3, with the HUD lines; its shape decided when the step starts): rows a mod publishes that a `CbList` lists, text fields sent the way names and private fields are (outside the simulation's hash); the layout stays the look's | fields are numbers; the shop's offers are written twice (`hud_shop.tscn`, `kOffers` in `shop.cpp`) |
| **A motion's state readable**: its cooldown left, its charges, whether it is on, in conditions and HUD lines | brawl rebuilds its cooldowns and combo window: five fields declared in C++, five `while` motions counting them down or resetting them, while a motion's own `cooldown` goes unused |
| **Rates said**: `change x -= 1 per second` | `change x -= 1` is once in a step and per second in a `while` |
| **One rule for expressions**: one word bare, anything longer in brackets | an expression ends at "the next option word" (`impulse (distance < frag.radius) * ... along away` works because `along` is one) |
| **`let NAME = EXPRESSION`** inside a motion | the frag's falloff is written out twice |
| **A cast and the owner's own effects in one step** | `dash_strike.rules`: `use if brawl.cd_dash <= 0` and `motion Hit on use if brawl.cd_dash <= 0`, two blocks with a copied condition |
| **`change` on the item; a timer on a carried item** | the bat's heat, if it is ever more than a look |
| **Curves as resources**: `curve(res://falloff.tres, reach)` or a curves dictionary on the node; the frag's blast falloff the first user (the `CbCurve` node is deleted in [3c](#3c-groundwork)) | no shipped sheet uses `curve()`; a node found by its name from text |
| **The planned words**: `$target:FIELD`, `at POINT`, a cast in a later step ([the list](docs/rules-language.md#planned)) | |
| **Tools**: a formatter; errors in the editor as you type; sheet tests run headless (a motion on a stand-in, then what moved) | |
| **`.rules` becomes `.cbsheet`**: 40 sheets and about 80 files that name `.rules`; `docs/rules-language.md` with them; a `.gitattributes` line and a small VS Code grammar | the name says what the file is, one family with `.cbmap` |
| **Every item drawn as data** (decided 2026-11-14; not needed by the slice, so last in the step): an item's look made once per kind from its scene, as a character's is since M260 (`ItemLook`): its meshes, its materials (an item's own copy where a rule changes one), its particles (the RenderingServer's: `GPUParticles3D` is a thin wrapper over them, its resources reused), its lights, its markers, its own animation's tracks on those; drawn with no node, for every item: another's, your own, one lying in the world, your first person's. Sounds and the scenes a reaction spawns stay nodes, pooled, only while they play. What the look does not take is said at the bake | a bat is 15 nodes in the game (11 in its scene, 4 its sheet's reactions; only items that just look are drawn without nodes, M260): at 255 fighting players bats are half of the scene's 7,900 nodes; at 1023 (a recording) making and freeing what comes into sight, held bats among it, is 68 ms of a 184 ms frame |
| **Reactions run from the compiled sheet** (the step's compiler, run in the presentation; last in the step, with the row above): one compiled sheet per kind, run for each entity that has it in the presentation's flecs world (the mirror): its `on` rules on cues, its `while` rules on its state, a little state per entity (cooldowns, what a `while` started), their effects on the look's parts by index. Hand-placed `CbReaction` nodes stay for authoring and the Cue Preview | four reaction nodes for every bat in sight; the logic is data, only its effects are Godot's |
| **Not taken: MultiMesh for items** | one draw for every copy of a mesh, but culled as one, its shadows all or none, and a per-item glow needs a shader that reads per-instance data, which a modder's material does not. Kept for thousands of one thing (a GPU crowd of far characters) |

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

**Considered and not taken**: a visual graph ([routes not taken](#routes-not-taken)); a general
deterministic VM ([later](#a-deterministic-vm-for-heavy-predicted-logic)).

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

**Today**: a character's own layers (the mannequin's `FullBody`, `UpperBody` and `Flinch`; the
critter's `FullBody`), which a pack replaces by name; four overlay slots after them, filled in the
order a player's items were carried, for a pack's layers the character does not have; bone filters
that are on or off; two special rules ("what the hands hold wins the arms", "a hand an overlay
names lets go").

**Short for a fighting game**: most of its skills are full-body (an uppercut, a slam, a dash
strike), and a full-body move is an overlay, ordered by which item was picked up first. A filter's
hard edge at the spine shows when a swing plays over a run.

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
| `EditorExportPlugin` | bake, check and pack on export: replaces `tools/pack_mod.ps1` and `tools/publish_mod.ps1`, on every platform |
| `EditorDebuggerPlugin` | the F1 motion lines, fields, events and rollbacks in the editor while the test game runs |
| `EditorScenePostImportPlugin` | a dropped `.glb` becomes a `CbCharacter`: bone map, hitboxes, sockets, a starter tree (`make_mannequin.gd` does it as a script) |

| Also | What |
|---|---|
| One authoring project | every mod in a folder of its own (`res://mods/<id>/`); the validator refuses files outside it. Today there are 21 Godot projects (the game, the SDK, 17 server mods' looks, a character, an example client mod) sharing one `res://` when loaded |
| One Play | save, bake, start a local server and the game with this mod, reload on save: Test mod, on every platform |
| Starters by kind | an ability, a weapon, a game mode, a character, an avatar, instead of one starter with everything |
| From "authoring, finished" | names offered inside expressions and sheets; an item used in its preview; the preview with someone else in it (a hit) and the character's own mesh; a change the server reads restarts the test |
| The modders' docs | in standard words ([How we work](#how-we-work)) |

### 10. Server rules in Luau

**Decided**: Luau. Today the workshop carries a mod's look, and its rules mean building the engine.

- **Luau**: what Roblox developers write; made to be sandboxed (no files, no OS, a limit on
  instructions); fast. Rules run on the server only, so they need no determinism.
- The same verbs as today's C++ (`Declare`, `Start`, `Tick`, the `Context`'s reads and commands),
  from a script in the mod's item: a server runs items from different authors without a build, and
  a script reloads on save.
- A script's error stops its mod, not the server.
- **C++ stays** for engine mechanisms and a mod too heavy for a script (compiled in, as now).
- **What is predicted stays in sheets**: a Lua VM's memory cannot be copied cheaply for rollback; a
  sheet's state is 28 bytes a motion.
- The shipped mods move over in the order the slice needs them.
- Rules travel through a store only once the sandbox holds ([before launch](#before-launch)).

**What a mod reaches:**

| | C++ mod (the owner's build) | Luau script (from anyone, no build) |
|---|---|---|
| Reads | the `Context`'s reads today; more of the engine opened to it, read-only, as mods ask: the server owner chose to compile it in | the `Context`'s reads, grown as mods ask |
| Writes | commands in the tick's frame, never the simulation directly (`mod_api.h`): every client follows the simulation from them | the same commands |
| Its API | may change with the engine: it is built with it | narrow and versioned: settled at 1.0, so a script made for 1.0 runs on later servers |
| Cost | native | measured: deathmatch ported first, its time a tick at 64 players |

Why writes are commands for both: the server's simulation is not the owner's alone. Every
client runs the same simulation (the hybrid's physics from the server's events, everything in
the inputs model), so a write behind its back desyncs every player.

### 11. Avatars

**In 1.0 (decided 2026-11-17).** The owner brings a few models when the step starts: they are what
it is built and tested on. The body and its appearance kept apart:

| | The body: one for everyone | The avatar: one per player |
|---|---|---|
| What | the standard humanoid `CbCharacter`: the skeleton with Godot's humanoid bone names, the state machine, hitboxes, capsule, sockets, camera | meshes skinned to the same bone names, material colors, accessories |
| Who knows it | the server and every simulation | clients only |

- **The standard look with the avatar's meshes**: since M260 a body is drawn from its character's
  look (`CharacterLook`); an avatar is the same skeleton with other meshes, materials and
  accessories on sockets, a look of its own, drawn the same way. Colors as instance uniforms.
- **The avatar never changes what the server sees**: everyone's hitboxes are the same (fair), and
  one set of clips and poses serves everyone.
- **Decided: the standard skeleton only**, no body scales (Roblox's R15 way: held items and
  hitboxes match the drawing). An avatar made on another rig is retargeted to it when it is
  published (assets ship baked).
- **An avatar never blocks joining**: today a workshop item a player cannot find or fetch refuses the join. An avatar is
  fetched in the background, and the mannequin is drawn until it arrives.
- **Budgets checked when publishing**: triangles, materials, texture sizes.
- **Loading**: packs cannot be unloaded and share `res://`, so avatars as packs would grow memory
  and collide. To prototype: each avatar one self-contained baked scene, loaded from `user://` by
  its hash, not mounted; freed when nobody wears it.
- **Which avatar a player wears**: a value per player for looks only, not in the simulation's
  hash, sent as names are.

### 12. Godot's own, used

| Godot's | For |
|---|---|
| `SpringBoneSimulator3D` | tails, hair, capes: cosmetic, nothing that can be shot. Since M260 nothing of a character's scene runs in the game, so a spring bone becomes something the look takes (its chain and settings), run on the drawn skeleton |
| Instance uniforms (`instance uniform` in a shader) | a look per entity (heat, a team color, a hit flash) without a material per entity: the bat copies its material to glow |
| Threaded loading (`ResourceLoader.load_threaded_request`) | packs and characters while joining |
| `MultiMeshInstance3D`, visibility ranges | debris; a GPU crowd of far characters ([later](#a-thousand-in-one-crowd)) |
| Animation detail by distance | **done (M259)**: far bodies posed less often, out of view rarely; drawing only, hit tests unchanged |
| Translations | a sheet's HUD text is English in the file |
| `Theme` resources | client mods restyling the UI |
| **Cosmetic physics** (decided later) | nothing of the simulation is in Jolt: every Cinderbox authoring node is a plain `Node3D`, collision is Box3D's, and a look's `Area3D`s are made quiet. So a cosmetic shell casing would fall through the floor: Jolt gets a static copy of the level when the map loads, from the same shapes; or Jolt goes |
| `PhysicalBone3D` | ragdolls for any skeleton ([later](#ragdolls-for-any-skeleton)) |

## From the Hytale comparison

[docs/hytale-comparison.md](docs/hytale-comparison.md) (2026-11-11, checked against the code at
M261) found Cinderbox built on Hytale's model (rules on the server, one client no mod changes,
prediction from data) and proposed seven things. **Decided by the owner (2026-11-17):**

| # | Proposal | Decided |
|---|---|---|
| 1 | **A server may refuse players' own packs**: a player's packs load after a server's items and may replace any file of a look, a character's scene included (see-through walls with no cheat program); nothing lets a server say no | **taken**: [before launch](#before-launch) |
| 2 | **The camera as data**: a server's camera rig (distance, top-down, side view, a cursor) instead of the game's four views | **postponed**, [kept open](#postponed-kept-open) |
| 3 | **Text and rows from the server**: text in a label's format and rows of a mod's own, the layout still the look's | **taken with [step 6](#6-sheets-improved)**, its shape decided there |
| 4 | **Sheets on things in the world**: doors, buttons, jump pads, trigger regions as sheets on map templates | **postponed**, [kept open](#postponed-kept-open) |
| 5 | **One sheet for many items, and words for how skills meet** | **taken**: [step 6](#6-sheets-improved) |
| 6 | **A game as one file**: the map, items by hash, options, the network model, the pack policy | **postponed** with how players find servers, [kept open](#postponed-kept-open) |
| 7 | **What an item was made for; rules from a store only after the Luau sandbox; shaders in packs decided** | **taken**: [before launch](#before-launch) |

**Not taken** (the comparison's reasons): a UI the server pushes change by change, assets sent on
join, Hytale's sync of predicted chains, a fixed set of keys.

### Postponed, kept open

Each postponed proposal, when it comes back, and what work before then must not close off. A change
that would go against one of these says so in its milestone.

**2. The camera as data.** Comes back after the slice, or when a mod first needs a view the game
does not have (top-down, a side view, a kill cam); the leftover "cameras a mod places" goes with it.

| Keep | Why |
|---|---|
| The views in one place: `ViewMode` (`src/sim/types.h`) and `godot/view_camera.gd`. A new view goes there, never as a camera a look carries (a look's cameras stay removed: `RemoveCameras`) | a rig from the server then replaces one place |
| Where a line of sight starts in one function (`Context::ViewPosition`), its numbers in named constants (`kShoulderOffset`, `kViewPivotHeight`, `kEyeAhead`, `kEyeUp`) | a rig's numbers, baked into the schema, feed it instead: the server must know where a player's aim starts |
| `PlayerInput::view` a number, not flags | a server's own rigs are numbered after the four |

**4. Sheets on things in the world.** Comes back after step 6, or when the slice's arena wants a
door, a jump pad or a trigger region.

| Keep | Why |
|---|---|
| Step 6's sheet a resource on a node that says which blocks it allows (`CbItem.sheet`, `CbMotionSet.sheet`, ...) | a map template becomes one more such node, not a new mechanism |
| Step 6's compiled sheet run per entity, not per player slot | a door, a button or a region is an entity, and has no slot |
| A motion's triggers an enum that grows (`Motion::When`: Press, While, Event, Edge) | entering a region, being used, being hit are more triggers |
| "Use what you look at" one intent (`SlotIntent::Open`, a container's today) | it grows to anything in the world a sheet says can be used |

**6. A game as one file.** Comes back with how players find servers: what a play link, a host and
Test locally would open.

| Keep | Why |
|---|---|
| Every setting of a game a named option (`--mod-option NAME=VALUE`, read by `ctx.Option`), not a constant in a mod | a game file is then a list of them |
| A server's policy on players' packs (proposal 1) a setting like the others | the file carries it |
| Workshop items named by their hash | the file lists them, and any copy of it gets the same game |
| Test locally and the game's host start `cb_server` from options | they open the file later instead |

## Before launch

| What | Why | State |
|---|---|---|
| **The pack loaders fuzzed, files limited in size** | workshop items are Godot files on every player's machine; the guard and the validator check what is in them, not Godot's parsers | not started |
| **Everything a client reads from a server fuzzed** | a client loads the server's whole world when it joins: the simulation's image (the ECS state, and Box3D's own world bytes through `b3DeserializeIntoShell`), the schema, the map, the sheets' text; then states and frames. A malformed welcome aborts the client today ([3c](#3c-groundwork) makes it an error). Every decoder fuzzed, handles checked after loading. Or 1.0 joins official or listed servers only ([step 0](#0-decisions)) | not started |
| **Terms for modders, a licence for mods** | `LICENSE` is "all rights reserved": nothing lets a modder use the SDK, or says what becomes of what is uploaded (avatars included) | not started |
| **Updates** | how a player gets the next build: a server refuses any other protocol version | needed for [the alpha](#4b-the-alpha) |
| **Cheating, decided** | inputs model: every client holds the whole world. Hybrid: every character is sent, far ones about every 12th state, so a client knows where everyone is; what is not sent is others' unshared fields, their items put away and their inputs. Wallhacks are trivial in both; fog of war for characters is possible in the hybrid (what is not sent cannot be seen), not built | open |
| **A server's policy on players' packs** (proposal 1) | a player's packs may replace any file of a look; the server says none, listed hashes, or any (Counter-Strike's `sv_pure`) in its info (`MsgInfo`), and the game loads only what it allows | decided 2026-11-17; to do |
| **What an item was made for** (proposal 7) | baked motions and graphs carry a format version, an item does not: it says nothing of the engine and Godot versions it was made for or the names it reads | decided 2026-11-17; to do |
| **Shaders in packs** (proposal 7) | the validator refuses scripts only; a pack's shaders run on every player's GPU. Refused, or allowed in a checked subset: decided with the loaders' fuzzing | decided 2026-11-17; to do |
| **A mod's crash** | takes the server down: mods are compiled into its process | [step 10](#10-server-rules-in-luau) |
| **Rules from a store** (proposal 7) | only once the Luau sandbox holds | [step 10](#10-server-rules-in-luau) |
| **How players find servers** | join by address only | [parked](#parked); needed for 1.0 |
| **Compatibility, promised** | what 1.0 keeps (the sheet, an item, the rules' API); a protocol of another version is refused at join today, as it should be; a format's change after 1.0 is versioned and said | to write |
| **The manual audited** | M261.5 fixed 19 stale lines (DESIGN, README, `docs/looks.md`, `mods_src/README.md`) and M261.6 this roadmap, each read against the code; `docs/characters.md`, `items.md`, `motions.md`, `maps.md`, `testing.md`, `playtest.md`, `server-mods.md` and `sdk/README.md` have not been read whole against it | to do |
| **Translations, tools on every platform** | the scripts are PowerShell; the extension is built from source | steps [12](#12-godots-own-used), [9](#9-cinderbox-studio) |

## Later

In the order they would likely come back.

### A thousand in one crowd

1023 players run in one crowd today, below 60 Hz ([docs/scaling.md](docs/scaling.md)). What is left,
each measured:

| Where | What is left |
|---|---|
| The server's thread (55 ms a tick at 1023) | the mods' loops over every player (inventory, frag, the layers: about 2 µs a player each); the motions' pass and the slots on one thread; keeping a state (its serialization); the physics |
| Planning states (on the state threads) | each client walks every entity: an interest grid would cut it where players are spread out (in a crowd every one is near) |
| Bandwidth (1.7 to 1.9 Mbit/s a player) | frames are 70%, most of them pushers' places (exact every tick: decided); far characters coarser |
| The game (5 frames a second with all in view) | held items with reactions made as they come into sight (step 6 draws every item as data); the mirror's and the HUD's work for a thousand others; Godot drawing a thousand skinned characters (a GPU crowd for far ones) |

Not in 1.0. The cheap way to more players is many servers of 50 to 255 per game, as Roblox scales a
game.

### Our own animation authoring, apart from Godot's

Decided after M231: when the time comes, the animation the simulation runs gets an authoring
mechanism of its own, apart from Godot's, and Godot's `AnimationTree`s are left for what is only
seen. [Body parts with priorities](#8-body-parts-with-priorities) would be its core. Much later;
it needs more decisions first.

### Ragdolls for any skeleton

A skeleton without a humanoid's bones falls as no ragdoll today (the critter dies without one).

| Piece | What |
|---|---|
| Authored on the scene | Godot's own way: `PhysicalBone3D` nodes on the skeleton (their shapes and joints), baked into a ragdoll the simulation builds for that character, as it builds the humanoid's eleven bodies |
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
Citizen took about a decade); many servers per game comes first.

## Parked

Decided to wait; each says what it waits for.

| What | Waits for |
|---|---|
| How players find servers | a decision, taken later (the owner, 2026-11-17): Steam, a website's "play" link that starts the game, the game's own list, a desktop shortcut. Today: join by address, Test locally, a server's mods listed before joining. **Needed for 1.0**; [a game as one file](#postponed-kept-open) comes back with it |
| Steam Workshop as the store | the decision above; the store is a folder today |
| Ships | Box3D growing what they lack (buoyancy) rather than more patches to it (five in `cmake/patches/`). In the simulation, so everyone predicts them: platform carry (vehicles), a buoyancy component with a wave function the water's look shares |
| **The player's body, for motions** | a mod that wants it; each a word of the sheet language ([its planned list](docs/rules-language.md#planned)), in the simulation, so a motion using it is predicted. In the order they would likely be wanted: |
| · Touching a wall | a condition (`wall`: touching one, beside or ahead) and a frame along it for forces and impulses: wallrunning, wall jumps, a ledge grab |
| · Teleport | a motion effect that sets where the player is: to the target at an offset in its frame (behind it), or to a cast's point. Blink, "behind you", swapping places |
| · What the body passes through | movement parameters `pass_players`, `pass_props`, `pass_level`: a dash through the target, phasing through crowds, a spectator flying through walls |
| · Not hittable | a movement parameter `hittable`: shots, casts and swings pass the player's hitboxes. A dodge's invulnerable frames (or a [window](#7-timing-from-the-clip)) |
| · The capsule's size | movement parameters for its height and radius, so a crouch fits under a low gap; the feet stay, and it does not stand up where the full height would not fit |
| · Keep speed, steer | a motion effect that turns the velocity without changing its size: a glide, a grind rail, a ricochet |
| · Hidden from others | a field the viewer reads to fade a body (and what it holds) for everyone but its player: invisibility, a cloak, a spectator |

## Leftovers, by area

Not scheduled. A row moves up into the road when the slice wants it.

| Area | What |
|---|---|
| Physics | on CI's macOS build (arm64), now and then a float in Box3D's contact data (most likely a contact point's `totalNormalImpulse`) differs between two simulations that should be the same, every body the same bit for bit; Windows and Linux are the same byte for byte. `net_physics_alone` reports it there instead of failing. Expected to go with a newer Box3D, or looked at again if it ever moves a body |
| Items | a state machine or a motion reading the hand's fields; a lifetime for an item that was not launched; a fuse that starts when the pin is pulled (a cook); bots that use what a mod adds; a bot count in the menu |
| The inventory | a split of one; a slot's item shown while dragged over; a key for the bag's slots; dropping or moving part of a stack; a mod taking one off a stack without throwing it; a corpse's bag (the ragdoll carries it, or a box where it fell: undecided); predicted answers to a mod's intents; numbers wider than a byte in an intent (an intent carries two) |
| Sheets and HUDs | a sheet for a list's rows; an item's HUD drawn on its slot |
| Motions | a lasting force as a C++ command; a test of one player hooked to another; a knocked-down ragdoll state; carrying a player |
| Animation | a slow-motion finisher (a state's speed is in, M230); the dash's and the hook's poses are placeholders |
| Feet on the ground | foot IK in the shared pose, beside the aim chain: each foot's ground found by a ray in the simulation, the hips lowered to the lower foot, two-bone IK per leg, so the legs' hitboxes are where the feet are drawn |
| The client | keys the server says: a server maps keys to what they do, an item's own key included, sent once at join; a player may still rebind (per server). Cameras a mod places (a round's end, a kill cam): with [the camera as data](#postponed-kept-open) |
| Games | teams and spectators (mods, once private fields and the board cover them); lists on one entity (array fields, or a `CbList` over a field family) |
| Finding the way | one mesh per character size; a mesh mended when a wall breaks; off-mesh links (a jump, a ladder) |
| NPCs | NPCs of a character of their own; a brain in a sheet (predicted exactly, at a rollback cost) |
| The uplink | an input packet every other tick would take a player's 34 kbit/s up to about 20 and delay inputs 8 ms on average: not taken (M261) |

## Decided earlier (still holds)

| Decision | Why |
|---|---|
| Possession, not an NPC type | a player is a controller (its keys, name, score); what it drives is a body. NPCs, spectators, Prop Hunt and mind control fall out of it |
| The controller is a part, not a kind | a body nobody drives costs what a prop costs; the controller's pass meets only driven bodies |
| What a body's scene says decides how it is driven | its capsule, a first-person camera, a skeleton and state machine, its movement numbers |
| The mover stays C++ | the hottest loop (every player, every tick, again in every rollback); sheets are bounded on purpose |
| Walls static until broken; debris players cannot push (`CatDebris`); settling by Box3D's own sleeping | a wall costs nothing in rollback; turning sleeping pieces static would save next to nothing |
| The ozz pose owns the body | hit tests use it, and the game draws exactly it (since M260 nothing of a character's scene animates a body in the game) |
| Physics players react to is shared | ragdolls and falling walls are the same on every screen; cosmetic physics is for what nobody could compare (capes, spring bones, casings) |
| The UI stays presentation-only | a screen shows fields and sends intents; what it cannot say becomes a widget the engine gives, not a script |
| Mods' files never cross the game connection | a server names the workshop items it needs by hash; a client finds them in its store |

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
| Another engine (2026-11-08) | the simulation, the netcode, the animation and the presentation's logic are the engine's own C++; Godot's part (the renderer, the UI, the modders' editor) is what a switch would lose. The scene tree's cost at scale was removed instead (M260) |
| Sheets in Godot's script slot | "every script is refused" is the security story; a script language through GDExtension is large and fragile for two features ([step 6](#6-sheets-improved)) |
| Sheets as `.res` or `.tres` | Godot's generic containers would hold the sheet as one quoted string: poor diffs, no editing as text |
| A visual graph for skills | hard to diff and review; Godot dropped VisualScript for lack of users |
| A three-letter extension | gains nothing (Godot's own are long: `.gdshader`, `.gdextension`); the `cb` three-letter names are taken: comic archives (`.cbr`, `.cbz`, `.cbt`, `.cb7`), Code::Blocks (`.cbp`), COBOL (`.cbl`) |
| Naming the sheet language Ember | already a programming language's name |
| Body scales for avatars | the standard skeleton only: held items and hitboxes match the drawing, nothing to clamp |
| Everyone simulating the whole world as the default | the rollback window's cost grows with every body; kept as a server's choice for small servers |
| The server's state as the second model (M244, M245) | it sends the physics world as state: bandwidth grows with every moving body, a destruction is hundreds of them at once, and the determinism the project is built on goes unused. The hybrid keeps the physics deterministic and sends only its events |
| Tiers of drawing (a full scene for the nearest players, something cheaper for the rest) (2026-11-06) | one way for every character, by distance only in detail (M260) |

## Housekeeping

Independent of the road; each is small.

| What | Note |
|---|---|
| The rest of `simulation.cpp` in modules | about 7,000 lines and about 200 methods on `Simulation`; the mover went first (M92). Split where steps 5 and 8 touch it: slots and items, motions, possession, breakables, sending states. `GameServer` the same (3,211 lines: connections, the schema, the mods, states, the bench) |
| Long functions | `ParseRules` 1,198 lines (step 6 replaces it), `CompileMotionSet` 957, `CbCharacter::BakeGraph` 765, `Simulation::ApplyCommand` 529, `GameServer::Start` 497, `Simulation::MoveCharacters` 433: table-driven parsers, one builder for the schema |
| The bench inside the server | `game_server.cpp` names the bench or fake players 132 times: a profiler interface and a fake transport |
| Box3D's pin | a commit 21 past its `v0.1.0` tag, with five patches: stays pinned through 1.0; an upgrade is a milestone of its own |
| `tests/test_main.cpp` in files by area | about 13,800 lines in one file; `tests/net_test.cpp` about 8,400 |
| Replays' extension | `.cbr` is also the comic-book archive (a double-click opens a comic reader): `.cbreplay` |
| A motion's field and a mod's | both may write one (commands first, then motions); the server could warn when a mod sets every tick a field a motion writes |
| Slots for what is worn | the dash boots take a slot: selected, they are held like a tool |
| `reconcile_every` 2 as the default (inputs model) | halves a client's rollback work with many players and latency (M139); others are seen up to 2 ticks later: to be looked at in play first |
| Physics threads beside drawing | on by default (M141), not yet watched in the game: `--physics-threads=0` if frames suffer |
| The old `build/` folder in the main checkout | builds go outside the source tree; it is ignored and can be deleted |
| Old copies of the extension in `godot/bin` | `~libcinderbox...TMP` files, left by rebuilds while Godot had the library open: git-ignored (21 in the main checkout, about 530 MB), can be deleted |
| The view codec's magic | `CBV7` since M252. M260 made an entity's record 36 bytes longer (what a player carries) and bumped the view file's version (10), not the magic. Nothing breaks today (frames cross between the two extensions of one build; a view file checks its version), but the magic should change with the record |
