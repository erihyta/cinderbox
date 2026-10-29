# Cinderbox

A deterministic multiplayer third-person physics sandbox, built with flecs, Box3D, ENet and
ozz-animation, with a Godot 4 client (rendering, VFX, UI and mods).

| Part | How |
|---|---|
| The simulation | one deterministic C++ simulation on the server and every client, bit-exact across Windows, Linux and macOS/ARM64 and across Clang, GCC and MSVC |
| Netcode | an authoritative server, client prediction and rollback, recordings that replay |
| Rules | C++ server mods that change the world only through commands: `inventory`, `combat`, `gun`, `melee`, `brawl`, `pickup`, `deathmatch`, `waves`, ... |
| The rules language | what moves on screen (an item's use, a dash, a hook, a skill) is a sheet the simulation runs, so it is predicted: [docs/rules-language.md](docs/rules-language.md) |
| Looks | Godot scenes with no code (reactions, predictions, HUD nodes), shipped as workshop items |
| Bodies | a player drives a body: its own character, another character's, or a loose prop; a mod drives bodies too (NPCs), which walk the server's navigation mesh |
| The world | physics props, destructible walls broken into authored pieces, items in the world, containers |
| Authoring | maps, characters, their animation trees, items and motions are made in the Godot editor and baked for the server |

Milestones M1 to M249 are done; [docs/HISTORY.md](docs/HISTORY.md) lists them. The roadmap was rewritten after M234 from a review of the whole project ([ROADMAP.md](ROADMAP.md)).

## The manual

| Read | For |
|---|---|
| this page | building, playing, the controls |
| [docs/server-mods.md](docs/server-mods.md) | writing a mod's rules, the shipped mods, private fields, hit tests, workshop items, the SDK |
| [docs/looks.md](docs/looks.md) | reactions, predictions, the expression language, HUD nodes, the Cue Preview, client mods |
| [docs/characters.md](docs/characters.md) | characters, what an animation tree bakes to, layers and stances, aiming, animation packs, bodies and possession, NPCs |
| [docs/items.md](docs/items.md) | held items, sockets, the inventory, grips, items in the world |
| [docs/motions.md](docs/motions.md) | what a mod adds to movement (a dash, a double jump, flight, a jetpack, a grappling hook), predicted: `CbMotion` and its parts (impulses, forces, probes, links); what players weigh |
| [docs/rules-language.md](docs/rules-language.md) | the rules language's reference: every line a sheet may hold, where it may stand, what it reads |
| [docs/maps.md](docs/maps.md) | maps, templates and components |
| [docs/testing.md](docs/testing.md) | the tools, the determinism checks, CI |
| [docs/playtest.md](docs/playtest.md) | the playtest checklist, and playing two games against yourself |
| [sdk/README.md](sdk/README.md) | the Godot project a mod's look is made in |
| [mods_src/README.md](mods_src/README.md) | client mods: the files the game looks up |
| [DESIGN.md](DESIGN.md) | how it works, by subsystem; the source layout; known limits |
| [ROADMAP.md](ROADMAP.md) | what comes next |


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

`clang-release` and `unix-clang-release` also build the two Godot extensions (`CB_BUILD_GODOT`, via
godot-cpp) into `godot/bin/` as `template_debug`: `libcinderbox` (the viewer) and
`libcinderbox_peer` (the simulation and the networking, for joining servers). `godot-export` builds only their
`template_release` versions, used by exported games. The other presets skip Godot, so they never
overwrite those DLLs.

Godot learns of an extension when the project is imported: after the first build (or after
pulling a change that adds one), open `godot/` in the editor once or run
`godot --headless --path godot --import`.

## Playing

The client is the Godot project in `godot/` (Godot 4.7 or later).

**Trying things on this machine:** the menu's **Test locally** starts a `cb_server` of this build
with its default mods, joins it, and stops it when you leave. From the command line:

```sh
godot --path godot -- --host-local                      # the default mods
godot --path godot -- --host-local=combat,waves --bots=2 # only these mods, and two bots
godot --path godot -- --host-local --server-arg="--map test_ground --mod-option waves.first=10"
powershell -ExecutionPolicy Bypass -File tools\test_mod.ps1 -Mod waves   # build, publish a mod's look, then the same
```

**A server of its own:**

```sh
# terminal 1 (the built-in sandbox, or a map baked in the editor: godot/maps/<name>.cbmap)
cb_server --port 7777
cb_server --port 7777 --map example_arena
# terminal 2, 3, ...: the Godot client (after building clang-release); it opens in the menu
godot --path godot
# or straight into a server
godot --path godot -- --host=127.0.0.1 --port=7777
# or watch a recording (cb_server --record FILE), or a view file (cb_server --record-view FILE)
godot --path godot -- --replay=FILE
godot --path godot -- --view=FILE
```

### The menu

The game starts in a menu (`res://ui/menu.tscn`, driven by `menu.gd`):

| Where | What |
|---|---|
| Join | Your name, a server address (`host` or `host:port`, port 7777 if left out), and the servers you joined last (one click to join again) |
| Esc, in game | Resume, Settings, Leave server, Quit. The game goes on behind it; you stop moving |
| Settings | Mouse sensitivity, volume, fullscreen |

A join that fails comes back to the menu and says why:

| What happened | What it says |
|---|---|
| The text is not an address | What an address looks like |
| The host name does not exist | The address could not be found |
| Nobody answers within 10 s | No answer: check the address and port, that the server runs, that its UDP port is open |
| The server refuses (full, another build) | The server's reason |
| A workshop item is missing or refused | Which one |

- Name, settings and recent servers are saved in `user://player.cfg` (`--config=FILE` for another file).
- The menu has no script, so a client mod can restyle it: `menu.gd` finds its nodes by unique name (listed at the top of `menu.tscn`).
- Leaving a server reloads the game scene, so nothing of it is left. Resource packs cannot be unloaded, so joining a server that does not use an item loaded earlier restarts the game, straight into that server.
- The camera stays out of the map: walls, floors and other static geometry pull it in at once and it eases back out. Props and players never block it.

### Controls

- WASD moves, Shift sprints and Space jumps: the engine's own controls.
- Everything else comes from the server's mods, bound to the keys they suggest, and every key can
  be changed on the **Keys** page (Settings). With the default mods:

  | Keys | What |
  |---|---|
  | 1, 2, 3, ... | the hotbar's slots. Each item goes to the slot it asks for, else the first free one. The brawl kit's come first: the fist (1, in the hand), then its skills, which the slot's key uses at once (the dash strike, the uppercut, the ground slam, the guard); then the pistol, the rifle, the grenade and the rest. The same key again empties the hands |
  | left / right mouse | use what is in the hand / its second use |
  | V, Q, H | the dash boots (twice, then they come back), the grappling hook (Q again lets go), a grenade: each item's own key, from wherever it is carried |
  | R, E, G, F | reload; pick up (or open a chest you look at); throw what is in the hand; spawn a prop with empty hands |
  | I, B, T, Tab | the bag (drag items between slots); the shop; chat (Enter sends); the scoreboard while held |

  Switched off by default ([what is off](docs/server-mods.md#switched-off-by-default)): the crouch
  (C, `sneak`), flight (T, `flight`), the impact grenade (J, `grenade`), the cannon (N, `cannon`),
  taking over the nearest body (P, `possess`), and the waves of NPCs (`waves`).
- The mouse orbits the camera and the wheel zooms.
- The key left of 1 (`` ` `` / `~`) switches between the camera behind the player and **first person**:
  from the character's eye height above its feet, on its mover, so steps, landings and the bowing
  body do not move it. Both views look 86 degrees up and down; from behind, the map pulls the
  camera in where it would go under the floor, so looking up ends with the camera at your feet.
- **Your own body in first person** is the same body with the same animations, drawn for that view:

  | | |
  |---|---|
  | What is drawn | the arms, and everything from the hips down. The torso, the neck and the head are not (the shadow is the whole body's) |
  | The arms | a steady pose of the same animations (standing still, not yet aimed), put under the camera as one piece and turned with it: the stance's own turn of the shoulders and the hands' place on the item are exactly the animation's. The walk's sway never reaches them, so what is held neither swings across the screen nor tilts with each step, and it keeps its place on the screen wherever you look. What the arms do themselves (a shot's recoil, a reload) shows |
  | Where they sit | each item's look can move them: `CbItem.view_offset`, metres to the right, up and ahead (the pistol: 5 cm up, 3 cm ahead). Keep it small: far from the body the arms' cut ends come into view |
  | Who sees it | only you. Other players, your shadow's pose source, hit tests and where shots start are the body's real pose |
  | Both hands | an item with a grip ([Both hands on an item](docs/items.md#both-hands-on-an-item)) has the other hand on it here too: it is solved again after the arms are pinned |
  | Limits | a stance that holds an item low or behind the body (the bat's) is out of view and an offset cannot bring it in: that takes an animation made for the view. The built-in box rig is drawn whole |
- Nothing in the shipped looks shakes the camera. (`CbReaction.shake` still does, for a mod that wants it.)
- Z moves the third-person camera over the right shoulder, the left, and back behind.
- **What is under the crosshair is what a shot is aimed at, in every view.** The shot itself always
  starts at the head: aiming over cover you are hidden behind hits the cover.
- Esc opens the in-game menu (see [The menu](#the-menu)), F1 toggles the debug HUD.

The HUD shows the network model, the predicted and confirmed ticks, round-trip time, clock error,
rollbacks (in the hybrid: states taken), stalls and checksum results.

Server and clients can be built with different compilers. The server rejects a client whose
simulation fingerprint differs from its own.

### Watching a recording

`--replay=FILE` plays a server's recording in the game instead of joining. It looks like the session
did: the recording names the workshop items it needs, and the player it follows is the local one,
so its HUD, its hit markers and its camera target are what that player had.

| Key | Does |
|---|---|
| Space | pause |
| Left / Right | 5 s back / on |
| Up / Down | twice / half the speed (0.125x to 16x) |
| `,` / `.` | one tick back / on, paused |
| Home | from the start |
| N | follow the next player |

The recording must come from the same build of the simulation (the stats say `build_matches`), and
checksums it does not reproduce count as `desyncs`.

`--view=FILE` plays a **view file** the same way, with the same keys. It holds the frames
themselves, not inputs to re-simulate, so it plays on any build and shows the players' names; it
is also much bigger (about 0.1 MB a second for 4 players, 1.2 MB for 64; with `--view-rate 20
--view-compact`, 0.01 and 0.09 MB).

Open `godot/` in the Godot editor to edit scenes, then press Play. Godot client options, given after `--`:
- `--host=H`, `--port=P`: join this server without the menu (leaving it lands in the menu).
- `--replay=FILE`, `--view=FILE`: watch a recording or a view file instead (see above).
- `--name=NAME`: your name (otherwise the one typed in the menu).
- `--config=FILE`: where name, settings and recent servers are kept (default `user://player.cfg`).
- `--workshop=DIR`: where subscribed workshop items are (default `user://workshop`).
- `--rollback=N`: fixes the prediction window.
- `--mods=DIR`: an extra mod folder.
- `--autoplay=SECONDS`, `--screenshot=FILE`: an unattended smoke test, on a server or a recording. The exit code is non-zero on a desync.

### Exporting the Godot client (Windows)

```powershell
cmake --preset godot-export; cmake --build --preset godot-export
powershell -ExecutionPolicy Bypass -File tools\export_client.ps1     # -> dist\Cinderbox\Cinderbox.exe
```

The export needs the Godot 4.7.2 export templates, installed either from the editor or extracted to
`%LOCALAPPDATA%\cinderbox-build\tools\godot\templates`. Packs in `mods\` are copied to `dist\Cinderbox\mods`.

`cb_server` is in `<build dir>/bin` (`cb_server --help` lists everything):

| Option | What |
|---|---|
| `--port N`, `--tick-rate HZ`, `--seed N`, `--substeps N` | the server itself |
| `--map NAME` or `--map FILE.cbmap` | a map baked in the editor (`godot/maps/`); without it, the built-in sandbox |
| `--mods A,B`, `--mods none`, `--list-mods` | which server mods run: every compiled one by default, except the ones switched off ([docs/server-mods.md](docs/server-mods.md)) |
| `--mod-option NAME=VALUE` | a mod's option (`waves.first=10`, `deathmatch.kills=20`); each mod's are in its file's top comment |
| `--character NAME [--workshop DIR]` | the character everyone plays as ([docs/characters.md](docs/characters.md)) |
| `--move NAME=VALUE` | how players move ([movement parameters](docs/server-mods.md#movement-parameters)) |
| `--prop-lifetime SEC`, `--props-per-player N`, `--props-global N` | the props players spawn |
| `--record FILE`, `--record-view FILE` | a recording, a view file (see [Watching a recording](#watching-a-recording)) |
| `--net-model inputs\|hybrid` | how players' games are kept: every game simulating the whole world from everyone's inputs (the default), or the hybrid: the physics from the server's events, the characters its states, your own predicted ([DESIGN.md](DESIGN.md#netcode)) |
| `--physics-threads N`, `--exit-when-empty SEC`, `--quiet` | threads for the physics; stop once nobody is connected; less output |

## License

Copyright (c) 2026 erihyta. **All rights reserved.** Cinderbox is not open source: the repository
is public to be read, and no permission is given to use, copy, modify or redistribute it without
written permission (see [LICENSE](LICENSE)). Third-party libraries and assets keep their own
licenses.

## Credits

- Default character and its animations: [Universal Animation Library](https://quaternius.com) by
  Quaternius, CC0 (`godot/characters/mannequin/source/LICENSE.txt`).
