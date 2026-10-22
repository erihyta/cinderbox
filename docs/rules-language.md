# Rules as a language

The grammar of [roadmap step 3](../ROADMAP.md#3-rules-as-a-language): the rules sheet grows into a
small language the simulation runs, so everything that moves on screen is predicted, and a mod's C++
keeps only what the server alone should decide. **Partly built**: what is marked *built* works
today; the rest is the plan.

| Part | State |
|---|---|
| Every mod's motions as sheets (none uses a motion node; the SDK's starter is a sheet) | *built* (M211) |
| Blocks: `motion` headers, `on use` / `on press` / `on EVENT` / `while` / `when`, `then when ... within`, `after`, `timeout`, `if` / `else`, `end`, `lasts`, `until`, `cooldown`, `uses`, `item` | *built* (M204) |
| Statements in a block: `change`, `emit`, `impulse`, `force`, `move [for S]`, `launch` (as a `use` rule has them) | *built* (M204) |
| `find` with shapes from nodes or written in the line, a block per thing found, `found`, `away`, `move for` on a player found, `emit` with who was found | *built* (M205) |
| `cast` (the probe in a sheet: a line, a ball or a shape drawn in the scene, swept along the look; what is under it happens while it holds), `link` | *built* (M206: the grapple is a sheet now) |
| Numbers that are expressions (an `impulse`'s and a `force`'s), `curve(Name, x)` from a `CbCurve`, `time`, `distance` and `reach` in a find | *built* (M207) |
| Markers (`along NODE`, `launch ... from NODE`), `turn (yaw, pitch)`, `follow PATH over S`, a launch's `speed (EXPRESSION)` | *built* (M208) |
| Expressions in a `move` (a parameter is read by the mover whenever it moves, with no motion's values at hand); `$target:FIELD` in a find's lines, `at POINT`, `PATH at T`, a cast in a later step | planned |
| `owner item` (an item's own motion, `on expired` or `on EVENT`), `break`, `remove self` | *built* (M209: the frag grenade's blast is its sheet's) |
| `impulse ... kind momentum`, `every S` | *built* (M210) |
| Made regular: one table of where a line may stand (every refusal made from it), `on target`, `along world (x, y, z)`, a find `turned` and `at`, every find shape from the feet, `change` in a find, `use` exactly `motion Use on use`, an item's motion one tick | *built* (M213) |

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

`use [if EXPRESSION]` is `motion Use on use [if EXPRESSION]`, word for word (M213): the same lines,
the same bake. An item has one motion named `Use`; its `CbItem`'s *When used* fields join it.

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
  least; a motion of one step too, since M213). A motion that waits is on while it waits
  (`motion.<name>` is true), so a press does not start it again.
- **`when EXPRESSION`** starts it on the tick the expression turns true. Its first answer is only
  remembered: a player standing from the start has not landed.
- **In an item's sheet** a motion is for whoever has the item out; `item carried`, whoever carries
  it. `on use` is the item's use (its key, or the button with it in the hand), whoever.
- **In the file** (`motions/<set>.cfg`, version 3): `step <after> <within> [condition]`,
  `timeout`, `guard [condition]` (an `if`, written out: `not ( a ) and ( b )`), `end`, `when edge
  <condition>`, and `param <name> <value> <seconds>`.

## Where a line may stand

*Built* (M213). One table: the parser says every refusal from it (`"force" cannot stand in a find:
there a line is change, emit, impulse or move for`), and this is the same table.

| Line | In a step | After `every` | In a `timeout` | In a `find` | In a `cast` | In an item's motion |
|---|---|---|---|---|---|---|
| `change` | yes | yes | yes | yes: the owner's field, once for each thing found | | yes |
| `emit` | yes | yes | yes | yes: with what it found | one, when it takes hold | yes |
| `impulse` | yes | yes | yes | yes: on what it found | yes: on what it holds | yes |
| `launch` | yes | yes | yes | | | |
| `break` | yes | yes | yes | | | yes |
| `find` | yes | yes | yes | | | yes |
| `if` / `else` | yes | yes | yes | | | yes |
| `move for S` | yes | | | yes: given to a player found | yes | |
| `move` (while the step lasts) | yes | | | | yes: while it holds | |
| `force`, `link` | yes | | | | yes | |
| `follow`, `end` | yes | | | | | |
| `remove self` | | | | | | yes (only there) |
| `cast` | the first step, not under an `if`, once a motion | | | | | |
| `every` | as far in as the motion's first line, once a step | | | | | |

- **Structure lines** (`then when`, `after`, `timeout`) and **settings** (`cooldown`, `uses`,
  `lasts`, `until`, `item`, `owner`) stand as far in as the motion's first line, not under an `if`;
  settings before the first `then` or `after`.
- **An item's motion** (`owner item`) happens in one tick, on an event said at it: no structure
  lines, no settings but `owner`, no `if` on its first line (an `if` block is fine), and what the
  table's last column allows. It has no look or keys: it pushes along `up`, `world`, `away` or `to`.
- **Why the gaps**: a find's and a cast's lines happen to something (each thing found, what is
  held); `every` and a timeout repeat or end, so nothing that lasts; a cast is thrown when the
  motion starts. `if` around `every` or a `cast` is not there: an `every` is the rest of its step,
  and a cast belongs to the motion, not to a step.

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
| `owner player` / `owner item` | *built*: who runs it: the player (the default), or the item itself while it lies in the world (in an item's sheet). An item's motion happens in one tick, when its event is said at the item: `on expired` (its time up: the launch's seconds; it runs before the item goes), or `on EVENT` (a mod's, said at it). What it may have is the last column of [where a line may stand](#where-a-line-may-stand); its `if` blocks read the item's fields, the game's, and `speed`. It says its events as its thrower's (A the thrower, B the item: whose blast it is) | **new** |

## Statements

Run top to bottom on the tick they are reached. Most finish at once; **waits** hold the motion
there until they are done; **lasting** ones act every tick until the motion ends (or their own
`for`).

### Once

| Statement | Does |
|---|---|
| `change FIELD -= N` / `+= N` / `= N` | a field of the owner; in a find, once for each thing found (*built*, M213: a count of what was hit). The target's own fields (`$target:FIELD`) are planned |
| `emit EVENT [at POINT]` | a mod event: for looks, state machines, and a mod's C++ (damage) |
| `impulse VALUE [kind velocity\|momentum] [on WHO] [along DIRECTION] [replace vertical\|horizontal\|all]` | *built*: a change of velocity (m/s, any weight: the default; to 200) or of momentum (`kind momentum`: N·s, shared out by what the target weighs, to 100000; heavy things move less) |
| `launch new KIND \| carried [speed S \| speed (EXPRESSION)] [lift] [ahead] [seconds] [along DIRECTION] [from NODE]` | *built*: an item into the world; `from` a marker: its place around the player (turned with its heading), instead of `ahead` of where its look starts (the cannon's muzzle) |
| `break radius R [speed S] [seconds S]` | *built*: the breakable walls whose nearest piece is within R of the owner (its feet, or the item) lose their pieces that near, flung at S (10 by default), staying S seconds (0, the default: for good). Each a number, a name (`frag.break_radius`: a field a mod sets from its options) or an expression in brackets |
| `remove self` | *built*: the owner item leaves the world (a mine once it has gone off). An item's motion only |
| `end` | the motion ends here |

### Lasting

| Statement | Does |
|---|---|
| `force VALUE [kind acceleration\|force\|velocity] [top S] [react] [on WHO] [along DIRECTION]` | every tick while on, as `CbForce` |
| `move [for S] PARAMETER VALUE ...` | movement parameters of the owner, or of the target inside `find` / `cast` (a slow, a root) |
| `link [length L] [reel S] [alone] [on WHO]` | *built*, under a cast: a rope to what it holds (or a field's entity), as `CbLink`; length 0: as long as it is when the cast takes hold; `alone`: it moves the player only |
| `follow PATH over S` | *built*: carries the player along a `Path3D` beside the sheet, drawn from its first point as if the player stood there facing −Z, in S seconds: each tick its stretch of the path is the player's velocity, turned as it faces, so walls still stop it (give it `move gravity 0`, or gravity pulls it off). A leap arc, a curved dash. Baked into 32 points at even steps |

### Waits and order

| Statement | Does |
|---|---|
| `then when EXPRESSION [within S]` | waits until it holds; with `within`, gives up after S seconds (the `timeout` lines below it run then) |
| `after S` | waits S seconds |
| `timeout` | what a `within` that gave up runs (once statements), before the motion ends |
| `every S` | *built*: the lines after it in its step happen when the step starts and every S seconds while it is the step, 64 times at most (a burn, a rapid fire, a pulse). What may follow it: [the table](#where-a-line-may-stand). In a `while` motion: from when it starts, while it holds |
| `if EXPRESSION` / `else` | a block (indented under it) runs or not, decided when its step starts |

### Finding things

| Statement | Does |
|---|---|
| `find SHAPE [players] [bodies] [self] [max N] [turned facing\|look\|none] [at (x, y, z)]` | *built*: everything the shape overlaps when its step starts. The lines further in run **once per thing found**, nearest first (then by id), at most N (16): `impulse` (on what it found, unless `on self`), `move for S ...` (given to a player found for S seconds), `change` (the owner's field), `emit` (said with the player as A and what it found as B, at it). Players and bodies (props, items lying about) both unless one is named; the player itself only with `self`; the shape turned as the player faces (the default), looks (with the pitch), or not at all (`none`; `along facing\|look\|world` is the older spelling); `at` moves it, metres right, up and ahead |
| `cast SHAPE range R [speed V] [holds level bodies players]` | *built*: the probe. Thrown when the motion starts, along the look at what is under the crosshair; speed 0: it holds at once on what is there, or the motion does not happen; otherwise it flies and holds what it meets first. The lines further in are what it does **while it holds**: `impulse` and `force` (on what it holds, unless `on self`), `link`, `move` (the player's while it holds), and one `emit`, said where and when it takes hold. Only in the motion's first step, not under an `if`, once a motion; with one, what that step does to bodies goes under it |

| A `SHAPE` is | |
|---|---|
| a node's path | *built*: an `Area3D` (its `CollisionShape3D` children together) or one `CollisionShape3D`, from the node the sheet's `CbRules` is under: a sphere, capsule, box, cylinder, or a convex shape made from a mesh (Godot's *Mesh > Create Collision Shape*). Drawn around the scene's origin as if the player stood there (its feet) facing -Z. **This is how a find's shape is authored**: draw it in the 3D view. An `Area3D` under an item is not part of its body |
| `ray` | *built*: a cast's plain line |
| `sphere R` (a cast) | *built*: a ball swept along the line: it catches what passes beside it |
| a node's path (a cast) | *built*: one `CollisionShape3D` (or an `Area3D` with one), drawn around its own origin as the line's tip, forward -Z; turned to the line, so what is level stays level (a flat blade stays flat) |
| `sphere R`, `box (X,Y,Z)`, `capsule R H` | *built*: written in the line, around the player's feet, as a drawn one is (M213; before, a metre up). `at (0, 1, 0)` puts it around the middle |

- **`along away`** (inside a find): from where the player is out to what it found, along the ground
  (right on top of it: where the player faces). What it found is the default target there (`on
  target`); `on self` reaches the player.
- **A `move` given** holds on that player for its seconds, after its own motions' parameters (a slow
  holds through its dash); the newest wins a parameter. A player holds six at most (the oldest goes).
- **Planned**: `point` and `$target:FIELD` readable in a find's lines; a find around a point other
  than the player (`at POINT` below). (`distance` and `reach` are built: [numbers](#numbers-and-curves).)

## Who and where

| `on WHO` | |
|---|---|
| `self` | the owner (the default outside `find` / `cast`) |
| `target` | *built* (M213): what the find found, or what the cast holds (the default inside one); only there. `found` and `hit` are the older words for it |
| `FIELD` | the entity a field names (`bind.partner`) |

| `along DIRECTION` | |
|---|---|
| `look`, `move`, `facing`, `up` | as today |
| `world (x, y, z)` | *built*: as given, in the world (`world direction (x, y, z)` is the older spelling) |
| `to` / `away` | toward the target / from the shape's origin out to the target (a shockwave) |
| `NODE` | *built*: a `Marker3D`'s (any `Node3D`'s) forward (−Z) as drawn beside the sheet, as if the player stood at the origin facing −Z; turned as the look is (its heading and its pitch): an arrow drawn in the 3D view |
| `PATH at T` | planned: the tangent of a `Path3D` at T (0 to 1) |
| any of these, then `turn (yaw, pitch)` | *built*: turned by degrees, right and up (pitch within ±90): `along look turn (0, 20)` throws a little above the look, `along move turn (90, 0)` sidesteps |

Planned (none of it is built):

| `at POINT` | |
|---|---|
| `here` | the owner now (the default) |
| `anchor` | where the motion was when it started |
| `point` | where the last `cast` hit |
| `NODE` | a marker in the owner's frame |

## Numbers and curves

*Built* (M207) for an `impulse`'s speed and a `force`'s strength: anything up to the first option
word is an [expression](looks.md#expressions), read when the effect acts (an impulse when its step
starts, a force every tick), within the same limits as a number (200 m/s; a force's own). A `move`'s
values and a `launch`'s are numbers still. Words added, for a motion's expressions:

| Write | Means |
|---|---|
| `curve(Name, x)` | the curve of the `CbCurve` node named `Name` beside the sheet (under the node its `CbRules` is under), read at `x`. Drawn in the inspector's curve editor; baked into 64 values at even steps over its domain (0 to 1 by default), read between two in a straight line, held past its ends: every machine reads the same |
| `time` | seconds since the motion's current step started (0 while it is off). Everywhere in a motion: a strength, a wait (`then when time >= 0.5`), an `until`, an `if` |
| `distance` | inside a find: metres from the player to what it found (its middle) |
| `reach` | inside a find: how far out along the ground what it found is, 0 at the player, 1 at the shape's farthest edge (its points and radius) |

So falloff, ramps and shapes over time are expressions, not new settings:

```
impulse 9 * curve(Falloff, reach) along away   # strong at the centre, weak at the edge
force 40 * curve(Ramp, time / 0.3) along look  # the curve is the ramp-in
impulse 2 * dash.power along up                # a field: a jump that grows with an upgrade
```

`ramp_in` and the like keep working; a curve is the general form. A state machine or a look reads
`time`, `distance` and `reach` as 0, and a curve as 0.

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

### The dash (*built*: `server_mods/dash/client/prefabs/boots.rules`): `use` is `motion Use on use`

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
    impulse 25 along world (0, -1, 0) replace vertical
    then when grounded
    emit brawl.slam.landed
    find Shockwave players                       # an Area3D: a cylinder 5 m around the feet
        impulse 9 along away
        impulse 4.5 along up
        emit brawl.slam.hit                      # the brawl mod: 25 damage
```

### A frag grenade whose blast is predicted (*built*: `server_mods/frag/client/prefabs/frag.rules`)

```
motion Blast on expired                          # its time up: the launch's seconds
    owner item
    emit frag.blast                              # A: who threw it; the frag mod hurts who is near
    break radius frag.break_radius speed frag.push seconds frag.rubble_seconds
    find Blast bodies                            # an Area3D: a ball 8 m round
        impulse (distance < frag.radius) * frag.push * (1 - distance / frag.radius) along away
        impulse (distance < frag.radius) * 0.35 * frag.push * (1 - distance / frag.radius) along up
```

The numbers are fields the frag mod sets from its server options (`frag.radius`, `frag.push`,
`frag.break_radius`, `frag.rubble_seconds`): the rules stay the mod's, the physics is the sheet's.

### The grappling hook, as one block (*built*: `server_mods/grapple/client/prefabs/hook.rules`)

```
motion Throw on use if not linked
    until grapple.hook.used or pressed.jump
    lasts 2
    emit grapple.fired
    cast ray range 40 speed 80
        emit grapple.hooked
        link reel 4 alone
        force 160 top 15 ramp 0.1 on self along to
        move air_control 0.6 gravity 0 airborne 1
```

### A cannon with its muzzle as a marker

```
use if not combat.dead
    cooldown 1.2
    emit cannon.fired
    launch new cannon.ball speed 36 lift 1 seconds 10 from Muzzle along Muzzle
```

### A flamethrower: a cone, ten times a second

```
motion Flame while held.use and flame.fuel > 0
    change flame.fuel -= 25                      # per second, while it is on
    every 0.1                                    # the lines below: now, and every 0.1 s
    find Cone players bodies                     # a convex shape made from a cone mesh
        impulse 0.6 along away
        emit flame.burn                          # the mod: damage over time
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
use        := "use" [ "if" expr ] ...                      (exactly: "motion Use on use" [ "if" expr ] ...)
trigger    := "on use" | "on press" ACTION | "on" EVENT | "while" expr | "when" expr
setting    := "cooldown" num | "uses" num [ "refill" ( "ground" | "after" num ) ]
            | "lasts" num | "until" expr | "item" ( "hand" | "carried" ) | "owner" ( "player" | "item" )
stmt       := once | lasting | wait | flow | query
once       := "change" field op value | "emit" EVENT [ "at" point ]
            | "impulse" value [ "kind" kind ] { option } | "launch" launch-args
            | "break" { option } | "remove self" | "end"
lasting    := "force" value { option } | "move" [ "for" value ] { PARAM value }
            | "link" { option } | "follow" NODE { option }
wait       := "then when" expr [ "within" num ] | "after" num | "timeout"
flow       := "every" num | "if" expr block-body [ "else" block-body ]   (every: the rest of its step repeats)
query      := "find" shape { option } block-body | "cast" shape { option } block-body
shape      := NODE | "ray" | "sphere" num | "box" vec | "capsule" num num
find-opts  := "players" | "bodies" | "self" | "max" num | "turned" ( "facing" | "look" | "none" ) | "at" vec
who        := "self" | "target" | FIELD                              (where each line stands: the table above)
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
2. **Finding** (*built*): `find` (M205: the brawl slam's shockwave is its sheet's now, the mod only hurts), `cast` (M206: the grapple is a sheet now). `distance` and `reach` come with curves.
3. **Curves and markers** (*built*): `curve()`, `time`, `distance`, `reach` (M207); `along NODE`, `from NODE`, `turn`, `follow` (M208).
4. **Items run motions** (*built*, M209): `owner item`, `break`, `remove self`; the frag grenade's blast predicted.
5. **`impulse kind momentum`**, `every` (*built*, M210).
6. **The mods converted** (*built*, M211): brawl, frag, cannon, grapple, flight, dash and the grenade mod are sheets; none uses a motion node; the SDK's starter motion set is a sheet. Nothing was lost (every test as before); the brawl fist's finisher and the dash strike now push what they hit, which their nodes never baked.
7. **Made regular** (*built*, M213): the table of where a line may stand, `on target`, `along world (x, y, z)`, `turned`, `at`, shapes from the feet, `change` in a find, `use` a motion, an item's motion one tick. Found on the way: a motion of one step with no `lasts` was on for no tick, so its `move for` never held (the dash boots' and the dash strike's, once `use` was a motion); it is now on for its longest `move for`, as a last step entered later always was.
