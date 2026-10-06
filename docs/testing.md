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
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_guard.gd` | Checks the scene guard: the game's own scenes pass, and scenes with an `HTTPRequest`, a script, a wired signal, a climbing path or an animation that calls `queue_free` are refused |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_predictions.gd` | Checks predictions on a bare director: a press plays its cue at once, the server's cue then plays only what waited, another player's cue is never an echo, conditions and cooldown hold a press back |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_motions.gd` | Checks what the motion nodes bake to, on sets built in code: the file's text (impulses, forces, a probe, links, targets), and what refuses the bake (no action, a parameter that is none, a condition that does not parse, an effect on what a probe found without a probe, a rope to the player itself, an effect outside a motion, more than 16) |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_movement.gd` | Checks what a character's `movement` bakes to, on the mannequin: `move.<name>` lines in the simulation's order, and what is refused |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_grips.gd` | Checks what an item (its `CbItem`, body and grip markers) bakes to, on scenes built in code: carried at the origin, at a carrying marker, at a turned one, and what is refused |
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_object_source.gd -- FILE.cbv` | Checks that the viewer draws from any object that hands it packets: a GDScript source reads a view file, with no peer extension involved |
| `godot --path godot -- --autoplay=S --screenshot=F.png --screenshot-every=S2` | Unattended client; also saves `F_1.png`, `F_2.png`, ... and prints the mod events it saw |
| `scripts/stress_test.sh --bots N --full M --latency MS --jitter MS --loss % --rollback T` | Starts a server, the simulator and the bots, and prints a summary |

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

`tests/reference_hashes.txt` holds the per-tick state hashes of the reference scenario, followed by
those of a second run with [motions](motions.md) (the reference scenario has none). Any change to
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

- builds the simulation, server, mods and tests (no Godot);
- runs every test (`ctest`, network sessions included);
- compares the per-tick hashes with `tests/reference_hashes.txt` and the pose hash with
  `tests/reference_anim_hash.txt`;
- uploads its hash dump, a portable snapshot and `cb_tests` for the cross-load jobs
  (`scripts/ci_cross.sh`), which also check that all dumps and snapshots are byte-identical.

Box3D is fetched with two local patches in `cmake/patches/` (see DESIGN.md, Determinism):
`box3d-snapshot-padding.patch` (snapshots carried uninitialized padding bytes and a heap pointer) and
`box3d-neon-minmax.patch` (ARM64 clamps returned -0 where x64 returns +0).
