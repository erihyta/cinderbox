# Testing

The tools, the determinism checks and CI. Part of the [manual](../README.md#the-manual).

## Testing tools

All tools are in `<build dir>/bin`.

| Tool | What it does |
|---|---|
| `cb_server --map FILE.cbmap` | Runs an authored map instead of the built-in sandbox |
| `cb_server --record FILE` | Records the whole session (input frames plus checksums) |
| `cb_replay info\|verify FILE` | Summarizes a recording, or re-simulates it and checks every checksum |
| `cb_server --record-view FILE [--view-rate HZ] [--view-compact]` | Records the session as a view file: the frames a viewer is shown, playable without a simulation. `--view-rate 20` keeps 20 frames a second instead of one per tick; `--view-compact` writes them the way a stream would (positions and animation on a grid, a few millimetres off at most) |
| `cb_replay view FILE` | Summarizes a view file: frames, players, bytes per frame and where they go |
| `godot --path godot -- --replay=FILE` (or `--view=FILE`) | Watches a recording (or a view file) in the game, with the mods' looks and the followed player's HUD |
| `cb_netsim --listen P --target HOST:PORT --latency MS --jitter MS --loss % [--duplicate %]` | UDP relay that degrades traffic (latency is added in each direction) |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_mod_validator.gd -- PACK.zip...` | Checks the pack validator: the named packs pass, built-in hostile packs are refused |
| `cb_bot --port P --count N --full M --duration S [--chaotic] [--shoot] [--melee]` | Headless players; the M "full" bots run prediction and rollback and report its cost; `--chaotic` changes every input every tick; `--shoot` makes full bots take out the pistol and fire at the nearest player; `--melee` makes them close in with the bat and swing |
| `godot --path godot --script res://addons/cinderbox_maps/check_menu.gd -- --test-port=P --config=FILE [--shots=DIR]` | Drives the menus against a running server: bad address, unknown host, dead port, join, camera, Esc menu, settings, leave, rejoin from the recent list. With `--other-port=P2 --result=FILE` (a server running other mods) also the restart that joins it |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_keys.gd -- --config=FILE` | The Keys page of the settings, with no server: an action's key changed by a key press and by a mouse button, left alone with Esc, a wheel refused, a screen's own key (`CbKey`) listed and changed, the choices remembered by a new start and by joining a server again, and reset. Exit code 0 when everything held |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_guard.gd` | Checks the scene guard: the game's own scenes pass, and scenes with an `HTTPRequest`, a script, a wired signal, a climbing path or an animation that calls `queue_free` are refused |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_predictions.gd` | Checks predictions on a bare director: a press plays its cue at once, the server's cue then plays only what waited, another player's cue is never an echo, conditions and cooldown hold a press back |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_motion_blocks.gd` | Checks what a rules sheet's motion blocks bake to: steps, ifs as guards, a timeout, an end, an edge, a move's seconds, an item's use and who may run its others; finds (a region drawn in the scene, a box ahead turned to the simulation's forward, one written in the line); casts (a ball that flies, a blade drawn in the scene); a curve from a CbCurve (64 values, written before what reads it) and expressions as an impulse's and a force's strength; a marker's forward and place (turned to +Z), a turn, a path of 32 points; an item's own motion on its expiry (owner item, break, remove); every and an impulse of momentum; the SDK's starter sheet; an Area3D under an item left out of its body; the sheet's reactions still made, its motions not; and what refuses the bake, with its file and line |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_motions.gd` | Checks what the motion nodes bake to, on sets built in code: the file's text (impulses, forces, a probe, links, targets), and what refuses the bake (no action, a parameter that is none, a condition that does not parse, an effect on what a probe found without a probe, a rope to the player itself, an effect outside a motion, more than 16) |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_item_cues.gd` | Checks that an item's own reactions answer a cue at its holder only while the item is in use: of two items of one player, the one in the hand plays and the one put away does not; a world reaction and a reaction about the item itself are not silenced |
| `godot --headless --path <project> --script <repo>/godot/addons/cinderbox_maps/check_names.gd` | Checks the names every scene of a project uses (events, fields in conditions and formats, actions, item kinds) against what the server's mods declare (`cinderbox_names.cfg`, written by the server's build); exit code 1 with the node, the property and the closest name for each one nobody declares. Publishing a mod runs it |
| `cb_server --dump-names FILE` | Writes what every compiled mod declares; the server's build does it for every look project |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_movement.gd` | Checks what a character's `movement` bakes to, on the mannequin: `move.<name>` lines in the simulation's order, and what is refused |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_sheet_editor.gd` | What the editor says while a sheet is typed: its mistakes by line (a name in a rule's first line on that line), the names offered, each word's colour |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_rules.gd` | A rules sheet (`CbRules`): its rules become reactions that act (a while, a cue with a condition), its mistakes are said at their lines, the rest still works |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_grips.gd` | Checks what an item (its `CbItem`, body and grip markers) bakes to, on scenes built in code: carried at the origin, at a carrying marker, at a turned one, and what is refused |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_object_source.gd -- FILE.cbv` | Checks that the viewer draws from any object that hands it packets: a GDScript source reads a view file, with no peer extension involved |
| `godot --path godot -- --autoplay=S --screenshot=F.png --screenshot-every=S2` | Unattended client; also saves `F_1.png`, `F_2.png`, ... and prints the mod events it saw |
| `godot --path godot -- --host-local[=MODS] [--bots=N]` | The game starts a server of its own (`cb_server` on a port of its own, the mods it runs by default or the ones named), joins it, and stops it when it leaves; with `--bots`, `cb_bot` players too. The menu's **Test locally** button does the same. `--server-exe=PATH` picks the server (default: the one next to the game, else the newest build of the checkout); `--server-arg=...` passes it one more argument. With `--autoplay` it is an unattended run that needs no server started first |
| `cb_server --exit-when-empty SEC` | The server stops by itself once nobody has been connected for SEC seconds: a server the game started cannot be left running |
| `cb_server --physics-threads N` | Threads for the physics step besides the server's own (default: a quarter of the machine's, at most 3; 0: none). The same for the game: `--physics-threads=N`. What is simulated does not depend on it: `cb_tests physics_threads` compares every tick for several worker and thread counts, and `cb_tests stress` prints the physics part of a step on 1 and on 4 threads |
| `tools\test_mod.ps1 -Mod <mod> [-Bots N] [-Only] [-NoBuild] [-Autoplay SEC]` | One step from an edit to playing it: builds the server, publishes the mod's look, and starts the game hosting a server of its own with it. The editor's **Test mod** button (in a mod's client project) runs it. With `-Autoplay` it plays by itself and exits 0 when everything agreed |
| `tools\publish_mod.ps1 -Mod <mod>` while a server runs | The server takes the new look within a second when it changes nothing the simulation reads (`<mod> was published again: its look is ... now`), tells everyone connected, and their games load it over the old one and draw everything again. Otherwise it prints which baked files differ and keeps the look it started with |
| `godot --path godot --max-fps 60 -- --autoplay=22 --view-probe=N [--probe-frames=DIR] [--probe-use]` | The first-person view, measured (with `--probe-use` it uses what it holds once, a second into standing still, so that part's frames show the use; each part also prints where the hand is from the camera, which is what an item's `view_offset` is worked out from): the unattended player goes into first person with slot N out (0: empty hands) and stands, walks, sprints, strafes and turns by a plan; the run prints how far its hand moved against the camera in each part (it should not), and with `--probe-frames` keeps 48 drawn frames of each part to compare outside the game (what is drawn can differ from what the nodes say) |
| `godot --path server_mods/<mod>/client --script <repo>/godot/addons/cinderbox_maps/check_item_preview.gd -- --scene=res://prefabs/<item>.tscn --out=F.png [--aim] [--view] [--cue=NAME --after=SEC]` | Draws an item's in-hand preview (`CbItem.preview_hold`) outside the editor and saves the picture: the figure beside the item, or with `--view` what the holder sees in first person (the editor's panel); with `--cue` that cue is fired a second in and the picture taken `--after` seconds later (a swing, a shot). Exit code 1 when no figure appears, when what is in the hand is not one copy of the item, or when the other hand is not at its marker |
| `scripts/stress_test.sh --bots N --full M --latency MS --jitter MS --loss % --rollback T [--reconcile-every N] [--spin 1] [--physics-threads N] [--server-physics-threads N]` | Starts a server, the simulator and the bots, and prints a summary. For each full bot: where a second went (steps, snapshots saved and loaded, hashes: how many and what one costs) what a step is made of (commands, characters, physics, the rest), what the physics step is made of (pairs, collide, solve, ...) and what the physics world holds (bodies, how many awake, contacts). `--spin 1`: the full bots' threads never sleep, as a game that draws does not; without it a processor saving power runs them at a third of its speed and every time is too high. Times still differ two to three times between runs on a laptop (heat): compare runs made one after another |

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

`tests/reference_hashes.txt` holds the per-tick state hashes of three runs, one after another:

| Ticks | Run | What it covers |
|---|---|---|
| 0 to 1199 | the reference scenario | movement, props, kills and ragdolls, fields, events, players coming and going |
| 1200 to 1799 | with [motions](motions.md) | dashes, forces, probes, links |
| 1800 to 2699 | with [slots](items.md#the-inventory) | items given, the slots' keys, moves, splits, drops, items' own keys, the use button, chests filled and one destroyed |

Any change to simulation code or tuning legitimately changes them. Regenerate the file with
`cb_tests --dump tests/reference_hashes.txt` and commit it together with the change.

- **A red CI is not optional.** CI was red from M136 to M161 (tests that assumed the local rifle,
  a reference not regenerated), so the cross-load job was skipped and no build was compared with
  another for 25 milestones. They all agreed (M162 checked the dumps), but nothing said so.
- **Tests run without the licensed assets** in CI: count what a life starts with, don't assume it.
- **Net tests run every mod that runs by default.** A disabled one (the impact grenade, flight,
  sneak) only when the test names it: the mod option `test.mods` (`grenade`). The brawler's kit runs by
  default but takes the hotbar's first slots, where the tests expect the weapons: named too.

### Continuous integration

Every push runs `.github/workflows/determinism.yml` on GitHub Actions:

| Job | Runs |
|---|---|
| `windows-clang`, `windows-gcc`, `windows-msvc` | Windows x64: Clang 20, MinGW GCC 16, MSVC 19.44 |
| `linux-gcc`, `linux-clang` | Ubuntu 24.04 x64: GCC 13, Clang 18 |
| `macos-arm64-clang` | macOS 15 on Apple silicon: Apple Clang 17, ARM64 |
| `cross-load on windows / linux / macos` | after all builds: all hash dumps and portable snapshots are byte-identical, and every build of that OS continues every build's snapshot |

Each build job (`scripts/ci_check.sh <preset> <name> <out-dir>`, also usable locally):

- builds the simulation, server, mods and tests (no Godot);
- runs every test (`ctest`, network sessions included);
- compares the per-tick hashes with `tests/reference_hashes.txt` and the pose hash with
  `tests/reference_anim_hash.txt`;
- uploads its hash dump, a portable snapshot and `cb_tests` for the cross-load jobs
  (`scripts/ci_cross.sh`), which also check that all dumps and snapshots are byte-identical.

Box3D is fetched with two local patches in `cmake/patches/` (see DESIGN.md, Determinism):
`box3d-snapshot-padding.patch` (snapshots carried uninitialized padding bytes and a heap pointer) and
`box3d-neon-minmax.patch` (ARM64 clamps returned -0 where x64 returns +0).
