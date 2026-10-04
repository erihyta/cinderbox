# Cinderbox

A deterministic multiplayer third-person physics sandbox, built with flecs, Box3D, ENet and
ozz-animation, with a Godot 4 client (rendering, VFX, UI and mods).

| Part | How |
|---|---|
| The simulation | one deterministic C++ simulation on the server and every client, bit-exact across Windows, Linux and macOS/ARM64 and across Clang, GCC and MSVC |
| Netcode | an authoritative server, client prediction and rollback, recordings that replay |
| Rules | C++ server mods that change the world only through commands: `inventory`, `combat`, `pistol`, `rifle`, `melee`, `pickup`, `deathmatch`, ... |
| Looks | Godot scenes with no code (reactions, predictions, HUD nodes), shipped as workshop items |
| Motions | what mods add to movement, authored as nodes, baked and run by every simulation, so a player's own are predicted |
| Authoring | maps, characters, state machines and item bodies are made in the Godot editor and baked for the server |

Milestones M1 to M98 are done; [docs/HISTORY.md](docs/HISTORY.md) lists them.

## The manual

| Read | For |
|---|---|
| this page | building, playing, the controls |
| [docs/server-mods.md](docs/server-mods.md) | writing a mod's rules, the shipped mods, private fields, hit tests, workshop items, the SDK |
| [docs/looks.md](docs/looks.md) | reactions, predictions, the expression language, HUD nodes, the Cue Preview, client mods |
| [docs/characters.md](docs/characters.md) | characters, state machines, layers and stances, aiming, animation packs |
| [docs/items.md](docs/items.md) | held items, sockets, the inventory, grips, items in the world |
| [docs/motions.md](docs/motions.md) | what a mod adds to movement (a dash, a double jump, flight, a jetpack, a grappling hook), predicted: `CbMotion` |
| [docs/maps.md](docs/maps.md) | maps, templates and components |
| [docs/testing.md](docs/testing.md) | the tools, the determinism checks, CI |
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

```sh
# terminal 1
cb_server --port 7777
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
- Everything else comes from the server's mods, bound to the keys they suggest. With the shipped mods:
  1 to 4 switch slots (hands, pistol, bat), the left mouse button fires or swings, R reloads, E picks
  up, G throws, V dashes (twice, then they come back), Q throws a grappling hook at what is
  under the crosshair and pulls you there (Q again lets go), and F with empty hands spawns a prop.
  Switched off by default: the crouch (C, the `sneak` mod), the `flight` mod (T flies, Shift
  glides, and its jetpack), and the second jump in the air
  ([what is off](docs/server-mods.md#switched-off-by-default)).
- Tab shows the scoreboard, the mouse orbits the camera and the wheel zooms.
- The key left of 1 (`` ` `` / `~`) switches between the camera behind the player and **first person**:
  from the character's eye height above its feet, on its mover, so steps, landings and the bowing
  body do not move it. Both views look 86 degrees up and down; from behind, the map pulls the
  camera in where it would go under the floor, so looking up ends with the camera at your feet.
- **Your own body in first person** is the same body with the same animations, drawn for that view:

  | | |
  |---|---|
  | What is drawn | the arms, and everything from the hips down. The torso, the neck and the head are not (the shadow is the whole body's) |
  | The arms | a steady pose of the same animations (standing still, not yet aimed), put under the camera as one piece and turned with it: the stance's own turn of the shoulders and the hands' place on the item are exactly the animation's. The walk's sway never reaches them, so what is held neither swings across the screen nor tilts with each step, and it keeps its place on the screen wherever you look. What the arms do themselves (a shot's recoil, a reload) shows |
  | Where they sit | each item's look can move them: `CbItemLook.view_offset`, metres to the right, up and ahead (the pistol: 5 cm up, 3 cm ahead). Keep it small: far from the body the arms' cut ends come into view |
  | Who sees it | only you. Other players, your shadow's pose source, hit tests and where shots start are the body's real pose |
  | Both hands | an item with a grip ([Both hands on an item](docs/items.md#both-hands-on-an-item)) has the other hand on it here too: it is solved again after the arms are pinned |
  | Limits | a stance that holds an item low or behind the body (the bat's) is out of view and an offset cannot bring it in: that takes an animation made for the view. The built-in box rig is drawn whole |
- Nothing in the shipped looks shakes the camera. (`CbReaction.shake` still does, for a mod that wants it.)
- Z moves the third-person camera over the right shoulder, the left, and back behind.
- **What is under the crosshair is what a shot is aimed at, in every view.** The shot itself always
  starts at the head: aiming over cover you are hidden behind hits the cover.
- Esc opens the in-game menu (see [The menu](#the-menu)), F1 toggles the debug HUD.

The HUD shows the predicted and confirmed ticks, round-trip time, clock error, rollbacks, stalls and
checksum results.

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

`cb_server` is in `<build dir>/bin`. Its options are `--tick-rate`, `--seed`, `--substeps`,
`--prop-lifetime`, `--props-per-player`, `--props-global`, `--move NAME=VALUE`
([movement parameters](docs/server-mods.md#movement-parameters)), and `--mods A,B` / `--mods none` / `--list-mods`
(every compiled server mod runs by default, except the ones switched off: see [docs/server-mods.md](docs/server-mods.md)).

## License

Copyright (c) 2026 erihyta. **All rights reserved.** Cinderbox is not open source: the repository
is public to be read, and no permission is given to use, copy, modify or redistribute it without
written permission (see [LICENSE](LICENSE)). Third-party libraries and assets keep their own
licenses.

## Credits

- Default character and its animations: [Universal Animation Library](https://quaternius.com) by
  Quaternius, CC0 (`godot/characters/mannequin/source/LICENSE.txt`).
