# Sheets

A **sheet** (a `.cbsheet` file, a `CbSheet` resource a `CbRules` node holds) is an item's, a motion
set's, a world's or a HUD's logic as lines of text. Its **motion blocks** are motions the
simulation runs: every machine (the server, every client) runs the same lines on the same input, so
what a player does to bodies is predicted on their own screen and rolled back like movement. Its
**reactions, predictions and HUD lines** are the viewer's: sounds, effects, a guess of what the
server will say, a label. A mod's C++ keeps only what the server alone should decide (damage,
score, rounds). This page is the reference: **everything on it is built**; what is planned is listed
[at the end](#planned), and where the language goes next is [step6-plan.md](step6-plan.md).

```
a .cbsheet sheet ──bake──> motions/<set>.cfg ──> every simulation runs it (server and clients)
    │   └── names nodes by path: shapes, markers, paths, curves (drawn in the 3D view)
    └── reactions, predictions, HUD lines ──> the viewer's nodes (CbReaction, CbPrediction, CbFieldBinding)

a server mod (C++) ──> damage, score, rounds, shops, who wins: the server's only, a lead late
```

| In the sheet's motions (every simulation) | In a mod's C++ (the server) | In the sheet's reactions (the viewer) |
|---|---|---|
| what moves a body, starts or ends a motion, finds who is near, changes a movement parameter, launches, breaks a wall, changes the player's fields | damage, health, score, inventories, round timers, anything secret | sounds, particles, shakes, HUD text: reactions and predictions |

See also: [motions.md](motions.md) (what motions are, what conditions read, the baked file),
[looks.md](looks.md#expressions) (the expression language), [looks.md](looks.md#rules-sheets)
(the editor's Sheet screen).

## A sheet

```
cbsheet 1                                   # the version: the first line

# The pistol: its trigger (every simulation) and how it looks (the viewer).
motion Fire on use if gun.ammo > 0 and not gun.reloading
	cooldown 0.2
	change gun.ammo -= 1
	emit item.use

react on item.use
	as holder
		create muzzle_flash under Muzzle for 0.3
		at Muzzle
			sound gunshot volume -5 jitter 0.06 range 70

predict use2 as item.use2 if !gun.reloading
	cooldown 0.95

hud Line
	text "[{key:slot2}]  DASH STRIKE  READY" if brawl.cd_dash <= 0
```

| Block | Runs | |
|---|---|---|
| `motion NAME TRIGGER [if EXPRESSION]` | **every simulation** | [below](#a-motion-block) |
| `use [if EXPRESSION]` | the same: `motion Use on use [if EXPRESSION]` | an item's use |
| `react on EVENT [if EXPRESSION]`, `react while EXPRESSION` | **the viewer**: effects, sounds, properties, shakes | [Reactions](#reactions) |
| `predict ACTION as EVENT [if EXPRESSION]` | the viewer: its own press shown at once, as the cue the server will answer with | [Predictions](#predictions) |
| `hud PATH` | the viewer: one control's lines | [HUD blocks](#hud-blocks) |

- **One rule for scope: a line governs what is indented under it.** A block's lines are under it; an
  `if`'s, a `find`'s, a `cast`'s, a reaction's `as` and `at` are further in. A tab is four columns;
  `cb_sheet fmt` writes tabs.
- **`#` starts a comment**, to the line's end. Blank lines mean nothing.
- **A mistake is said with its line**: in the editor as you type, and when the look is published (a
  name no mod declares too). Its rule is left out; the rest of the sheet still works.
- **Version 0**, the spelling before M285 (no first line, `on`/`while` rules whose settings hold for
  the actions below them, `then when`, `subject ^^`, ...), is still read, until nothing ships it.
  [Version 0's words and their version 1 spelling](#version-0).

### Declarations

A sheet declares the names it brings, one a line, at the start of a line (M286):

```
field gun.ammo int                        # a field of the player (the default), sent to its player
field team.id int shared                  # ... sent to every player
field shop.coins int private              # ... its player's alone, never shown on others
field flight.jetpack_on bool on world     # the game's, one for everyone
field gun.rounds int on item = 12         # a field of each copy of the item; a new copy starts so
event brawl.jab.hit                       # an event (at a character: heard by who sees it)
event deathmatch.kill everyone            # ... heard by every player
action reload key R                       # a key a player presses (R by default)
param gun.damage 25                       # a number of the item whose scene holds the sheet
param gun.reload_seconds 1.5 s
```

| Line | Is |
|---|---|
| `field NAME int\|float\|bool\|timer [on player\|world\|item] [shared\|private] [= VALUE]` | (`timer`, M287: a float that counts down to 0 by itself, a second a second, in every simulation: a player's, the world's, or an item's (while it is carried or lies in the world); `change brawl.cd_dash = 3` and it is a cooldown the HUD shows. `on item`: **in the sheet's motions the name is the item's own field**, read in their conditions and changed by their lines, a find's too: the copy in the hand, or the first carried for a motion `for carrier`; a look reads the one in the hand as `hand.FIELD`. An item's field may be `shared`: every player is sent it, the bat's heat.) | a field. **The server declares it for the mod whose item has the sheet** (after the mod's C++: the C++'s `declare.Field` finds it; one declared with another type is a mistake when the server starts, with the sheet's path and line). `on item`: each copy's own, baked into `items/<kind>.cfg` (was the `CbItem`'s `fields`) |
| `event NAME [everyone]` | an event, the same way |
| `action NAME [key KEY]` | an action, its key as Godot names keys (`R`, `F`, `1`, `MouseLeft`) |
| `param NAME NUMBER [UNIT]` | a number of the item whose scene holds the sheet: baked into `items/<kind>.cfg` as its property, which server mods read (`ctx.ItemProperty`; was the `CbItem`'s `properties`). **In the sheet it is its number**: `gun.ammo < gun.magazine` reads `gun.ammo < 12` |

- **The editor knows a sheet's own names** as you type, with no server build; the names file
  (`cinderbox_names.cfg`, the server's build) brings the rest, the sheets' of every mod among them.
- A motion set's sheet (`motion_sets/`) is not in the mod's item (what it bakes to is): its names
  are declared in a sheet that is (the flight mod's in `vfx/reactions_flight.cbsheet`).
- **Units** may follow any number: `s`, `m`, `m/s`, `deg`. They are checked against the word they
  are for (`cooldown 0.4 m` is a mistake: cooldown takes a time) and dropped; a number without one
  means what it always did.

### One sheet, many items

Every gun's trigger, rounds and reload are one sheet, `prefabs/gun.cbsheet` (M289); each gun's sheet
uses it and says what is its own:

```
cbsheet 1
uses res://prefabs/gun.cbsheet            # its declarations, lets and blocks, as if written here

field gun.rounds int on item = 30         # a declaration of the same name: this one instead
param gun.damage 14

replace motion Fire when using.gun.rifle and gun.rounds > 0 and not gun.reloading
	cooldown 0.1                          # a block of the same head (a motion's: its name): this one
	...
```

- **`uses PATH`** (a `res://` path): the used sheet's lines are this one's, in its order; this sheet's
  declarations of the same names (`param`, `field`, `event`, `action`, `status`, `let`) take their
  places, its `replace` blocks the blocks of the same head (a motion by its name, any other block by
  its first line), and its other blocks come after. A block of the same head without `replace` is a
  mistake. A sheet uses one at most, which uses none.
- **A mistake in the used sheet** is said at the uses line: `line 2: in res://prefabs/gun.cbsheet
  line 31: ...`.
- **Found by its path** wherever a sheet is read: the project's file in the editor and the game (a
  pack carries both: `*.cbsheet`), the mod's item on the server, the project's folder for `cb_sheet`.
- **`let NAME = WORDS`**: NAME stands for the words wherever it is a word of a line (not in quotes or
  a comment). The fist's box is written once: `let hitbox = box (0.8, 1.2, 2.6) at (0, 1, 1.4) turned
  look max 1`, then `find hitbox`. Words that are an expression: put them in brackets yourself.

### Statuses

A **status** is a timed flag on a player (M288): `status stunned` declares it (in any sheet, or a
mod's `declare.Status`), `apply stunned for 0.8` in a motion gives it (on its owner, or in a find on
each player found), and a server mod gives it with `ctx.ApplyStatus( target, stunned, 0.8f )` (the
sword's parry). A motion reads it (`stunned`, `remaining stunned`), is `blocked by stunned`, or runs
`while stunned` (the sword's `Reeling`: slowed, no jump). It is the player's, in every simulation:
predicted when a motion applies it, and the server's says it again.

### Words in expressions

The same [expression language](looks.md#expressions) everywhere: a condition, a value, a HUD line.

| Write | Means |
|---|---|
| `not a` / `!a`, `a and b` / `a && b`, `a or b` / `a \|\| b` | logic: both spellings |
| `known gun.ammo` | its mod runs (the server declared it) |
| `combat.health of target`, `combat.dead of holder` | another entity's field: `of me`, `holder`, `actor`, `target`, `local`, `world` |
| `local` | the viewer's own player is the subject (looks only) |
| `active Slam`, `active brawl.fist.Jab` | a motion is on now (its own sheet's by name, another item's by its kind) |
| `stunned`, `remaining stunned` | in a motion (M288): the player has the status now; the seconds it has left |
| `player`, `combat.health of target`, `stunned of target` | in a find's (or a grab's, a cast's) lines (M290): what it found is a player; its field; its status |
| `ready Fire`, `cooldown Fire`, `uses Fire` | in a motion (M287): the motion could start now (not on, off cooldown, a use left); the seconds of cooldown it has left; its uses left (0 without a limit). A HUD does not read them yet: a timer field is what it shows |
| everything else | numbers, fields, `+ - * /`, comparisons, `( )`, `grounded`, `speed`, `held.jump`, `pressed.dash`, `using.gun.rifle`, `carried.KIND`, `hand.FIELD`, `event.value`, `time`, `distance`, `reach` |

A HUD line's format (`"{brawl.cd_dash}s"`) is read as written: inside quotes, version 0's signs
(`?name`, `$other:name`) for now.

**The compiler** (M283): `src/sheet`, plain C++ with no Godot types: one parser of every block of a
sheet (`sheet_rules.h`; version 1 read by lowering it to version 0, line for line: `sheet_v1.h`)
and the bake of its motion blocks (`sheet_bake.h`, the scene it is in answering for the nodes a
line names). The editor, the bake and the names check use it; the server, the cooker and the tests
can. Every shipped sheet's parse is a golden file (`tests/golden/sheets`, the `sheets_golden` test),
and every mod's committed bakes are checked to be what its sheets bake to now (the Godot checks).

**The file** (M284): a sheet is a `.cbsheet` file, a `CbSheet` resource (the extension's own loader
and saver: the file is the text, whole), held by a `CbRules` node in the scene (`sheet`). It opens in
the editor's Sheet screen ([looks.md](looks.md#rules-sheets)).

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
- A sheet's other blocks (`react`, `predict`, `hud`) are the viewer's; the first word keeps the
  two apart.
- Indenting counts: an `if`'s lines, a `find`'s and a `cast`'s are further in. A tab is four spaces.
- In an **item's sheet** a motion is for whoever has the item out (`for carrier`: whoever carries
  it); in a **motion set** (`CbMotionSet`), everyone's.

### Triggers

| Trigger | Starts it |
|---|---|
| `on release ACTION` | the action comes up (M288) |
| `on tap ACTION [S]` | it comes up within S seconds (0.25) of going down: a short press |
| `on hold ACTION [S]` | it is still down S seconds (0.25) after it went down: on that tick, a long press (the first 16 actions a server declares) |
| `on use` | the item is used (its key, or the use button with it in the hand). An item used while its key is held (`use`: a press, and while held) has it on while it is held. An item's sheet only |
| `on press ACTION` | the action goes down: `jump`, `sprint`, or one a mod declares |
| `on EVENT` | a mod event, or a simulation event, said at the owner this tick (`brawl.stunned`); in an item's sheet `on expired` is its own time running out |
| `while EXPRESSION` | on every tick it holds; off when it stops. No steps |
| `when EXPRESSION` | once, on the tick it turns true (false the tick before; its first answer is only remembered): `when grounded` is a landing |

### Settings

As far in as the motion's first line, not under an `if`, before its first `wait`.

| Setting | Means |
|---|---|
| `cooldown S` | seconds between two starts |
| `uses N`, `uses N refill ground`, `uses N refill after S` | charges |
| `lasts S` | it ends S seconds after it starts |
| `until EXPRESSION` | it ends when this holds (several lines: any) |
| `cooldown S shared NAME` | its cooldown, and every motion that names the same shares it: one starting starts the others' (M288) |
| `tags NAME, ...` | what kind of thing it is, for others to block or cancel (`attack`, `guard`, `punch`) |
| `blocked by NAME, ...` | it does not start while a motion of one of these tags is on, or a motion of that name, or the player has that status (`blocked by guard, stunned`) |
| `cancels NAME, ...` | starting, it ends the motions of these tags (or names) that are on (a parry ends an attack) |
| `for holder` / `for carrier` | in an item's sheet: whoever has it out (the default), or whoever carries it |
| `on item` | the item runs it, while it lies in the world ([an item's motion](#an-items-motion)); without it the player does |

### How a block runs

- **Steps.** `wait for EXPRESSION` or `wait S` starts a new step. The first step is what happens
  when the motion starts; each next one starts once its wait is over (S seconds since the step
  before started, or its condition), at the earliest on the tick after.
- **Once and lasting.** A step's once lines (`change`, `emit`, `impulse`, `launch`, `break`,
  `find`, `end`) happen when it starts; its lasting ones (`force`, `move`, `link`, `follow`) act
  while it is the current step (`move for S`: S seconds from its start).
- **`if` is decided when its step starts** and holds for the whole step. A condition asked every
  tick is a `while` motion's.
- **Waiting for an animation's marker.** A marker's event is said after the motions have run in
  its tick, so a motion hears it the tick after, in a trigger (`on EVENT`), a wait (`wait for EVENT`)
  or an `if` alike (before M240 a wait or an `if` never heard one). The fist waits for its punch's
  `brawl.fist.strike` (the hit) and then `brawl.fist.ready` (the next punch may start).
- **Giving up.** `wait for EXPRESSION within S` gives up after S seconds: its `timeout` lines run,
  and the motion ends (without a `timeout`, it just ends).
- **Or after a while.** `wait for EXPRESSION or S`: the step after it starts once it holds, or after S
  seconds without it (a marker an animation may not have), and the motion goes on (M287); anywhere
  in a motion
  (it is a `within` whose `timeout` repeats the lines: the frag's and the grenade's throws).
- **It ends** when its `lasts` run out, its `until` holds, an `end` is reached, a wait gives up, or,
  with neither `lasts` nor `until`, when its last step's longest `move for` is over (one tick at
  least). A motion that waits is on while it waits (`active <name>` is true), so a press does not
  start it again.
- **Its effects on other players** land after their own motions, on the same tick, whatever their
  slots (M212); pushes from several players on one add up.

## Where a line may stand

One table: the parser says every refusal from it (`"force" cannot stand in a find: there a line is
change, emit, impulse, move for or apply`).

| Line | In a step | After `every` | In a `timeout` | In a `find` | In a `grab` (M290) | In a `cast` | In an item's motion |
|---|---|---|---|---|---|---|---|
| `change` | yes | yes | yes | yes: the owner's field, once for each thing found | yes: once for each, when the step starts | | yes |
| `apply` | yes | yes | yes | yes: on each player found | yes | yes: on the player it holds | |
| `emit` | yes | yes | yes | yes: with what it found | yes | one, when it takes hold | yes |
| `impulse` | yes | yes | yes | yes: on what it found | yes: when the step starts | yes: on what it holds | yes |
| `throw` | | | | yes | yes | yes | |
| `launch` | yes | yes | yes | | | | |
| `break` | yes | yes | yes | | | | yes |
| `find` | yes | yes | yes | | | | yes |
| `grab` | yes | | | | | | |
| `if` / `else` | yes | yes | yes | yes: for each thing found | yes | | yes |
| `move for S` | yes | | | yes: given to a player found | yes | yes | |
| `move` | yes | | | | | yes: while it holds | |
| `force` | yes | | | | yes: every tick, on all it holds | yes | |
| `link` | yes | | | | | yes | |
| `follow`, `end` | yes | | | | | | |
| `remove self` | | | | | | | yes (only there) |
| `cast` | the first step, not under an `if`, once a motion | | | | | | |
| `every` | as far in as the motion's first line, once a step | | | | | | |

Why the gaps: a find's and a cast's lines happen *to* something; `every` and a `timeout` repeat or
end, so nothing in them lasts; a cast belongs to the motion, not to a step.

## Lines

### Once

| Line | Does |
|---|---|
| `change FIELD = VALUE` / `+= VALUE` / `-= VALUE` | a field of the owner (in a find: once for each thing found) |
| `emit EVENT [value VALUE]` | a mod event, at the owner: for looks, state machines and a mod's C++ (`event.value` in a look; M287: the value, a number or an expression, read when it happens; not yet in a find). In a find, A is the player and B what it found, at it |
| `impulse VALUE [kind velocity\|momentum] [on WHO] [along DIRECTION] [replace nothing\|vertical\|horizontal\|all]` | a change of velocity (m/s whatever it weighs, to 200), or of momentum (`kind momentum`: N·s, to 100000, shared out by what the target weighs). `replace`: that part of the velocity is cleared first |
| `launch new KIND \| carried [speed S] [lift L] [ahead M] [seconds S] [along DIRECTION] [from NODE]` | an item into the world: a new one of a kind, or the one carried (in a hand first, else the lowest slot's), with the player's own velocity; `seconds` until it is removed (0: it stays); `from` a marker's place instead of `ahead` of where the look starts |
| `break radius R [speed S] [seconds S] [at POINT]` | breakable walls whose nearest piece is within R of the owner (or of the point, M290: `look D`, `hit`) lose their pieces that near, flung at S (10), staying S seconds (0: for good) |
| `remove self` | the owner item leaves the world |
| `apply STATUS for S [on target]` | a status on the owner for S seconds (in a find: on each player it found; in a cast: on the player it holds, when it takes hold, M290). The same status again: the longer stands; a player has 8 at most (a new one takes the place of the one that ends first) (M288) |
| `throw VALUE [along DIRECTION] marks EVENT for S` | in a find, a grab or a cast (M290): an impulse that marks the body it throws as the owner's for S seconds: the first other player it strikes in that time has EVENT said (A the thrower, B the one struck, its value the speed in m/s, at the body). Once a body strikes, its mark is spent; a player has 8 marks out at most |
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
| `wait for EXPRESSION [within S]` | the next step, once this holds (giving up after S seconds) |
| `wait for EXPRESSION or S` | the next step, once this holds or after S seconds without it: it goes on either way |
| `wait S` | the next step, S seconds after this one started |
| `timeout` | what a `within` that gave up runs, before the motion ends |
| `every S` | the lines after it in its step happen when the step starts and every S seconds while it is the step (64 times at most) |
| `if EXPRESSION` / `else` | the lines further in happen or not, decided when the step starts |

### Finding

| Line | Does |
|---|---|
| `find SHAPE [players] [bodies] [self] [max N] [turned facing\|look\|none] [at POINT] [players first]` | everything the shape overlaps when its step starts; the lines further in run once for each, nearest first, at most N (16). Players and bodies (props, items lying about) unless one is named; the player itself only with `self`; the shape turned as the player faces (the default), as it looks (with the pitch, about the chest: 1 m up, M279), or not at all. `players first` (M290): every player before any body, whatever is nearer (the bat: one find, a player never missed for a ball lying nearer) |
| `grab SHAPE [players] [bodies] [max N] [turned ...] [at POINT] [players first]` | (M290) a find whose catch is **held** while its step lasts (8 at most): its once lines (impulses, throws, emits, changes, applies, `move for`) on each when the step starts, its forces on everything it holds **every tick**. It takes in what comes into its shape while it holds fewer than it may; what is further from the shape's middle than twice its size is let go, and so is what the player threw (a mark). In a step, not after an `every` |
| `if EXPRESSION` / `else` under a find or a grab | (M290) decided **for each thing found**, not when the step starts: `if player`, `if combat.health of target < 30` |
| `cast SHAPE range R [speed V] [holds level bodies players] [along look\|facing]` | a line thrown along the look (`along facing`: level along the body's facing, whatever the camera does: a punch's) when the motion starts: at once (speed 0: it holds what is there, or the motion does not happen) or flying at V m/s, holding what it meets first. The lines further in act **while it holds**; with a cast, what the first step does to bodies goes under it |

| A `SHAPE` | |
|---|---|
| a node's path (a find) | an `Area3D` (its `CollisionShape3D` children) or one `CollisionShape3D` beside the sheet: a sphere, capsule, box, cylinder or convex shape, drawn as if the player stood at the scene's origin (its feet) facing −Z |
| `sphere R`, `box (X,Y,Z)`, `capsule R H` (a find) | written in the line, around the player's feet |
| `at (x, y, z)` (a find) | moves either: metres right, up and ahead |
| `at NODE` (a find, M290) | a `Marker3D` beside the sheet: its place, as `at (x, y, z)` |
| `at look D` (a find, a grab, a break; M290) | around a point D metres along the look from the chest, stopped a little short of the level (anything static): a point the player steers. D a number, a param or an expression |
| `at hit` (M290) | around where the motion's cast holds (holding nothing: the line does nothing) |
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
| `toward owner`, `toward look D`, `toward hit` (M290, in place of `along`) | from the target to the owner's chest (a pull), to a point along the look, to where the cast holds. A force of `kind velocity` toward a point brings the target's whole velocity to its speed toward it, no faster than it can still stop there: what it pulls arrives and stays |
| `NODE` | a `Marker3D`'s (any `Node3D`'s) forward (−Z) as drawn, turned as the look is |
| then `turn (yaw, pitch)` | turned by degrees, right and up |

## An item's motion

`on item` in an item's sheet: the item runs it, where it lies in the world, **in one tick**, when
its event is said at it (`on expired`: its time is up, before it goes; `on EVENT`: a mod's, said at
it). So: no `wait` or `timeout`, no settings but `on item`, no `if` on its first line (an
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

**Numbers, always**: the motion's own times (`cooldown`, `lasts`, `wait`, `within`, `every`, `move
for`'s seconds, `follow ... over`, a force's `ramp`), counts (`uses`, `max`), directions and places
(`world (x, y, z)`, `turn`, `at`), and a cast's `range` and `speed`. They shape the motion, or are
needed before anything is read.

What a motion's expressions read, beyond what [conditions read](motions.md#when-how-long-what-changes):

| Word | |
|---|---|
| `curve(Name, x)` | refused in a sheet since M268: curves were drawn in `CbCurve` nodes beside it, deleted with the motion nodes (no shipped sheet used one). The simulation still reads a motions file's curves; curves as resources are [postponed](../ROADMAP.md) |
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

The ground slam (`server_mods/brawl/client/prefabs/slam.cbsheet`): a dive, and on landing everyone
in the drawn `Shockwave` cylinder thrown away and up; the brawl mod only hurts them.

```
motion Slam on use if brawl.cd_slam <= 0 and not grounded
	lasts 3
	change brawl.cd_slam = 5
	emit brawl.slam.started
	impulse 25 along world (0, -1, 0) replace vertical
	wait for grounded
	emit brawl.slam.landed
	find Shockwave players
		impulse 9 along away
		impulse 4.5 along up
		emit brawl.slam.hit
```

The frag grenade (`server_mods/frag/client/prefabs/frag.cbsheet`): thrown on the arm's marker (or a
moment later on a body the arm's pack does not fit), it
goes off where it lies when its time is up, on the same tick everywhere; its numbers are the mod's
options, published as fields.

```
motion Throwing on use
	for carrier
	cooldown 0.7
	lasts 0.7
	wait for frag.release or 0.3
	emit frag.thrown
	launch carried speed 15 lift 3 ahead 0.8 seconds 2.5

motion Blast on expired
	on item
	emit frag.blast
	break radius frag.break_radius speed frag.push seconds frag.rubble_seconds
	find Blast bodies
		impulse (distance < frag.radius) * frag.push * (1 - distance / frag.radius) along away
		impulse (distance < frag.radius) * 0.35 * frag.push * (1 - distance / frag.radius) along up
```

The grappling hook (`server_mods/grapple/client/prefabs/hook.cbsheet`):

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

The dash boots (`server_mods/dash/client/prefabs/boots.cbsheet`) and flight's jetpack
(`server_mods/flight/client/motion_sets/flight_moves.cbsheet`, a while motion):

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

## Reactions

```
react on item.hit if event.value > 0 and local      # when: on an event (its "if" read then), or while
	cooldown 0.1                                     # settings of the block: first
	server only
	as holder                                        # scopes: for the lines under them
		at point
			sound body_hit volume -8 jitter 0.15 range 40
			create hit_puff for 1
		at holder/Head
			after 0.1
				sound hit_marker volume -6
```

| Line | Means |
|---|---|
| `react on EVENT [if EXPRESSION]` | on the cue; its condition read then, about its lines' subject |
| `react while EXPRESSION` | while it holds (a set property goes back when it stops) |
| `cooldown S` | a setting: not more often than every S seconds |
| `server only` | a setting: on the server's cue only, not on a press of the viewer's that is guessed |
| `as holder` / `actor` / `target` / `me` / `local` / `world` / PATH | a scope: whose the lines are (whose fields the condition reads, who the cue must name). An item's holder, the cue's two (world reactions), the sheet's own entity, the viewer's player, the world. With none: `me` |
| `at point` / `end` / `beam from PATH` / `follow` / PATH `[offset X Y Z]` | a scope: where what is created and heard goes: the cue's point or end (where a shot ended), a beam from a node to the end, following the subject, at a node. With none: where the `CbRules` node is |
| `after S` | a scope: S seconds later |
| `chance P` | a scope: only so often (0 to 1) |
| `if actor is prop`, `if actor is bouncy_ball` (first in the `if`) | only when the subject is this kind of entity (`player`, `prop`, `static`, `ragdoll`, `item`) or comes from this template; its lines must be `as` that subject |

| Action | Does |
|---|---|
| `sound NAME [volume DB] [pitch P] [jitter J] [range M] [bus NAME] [volume_by EXPRESSION]` | a sound: `NAME` is `assets/sfx/NAME.wav` of the sheet's project, or a full `res://` path |
| `create NAME [under PATH] [for S]` | an effect: `vfx/NAME.tscn` of the sheet's project (or a full path), riding `under` a node, removed after S seconds |
| `play PATH ANIMATION [off ANIMATION]` | an `AnimationPlayer` plays it (and the other when a `while` stops) |
| `set PATH PROPERTY VALUE [blend S]`, `set PATH PROPERTY = EXPRESSION` | a property: a number, `true`, `false`, `rgb(r, g, b)`, `rgba(r, g, b, a)`, `(x, y, z)`, a word or `"text"`; or the expression's value |
| `call PATH METHOD [ARGUMENT ...]` | a method of a node |
| `shake AMOUNT [for S]` | the camera shakes (0 to 1) |
| `flash rgba(r, g, b, a) [for S]` | the screen flashes |

**Paths** are from the node the `CbRules` node is under (`Barrel`, `Model/Sparks`; `.` is that
node), and climb no higher than the scene; or from an anchor: `holder/RightHand`, `actor/Hip`,
`target/Head`, `local@pickup.target` (the entity a field names). Every action becomes one
`CbReaction` node under the `CbRules` node (never saved with the scene), so the director, the guard
and the Cue Preview see nothing new.

## Predictions

```
predict use as gun.shot if gun.ammo > 0 and not gun.reloading
	cooldown 0.19
	held                      # again and again while held, every cooldown: automatic fire
	change gun.ammo -= 1      # until the server answers; one line each
	stance melee_swing on full
```

A prediction is the viewer's own press shown at once, as the cue a server mod will answer with (a
`CbPrediction`). What every simulation runs is a motion's, not a prediction's.

## HUD blocks

```
hud Skills/Dash
	show if carried.brawl.dash_strike
	text "[{key:slot2}]  DASH STRIKE  READY" if brawl.cd_dash <= 0
	text "[{key:slot2}]  DASH STRIKE  {brawl.cd_dash}s" into text if brawl.cd_dash > 0
	bind modulate:r = 1 - 0.45 * (brawl.cd_dash <= 0)
```

| Line | Means |
|---|---|
| `hud PATH` | the block: a control of the HUD scene, its path written once |
| `show if EXPRESSION` | shown while it holds |
| `text "FORMAT" [into PROPERTY] [if EXPRESSION]` | words, `{field}` filled in (several may take turns) |
| `bind PROPERTY = EXPRESSION [if EXPRESSION]` | an expression into a property, or a part of one (`modulate:a`) |

Each line becomes a `CbFieldBinding` on the control.

## Version 0

A sheet without the `cbsheet 1` line is read as version 0, the spelling before M285. `cb_sheet
convert` writes it as version 1, comments kept, and writes nothing if the result would mean anything
else (each action's settings, compared); every shipped sheet was converted so.

| Version 0 | Version 1 |
|---|---|
| (no first line) | `cbsheet 1` |
| `on EVENT` / `while EXPRESSION` at the start of a line, its settings holding for the actions below them | `react on EVENT` / `react while EXPRESSION`, scopes with the actions indented under them |
| `subject ^` / `^^` / `$at` / `$other` / `$local` / `$world` | `as me` / `holder` / `actor` / `target` / `local` / `world` |
| `kind prop`, `template NAME` | `if actor is prop`, `if actor is NAME` |
| `delay S` | `after S` (a scope) |
| `wait` | `server only` |
| `scene res://vfx/NAME.tscn [under N] [life S]` | `create NAME [under N] [for S]` |
| `sound res://assets/sfx/NAME.wav ...` | `sound NAME ...` |
| `shake A time S`, `flash C time S` | `shake A for S`, `flash C for S` |
| `show PATH ...`, `text PATH ...`, `bind PATH ...` | `hud PATH` with `show ...`, `text ...`, `bind ...` under it |
| `then when EXPRESSION [within S]` | `wait for EXPRESSION [within S]` |
| `then when X within S`, lines, `timeout`, the same lines, the end | `wait for X or S`, the lines |
| `after S` (a step) | `wait S` |
| `item hand` / `item carried`, `owner item` | `for holder` / `for carrier`, `on item` |
| `?name`, `$other:name`, `^^:name`, `is_local`, `motion.NAME` | `known name`, `name of target`, `name of holder`, `local`, `active NAME` |
| `^^/Head`, `$at/RightHand`, `$local@pickup.target` | `holder/Head`, `actor/RightHand`, `local@pickup.target` |

Not in version 1 yet (no shipped sheet says them): `side`, `wait off`, `at here`.

## Tools

| Tool | |
|---|---|
| `cb_sheet check FILE...` | reads each sheet, its mistakes as `FILE:LINE: why`; exit code 1 if any. A motion's nodes (a find's shape) are the scene's: the editor and the bake check those |
| `cb_sheet fmt [--check] FILE...` | writes each version 1 sheet in the one style: tabs, no spaces at the ends of lines, one blank line at most; what it means is checked to stay the same |
| `cb_sheet convert FILE...` | version 0 to version 1, in place |
| the editor's Sheet screen | colours, names offered, mistakes on their lines as you type ([looks.md](looks.md#rules-sheets)) |

`cb_sheet` is built with the tests (`bin/cb_sheet`). `cb_tests sheets_version1` checks that every
shipped sheet is version 1, in the one style, and converts back to itself from version 0.

## Grammar, in short

```
sheet      := "cbsheet 1" { block }
block      := motion | use | react | predict | hud
motion     := "motion" NAME trigger [ "if" expr ] NEWLINE INDENT { setting } { line } DEDENT
use        := "use" [ "if" expr ] ...                          (exactly "motion Use on use" ...)
trigger    := "on use" | "on press" ACTION | "on" EVENT | "while" expr | "when" expr
setting    := "cooldown" num | "uses" num [ "refill" ( "ground" | "after" num ) ] | "lasts" num
            | "until" expr | "for" ( "holder" | "carrier" ) | "on item"
line       := once | lasting | order | finding          (where each may stand: the table above)
once       := "change" FIELD op amount | "emit" EVENT | "impulse" amount { option }
            | "launch" ( "new" KIND | "carried" ) { launch-option } | "break" { break-option }
            | "remove self" | "end"
lasting    := "force" amount { option } | "move" [ "for" num ] { PARAM amount }
            | "link" { link-option } | "follow" NODE "over" num
order      := "wait for" expr [ ( "within" | "or" ) num ] | "wait" num | "timeout" | "every" num
            | "if" expr ... [ "else" ... ]
finding    := "find" shape { find-option } ... | "cast" shape "range" num [ "speed" num ] [ "holds" ... ] [ "along" ( "look" | "facing" ) ] ...
amount     := a number, or an expression up to the next option word
option     := "on" who | "along" direction [ "turn" vec2 ] | "replace" ... | "kind" ... | "top" amount
            | "ramp" num | "react"
who        := "self" | "target" | FIELD
direction  := "look" | "move" | "facing" | "up" | "to" | "away" | "world" vec | NODE
react      := "react" ( "on" EVENT [ "if" expr ] | "while" expr ) NEWLINE INDENT { "cooldown" num | "server only" } { rline } DEDENT
rline      := ( "as" subject | "at" place | "after" num | "chance" num ) NEWLINE INDENT { rline } DEDENT
            | "sound" ASSET { sound-option } | "create" ASSET [ "under" PATH ] [ "for" num ]
            | "play" PATH ANIMATION [ "off" ANIMATION ] | "set" PATH PROPERTY ( value [ "blend" num ] | "=" expr )
            | "call" PATH METHOD { value } | "shake" num [ "for" num ] | "flash" COLOR [ "for" num ]
predict    := "predict" ACTION "as" EVENT [ "if" expr ] NEWLINE INDENT { "cooldown" num | "held" | "change" FIELD op num | "stance" NAME [ "on" LAYER ] } DEDENT
hud        := "hud" PATH NEWLINE INDENT { "show if" expr | "text" STRING [ "into" PROPERTY ] [ "if" expr ] | "bind" PROPERTY "=" expr [ "if" expr ] } DEDENT
subject    := "me" | "holder" | "actor" | "target" | "local" | "world" | PATH
place      := "point" | "end" | "beam from" PATH | "follow" | PATH, then [ "offset" X Y Z ]
```

## The nodes

A sheet is the one way to write a motion: the motion nodes (`CbMotion`, `CbImpulse`, `CbForce`,
`CbMove`, `CbLink`, `CbLaunch`, `CbProbe`) and `CbCurve` were deleted in M268 (no shipped mod used
them after M243). What a sheet names stays a node: `CollisionShape3D` and `Area3D` (shapes),
`Marker3D` (arrows and places), `Path3D` (paths).

## Planned

| What | |
|---|---|
| `at POINT` for an emit, an effect's place | today a find, a grab and a break (M290) |
| `at target` | a find inside a find's lines |
| `PATH at T` | a direction along a `Path3D` |
| A cast in a later step | today a cast is thrown when the motion starts |
| The player's body | `wall` (a condition and a direction), `teleport`, passing through, not hittable, the capsule's size, keep speed and steer, hidden from others ([ROADMAP.md](../ROADMAP.md#parked)) |

| Open question | Leaning |
|---|---|
| Two slows on one player: the stronger, the newer, or multiplied? | the newer wins per parameter (today's); a `multiply` to stack |
| May a sheet change another player's fields? | not at first: health is a mod's rule (`emit`, then the mod's C++) |
| `on item` for a map's prop (a mine, a door)? | later |
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
| M283 to M285 | one compiler in plain C++ (`src/sheet`); a sheet a `.cbsheet` resource; version 1, one spelling for every block (`react`, `hud`, scopes, `wait for`, `create`, short names), every shipped sheet converted; this page moved from `sheets.md` |
| M286 | declarations in the sheet (`field`, `event`, `action`, `param`), read by the server from the mod's item; an item's numbers and own fields its sheet's (the `CbItem`'s `properties` and `fields` went); units |
