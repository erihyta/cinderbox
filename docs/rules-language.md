# Rules as a language

The grammar of [roadmap step 3](../ROADMAP.md#3-rules-as-a-language): the rules sheet grows into a
small language the simulation runs, so everything that moves on screen is predicted, and a mod's C++
keeps only what the server alone should decide. **Partly built**: what is marked *built* works
today; the rest is the plan.

| Part | State |
|---|---|
| Blocks: `motion` headers, `on use` / `on press` / `on EVENT` / `while` / `when`, `then when ... within`, `after`, `timeout`, `if` / `else`, `end`, `lasts`, `until`, `cooldown`, `uses`, `item` | *built* (M204) |
| Statements in a block: `change`, `emit`, `impulse`, `force`, `move [for S]`, `launch` (as a `use` rule has them) | *built* (M204) |
| `find` with shapes from nodes or written in the line, a block per thing found, `found`, `away`, `move for` on a player found, `emit` with who was found | *built* (M205) |
| `cast` (a shape swept along a direction: the probe in a sheet), `distance` / `reach` and `$target:FIELD` in a find's lines, `at POINT` | planned (3.2, next) |
| `curve()`, `time`, markers, `follow` | planned (3.3) |
| `owner item`, `break`, `remove self` | planned (3.4) |
| `impulse kind momentum`, `every` | planned (3.5) |

## The idea in one picture

```
a .rules sheet ──bake──> motions/<set>.cfg ──> every simulation runs it (server and clients)
    │                                            same input, same state, same result: predicted
    └── names nodes by path: shapes, markers, paths, curves (things you see in the 3D view)

a server mod (C++) ──> damage, score, rounds, shops, who wins: the server's only, a lead late
```

| Goes in the sheet (simulation) | Stays in C++ (server) | Stays in looks (viewer) |
|---|---|---|
| anything that moves a body, starts or ends a motion, finds who is near, changes a movement parameter, launches, breaks a wall | damage, health rules, score, inventories, timers of a round, anything secret or decided by the server | sounds, particles, camera shakes, HUD text: reactions and predictions as today |

## What the language may not do

Every machine runs it, and rollback runs a tick again many times. So:

| Rule | Why |
|---|---|
| **Deterministic**: no clock, no randomness but the simulation's own (`random` is seeded by the tick and the player) | the server and every client must reach the same result |
| **Bounded**: no loops but `every`, which ends with its motion and runs at most 64 times; `find` takes at most 16 targets | a tick always costs about the same, however often it is run again |
| **No storage of its own**: state is fields, the world, and each running motion's small record (below) | the record is rolled back with the rest; nothing outside it can drift |
| **No files, no network, no text building** | it runs inside the physics step |

A running motion's record: which step it waits at, since which tick, its anchor (a point and a
facing), its current target. A fixed size, like the 28 bytes a motion has today (its step, since when, its ifs' answers).

## A sheet: three kinds of block

A `.rules` file already holds reactions (`on`, `while`), predictions (`predict`) and HUD lines
(`show`, `text`, `bind`), all run by the viewer. The new block is **`motion`**, run by the
simulation. Its first word keeps the two apart: `on` and `while` stay the viewer's.

```
motion NAME TRIGGER [if EXPRESSION]
    [setting lines]
    statements
```

`use [if ...]` (today's form) stays; `motion NAME on use` is a motion of its own, beside it.

### How a block runs (built)

- **Steps.** A `then when` or an `after` line starts a new **step**. The first step is what happens
  when the motion starts; each next one starts once its wait is over (`after` seconds since the step
  before started, then `when` its condition holds), at the earliest on the tick after. So `then when
  grounded` after a dive is the tick after the player touches the ground.
- **Once and lasting.** A step's once statements (`change`, `emit`, `impulse`, `launch`, `end`)
  happen when it starts. Its lasting ones (`force`, `move`) act while it is the current step;
  `move for S`, for S seconds from when the step started.
- **`if` is decided when its step starts**, and holds for the whole step: an `if` around a `move`
  moves for the whole step or not at all. A condition asked every tick is a `while` motion's.
- **Giving up.** `then when EXPRESSION within S` gives up after S seconds: the `timeout` lines
  below it (once statements only) run, and the motion ends. Without a `timeout`, it just ends.
- **When it ends**: its `lasts` run out, its `until` holds, an `end` is reached, a wait gives up,
  or, with none of `lasts` and `until`, its last step's longest `move for` is over (one tick at
  least). A motion that waits is on while it waits (`motion.<name>` is true), so a press does not
  start it again.
- **`when EXPRESSION`** starts it on the tick the expression turns true. Its first answer is only
  remembered: a player standing from the start has not landed.
- **In an item's sheet** a motion is for whoever has the item out; `item carried`, whoever carries
  it. `on use` is the item's use (its key, or the button with it in the hand), whoever.
- **In the file** (`motions/<set>.cfg`, version 3): `step <after> <within> [condition]`,
  `timeout`, `guard [condition]` (an `if`, written out: `not ( a ) and ( b )`), `end`, `when edge
  <condition>`, and `param <name> <value> <seconds>`.

## Triggers

| Trigger | Starts the motion | Today's equivalent |
|---|---|---|
| `on use` | the item is used (in an item's sheet) | `use`, CbItem's When used |
| `on press ACTION` | the action goes down (`dash`, `jump`, `sprint`, any a mod declares) | On a press |
| `on EVENT` | a mod event or a simulation event at the owner (`brawl.stunned`, `frag.grenade.expired`, `impact`) | On a cue |
| `while EXPRESSION` | on every tick it holds, off when it stops | While |
| `when EXPRESSION` | **new**: once, on the tick the expression turns true (it was false the tick before) | none |

`when` is what replaces a "landed" trigger: `when grounded` is a landing, `when flight.fuel <= 0`
an empty tank, `when combat.health < 20` a low-health burst, `when wall` the first touch of a
wall (once that condition exists). One general edge, not one trigger per case.

## Settings (lines under the header, before any statement)

| Line | Means | Today's |
|---|---|---|
| `cooldown S` | seconds between two starts | `cooldown` |
| `uses N`, `uses N refill ground`, `uses N refill after S` | charges | `uses`, `refill` |
| `lasts S` | the motion ends S seconds after it starts | `duration` |
| `until EXPRESSION` | ends it when it holds (several lines: any) | `until` |
| `item hand` / `item carried` | in an item's sheet: who may run it, whoever has the item out (the default) or whoever carries it | `with_item` |
| `owner self` / `owner item` | who runs it: the player (default), or the item itself while it is in the world (a grenade's own fuse). *Planned* | **new** |

## Statements

Run top to bottom on the tick they are reached. Most finish at once; **waits** hold the motion
there until they are done; **lasting** ones act every tick until the motion ends (or their own
`for`).

### Once

| Statement | Does |
|---|---|
| `change FIELD -= N` / `+= N` / `= N` | a field of the owner (or the target, inside `find` / `cast`, with `$target:FIELD`) |
| `emit EVENT [at POINT]` | a mod event: for looks, state machines, and a mod's C++ (damage) |
| `impulse VALUE [kind velocity\|momentum] [on WHO] [along DIRECTION] [replace vertical\|horizontal\|all]` | a change of velocity (m/s, any weight: the default) or of momentum (N·s: heavy things move less) |
| `launch new KIND \| carried [speed] [lift] [ahead] [seconds] [along DIRECTION] [from NODE]` | an item into the world, as today; `from` a marker (the cannon's muzzle) |
| `break [radius R] [speed S] [seconds S] [at POINT]` | breakable walls near the point open, as a grenade's blast does today |
| `remove self` | the owner item leaves the world (a mine once it has gone off) |
| `end` | the motion ends here |

### Lasting

| Statement | Does |
|---|---|
| `force VALUE [kind acceleration\|force\|velocity] [top S] [react] [on WHO] [along DIRECTION]` | every tick while on, as `CbForce` |
| `move [for S] PARAMETER VALUE ...` | movement parameters of the owner, or of the target inside `find` / `cast` (a slow, a root) |
| `link [length L] [reel S] [react]` | a rope to the target, as `CbLink` |
| `follow PATH [over S \| speed V] [turn]` | **new**: carries the owner along a `Path3D` placed relative to where the motion started (a leap arc, a curved dash, a rail) |

### Waits and order

| Statement | Does |
|---|---|
| `then when EXPRESSION [within S]` | waits until it holds; with `within`, gives up after S seconds (the `timeout` lines below it run then) |
| `after S` | waits S seconds |
| `timeout` | what a `within` that gave up runs (once statements), before the motion ends |
| `every S` | the block below runs now and every S seconds while the motion lasts (a burn, a rapid fire, a pulse) |
| `if EXPRESSION` / `else` | a block (indented under it) runs or not, decided when its step starts |

### Finding things

| Statement | Does |
|---|---|
| `find SHAPE [players] [bodies] [self] [max N] [along facing\|look\|world]` | *built*: everything the shape overlaps when its step starts. The lines further in run **once per thing found**, nearest first (then by id), at most N (16): `impulse` (on what it found, unless `on self`), `move for S ...` (given to a player found for S seconds), `emit` (said with the player as A and what it found as B, at it). Players and bodies (props, items lying about) both unless one is named; the player itself only with `self`; the shape turned as the player faces (the default), looks (with the pitch), or not at all |
| `cast SHAPE [along DIRECTION] [range R] [speed V] [holds level\|bodies\|players]` | a shape moved along a direction: the first thing it hits. Speed 0: at once (a ray); otherwise it flies (today's probe). The block runs when it hits, with `$target` and `point` |

| A `SHAPE` is | |
|---|---|
| a node's path | *built*: an `Area3D` (its `CollisionShape3D` children together) or one `CollisionShape3D`, from the node the sheet's `CbRules` is under: a sphere, capsule, box, cylinder, or a convex shape made from a mesh (Godot's *Mesh > Create Collision Shape*). Drawn around the scene's origin as if the player stood there (its feet) facing -Z. **This is how a probe's shape is authored**: draw it in the 3D view. An `Area3D` under an item is not part of its body |
| `ray` | a line (today's probe; with `cast`, planned) |
| `sphere R`, `box (X,Y,Z)`, `capsule R H` | *built*: written in the line, around the player's middle (a metre up) |

- **`along away`** (inside a find): from where the player is out to what it found, along the ground
  (right on top of it: where the player faces). `on found` is the default target there; `on self`
  reaches the player.
- **A `move` given** holds on that player for its seconds, after its own motions' parameters (a slow
  holds through its dash); the newest wins a parameter. A player holds six at most (the oldest goes).
- **Planned**: readable in a find's lines, `distance` (from the player to what it found), `reach`
  (that over the shape's size: 0 at the centre, 1 at the edge), `point`, `$target:FIELD`; a find
  `at` a point other than the player.

## Who and where

| `on WHO` | |
|---|---|
| `self` | the owner (the default outside `find` / `cast`) |
| `target` | the one found (the default inside) |
| `FIELD` | the entity a field names (`bind.partner`) |

| `along DIRECTION` | |
|---|---|
| `look`, `move`, `facing`, `up` | as today |
| `world (x,y,z)` | as given |
| `to` / `away` | toward the target / from the shape's origin out to the target (a shockwave) |
| `NODE` | **new**: a `Marker3D`'s forward (−Z) in the owner's frame: an arrow drawn in the 3D view |
| `PATH at T` | **new**: the tangent of a `Path3D` at T (0 to 1) |
| any of these `turn (yaw, pitch)` | turned by degrees: `along look turn (0, 20)` throws a little above the look |

| `at POINT` | |
|---|---|
| `here` | the owner now (the default) |
| `anchor` | where the motion was when it started |
| `point` | where the last `cast` hit |
| `NODE` | a marker in the owner's frame |

## Numbers and curves

Any number in a statement may be an [expression](looks.md#expressions), read when the statement
runs. Two words are added:

| Write | Means |
|---|---|
| `curve(NODE, X)` | a `Curve` resource on a node, read at X. Baked into a table of 64 points, so every machine reads the same |
| `time` | seconds since this step started (since the motion started, at its first step) |

So falloff, ramps and shapes over time are expressions, not new settings:

```
impulse 9 * curve(Falloff, reach) along away          # strong at the centre, weak at the edge
force 40 * curve(Ramp, time / 0.3) along look   # the curve is the ramp-in
move speed 0.3 + 0.7 * time / 2 for 2           # a slow that wears off
```

`ramp_in` and the like keep working; a curve is the general form.

## Impulse, force: the physics words

| Word | Is | Unit | Weight matters |
|---|---|---|---|
| `impulse ... kind velocity` (default) | a change of velocity, once | m/s | no: a crate and a truck both get 9 m/s |
| `impulse ... kind momentum` | an impulse proper: a change of momentum, once | N·s | yes: 900 N·s is 9 m/s to 100 kg, 0.9 m/s to 1000 kg |
| `force ... kind acceleration` | a change of velocity per second, every tick | m/s² | no |
| `force ... kind force` | a force, every tick | N | yes |

An impulse is a force over a moment (N·s = N × s), not an acceleration: an acceleration is what
a force gives per second. `kind velocity` is what games mostly want (Unity's `VelocityChange`),
and Godot's `apply_impulse` is `kind momentum`.

## Examples

### The dash, as today (unchanged)

```
use if dash.charges > 0
    change dash.charges -= 1
    cooldown 0.4
    emit dash.started
    impulse 11 replace horizontal
    move for 0.18 friction 0
```

### The ground slam: no C++ but damage (*built*: `server_mods/brawl/client/prefabs/slam.rules`)

```
motion Slam on use if brawl.cd_slam <= 0 and not grounded
    lasts 3
    change brawl.cd_slam = 5
    emit brawl.slam.started
    impulse 25 along world direction (0, -1, 0) replace vertical
    then when grounded
    emit brawl.slam.landed
    find Shockwave players                       # an Area3D: a cylinder 5 m around the feet
        impulse 9 along away
        impulse 4.5 along up
        emit brawl.slam.hit                      # the brawl mod: 25 damage
```

### A frag grenade whose blast is predicted

```
# frag.grenade's own sheet: it runs on the grenade while it is in the world
motion Blast on frag.grenade.expired
    owner item
    emit frag.blast
    break radius 2.5 speed 14 seconds 8
    find Blast players bodies                  # a sphere, 5 m
        impulse 14 * curve(Falloff, reach) along away
        emit frag.blast.hit                      # the frag mod: damage by distance
                                                 # (the simulation removes it on .expired, as today)
```

### The grappling hook, as one block

```
motion Hook on use
    until pressed.jump or pressed.grapple
    lasts 2
    cast ray along look range 40 speed 80 holds level bodies players
        emit grapple.hooked at point
        link reel 4
        force 120 kind velocity top 24 on self along to
        move gravity 0.6
```

### A cannon with its muzzle as a marker

```
use if not combat.dead
    cooldown 1.2
    emit cannon.fired
    launch new cannon.ball speed 36 lift 1 seconds 10 from Muzzle along Muzzle
```

### A flamethrower: a cone, every tick

```
motion Flame while held.use and flame.fuel > 0
    change flame.fuel -= 25
    every 0.1
        find Cone players bodies               # a convex shape made from a cone mesh
            impulse 0.6 along look
            emit flame.burn                       # the mod: damage over time
```

### A leap along a drawn arc

```
motion Leap on press leap if grounded
    cooldown 4
    follow LeapArc over 0.6 turn                 # a Path3D in the item's scene
    then when grounded within 2
        find Landing players
            move for 1 speed 0                   # rooted for a second
```

### What the parked "player's body" pieces would add (words, not new nodes)

```
motion WallRun while wall and held.jump and not grounded
    move gravity 0.2
    force 30 kind velocity top 9 along wall

motion Blink on press blink
    cooldown 6
    cast ray along look range 20
        teleport to target behind 1.2
```

## Grammar, in short

```
sheet      := { block }
block      := motion | use | reaction | prediction | hud-line          (the last three as today)
motion     := "motion" NAME trigger [ "if" expr ] NEWLINE INDENT { setting } { stmt } DEDENT
use        := "use" [ "if" expr ] NEWLINE INDENT { setting } { stmt } DEDENT
trigger    := "on use" | "on press" ACTION | "on" EVENT | "while" expr | "when" expr
setting    := "cooldown" num | "uses" num [ "refill" ( "ground" | "after" num ) ]
            | "lasts" num | "until" expr | "item" ( "hand" | "carried" ) | "owner" ( "self" | "item" )
stmt       := once | lasting | wait | flow | query
once       := "change" field op value | "emit" EVENT [ "at" point ]
            | "impulse" value [ "kind" kind ] { option } | "launch" launch-args
            | "break" { option } | "remove self" | "end"
lasting    := "force" value { option } | "move" [ "for" value ] { PARAM value }
            | "link" { option } | "follow" NODE { option }
wait       := "then when" expr [ "within" num ] | "after" num | "timeout"
flow       := "every" num block-body | "if" expr block-body [ "else" block-body ]
query      := "find" shape { option } block-body | "cast" shape { option } block-body
shape      := NODE | "ray" | "sphere" num | "box" vec | "capsule" num num
value      := expr                                  (looks.md's expressions, plus curve() and time)
option     := "on" who | "along" direction | "at" point | "replace" what | "top" value | ...
```

Indentation is a tab or four spaces, as sheets are today. Mistakes are said with their line, in
the editor and at publish, as today.

## The nodes

| Node | After |
|---|---|
| `CbMotion`, `CbImpulse`, `CbForce`, `CbMove`, `CbLink`, `CbLaunch`, `CbProbe` | keep working, bake to the same file; no new features. A tool writes a scene's motions as a sheet, as `reactions_to_rules.py` did for reactions |
| `CollisionShape3D`, `Marker3D`, `Path3D`, a node holding a `Curve` | what the sheet names: places, shapes and arrows, seen and edited in the 3D view |

## Open questions

| Question | Leaning |
|---|---|
| Two slows on one player: the stronger, the newer, or multiplied? | the newer wins per parameter, as motions do today; `move ... multiply` to stack |
| How many timed `move`s may a player carry from others? | a few slots each (4), the oldest dropped: a fixed size for rollback |
| May a sheet change another player's fields (`$target:combat.health -= 25`)? | not at first: health is a mod's rule; `emit` and the mod's C++ do it. The HUD shows it a lead late, which is how damage is today |
| `owner item` for a prop of the map (a mine, a door), not only an item? | later: once items work |
| A shape that follows a bone (a sword's swing sweeping its arc)? | later: a `cast` along a pose marker over the swing's ticks |
| Other players' presses | unchanged: seen when their input arrives. The language makes the presser's own screen instant and keeps everyone else's from being corrected |

## Order of work

1. **Blocks** (*built*, M204): `motion` headers, `when`, `then when`, `after`, `timeout`, `if` / `else`, `end`; today's `use` sheets unchanged. The brawl slam's landing is its sheet's now (`then when grounded`).
2. **Finding**: `find` (*built*, M205: the brawl slam's shockwave is its sheet's now, the mod only hurts), then `cast` with shapes from nodes, `distance` and `reach`.
3. **Curves and markers**: `curve()`, `time`, `along NODE`, `from NODE`, `turn`; `follow`.
4. **Items run motions**: `owner item`, `break`, `remove self`; the frag grenade's blast predicted.
5. **`impulse kind momentum`**, `every`.
6. **The mods converted**: brawl, frag, cannon, grapple, flight, dash, as the test that nothing is lost.
