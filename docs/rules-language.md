# Rules as a language

A **motion block** in a `.rules` sheet is a motion the simulation runs: every machine (the server,
every client) runs the same lines on the same input, so what a player does to bodies is predicted
on their own screen and rolled back like movement. A mod's C++ keeps only what the server alone
should decide (damage, score, rounds). This page is the reference: **everything on it is built**;
what is planned is listed [at the end](#planned).

```
a .rules sheet ──bake──> motions/<set>.cfg ──> every simulation runs it (server and clients)
    └── names nodes by path: shapes, markers, paths, curves (drawn in the 3D view)

a server mod (C++) ──> damage, score, rounds, shops, who wins: the server's only, a lead late
```

| In the sheet (every simulation) | In a mod's C++ (the server) | In looks (the viewer) |
|---|---|---|
| what moves a body, starts or ends a motion, finds who is near, changes a movement parameter, launches, breaks a wall, changes the player's fields | damage, health, score, inventories, round timers, anything secret | sounds, particles, shakes, HUD text: reactions and predictions |

See also: [motions.md](motions.md) (what motions are, what conditions read, the baked file),
[looks.md](looks.md#expressions) (the expression language).

## What the language may not do

Every machine runs it, and rollback runs a tick again many times:

| Rule | Why |
|---|---|
| **Deterministic**: no clock, no randomness but the simulation's | the server and every client reach the same result |
| **Bounded**: no loops but `every` (64 times at most); a `find` takes 16 things at most | a tick costs about the same however often it is run again |
| **No storage of its own**: fields, the world, each motion's small record (28 bytes: its step, since when, its ifs' answers) | the record rolls back with the rest |
| **No files, no network, no text** | it runs inside the physics step |

## A motion block

```
motion NAME TRIGGER [if EXPRESSION]
    settings
    lines
```

- **`use [if EXPRESSION]`** is `motion Use on use [if EXPRESSION]`, word for word: an item's use.
  An item has one motion named `Use`; its `CbItem`'s *When used* fields join it.
- A sheet's other blocks (`on`, `while`, `predict`, `show`, `text`, `bind`) are the viewer's
  ([looks.md](looks.md)); the first word keeps the two apart.
- Indenting counts: an `if`'s lines, a `find`'s and a `cast`'s are further in. A tab is four spaces.
- In an **item's sheet** a motion is for whoever has the item out (`item carried`: whoever carries
  it); in a **motion set** (`CbMotionSet`), everyone's.

### Triggers

| Trigger | Starts it |
|---|---|
| `on use` | the item is used (its key, or the use button with it in the hand). An item used while its key is held (`use`: a press, and while held) has it on while it is held. An item's sheet only |
| `on press ACTION` | the action goes down: `jump`, `sprint`, or one a mod declares |
| `on EVENT` | a mod event, or a simulation event, said at the owner this tick (`brawl.stunned`); in an item's sheet `on expired` is its own time running out |
| `while EXPRESSION` | on every tick it holds; off when it stops. No steps |
| `when EXPRESSION` | once, on the tick it turns true (false the tick before; its first answer is only remembered): `when grounded` is a landing |

### Settings

As far in as the motion's first line, not under an `if`, before its first `then` or `after`.

| Setting | Means |
|---|---|
| `cooldown S` | seconds between two starts |
| `uses N`, `uses N refill ground`, `uses N refill after S` | charges |
| `lasts S` | it ends S seconds after it starts |
| `until EXPRESSION` | it ends when this holds (several lines: any) |
| `item hand` / `item carried` | in an item's sheet: whoever has it out (the default), or whoever carries it |
| `owner player` / `owner item` | who runs it: the player (the default), or the item while it lies in the world ([an item's motion](#an-items-motion)) |

### How a block runs

- **Steps.** `then when EXPRESSION` or `after S` starts a new step. The first step is what happens
  when the motion starts; each next one starts once its wait is over (`after` seconds since the step
  before started, then its condition), at the earliest on the tick after.
- **Once and lasting.** A step's once lines (`change`, `emit`, `impulse`, `launch`, `break`,
  `find`, `end`) happen when it starts; its lasting ones (`force`, `move`, `link`, `follow`) act
  while it is the current step (`move for S`: S seconds from its start).
- **`if` is decided when its step starts** and holds for the whole step. A condition asked every
  tick is a `while` motion's.
- **Giving up.** `then when EXPRESSION within S` gives up after S seconds: its `timeout` lines run,
  and the motion ends (without a `timeout`, it just ends).
- **It ends** when its `lasts` run out, its `until` holds, an `end` is reached, a wait gives up, or,
  with neither `lasts` nor `until`, when its last step's longest `move for` is over (one tick at
  least). A motion that waits is on while it waits (`motion.<name>` is true), so a press does not
  start it again.
- **Its effects on other players** land after their own motions, on the same tick, whatever their
  slots (M212); pushes from several players on one add up.

## Where a line may stand

One table: the parser says every refusal from it (`"force" cannot stand in a find: there a line is
change, emit, impulse or move for`).

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
| `move` | yes | | | | yes: while it holds | |
| `force`, `link` | yes | | | | yes | |
| `follow`, `end` | yes | | | | | |
| `remove self` | | | | | | yes (only there) |
| `cast` | the first step, not under an `if`, once a motion | | | | | |
| `every` | as far in as the motion's first line, once a step | | | | | |

Why the gaps: a find's and a cast's lines happen *to* something; `every` and a `timeout` repeat or
end, so nothing in them lasts; a cast belongs to the motion, not to a step.

## Lines

### Once

| Line | Does |
|---|---|
| `change FIELD = VALUE` / `+= VALUE` / `-= VALUE` | a field of the owner (in a find: once for each thing found) |
| `emit EVENT` | a mod event, at the owner: for looks, state machines and a mod's C++. In a find, A is the player and B what it found, at it |
| `impulse VALUE [kind velocity\|momentum] [on WHO] [along DIRECTION] [replace nothing\|vertical\|horizontal\|all]` | a change of velocity (m/s whatever it weighs, to 200), or of momentum (`kind momentum`: N·s, to 100000, shared out by what the target weighs). `replace`: that part of the velocity is cleared first |
| `launch new KIND \| carried [speed S] [lift L] [ahead M] [seconds S] [along DIRECTION] [from NODE]` | an item into the world: a new one of a kind, or the one carried (in a hand first, else the lowest slot's), with the player's own velocity; `seconds` until it is removed (0: it stays); `from` a marker's place instead of `ahead` of where the look starts |
| `break radius R [speed S] [seconds S]` | breakable walls whose nearest piece is within R of the owner lose their pieces that near, flung at S (10), staying S seconds (0: for good) |
| `remove self` | the owner item leaves the world |
| `end` | the motion ends here |

### Lasting

| Line | Does |
|---|---|
| `force VALUE [kind acceleration\|force\|velocity] [top S] [ramp S] [react] [on WHO] [along DIRECTION]` | every tick: m/s² (`acceleration`, the default), newtons (`force`), or toward the speed `top` (`velocity`); `top` caps the other two; `ramp` seconds to full strength; `react`: the other end is pushed back |
| `move [for S] PARAMETER VALUE ...` | the owner's movement parameters (`walk_speed`, `gravity`, `friction`, `air_control`, ... [the list](server-mods.md#movement-parameters)) while it is the step, or for S seconds. In a find, given to the player found for S seconds (a slow, a root); a player holds six such at most |
| `link [length L] [reel S] [alone] [on WHO]` | under a cast: a rope to what it holds (or a field's entity). Length 0: as long as it is when it takes hold; reel: metres a second taken in; `alone`: it moves the player only |
| `follow PATH over S` | carried along a `Path3D` beside the sheet (drawn from its first point, facing −Z), in S seconds; walls still stop it |

### Order

| Line | Does |
|---|---|
| `then when EXPRESSION [within S]` | the next step, once this holds (giving up after S seconds) |
| `after S` | the next step, S seconds after this one started |
| `timeout` | what a `within` that gave up runs, before the motion ends |
| `every S` | the lines after it in its step happen when the step starts and every S seconds while it is the step (64 times at most) |
| `if EXPRESSION` / `else` | the lines further in happen or not, decided when the step starts |

### Finding

| Line | Does |
|---|---|
| `find SHAPE [players] [bodies] [self] [max N] [turned facing\|look\|none] [at (x, y, z)]` | everything the shape overlaps when its step starts; the lines further in run once for each, nearest first, at most N (16). Players and bodies (props, items lying about) unless one is named; the player itself only with `self`; the shape turned as the player faces (the default), as it looks (with the pitch), or not at all |
| `cast SHAPE range R [speed V] [holds level bodies players]` | a line thrown along the look when the motion starts: at once (speed 0: it holds what is there, or the motion does not happen) or flying at V m/s, holding what it meets first. The lines further in act **while it holds**; with a cast, what the first step does to bodies goes under it |

| A `SHAPE` | |
|---|---|
| a node's path (a find) | an `Area3D` (its `CollisionShape3D` children) or one `CollisionShape3D` beside the sheet: a sphere, capsule, box, cylinder or convex shape, drawn as if the player stood at the scene's origin (its feet) facing −Z |
| `sphere R`, `box (X,Y,Z)`, `capsule R H` (a find) | written in the line, around the player's feet |
| `at (x, y, z)` (a find) | moves either: metres right, up and ahead |
| `ray` (a cast) | a line |
| `sphere R` (a cast) | a ball swept along the line |
| a node's path (a cast) | one convex `CollisionShape3D`, drawn around its own origin as the line's tip, forward −Z |

An `Area3D` under an item is not part of the item's body.

### Who and which way

| `on WHO` | |
|---|---|
| `self` | the owner (the default outside a find or a cast) |
| `target` | what the find found, or what the cast holds (the default inside one); only there. `found` and `hit` are older words for it |
| `FIELD` | the entity a field of the player names |

| `along DIRECTION` | |
|---|---|
| `look`, `move`, `facing`, `up` | where the camera looks; the move input (the facing with none); the body's facing; up |
| `world (x, y, z)` | as given, in the world (`world direction (x, y, z)`, the older spelling) |
| `to` | toward the target (what the cast holds, or a field's entity) |
| `away` | in a find: from the player out to what it found, along the ground |
| `NODE` | a `Marker3D`'s (any `Node3D`'s) forward (−Z) as drawn, turned as the look is |
| then `turn (yaw, pitch)` | turned by degrees, right and up |

## An item's motion

`owner item` in an item's sheet: the item runs it, where it lies in the world, **in one tick**, when
its event is said at it (`on expired`: its time is up, before it goes; `on EVENT`: a mod's, said at
it). So: no `then`, `after` or `timeout`, no settings but `owner`, no `if` on its first line (an
`if` block is fine), and only what [the table's](#where-a-line-may-stand) last column allows. It has
no look or keys, so it pushes along `up`, `world`, `away` or `to`. Its events are said as its
thrower's (A the thrower, B the item); its expressions read its fields, the game's, and `speed`.

## Numbers

**Every amount may be an [expression](looks.md#expressions)**: the words up to the line's next
option word (for a `move`, up to the next parameter's name), or a word in brackets, read when its
line acts, within the same limits as a number.

| Read | Amounts |
|---|---|
| when its step starts | an `impulse`'s; a `change`'s value; a `launch`'s speed, lift, ahead and seconds; a `break`'s radius, speed and seconds; a `move`'s values (held as they were while the move holds) |
| for each thing found | in a find: an `impulse`'s, a `change`'s, a `move for`'s values |
| every tick | a `force`'s value and its `top`; a `link`'s length and reel |

**Numbers, always**: the motion's own times (`cooldown`, `lasts`, `after`, `within`, `every`, `move
for`'s seconds, `follow ... over`, a force's `ramp`), counts (`uses`, `max`), directions and places
(`world (x, y, z)`, `turn`, `at`), and a cast's `range` and `speed`. They shape the motion, or are
needed before anything is read.

What a motion's expressions read, beyond what [conditions read](motions.md#cbmotion-the-trigger):

| Word | |
|---|---|
| `curve(Name, x)` | the `CbCurve` named `Name` beside the sheet, read at x (baked into 64 values, read between two in a straight line, held past its ends) |
| `time` | seconds since the current step started (0 while off) |
| `distance` | in a find: metres from the player to what it found |
| `reach` | in a find: how far out along the ground it is, 0 at the player, 1 at the shape's edge |

```
impulse 9 * curve(Falloff, reach) along away   # strong at the centre, weak at the edge
force 40 * curve(Ramp, time / 0.3) along look  # a ramp drawn as a curve
move walk_speed 4 + slow.bonus friction 0      # read when the step starts
change brawl.combo += 1 + brawl.streak
```

| Physics word | Is | Unit | Weight matters |
|---|---|---|---|
| `impulse` (`kind velocity`) | a change of velocity, once | m/s | no: a crate and a truck both get 9 m/s |
| `impulse ... kind momentum` | a change of momentum, once | N·s | yes: 900 N·s is 9 m/s to 100 kg |
| `force` (`kind acceleration`) | a change of velocity per second | m/s² | no |
| `force ... kind force` | a force | N | yes |

## Limits

| Limit | |
|---|---|
| things a find takes | 16 |
| times an `every` repeats | 64 |
| `if`s a step decides | 16 |
| steps a motion has | 64 |
| a step's `move` values from expressions | 16 |
| moves a player is given at once | 6 (the oldest goes) |
| casts a motion has | 1 |

## Seeing what a motion does

The debug HUD (F1) lists your motions: each that is on, its step and what it waits for, how long it
has been on and has left, what its cast holds; and the last one you asked for that did not start,
and why (its cooldown, its uses, its `if` as written, it is on already, its cast found nothing, you
may not act). A recording shows the followed player's.

## Examples (the shipped sheets)

The ground slam (`server_mods/brawl/client/prefabs/slam.rules`): a dive, and on landing everyone
in the drawn `Shockwave` cylinder thrown away and up; the brawl mod only hurts them.

```
motion Slam on use if brawl.cd_slam <= 0 and not grounded
	lasts 3
	change brawl.cd_slam = 5
	emit brawl.slam.started
	impulse 25 along world (0, -1, 0) replace vertical
	then when grounded
	emit brawl.slam.landed
	find Shockwave players
		impulse 9 along away
		impulse 4.5 along up
		emit brawl.slam.hit
```

The frag grenade (`server_mods/frag/client/prefabs/frag.rules`): thrown on the arm's marker, it
goes off where it lies when its time is up, on the same tick everywhere; its numbers are the mod's
options, published as fields.

```
motion Release on frag.release
	item carried
	emit frag.thrown
	launch carried speed 15 lift 3 ahead 0.8 seconds 2.5

motion Blast on expired
	owner item
	emit frag.blast
	break radius frag.break_radius speed frag.push seconds frag.rubble_seconds
	find Blast bodies
		impulse (distance < frag.radius) * frag.push * (1 - distance / frag.radius) along away
		impulse (distance < frag.radius) * 0.35 * frag.push * (1 - distance / frag.radius) along up
```

The grappling hook (`server_mods/grapple/client/prefabs/hook.rules`):

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

The dash boots (`server_mods/dash/client/prefabs/boots.rules`) and flight's jetpack
(`server_mods/flight/client/motion_sets/flight_moves.rules`, a while motion):

```
use if dash.charges > 0
    change dash.charges -= 1
    cooldown 0.4
    emit dash.started
    impulse 11 replace horizontal
    move for 0.18 friction 0

motion Thrust while flight.jetpack_on and held.jump and not grounded and not flight.on and flight.fuel > 0
	change flight.fuel -= 30          # per second, in a while motion
	emit flight.thrust
	force 26 along up
```

## Grammar, in short

```
block      := motion | use
motion     := "motion" NAME trigger [ "if" expr ] NEWLINE INDENT { setting } { line } DEDENT
use        := "use" [ "if" expr ] ...                          (exactly "motion Use on use" ...)
trigger    := "on use" | "on press" ACTION | "on" EVENT | "while" expr | "when" expr
setting    := "cooldown" num | "uses" num [ "refill" ( "ground" | "after" num ) ] | "lasts" num
            | "until" expr | "item" ( "hand" | "carried" ) | "owner" ( "player" | "item" )
line       := once | lasting | order | finding          (where each may stand: the table above)
once       := "change" FIELD op amount | "emit" EVENT | "impulse" amount { option }
            | "launch" ( "new" KIND | "carried" ) { launch-option } | "break" { break-option }
            | "remove self" | "end"
lasting    := "force" amount { option } | "move" [ "for" num ] { PARAM amount }
            | "link" { link-option } | "follow" NODE "over" num
order      := "then when" expr [ "within" num ] | "after" num | "timeout" | "every" num
            | "if" expr ... [ "else" ... ]
finding    := "find" shape { find-option } ... | "cast" shape "range" num [ "speed" num ] [ "holds" ... ] ...
amount     := a number, or an expression up to the next option word
option     := "on" who | "along" direction [ "turn" vec2 ] | "replace" ... | "kind" ... | "top" amount
            | "ramp" num | "react"
who        := "self" | "target" | FIELD
direction  := "look" | "move" | "facing" | "up" | "to" | "away" | "world" vec | NODE
```

## The nodes

The motion nodes (`CbMotion`, `CbImpulse`, `CbForce`, `CbMove`, `CbLink`, `CbLaunch`, `CbProbe`)
bake to the same file and are kept as they are, not maintained, until they are reworked with the
other authoring nodes. No shipped mod uses them. What a sheet names stays a node: `CollisionShape3D`
and `Area3D` (shapes), `Marker3D` (arrows and places), `Path3D` (paths), `CbCurve` (curves).

## Planned

| What | |
|---|---|
| `$target:FIELD` | the target's own fields, read in a find's or a cast's lines |
| `at POINT` | a find or an emit around a point other than the owner (`anchor`, where the cast hit, a marker) |
| `PATH at T` | a direction along a `Path3D` |
| A cast in a later step | today a cast is thrown when the motion starts |
| The player's body | `wall` (a condition and a direction), `teleport`, passing through, not hittable, the capsule's size, keep speed and steer, hidden from others ([ROADMAP.md](../ROADMAP.md#parked)) |

| Open question | Leaning |
|---|---|
| Two slows on one player: the stronger, the newer, or multiplied? | the newer wins per parameter (today's); a `multiply` to stack |
| May a sheet change another player's fields? | not at first: health is a mod's rule (`emit`, then the mod's C++) |
| `owner item` for a map's prop (a mine, a door)? | later |
| A shape that follows a bone (a sword's arc)? | later: a cast along a pose marker over the swing's ticks |

## How it came to be

| Milestone | |
|---|---|
| M204 | blocks: steps, `if` / `else`, `end`, `when` |
| M205, M206 | `find`; `cast` (the grapple a sheet) |
| M207, M208 | curves, `time`, `distance`, `reach`; markers, `turn`, `follow` |
| M209, M210 | an item's motion, `break`, `remove self`; `kind momentum`, `every` |
| M211 | every mod's motions are sheets |
| M212 | effects on other players do not depend on the slots |
| M213 | made regular: where a line may stand, `target`, `along world (x, y, z)`, `turned`, `at`, shapes from the feet, `change` in a find, `use` a motion; a motion of one step on for its longest `move for` |
| M214 | every amount an expression |
| M215 | this reference, rewritten from what is built; the motion lines in the debug HUD |
