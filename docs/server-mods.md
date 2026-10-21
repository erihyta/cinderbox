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
| `Emit` | announces a mod event (`"item.use"`): two entities, a value, a point, a vector |
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
| `inventory` | `declare.Slots( columns x rows )` (8 x 4 by default: the options `inventory.columns`, `inventory.rows`, read with `declare.Option` while declaring); the field `inventory.columns` | the rules of [the inventory](items.md#the-inventory): a grid whose first row is the hotbar (the slots themselves, and selecting, moving and dropping, are the engine's); what a life starts with (`inventory.start`) and what a death takes back and drops. Its look: the hotbar, and the inventory screen on I (drag and drop, icons) |
| `melee` | layer `full`, stances `melee`, `melee_swing`; events `melee.swing`, `melee.hit`, `combat.damage`, and the clip's markers `melee.strike`, `melee.done` | the bat: it brings the `melee.hold` animation pack (`ItemLayers`), an upper body of its own (ready, and the swing on the `melee_swing` stance), over the character's own legs; in first person the same swing is seen from the eye (the item's `view_offset` and `view_rotation` bring the hip-high hold into the picture; one arm drawn); left mouse swings, for as long as the clip says (to its `melee.done` marker, 1.1 s; the next swing may start then), a fan of 1.8 m rays from the chest at the strike, spread sideways and pitched as far up or down as the player looks (look at the floor to hit what is low), 40 damage through `combat.damage` |
| `props` | action `spawn_prop` (F) | F with empty hands throws a prop (the map's spawnable template, or a random box or sphere) |
| `item` | events `item.use`, `item.use_failed`, `item.use2`, `item.hit`, `item.reload`; actions `use2` (right mouse), `reload` (R) | what using an item is called, whatever the item: the cues an item's own scene answers, which do not name it. For an item no mod rules it says `item.use` and `item.use2` on the press, so an item with no rules still has its look; a mod that rules a kind marks it `item.ruled` and says the cues when its rule does |
| `gun` | `gun.ammo`, `gun.magazine`, `gun.reloading` (of the gun in the hand); stance `gun` on `upper`; the item mod's cues; `combat.damage` | the rules of anything that shoots. **Which guns there are is not in the code**: every item of its look whose scene says `gun.damage` is one (`declare.ItemsOfLook()`), with `gun.magazine`, `gun.fire_seconds`, `gun.automatic` (held fire), `gun.reload_seconds`, `gun.range`, `gun.kill_push`, `gun.prop_push`; its animation pack, holster and slot are the scene's too. A magazine is the gun's own: its rounds are the gun's field `gun.rounds`, which its scene declares with the full count (`CbItem.fields`). Shipped: the pistol (25 damage, 12 rounds, a press a shot). The rifle (14, 30 rounds, held) is a second scene whose files are local (licensed animations) |
| `mark` | `mark.found`; the item mod's `use2` and `item.use2` | a second use for an item that says `mark.range` (the pistol): a ray that harms nothing and names the player it finds. The example of a use added to items made in the editor |
| `secret` | `secret.number`, a private field; option `secret.numbers` | the example of a private field: off unless `--mod-option secret.numbers=1`; then each player is told a number from 1 to 99 that nobody else is sent |
| `dash` | `dash.charges`, `dash.max`; events `dash.started`, `dash.double_jump`; the item `dash.boots` and the motion set `dash.moves` | the example of [motions](motions.md), and of [a motion an item brings](motions.md#what-an-item-does): the dash is the boots' (every life starts with a pair, with the inventory mod), the double jump everyone's. a dash along where you walk and a second jump in the air, both run by every simulation from the player's input, so your own are predicted. The mod only keeps the charges: 2, one back every 2 s (`dash.charges`, `dash.recharge_seconds`). The second jump is off unless `--mod-option dash.double_jump=1` |
| `cannon` (off) | the items `cannon.cannon` (key N) and `cannon.ball` from its look; events `cannon.fired`, `cannon.ball.smash`; option `cannon.damage` | walls broken by what hits them, with no blast. The cannon launches a 40 kg ball (0.75 m across) at 36 m/s (its scene: a `CbLaunch` under the item); what a ball does to a wall is its scene's too (its CbItem's **Breaks walls**: 12 m/s, a 1.2 m hole at 30 m/s, on through with 0.6 of its speed), done by the simulation, so it is predicted; the mod's C++ only hurts a player a ball hits (`combat.damage`, by speed) |
| `flight` | `flight.on`, `flight.fuel`; action `fly` (T); events `flight.started`, `flight.stopped`, `flight.thrust`; the motion set `flight.moves` | the example of [motions that hold](motions.md#motions-that-hold-the-flight-mod): T switches flight on and off, Space held in the air is a jetpack with a tank, Shift held while falling glides. All of it is the motion set; the mod only gives the first tank (`flight.fuel`). The jetpack is off unless `--mod-option flight.jetpack=1`. The whole mod is [switched off by default](#switched-off-by-default) |
| `grapple` | events `grapple.hook.used`, `grapple.fired`, `grapple.hooked`; `declare.Slots( 4 )`; the item `grapple.hook` from its look | the example of a probe, a force and a link; it holds anything but moves only its own player (its rope and pull do not react): all of it in the hook's scene and sheet (`client/prefabs/hook.tscn` and `hook.rules`: the item, its `Throw` motion, a `cast` with a rope and a pull); used by its slot's key, where it is. The mod declares the two events |
| `brawl` | five skills from its look (`brawl.fist`, `.dash_strike`, `.uppercut`, `.slam`, `.guard`); fields `brawl.combo`, `brawl.combo_left`, `brawl.cd_*`; hit events; the set `brawl.moves` | a brawler's kit, step 5's measure: what a skill does to bodies is its scene's (predicted); this mod turns the probes' hit events into `combat.damage` (a quarter on a guard) and hurts whom a slam's shockwave finds (`brawl.slam.hit`: the slam's sheet waits for the ground, finds who is in its `Shockwave` region and throws them, on every machine). `--mods inventory,combat,brawl` |
| `shop` | intent `shop.buy`; `shop.coins`, a private field; events `shop.bought`, `shop.refused`; option `shop.start` | the example of an [intent a mod declares](#intents-a-mod-declares): a screen on B with three buttons (a frag grenade 1 coin, a bat 1, a rifle 2); a player starts with `shop.start` coins (3) and gets one per kill |
| `chat` | the text channel `chat`; options `chat.max_length`, `chat.burst`, `chat.burst_seconds` | the example of [typed text](#typed-text): what a player types (T, Enter) is said to everyone with its name; a line is cut to 120 characters, and more than 4 lines in 8 s gets only the sender a "slow down". Off: start the server without it, or a `disabled` file |
| `chest` | the template `chest` (`declare.ContainerTemplate`: a static box with `chest.slots` slots, its look `prefabs/chest.tscn`); options `chest.count`, `chest.radius`, `chest.slots`, `chest.items` | the example of a [container](items.md#containers-chests): places chests on the ground around the spawn point (`ctx.FindTemplate`, `ctx.SpawnTemplate`, `ctx.CastRay` for the ground) and fills each (`ctx.Containers`, `ctx.PutInContainer`) |
| `frag` | the item `frag.grenade` from its look (slot 6, key H); events `frag.release`, `frag.thrown`, `frag.grenade.expired`, `frag.blast`; `declare.Slots( 6 )`; options `frag.damage`, `frag.radius`, `frag.push`, `frag.refill_seconds`, `frag.carry`, `frag.break_radius` (the hole: a wall's pieces whose centre is this close go; default half of `frag.radius`; 0: nothing breaks), `frag.rubble_seconds` (pieces go after this long; 0 keeps them); the global fields `frag.radius`, `frag.push`, `frag.break_radius`, `frag.rubble_seconds` (those options, for its sheet) | the fused grenade, the example of [an item's time being up](motions.md#things-that-fly-the-grenades), of [an item's own motion](rules-language.md#a-frag-grenade-whose-blast-is-predicted) and of a [stack](items.md#stacks): three to a slot, thrown one at a time from the hand; it bounces, and when its time is up its sheet sets it off on every machine (the blast, the hole, what is loose thrown); the mod hurts who is near and gives them back one at a time while a player has fewer than three |
| `grenade` (off) | the item `grenade.shell` from its look (slot 7); events `grenade.release`, `grenade.thrown`, `grenade.blast`; `declare.Slots( 7 )`; option `grenade.refill_seconds` | the impact grenade, switched off (its `disabled` file; start it by name): thrown from the hand, it goes off on the first hard hit (`ctx.Hits`) |
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
- As many private names as the mods declare, counted apart from entity and global fields.
- What happens in the world (where someone is, what they hold) is the simulation's and is not
  secret: every client simulates the whole world, so entities cannot be hidden from one.
- The `secret` mod is the example, and `net_private_fields` the check.

### Intents a mod declares

A screen's button can ask something of a mod ("buy offer 2", "vote for map 3") with no client
code: the mod declares the intent, a `CbClick` sends it, the mod answers.

```cpp
m_buy = declare.Intent( "shop.buy" );
...
for ( const IntentSent& asked : ctx.Intents( m_buy ) )   // who, and its numbers a and b
{
	ctx.GiveItem( SlotTarget( asked.slot ), ... );
}
```

| | What |
|---|---|
| Sent | in the player's own input: the code `kFirstModIntent` (32) + the intent's place in the schema, with two numbers (0 to 255) |
| Heard | `ctx.Intents( handle )`: each player who sent it, once, on the tick it arrives (a late or guessed input does not repeat it) |
| Answered | by the mod, on the server; not predicted: the screen shows the answer when the server's state arrives |
| A screen | `CbClick` `mod_intent = "shop.buy"`, `intent_a = "2"` (an expression: `slot.number` in a row); GDScript: `client.send_mod_intent( "shop.buy", 2, 0 )` |

- As many as 223 intents across the mods; the same name from two mods is one intent.
- The `shop` mod is the example, and `net_shop` the check.

### Typed text

Words a player types (chat, a team name, a vote) are text nobody authored, so they cannot be in a
look. The engine only carries them; what is said to whom is a mod's.

```cpp
declare.TextChannel( "chat" );
...
for ( const TypedText& typed : ctx.Typed() )            // who, which channel, the words
{
	ctx.Say( "chat", typed.text, typed.slot );           // to everyone, from that player
}
ctx.Say( "chat", "Slow down.", kNoSlot, typed.slot );    // the server's own words, to one player
```

| | What |
|---|---|
| Sent | a reliable message (`Text`), not the input: at most 200 bytes, printable, cut on a character |
| Heard | `ctx.Typed()`: everything typed since the last tick, into a channel some mod declared; at most 4 a tick from one player. Text for a channel nobody declared goes nowhere |
| Said | `ctx.Say( channel, text, from, to )`: from a player or from nobody (`kNoSlot`), to everyone or one player |
| Shown | the mod's look: a [`CbTextLog` and a `CbTextEntry`](looks.md#hud-nodes). No such mod, no box |
| Not | in the simulation, its hash, recordings or view files |

- The `chat` mod is the example, and `net_chat` the check.

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

## Breaking a wall

A map's [breakable walls](maps.md#walls-that-break) break only when a mod says so.

| Call | Does |
|---|---|
| `ctx.BreakablesNear( point, radius )` | the walls (whole, or what stands of them) with a piece's centre within `radius` of `point`, nearest first: one each, to give `Break` |
| `ctx.Break( wall, blast, speed, seconds, radius )` | a hole: the wall's pieces within `radius` of `blast` (0: all of them) fly away from it (and a little up) at `speed / (1 + their distance)`; the rest stand, static, and any that nothing holds up any more falls. `seconds` > 0: what goes is removed after that long |

| `ctx.IsBreakable( netId )` | whether it is a wall that breaks (whole, or a piece of one left standing): for a mod that hears what something hit |

- **By a blast**: the frag grenade blows a hole `frag.break_radius` across (2.5 m by default) in
  every wall near where it goes off, at `frag.push`: its sheet's `break` (every machine, the tick
  its time is up), the numbers the mod's options.
- **By what hits it**: an item whose scene says it breaks walls ([its CbItem's **Breaks
  walls**](items.md): the cannon ball's) knocks a hole where it meets one, in the simulation's own
  step. So the player who fired it sees the hole on the tick the ball arrives, as the server does.
  A mod could do the same from `ctx.Hits` (what its items ran into) and `ctx.Break`, but that runs on
  the server only: the player would see the ball bounce off whole for as long as its lead (a few
  ticks even at no ping), then the correction. A wall is still never broken by a bump: only by what
  a mod's item says breaks it.

## A push that turns

`ctx.Push( target, point, vector, mode )` acts at `point`. Through a body's centre it only moves it;
off the centre it turns it too, as a real hit would. No marker in the body is needed: the centre
is the body's own (`EntityTransform`, or a `NearBody`'s `position`), and the mod picks the point.

| To | Give as `point` |
|---|---|
| Move it without turning it | its centre |
| Make it tumble away from a blast | its centre, moved a share of its size (`NearBody::radius`) toward the blast and down |
| Spin it where a shot hit | the hit point (the pistol does) |
| Throw a fallen body, all of it | any point, with the mode `ImpulseThrow`: every part of the ragdoll gets the change of speed. (The other modes hit the part nearest the point, as a shot does: the rest only follows by its joints) |

- A small share for small things: the same offset turns a light, small body much faster.
- Players are not bodies the solver turns: a push on a living player only changes its velocity.
- **A body that a hit kills does not exist yet when the hit is dealt**: the combat mod hears of the damage on the next tick, and the ragdoll is made then. A mod that wants to throw it remembers the blast for a few ticks and throws the bodies that fall inside it (`NearBody::ragdollSince`), as the grenade does.

## How much a server may have

A mod's own code may do as much work as the host's machine allows: it runs on the server only,
and nothing measures it. What its mods *declare* is state that every client keeps, hashes and
rolls back, so that is what has a size, and the size is the mods' to choose.

| What | How many | What one costs |
|---|---|---|
| Fields (an entity's, the game's, a player's private ones) | as many as the mods declare, per scope | 4 bytes on every entity that has any field set; the game's and private ones, 4 bytes once |
| Motions | as many as the mods' sets have | 28 bytes on every player |
| Animation layers | as many as the character's state machine and the mods name | 40 bytes on every player, and a state machine sampled and blended per player per tick |
| Actions (keys of the mods' own) | 64 | a bit in every input; a packet carries only the bytes in use |
| Events | 65,535 names | nothing until one happens |
| Commands a tick | 1,024 | they travel in the frame everyone gets |
| Slots a player | 36 | |
| Item kinds, stances, sockets, animation packs, motion sets | about 255 each | an id of one byte |

- The server says what its mods asked for when it starts: `state the mods asked for: 41 fields an entity (164 bytes for one that has any), ...`.
- The numbers are part of what a session is (`SimConfig::fields`, `globalFields`, `motions`, `layers`): they go out in the welcome and in a replay's header, and every simulation sizes its state by them.
- What is left with a number is what a packet or a file writes it in (a field's number is 16 bits, an item kind's 8), and actions: an input is a record of one size, copied for every player every tick.

## Switched off by default

These ship but do not run unless a server asks for them. Nothing was deleted.

| What | Switched off by | Switch it on |
|---|---|---|
| The `sneak` mod (the crouch, C) | the file `server_mods/sneak/disabled`: the mod is compiled in, and `cb_server` leaves it out of its default mods | name it: `cb_server --mods combat,dash,...,sneak`, or delete the file and re-run CMake |
| The double jump (a motion of the `dash` mod) | the server option `dash.double_jump`, 0 by default: the motion's condition reads `dash.double_jump_on`, which the mod publishes | `cb_server --mod-option dash.double_jump=1` |
| The `flight` mod (T flies, Shift glides) | the file `server_mods/flight/disabled`, like `sneak` | name it: `cb_server --mods combat,dash,...,flight`, or delete the file and re-run CMake |
| The jetpack (motions of the `flight` mod) | the server option `flight.jetpack`, 0 by default: the motions' conditions read `flight.jetpack_on`, and the fuel gauge hides without it | run the `flight` mod, with `--mod-option flight.jetpack=1` |

- **A whole mod**: the `disabled` file. `cb_server --list-mods` marks it, and `--mods` can still name it. Net tests run it only when they name it (`test.mods`); they leave out the `brawl` kit too, which runs by default but takes the hotbar's first slots.
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
| `max_slope` | 50 | the steepest ground that holds the character up, in degrees: on steeper ground (a ramp, the side of a ball) it slides down and cannot walk or jump up it. 90: any (as before M190). Unity starts at 45, Unreal at 44.8 |

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
mods needs (a name and a SHA-256), and players get that exact item from the workshop: subscribed
beforehand, or fetched when they connect.

| Piece | Where |
|---|---|
| The item's source | `server_mods/<mod>/client/`, a Godot project next to the mod's code, with a "Mod" export preset |
| Publishing | `tools\publish_mod.ps1 -Mod <mod>`: packs it, names it by its SHA-256, installs it into the local workshop, writes `server_mods/<mod>/client_item.cfg` (commit it) |
| Subscribed items | kept: `user://workshop/<mod>/<sha256>.zip` (`%APPDATA%\Godot\app_userdata\Cinderbox\workshop`, or `--workshop=DIR`) |
| Cached items | fetched on connect: `user://cache/<mod>/<sha256>.zip` (or `--cache=DIR`); at start, what was not used for 30 days goes, then the least recently used past 2 GB |
| The workshop store | where missing items come from: for now a folder of the same layout (`--store=DIR`, or Settings > Workshop store). `godot/workshop.gd` is the one place Steam Workshop would plug in |
| What the server announces | the hash from `bin/items/<mod>.item` (the build copies `client_item.cfg` there; `--items DIR` to use another folder) |

Joining a server from the menu:
1. **Asked first** (an `InfoRequest` on the game port: the asker never becomes a player). The server
   answers with its map, its players and the items it needs.
2. **The server panel** lists each item: Subscribed, In the cache, Downloads on connect (its size),
   or Not available (not in the store: Connect is off). Subscribe on a row keeps an item.
3. **Connect** (bottom right) fetches what is missing from the store into the cache, checks every
   file against its hash, then joins.
4. Items load after the base game and **before your own mods**, which load again last, so you can
   still restyle what an item ships. Their `ui/hud_<mod>.tscn` is laid over the game's HUD.

Joining without the menu (`--host`, a link later) fetches from the store the same way; an item the
store has not got ends the attempt, naming it.

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
