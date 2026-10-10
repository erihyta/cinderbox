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
cue, or while conditions hold, do this to the player (and to what it reaches)*. It is authored in Godot, baked to text, sent in the schema
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
| The motions | `server_mods/<mod>/client/motion_sets/<name>.tscn` | a `CbMotionSet` with `CbMotion` children, each with its parts under it; saving the scene bakes `motions/<set_name>.cfg` |

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
│   │                      duration 0.18 s with parameters { friction: 0 }
│   │                      changes dash.charges -= 1      emits dash.started
│   └── Push    CbImpulse  11 m/s along the move input, replacing the horizontal velocity
└── DoubleJump  CbMotion   action "jump"   conditions not grounded   uses 1, back on the ground
    │                      emits dash.double_jump
    └── Up      CbImpulse  6.5 m/s up, replacing the vertical speed
```

## The nodes

A motion is a trigger with parts under it. The trigger says *when* and *for how long*; the parts say
*what happens to whom*.

```
CbMotionPart            what they share: each bakes to lines, and warns in the tree when it cannot
├── CbMotionSet         the root: a mod's motions, one baked file
├── CbMotion            the trigger: when, how long, parameters, changes, the event
├── CbProbe             a line along the look that has to find something
├── CbLaunch            an item of a kind thrown into the world, once
└── CbMotionEffect      what it does to a body: on whom (target), in which direction (frame)
    ├── CbImpulse       a change of velocity, once
    ├── CbForce         a push for as long as the motion is on
    └── CbLink          a rope to the target
```

| To make | Under the `CbMotion` |
|---|---|
| A dash, a double jump, a shove | a `CbImpulse` |
| A jetpack, a wind, a conveyor, a brake | a `CbForce` (the motion a While, or with a `duration`) |
| A grappling hook | a `CbProbe`, a `CbForce` toward what it found, a `CbLink` |
| A grenade, a thrown ball, a rocket | a `CbLaunch` ([Things that fly](#things-that-fly-the-grenade-mod)) |
| A tractor beam on a prop | a `CbProbe`, a `CbForce` whose target is what it found |
| Two players bound together | a `CbLink` whose target is a field the server mod writes the partner into |
| Flight, a glide, a slide | nothing: `parameters` on the motion are enough |

## `CbMotion`: the trigger

| Group | Field | Meaning |
|---|---|---|
| When | `when` | **On a press**: once, when `action` goes down (a dash). **While**: every tick its `conditions` hold (flight, a glide, a jetpack). **On a cue**: once, when the mod event `event` is recorded at the player (a stun a server mod starts) |
| | `action` | On a press: one a server mod declares (`dash`), or the engine's `jump` / `sprint`. Held, it is one press |
| | `event` | On a cue: the event. A server mod emits it at the player, so the start is the server's and comes with the frame; from then on every simulation runs the motion |
| | `conditions` | [expressions](looks.md#expressions) that must all hold on the player before this tick's movement. They read what a state machine reads (`grounded`, `airborne_time`, `speed`, `vertical_speed`, a field, an item kind held, a stance), whether a key is down (`held.jump`, `held.dash`) or went down this tick (`pressed.dash`), `linked` (a probe of the player's is out) and `link_distance` (metres from the player's chest to the point its probe holds; 0 with none) |
| | `cooldown` | seconds between two uses; for a While, between its end and its next start |
| Uses | `uses` | how many before a refill; 0: no limit. Not for a While |
| | `refill`, `refill_seconds` | **On the ground**: back when the player stands. **After seconds**: back that long after the last use |
| While it lasts | `duration` | how long the motion is on after it starts (with a probe: after the probe takes hold). A While is on while its conditions hold |
| | `until` | expressions, any of which ends it (`pressed.grapple`, `not held.grapple`). With one, or with a probe, and no `duration`, the motion is on until something ends it. Not for a While |
| | `parameters` | [movement parameters](server-mods.md#movement-parameters) the player has while it is on (`friction` = 0). They win over the server's, the character's and a mod's `SetMove` for that long |
| When it happens | `changes` | fields of the player: `dash.charges -= 1` (`-=`, `+=`, `=` with a number), written like a [prediction's](looks.md#predictions). In a While, `-=` and `+=` are per second and need a Float field (`flight.fuel -= 30`), and `=` is set when it starts |
| | `emits` | a mod event at the player; with a probe, at the point it will hold. A While emits it when it starts |

- **Order**: a tick runs the frame's commands, then each player's motions (in the schema's order), then the mover. So a `Set` from the mod and a `change` from a motion in the same tick both count.
- **Conditions read the fields as the tick found them**: a motion that changes a field does not start another until the next tick. That is what makes a toggle of two motions (`FlyOn` if not `flight.on`, `FlyOff` if `flight.on`) one switch a press.
- **A frozen or dead player** does none; a key held through a freeze is not a press when it ends.
- **As many motions as the mods' sets have**, all together: a player carries 16 bytes for each.
- **In the editor**: every group has an info row (click the icon), every property a hover text, F1 opens the class reference. A part that cannot run is a warning on its node, on its motion and on the set, and stops the bake.
- **Names the editor cannot check**: the action, fields and events are the server mod's. One no mod declares makes that part do nothing (the motion never happens, the change is skipped, nothing is emitted), and the server says so when it starts.

## Effects: on whom, in which direction

Every `CbImpulse`, `CbForce` and `CbLink` has the same two questions (`CbMotionEffect`).

| `target` | Acts on |
|---|---|
| **The player** | whose motion it is |
| **What the probe found** | a prop or a player, at the point the probe holds. The world takes nothing |
| **The entity a field names** | `target_field`: a field of the player holding an entity's id (`bind.partner`), which the server mod writes. Names nothing: the effect does nothing |

| `frame` | The direction |
|---|---|
| **Look** | where the camera looks, pitch included |
| **Move input** | WASD, relative to the camera; the facing when none is held |
| **Facing**, **Up** | where the body faces; straight up |
| **World direction** | `direction`, as given |
| **Toward the target** | from the player to the target; for an effect on the player itself, to what its probe found. A pull |

The direction is always the acting player's own: "look" on another player pushes it where *you* look.

### `CbImpulse`

Once, on the tick the motion starts (with a probe: when it takes hold).

| Field | Meaning |
|---|---|
| `speed` | the change of velocity, m/s (up to 200), whatever the target weighs |
| `replace` | what of the target's velocity is cleared first: **Nothing**, **Vertical speed**, **Horizontal velocity**, **All** |

### `CbForce`

Every tick the motion is on.

| Field | Meaning |
|---|---|
| `kind` | **Acceleration**: `strength` in m/s², the same for everything (a jetpack). **Force**: newtons, divided by what the target weighs (a hook's pull). **Velocity**: brings the target's speed along the direction to `speed`, by at most `strength` m/s per second (a conveyor, a brake) |
| `strength` | how hard |
| `speed` | Acceleration and Force: a top speed along the direction, past which it pushes no more (0: none). Velocity: the speed it goes to |
| `ramp_in` | seconds over which it rises from nothing to its strength |
| `react` | the other end takes the same momentum the other way: the player, when the force is on something else; what the probe found, when it is on the player. With Velocity, `speed` is then how fast the two close on each other, and each takes its share by weight |

- **A ramp hides latency.** Other players see your press a moment late; a force that takes 0.15 s to arrive is still weak when they catch up, so little has to be corrected. It is also what makes a pull feel heavy.
- **A pull upward wants the Velocity kind.** A force toward a point that is above and far off has only its upward part against all of gravity: 24 m/s² at 18 degrees is 7.5 m/s² up, gravity is 18, and the player is dragged along the floor until the line is steep (0.97 s for a hook 2.5 m up and 8 m away). Velocity brings the speed along the line to its target whatever gravity takes meanwhile: the same hook lifts after 0.25 s. Its `strength` has to be well above gravity over the sine of the shallowest angle (120 here). Gravity still acts across the line, so the player still swings.
- **A zip line turns gravity off.** With gravity on, the pull keeps the speed *along* the line and gravity keeps dragging the player *off* it: the line swings steeper before the climb is felt (a hook 6 m up and 8 m away: 0.5 m of rise after 0.68 s). `gravity` 0 in the motion's `parameters` makes the path the straight line to the point: the same hook lifts 0.5 m in 0.33 s, and the speed is the same at any angle. Gravity is back the tick the motion ends.
- **`react` is what makes weight count**: hooked to a crate, the crate comes to you; hooked to something ten times your weight, you go to it.

### `CbLink`

A rope between the player and the target, every tick the motion is on. Slack, it does nothing. Taut,
the two come back to its length, and what that takes is shared by what they weigh: the world gives
nothing (you swing), a light crate trails behind you, of two players the lighter is dragged more.

| Field | Meaning |
|---|---|
| `target` | what the probe found (the default), or the entity a field names. Not the player itself |
| `length` | metres. 0: as long as the two are apart when the probe takes hold (a motion without a probe has to give one) |
| `reel` | with a length of 0: metres of rope taken in a second (never shorter than a metre) |

## Weight

Players weigh something: `mass` is a [movement parameter](server-mods.md#movement-parameters) (80 kg).

| Where it counts | How |
|---|---|
| Walking into a prop | the player and the prop trade momentum by their masses: a 4 kg crate is kicked away, a 2 t block stops you |
| A `CbForce` of kind Force | newtons divided by the mass |
| A `react`, a `CbLink` | shared by the two masses |
| A ragdoll | weighs what its player did |

- **Who sets it**: like any parameter. The server (`--move mass=120`), the character, a mod (`ctx.SetMove( target, MoveParam::Mass, 300.0f )` for a suit of armour), a motion's `parameters` (heavy while it is on).
- **The controller stays kinematic**: a player is still moved by the mover, not by the solver; the mass decides what its contacts and effects exchange. A player is not knocked over by a prop.
- **Props** weigh their shape's density times its volume (40 kg/m³ unless the map says otherwise: a 1 m crate is 40 kg).

## Probes, forces and links: the grapple mod

A probe is a line a motion throws at what the player looks at. `server_mods/grapple` is an item, one
motion with three parts, and a look. Its C++ declares the names and gives every life a hook.

The hook is an [item used by its slot's key](items.md#using-an-item) (`prefabs/hook.tscn`, a `CbItem`
with `use` "Its slot key uses it" and `slot` 4): the key uses it where it is, nothing comes into
the hand, and the engine records `grapple.hook.used` on that tick in every simulation. The motion
starts on that event, and ends on the same event, so the throw is predicted like any press.

```
Moves   CbMotionSet   set_name "grapple.moves"               (motion_sets/grapple_moves.tscn)
└── Hook      CbMotion   on the cue grapple.hook.used (the hook's slot key)   conditions not linked
    │                    until grapple.hook.used, pressed.jump      duration 2 s
    │                    parameters { airborne: 1, air_control: 0.6, gravity: 0 }   emits grapple.fired
    ├── Line  CbProbe    range 40 m, flies at 80 m/s
    ├── Pull  CbForce    on the player, toward the target: 160 m/s² up to 15 m/s, ramp 0.1 s, react
    └── Rope  CbLink     to what the probe found: its length when it takes hold, reel 4 m/s
```

| `CbProbe` | Meaning |
|---|---|
| `range` | how far the line reaches, in metres. The motion **happens only if the line finds something** |
| `travel` | the speed it flies at: it takes hold after distance / speed. 0: at once |

| Step | What happens |
|---|---|
| The throw | two traces, like a shot: what is under the crosshair (along the camera's line from the point it orbits, or a shoulder), then from the player to that point, so something in between stops it. Nothing within range: nothing happens, not even the cooldown |
| What it finds | the world: a point. A prop: a point on that body, which moves with it. A player: a point on its capsule (not a limb: hitboxes are the server's) |
| Flying | until `distance / travel` has passed, the motion is not on yet: no parameters, no effects; the look draws the line growing |
| Holding | the motion is on. Its effects run every tick, before the mover: here the pull (up to 15 m/s along the line) and the rope. `react` shares the closing speed with what it holds, so a crate flies to the player |
| While it holds | its `parameters` hold. The grapple's `airborne` = 1 puts the player in the air from the moment the hook takes hold until it lets go: nothing rubs the pull off, the in-air animation plays, and the player falls back to the ground afterwards. `gravity` = 0 makes it a zip line: straight to the point at one speed |
| Its time | `duration` 2: it lets go by itself two seconds after it took hold, and the player keeps its speed; gravity is back on that tick. (`link_distance < 1.8` in `until` would let go on arrival instead) |
| Letting go | `until` (the key again, or a jump: `pressed.jump`); the `duration`; what it held on to being destroyed; the player dying, or being put somewhere else (a respawn) |

- **Hold or toggle** is two lines of the motion:

  | | `conditions` | `until` |
  |---|---|---|
  | Use to throw, use again to let go (the grapple mod: an item) | `not linked` | `grapple.hook.used` |
  | A key of its own: press to throw, press again to let go | `not linked` | `pressed.grapple` |
  | A key of its own: hold to grapple | | `not held.grapple` |

  `not linked` keeps the second use from throwing a second line; the event (or `pressed.grapple`) is that use letting the first go. A hook that ends by itself (its prop destroyed, a death) leaves nothing behind: the next press throws.
- **One probe per player**: a second throw replaces the first.
- **It is state** (`MotionHold`, a component a player has once it threw one): hashed, rolled back, in snapshots.

### The rope: `CbLinkLook`

```
GrappleReactions            (vfx/reactions_grapple.tscn)
├── Rope    CbLinkLook      motion "grapple.moves/Hook"   scene vfx/grapple_rope.tscn   from "RightHand"
├── Fired   CbReaction      on grapple.fired, subject $at: the throw's sound at the hand
└── Hit     CbReaction      on grapple.fired, subject $at: a puff at the cue's point
```

| Field | Meaning |
|---|---|
| `motion` | whose line it draws: the set and the node (`grapple.moves/Hook`). Empty: any line without a look of its own |
| `scene` | a scene one metre long along its -Z; the game stretches it from the player to the line's end, while it flies and while it holds |
| `from` | the player's socket it starts at (`RightHand`); empty, or a socket the character lacks: its chest |

In conditions (a reaction, a HUD node), `linked` is true for a player whose line is out, and
`link_holds` once it has taken hold.

## Things that fly: the grenade mod

A probe is a line: it has no body, nobody sees it coming, nothing can step out of its way. A
`CbLaunch` throws a *thing*. `server_mods/grenade` is one motion, one item and thirty lines of rules.

```
Moves   CbMotionSet   set_name "grenade.moves"               (motion_sets/grenade_moves.tscn)
└── Throw     CbMotion   on the press of throw (H)   cooldown 1.2 s   emits grenade.thrown
    └── Shell CbLaunch   item_kind grenade.shell   speed 17 m/s   lift 3 m/s   ahead 0.8 m   seconds 4

Shell   CbItem   kind "grenade.shell"   mass 0.4   friction 0.8   bounce 0.35    (prefabs/shell.tscn)
├── Body  CollisionShape3D   a sphere, 9 cm
└── Ball, Band  MeshInstance3D
```

| `CbLaunch` | Meaning |
|---|---|
| `item_kind` | what is thrown: an item kind a server mod declares and a [`CbItem`](items.md) bakes. Its [body](maps.md#a-body), its weight, how it bounces and its look are the item's |
| `speed`, `frame`, `direction` | how fast and where to: along the look (the default), the move input, the facing, up, or a world direction |
| `lift` | metres per second upward added: an arc |
| `ahead` | how far in front of where the look starts it begins (0.7 m): clear of the thrower's own body |
| `seconds` | when it is removed again. 0: it stays, like anything dropped |

| Step | What happens |
|---|---|
| The press | every simulation makes the item on that tick, from the thrower's own input: the thrower sees it leave at once, with no round trip. It also has the thrower's own velocity: a throw on the run goes further |
| Flying | it is an item lying in the world, moving: the physics is the simulation's, the same everywhere. `net_grenade`: behind 100 ms, 111 predicted ticks of its flight are exactly the server's |
| Who threw it | the item knows (`ctx.ThrownBy( item )`), until someone picks it up |
| What it hits | the server mod's to decide: `ctx.Hits( kind )` is what items of the kind ran into on the tick before (what, how fast, where). The grenade goes off on the first hard hit: `combat.damage` to everyone within 4 m, less with distance; a push on everything loose near it (`ctx.BodiesNear( point, radius )`: props, items, ragdolls), given off each body's centre so it tumbles; and `ctx.Destroy( item )`. The living keep their feet; one it kills is thrown, as a ragdoll |
| Its look | its own scene, wherever it is; what going off looks like is a reaction on the mod's event (`grenade.blast`: a burst, a bang, a shake) |
| How many | a launched item counts against the server's caps on what a player may leave in the world (`--props-per-player`, `--props-global`: the oldest go first), and the motion's `cooldown` or `uses` say how often |

- **It is an item like any other**: it can be picked up and carried, unless its kind says no (the grenade's `pickup.never` property, which the pickup mod asks).
- **Other players' throws** are seen when their press arrives, like their dash: a moment into the flight, at the place the simulation has it by then.
- **Not for a While**: a launch happens when a motion starts.

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
├── Thrust   while   flight.jetpack_on, held.jump, not grounded, not flight.on, flight.fuel > 0
│   │                changes flight.fuel -= 30     emits flight.thrust
│   └── Lift   CbForce   acceleration 26 m/s² up (gravity is 18)
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
cinderbox_motions	2
motion	Dash
when	press	dash
if	( dash.charges > 0 )
cooldown	0.4
duration	0.18
param	friction	0
impulse	self	11	move	horizontal
change	dash.charges	-=	1
emit	dash.started
```

| Line | Fields |
|---|---|
| `probe` | range, travel |
| `impulse` | target, speed, frame, replace, and x y z for a world direction |
| `force` | target, frame, kind (`accel`, `force`, `velocity`), strength, speed, ramp, react (0 or 1), and x y z |
| `link` | target, length, reel |
| `launch` | item kind, speed, frame, lift, ahead, seconds, and x y z for a world direction |

A target is `self`, `hit` or `@field`.

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
| The client | predicted every tick of the two dashes and the double jump exactly as the server then had it: 384 ticks compared, none different |
| The recording | replays to the same checksums, motions included |

`net_flight` does the same for the flight mod: flight switched on, a flight into the sandbox's wall, off and down, the jetpack
held until its tank is empty, a glide, the landing and the refuel. 1,664 predicted ticks compared, none different: the
position, the fuel's exact bits, the events.

`net_grapple` does it for the hook: two throws at walls, the flight of the line, the pull along the rope and the letting
go. 505 predicted ticks compared, none different (the position and the line's end). Then two players put their hooks in
one ball: both hold on to it, it goes 8 m, and both clients have the server's state for every confirmed tick.

## Not yet

| Missing | Roadmap |
|---|---|
| An effect on another player is felt by it on its next tick when it has a lower slot (it has moved already), and no test hooks one player to another yet. In first person a line starts at the same point as in third person, not the eye | later |
| A lasting force as a server mod's command (C++), a knocked-down ragdoll a heavy hit puts a player in, carrying a player, structures that break | later |
| A preview panel; `motion.<name>` in conditions and state machines; names checked at publish | Motions in the editor |
| Another player's dash is seen when its input arrives: its press is guessed by repeating its last input, as a jump is | by design |
