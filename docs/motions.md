# Motions

What a mod adds to how players move (a dash, a double jump, flight, a jetpack, a grappling hook), predicted like walking is. Part of the [manual](../README.md#the-manual).

## Why they exist

| | A mod's command (`Push`, `SetMove`) | A motion |
|---|---|---|
| Decided by | the server mod's C++, each tick | data every simulation runs: the server's and each client's |
| Reaches your own screen | a round trip after the press | on the tick of the press |
| A wrong guess | does not happen: nothing is guessed | rolled back, like a mispredicted step |
| Good for | what lasts and need not be instant (a crouch's speed, knockback from a hit) | what must answer a key at once (a dash, a second jump) |

A motion is to movement what a [`CbReaction`](looks.md#reactions) is to the look: *on a press, on a
cue, or while conditions hold, do this to the mover*. It is authored in Godot, baked to text, sent in the schema
and run by the simulation, the way a character's [state machine](characters.md#state-machines) is.

```
CbMotion nodes ──bake──> motions/<set>.cfg ──> the server reads it from the mod's item
                                                     │ the schema (welcome, recordings)
   your press ──> your simulation runs the motion ───┤
                  the server runs the same motion  ──┘   same input, same state, same result
```

## The two halves

| Half | Where | What |
|---|---|---|
| The rules | `server_mods/<mod>/<mod>.cpp` | declares the action, the fields and the events the motions name, names the set (`declare.Motions( "dash.moves" )`), and decides who may: a field a condition reads |
| The motions | `server_mods/<mod>/client/motion_sets/<name>.tscn` | a `CbMotionSet` with `CbMotion` children; saving the scene bakes `motions/<set_name>.cfg` |

```cpp
// server_mods/dash/dash.cpp: the names, and how many dashes a player has. Not the dash.
m_dash = declare.Action( "dash", "V" );
m_charges = declare.Field( "dash.charges", BoardType::Int );
m_started = declare.Event( "dash.started" );
m_moves = declare.Motions( "dash.moves" );
...
ctx.Set( SlotTarget( slot ), m_charges, charges + 1 );   // one back every 2 s
```

```
Moves        CbMotionSet   set_name "dash.moves"                (motion_sets/dash_moves.tscn)
├── Dash        CbMotion   action "dash"   conditions dash.charges > 0   cooldown 0.4
│                          impulse 11 m/s along the move input, replacing the horizontal velocity
│                          duration 0.18 s with parameters { friction: 0 }
│                          changes dash.charges -= 1      emits dash.started
└── DoubleJump  CbMotion   action "jump"   conditions not grounded   uses 1, back on the ground
                           impulse 6.5 m/s up, replacing the vertical speed    emits dash.double_jump
```

## `CbMotion`

| Group | Field | Meaning |
|---|---|---|
| When | `when` | **On a press**: once, when `action` goes down (a dash). **While**: every tick its `conditions` hold (flight, a glide, a jetpack). **On a cue**: once, when the mod event `event` is recorded at the player (a stun a server mod starts) |
| | `action` | On a press: one a server mod declares (`dash`), or the engine's `jump` / `sprint`. Held, it is one press |
| | `event` | On a cue: the event. A server mod emits it at the player, so the start is the server's and comes with the frame; from then on every simulation runs the motion |
| | `conditions` | [expressions](looks.md#expressions) that must all hold on the player before this tick's movement. They read what a state machine reads (`grounded`, `airborne_time`, `speed`, `vertical_speed`, a field, an item kind held, a stance), whether a key is down (`held.jump`, `held.dash`) or went down this tick (`pressed.dash`), and `tethered` (a tether of the player's is out) |
| | `cooldown` | seconds between two uses; for a While, between its end and its next start |
| Uses | `uses` | how many before a refill; 0: no limit. Not for a While |
| | `refill`, `refill_seconds` | **On the ground**: back when the player stands. **After seconds**: back that long after the last use |
| Impulse | `impulse` | the change of velocity, m/s. A While adds that much every second it is on: a thrust |
| | `impulse_frame` | **Look** (the camera, pitch included), **Move input** (WASD; the facing when none is held), **Facing**, **Up**, **World direction** (`impulse_direction`) |
| | `replace` | what of the velocity is cleared first: **Nothing**, **Vertical speed**, **Horizontal velocity**, **All**. Not for a While |
| While it lasts | `duration` | how long the motion stays on after a press or a cue. A While is on while its conditions hold |
| | `parameters` | [movement parameters](server-mods.md#movement-parameters) the player has while it is on (`friction` = 0). They win over the server's, the character's and a mod's `SetMove` for that long |
| When it happens | `changes` | fields of the player: `dash.charges -= 1` (`-=`, `+=`, `=` with a number), written like a [prediction's](looks.md#predictions). In a While, `-=` and `+=` are per second and need a Float field (`flight.fuel -= 30`), and `=` is set when it starts |
| | `emits` | a mod event at the player, with the impulse as its vector. A While emits it when it starts |

- **Order**: a tick runs the frame's commands, then each player's motions (in the schema's order), then the mover. So a `Set` from the mod and a `change` from a motion in the same tick both count.
- **Conditions read the fields as the tick found them**: a motion that changes a field does not start another until the next tick. That is what makes a toggle of two motions (`FlyOn` if not `flight.on`, `FlyOff` if `flight.on`) one switch a press.
- **A frozen or dead player** does none; a key held through a freeze is not a press when it ends.
- **At most 16 motions** on a server, all its mods' sets together.
- **In the editor**: every group has an info row (click the icon), every property a hover text, F1 opens the class reference. A motion that cannot run is a warning on the node and stops the bake.
- **Names the editor cannot check**: the action, fields and events are the server mod's. One no mod declares makes that part do nothing (the motion never happens, the change is skipped, nothing is emitted), and the server says so when it starts.

## Tethers: the grapple mod

A tether is a line a motion throws at what the player looks at. It flies there, takes hold, and
pulls; with a rope it also keeps the player within the rope's length, so the player swings.
`server_mods/grapple` is one motion and a look. Its C++ declares three names.

```
Moves   CbMotionSet   set_name "grapple.moves"               (motion_sets/grapple_moves.tscn)
└── Hook   on "grapple" (Q)   conditions not tethered
           tether: range 40 m, flies at 60 m/s, a rope, pull 24, reel 4, until pressed.grapple
           parameters { friction: 0, air_control: 0.6 }       emits grapple.fired
```

| Field | Meaning |
|---|---|
| `tether_range` | how far the line reaches, in metres; 0: the motion throws none. With a tether the motion **happens only if the line finds something** |
| `tether_travel` | the speed it flies at: it takes hold after distance / speed. 0: at once |
| `tether_rope` | the distance when it takes hold is a rope's length: the player cannot go further out. Off: it only pulls |
| `tether_pull` | acceleration toward the point while it holds, m/s per second |
| `tether_reel` | metres of rope taken in a second (never shorter than a metre) |
| `tether_until` | expressions, any of which lets it go. Without one it holds until what it holds on to is gone |

| Step | What happens |
|---|---|
| The throw | two traces, like a shot: what is under the crosshair (along the camera's line from the point it orbits, or a shoulder), then from the player to that point, so something in between stops it. Nothing within range: nothing happens, not even the cooldown |
| What it finds | the world: a point. A prop: a point on that body, which moves with it. A player: a point on its capsule (not a limb: hitboxes are the server's) |
| Flying | until `distance / tether_travel` has passed, nothing pulls; the look draws the line growing |
| Holding | the pull, the reel and the rope, every tick, before the mover. A prop it holds on to is pulled the other way with the player's weight |
| While it is out | the motion is on: its `parameters` hold (`friction` = 0, or the ground rubs the pull off) |
| Letting go | `tether_until`; what it held on to being destroyed; the player dying, or being put somewhere else (a respawn) |

- **Hold or toggle** is two lines of the motion:

  | | `conditions` | `tether_until` |
  |---|---|---|
  | Press to throw, press again to let go (the grapple mod) | `not tethered` | `pressed.grapple` |
  | Hold to grapple | | `not held.grapple` |

  `not tethered` keeps the second press from throwing a second line; `pressed.grapple` is that press letting the first go. A hook that ends by itself (its prop destroyed, a death) leaves nothing behind: the next press throws.
- **One tether per player**: a second throw replaces the first.
- **The event** a tether motion emits is at the point where it will hold (a puff there), and its vector is the motion's impulse.
- **It is state** (`Tether`, a component a player has once it threw one): hashed, rolled back, in snapshots.

### The rope: `CbTetherLook`

```
GrappleReactions            (vfx/reactions_grapple.tscn)
├── Rope    CbTetherLook    motion "grapple.moves/Hook"   scene vfx/grapple_rope.tscn   from "RightHand"
├── Fired   CbReaction      on grapple.fired, subject $at: the throw's sound at the hand
└── Hit     CbReaction      on grapple.fired, subject $at: a puff at the cue's point
```

| Field | Meaning |
|---|---|
| `motion` | whose tethers it draws: the set and the node (`grapple.moves/Hook`). Empty: any tether without a look of its own |
| `scene` | a scene one metre long along its -Z; the game stretches it from the player to the tether's end, while it flies and while it holds |
| `from` | the player's socket it starts at (`RightHand`); empty, or a socket the character lacks: its chest |

In conditions (a reaction, a HUD node), `tethered` is true for a player whose tether is out, and
`tether_holds` once it has taken hold.

## A motion a server can switch off

A motion has no switch of its own: who may use one is a field its conditions read. For a part
of a mod that a server should be able to turn off, the mod publishes a server option as a field
and the motion asks for it:

```cpp
m_doubleJumpOn = declare.Field( "dash.double_jump_on", BoardType::Bool, BoardScope::Global );
...
ctx.Set( 0, m_doubleJumpOn, ctx.Option( "dash.double_jump", 0.0 ) != 0.0 ? 1 : 0 );
```

```
DoubleJump  CbMotion   action "jump"   conditions not grounded, dash.double_jump_on
```

The double jump and the jetpack ship this way, off by default
([Switched off by default](server-mods.md#switched-off-by-default)).

## Motions that hold: the flight mod

`server_mods/flight` is all three kinds of holding motion. The mod is
[switched off by default](server-mods.md#switched-off-by-default): start the server with it by name to try it. Its C++ declares the names and gives a
player its first tank; everything else is the set.

```
Moves     CbMotionSet   set_name "flight.moves"             (motion_sets/flight_moves.tscn)
├── FlyOn    on "fly" (T)   conditions not flight.on    changes flight.on = 1    emits flight.started
├── FlyOff   on "fly" (T)   conditions flight.on        changes flight.on = 0    emits flight.stopped
├── Flying   while   flight.on
│                    parameters { gravity: 0, move_frame: 1, air_control: 1, air_friction: 3, walk_speed: 7, sprint_speed: 12 }
├── Thrust   while   held.jump, not grounded, not flight.on, flight.fuel > 0
│                    impulse 26 m/s per second up     changes flight.fuel -= 30     emits flight.thrust
├── Refuel   while   grounded, flight.fuel < 100      changes flight.fuel += 40
└── Glide    while   held.sprint, not grounded, vertical_speed < 0, not flight.on
                     parameters { gravity: 3, max_fall: 2.5, air_control: 0.8 }
```

| Key | What happens |
|---|---|
| T | flight on and off. On: no gravity, WASD moves along the camera (look up and walk to rise), letting go stops |
| Space, held in the air | the jetpack: up, for as long as the tank lasts (a little over 3 s); it fills again on the ground. Off unless the server runs with `--mod-option flight.jetpack=1` |
| Shift, held while falling | a glide: the fall slows to 2.5 m/s |

- **`move_frame`** and **`air_friction`** are [movement parameters](server-mods.md#movement-parameters) made for this: the first turns the movement input toward where the camera looks, the second is what stops a body in the air.
- **The gauge is predicted**: the fuel is a field the motions count themselves, so the HUD's bar (`CbFieldBinding` on `flight.fuel`) moves as you thrust.
- **A While says so every tick**: between two ticks nothing of it is on, so `ctx.Move` in a server mod reads the parameters without it.

## The look of a motion

The event a motion emits is recorded in the simulation that ran it, the viewer's own included. So:

| | A server mod's event | A motion's event |
|---|---|---|
| Your own shows | when a [`CbPrediction`](looks.md#predictions) guesses it | at once, with no prediction node |
| A wrong guess | the reaction stays played | the rollback un-counts the event |

```
DashReactions                 (vfx/reactions_dash.tscn)
├── Dashed        on dash.started       subject $at   a puff at its feet + a whoosh
└── DoubleJumped  on dash.double_jump   subject $at   the jump's puff + its sound, higher
```

Fields a motion changes are state the viewer already has: the dash mod's HUD is a `CbFieldLabel`
with `"[{key:dash}]  DASH {dash.charges} / {dash.max}"`, and the count drops on the press.

## Baked file

`motions/<set_name>.cfg`, tab-separated, written on save and when the mod is published. Never edited.

```
cinderbox_motions	1
motion	Dash
when	press	dash
if	( dash.charges > 0 )
cooldown	0.4
duration	0.18
impulse	11	move	horizontal
param	friction	0
change	dash.charges	-=	1
emit	dash.started
```

| Step | Who |
|---|---|
| Bake | saving the scene, the **Bake motions** button, `tools\publish_mod.ps1` (`bake_motions.gd`) |
| Ship | the mod's item: `motions/*.cfg` is packed, `motion_sets/` is not |
| Read | the server, from the item (its SHA-256 checked); a set it cannot read does nothing |
| Send | in the schema: the welcome, and a recording's header |
| Compile | every simulation, against the schema's names (`CompileMotions`) |

## What is measured

`net_dash` runs the dash mod behind 50 ms each way (a 100 ms round trip):

| | |
|---|---|
| The server | has the dash on the tick of the press: 3 m/s before, 11 m/s after, a charge taken |
| The client | predicted every tick of the two dashes and the double jump exactly as the server then had it: 390 ticks compared, none different |
| The recording | replays to the same checksums, motions included |

`net_flight` does the same for the flight mod: flight switched on, a flight into the sandbox's wall, off and down, the jetpack
held until its tank is empty, a glide, the landing and the refuel. 1,710 predicted ticks compared, none different: the
position, the fuel's exact bits, the events.

`net_grapple` does it for the hook: two throws at walls, the flight of the line, the pull along the rope and the letting
go. 501 predicted ticks compared, none different (the position and the line's end). Then two players put their hooks in
one ball: both hold on to it, it goes 16 m, and both clients have the server's state for every confirmed tick.

## Not yet

| Missing | Roadmap |
|---|---|
| A tether pulls a prop but not another player (the line follows a player it holds on to); in first person the line's start is the same point as in third person, not the eye | later |
| A preview panel; `motion.<name>` in conditions and state machines; names checked at publish | Motions in the editor |
| Another player's dash is seen when its input arrives: its press is guessed by repeating its last input, as a jump is | by design |
