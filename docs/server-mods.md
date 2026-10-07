# Server mods

The game's rules: C++ mods compiled into `cb_server`, and the workshop items that carry their looks. Part of the [manual](../README.md#the-manual).

## Writing a server mod

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
| `SetMove` | sets one of a player's [movement parameters](#movement-parameters) (walk speed, jump, gravity, ...), or gives it back |

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

Add the folder, re-run CMake, and the mod is in `cb_server --list-mods`. A file named `disabled` in
the folder keeps the mod compiled in but off ([Switched off by default](#switched-off-by-default)). The server sends every client
a **schema** on join: field names and types, event names, and action names with suggested keys. The
Godot client binds those keys (InputMap actions `cb_<name>`), and reactions refer to fields and events
by name.

The mods that ship:

| Mod | Declares | Rules |
|---|---|---|
| `combat` | `combat.health`, `.max_health`, `.dead`, `.kills`, `.deaths`; hears `combat.damage`, `combat.heal`, `game.round_start`; says `combat.hurt`, `combat.killed`, `combat.respawned` | the one place health lives: applies any mod's damage, credits the kill, leaves a ragdoll, brings the player back. See [The combat mod](#the-combat-mod) |
| `inventory` | `inventory.slot`, `inventory.item_1` .. `item_3`; `declare.Slots( 3 )` | the rules of [the inventory](items.md#the-inventory): three slots (the slots themselves, and selecting, moving and dropping, are the engine's, predicted); a life starts with every kind that says `inventory.start`; dying takes those back and drops the rest; empty hands are freelook |
| `melee` | layer `full`, stances `melee`, `melee_swing`; events `melee.swing`, `melee.hit`, `combat.damage` | the bat: a full-body stance while it is out; left mouse swings (0.45 s, every 0.6 s), a fan of 1.8 m rays from the chest at the strike, spread sideways and pitched as far up or down as the player looks (look at the floor to hit what is low), 40 damage through `combat.damage` |
| `props` | action `spawn_prop` (F) | F with empty hands throws a prop (the map's spawnable template, or a random box or sphere) |
| `pistol` | `pistol.ammo`, `pistol.reloading`; the use button (left mouse), `reload` (R), `mark` (right mouse); events `pistol.fired`, `pistol.hit`, `pistol.reload`, `pistol.dry`, `pistol.scan`, `pistol.marked`, `combat.damage` | hitscan from the camera pivot, 25 damage (the head doubles it) through `combat.damage`, 12 rounds, 1.5 s reload; the `pistol` stance on the `upper` layer while it is out; a new life (`combat.respawned`) comes with a full magazine |
| `rifle` | `rifle.ammo`, `rifle.reloading`; the use button (left mouse, **held**), `reload` (R); events `rifle.fired`, `rifle.hit`, `rifle.reload`, `rifle.dry`, `combat.damage` | the automatic one: a shot every 0.1 s for as long as `fire` is held, 14 damage (the head doubles it), 30 rounds, 2 s reload; slot 4, on the back while put away. Held on an empty magazine it clicks once, reloads and fires on. While it is out the `upper` layer has the `rifle` stance, and the item brings the `rifle.hold` animation pack (`ItemLayers`): its `UpperBody` layer plays instead of the character's. **Its look is not in the repository** (`server_mods/rifle/client` is git-ignored: its animations are licensed): without it a rifle is a plain box, held as a pistol (`pistol or rifle`) |
| `secret` | `secret.number`, a private field; option `secret.numbers` | the example of a private field: off unless `--mod-option secret.numbers=1`; then each player is told a number from 1 to 99 that nobody else is sent |
| `dash` | `dash.charges`, `dash.max`; action `dash` (V); events `dash.started`, `dash.double_jump`; the motion set `dash.moves` | the example of [motions](motions.md): a dash along where you walk and a second jump in the air, both run by every simulation from the player's input, so your own are predicted. The mod only keeps the charges: 2, one back every 2 s (`dash.charges`, `dash.recharge_seconds`). The second jump is off unless `--mod-option dash.double_jump=1` |
| `flight` | `flight.on`, `flight.fuel`; action `fly` (T); events `flight.started`, `flight.stopped`, `flight.thrust`; the motion set `flight.moves` | the example of [motions that hold](motions.md#motions-that-hold-the-flight-mod): T switches flight on and off, Space held in the air is a jetpack with a tank, Shift held while falling glides. All of it is the motion set; the mod only gives the first tank (`flight.fuel`). The jetpack is off unless `--mod-option flight.jetpack=1`. The whole mod is [switched off by default](#switched-off-by-default) |
| `grapple` | action `grapple` (Q); event `grapple.fired`; the motion set `grapple.moves` | the example of a [probe, a force and a link](motions.md#probes-forces-and-links-the-grapple-mod): press Q to throw a hook at what is under the crosshair; it flies there, holds, and pulls you in on a rope you swing on; press Q again to let go; a prop it holds comes to you. All of it is the motion; the mod's C++ declares the three names |
| `deathmatch` | `deathmatch.score` per player; `deathmatch.phase`, `.seconds`, `.round`, `.winner`, `.kill_limit` for the game; `deathmatch.ending`, how the last round ended, a number the look has [the words](looks.md#saying-things) for; events `deathmatch.round_end`, `game.round_start` | rounds: first to 10 kills, or the best score after 300 s; falling costs a point; everyone is frozen for a 6 s intermission, then the world is cleared, everyone respawns and scores reset |

Mods cooperate through the board and through item properties (`pistol` and `melee` tell `inventory` which slot
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
| Sent to | every client, as a command in the frame | its owner only: a reliable message |
| A look reads it | for any entity | for the viewer's own player (`{cards.role}`, `cards.role == 3`); for anyone else it reads 0 |
| Predictions | `changes` apply | `changes` apply |
| State machines | can read it | cannot (the simulation does not have it; the bake and the server say so) |
| Recordings, view files | have it | do not: a recording watched later shows 0 |

- Per player, not per entity: the target of `Set` is a player (`SlotTarget`, or its NetId).
- A slot that is given up is cleared; a reconnecting player gets its values again with the welcome.
- 32 private names per server, counted apart from entity and global fields.
- What happens in the world (where someone is, what they hold) is the simulation's and is not
  secret: every client simulates the whole world, so entities cannot be hidden from one.
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

```bash
cb_server --port 7777 --mod-option deathmatch.kills=5 --mod-option deathmatch.round_seconds=120
```

## Switched off by default

These ship but do not run unless a server asks for them. Nothing was deleted.

| What | Switched off by | Switch it on |
|---|---|---|
| The `sneak` mod (the crouch, C) | the file `server_mods/sneak/disabled`: the mod is compiled in, and `cb_server` leaves it out of its default mods | name it: `cb_server --mods combat,dash,...,sneak`, or delete the file and re-run CMake |
| The double jump (a motion of the `dash` mod) | the server option `dash.double_jump`, 0 by default: the motion's condition reads `dash.double_jump_on`, which the mod publishes | `cb_server --mod-option dash.double_jump=1` |
| The `flight` mod (T flies, Shift glides) | the file `server_mods/flight/disabled`, like `sneak` | name it: `cb_server --mods combat,dash,...,flight`, or delete the file and re-run CMake |
| The jetpack (motions of the `flight` mod) | the server option `flight.jetpack`, 0 by default: the motions' conditions read `flight.jetpack_on`, and the fuel gauge hides without it | run the `flight` mod, with `--mod-option flight.jetpack=1` |

- **A whole mod**: the `disabled` file. `cb_server --list-mods` marks it, `--mods` can still name it, and the tests run every compiled mod whatever the file says.
- **A part of a mod**: a server option the mod publishes as a field, and a condition on it. That is the pattern for any motion a server should be able to switch: the rule is the server's, the motion asks.

## Movement parameters

How players move is a set of values, not constants in the engine (`src/sim/move_params.h`).

| Parameter | Default | Meaning |
|---|---|---|
| `walk_speed`, `sprint_speed` | 3, 6.5 m/s | speed on the ground, without and with sprint |
| `accelerate` | 12 | how fast the speed is reached: per second, times the speed asked for |
| `friction`, `stop_speed` | 6, 1 | how fast a grounded character slows down; below `stop_speed` it stops |
| `air_control` | 0.3 | the share of `accelerate` a character has in the air |
| `gravity`, `jump_speed` | 18 m/s², 6.5 m/s | the fall, and the speed a jump starts with |
| `turn_rate` | 12 rad/s | how fast the body turns toward where it walks (freelook) |
| `max_fall` | 0 | the fastest a character falls, in m/s; 0: no limit |
| `air_friction` | 0 | how fast a character in the air slows down, per second, in every direction; 0: not at all |
| `move_frame` | 0 | 0: the movement input moves along the ground. 1: along the camera, pitch included (flight) |
| `airborne` | 0 | 1: the character is in the air whatever is under it (on a hook): no ground friction, no jump, the in-air animation. The ground still carries it, so it does not sink; it lands when the parameter is gone |
| `mass` | 80 kg | what the character weighs: against the props it walks into (the two trade momentum), under a motion's force in newtons, and on a rope ([Weight](motions.md#weight)). 1 to 10,000 |

| Who sets it | How | Wins over |
|---|---|---|
| The engine | the defaults above | |
| The server | `cb_server --move walk_speed=4` (repeatable) | the engine |
| The character | `movement` on its `CbCharacter`, baked into `anim.cfg` ([Making a character](characters.md#making-a-character)) | the server |
| A mod | `ctx.SetMove( target, MoveParam::WalkSpeed, 1.5f )`, back with `ctx.ResetMove( target, MoveParam::WalkSpeed )` | the character, for that player |

```cpp
// server_mods/sneak/sneak.cpp: a crouch is slow, and does not run.
ctx.SetMove( target, MoveParam::WalkSpeed, speed );
ctx.SetMove( target, MoveParam::SprintSpeed, speed );
...
ctx.ResetMove( target, MoveParam::WalkSpeed );
ctx.ResetMove( target, MoveParam::SprintSpeed );
```

- **The first three are one set**, the server's (`SimConfig::move`): it goes to every client in the welcome and into recordings, and the server prints what differs from the engine's when it starts (`movement: walk_speed = 4`).
- **A mod's value is the player's own**: simulation state, hashed and rolled back, so every client moves that player the same way. A respawn keeps it; only the mod gives it back.
- **Values are clamped** to the parameter's range (`MoveParamInfoOf`); what is not a number is ignored.
- **Reading**: `ctx.Move( slot, MoveParam::WalkSpeed )` is what the player moves with now.
- **Not predicted**: `SetMove` is a command, so the change reaches the player's own simulation with the frame. That suits a change that lasts (a crouch, a slow, a heavy item). Movement that must answer a press at once is a [motion](motions.md), which can also set parameters for as long as it lasts.
- **Not a parameter**: the capsule's size (hitboxes, the camera and the hover all go by it).
- The `sneak` mod's speed is the server option `sneak.speed` (1.5 m/s; 0 leaves the speeds alone). The mod is [switched off by default](#switched-off-by-default).

## Hit zones

Shooting at the crosshair is `ctx.CastAim( slot, range, hit, origin, direction )`, two rays:

| Step | Ray | Why |
|---|---|---|
| What is aimed at | from where the player's line of sight starts (`ViewPosition`), along the camera's direction | the camera's choice: the character's eye height above its feet in first person, the pivot above the body behind it, 0.45 m to a side over a shoulder (`PlayerInput::view`) |
| What is hit | from the eye on the posed head (`HeadPosition`) to that point | the body's: whatever is between the head and the target stops the shot, even where the camera sees past it |

The client sends only which view it uses and where it looks; both origins are computed by the
server from the simulation, so a shot cannot start anywhere the player is not.

Gameplay mods cast rays with `ctx.CastRay`. Players are hit by their character's hitboxes, posed
from the simulation's animation state at that tick, and `hit.zone` names the zone. Only the server
does this (mods run there), so hit tests cost clients nothing and never enter the rolled-back
simulation. The pistol multiplies its damage by `pistol.zone.<zone>`.

## Workshop items

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

## The SDK

A mod's look is made in a Godot project of its own, made from the **SDK project** (`sdk/`):

```sh
cmake --build --preset godot-export
powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -Setup         # fill sdk\ itself
powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -New mymod     # server_mods\mymod\client
```

| It gives the project | For |
|---|---|
| the extension | the `Cb*` nodes, the bakers, the Cue Preview panel |
| `placeholder/` | a placeholder skeleton (no model: the editor draws its bones) and its locomotion clips, to author animations and state machines on |
| starter scenes named after the mod | an item, its reactions, a HUD, and an animation pack (`animation_packs/`): the default tree in full, replacing the upper body |
| a "Mod" export preset | ships every resource but the SDK's own: nothing to list |

None of the SDK's parts are packed into the mod. [sdk/README.md](../sdk/README.md) has the steps, what
is shipped, and what the SDK does not do yet. The rifle's (local) project is one.
