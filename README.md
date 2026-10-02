# Cinderbox

A deterministic multiplayer third-person physics sandbox, built with flecs, Box3D, ENet and
ozz-animation, with a Godot 4 client (rendering, VFX, UI and mods). See [DESIGN.md](DESIGN.md) for the architecture and decisions, and [ROADMAP.md](ROADMAP.md) for what comes next.

## Status

| Milestone | State |
|---|---|
| M1: build, deterministic sim core, snapshots, determinism tests | done |
| M2: ENet server, raylib client, rollback netcode, join/leave/reconnect | done |
| M3: ozz animation, procedural box skeleton, locomotion blend, asset pipeline | done |
| M4: replay tool, network simulator, bots, stress test | done |
| M5: unreliable frame batches, automatic prediction window | done |
| M6: Godot client (GDExtension), prefabs, VFX, HUD, mod packs, Windows export | done |
| M7: maps authored in the Godot editor, baked .cbmap format, map sent on join | done |
| M8: component registry, entity templates authored in the inspector, runtime spawning | done |
| M9: declarative effect bindings, so mods add effects as data | done |
| M10: sounds and screen effects in the same bindings | done |
| M11: impacts and footsteps reported by the simulation | done |
| M12: Godot AnimationTree driven by the simulation's animation state | done |
| M13: humanoid-profile bone names and retargeting onto any character | done |
| M14: server gameplay mods (C++), commands in the frame, board and mod events, deterministic ragdolls, data-driven mod presentation, pistol demo | done |
| M15: workshop items (mods' looks, announced by servers, never sent), hardened pack validator, HUD from fields (bars, kill feed, scoreboard), player names, fast clock catch-up | done |
| M16: deathmatch rounds as a server mod, `Freeze` command, mod options (`--mod-option`) | done |
| M17: CI on GitHub Actions: six compilers on Windows, Linux and macOS ARM64 must agree bit for bit | done |
| M18: Box3D snapshots no longer carry stale memory; byte-identical across all builds | done |
| M19: characters as workshop items (baked in the editor: ozz skeleton, clips, hitboxes), `cb_server --character`, hit zones for mods | done |
| M20: one pose for players: aiming is part of the ozz pose (so hitboxes follow the raised arm), Godot animation on players is cosmetic only | done |
| M21: facing modes for mods: freelook (default) or camera-facing, with legs that walk where the body goes | done |
| M22: animation layers and stances chosen by mods (the pistol on the upper body, a new melee bat on the whole body) | done |
| M23: companion tracks: VFX, sounds, lights and props keyed in a character's Godot animations play in step with the ozz pose | done |
| M24: a real default character: the Universal Animation Library's mannequin ships with the game; held items sit the same on every rig | done |
| M25: state machines authored in Godot: a character's AnimationTree is baked and run by the simulation, markers become mod events | done |
| M26: strafing with real clips: 2D blend spaces, body-frame velocity, per-character leg turning; a local-only character from the paid animation pack | done |
| M27: the chest faces the camera while strafing (`face_forward`), though the strafe clips turn the torso | done |
| M28: the bat burns while it swings, as the melee mod's own look (not the character's) | done |
| M29: held items are entities with their own state, drawn in sockets; a character's animations play the held item's animations | done |
| M30: one event resolved by what the player holds (item kinds and event values in conditions; events reach held items); no game content left in the engine | done |
| M31: a bat taken out again still slashes (item swaps refresh the animation track caches) | done |
| M32: animation packs: mods ship AnimationTree layers and swap a player's own for them (a crouch walk), retargeted to any humanoid-profile character | done |
| M33: `CbReaction` nodes: an entity's scene reacts to its board and events with no code; the pistol is a held item; state bindings and the implicit item rules are gone | done |
| M34: entity paths: a reaction's subject, conditions and the scene it acts in can be any related entity (`holder/item:LeftHand`, `event.b`, `local`, `world`) | done |
| M35: world reactions: `vfx/reactions*.tscn` scenes of `CbReaction` nodes replace effect bindings (`CbEffect`); built-in events, placement, sounds and screen effects on the same node | done |
| M36: reactions address the scene tree the Roblox way (`^^/RightHand/Item`, `$other/Head`); `CbDirector` + `CbReaction` are a standalone Godot addon the client drives with cues and state | done |
| M37: Cue Preview: an editor panel that plays a scene's reactions (fire cues, set state, pick the viewer) with no game running | done |
| M38: reaction polish: fixes, `method_args`, delay and chance, blended properties, "why didn't it fire" in the preview, and help in the editor (hover texts, info buttons, class reference) | done |
| M39: items in the world: dropped, thrown and picked up, with physics everyone agrees on; a pickup mod with a proximity prompt made of data (`CbPromptLabel`, `$local@pickup.target`, `{key:pickup}`) | done |
| M40: looks follow what is held: item kinds are conditions in the HUD and reactions (`pistol.gun`), so picked-up items look and sound right | done |
| M41: a loadout slot gives one item per life (no duplicating by dropping); an `expire` mod removes items left lying | done |
| M42: item bodies authored in Godot: a `CbItemBody` in the item's scene, baked into the mod's item, read by the server | done |
| M43: hold-to-use prompts: item properties (`pickup.hold_seconds`), `pickup.progress`, and a prompt bar that fills | done |
| M44: held items bring animation layers (the bat changes how its holder stands and walks); a mod's own swap wins over an item's | done |
| M45: join menu (address, recent servers), Esc menu, settings; failed joins say why; camera collision with the map | done |
| M46: an inventory (slots, stowed items, optional holsters): switching and picking up no longer drop other items | done |
| M47: item properties and hold times authored on the `CbItemBody`, a Bake button for it, hold progress drawn from a start tick | done |
| M48: 32 board fields per scope instead of 16 (all 16 were used); the inventory shows its slots on the HUD | done |
| M49: the viewer protocol: the Godot client draws frames from a source (a live connection, a recording) and knows neither; recordings are watched in the game, as the player they follow | done |
| M50: frames as bytes: a viewer's frames encode to packets (whole or as deltas), the server records view files that play with no simulation, and the Godot client watches them | done |
| M51: two extensions: the viewer (no simulation, no networking; all a mod's project needs) and the peer (joins servers, plays recordings), with frames crossing as bytes; any object, even a script, can be a viewer's source | done |
| M52: packs are contained: a scene is checked before it is used (listed node classes only, no scripts, no wired signals, no paths out of the scene, animations and reactions call only listed methods) | done |
| M53: smaller frames: compact packets and fewer frames than ticks, what a stream will carry (32 players: 6.8 Mbit/s down to 0.43); view files can be recorded that way | done |
| M54: streaming clients: a client that does not simulate is sent the frames to draw (`--stream`); the server's mods decide what each one sees (fog of war, the `fog` mod); a third, simulation-free extension | done |
| M56: the viewer predicts: a look says which cue the server will answer a press with (`CbPrediction`), and it plays at once with the same reactions; the server's cue then plays only what had to wait for it | done |
| M57: the bat lights its own flames: a reaction on the swing's cue in the bat's scene, not a key in the character's animation; `wait_for_server` keeps a reaction off a predicted press | done |
| M58: an example of a second action: the pistol's right button marks the player its ray finds (a zone around them for 2 seconds), with the server part and the look part side by side | done |
| M59: no companion files: an animation's non-bone tracks are read from the character's own `AnimationPlayer` when it is first drawn; a character is baked when its scene is saved and again when its item is packed | done |
| M60: the raylib client is gone: the Godot client is the client | done |
| M61: a `combat` mod: health, death and respawning in one place, spoken to by events (`combat.damage`, `combat.heal`) with server options; the pistol only keeps the gun | done |
| M62: one animation system: every character is a state machine (the placeholder rig's is built in); the old clip blending, stance clip tables, `CinderboxAnimator` and the glTF converter are gone | done |
| M63: DESIGN.md by subsystem instead of by milestone | done |
| M64: predicted state in the viewer: a prediction also says what the server's answer changes, so the ammo count drops and the swing or the recoil starts on the click | done |
| M65: a hot bat cools by itself wherever it is (put away or dropped while hot, it stayed hot) | done |
| M66: private fields: a mod tells one player something nobody else is sent (a role, a hand of cards); looks read it like any field, for the viewer's own player | done |
| M67: one expression language: reactions, predictions, HUD nodes and state machines parse the same text (`cb_expr`): `and` / `or`, arithmetic, a field against a field; a reaction's property and volume can be expressions | done |

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

`clang-release` and `unix-clang-release` also build the three Godot extensions (`CB_BUILD_GODOT`, via
godot-cpp) into `godot/bin/` as `template_debug`: `libcinderbox` (the viewer) and
`libcinderbox_peer` (the simulation and the networking, for joining servers) and
`libcinderbox_stream` (networking only, for being sent frames). `godot-export` builds only their
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
# or join without simulating: the server sends the frames to draw (nothing is predicted)
godot --path godot -- --host=127.0.0.1 --port=7777 --stream
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

### Streaming

`--stream` joins a server without simulating it. The server sends 20 frames a second
(`cb_server --stream-rate HZ`) and plays the input it is sent.

| | A client that simulates | A streaming client |
|---|---|---|
| Runs | the whole world, with prediction and rollback | nothing: it draws what it is sent |
| Your own character | answers at once | answers a round trip later |
| Is shown | everything (it has the world) | what the server's mods let it see |
| Needs | the same build of the simulation as the server | any build that speaks the protocol |
| Downloads | inputs: ~55 kbit/s at 32 players | frames: ~565 kbit/s at 32 players, ~117 with `fog.radius=12` |

**Fog of war** is a server mod's decision: `ServerMod::Sees( ctx, viewer, netId )` is asked for
every entity of every frame a streaming client is sent. The `fog` mod is the example:

```bash
cb_server --mod-option fog.radius=12     # streaming clients are sent the level and what is within 12 m
```

A client that simulates cannot be kept in the dark, so a server that depends on fog should turn
away clients that are not streaming (it does not yet).

Open `godot/` in the Godot editor to edit scenes, then press Play. Godot client options, given after `--`:
- `--host=H`, `--port=P`: join this server without the menu (leaving it lands in the menu).
- `--stream`: join as a streaming client: no simulation, no prediction, shown what the server sends (see [Streaming](#streaming)).
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
`--prop-lifetime`, `--props-per-player`, `--props-global`, and `--mods A,B` / `--mods none` / `--list-mods`
(every compiled server mod runs by default, see [Server mods](#server-mods)).

## Server mods

The game's rules live in C++ mods compiled into `cb_server` (`server_mods/<name>/<name>.cpp`). They run
**only on the server**. Clients never run gameplay code and never learn what a pistol is.

| What a mod does | How |
|---|---|
| Reads the world | the state before this tick: players, positions, board values, ray casts |
| Reads input | this tick's inputs; `Pressed()` / `Held()` on actions it declared |
| Changes the world | **commands** added to the tick's authoritative frame |
| Keeps its own state | a flecs world shared by all mods (never rolled back, never sent) |
| Talks to presentation | board fields and mod events, by name |

Every client applies the frame's commands exactly like inputs, so the world stays deterministic
and mods need no determinism of their own.

| Command | Effect |
|---|---|
| `Set` | writes a board field of an entity, or of the game (global) |
| `Emit` | announces a mod event (`"pistol.fired"`): two entities, a value, a point, a vector |
| `SpawnProp` / `SpawnTemplate` | creates a prop, owned by a player or the level |
| `Destroy` | removes an entity (never a player) |
| `Push` | an impulse or a velocity change; knockback for characters |
| `Kill` | a player dies, optionally leaving a ragdoll (lifetime and cap chosen by the mod) |
| `Respawn` / `RespawnAt` | brings a dead player back |
| `Freeze` | stops a player moving and acting (it still looks around), or releases it |
| `Aim` | turns the player's aim chain (its character's arm, by default) toward where it looks, or lets it go |
| `Facing` | the body faces where the camera looks, or turns toward where it walks (freelook, the default) |
| `Stance` | plays a stance on one of the player's animation layers, or clears it |

```cpp
// server_mods/jumper/jumper.cpp: a jump boost on Q, the whole mod.
class JumperMod final : public cb::mods::ServerMod {
	cb::mods::ActionHandle m_boost;
	cb::mods::EventHandle m_boosted;
public:
	const char* Name() const override { return "jumper"; }
	void Declare( cb::mods::Declarations& d ) override {
		m_boost = d.Action( "boost", "Q" );       // clients bind Q to it
		m_boosted = d.Event( "jumper.boosted" );  // reactions can play something on it
	}
	void Tick( cb::mods::Context& ctx ) override {
		for ( int i = 0; i < cb::kMaxPlayers; ++i )
			if ( ctx.InWorld( i ) && ctx.Pressed( i, m_boost ) ) {
				ctx.Push( cb::SlotTarget( i ), {}, { 0, 9, 0 }, cb::ImpulseVelocity );
				ctx.Emit( m_boosted, cb::SlotTarget( i ) );
			}
	}
};
std::unique_ptr<cb::mods::ServerMod> CreateMod_jumper() { return std::make_unique<JumperMod>(); }
```

Add the folder, re-run CMake, and the mod is in `cb_server --list-mods`. The server sends every client
a **schema** on join: field names and types, event names, and action names with suggested keys. The
Godot client binds those keys (InputMap actions `cb_<name>`), and reactions refer to fields and events
by name.

The mods that ship:

| Mod | Declares | Rules |
|---|---|---|
| `combat` | `combat.health`, `.max_health`, `.dead`, `.kills`, `.deaths`; hears `combat.damage`, `combat.heal`, `game.round_start`; says `combat.hurt`, `combat.killed`, `combat.respawned` | the one place health lives: applies any mod's damage, credits the kill, leaves a ragdoll, brings the player back. See [The combat mod](#the-combat-mod) |
| `inventory` | `inventory.slot`, `inventory.item_2` .. `item_4`; actions `slot_1` .. `slot_4` (keys 1 to 4) | what a player carries: slot 1 is empty hands (and freelook), slots 2 to 4 hold one item each; the slot that is out has its item in the right hand, the rest are stowed. See [The inventory](#the-inventory) |
| `melee` | layer `full`, stances `melee`, `melee_swing`; events `melee.swing`, `melee.hit`, `combat.damage` | the bat: a full-body stance while it is out; left mouse swings (0.45 s, every 0.6 s), a fan of 1.8 m rays from the chest at the strike, 40 damage through `combat.damage` |
| `props` | action `spawn_prop` (F) | F with empty hands throws a prop (the map's spawnable template, or a random box or sphere) |
| `pistol` | `pistol.ammo`, `pistol.reloading`; `fire` (left mouse), `reload` (R), `mark` (right mouse); events `pistol.fired`, `pistol.hit`, `pistol.reload`, `pistol.dry`, `pistol.scan`, `pistol.marked`, `combat.damage` | hitscan from the camera pivot, 25 damage (the head doubles it) through `combat.damage`, 12 rounds, 1.5 s reload; the `pistol` stance on the `upper` layer while it is out; a new life (`combat.respawned`) comes with a full magazine |
| `fog` | nothing; option `fog.radius` | streaming clients are sent only what is within the radius of their player (off by default) |
| `secret` | `secret.number`, a private field; option `secret.numbers` | the example of a private field: off unless `--mod-option secret.numbers=1`; then each player is told a number from 1 to 99 that nobody else is sent |
| `deathmatch` | `deathmatch.score` per player; `deathmatch.phase`, `.seconds`, `.round`, `.winner`, `.kill_limit` for the game; events `deathmatch.round_end`, `game.round_start` | rounds: first to 10 kills, or the best score after 300 s; falling costs a point; everyone is frozen for a 6 s intermission, then the world is cleared, everyone respawns and scores reset |

Mods cooperate through the board (`pickup` reads the `inventory.slot` that `inventory` publishes, to
know there is one) and through item properties (`pistol` and `melee` tell `inventory` which slot
their item lives in). They also cooperate through events: `pistol` and `melee` say `combat.damage`,
`combat` answers with `combat.hurt` and `combat.killed`, `deathmatch` scores the kills, and its
`game.round_start` gives everyone full health (`combat`) and a full magazine (`pistol`).

### Private fields

Every client that simulates has the whole world: it could not predict otherwise. So what one
player must not know about another cannot be a board field. A **private field** is told to its
owner alone:

```cpp
m_role = declare.Field( "cards.role", BoardType::Int, BoardScope::Private );
...
ctx.Set( SlotTarget( slot ), m_role, 3 );     // that player is told; nobody else is
```

| | An entity field | A private field |
|---|---|---|
| Lives in | the simulation's state (hashed, rolled back, recorded) | the server, per player slot |
| Sent to | every client, as a command in the frame | its owner only: a reliable message, or in a streaming client's frames |
| A look reads it | for any entity | for the viewer's own player (`{cards.role}`, `cards.role == 3`); for anyone else it reads 0 |
| Predictions | `changes` apply | `changes` apply |
| State machines | can read it | cannot (the simulation does not have it; the bake and the server say so) |
| Recordings, view files | have it | do not: a recording watched later shows 0 |

- Per player, not per entity: the target of `Set` is a player (`SlotTarget`, or its NetId).
- A slot that is given up is cleared; a reconnecting player gets its values again with the welcome.
- 32 private names per server, counted apart from entity and global fields.
- What happens in the world (where someone is, what they hold) is the simulation's and is not
  secret from a simulating client. Hiding *entities* is `Sees`, for streaming clients only
  ([Streaming](#streaming)).
- The `secret` mod is the example, and `net_private_fields` the check.

### The combat mod

Health, death and the next life, for every mod that hurts. A weapon does not own health and does
not know the other weapons: it declares the names it uses (the same name is the same event) and
talks to `combat` through them.

| A mod | Event | Carries |
|---|---|---|
| emits | `combat.damage` | a = attacker (0: nobody), b = who is hurt, value = damage, point = where, vector = the push the body gets if it dies |
| emits | `combat.heal` | b = who, value = health given back |
| emits | `game.round_start` | everyone alive, full health |
| hears | `combat.hurt` | a = attacker, b = victim, value = health actually lost, point |
| hears | `combat.killed` | a = killer (0: the world, a fall), b = who died |
| hears | `combat.respawned` | a = who is back (after dying, or with a new round) |

```cpp
// A weapon, in full: declare the name, say what you did.
m_damage = declare.Event( "combat.damage" );
...
ctx.Emit( m_damage, SlotTarget( slot ), hit.netId, 40, hit.point, push );
```

| Option (`--mod-option`) | Default | Meaning |
|---|---|---|
| `combat.max_health` | 100 | health at the start of a life |
| `combat.respawn_seconds` | 3 | from death to the next life |
| `combat.ragdoll_seconds`, `combat.ragdoll_cap` | 10, 16 | how long a body lies, and how many at once |
| `combat.fall_counts` | 1 | falling out of the world is a death (0: it is not) |

- Events are heard a tick after they are sent: damage lands the tick after the hit.
- Without the mod (`--mods pistol,inventory`) weapons fire and report hits, and nobody dies.
- Its look is its own workshop item: the health bar, YOU DIED, the kill feed and the scoreboard
  (`ui/hud_combat.tscn`), and the red flash when you are hurt or die (`vfx/reactions_combat.tscn`),
  whatever weapon did it. A weapon's look keeps what is the weapon's (the hit puff, the hit marker).

Server operators tune mods with `--mod-option NAME=VALUE` (repeatable); a mod reads them with
`ctx.Option( "deathmatch.kills", 10 )`.

| Option | Default |
|---|---|
| `deathmatch.kills` | 10 kills to win a round |
| `deathmatch.round_seconds` | 300 |
| `deathmatch.pause_seconds` | 6 (the intermission) |
| `deathmatch.fall_penalty` | 1 point lost for falling out of the world |
| `pistol.zone.<zone>` | damage multiplier for a hit zone of the server's character: `head` 2, anything else 1 |
| `pickup.spawn_each` | 0; N drops N of every item kind the mods declared around the spawn point at start |
| `pickup.hold_seconds` | 0 (a tap); how long E must be held to pick up an item whose mod does not say (see item properties) |
| `expire.seconds` | 60; an item that was held and then left lying is removed after this long (0: never) |
| `fog.radius` | 0 (off); streaming clients are sent the level and what is within this many metres of their player |

```bash
cb_server --port 7777 --mod-option deathmatch.kills=5 --mod-option deathmatch.round_seconds=120
```

### Workshop items

A mod's **look** (reactions, HUD, meshes, sounds) is a workshop item, like a Steam Workshop or
Counter-Strike mod. **Game servers never send it**: a server only announces which item each of its
mods needs, and players must already have that exact item.

| Piece | Where |
|---|---|
| The item's source | `server_mods/<mod>/client/`, a Godot project next to the mod's code, with a "Mod" export preset |
| Publishing | `tools\publish_mod.ps1 -Mod <mod>`: packs it, names it by its SHA-256, installs it into the local workshop, writes `server_mods/<mod>/client_item.cfg` (commit it) |
| The workshop | for now a folder: `user://workshop/<mod>/<sha256>.zip` (`%APPDATA%\Godot\app_userdata\Cinderbox\workshop`); `godot/workshop.gd` is the one place a real workshop would plug in |
| What the server announces | the hash from `bin/items/<mod>.item` (the build copies `client_item.cfg` there; `--items DIR` to use another folder) |

Joining a server:
1. The server's schema lists the items its mods need.
2. Anything missing or different: the client leaves and says exactly which items to get.
3. Otherwise items load after the base game and **before your own mods**, which load again last, so you
   can still restyle what an item ships. Their `ui/hud_<mod>.tscn` is laid over the game's HUD.

After changing a mod's client project, publish it again and rebuild: the new hash is what servers
announce, and older copies stay in the workshop for servers that still announce them.

## Client mods

Client mods are cosmetic Godot resource packs (`.zip`). A mod can replace or add:
- entity visuals in `prefabs/`;
- effects in `vfx/`, and world reactions as `vfx/reactions_<name>.tscn`;
- sounds and other shared files in `assets/`;
- the HUD in `ui/`;
- map visuals in `maps/` (the scene named after the map the server runs);

Client mods and workshop items cannot contain code, and a scene without code cannot act like code.
Two checks:

| When | What | Checked by |
|---|---|---|
| Before a pack loads | only known kinds of files in these folders (and Godot's converted copies of them), redirects that stay inside the pack, no compressed resources, no resource that names a script type or a script file | `boot.gd`; `check_mod_validator.gd` runs it on real packs and hostile ones |
| Before a scene is used | only listed node classes (meshes, particles, lights, sounds, animation, UI controls, the `Cb*` nodes: never an `HTTPRequest`, a `Window`, a camera), no script, no signal wired to a method, no node path that leaves the scene, animations that call only listed methods | the scene guard (`src/godot/cue/cue_guard.h`); `check_guard.gd` |

A scene the guard refuses is not instantiated (a warning says what it found); the game draws its
plain stand-in instead. Gameplay stays in the simulation and in the server's mods, so a pack cannot
change it. What no check can promise is that Godot's own parsers are safe against a deliberately
malformed file, so packs are still something to take from people you trust.

1. Create a Godot project under `mods_src/<name>`. Copy `mods_src/example_neon` as a starting point.
2. Put your files at the same paths the game uses, for example `vfx/prop_spawn.tscn`.
3. List them in the project's "Mod" export preset.
4. Run `tools\pack_mod.ps1 -Project mods_src\<name>` to create `mods\<name>.zip`.

The game loads packs from these places, in this order:
1. `<game folder>/mods`
2. `user://mods`
3. each `--mods=DIR`

Within a folder, packs load in alphabetical order, and a later pack overrides an earlier one.
[mods_src/README.md](mods_src/README.md) lists the files the game looks up.

## Maps

A map is a Godot scene with three marker nodes in it, baked into a `.cbmap` file the server loads.
The markers carry the collision the simulation needs; everything else in the scene is the look.

| Node | What it becomes |
|---|---|
| `CbStatic` | A solid box: floor, wall, ramp, step, platform. `size` is the full size in metres. |
| `CbProp` | A dynamic box or sphere the level starts with. |
| `CbSpawn` | Where players appear. One per map. |
| `CbTemplate` | A named entity built from components (see Entities below). |
| `CbComponent` | One component on a template, with the fields the simulation defines. |
| `CbEntity` | Places a template in the level, or defines one inline from its own components. |

1. Make a scene in `godot/maps/`, for example `arena.tscn` (copy `example_arena.tscn` to start).
2. Place `CbStatic` boxes for the collision, and your own meshes, lights and effects for the look.
3. Press **Bake Map** in the 3D toolbar, or run `tools\bake_map.ps1 -Scene res://maps/arena.tscn`.
4. Run the server on it: `cb_server --map godot\maps\arena.cbmap`.

The server sends the baked map to every client when it joins, so clients always play the server's
level and can never disagree about it. Clients then look for `res://maps/<name>.tscn` to draw it; if
they do not have that scene, they draw the baked collision boxes instead, which is what the
built-in sandbox does.

Baked values, including authored component fields, are rounded to fixed-point (1/1024 m,
1/4096 rad) so a map is identical on every platform, and the order of the nodes in the scene is
the order entities are created in, which is part of the map's identity. See `src/sim/map.h` for
the format and `src/sim/reflect.h` for the component registry.

## Entities and components

An entity is described by attaching components to it in the inspector, the same components the
simulation uses. Add a `CbTemplate`, give it `CbComponent` children, pick a component in each one,
and its fields appear:

| Component | Fields |
|---|---|
| `Shape` | `kind` (Box, Sphere, Capsule), `size`, `radius`, `height` |
| `Body` | `type` (Static, Kinematic, Dynamic), `gravity_scale`, `linear_damping`, `angular_damping` |
| `Material` | `density`, `friction`, `restitution` |
| `Velocity` | `linear`, `angular` the entity starts with |
| `Prop` | `lifetime_seconds` (0 keeps it forever); makes it count against the prop caps |

Those fields come from the simulation's own registry (`src/sim/reflect.h`), so adding a field there
makes it appear in the editor with no Godot-side code. A component an author does not attach is
left at the engine's default, which is also how a map baked before a field existed still loads.

- `visual` on a template names the prefab clients draw for it: `res://prefabs/<visual>.tscn`.
  Without it, the shape's default prefab is used.
- `spawnable` marks the one template the spawn button (F) creates. Anything a player spawns still
  expires and counts against the prop caps, whatever the template says.
- A `CbEntity` with a `template_name` places that template. A `CbEntity` with its own
  `CbComponent` children defines a template just for itself, and identical ones are shared.
- A template with `Body.type = Static` becomes level geometry: it never falls and the kill plane
  ignores it.

Templates are part of the map, so they travel to clients with it and the server can spawn them at
runtime. They are initial values only: a template says what an entity starts as, never how it
behaves. Behaviour stays in the simulation.

## Effects

What plays when is data, not code: **world reactions**. A mod ships `res://vfx/reactions_<name>.tscn`,
a scene of `CbReaction` nodes, and the client loads every `res://vfx/reactions*.tscn` once. They are
the same node as the reactions inside entity scenes ([Reactions](#reactions)), with no entity of
their own: each names what it is about from the cue (`$at`, `$other`). Files add to each
other, so two mods add effects without fighting over one list; a mod replaces
`vfx/reactions.tscn` to change the game's own.

```
PistolReactions                      (vfx/reactions_pistol.tscn)
├── PredictFire   CbPrediction        fire -> pistol.fired   pistol.gun, pistol.ammo > 0, !pistol.reloading
├── Fired         on pistol.fired     subject $at                         muzzle flash + gunshot at $at/RightHand
├── Kick          on pistol.fired     subject $at      is_local           camera shake
├── Tracer        on pistol.fired     subject $at                         beam from $at/RightHand to the cue's end
├── Hurt          on pistol.hit       subject $other   is_local           camera shake + red flash
└── Gun           CbItemLook          pistol.gun -> res://prefabs/pistol.tscn
```

| Events | Carry |
|---|---|
| a mod's, by name (`pistol.hit`) | A (who it is about), B (the other one), `event.value`, the point and the end (where a shot ended) |
| `spawned`, `destroying` | the entity; its position (`spawned` only for entities that appear in play, not a world reset) |
| `jumped`, `landed`, `footstep` | the player; at its feet |
| `impact` | both bodies; `event.strength` (approach speed, m/s) |

What a world reaction adds to the [reaction fields](#reactions):

| Field | Meaning |
|---|---|
| `subject_kind`, `subject_template` | only for a player / prop / static / ragdoll / item, or entities from one map template |
| `cooldown` | shortest gap between two firings (twenty props landing at once play one sound) |
| `place`, `place_node`, `offset` | where its scene and sound go: under its parent, at the cue's **point** or **end**, a **beam** from `place_node` (or the point) to the end, **at** `place_node` (a socket: `$at/RightHand`, `$at/Head`), or **following** the subject |
| `sound`, `volume_db`, `pitch_scale`, `pitch_jitter`, `bus`, `max_distance` | a sound, once per firing |
| `shake`, `shake_time`, `flash_color`, `flash_time` | camera shake and a full-screen flash: the viewer's, so pair them with `is_local` or subject `local` |

Footsteps and impacts come from the simulation, not from the renderer guessing:
- A **footstep** is a stride, counted by distance walked, so the rate follows the speed on its own.
- An **impact** is a collision the physics engine reported above 1.5 m/s, carrying where it
  happened, both entities and how fast they were approaching. Two reactions with different
  `event.strength` conditions give a soft hit and a hard one different effects.

Both are part of the simulation's state, so they are identical on every machine, survive rollback,
and a client that skipped frames still sees them.

### Predictions

A mod's rules run on the server, so what your own press did comes back a round trip later. A
**`CbPrediction`** node in the look says what the server is going to answer, and the viewer shows
it at once:

```
PredictFire   CbPrediction   action "fire"   cue "pistol.fired"
                             conditions pistol.gun, pistol.ammo > 0, !pistol.reloading, !combat.dead
                             cooldown 0.19
                             changes    pistol.ammo -= 1

PredictSwing  CbPrediction   action "fire"   cue "melee.swing"   conditions melee.bat   cooldown 0.58
                             stance "melee_swing" on stance_layer "full"
```

| Step | What happens |
|---|---|
| You press `fire` and the conditions hold on your player | `pistol.fired` plays for you now, with the same reactions everyone else's shot plays: one reaction per cue, none written twice |
| The prediction has `changes` | your own player's fields read the changed value at once: the ammo count drops on the click |
| The prediction has a `stance`, or your character's state machine reads the cue | your own upper body is shown ahead of the server: the swing or the recoil starts on the click |
| A reaction has `wait_for_server` on | it waits for the server's cue anyway |
| A reaction needs what only the server knows | it waits: placed at the cue's point, end or beam, a path starting with `$other`, or a condition on `event.*` (the tracer, the hit spark) |
| The server's `pistol.fired` for you arrives (within a second) | it is the echo: what already played stays quiet, what waited plays now |
| No press was predicted | the server's cue plays in full, as for any other player |

| Field | Meaning |
|---|---|
| `action` | the action whose press is predicted (one a server mod declares) |
| `cue` | the cue the server sends for it: the same name, so the same reactions |
| `conditions` | the look's copy of the server's rule, read on your own player: [expressions](#effects) |
| `cooldown` | the server's own rate (seconds between two predictions) |
| `changes` | what the server's answer will change on your player: `pistol.ammo -= 1`, `x += 2`, `x = 0`. Shown until the server's cue comes (its own value is in the same frame) or the prediction expires |
| `stance`, `stance_layer` | the stance the server's mod will set, and the layer it sets it on (`melee_swing` on `full`) |

- **Both halves are the modder's**: the server mod emits the cue, its look predicts it by name.
- **A wrong guess**: a reaction that played is not taken back; changed fields and the led body
  go back to the server's when the prediction expires (a second). Keep the conditions as close to
  the server's rule as the board allows.
- **How the body is led**: the viewer runs your character's state machine forward from the state
  the server sent, with the predicted stance or event put in at the moment of the press, for the
  layers above the base (the legs follow movement, which is predicted already). When the server's
  answer arrives nothing jumps; afterwards the lead is given back slowly (the upper body plays 15%
  slower until it is level again). At most half a second ahead.
- **Looks only**: nothing is sent anywhere, and no rule runs on the client.
- `CbDirector.explain_press( "fire" )` says which predictions a press would make, or why not;
  `check_predictions.gd` drives one by hand.
- With 50 ms of delay each way, a click shows its shot 2 ms later and the ammo count at the next
  frame; the server's cue (the tracer) follows at about 250 ms. A swing's hand moves 70 ms after
  the click and the bat's flames light at 110 ms; the server's swing comes at about 220 ms.

### Example: a second action

The pistol's right mouse button casts a ray and puts a zone around the player it finds for 2
seconds. The whole feature is one function on the server and four nodes in the look:

| Where | What | Why there |
|---|---|---|
| `pistol.cpp`, `Declare` | `declare.Action( "mark", "MouseRight" )`, events `pistol.scan` and `pistol.marked` | names the look can use; the key shows in the controls hint by itself |
| `pistol.cpp`, `Mark` | on a press (once a second): `ctx.CastRay` from the eye; `Emit( pistol.scan, caster, hit, eye, end )`; if it found a living player, `Emit( pistol.marked, caster, player )` | who is hit is the server's decision |
| look: `PredictScan` | `CbPrediction`: `mark` -> `pistol.scan` | your own click is heard at once |
| look: `Scan` | on `pistol.scan`, subject `$at`: a click at `$at/RightHand` | plays on the press for you, on the server's cue for everyone else |
| look: `ScanBeam` | on `pistol.scan`: `vfx/scan_beam.tscn` as a **beam** to the cue's end | needs the server's end point, so it waits for the server |
| look: `Marked` | on `pistol.marked`, subject `$other`: `vfx/mark_zone.tscn`, place **Follow the subject**, `scene_lifetime = 2` | the zone is a child of the marked player for 2 seconds, then freed |

The server keeps nothing about the zone: how it looks and how long it shows is the look's. If the
mark had to *do* something for those 2 seconds (slow the player), that would be a board field the
server sets and clears, and the zone a **While** reaction on it. Behind 50 ms each way: the click
at 1 ms, the server's events at about 160 ms, the zone on for 1.97 s.

Conditions and values are written in **one expression language**, the same text wherever it is
read: a reaction, a prediction, a HUD node, a character's state machine.

| Write | Means |
|---|---|
| `name` / `!name` (or `not name`) | the field is not zero / is zero |
| `?name` | the server declared the field (its mod is running); `!?name`: it did not (hide the pistol's scoreboard when deathmatch shows its own) |
| `pistol.gun` (an item kind) | the player holds one, in any socket: what a look should ask, not which loadout slot is out |
| `name == 2`, `!=`, `>`, `>=`, `<`, `<=` | a comparison: 1 or 0 (`true` / `false` are 1 / 0) |
| `+ - * /`, `-x`, `( )` | arithmetic; dividing by 0 gives 0 |
| `and` / `&&`, `or` / `\|\|` | both, either |
| a field against a field | `combat.health <= combat.max_health / 4` |

- **Precedence**, loosest first: `or`, `and`, `not`, comparisons, `+ -`, `* /`, unary `-`. So
  `!a == 2` is `not (a == 2)`, and `a or b and c` is `a or (b and c)`.
- **Truth**: anything that is not 0. A name nobody declared reads as 0, so looks for a mod that is
  not running never match. Text that does not parse is false, and the editor says why.
- **Who reads what**:

| Reader | Plain names are | Also |
|---|---|---|
| `CbReaction`, `CbPrediction` | the subject's state, then the world's | `is_local`, `event.value`, `event.strength`; a path and a colon reads another entity: `^^:combat.dead`, `$other:combat.health < 20` |
| HUD nodes | the local player's fields (private ones too), then the world's | item kinds |
| [State machines](#state-machines) | simulation values, stances, events, fields, item kinds | resolved once at bake; nothing private |

A list of conditions (`conditions`) holds when all of them do: `["pistol.gun", "!combat.dead"]`
is `pistol.gun and !combat.dead`.

What an entity looks like *while* something holds (a glowing bat) is authored inside its own scene,
with the same [`CbReaction` nodes](#reactions).

The HUD reads the board too, through script-free nodes any HUD scene can use:

| Node | Does |
|---|---|
| `CbFieldLabel` | a Label with a `text_format` (`"AMMO {pistol.ammo} / 12"`), shown while its `conditions` hold. `{an expression}` works too: `"{combat.health * 100 / combat.max_health}%"` |
| `CbFieldBinding` | writes a field, or an expression over fields (`combat.health / combat.max_health`), into any property of its `target` (default: its parent), `value = field * multiply + add`; with conditions it hides the target while they fail. A `ProgressBar`'s `value` and `max_value`, a panel's `visible`, a colour |
| `CbEventFeed` | a line per mod event, `"{a}  >  {b}"` with player names, fading after `line_seconds` (a kill feed) |
| `CbScoreboard` | players as rows: `cells` like `"{name}"`, `"{combat.kills}"`, sorted by `sort_field`, shown while Tab is held and its `conditions` hold |

In formats, `{field}` is the local player's field, `{name}` a player's name, `{name:field}` the
name of the player a field points at (`"{name:deathmatch.winner} WINS"`), `{look:field}` what the
entity a field points at is called (an item's `display_name`: `"Bat"`), and `{key:action}` the key
the player has that action bound to now (`"E"`, `"LMB"`: rebinding shows).

`CbPromptLabel` is the same in the world: a `Label3D` with a `text_format`, upright above its parent,
facing the camera, the same size at any distance, hidden while its text is empty; with a
`progress_field` it draws a bar under the text that fills as the field goes from 0 to 1. A reaction
puts it where it belongs (see [Items in the world](#items-in-the-world)).

The pistol's HUD (`server_mods/pistol/client/ui/hud_pistol.tscn`) is built from these: a health bar
(`ProgressBar` from `combat.health` and `combat.max_health`), ammo, reloading, crosshair, kills and
deaths, the kill feed and the scoreboard. None of it is script, so a client mod can restyle all of it.

`godot/vfx/reactions.tscn` is the game's own set; the pistol's look (predicted shots, tracers, hits,
reload, the pistol item's look) is `vfx/reactions_pistol.tscn` in its workshop item, and being hurt
or dying looks the same for every weapon (`vfx/reactions_combat.tscn`, the combat mod's item); `mods_src/example_neon/vfx/reactions_neon.tscn` shows a mod adding three more,
including its own sound. All are edited in the Godot editor, as scenes.

The sounds in `godot/assets/sfx/` are placeholders in the same spirit as the procedural rig: short,
synthetic, and meant to be replaced. `tools/make_sfx.py` regenerates them.

## Animations

One system poses every player: the character's **state machine**, authored as a Godot
`AnimationTree`, baked, and run by the simulation ([State machines](#state-machines)). Its clips
are sampled with ozz into the pose every client draws and the server hit-tests. Nothing else poses
a player.

| Character | Its state machine |
|---|---|
| the mannequin, the robot, any workshop character | baked from the `AnimationTree` in its scene |
| the placeholder rig (`--character none`) | built into the engine: one layer (idle, walk and run by speed, a jump), six procedural clips, no assets |

The placeholder rig is drawn as one box per bone, with the bone names of Godot's
`SkeletonProfileHumanoid`: `Hips`, `Spine`, `Chest`, `UpperChest`, `Neck`, `Head`,
`Left/RightShoulder`, `UpperArm`, `LowerArm`, `Hand`, `UpperLeg`, `LowerLeg`, `Foot`, `Toes`. That
is the profile Godot retargets imported characters onto, so a character imported the normal way
is driven with no mapping of our own.

### Godot animation on players: cosmetic only

**One rule:** the ozz pose, computed from the simulation's state, is the only thing that places a
player's body. It is the pose the server poses hitboxes with, so a body drawn any other way would
be shot where it is not. Godot animation may add to a player what the pose leaves alone: faces,
fingers, props, materials, effects.

This is enforced, not just advised. A `CinderboxSkeleton` driving a `Skeleton3D` gives it a
`CbPoseModifier` as its first skeleton modifier, which applies the ozz pose again after any
`AnimationPlayer` or `AnimationTree` has run. Modifiers after it (look-at, spring bones) run on top of
the pose; the character bake warns about them and about an `AnimationTree`, naming the bones that
have hitboxes.

What a character's animations do besides moving bones (particles, sounds, lights) stays in the
animations and plays in step with the pose: see
[An animation's other tracks](#an-animations-other-tracks).

```sh
# check that the ozz pose wins over Godot animation on a driven skeleton
godot --headless --path godot --script res://addons/cinderbox_maps/check_pose_wins.gd
```

### Ragdolls

A `Kill` command can leave a ragdoll: eleven Box3D bodies joined by cone-and-twist and hinge joints,
built from a fixed standing pose in the simulation (`src/sim/ragdoll.h`). It is simulation state,
identical on every machine, pushable, shootable, and it piles up with props and other ragdolls.

Clients draw it with the player's own prefab (or `prefabs/ragdoll.tscn` if there is one): every joint
of the skeleton follows the nearest body part, so any character works, and the pose the player was
last drawn in is blended into the ragdoll over 0.15 s, so there is no snap.

### Driving an imported character with ozz

Point a `CinderboxSkeleton` at a `Skeleton3D` and it retargets onto it: each bone is rotated
relative to **its own rest**, so the character keeps its proportions, and the hips move by an
amount scaled to its height. Our placeholder rests with its arms down while the humanoid profile
rests in a T-pose, and the difference between those two postures is bridged when the skeleton is
bound, so arms end up down rather than sticking out.

Turn `retarget` off to force every bone to exactly where our rig has it, which only makes sense for
a character built to our proportions.

```sh
# a humanoid with long legs and short arms, posed from the simulation's animation state
godot --headless --path godot --script res://addons/cinderbox_maps/check_retarget.gd
```

Client controls:
- WASD moves, Shift sprints and Space jumps: the engine's own controls.
- Everything else comes from the server's mods, bound to the keys they suggest. With the shipped mods:
  1 to 4 switch slots (hands, pistol, bat), the left mouse button fires or swings, R reloads, E picks
  up, G throws, C crouches, and F with empty hands spawns a prop.
- Tab shows the scoreboard, the mouse orbits the camera and the wheel zooms.
- Esc opens the in-game menu (see [The menu](#the-menu)), F1 toggles the debug HUD.

The HUD shows the predicted and confirmed ticks, round-trip time, clock error, rollbacks, stalls and
checksum results.

Server and clients can be built with different compilers. The server rejects a client whose
simulation fingerprint differs from its own.

## Characters

Everyone on a server plays as one character, chosen by the server:

```bash
cb_server                      # the default: mannequin, shipped with the game
cb_server --character robot    # a workshop item [--workshop DIR]
cb_server --character none     # the procedural placeholder rig
```

| Kind | Lives in | Server reads | Players need |
|---|---|---|---|
| shipped with the game (`mannequin`) | `godot/characters/<name>/` | `bin/characters/<name>/` (the build copies the baked files there) | nothing: it is in the base game |
| workshop item (`robot`) | `characters/<name>/client/` | the item's zip in the workshop | the exact item (by SHA-256) |

### The default character: the mannequin

The [Universal Animation Library](https://quaternius.com) mannequin by Quaternius (CC0), in
`godot/characters/mannequin/`:

- **Source**: `source/UAL1_Standard.glb`, the in-place version (the `_RM` files carry root motion,
  which the simulation does not want: it moves the player itself). Its import retargets the
  UE-style bones (`pelvis`, `spine_01`, `upperarm_r`) onto `SkeletonProfileHumanoid` with
  `source/bone_map.tres`.
- **State machine**: an ordinary `AnimationTree` in `character.tscn` (see
  [State machines](#state-machines)): locomotion, jumps, and an upper-body layer for the pistol
  (with a shot on `pistol.fired`) and the bat (with a swing that strikes on a marker; the bat's
  fire is the melee mod's own look).
- **Hitboxes**: capsules along the spine, arms and legs sized from the bone lengths, a head
  sphere, a hips box.
- **Edit it** in the editor like any scene: saving the scene bakes it. The generator that made
  it only re-bakes an existing scene; `-- --force` builds it from scratch (and discards edits):

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_mannequin.gd
```

Items held in a hand (the pistol, the bat) use a hand frame that is the same on every rig
(`AnimSet::AttachFrame`), so an item scene made for the hand socket sits the same on the
mannequin, the robot and the placeholder rig alike.

### The paid animation pack (local only)

The library's paid **Source** version has 120 animations, among them jogs in eight directions. It
must never reach GitHub (the repository is public), so neither it nor anything baked from it is
committed:

| Where | What | In git |
|---|---|---|
| `Universal Animation Library[Source]/` | the purchase | ignored |
| `godot/characters/ual_mannequin/` | `source/UAL1.glb`, the scene, the baked clips | ignored |
| `make_mannequin.gd --pack=source` | how to build it | committed (names only) |

To build it: copy `Unreal-Godot/UAL1.glb` to `godot/characters/ual_mannequin/source/`, give its
import the same settings as `mannequin/source/UAL1_Standard.glb.import` (the bone map, and
`Sword_Attack` saved to `res://characters/ual_mannequin/animations/`), then:

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_mannequin.gd -- --pack=source --force
```

Its locomotion is a 2D blend space: idle in the middle, the eight jogs on a circle at the game's
jog speed (3 m/s), a walk inside and the sprint ahead; `turn_legs` is off and `face_forward` on. `cb_server` picks
`ual_mannequin` by default where it is built, and `mannequin` everywhere else; its test
(`ual_mannequin`) skips itself when the character is not there, as on CI.

### Workshop characters

A workshop character is a **workshop item**, like a mod's look: the server announces it by name and SHA-256
and players must already have that exact item (a missing one refuses the join). It is shipped in
Godot's runtime format, and **nothing is imported or converted when a player joins**: the item
already holds what the game needs, baked in the editor.

| In the item, under `characters/<name>/` | Made by | Read by |
|---|---|---|
| `character.tscn`: the model, a `CbCharacter` at the root, a `CinderboxSkeleton`, `CbHitbox` zones | the author | clients (the player prefab) |
| `skeleton.ozz`, `clip_<n>.ozz` (one per animation the tree plays), `anim.cfg`, `graph.cfg` | the bake (on save, and when the item is packed) | clients (poses) and the server (the state machine, hit tests) |
| `hitboxes.cfg` | the bake | the server |

With `--character none`, players use the procedural placeholder rig, which has default zones
(head, torso, arm, leg).

### Making a character

1. Make an item project: `characters/<name>/client/` with `project.godot` and a "Mod" export
   preset (copy `characters/robot/client`). `tools\pack_mod.ps1` copies the Cinderbox viewer extension into
   it; do that once (or copy `godot/cinderbox.gdextension` and `godot/bin`) before opening it in
   the editor.
2. Import the model with Godot's humanoid retargeting: in the import dialog, Skeleton3D → Retarget
   → Bone Map with `SkeletonProfileHumanoid`, and the rest fixer's Apply Node Transforms and
   Normalize Position Tracks. Bones then have profile names (`Hips`, `Head`, `LeftUpperArm`...).
3. Make `res://characters/<name>/character.tscn`:
   - a `CbCharacter` root: `character_name`, and the paths to the `Skeleton3D`, the
     `AnimationPlayer` and the `AnimationTree`;
   - the `AnimationTree`: the character's state machine ([State machines](#state-machines)). Copy
     the robot's or the mannequin's to start: legs by speed and a jump on a base layer, the mods'
     stances on an upper-body layer;
   - the imported model under it, with the `Skeleton3D` at the root's origin, unrotated and
     unscaled (the bake checks this);
   - a `CinderboxSkeleton` whose `skeleton_path` points at the `Skeleton3D`, with `retarget` off
     (the baked skeleton is that skeleton) and `draw_bone_boxes` off;
   - `BoneAttachment3D` nodes with `CbHitbox` children: sphere, capsule or box shapes, each with a
     `zone` ("head", "torso", "arm", "leg", or your own);
   - the aim chain on the `CbCharacter`: `aim_chain` (bones with weights, turned in order, e.g.
     `UpperChest:0.3 RightUpperArm:1`) and `aim_tip` (the bone that ends up on the line of sight).
     The default, `RightUpperArm:1` to `RightHand`, points the right arm.
4. Save the scene. That bakes it: the `.ozz` files, `anim.cfg`, `graph.cfg` and `hitboxes.cfg` are written next
   to the scene (clips are sampled at `sample_rate`, 30 Hz). Publishing bakes it again from the
   scene that ships, so an item is never stale. The **Bake character** button on the
   `CbCharacter` does the same by hand (for an animation saved to its own file, which a scene save
   does not see). A bake that changes nothing writes nothing.
5. List `character.tscn` in the preset's `export_files` and the baked files in its
   `include_filter` (see the robot's preset), then publish:

```powershell
powershell -ExecutionPolicy Bypass -File tools\publish_mod.ps1 -Character <name>
```

`characters/robot` is a complete example: a rigid robot, taller than the built-in rig, generated by
a script so a fresh clone can rebuild it (`make_character_example.gd`), then baked by the same code
as the button:

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_character_example.gd -- --out=<abs path>/characters/robot/client/characters/robot
```

### Hit zones

Gameplay mods cast rays with `ctx.CastRay`. Players are hit by their character's hitboxes, posed
from the simulation's animation state at that tick, and `hit.zone` names the zone. Only the server
does this (mods run there), so hit tests cost clients nothing and never enter the rolled-back
simulation. The pistol multiplies its damage by `pistol.zone.<zone>`.

### Aiming

Aiming is part of the pose. A mod sends `ctx.Aim( player, true )` (the pistol does while it is out),
and the pose turns the character's aim chain toward where the player looks, relative to its body.
Every client draws that and every server hit test uses it, so a raised arm can be hit where it is
seen. Respawning keeps the aim; only the mod lets it go.

### Facing

| Mode | The body | Set by |
|---|---|---|
| Freelook (default) | turns toward where the player walks; the camera looks around freely | nothing |
| Camera-facing | faces where the camera looks, every tick (a shooter's stance) | `ctx.FaceCamera( player, true )` |

The pistol switches to camera-facing while it is out, together with aiming. In camera-facing the
legs still walk where the player goes: the hips turn toward the direction of travel (up to 90
degrees) and the spine turns back, and moving away from the facing plays the walk cycle backwards.
This works with any character's forward clips, no strafe clips needed.

### Layers and stances

A mod says what a player is doing; the character says what that looks like.

| Who | Does | Example |
|---|---|---|
| a mod | declares names: `d.Layer( "upper" )`, `d.Stance( "pistol" )`, and sets a stance on a layer: `ctx.SetStance( player, layer, stance )` (a default `StanceHandle` clears it) | the pistol sets `pistol` on `upper` while it is out; the bat sets `melee`, and `melee_swing` for a swing |
| a character | reads stances by name in its state machine's conditions and layer weights | the mannequin's upper layer: `Rest -> Pistol` when `pistol`, weight `pistol or melee or melee_swing` |

- A stance is a name and nothing more: no clip is tied to it. A character whose machine never
  reads `pistol` simply does not change when the pistol comes out.
- Stances are part of the simulation's animation state, so everyone draws what they lead to and
  the server's hit tests use it.
- The mannequin and the robot have states for the shipped mods' stances; the placeholder rig has
  none (the engine carries no game content).

### An animation's other tracks

A character's animations are ordinary Godot animations, authored once in its `AnimationPlayer`:
bone tracks next to any other track. One rule says where each track goes:

| Tracks | Become | Played by |
|---|---|---|
| position / rotation / scale of the `Skeleton3D`'s bones | ozz clips (the bake) | the pose (drawn by clients, hit-tested by the server) |
| everything else: value (`emitting`, `visible`, `light_energy`, material colours), method, audio, animation | nothing: they stay in the animation | each client, in step with the pose |

So a flame on the swing is two keys on the swing animation (`Flame:emitting` on at 0.1 s, off at
0.34 s), and a whoosh is an audio key, all in Godot's animation editor. There is no second file
and nothing to bake for them: when a character is first drawn, the game copies the non-bone
tracks out of its `AnimationPlayer` and plays them, per channel (the base locomotion, each stance
layer), at the clip and time the pose is playing.

- Values land exactly; method and audio keys fire once (a rollback that replays a moment does not
  fire it again; a long jump such as a join fires nothing).
- A channel whose clip is bones only returns to the `RESET` animation's values.
- No scripts: tracks call built-in methods (`restart`, `play`) or set properties.
- Looks only: the server never sees these tracks.
- With network delay, a swing the server started is first seen a little way in, and method and
  audio keys before that point do not fire (values do). Sounds that must not be missed belong to
  a [reaction](#reactions) on the mod's cue.
- Animation packs are bones only: their other tracks are not played.

The robot's bat swing has a fire trail and a whoosh made this way. Put the nodes the tracks reach
(particles, lights, an `AudioStreamPlayer3D`) in the character scene, and list the sounds in the
item's export preset.

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/check_track_player.gd
```

### Animation packs

A mod can ship **layers** of an AnimationTree and swap a player's own layer of the same name for them:
a crouch walk for `Base`, a swim, a limp. The character keeps its other layers (the pistol still aims
while crouched).

| Step | Where |
|---|---|
| author | a `CbAnimPack` scene in the mod's client project: a model on a humanoid-profile skeleton, its AnimationPlayer, an AnimationTree whose state machines are named like the characters' layers; **Bake** writes `anim/<pack>/` |
| declare | `declare.AnimPack( "sneak.crouch" )` in the server mod |
| swap | `ctx.SwapLayer( SlotTarget( slot ), pack, "Base" )`, back with `ctx.RestoreLayer( ..., "Base" )` |
| fit | the game rebuilds the pack's clips for each character's skeleton by profile bone names, once (as they are when the skeleton is the same) |

The swap is simulation state: the server's hit tests and every client pose the swapped layer, and a
rollback replays it. The server reads the pack from the mod's workshop item; its graph travels in the
schema; each player's pack clips come from the item they subscribed to.

Retargeting assumes characters imported the Godot way: the humanoid bone map, the rest fixer's
**Overwrite Axis**, and **Fix Silhouette** when rest shapes differ (T-pose vs A-pose), so a joint's
turn from rest means the same on every skeleton. `sneak` is the example: hold C to crouch.

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_sneak_pack.gd -- --out=<abs>/server_mods/sneak/client/anim/sneak.crouch
```

**Items bring layers.** A mod can tie a pack to an item kind,
`declare.ItemLayers( bat, declare.AnimPack( "melee.carry" ) )`: while a player holds one (in use, not stowed), the pack's layers play instead of the player's own of the same names,
and stop when it is dropped. A mod's own `SwapLayer` on the same layer wins while it lasts, so a
crouch still crouches with a bat in hand and the carry returns when the player stands up. The bat's
pack replaces `Base`: standing ready, a measured walk, the usual jog.

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_carry_pack.gd -- --out=<abs>/server_mods/melee/client/anim/melee.carry
```

### Held items and sockets

A weapon, a torch, a shield: a **held item** is a simulation entity of its own (NetId, board,
events), held by a player in a **socket**. One small command changes its state; everything it
looks like is authored in Godot.

| Piece | Where | What |
|---|---|---|
| socket | the character scene: a `CbSocket` under a `BoneAttachment3D` | where items go, in the item's frame (grip at the origin, pointing along -Z); `RightHand` and `LeftHand` exist on every character (made at the hands if the scene has none) |
| item kind | the mod: `declare.ItemKind( "melee.bat" )` | spawned with `ctx.SpawnItem( SlotTarget( slot ), kind, socket )`, addressed with `ItemTarget( slot, socket )` for `Set`, `Emit`, `Destroy` |
| look | a `CbItemLook` node (kind -> scene) in the mod's `vfx/reactions_<name>.tscn` | drawn as the socket's child `Item` |
| item state and events | `CbReaction` nodes in the item scene | flames on its holder's `melee.swing`, glow while `melee.hot`, sparks on its holder's `melee.hit` (see [Reactions](#reactions)) |

One attack, resolved by what is held, with no client code:

```
server:    Emit( attack, SlotTarget( slot ), value )          one small command
character: Idle -> Heavy      [attack == 2]                    priority 0
           Idle -> BatSwing   [attack and melee.bat]           priority 1
           Idle -> SwordSlash [attack and melee.sword]         priority 1
           Idle -> Punch      [attack]                         priority 2
item:      its own CbReaction on "attack" (subject: its holder), if it has one
```

The body's choice runs in the simulation, so the server's hit tests follow it; each item decides
what an attack looks like on it.

The bat: the melee mod spawns a `melee.bat` when the bat is taken out. Its look is its own, three
reactions in `prefabs/bat.tscn`:

| Reaction | On | Does |
|---|---|---|
| `FlamesOnSwing` | its holder's `melee.swing` (predicted for your own swing) | plays the bat's `slash` animation (flames along the barrel, on and off) |
| `GlowWhileHot` | while `melee.hot` (set by a hit) | the barrel glows |
| `SparksOnHit` | its holder's `melee.hit` | a burst of sparks |

Nothing in the character knows about the bat: any character swings any item, and an item with no
reaction on the swing is simply quiet. The pistol works the same way.

A mod taking its item away destroys the NetId `ctx.HeldItem( slot, socket )` gives, not
`ItemTarget`: another mod may put its item in that socket in the same tick (a weapon swap), and
`ItemTarget` would find that one. What is in the hand decides: the melee mod swings any `melee.bat`
in the right hand and the pistol fires any `pistol.gun`, wherever it came from. Looks follow the
same rule: the pistol's HUD and its predicted shot ask `pistol.gun` (true while the player has one
in use), never which slot is out, so a picked-up pistol shows its ammo and a holstered one does not.

### The inventory

An item a player carries is **in use** (in a hand) or **stowed** (carried, in no hand). Stowing is
the engine's; who carries what in which slot is the `inventory` mod's.

| Engine verb (`Context`) | What it does |
|---|---|
| `GiveItem( holder, kind, holster )` | a new item, stowed |
| `StowItem( item, holster )` | puts a held item away; `holster` is the socket it is drawn in meanwhile (none: out of sight) |
| `HoldItem( item, socket )` | takes a carried item in use; does nothing if that socket has one in use (stow that first, same tick) |
| `PickUpStowed( holder, item, holster )` | from the world straight to stowed |
| `CarriedItems( slot )` | everything the player carries, in use and stowed |

A stowed item is in no hand: `HeldItem`, item layers, state machine conditions and item-kind
conditions in looks do not see it. It keeps its entity, its board and its look, and drops like any other.

The `inventory` mod's rules:

| Situation | What happens |
|---|---|
| A life starts | Every item kind with `inventory.start` is given, stowed |
| A slot key (1 to 4) | That slot's item comes into the right hand; what was in the hand is stowed. Nothing is made, destroyed or dropped |
| An item is picked up (E) | It goes to its kind's slot and comes into the hand. If that slot had an item, the old one drops (one per slot). Other slots are untouched |
| G | Throws what is in the hand; its slot is empty until something is picked up |
| Death | What the life started with is taken back; anything else carried drops where the player stood. The next life starts with the slot that was out |

The mod's look (`server_mods/inventory/client`, a workshop item like the others) is a row of slots
along the bottom of the screen, all data: `inventory.item_N` holds the NetId of slot N's item, a
label shows `{look:inventory.item_N}` (what that item is called), and the slot that is out
(`inventory.slot == N`) is highlighted.

Other mods describe their items with properties and never touch the slots:

```cpp
m_bat = declare.ItemKind( "melee.bat" );
declare.ItemProperty( m_bat, "inventory.slot", 3.0f );                       // lives in slot 3 (2..4); without it: the first free slot
declare.ItemProperty( m_bat, "inventory.start", 1.0f );                      // every life starts with one
declare.ItemProperty( m_bat, "inventory.holster", declare.Socket( "Back" ) ); // optional: where it hangs while stowed
```

**Holsters are optional, twice over.** The mod chooses whether its item has one, and the character
chooses whether it has that socket: a `CbSocket` node named like it (`Back`, `Hip`) under a
`BoneAttachment3D`, moved in the editor like the hand sockets. Without either, a stowed item is
simply out of sight. The mannequin has both: the bat hangs across the back, the pistol on the right hip.

### Items in the world

An item can also **lie in the world**: the same entity (NetId, board, look) with a physics body,
so it falls, tumbles, gets shot across the floor, and does so identically on every screen.

| Mod API | Does |
|---|---|
| a `CbItemBody` in the item's scene | its body in the world: a box or a sphere with a mass, placed from the grip (the bat: a 0.82 m box whose centre is 0.31 m in front of it). Authored with Godot's shape gizmo, baked to `items/<kind>.cfg` |
| `declare.ItemKind( "x", BoxItem( half, center, mass ) )` | the same from code, for a mod without a look (`SphereItem` too); a baked body replaces it |
| `ctx.SpawnWorldItem( kind, grip, rotation, velocity )` | one on the floor |
| `ctx.DropItem( item, grip, rotation, velocity )` | out of the hand; thrown if it has a velocity |
| `ctx.PickUpItem( SlotTarget( slot ), item, socket )` | into a free socket (drop what is there first, in the same tick) |
| `ctx.ItemsNear( point, radius )`, `ctx.Items()`, `ctx.ItemKindOf( id )`, `ctx.ItemHolder( id )` | what lies around, nearest first; every item; what and whose |
| `ctx.SpawnItem` into a taken socket | drops what was there (it may be one someone picked up) |

The body is **authored in Godot and baked**, like a character's hit zones: put a `CbItemBody` in
the item's scene, give it a `BoxShape3D` or `SphereShape3D` and a `mass`, and move it to where the
shape's centre is. The same node carries what else the server should know about the item:

| On the `CbItemBody` | Meaning |
|---|---|
| `shape`, its position | the body when it lies in the world |
| `mass` | kg |
| `properties` | named numbers any server mod may read, e.g. `pickup.hold_seconds` = 0.5. They replace what the item's mod declared in code for the same name |
| **Bake item body** (button) | writes `res://items/<kind>.cfg` for every kind whose `CbItemLook` draws this scene. Save the scene first |

Publishing the mod bakes every item too (`tools\publish_mod.ps1`, or by hand below) and ships the
files in the item; the server reads them from there (`item melee.bat: body from mod melee's item
(box, 1.10 kg)`, `item melee.bat: pickup.hold_seconds = 0.5`). The baked files are committed with
the mod, so servers and tests built from source have them. A change reaches servers when the mod is
published again.

```sh
godot --headless --path server_mods/melee/client --script <repo>/godot/addons/cinderbox_maps/bake_items.gd
```

Who may pick up what, and when, is a mod's. The **pickup** mod is the example:

- Near an item (1.5 m along the ground, not behind you), its NetId goes on your board as
  `pickup.target`. **E** takes it, **G** throws what you hold. With the `inventory` mod running, a
  taken item goes to its slot (see [The inventory](#the-inventory)); without it there is only the
  right hand: what was there drops, and dying drops it.
- **Hold to pick up**: an item may take a moment: `pickup.hold_seconds` in its `CbItemBody`'s
  `properties` (the bat: 0.5; the pistol is a tap), or `declare.ItemProperty( kind,
  "pickup.hold_seconds", 0.5f )` in its mod. `pickup.hold` says how long the item in reach needs,
  and while E is held on it `pickup.since` is the tick the hold began (0: none). Letting go or
  losing the item starts over. The board changes when a hold starts and ends, not every tick.
- Its look is a proximity prompt, all data: a world reaction while `$local`'s `pickup.target` is set
  puts a `CbPromptLabel` on the item it names, `$local@pickup.target`, and moves it when that
  changes: `"[{key:pickup}]  Pick up {look:pickup.target}"` for a tap, `"Hold [...]"` with a bar
  that fills (`since_field = "pickup.since"`, `duration_field = "pickup.hold"`: the label counts from
  the game's clock) for an item that needs holding.

```
PickupReactions            (vfx/reactions_pickup.tscn)
├── Prompt      while  $local: pickup.target, pickup.hold == 0   prompt.tscn        "[E]  Pick up Pistol"
└── PromptHold  while  $local: pickup.target, pickup.hold > 0    prompt_hold.tscn   "Hold [E]  Pick up Bat" + bar
                both under $local@pickup.target
```

**Item properties** are how mods agree on what an item is like without knowing each other: a named
number on an item kind (`declare.ItemProperty( kind, name, value )`) that any mod reads with
`ctx.ItemProperty( kind, name, fallback )`. The melee mod says how long its bat takes; the pickup
mod is the one that cares.

The same pieces make any prompt: a mod puts an entity's NetId in a field, its look shows a
`CbPromptLabel` on `$local@thatfield`.

The **expire** mod keeps the floor clean: an item someone held and then left lying is removed after
`expire.seconds`; picking it up stops the clock. Items nobody ever held (a map's own, the seeded
ones) stay.

### Reactions

A `CbReaction` node makes a scene react to the game, with no code, the way a Roblox script uses
its hierarchy: `^^/RightHand/Item` is "the item in my holder's right hand". It is part of a
standalone Godot addon (`src/godot/cue`, godot-cpp only): a **`CbDirector`** runs the reactions
under it from **cues** ("melee.hit" at an entity) and **entity state** ("melee.hot" = true). In
the game the director is the client's `World` node, and the client is only an adapter that tells
it what the simulation shows.

```
World (CbDirector)
├── player_0                       an entity: state {combat.health, loadout.slot, ...}
│   ├── (the character's scene)
│   ├── RightHand, LeftHand, Head  sockets: children of the entity in the game, on every rig
│   │   └── Item                   the held item, an entity too: state {melee.hot}
│   │       ├── Barrel, Sparks
│   │       ├── GlowWhileHot       CbReaction  while melee.hot          set Barrel emission = 4
│   │       └── SparksOnHit        CbReaction  on melee.hit, subject ^^  Sparks.restart()
├── player_1 ...                   players by slot; everything else <kind>_<net id>
├── Map                            the map's own scene
└── PistolReactions ...            world reactions (vfx/reactions*.tscn)
```

Every node path in a reaction is a Godot `NodePath`; its first name may be an **anchor**:

| Path | Means |
|---|---|
| `Barrel`, `../Sparks` | an ordinary path from the reaction |
| `^` | my entity: the nearest entity at or above the reaction (the default subject) |
| `^^`, `^^^` | the entity above that one (a held item's holder), and so on |
| `$at`, `$other` | the entities the cue names: who it is about (the attacker), the other one (the victim) |
| `$local` | the local player |
| `$world` | the World node (its state is the global board) |
| `$local@pickup.target`, `@field` | the entity whose NetId is in that entity's state field (`@field`: my entity's) |
| `^^/RightHand/Item`, `$other/Head` | an anchor, then an ordinary path from it |

A path that finds nothing (an empty hand) makes the reaction do nothing, and no path leaves the
World node: a workshop item cannot reach the game's HUD or menus.

| Field | Meaning |
|---|---|
| `when` | **On a cue** (once per cue) or **While** (its conditions hold) |
| `event` | On a cue: its name (`melee.hit`, `footstep`: see [Effects](#effects)). Your own press shows at once through a [prediction](#predictions) |
| `subject` | a path (default `^`): whose state plain condition names read |
| `event_side` | On a cue: **A**, the cue is at the subject (`melee.hit` is at the attacker); **B**, the subject is the other one (the victim); or **Either**. A subject starting with `$at` / `$other` matches every cue of the name |
| `subject_kind`, `subject_template` | only for a player / prop / static / ragdoll / item, or one map template (for an item: its kind, `melee.bat`) |
| `conditions` | [expressions](#effects), all of which must hold; also `is_local`, `event.value`, `event.strength`. Plain names read the subject's state, then the world's; a path and a colon read another's: `!^^:combat.dead`, `$other:combat.health < 20`, `$world:deathmatch.round` |
| `delay`, `chance`, `cooldown` | cue reactions: act N seconds later, only sometimes (0-1), and not more often than every N seconds |
| `wait_for_server` | cue reactions: do not act on a [predicted](#predictions) press, act when the server's cue comes |
| `animation_player`, `animation` | play this animation from the start; `animation_off` when a While ends (without one, the animation stops) |
| `target`, `property`, `value` | set a property on a node; a While puts the old value back when it ends. Sub-paths work: `surface_material_override/0:albedo_color` |
| `value_expression` | instead of `value`: the property becomes an [expression](#effects)'s value (`combat.health / combat.max_health`, `event.strength * 0.1`). A While keeps it up to date while it is on; a bool property gets true / false, an int a whole number. Reading `event.*` waits for the server's cue |
| `blend_time` | fade the property there (and back) instead of snapping: numbers, vectors, colours |
| `target`, `method`, `method_args` | call a method: `restart`, `play` `["slash"]`, `set_visible` `[false]` |
| `scene`, `scene_parent`, `scene_lifetime` | add a scene (under the reaction's parent by default); a cue's goes after `scene_lifetime` s, a While's when it ends |
| `place`, `place_node`, `offset` | where the scene and sound go: under its parent, at the cue's **point** or **end**, a **beam** from `place_node` (or the point) to the end, **at** `place_node` (`$at/RightHand`), or **following** the subject |
| `sound`, `volume_db`, `pitch_scale`, `pitch_jitter`, `bus`, `max_distance` | a sound, once per firing |
| `volume_expression` | how loud this firing is, as a factor on `volume_db`: `event.strength / 4` (0 plays nothing, 1 is `volume_db`, at most 4) |
| `shake`, `shake_time`, `flash_color`, `flash_time` | camera shake and a full-screen flash (the director's `screen_effect` signal): the viewer's, so pair them with `is_local` |

- **Presentation only**: nothing here changes the simulation. Everything that exists in the game
  (an item, a prop) is spawned by a server mod.
- **State is visible**: every entity node carries its state as `state` metadata, so the Remote
  inspector shows `melee.hot` changing live.
- **Sockets are children of the entity** in the game, whatever bone they were authored under, so
  paths through them are the same on every character. The character's animation tracks that
  reached an item through a socket's authored place (the bat's `slash`) are pointed at the new
  place when the game loads them.
- **A While follows its target**: when its path finds another node (a new item in the hand), it
  ends on the old one (puts values back) and starts on the new one.
- **Only listed methods**: `restart`, `play`, `play_backwards`, `stop`, `pause`, `queue`, `seek`,
  `advance`, `show`, `hide`, `set_visible`, `set_emitting`, `set_text`, `set_value`, `set_frame`,
  `set_modulate`, `set_volume_db`, `set_pitch_scale`, `set_speed_scale`. Anything else is not called
  (set it with `property` instead); the `script` property and metadata are never set. A scene a
  reaction adds goes through the scene guard first. A path or condition that does not parse is a
  configuration warning on the node (`^^combat.health` asks for its colon), and the game skips that
  reaction with a warning.
- **Nothing is left behind**: a While that leaves the tree while on (an item put away, a world scene
  reloaded) puts back what it set and frees its scene.
- **Help in the editor**: every group in the inspector starts with an info line (click the icon for
  more), every property has a hover text, and F1 on `CbReaction` opens its class reference. Anchors
  are typed into a path field through its ⋮ menu, Edit.
- **Resources are shared** between instances of a scene. A reaction that changes a material changes
  every copy, unless the material is **Local to Scene** (the bat's barrel is).
- **Rollback**: like an animation's other tracks, a cue reaction that already played is not taken back if a
  prediction turns out wrong.
- **Anything can drive a director**: `add_entity( node, kind, template )`, `set_state( node, {...} )`,
  `set_world_state`, `set_local`, `cue( name, at, other, { value, strength, point, end } )`,
  `press( action )`.
  `check_reactions.gd` drives one by hand, with no server:
  `godot --headless --path godot --script res://addons/cinderbox_maps/check_reactions.gd`.

#### Cue Preview (editor)

Every project with the Cinderbox extension has a **Cue Preview** bottom panel: it plays the edited
scene's reactions with no game running. The scene is copied onto a small stage (unsaved edits
included; the edited scene is never touched), next to two stand-in players with `RightHand`,
`LeftHand` and `Head` sockets:

| The scene is | It goes | Found by |
|---|---|---|
| a held item (the bat) | `player_0/RightHand/Item`, so `^^` is player_0 | default |
| a character | `player_0` itself | it has a `Skeleton3D` |
| world reactions (`vfx/reactions*.tscn`) | under the World | subjects start with `$`, nothing to draw |

The mode can be picked by hand, and **Reload** copies the scene again after edits.

- **Fire a cue**: its name (the scene's own cues are listed, and the game's), `$at` and `$other`
  (player_0, player_1, the item), `value` and `strength`. The cue's point is `$other`'s chest.
  Below, every reaction listening for it says what it did, or why not ("the subject (player_1) is
  not at ($at) in this cue", "condition \"melee.hot\" is false", "cooling down").
- **State**: every name the scene's conditions read gets a field, on player_0, player_1, the item
  or the world (0 is false). The bat: `melee.hot` = 1 lights the barrel, 0 puts it out.
- **local**: which stand-in is the viewer (`$local`, `is_local`).
- Screen effects flash and shake the preview; sounds play. Each control has an info icon.
- Like the game's other bottom panels, it only draws while it is showing (opening a character
  brings up the Animation panel instead).

### State machines

A character's animation logic is authored as a Godot `AnimationTree`, the normal way, and baked.
Every character has one: it is the only thing that chooses what a player's body plays. The
simulation runs the baked machine every tick, so the server's hit tests and every screen agree and
rollback replays it exactly; the tree itself never runs in the game.

| In the tree | Baked as |
|---|---|
| root: a state machine, or a blend tree of state machines stacked with `Blend2` nodes | layers (at most 4); a `Blend2`'s filter is the layer's bone mask |
| states: `Animation` nodes, `BlendSpace1D`, `BlendSpace2D` (points are animations, play mode forward or backward) | clip states, blend states (phase-synced, so feet stay in step; 2D blends inside Godot's triangles) |
| transitions: Auto advance, advance condition, advance expression, priority, crossfade, Immediate / At End | the same (Sync switching becomes Immediate; crossfades are linear) |
| markers on animations | the mod event of the same name, from the player, when the clip passes it |
| every other track (particles, sounds, lights) | stays in the animation, played by clients in step ([An animation's other tracks](#an-animations-other-tracks)) |

Set it up on the `CbCharacter`:

- `animation_tree_path`: the tree. Its `root_node` should be the model (tracks' paths start there).
- `graph_inputs`: what drives the tree's numbers, by parameter path:
  `"Base/Locomotion/blend_position": "forward_speed"`, `"UpperBlend/blend_amount": "pistol or melee"`.
  A 2D blend space takes two expressions, x then y: `"move_right, move_forward"`.
- `turn_legs`: on (the default), the hips turn toward the direction of travel so a forward walk
  goes sideways. Turn it off for a character with its own directional clips (strafes).
- `face_forward`: off by default. On, the spine is turned back by however much the clips turned the
  hips, so the chest faces where the body faces (strafe clips often turn the torso toward the
  travel); the head keeps looking ahead. The legs still run where they run.

Conditions and expressions read simulation values, never scripts:

| Name | Value |
|---|---|
| `speed`, `forward_speed` | smoothed ground speed (m/s); negative forward_speed while backing up |
| `move_forward`, `move_right` | smoothed ground velocity in the body's frame (m/s): for directional blend spaces |
| `vertical_speed`, `grounded`, `airborne_time`, `jumped` | the body's movement (`jumped`: on the tick of a jump) |
| `aiming`, `backward`, `state_time` | a mod's Aim; walking backwards; seconds in the current state |
| a stance's name (`pistol`, `melee`) | true while any layer has it (mods' `SetStance`) |
| a mod event's name (`pistol.fired`, `attack`) | on the tick it is emitted at this player: its value (1 if the value is 0), so `attack` and `attack == 2` both work: a trigger |
| a board field's name (`loadout.slot`) | the player's value (or the global one) |
| an item kind's name (`melee.bat`) | true while the player holds one, in any socket |

The grammar is the [one expression language](#effects) (`and or not`, comparisons, arithmetic,
`?name`, parentheses). A name no mod declares reads as 0 (the server logs it). The bake fails with a reason for anything it cannot run (nested
state machines, other blend nodes, a missing animation).

A mod times its effect by the animation with `ctx.AnimationEmits( event )`: the melee mod hits on
the mannequin's `melee.strike` marker, and on its own timer for characters without one. To edit an
imported animation (add a marker or a track), save it to a file in the import settings (Save to
File, Keep Custom Tracks), as the mannequin does with `Sword_Attack`. Check markers after a
reimport: a reimport in Godot 4.7.1 kept the added track but dropped the marker (4.7.2 kept both).
The mannequin's generator puts its marker back whenever it bakes.

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
| `godot --headless --path godot --script res://addons/cinderbox_maps/check_object_source.gd -- FILE.cbv` | Checks that the viewer draws from any object that hands it packets: a GDScript source reads a view file, with no peer extension involved |
| `godot --path godot -- --autoplay=S --screenshot=F.png --screenshot-every=S2` | Unattended client; also saves `F_1.png`, `F_2.png`, ... and prints the mod events it saw |
| `scripts/stress_test.sh --bots N --full M --latency MS --jitter MS --loss % --rollback T` | Starts a server, the simulator and the bots, and prints a summary |

Replay viewer controls:
- Space pauses, Up/Down change the speed, Left/Right seek 5 s, and `,` / `.` step one tick.
- Home restarts, Tab follows the next player, and Backspace switches to a free camera.
- The right mouse button or Esc orbits the camera.

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

`tests/reference_hashes.txt` holds the per-tick state hashes of the reference scenario. Any change to
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

## Layout

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
  simulation.*      the engine: level, character mover, props, commands, ragdolls, snapshots, hashing
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
src/stream/       StreamSource: a view source that is sent frames by a server (networking, no simulation)
src/present/      engine-independent presentation, what the Godot extensions draw from
  visibility.*      a frame with what one viewer must not be shown taken out
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
                  CbItemLook, HUD labels (cinderbox_hud.*). No simulation, no networking
  object_source.*   a source that is a Godot object handing over packets (the peer, or a script)
  peer/             the peer GDExtension (cinderbox_peer): CinderboxPeer, the sources that simulate
  stream/           the stream GDExtension (cinderbox_stream): CinderboxStream, the source a server sends frames to
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
tools/            pack_mod.ps1, publish_mod.ps1, export_client.ps1, bake_map.ps1, make_sfx.py (placeholder sounds)
```

## Credits

- Default character and its animations: [Universal Animation Library](https://quaternius.com) by
  Quaternius, CC0 (`godot/characters/mannequin/source/LICENSE.txt`).
