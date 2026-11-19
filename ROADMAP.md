# Cinderbox — Roadmap

The road to launch: what comes next, in order, and why. **Launch** is the day Cinderbox opens to
people outside the project ([the goal](#the-goal)); the plan names no version number.

| Read with | For |
|---|---|
| [DESIGN.md](DESIGN.md) | how it works today |
| [docs/HISTORY.md](docs/HISTORY.md) | every milestone done, newest last |
| [docs/scaling.md](docs/scaling.md) | how many players a server holds, and every measurement behind it |
| [docs/hytale-comparison.md](docs/hytale-comparison.md) | what Cinderbox could take from Hytale's mods and plugins ([what was taken](#from-the-hytale-comparison)) |

Rewritten after M261 (M261.6), an outside review's findings added (M261.7), shortened (M261.8); a
second review, the pack check's hole and the owner's decisions on cooked items added (M261.9);
the macOS test's run made steady (M261.10); the repository moved out of OneDrive (M261.11); the pack
check tightened (M262).
Statements about what exists were checked against the code at `3d67daf` (M261.8's merge); what is
planned says so. Detail of finished steps is in the history.

## Where it stands (M261)

| Area | Today |
|---|---|
| The simulation | flecs and Box3D, deterministic on six builds (Windows: Clang, GCC, MSVC; Linux: GCC, Clang; macOS arm64: Clang); CI compares snapshots and the pose hash across all of them |
| Netcode | an authoritative server, two network models (the server's choice). **Hybrid** (the default since 2026-11-01): every client steps the physics from the server's events, never sent as state; other characters are puppets of a few bytes; your own is predicted; lag compensation. **Inputs**: every client simulates everything from everyone's inputs. Replays, view files, bots, a network simulator, a chaos test |
| Scale | 1023 players a server; 255 fighting in one crowd hold 60 Hz on the server and draw at 47 frames a second; 1023 in one crowd run below 60 Hz ([docs/scaling.md](docs/scaling.md)) |
| The client | Godot 4.7 with the engine as a GDExtension; draws whatever a source hands it (a server, a replay, a view file) and runs no mod's code (packs are checked before Godot loads them: Looks, below); every body drawn one way, through the RenderingServer (M260) |
| Bodies | a character per body, any skeleton, possession of a body or a prop, NPCs a mod drives on the server's navigation mesh |
| Animation | Godot AnimationTrees baked to a deterministic subset (blend spaces, TimeScale, crossfade curves, OneShot, Add2, nested machines) that the simulation runs; packs and overlays items bring; far bodies posed less often (M259) |
| Rules | 24 server mods in C++, compiled into the server, 18 on by default. What moves bodies is in sheets every simulation runs, so it is predicted: brawl, dash, flight, grapple, frag, grenade, cannon. The bat and the guns are still C++ with hand-written predictions |
| Looks | reactions, predictions and HUD lines in sheets; workshop items are Godot packs, checked by a pack validator and a scene guard. The validator was bypassed by an escaped script name (found in M261.9); tightened in M262. Godot's parsers still read strangers' files until [6b](#6b-cooked-items) |
| The world | props, destructible walls, items, containers; maps baked from Godot scenes |
| Modding | a Godot project per mod, made from the SDK; Test mod (build, publish, play in one step); a look published again is taken up live; workshop items found by hash, never sent over the game connection; the store is a folder; the editor knows a mod's names after a server build |
| Tests | 180 tests in `ctest`, all passing, and 29 Godot checks (`check_*.gd`); the determinism CI after every push (M261.8's merge went red on macOS: `net_brawl_lag_compensation` short of strikes there, made steady in M261.10). Every test in the runners' tables is registered, read from the tables themselves (M263) |

## The goal

**Decided (2026-11-17).** Launch is when people outside the project can build on Cinderbox: players
find a server and play a game made with Cinderbox, wearing avatars of their own, and modders make
and publish mods, rules included, without building the engine. Strangers play earlier, in
[the alpha](#4b-the-alpha), right after the slice: what they run into shapes the rest.

**Ready to launch when:**

| | Step |
|---|---|
| Strangers play the slice and want another session | [4](#4-the-slice), [4b](#4b-the-alpha) |
| A mod's look and rules are made, tested and published with the prebuilt SDK, and run on any server without a build | [6b](#6b-cooked-items), [9](#9-cinderbox-studio), [10](#10-server-rules-in-luau) |
| Anyone's items are safe to load: cooked items read by Cinderbox's own reader, never by Godot's resource loader | [6b](#6b-cooked-items), [before launch](#before-launch) |
| The formats modders depend on are settled (the sheet, a cooked item, the rules' API), each carrying its version; after launch a change to one is versioned and announced | [6](#6-sheets-improved), [6b](#6b-cooked-items), [10](#10-server-rules-in-luau), [before launch](#before-launch) |
| Joining a server is safe: any server once every decoder of server data is fuzzed, or only official or listed servers (**open**: [which servers the game joins](#0-decisions)) | [before launch](#before-launch) |
| Players find servers without typing an address (the alpha needs only one address) | [parked](#parked): how, decided later |
| A player wears an avatar of their own: uploaded, or chosen from a curated set (**open**: [players' own avatars](#0-decisions)) | [11](#11-avatars) |
| The manual says what the code does, in the words modders use | [before launch](#before-launch) |

Which steps launch needs: [the road](#the-road).

## How we work

| Rule | Why |
|---|---|
| A milestone is done when shipped content uses it and it survived a playtest, not when its test passes. From step 4 on, a playtest by hand every week (two games, 150 ms) is the gate | M230 to M232 were tested and unused until M236 and M242; the checklist by hand has waited since M235 |
| Features wait until [the slice](#4-the-slice) needs them | an engine with no game makes every feature generic, and a generic feature is never finished |
| One fact, one place | the bat's swing length is in three places (the clip, the sheet, the C++) |
| The pack is the source of truth | names declared in C++ make the editor wait for a server build |
| Standard words in the modders' docs | modders come from Roblox, Unity and Godot: client-side prediction, replicated field, ability, cooldown, hitscan |
| The docs say what the code does | a statement about today is checked in the code first; where they differ, the code wins |
| CI after every push | the determinism CI was red from M136 to M161 and nobody saw |

## The road

| # | Step | State | For launch |
|---|---|---|---|
| 0 | [Decisions](#0-decisions) | eighteen taken; nine open | |
| 1 | [Stabilize M215 to M234](#1-stabilize-m215-to-m234) | unattended: clean. **By hand: to do** | |
| 2 | [Small fixes from the review](#2-small-fixes-from-the-review) | done (M243) | |
| 3 | [Two network models](#3-two-network-models) | done (M244 to M250) | |
| 3b | [Many players](#3b-many-players) | done (M251 to M261) | |
| 3c | [Groundwork](#3c-groundwork) | **in progress**: the pack check (M262), the tests' registration (M263), CI skipping docs (M264), lag compensation's limit (M265), images failing softly (M266) done; about a day each | yes, first: every later step leans on it |
| 4 | [The slice](#4-the-slice) | **next**: an arena brawler | yes: the test of everything else |
| 4b | [The alpha](#4b-the-alpha) | after 4: strangers on one official server, official items only | yes, before 9 to 11 |
| 5 | [Every item a sheet](#5-every-item-a-sheet) | beside the slice | yes: one way for every item before modders copy one |
| 6 | [Sheets improved](#6-sheets-improved) | after 5 | yes: the sheet's format settled, with a version |
| 6b | [Cooked items](#6b-cooked-items) | after the slice, with 6 | yes: before anyone else's items reach players |
| 7 | [Timing from the clip](#7-timing-from-the-clip) | when the slice asks | as far as the slice needs |
| 8 | [Body parts with priorities](#8-body-parts-with-priorities) | when the slice asks | as far as the slice needs |
| 9 | [Cinderbox Studio](#9-cinderbox-studio) | after the alpha | yes: modding without a compiler |
| 10 | [Server rules in Luau](#10-server-rules-in-luau) | after the alpha | yes: rules without a compiler; a mod's crash stops the mod, not the server |
| 11 | [Avatars](#11-avatars) | after the alpha | yes (decided 2026-11-17) |
| 12 | [Godot's own, used](#12-godots-own-used) | one row done (M259) | what the slice and launch need (translations, themes) |
| | [Before launch](#before-launch) | open | yes |
| | [A thousand in one crowd](#a-thousand-in-one-crowd) | later | no: 255 in one fight hold 60 Hz today |

- **Groundwork first**: the pack check could be bypassed (tightened: M262), four tests never ran
  (registered: M263), a malformed welcome aborted the client (an error since M266), and thread safety rests on comments. Each fix is about a day, and every later
  step leans on them.
- **Cooked items before anyone else's items**: a Godot pack from a stranger can run code today
  (M261.9). Until 6b, the alpha loads official items only.
- **The slice next**: steps 5 to 8 are what a fighting game leans on, and the slice says which
  parts matter. Steps 9 to 11 are worth more once there is a game to mod and strangers who played it.
- **Why the slice came late**: steps 3 and 3b (M244 to M261) went to network models and to 255 and
  1023 players, while the slice is an arena of a few to tens. Chosen on purpose (2026-10-28,
  2026-11-03); it bought headroom. From here the slice decides.
- **Why stabilizing is not finished**: unattended sessions catch crashes, desyncs and replays that
  do not verify; what only a person sees (a swing that looks late, a camera that jumps) needs two
  games played by hand.

### 0. Decisions

**Taken:**

| Decision | Taken | Means |
|---|---|---|
| **The network model** (2026-10-28; the default since 2026-11-01) | the **hybrid** is the default; the inputs model stays a server's choice | [step 3](#3-two-network-models) |
| **The sheet's file name** (2026-10-27) | **`.cbsheet`** (Ember, also considered, is already a programming language's name) | [step 6](#6-sheets-improved) |
| **How server-only rules are written** (2026-10-27) | **Luau** | [step 10](#10-server-rules-in-luau) |
| **Avatars' proportions** (2026-10-27) | **the standard skeleton only**: no body scales | [step 11](#11-avatars) |
| **One way to draw every character** (2026-11-06) | a look made from the character's scene, drawn through the RenderingServer, the viewer's own included. Built in M260 | DESIGN.md |
| **Stay on Godot** (2026-11-08; raised again 2026-11-17 after the pack finding, and kept) | the expensive parts are the engine's own C++; Godot is the renderer, the UI, the audio, the platform layer and the modders' free editor. Revisited only if a measured target fails in Godot's renderer and a GPU crowd cannot fix it. The pack hole is not Godot's renderer: it is a check of ours and Godot files from strangers, which [6b](#6b-cooked-items) ends | [routes not taken](#routes-not-taken) |
| **Every item drawn as data** (2026-11-14) | an item's look made once per kind; its reactions run from the compiled sheet; no node per item. Part of the cooked items since 2026-11-17 | [step 6b](#6b-cooked-items) |
| **Pushers stay exact** (2026-11-06) | a pusher's place every tick, not every other tick filled in: that would change how they push | [docs/scaling.md](docs/scaling.md#the-plan-for-many-players-step-3b-2026-11-03) |
| **The goal's scope** (2026-11-17) | as proposed, with avatars in it | [the goal](#the-goal) |
| **The slice's game** (2026-11-17) | an arena brawler | [step 4](#4-the-slice) |
| **Avatars before launch** (2026-11-17) | yes; the owner brings a few models when the step starts | [step 11](#11-avatars) |
| **The Hytale comparison's proposals** (2026-11-17) | 1 and 7 before launch; 5 in step 6, 3 with it; 2, 4 and 6 postponed and kept open | [From the Hytale comparison](#from-the-hytale-comparison) |
| **No version number in the plan** (2026-11-17) | the road ends at launch, and no release is named by a number (agents confused one with the protocol's and the formats' versions) | this file |
| **Mod content ships as cooked items** (2026-11-17) | one cooker makes each item from what Godot imported; the client reads it with Cinderbox's own reader and draws it through the RenderingServer; no Godot pack and no Godot resource loader for anyone's mod content. The base game stays a Godot pack. Godot's editor and node trees are the modders' authoring tool only | [step 6b](#6b-cooked-items) |
| **Nodes never grow with players** (2026-11-17) | every visual that comes with an entity is drawn through the RenderingServer; nodes are a fixed set (the HUD's frame, menus, the camera) and fixed pools (sound voices); lists make only the rows in view | [step 6b](#6b-cooked-items), DESIGN.md |
| **Not on Steam** (2026-11-17, the working assumption) | no Steam Workshop: the store is Cinderbox's own | [step 6b](#6b-cooked-items), [parked](#parked) |
| **The repository out of OneDrive** (2026-11-19) | moved to a plain folder (`C:\Cinderbox_Dev\cinderbox`, a fresh clone of `main`), GitHub the backup; the paid assets copied beside it, still outside git. Every text file is LF in the working copy whatever `core.autocrlf` says (`.gitattributes`): a clone with CRLF published every item under another hash | M261.11 |
| **GDScript stays in the shipped game, for now** (2026-11-17) | taking it out (export templates built without it, the game's own scripts moved into the extension) is postponed: once no mod content is a Godot pack, content cannot reach a script engine. Until then the tightened pack check (M262) is the only guard | [3c](#3c-groundwork), [6b](#6b-cooked-items) |

**Open:**

| Decision | Options | Leaning | Decides |
|---|---|---|---|
| **How players find servers** | a website's "play" link that starts the game; the game's own list; a desktop shortcut (not Steam: 2026-11-17) | decided later (the owner, 2026-11-17); needed for launch, not for the alpha. The second review (2026-11-17): it is one decision with the website, updates, the store and avatar uploads; take it before the alpha ends | [parked](#parked) |
| **The players an arena is made for** | a few (a duel, 2v2) to tens | decided with the slice's map | [step 4](#4-the-slice) |
| **Cosmetic physics** | Jolt with a static copy of the level; no Jolt at all | decided later | [step 12](#12-godots-own-used) |
| **The slice's network model** | the hybrid (the default); the inputs model | chosen by playing both (`playtest.ps1 -NetModel inputs`), at 60 ms and at 150 ms, in an arena with physics in it (review, 2026-11-17). The review's lean, for up to 16 players where physics matters: the inputs model (in the hybrid your own hit on a body shows about 0.2 s late at 150 ms) | [step 4](#4-the-slice) |
| **Both network models after the alpha** | keep both (the inputs model for small servers); keep one | after the alpha. Two models cost every feature twice (three client sessions in `game_client.h`; NetIds used up by hand to follow the server in `Simulation::ApplyCommand`). The review: the model the slice does not take gets no new features from that day | [step 3](#3-two-network-models) |
| **Which servers the game joins at launch** | any; official or listed ones only | none yet: any server needs every decoder of server data fuzzed first. The review's lean: official and listed. Mods are compiled C++, so a community host runs a server program of its own, and Box3D's world bytes were not made to be read from a hostile source | [before launch](#before-launch) |
| **Players' own avatars at launch** | uploaded by players; a curated set (the owner's models) | none yet: uploads need terms, moderation and budgets. The review's lean: a curated set; with a store of our own (not Steam), moderating uploads is all ours | [step 11](#11-avatars) |
| **Our own mod editor** (raised 2026-11-17) | Godot's editor with plugins ([step 9](#9-cinderbox-studio)); an editor of our own, closer to the ECS | the review's lean: Godot's editor. Authoring in a tree and baking to an ECS is the usual split (Unity's DOTS authors GameObjects and bakes them to entities); Godot's import, animation, AnimationTree, particle, material and UI editors would each have to be rebuilt. The ECS-native tool worth making is a live tuning panel in the game (the mirror's entities, the board) | [step 9](#9-cinderbox-studio) |
| **Where items are cooked** (raised 2026-11-17) | on the modder's machine; in the store, from the uploaded project (Roblox converts uploads on its side) | the review's lean: the modder's machine first; the store checks every upload with the client's reader and the budgets before anyone can download it. Cooking in the store lets old items be cooked again when the format changes, but the store then reads strangers' Godot files and needs a sandbox | [step 6b](#6b-cooked-items) |

### 1. Stabilize M215 to M234

A written checklist ([docs/playtest.md](docs/playtest.md)) of what M215 to M234 added, played
together under latency and loss: `tools\playtest.ps1` starts the server, `cb_netsim` (150 ms round
trip, 2% lost) and two games, records the session and verifies its replay. `-Autoplay SEC` plays
unattended.

| | State |
|---|---|
| Unattended sessions (default mods, the critter, waves, 32 NPCs) | clean after M235 to M242; run on the inputs model, the default then |
| M230 to M232 in use | TimeScale and crossfade curves: the fist (M236); OneShot: the mannequin's flinch (M242); a nested machine: the fist's guard (M242); Add2 kept for later (a lean or breathing sway, once there are clips) |
| **The checklist by hand, two games** | **to do**, on the hybrid |

| Still open from the sessions | Goes with |
|---|---|
| Spamming use breaks the bat's and the guns' animations: their hand-written `predict` lines guess more uses than the server allows | [step 5](#5-every-item-a-sheet) |
| With brawl on, the unattended player never reaches the pistol or the bat (the kit takes the first slots) | the checklist, by hand |
| With the fist out, the walk's arm swing does not show (the fist's pack owns the upper body) | the owner's own clips |
| HUD lines overlap in half-screen windows | not a priority: the game is played full screen |

**Done when** the checklist passes by hand and every feature since M215 is used by shipped content
or gone (`CbCurve` is unused: deleted in [3c](#3c-groundwork)).

### 2. Small fixes from the review

**Done (M243).** Left over: the motion nodes go in [3c](#3c-groundwork); the bat's dead stance
goes with [step 5](#5-every-item-a-sheet).

### 3. Two network models

**Done (M244 to M250).** The hybrid is `cb_server`'s default; the inputs model is a server's choice
(`--net-model inputs|hybrid`), and the welcome says which. How it works: [DESIGN.md](DESIGN.md#netcode);
the two side by side and the hybrid's accepted limitations:
[docs/scaling.md](docs/scaling.md#the-two-network-models). M244 and M245's server-state model was
dropped ([routes not taken](#routes-not-taken)).

### 3b. Many players

**Done (M251 to M261).** Everything measured is in [docs/scaling.md](docs/scaling.md).

| Players in one fight | Server thread per tick | Each player, down | The game, all in view |
|---|---|---|---|
| 255 | 15 to 17.5 ms of 16.7 | 0.51 Mbit/s | 47 frames a second (a recording) |
| 1023 | 55 ms | 1.7 to 1.9 Mbit/s | 5 frames a second (a recording) |

- Each player sends 34 kbit/s up (M261). The targets set for 1000 in one area are not met; what is
  left is [later](#a-thousand-in-one-crowd).
- **No fog of war**: every character is sent to every player (far ones about every 12th state).
  Not sent: others' unshared fields, others' items put away, others' inputs. Cheating is decided
  [before launch](#before-launch).

### 3c. Groundwork

From the outside review (M261.7) and the second review (M261.9), both checked against the code.
Each is about a day or less.

| What | Why |
|---|---|
| **Done (M262): the pack check tightened** (the stopgap until [6b](#6b-cooked-items)): text resources refused (exports write binary ones); a redirect may hold no backslash, no key an export does not write and no value that builds an object, and points only at a converted file of the pack named by the MD5 of its own file's path; every converted file must be named by one; a binary resource's header and external resources read (no script class; each external resource a typed file in the moddable folders, and not by another file's id, which Godot follows first); a binary sample or font checked as a resource. `check_mod_validator.gd` builds each case as a hostile pack (15 new) | **found in M261.9**: an escaped `GDScript` type in a text resource passed the search of the bytes and ran inside `ResourceLoader.load`; an escaped `res://` redirected anywhere; any file under `.godot/exported` and `.godot/imported` was accepted. **Changed from the plan**: not "any file whose path exists in the game": a player's restyle replaces the game's own converted copy of a moddable file (`example_neon`'s `prefabs/prop_box.tscn`), so a converted file is tied to its redirect's file instead |
| **Done (M263)**: the four tests that never ran registered (`motion_order`, `motion_regular`, `motion_amounts`, `motion_inspector`); CMake reads both runners' tables, and a table line of another shape stops the configure | they are in `test_main.cpp`'s table but not in `tests/CMakeLists.txt`: `ctest` and CI never ran them. All four pass when run (2026-11-17) |
| The viewer extension built and the 29 Godot checks run in CI (Windows, headless) | the client's half has no CI; Godot runs headless here without trouble |
| Two Linux CI jobs: ASan and UBSan running every test; TSan running the `net_*` tests (with `CB_NO_TIMING_CHECKS=1`: TSan is 5 to 15 times slower, too slow for the late-input limits) | thread safety rests on comments: a new thread for each state (`GameServer::m_making`), `FinishStates( true )` in seven places, `ItemsOf` rebuilding a `mutable` index inside a const method |
| **Done (M266)**: reading a server's image fails with an error, never `std::abort()`: `simulation.cpp`'s reader reads nothing past the end; `Simulation::EcsImageFits` reads the simulation's part whole before anything changes (its magic, every entity's components whole and of known kinds, NetIds in order, nothing after); `LoadPortable` refuses an image with bytes after its worlds. `portable_broken`: an image broken 25,190 ways, every one refused (the old reader aborted on the first), and the simulation then loads the whole image and runs on with the server. A snapshot the simulation saved itself (`Load`, rollback) still aborts when it does not fit: that is a fault, not bad data | a malformed welcome aborted the client (`Simulation::LoadPortable` through `Reader::Take` and `DeserializeEcs`). **Left** ([before launch](#before-launch)): the values in the image are taken as they are, and Box3D's world bytes go to `b3DeserializeIntoShell` once their sizes fit; fuzzing both is the before-launch row |
| **Done (M265)**: lag compensation capped by the client's measured round trip (`GameServer::SeenBackLimit`: the round trip and four times its spread, two states, 150 ms; cut where the input comes in, so the frame, the mods and the simulation all see the cut value). `net_seen_back_capped`: a game saying 255 ticks back is cut to about 16 on the server's own link and 25 at 50 ms each way (30 before, the whole window); the honest player is never cut | `seenBack` was capped at the server's 500 ms only (`Simulation::SeenAt`), so any client could always claim the full 500 ms. **Left**: the round trip is the client's answers' (a game that holds its acknowledgements back looks further away, and pays for it in frames sent again); a link losing 3% each way says more than the limit in under 1% of its inputs, judged a little nearer the present than it saw |
| **Done (M264)**: CI skips pushes that change only Markdown (`paths-ignore: ['**.md']`; run by hand with `workflow_dispatch`) | M261.5 to M261.8 each started two runs of six jobs, about 20 minutes each, for docs alone |
| Find out why lag compensation disagrees with the attacker's screen when the game stalls | M261.10: with the whole process frozen about 150 ms every second or so (a busy machine), `net_brawl_lag_compensation` still fails 2 runs in 6: jabs whose input came on time and within the 500 ms the server keeps, judged against what the attacker's game showed, hit what it saw out of reach or miss what it saw in reach. Either the test measures the screen wrongly during a catch-up, or a player whose game hitches gets judged against the wrong past. Never seen without stalls, nor yet on CI |
| A `.clang-format` and a `.clang-tidy` (the `bugprone-*` checks); `-Werror` for our own targets in CI | lower value than it looked: `clang-tidy`'s `bugprone`, `performance` and `concurrency` checks on ten core files found about 60 things, none that matter (the byte comparisons are of structs asserted free of padding) |
| Delete the motion nodes (`CbMotion`, `CbProbe`, `CbImpulse`, `CbForce`, `CbLink`, `CbMove`, `CbLaunch`) and `CbCurve`, after moving `BakeSheetMotion` to a file of its own | no shipped content uses them; `BakeSheetMotion` (the sheet's bake) lives beside them in `cinderbox_motion.cpp` |
| Move the protocol's changelog out of `protocol.h` (`kProtocolVersion`'s comment) into the history | the code says what is; the history says how it came to be (the comment is one line of about 6,000 characters) |

**Not here**: the large splits ([housekeeping](#housekeeping)) happen where later steps touch the
code: `Simulation` with steps 5 to 8, `GameServer` with [step 10](#10-server-rules-in-luau). (The
second review proposes one exception, open: `simulation.cpp` moved into files by area now, as a
pure move that the reference hashes prove changed nothing.)

### 4. The slice

One game made with the engine and played by people. **Decided (2026-11-17): an arena brawler**,
like a Roblox fighting game: one map, one mode (rounds and a score), the brawl kit and the bat, a
ten-minute session that is fun. How many players an arena is made for is decided with its map.

| | Today |
|---|---|
| The mode | `deathmatch`, on by default: a round ends at 10 kills or 5 minutes; a scoreboard |
| The kit | `brawl`: fist, dash strike, uppercut, ground slam, guard, as hotbar skills; all predicted from sheets |
| The bat | `melee`: each life starts with one (the shop sells another); C++ with a hand-written prediction ([step 5](#5-every-item-a-sheet)) |
| The map | `test_ground` and `example_arena` are test maps; none is made for the slice yet |
| Players | the owner, with two games on one machine |
| The network model | not chosen. The hybrid is the default; [docs/scaling.md](docs/scaling.md#the-two-network-models) calls the inputs model the fighting games' (10 to 30 players). In the hybrid your punch moves another player a round trip and the buffer late; in the inputs model at once, with every client simulating everyone |

- **First**: both network models played by hand (`tools\playtest.ps1 -NetModel inputs`, then the
  default), the brawl kit against each other, at 150 ms and at 60 ms (150 alone is the hard case,
  not the usual one); the slice takes the one that feels right.
- Built beside steps 5 to 8: each of those is done when the slice uses it. Played every week, with
  latency; what it runs into is the next milestone.
- **Proposed by the second review (2026-11-17), not decided**: an arena that shows what only
  Cinderbox has, shared physics (walls that break, props to throw, ring-outs), so the network model
  is chosen on what matters; and bots that fight with the kit, since strangers in a weekly alpha
  will often find the server empty.
- **Done when** the playtest checklist ([docs/playtest.md](docs/playtest.md)) passes by hand and our own ten-minute sessions run clean and are fun.
  **Time limit: four weeks** from the step's start; then [the alpha](#4b-the-alpha) goes ahead with
  what is left.

### 4b. The alpha

Strangers play the slice on one official server, every week. Steps 9 to 11 come after it.

| Needs | Why |
|---|---|
| **Updates**: a build players download, and a way to get the next one (not Steam: 2026-11-17) | the server refuses any other protocol version (`kProtocolVersion`) or build: every protocol change locks out the old game |
| The official server's address handed out (a link, or typed) | how players find servers is decided later; one server needs no list |
| The alpha's build joins only the official server's address | a stranger's server is not safe to join until [before launch](#before-launch)'s decoders are fuzzed; a rule players are asked to keep is not a defence |
| Official items only (the pack check tightened: M262) | a stranger's Godot pack is read by Godot's parsers until [6b](#6b-cooked-items), and no check makes them safe against a malformed file; players' own mods stay a risk they take ("install only what you trust") |
| A place to hear back from players | what they run into is the next milestone |
| Proposed by the second review (2026-11-17): crash reports from the game and the server; every official session recorded (`--record`); the server restarted and logged by itself; a refusal that says "update required" with where to download; third-party licence notices in the build | an invariant that fails calls `abort()` and nobody hears of it; a recording replays any bug; the licences of Box3D, flecs, ozz, ENet, Recast, godot-cpp and Godot (MIT and zlib) ask for their notices in what is shipped, and none are today |

**Done when** strangers play sessions and come back for more.

### 5. Every item a sheet

The fist's way for every item: the item's sheet runs its use in every simulation (predicted), and
the mod's C++ turns its hit events into damage (`brawl.cpp` is the model, 158 lines).

**The bat today:**

| | |
|---|---|
| The swing | `melee.cpp` (272 lines): five rays fanned out from the chest at the clip's `melee.strike` marker; the hit test poses the target (hit zones) |
| The look's guess | `predict use as melee.swing`, `cooldown 1.1` copied from the clip, `stance melee_swing on full` (read by nothing) |
| Its length | in three places: `melee.done` at 1.1 s in the clip, `cooldown 1.1` in `bat.rules`, `kCooldownSeconds 0.6` in `melee.cpp` |
| `melee.hot` | a field on the bat the server sets when a swing hits a player, cleared by the mod's timer 2 s later; not predicted; a prop hit does not heat it |
| NPCs | cannot swing it: `melee.cpp` loops over player slots (the fist works for NPCs: sheets run for every driver, M228) |
| Its sheets | `bat.rules` in the bat's scene (the glow, the flames, the sparks, the guess); `reactions_melee.rules` in a world reactions scene (the sounds) |

**The bat after** (a sketch, not tried):

```
use
	until melee.done          # the clip's marker ends it: one place for the swing's length
	emit melee.swing          # the pack's Swing state starts on it, predicted: no predict line

motion Strike on melee.strike
	cast sphere 0.25 range 1.8
		emit melee.hit
		impulse 7 along look
```

- `melee.cpp` turns `melee.hit` into `combat.damage` (about 60 lines); NPCs can swing it.
- **The glow** is only a look: `on melee.hit` in the bat's sheet sets the emission and resets it
  after 2 s, at once, because the hit is predicted. If heat should matter to the game, the language
  needs `change` on the item and a timer on a carried item ([step 6](#6-sheets-improved)).
- **A sheet's cast hits capsules** (the simulation has no pose; hit zones are the server's). A bat
  can live with that, as the fist does; a shape that follows the bat is [step 7](#7-timing-from-the-clip).
- **The guns**: their use (ammo, rate of fire, the cue) moves into the sheet, so their `predict`
  lines go; the shot's hit stays the server's (hit zones need posed hitboxes) and hears the sheet's
  event. A cast that poses what it passes near is possible later (the pose is deterministic).

### 6. Sheets improved

The language is right: bounded, deterministic, run by every simulation, so skills are predicted
(Overwatch does the same with a visual language, Statescript). What changes is how it is attached,
written and checked. Its format is settled before launch, so it changes here: a sheet carries
its format's version, and the compiler reads older ones, so words can still be added after launch.

| Change | Why |
|---|---|
| **First: the sheet compiler as a plain C++ library**, with no Godot types, like `src/expr`; the first part of the single cooker ([6b](#6b-cooked-items)). Today a sheet is read twice: `ParseRules` (1,198 lines, `cue_rules.cpp`) into Godot Dictionaries in the editor, then `BakeSheetMotion` (`cinderbox_motion.cpp`) writes a second text format that every simulation parses (`CompileMotionSet`, 957 lines, `motions.cpp`) | the server then reads sheets (and their names) itself; sheet tests run headless; the formatter and errors as you type share the one parser. The language's words are kept by hand in step across the parsers: the list of directions is written three times (`motions.cpp:151`, `cinderbox_motion.cpp:116`, `cue_rules.cpp:391`) |
| **A resource on its node**: `CbSheet`, plain text in a `.cbsheet` file, read and written by the extension's own loader and saver (as `.gd` and `.gdshader` are); a property on the node (`CbItem.sheet`, `CbMotionSet.sheet`, the world reactions root, the HUD root), which says which blocks it allows; may be saved inside the scene. **`CbRules` goes** | `CbRules.file` is a plain string: no UID, a rename breaks it, Godot does not see the dependency |
| **Not Godot's script slot** | "every script is refused" is the security story (the check, tightened in M262, still lets Godot parse strangers' files; cooked items end that, [6b](#6b-cooked-items)); a script language through GDExtension is a large, fragile API for two features |
| **Where each block runs, marked**: simulated blocks and the viewer's in separate sections (or files); one indentation rule (it counts) | today four languages share a file: `motion` and `use` (simulated, indentation counts), `on` and `while` (reactions), `predict`, and `show` / `text` / `bind` (HUD) |
| **Names in the sheet**: `field brawl.combo int`, `event brawl.jab.hit`; the server reads them from the pack | the editor waits for a server build |
| **Parameters**: `param damage 8`, shown in the inspector, read by the server's rules | numbers written twice (the bat) |
| **One sheet for many items** (Hytale proposal 5): shared across items, each with its own parameters or replacing named parts | `fist.rules` writes the same `find` block four times |
| **Words for how skills meet** (proposal 5): `blocked by`, a combo with a window, a charge, tap against hold, a cooldown shared by name | the fist guards motions with `not motion.brawl.fist.Finisher`; brawl counts its combo window down in a field |
| **Text and rows from the server** (proposal 3; shape decided when the step starts): rows a mod publishes for a `CbList`, text fields sent like names and private fields (outside the hash); the layout stays the look's | fields are numbers; the shop's offers are written twice (`hud_shop.tscn`, `kOffers` in `shop.cpp`) |
| **A motion's state readable** (cooldown left, charges, on) in conditions and HUD lines | brawl rebuilds its cooldowns with five C++ fields and five `while` motions, while a motion's own `cooldown` goes unused |
| **Rates said**: `change x -= 1 per second` | `change x -= 1` is once in a step but per second in a `while` |
| **One rule for expressions**: one word bare, anything longer in brackets | an expression ends at "the next option word" |
| **`let NAME = EXPRESSION`** in a motion | the frag's falloff is written out twice |
| **A cast and the owner's own effects in one step** | `dash_strike.rules` needs two blocks with a copied condition |
| **`change` on the item; a timer on a carried item** | the bat's heat, if it is ever more than a look |
| **Curves as resources** (`curve(res://falloff.tres, reach)` or a curves dictionary on the node); the frag's falloff the first user | no shipped sheet uses `curve()`; `CbCurve` is found by name from text (deleted in [3c](#3c-groundwork)) |
| **The planned words**: `$target:FIELD`, `at POINT`, a cast in a later step ([the list](docs/rules-language.md#planned)) | |
| **Tools**: a formatter; errors in the editor as you type; headless sheet tests (a motion on a stand-in, then what moved) | |
| **`.rules` becomes `.cbsheet`**: 40 sheets and about 80 files that name `.rules`; `docs/rules-language.md`; a `.gitattributes` line and a small VS Code grammar | the name says what the file is, one family with `.cbmap` |
| **Last, not needed by the slice: items drawn as data and reactions run from the compiled sheet** | moved to [6b](#6b-cooked-items) (2026-11-17) |

**The dash strike after** (proposed, not valid today); its HUD reads the cooldown itself, and
brawl's `cd_dash` field and `CoolDash` motion go:

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

### 6b. Cooked items

**Decided (2026-11-17).** Mod content stops being Godot packs. One cooker makes each item from what
Godot imported; the game reads it with Cinderbox's own reader and draws it through the
RenderingServer. Godot's editor and its node trees are the modders' authoring tool only. The base
game stays a Godot pack (it is ours).

**Why**:
- **A Godot pack can run code** (found in M261.9): Godot builds a pack's resources, scripts
  included, while it loads them, before any check of ours sees a node. A byte scan cannot follow
  Godot's own decoding (an escaped type name passed it), and Godot has no loader that skips
  scripts ([its proposal #4925](https://github.com/godotengine/godot-proposals/issues/4925) is open).
- **Nodes never grow with players** (decided the same day): a look must be data the
  RenderingServer draws, as characters are since M260.

**The cooker**: one, in the extension, run when publishing and in the editor. It replaces today's
pack bake, the autobake, `BakeSheetMotion` and the scene reading in `CharacterLook` and `ItemLook`.

| It takes | From |
|---|---|
| simulation data: item bodies, motions, characters' state machines, clips, hitboxes | today's bakes |
| sheets | [step 6](#6-sheets-improved)'s compiler |
| looks: meshes, skins, materials, sockets, lights, emitters, sounds, animation tracks | what `CharacterLook` and `ItemLook` take from a scene today, at publish time instead of at load |
| the bytes of meshes, textures and sounds | what Godot already imported: mesh arrays, textures as GPU-ready compressed blocks, Ogg Vorbis. A player's machine imports nothing |

An item is then a manifest (the format's version, the engine's, the names it reads, each file's
size and hash) and files named by their hash. Hytale proposal 7's "what an item was made for" is
the manifest's versions.

**The reader is the security boundary.** Anyone can upload an item made by hand, so the cooker is
for correctness and the reader for safety: a small C++ library with no Godot types (like
`src/expr`), fuzzed headless in CI. No `load_resource_pack` and no Godot resource loader for
anyone's items, players' own restyles included.

| In an item | The reader checks |
|---|---|
| meshes | counts capped, every index below the vertex count, every float finite |
| textures | GPU block formats only, sizes matching their bytes: no PNG or JPEG decoded on a player's machine |
| sounds | Ogg Vorbis only, one decoder, fuzzed with the reader |
| animation | ozz clips; the ozz archive reader fuzzed |
| sheets | step 6's compiler, plain text |
| what a part is (a light, an emitter, a socket) | a kind from a fixed list and named fields, never a class name for Godot to build |
| everything | sizes declared before anything is read, and capped |

**Delivery** (not Steam, 2026-11-17):
- The store is Cinderbox's own: files named by SHA-256 over HTTPS (static files behind a CDN are
  enough), never over the game connection. A server names its items by hash, as today; the game
  fetches what it lacks, checks the declared sizes before downloading and every hash after, and
  caches by hash (it does all this today from a store that is a folder: `workshop.gd`,
  `workshop_fetch.gd`).
- Every version stays fetchable while the store keeps it, so a server pinned to a hash always finds
  its item (Steam would serve only an item's latest version).
- The store checks every upload with the same reader and the budgets (triangles, materials, texture
  sizes) before anyone can download it. Where items are cooked is open ([step 0](#0-decisions)).
- Downloads compressed (Zstd) once items grow; today's items are 1.5 to 88 KB.
- From Hytale ([docs/hytale-comparison.md](docs/hytale-comparison.md#asset-delivery-looked-at-again-2026-11-17)):
  sizes declared before any byte, a cap per file; not its transfer on join, which a plugin exists
  to skip.

**Drawn so that nodes never grow with players:**

| Comes with entities | Drawn by |
|---|---|
| bodies | the RenderingServer (M260) |
| held items | the RenderingServer, from the cooked item (decided 2026-11-14 as "every item drawn as data") |
| particles, lights, decals | RenderingServer instances, started by reactions run from the compiled sheet per entity in the presentation's flecs world (the mirror): `on` rules on cues, `while` rules on state, effects on the look's parts by index. Hand-placed `CbReaction` nodes stay for authoring and the Cue Preview |
| sounds | a fixed pool of voices |
| lists (a scoreboard of 1023) | only the rows in view |
| the map | RenderingServer instances from the cooked map |

- Measured: a bat is 15 nodes in the game; at 255 fighting players bats are half of the scene's
  7,900 nodes; at 1023, making and freeing what comes into sight is 68 ms of a 184 ms frame.
- A ratchet in the crowd bench: the node count must not grow with players (`tools\crowd_client.ps1`
  prints it).
- **Not taken: MultiMesh for items**: culled as one, shadows all or none, and a per-item glow needs
  a shader a modder's material does not have; kept for thousands of one thing.

**In order**: characters and items (the scene readers exist), then effects (particles, lights,
sounds, track events), then the map (statics, lights, environment, lightmaps: the largest), then
HUDs (Godot scenes behind the guard until then, or a fixed-list description our code builds into
controls).

**Also:**
- The editor's previews (the Cue Preview, an item's, a character's) cook on the fly and draw with
  the game's own code, so a look is the same in the editor and in the game.
- Shaders are decided with the reader: refused, or a checked subset (a modder's shader source would
  be read by Godot's shader compiler).
- Once no mod content is a Godot pack, nothing a stranger made reaches GDScript, so removing it from
  the shipped game is not needed for content (postponed: [step 0](#0-decisions)).

### 7. Timing from the clip

The sheet says what; the clip says when (Unreal's montages with notify windows; fighting games'
frame data).

| On the animation | Baked to | Read as |
|---|---|---|
| a marker (today) | an event at a time | `on melee.strike` |
| a window (hit, invulnerable, cancel, armor): two markers, start and end (Godot's timeline has points, not spans) | a span of the clip | the sheet's `then when x.open` ... `when x.close`; a `window.hit` condition only if sheets keep needing it |
| a hit shape on a bone (a `CollisionShape3D` under a `BoneAttachment3D`) | swept from tick to tick along the pose while its window is open | a cast that follows the bat |

The simulation poses one chain of the owner's bones while a window is open, not the whole body
(the pose is deterministic). Gives the bat's arc, a kick's foot, the critter's tail, and answers the
language's open question "a shape that follows a bone"; removes numbers copied from clips (`cooldown 1.1`).

### 8. Body parts with priorities

**Today**: a character's own layers (the mannequin's `FullBody`, `UpperBody`, `Flinch`), which a pack
replaces by name; four overlay slots, filled in the order items were carried; bone filters on or
off; two special rules ("what the hands hold wins the arms", "a hand an overlay names lets go").
**Short for a fighting game**: most skills are full-body, so a full-body move is an overlay ordered
by pickup; a filter's hard edge at the spine shows when a swing plays over a run.

**After** (Unreal's montage slots and per-bone blending, in the simulation's shared pose, so
hitboxes follow):

| | |
|---|---|
| Parts | a humanoid gets Legs, Torso, Head, LeftArm, RightArm from its bone profile; another skeleton authors its own (Tail, Wings) |
| Requests | every animation asks for parts at a priority, with fade times, replacing or added |
| Who wins | per part, the highest priority (locomotion, then the held item, a skill, a hit reaction or stun); added ones (recoil, flinch) on top |
| Legs | a request keeps the legs walking (a jab on the run) or takes them (a heavy swing roots you) |
| Borders | weights rising along the spine, not a cut |
| The two rules | become one: the held item asks for its arms above an overlay; a throw asks for its arm above that |
| Authoring | still an AnimationTree per pack; the pack's root says its parts and priority |

### 9. Cinderbox Studio

Godot's editor is the studio; the extension makes it the mod tool. (An editor of our own was raised
again on 2026-11-17: open, [step 0](#0-decisions).)

| Godot's hook | Use |
|---|---|
| A prebuilt extension from CI (Windows, Linux, macOS) | making a look needs no compiler |
| `EditorFeatureProfile`, shipped with the SDK | hides script creation, the Asset Library and every node class the scene guard refuses |
| A main-screen plugin | a Cinderbox tab: the mod's items, names, problems, Publish, Test |
| `EditorNode3DGizmoPlugin` | every authored number drawn: a find's shape, a cast's range, a rope, sockets, hitboxes, the capsule, the eyes |
| `EditorExportPlugin` | cook, check and publish to the store, on every platform ([6b](#6b-cooked-items); replaces `pack_mod.ps1` and `publish_mod.ps1`) |
| `EditorDebuggerPlugin` | the F1 motion lines, fields, events and rollbacks in the editor during a test |
| `EditorScenePostImportPlugin` | a dropped `.glb` becomes a `CbCharacter` (what `make_mannequin.gd` does as a script) |

Also: **one authoring project**, every mod in `res://mods/<id>/` and the cooker refusing files
outside it (today 21 Godot projects share one `res://` when loaded); **one Play** (save, bake, start
a local server and the game, reload on save) on every platform; **starters by kind** (an ability, a
weapon, a game mode, a character, an avatar); names offered inside expressions; an item used in
its preview, with someone else in it and the character's own mesh; the modders' docs in standard
words.

**Proposed by the second review (2026-11-17), not decided**: split the step. What launch needs is the
prebuilt extension and the export plugin; the tab, the gizmos and the debugger plugin come as
modders ask. Luau ([step 10](#10-server-rules-in-luau)) before those: rules without a compiler are
the larger gain, and real mods are what settle its API.

### 10. Server rules in Luau

**Decided**: Luau, what Roblox developers write: made to be sandboxed (no files, no OS, an
instruction limit), and fast. Rules run on the server only, so they need no determinism.

- The same verbs as today's C++ (`Declare`, `Start`, `Tick`, the `Context`'s reads and commands),
  from a script in the mod's item: a server runs items from different authors without a build; a
  script reloads on save; a script's error stops its mod, not the server.
- **C++ stays** for engine mechanisms and mods too heavy for a script.
- **What is predicted stays in sheets**: a Lua VM's memory cannot be copied cheaply for rollback; a
  sheet's state is 28 bytes a motion.
- The shipped mods move over in the order the slice needs them; rules travel through a store only
  once the sandbox holds.
- **`GameServer` is split** when this step starts (connections, the schema, the mod host, states,
  the bench): the Luau host replaces part of it ([housekeeping](#housekeeping)).

| What a mod reaches | C++ mod (the owner's build) | Luau script (anyone's, no build) |
|---|---|---|
| Reads | the `Context`'s reads, and more of the engine opened read-only as mods ask | the `Context`'s reads, grown as mods ask |
| Writes | commands in the tick's frame, never the simulation directly | the same commands |
| Its API | may change with the engine | narrow and versioned: settled before launch |
| Cost | native | measured: deathmatch ported first, its time a tick at 64 players |

Writes are commands for both because every client runs the same simulation from them: a write
behind its back desyncs every player.

### 11. Avatars

**Before launch (decided 2026-11-17).** The owner brings a few models when the step starts. Whether players
upload their own is **open** ([step 0](#0-decisions)).

| | The body: one for everyone | The avatar: one per player |
|---|---|---|
| What | the standard humanoid `CbCharacter`: skeleton with Godot's humanoid bone names, state machine, hitboxes, capsule, sockets, camera | meshes skinned to the same bones, material colors, accessories |
| Who knows it | the server and every simulation | clients only |

- An avatar is the standard look (`CharacterLook`, M260) with other meshes, materials and
  accessories on sockets; colors as instance uniforms.
- **It never changes what the server sees**: everyone's hitboxes are the same, one set of clips
  serves everyone. **Standard skeleton only** (Roblox's R15 way); an avatar made on another rig is
  retargeted when it is published.
- **It never blocks joining**: fetched in the background, the mannequin drawn until it arrives.
- **Budgets** checked by the cooker and again by the store: triangles, materials, texture sizes.
- **Loading**: an avatar is a cooked item ([6b](#6b-cooked-items)): fetched by hash, drawn through
  the RenderingServer, freed when nobody wears it. No pack is mounted (packs cannot be unloaded).
- **Which avatar a player wears**: a per-player value for looks only, outside the hash, sent as
  names are.

### 12. Godot's own, used

| Godot's | For |
|---|---|
| `SpringBoneSimulator3D` | tails, hair, capes (cosmetic): something the look takes and runs on the drawn skeleton |
| Instance uniforms | a look per entity (heat, a team color, a hit flash) without a material per entity: the bat copies its material to glow |
| Threaded loading | the base game's scenes while joining (cooked items are read on a thread of ours: [6b](#6b-cooked-items)) |
| `MultiMeshInstance3D`, visibility ranges | debris; a GPU crowd of far characters ([later](#a-thousand-in-one-crowd)) |
| Animation detail by distance | **done (M259)**: drawing only, hit tests unchanged |
| Translations | a sheet's HUD text is English in the file |
| `Theme` resources | client mods restyling the UI |
| **Cosmetic physics** (open) | nothing of the simulation is in Jolt (collision is Box3D's; a look's `Area3D`s are quiet), so a cosmetic shell casing would fall through the floor: give Jolt a static copy of the level when the map loads, or drop Jolt |
| `PhysicalBone3D` | [ragdolls for any skeleton](#ragdolls-for-any-skeleton) |

## From the Hytale comparison

[docs/hytale-comparison.md](docs/hytale-comparison.md) (2026-11-11) found Cinderbox built on
Hytale's model (rules on the server, one client no mod changes, prediction from data) and proposed
seven things. **Decided by the owner (2026-11-17):**

| # | Proposal | Decided |
|---|---|---|
| 1 | **A server may refuse players' own packs** (they load last and may replace any file of a look: see-through walls with no cheat program; after [6b](#6b-cooked-items), players' own restyles are cooked items, and the policy covers them) | **taken**: [before launch](#before-launch) |
| 2 | **The camera as data**: a server's camera rig instead of the game's four views | **postponed**, [kept open](#postponed-kept-open) |
| 3 | **Text and rows from the server**, the layout still the look's | **taken with [step 6](#6-sheets-improved)** |
| 4 | **Sheets on things in the world**: doors, buttons, jump pads, trigger regions on map templates | **postponed**, [kept open](#postponed-kept-open) |
| 5 | **One sheet for many items, and words for how skills meet** | **taken**: [step 6](#6-sheets-improved) |
| 6 | **A game as one file**: the map, items by hash, options, the network model, the pack policy | **postponed** with how players find servers, [kept open](#postponed-kept-open) |
| 7 | **What an item was made for; rules from a store only after the Luau sandbox; shaders in packs decided** (the first becomes the cooked item's manifest: [6b](#6b-cooked-items)) | **taken**: [before launch](#before-launch) |

**Not taken**: a UI the server pushes change by change, assets sent on join, Hytale's sync of
predicted chains, a fixed set of keys.

### Postponed, kept open

What work before then must not close off. A change that goes against one of these says so in its
milestone.

**2. The camera as data.** Comes back after the slice, or when a mod first needs a view the game
lacks (top-down, side view, kill cam); "cameras a mod places" comes with it.

| Keep | Why |
|---|---|
| The views in one place: `ViewMode` (`src/sim/types.h`) and `godot/view_camera.gd`. A new view goes there, never as a camera a look carries (looks' cameras stay removed: `RemoveCameras`) | a server's rig then replaces one place |
| Where a line of sight starts in one function (`Context::ViewPosition`), its numbers named constants (`kShoulderOffset`, `kViewPivotHeight`, `kEyeAhead`, `kEyeUp`) | a rig's numbers, baked into the schema, feed it instead: the server must know where aim starts |
| `PlayerInput::view` a number, not flags | a server's own rigs are numbered after the four |

**4. Sheets on things in the world.** Comes back after step 6, or when the slice's arena wants a
door, a jump pad or a trigger region.

| Keep | Why |
|---|---|
| Step 6's sheet a resource on a node that says which blocks it allows | a map template becomes one more such node |
| Step 6's compiled sheet run per entity, not per player slot | a door or a region is an entity with no slot |
| A motion's triggers an enum that grows (`Motion::When`: Press, While, Event, Edge) | entering a region, being used, being hit are more triggers |
| "Use what you look at" one intent (`SlotIntent::Open`, a container's today) | it grows to anything a sheet says can be used |

**6. A game as one file.** Comes back with how players find servers: what a play link, a host and
Test locally would open.

| Keep | Why |
|---|---|
| Every setting of a game a named option (`--mod-option NAME=VALUE`, read by `ctx.Option`), not a constant in a mod | a game file is then a list of them |
| A server's policy on players' packs (proposal 1) a setting like the others | the file carries it |
| Workshop items named by their hash | any copy of the file gets the same game |
| Test locally and the game's host start `cb_server` from options | they open the file later instead |

## Before launch

| What | Why | State |
|---|---|---|
| **The cooked-item reader fuzzed, files limited in size** | after [6b](#6b-cooked-items) an item is read by our reader alone (and the Ogg Vorbis and ozz readers behind it). Until then workshop items are Godot files on every player's machine: neither the validator (tightened: M262) nor the guard makes Godot's parsers safe | not started |
| **Everything a client reads from a server fuzzed** | a client loads the server's whole world on join: the simulation's image (the ECS state, and Box3D's own world bytes through `b3DeserializeIntoShell`), the schema, the map, the sheets' text; then states and frames. Every decoder fuzzed, handles checked after loading. Or the game joins only official or listed servers at launch ([step 0](#0-decisions)) | not started (errors instead of aborts for the simulation's image: M266) |
| **Terms for modders, a licence for mods** | `LICENSE` is "all rights reserved": nothing lets a modder use the SDK or says what becomes of uploads (avatars included) | not started |
| **Updates** | | needed for [the alpha](#4b-the-alpha), described there |
| **Cheating, decided** | inputs model: every client holds the whole world. Hybrid: every character is sent, so a client knows where everyone is. Wallhacks are trivial in both; fog of war for characters is possible in the hybrid, not built. Lag compensation trusts how far back a client says it saw, no further than its round trip explains (M265) | open |
| **A server's policy on players' own packs** (proposal 1; their restyles are cooked items after [6b](#6b-cooked-items)) | the server says none, listed hashes, or any (Counter-Strike's `sv_pure`) in its info (`MsgInfo`); the game loads only what it allows | decided; to do |
| **What an item was made for** (proposal 7) | baked motions and graphs carry a format version, an item does not: nothing says the engine and Godot versions it was made for or the names it reads | decided; the cooked item's manifest ([6b](#6b-cooked-items)) |
| **Shaders in items** (proposal 7) | the validator refuses scripts only, and a pack's shaders run on every player's GPU: refused, or a checked subset, decided with the cooked-item reader ([6b](#6b-cooked-items)); a modder's shader source would be read by Godot's shader compiler | decided; to do |
| **GDScript out of the shipped game** | the game's own scripts moved into the extension, export templates built without GDScript: no script engine for a pack to reach. Not needed for content once [6b](#6b-cooked-items) ships | postponed (2026-11-17) |
| **A mod's crash** | takes the server down (mods are compiled in) | [step 10](#10-server-rules-in-luau) |
| **Rules from a store** (proposal 7) | only once the Luau sandbox holds | [step 10](#10-server-rules-in-luau) |
| **How players find servers** | join by address only | [parked](#parked); needed for launch |
| **Compatibility, promised** | what launch keeps (the sheet, a cooked item, the rules' API); a format change after launch is versioned and announced | to write |
| **The manual audited** | DESIGN, README, `docs/looks.md`, `mods_src/README.md` and this roadmap were checked against the code (M261.5 to M261.7; M261.8 shortened DESIGN and the roadmap; M261.9 corrected what each says about the pack check, in `sdk/README.md`, `server-mods.md` and `testing.md` too); `docs/characters.md`, `items.md`, `motions.md`, `maps.md`, `testing.md`, `playtest.md`, `server-mods.md` and `sdk/README.md` have not been read whole against it | to do |
| **Tools on every platform, translations** | the scripts are PowerShell; the extension is built from source | steps [9](#9-cinderbox-studio), [12](#12-godots-own-used) |

## Later

In the order they would likely come back.

### A thousand in one crowd

1023 players run in one crowd today, below 60 Hz. What is left, each measured
([docs/scaling.md](docs/scaling.md)):

| Where | What is left |
|---|---|
| The server's thread (55 ms a tick at 1023) | the mods' loops over every player (about 2 µs a player each); the motions' pass and the slots on one thread; keeping a state; the physics |
| Planning states | each client walks every entity: an interest grid would help where players are spread out |
| Bandwidth (1.7 to 1.9 Mbit/s a player) | frames are 70%, mostly pushers' places (exact every tick: decided); far characters could be coarser |
| The game (5 frames a second with all in view) | held items made as they come into sight ([6b](#6b-cooked-items) draws items as data); the mirror's and the HUD's work; Godot drawing a thousand skinned characters (a GPU crowd for far ones) |

Not before launch. The cheap way to more players is many servers of 50 to 255 per game, as Roblox does.

### Our own animation authoring

Decided after M231: when the time comes, the animation the simulation runs gets its own authoring,
apart from Godot's; `AnimationTree`s stay for what is only seen. Step 8 would be its core. Needs
more decisions first.

### Ragdolls for any skeleton

A non-humanoid skeleton has no ragdoll today (the critter dies without one). Authored Godot's way
(`PhysicalBone3D` nodes: shapes and joints), baked into a ragdoll the simulation builds for that
character, its parts and joints in the schema.

### Vehicles

Joints in templates (several bodies joined by Box3D's revolute, prismatic, weld and wheel joints,
authored as nodes); a seat (the mover off, the player carried and drawn in the vehicle's pose);
input routed to motors and forces as a motion routes it to the mover (who drives, damage: a mod's);
platform carry; every body says how it is driven, and possession takes its controls with it.

### Building

Players placing pieces (walls, ramps, floors): a sheet statement that places a template on a grid
in front of the player, predicted like a launch; a ghost preview from the same cast; pieces welded
where they touch (glue, M202), each breakable; costs and permissions a mod's; saving by a server
mod; caps per player and server, static pieces where possible (every piece is snapshot state).

### A deterministic VM for heavy predicted logic

WebAssembly, for what a sheet cannot say and must still be predicted (an NPC's brain): any source
language, memory cheap to copy for rollback, deterministic with canonical NaNs and no threads, an
instruction budget. An escape hatch, not the main way: modders would need a compiler.

### One world across servers

A seamless world split across server processes (meshing): a company-sized project (Star Citizen
took about a decade). Many servers per game come first.

## Parked

Decided to wait; each says what it waits for.

| What | Waits for |
|---|---|
| How players find servers | a decision, taken later (the owner, 2026-11-17): a website's "play" link, the game's own list, a desktop shortcut (not Steam). Today: join by address, Test locally, a server's mods listed before joining. **Needed for launch**; [a game as one file](#postponed-kept-open) comes back with it |
| The store's form | Cinderbox's own (not Steam, 2026-11-17), described in [6b](#6b-cooked-items); a folder today |
| Ships | Box3D growing buoyancy rather than more patches (five already). In the simulation, so everyone predicts them: platform carry, a buoyancy component with a wave function the water's look shares |
| **The player's body, for motions** | a mod that wants it; each a word of the sheet language ([planned list](docs/rules-language.md#planned)), in the simulation, so predicted. In likely order: |
| · Touching a wall | a condition (`wall`) and a frame along it: wallrunning, wall jumps, a ledge grab |
| · Teleport | set where the player is: behind the target, or at a cast's point. Blink, swapping places |
| · What the body passes through | `pass_players`, `pass_props`, `pass_level`: dashing through, phasing, a spectator through walls |
| · Not hittable | `hittable`: shots, casts and swings pass the player (a dodge's invulnerable frames, or a [window](#7-timing-from-the-clip)) |
| · The capsule's size | height and radius as parameters, so a crouch fits under a gap; feet stay; no standing up where the full height would not fit |
| · Keep speed, steer | turn the velocity without changing its size: a glide, a grind rail, a ricochet |
| · Hidden from others | a field the viewer reads to fade a body for everyone but its player: invisibility, a spectator |

## Leftovers, by area

Not scheduled. A row moves into the road when the slice wants it.

| Area | What |
|---|---|
| Physics | on CI's macOS arm64 build, now and then a float in Box3D's contact data (most likely `totalNormalImpulse`) differs between two simulations that should match, every body still identical; Windows and Linux match byte for byte. `net_physics_alone` reports it there instead of failing. Expected to go with a newer Box3D, or looked at again if it ever moves a body |
| Items | a state machine or motion reading the hand's fields; a lifetime for an item not launched; a fuse that starts when the pin is pulled; bots that use what mods add; a bot count in the menu |
| The inventory | a split of one; a slot's item shown while dragged over; a key for the bag; dropping or moving part of a stack; a mod taking one off a stack without throwing it; a corpse's bag (undecided); predicted answers to a mod's intents; intent numbers wider than a byte |
| Sheets and HUDs | a sheet for a list's rows; an item's HUD drawn on its slot |
| Motions | a lasting force as a C++ command; a test of one player hooked to another; a knocked-down ragdoll state; carrying a player |
| Animation | a slow-motion finisher (a state's speed exists, M230); the dash's and the hook's poses are placeholders |
| Feet on the ground | foot IK in the shared pose: each foot's ground by a ray in the simulation, the hips lowered to the lower foot, two-bone IK per leg, so leg hitboxes are where the feet are drawn |
| The client | keys the server says (a server maps keys to actions at join; players may still rebind, per server); cameras a mod places, with [the camera as data](#postponed-kept-open); two mods' keys may clash and nothing says so (chat's text entry, `CbTextEntry`, opens on T by default, and `flight`'s `fly` action suggests T) |
| Games | teams and spectators (mods, once private fields and the board cover them); lists on one entity (array fields, or a `CbList` over a field family) |
| Finding the way | one mesh per character size; mending the mesh when a wall breaks; off-mesh links (a jump, a ladder) |
| NPCs | NPCs of their own character; a brain in a sheet (predicted exactly, at a rollback cost) |
| The uplink | an input packet every other tick would cut 34 kbit/s up to about 20 and delay inputs 8 ms on average: not taken (M261) |

## Decided earlier (still holds)

| Decision | Why |
|---|---|
| Possession, not an NPC type | a player is a controller (keys, name, score); what it drives is a body. NPCs, spectators, Prop Hunt and mind control follow from it |
| The controller is a part, not a kind | a body nobody drives costs what a prop costs; the controller's pass meets only driven bodies |
| A body's scene decides how it is driven | its capsule, a first-person camera, a skeleton and state machine, its movement numbers |
| The mover stays C++ | the hottest loop (every player, every tick, again in every rollback); sheets are bounded on purpose |
| Walls static until broken; debris players cannot push (`CatDebris`); settling by Box3D's sleeping | a wall costs nothing in rollback; turning sleeping pieces static would save next to nothing |
| The ozz pose owns the body | hit tests use it, and the game draws exactly it |
| Physics players react to is shared | ragdolls and falling walls are the same on every screen; cosmetic physics is for what nobody could compare (capes, spring bones, casings) |
| The UI stays presentation-only | a screen shows fields and sends intents; what it cannot say becomes a widget the engine gives, not a script |
| Mods' files never cross the game connection | a server names the workshop items it needs by hash; a client finds them in its store |

## Routes not taken

| Route | Why not |
|---|---|
| Server mods also running on clients | tried as M55 and dropped: rules would ship to clients and every mod would need determinism. The sheet language took its place |
| A built-in dash, a built-in grapple | rules in the engine; mods could only tune what the engine thought of |
| The viewer guessing a command (`CbPrediction` with a push) | the viewer has no simulation: a guessed push cannot collide, and a pull would fight every frame's correction |
| Separate physics for the map and the game | there is one world; a map's bodies and a mod's are authored the same way (M185) |
| Godot's physics at runtime | collision is Box3D's on the server and in every prediction; `CollisionShape3D` is only how shapes are authored |
| Hitboxes as physics bodies | every bone moved every tick, copied in every snapshot, shoving props with its limbs; hitboxes are shapes on the shared pose that hit tests ask |
| A fork of Godot | merging every release by hand. The extension's editor hooks cover [step 9](#9-cinderbox-studio). (An editor of our own, also listed here before, was raised again on 2026-11-17 and is open: [step 0](#0-decisions)) |
| Leaving Godot over the pack hole (asked 2026-11-17) | the hole is a check of ours and Godot files from strangers, not Godot's renderer; any engine that loads a rich object format from strangers has it (Unreal's packs can carry Blueprint code), and a renderer, UI, audio, platform layer and import of our own would cost a year. Cooked items end it instead ([6b](#6b-cooked-items)) |
| The Steam Workshop (2026-11-17) | not on Steam: the store is our own ([6b](#6b-cooked-items)) |
| Another engine (2026-11-08) | the simulation, netcode, animation and presentation logic are our own C++; Godot's part (renderer, UI, the modders' editor) is what a switch would lose. The scene tree's cost at scale was removed instead (M260) |
| Sheets in Godot's script slot | "every script is refused" is the security story; a script language through GDExtension is large and fragile for two features |
| Sheets as `.res` or `.tres` | Godot's generic containers would hold the sheet as one quoted string: poor diffs, no editing as text |
| A visual graph for skills | hard to diff and review; Godot dropped VisualScript for lack of users |
| A three-letter extension | gains nothing (Godot's own are long); the `cb` three-letter names are taken (comic archives, Code::Blocks, COBOL) |
| Body scales for avatars | the standard skeleton only: held items and hitboxes match the drawing |
| Everyone simulating the whole world as the default | the rollback window's cost grows with every body; kept as a server's choice for small servers |
| The server's state as the second model (M244, M245) | it sends the physics world as state: bandwidth grows with every moving body, a destruction is hundreds at once, and the determinism goes unused. The hybrid keeps the physics deterministic and sends only its events |
| Tiers of drawing (2026-11-06) | one way for every character, varying only detail by distance (M260) |

## Housekeeping

Independent of the road; each is small unless it says otherwise.

| What | Note |
|---|---|
| `Simulation` in modules | `simulation.cpp` about 7,000 lines, about 200 methods and 100 members; the mover went first (M92). Split where steps 5 to 8 touch it: slots and items, motions, possession, breakables, sending states. The second review (2026-11-17) proposes the pure move into files by area now: the reference hashes prove nothing changed |
| Typed commands | a `SimCommand`'s `mode`, `index`, `value`, `a`, `b` and `c` mean something else for each command, said only in comments (`types.h`), written in `mod_api.cpp` and read in `simulation.cpp`: a pack and an unpack per command in one header, the wire unchanged; before Luau binds the API (review, 2026-11-17) |
| What a follower skips, in one place | the hybrid sorts commands in `FollowerSkips` (the client), `FollowerSkipped` (the server) and a NetId fix-up written twice (`simulation.cpp:3981`, `FollowerSkipped`); a new command is classified in each, and a slip desyncs: one table per command type, or the server's NetId in the command (review, 2026-11-17) |
| The state thread | a new `std::thread` every state (30 a second): one thread that lives, and a debug check in every function that changes a client while a state is made (review, 2026-11-17) |
| `CinderboxClient` | one class in five files, about 140 methods |
| `GameServer` in modules | 3,211 lines: connections, the schema, the mods, states, the bench. Split when [step 10](#10-server-rules-in-luau) starts: the Luau host replaces part of it |
| Long functions | `ParseRules` 1,198 lines (step 6 replaces it), `CompileMotionSet` 957, `CbCharacter::BakeGraph` 765, `Simulation::ApplyCommand` 529, `GameServer::Start` 497, `Simulation::MoveCharacters` 433: table-driven parsers, one builder for the schema |
| The bench inside the server | `game_server.cpp` names the bench or fake players 132 times: a profiler interface and a fake transport |
| Box3D's pin | a commit 21 past its `v0.1.0` tag, with five patches: pinned until launch; an upgrade is a milestone of its own |
| `tests/test_main.cpp` in files by area | about 13,800 lines; `tests/net_test.cpp` about 8,400. For reading: alone, `test_main.cpp` recompiles in about 17 s and a file of the extension in 2 to 7 s (2026-11-17), so build times are not the reason |
| The docs' weight | about 830 KB of Markdown for about 77k lines of C++; the history is 361 KB, with lines up to 5,400 characters; code comments carry 370 milestone tags. Proposed (review, 2026-11-17): the history a line per milestone with its merge commit; reference generated from the code (server options from `--help`, sheet words from the compiler); numbers that change named where they live, not quoted; a glossary (puppet, stand-in, pusher, sheet, look, board, mirror) |
| Replays' extension | `.cbr` is also the comic-book archive: `.cbreplay` |
| A motion's field and a mod's | both may write one (commands first, then motions); the server could warn when a mod sets a field a motion writes every tick |
| Slots for what is worn | the dash boots take a slot: selected, they are held like a tool |
| `reconcile_every` 2 as the inputs model's default | halves rollback work with many players and latency (M139); others are seen up to 2 ticks later: look at it in play first |
| Physics threads beside drawing | on by default (M141), not yet watched in the game: `--physics-threads=0` if frames suffer |
| The view codec's magic | `CBV7` since M252; M260 made an entity's record 36 bytes longer and bumped the view file's version (10), not the magic. Nothing breaks today, but the magic should change with the record |
