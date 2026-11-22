# Step 6: sheets improved, planned

Written 2026-11-21 (after M282), for the owner and for whoever implements it, a person or an agent.
Nothing here is built yet; [ROADMAP.md step 6](../ROADMAP.md#6-sheets-improved) is the summary and
this is the plan behind it. Where this file and the code disagree about what exists today, the code
wins ([the rule](../ROADMAP.md#how-we-work)).

**In one paragraph.** A sheet is the one text a modder writes for what an item, a skill or a look
does. Today it is four small languages in one file, read by two parsers that disagree in places,
with names declared in C++, numbers written twice, settings whose scope is invisible, and no way to
keep an item's own state or a timer, so a mod's C++ still does what a sheet should. Step 6 makes it
one regular language with one compiler (plain C++, no Godot types), a real file type
(`.cbsheet`), names and parameters declared in the sheet, the words fighting games need (cooldowns
you can read, statuses, combos, charge, tap and hold), and the planned words (`of target`,
`at POINT`) that the owner's telekinesis needs. Every shipped sheet is converted by a tool and
checked to compile to exactly what it did before, so nothing a player sees changes by accident.
Then 6b cooks items, and the compiler is the part of the cooker that reads sheets.

## Contents

1. [What a sheet is for: what does not change](#1-what-a-sheet-is-for-what-does-not-change)
2. [Where it stands](#2-where-it-stands)
3. [What is wrong](#3-what-is-wrong)
4. [Goals and non-goals](#4-goals-and-non-goals)
5. [How we design it](#5-how-we-design-it)
6. [The language](#6-the-language)
7. [Before and after: the shipped sheets](#7-before-and-after-the-shipped-sheets)
8. [The compiler](#8-the-compiler)
9. [What the engine has to learn](#9-what-the-engine-has-to-learn)
10. [Moving every sheet over](#10-moving-every-sheet-over)
11. [Tools](#11-tools)
12. [Milestones, in order](#12-milestones-in-order)
13. [Decisions for the owner](#13-decisions-for-the-owner)
14. [Risks](#14-risks)
15. [Appendix A: grammar](#appendix-a-grammar)
16. [Appendix B: every old word and its new one](#appendix-b-every-old-word-and-its-new-one)
17. [Appendix C: for whoever builds it](#appendix-c-for-whoever-builds-it)

---

## 1. What a sheet is for: what does not change

| Holds today, holds after | Why |
|---|---|
| **A simulated block runs on every machine**, the server and every client, on the same input, and rolls back like walking | what a player does to bodies is predicted on their own screen, never a guess |
| **Deterministic**: no clock, no randomness but the simulation's, the same arithmetic everywhere (`expr::Apply`, the sim's own copy of a program) | the server and every client reach the same result |
| **Bounded**: a tick's work is capped (a find takes 16, steps 64, a step's ifs 16) | rollback runs a tick again many times; a tick has to cost about the same every time |
| **No storage of its own** beyond fields and each motion's small record (28 bytes) | the record rolls back with the rest |
| **No files, no network, no text in the simulation** | it runs inside the physics step |
| **What only the server should decide stays a server mod's** (damage, health, score, inventories, round timers, anything secret, hit tests against posed hitboxes) | a client could lie; a secret sent to every client is no secret |
| **Looks never change the simulation** | a look is the viewer's; a player's own restyle must not be a cheat |
| **No code from a stranger runs** | sheets are data a compiler checks; Luau comes later and only on the server ([step 10](../ROADMAP.md#10-server-rules-in-luau)) |
| **Authored in Godot**: the editor is the modders' tool; nodes stay for what is a place or a thing (a shape, a marker, a path, a mesh to light up) | modders come from Godot, Roblox and Unity; they draw places, they type logic |

## 2. Where it stands

### The four languages in one file

| Block | Starts with | Runs | Becomes today |
|---|---|---|---|
| **motion** (simulated) | `motion NAME on ...`, `motion NAME while ...`, `motion NAME when ...`, `use` | every simulation | baked by `BakeSheetMotion` (`src/godot/sheet_motions.cpp`, 1,126 lines) to text, `motions/<kind>.cfg`, which every simulation parses again (`CompileMotionSet`, `src/sim/motions.cpp`, 1,728 lines) |
| **reaction** (look) | `on EVENT`, `while EXPR` | the viewer | parsed by `ParseRules` (`src/godot/cue/cue_rules.cpp`, 2,109 lines) into Godot Dictionaries, made into `CbReaction` nodes under the `CbRules` node when the scene loads |
| **prediction** (look) | `predict ACTION as CUE` | the viewer | one `CbPrediction` node; after M282 one is left in shipped content (the mark mod's scan) |
| **HUD line** (look) | `show`, `text`, `bind` | the viewer | one `CbFieldBinding` node each |

The expressions inside all four are one language (`src/expr`, 655 lines), also read by characters'
state machines.

### The shipped sheets (2026-11-21)

42 files, 832 lines, in the game, the mods, the SDK, the example client mod and the checks. By what they hold:

| Kind | Files | Examples |
|---|---|---|
| an item's: simulated and look blocks together | 13 | `pistol.rules` (Fire, Dry, Reload, its flash, tracer, sounds, the scan's prediction), `bat.rules`, `sword.rules`, `fist.rules`, the four skills, `frag.rules`, `shell.rules`, `cannon.rules`, `boots.rules`, `hook.rules` |
| a motion set's (everyone's) | 4 | `brawl_moves.rules` (five count-down timers), `flight_moves.rules`, `dash_moves.rules`, the SDK's starter |
| world reactions | 15 | `godot/vfx/reactions.rules`, `reactions_<mod>.rules`, `brawl.rules` |
| HUD | 6 | `hud_brawl.rules`, a line per skill, `pistol_hud.rules` |
| the checks' own | 4 | `check_rules_good.rules`, `check_rules_bad.rules`, ... |

### How a sheet gets to a game

```
Godot editor                            publish (tools\publish_mod.ps1)               the game
------------                            -------------------------------               --------
foo.rules  --CbRules.file (a string)--> BakeSheetMotion --> motions/<kind>.cfg (text) --> server: CompileMotionSet
                                                                                          --> schema --> every client
           --ParseRules at load-------> CbReaction / CbPrediction / CbFieldBinding nodes (the viewer)
names      <--cinderbox_names.cfg------ written by every server build (the C++ declarations)
numbers    <--CbItem.properties-------- (the inspector), read by the server's C++ (gun.damage)
```

## 3. What is wrong

Each problem is one this conversation or a shipped sheet ran into. The fix is in [§6](#6-the-language).

| # | Problem | Where it shows |
|---|---|---|
| P1 | **Two parsers, one language.** `ParseRules` and `CompileMotionSet` read different things (the sheet, the baked cfg); words are kept in step by hand (the directions written three times: `motions.cpp`, `cinderbox_motion.cpp`, `cue_rules.cpp`) | a word added in one place and not another is a sheet that bakes and does nothing |
| P2 | **The file is a string on a node** (`CbRules.file`): no UID, a rename breaks it, Godot does not see the dependency | a moved sheet is a silent look with no rules |
| P3 | **Four languages share a file**, told apart by the first word: `while` is a look rule at the top and a motion's trigger after `motion NAME`; `on` the same | a reader cannot see at a glance what runs on every machine |
| P4 | **Scope you cannot see.** A reaction's settings (`subject`, `at`, `delay`, `chance`, `cooldown`, `wait`) "hold for the actions below, until another line changes it" at the same indentation; motion steps (`then when`) are flat too | the owner asked whether the lines under `at` lost their indentation; they never had any, which is the problem |
| P5 | **Two spellings** for the same thing: `!`/`not`, `&&`/`and`, `\|\|`/`or`; `found`/`hit`/`target`; `world (x, y, z)`/`world direction (x, y, z)` | sheets mix them; `!a == 2` means `not (a == 2)` |
| P6 | **Symbols where words would do**: `^`, `^^`, `$at`, `$other`, `$local`, `$world`, `@field`, `^^:combat.dead`, `?name` | a modder from Roblox reads `^^:combat.dead` as noise |
| P7 | **Names declared in C++**: a sheet names a field or an event a mod's C++ declares; the editor knows them only after a server build writes `cinderbox_names.cfg` | the editor waits for a compiler; a look-only modder cannot add an event |
| P8 | **Numbers written twice**: `gun.damage` in `CbItem.properties`, the time between shots in the sheet's `cooldown` (it was in the scene too until M282); the fist's box written four times | one fact, two places ([the rule](../ROADMAP.md#how-we-work)) |
| P9 | **No state on an item, no timers**: a sheet can change only the player's fields, and nothing counts down by itself | the gun's rounds and the reload's end are C++ (`gun.cpp` keeps them and corrects the holder's copy); the bat's heat is C++; brawl's five cooldowns are five `while` motions in `brawl_moves.rules` counting fields down |
| P10 | **A motion's own state is not readable**: its cooldown, its uses, whether it is on (only `motion.NAME`) | brawl writes its own cooldown fields so the HUD can show seconds left; `cooldown 3` on the motion itself goes unused |
| P11 | **The marker-or-timeout copy**: "wait for the clip's marker, or go on after S seconds" is written as `then when X within S` plus a `timeout` with the same lines again | the fist (twice), the sword, the bat, the frag, the grenade shell: every melee sheet doubles its find |
| P12 | **No words for how skills meet**: being stunned, one skill blocking another, combos, charge, tap against hold, shared cooldowns | the fist guards with `not motion.brawl.fist.Finisher`; brawl counts its combo window in a field; `sword.stunned` is C++ with a timer |
| P13 | **A find's event has no value** | the bat's hit sound can no longer tell a player from a prop (M282) |
| P14 | **Effects only around the player**: a find is around the owner's feet, a `break` around the owner, `force`/`link` act on one held thing | the owner's telekinesis (a sphere the player steers, pulling many bodies, breaking a wall there) cannot be written |
| P15 | **Full paths for every sound and scene**: `sound res://assets/sfx/ricochet.wav` | the owner asked for `sound ricochet.wav` |
| P16 | **`scene` names a Godot scene** the game loads | after 6b no Godot scene reaches a player: the word should say what it is (an effect), and the cooker should turn it into data |
| P17 | **Errors are found late**: at bake, at publish, or as "the motion never happens" | no formatter, no headless tests of a sheet, errors as you type only for names |
| P18 | **A repeat is capped at 64** (`kMotionRepeats`), though a repetition only costs its own tick | an automatic gun with a magazine over 64 cannot be written |
| P19 | **A find takes the nearest of everything**: players and props compete | the bat found a pistol lying closer than the player (M282); it needs two finds |

## 4. Goals and non-goals

**Goals**

- G1. **One language, one compiler**, used by the editor, the server, the cooker (6b) and tests.
- G2. **Readable by a modder from Roblox, Unity or Godot** without a glossary: words, not symbols;
  the standard terms (cooldown, charges, status, parameter, client-side prediction; "ability" in the
  guide's prose for a skill, written as a motion).
- G3. **What runs where is visible**: a block's first word says whether every machine runs it or only
  the viewer.
- G4. **One rule for scope**: what a line governs is what is indented under it. Nothing else.
- G5. **One place for a fact**: names, parameters and an item's state are the sheet's.
- G6. **The words a fighting game needs**, so the slice's kit, the sword, the guns and the owner's next
  weapons are sheets with as little C++ as the server truly needs.
- G7. **The format settled with a version** before launch; later versions add words, and the compiler
  reads every older version.
- G8. **Nothing a player sees changes by accident**: every converted sheet compiles to what it did.

**Non-goals**

- Not a scripting language: no loops but bounded ones, no functions a modder writes, no recursion,
  no strings in the simulation. (A server's free rules are Luau, step 10.)
- Not Godot's script slot ([decided](../ROADMAP.md#6-sheets-improved): the security story is "no
  scripts at all").
- Not a visual language now (Statescript-like graphs could sit on the same compiled form later).
- Not hit tests against posed hitboxes in a sheet: that is [step 7](../ROADMAP.md#7-timing-from-the-clip)
  (a shape that follows a bone). Step 6 keeps the server's hit test reachable from a sheet's event.
- Not drawing items as data or running reactions from the compiled sheet in the presentation's ECS:
  moved to [6b](../ROADMAP.md#6b-cooked-items). Step 6 produces the data; 6b consumes it without nodes.

## 5. How we design it

| Rule | Means |
|---|---|
| **Words over symbols** | `holder` not `^^`, `of` not `:` (logic keeps both: `!` and `not`, decided D3) |
| **One way to say a thing** | the compiler reads one spelling; the converter rewrites the others; the formatter prints the one |
| **Indentation is the only scope** | a line that opens something (a block, a step's `if`, a `find`, a `cast`, an `at`) has its lines further in; a tab or four spaces |
| **The first word says what a line is** | every line starts with its verb or its keyword; options follow as `word value` pairs |
| **Declared where it is used** | a field, an event, a parameter is declared in the sheet that uses it; a server mod's C++ looks it up by name, or declares it the same way |
| **Defaults from where the sheet sits** | in an item's sheet, a reaction's subject is the holder; in world reactions, the event's actor |
| **What a modder writes, the compiler checks** | every name, unit, place, limit; a mistake says its line, the reason, and the nearest right word |
| **Converted, not rewritten by hand** | a tool moves every shipped sheet; the compiled output is compared |
| **The record does not grow without a reason** | new state lives in fields (which have a size and a schema), not in each motion's record |

## 6. The language

Examples are written as they would be in version 1. Words marked **new** do not exist today; the
rest are today's words, respelled where §3 asks. [Appendix B](#appendix-b-every-old-word-and-its-new-one)
lists every change.

### 6.1 A file

```
cbsheet 1                                   # the format's version: always the first line

# Everything else is blocks, in any order (the formatter puts them in the order below).
```

- **The extension `.cbsheet`** ([decided 2026-10-27](../ROADMAP.md#0-decisions)). A file without the
  version line is version 0, today's `.rules`; the compiler reads it until every sheet is converted,
  then refuses it.
- **A resource on a node**, not a string: `CbSheet` (plain text, its own loader and saver, as `.gd`
  and `.gdshader` are), set on the node it belongs to: `CbItem.sheet`, `CbMotionSet.sheet`, the
  world reactions root's, a HUD root's. It may be saved inside the scene. `CbRules` goes.
- **What a sheet may hold is the node's to say**: an item's sheet motions, reactions, predictions,
  its declarations; a motion set's motions and declarations; world reactions only reactions; a
  HUD's sheet HUD blocks and reactions. A block in the wrong sheet is an error at its line. A map's
  things (doors, pads, regions: [Hytale proposal 4, kept open](../ROADMAP.md#postponed-kept-open))
  become one more such node later.
- **Comments** with `#`, to the line's end. **Blank lines** mean nothing.

### 6.2 Blocks: what runs where

The first word of a block says who runs it:

| Block | Runs | Was |
|---|---|---|
| `motion NAME TRIGGER [if COND]` | **every simulation** (predicted, rolled back) | unchanged (D1) |
| `use [if COND]` | the same: an item's use, short for `motion Use on use [if COND]` | `use` (unchanged) |
| `react TRIGGER [if COND]` | **the viewer only**: effects, sounds, properties, shakes | `on EVENT` / `while COND` at the top of a sheet |
| `predict ACTION as CUE [if COND]` | the viewer: a guess of what a server mod will say | `predict` (unchanged) |
| `hud PATH` | the viewer: one control's lines (`show`, `text`, `bind`) | `show PATH ...`, `text PATH ...`, `bind PATH ...` |
| `field`, `event`, `action`, `status`, `param`, `let` | declarations: the compiler and the server read them | C++ `declare.*`, `CbItem.properties` |

- **Why `motion`** (decided D1, 2026-11-21): a simulated block is anything every machine runs, an
  impulse, a find, a field change, an event, a refuel, a gun's rounds, a grenade's blast, and not
  only a player's skill: `ability` fit the skills and not the rest. `motion` is the engine's word
  already (the C++, the debug HUD, the docs) and is defined as "what every machine simulates". A
  block type of its own for state that is not a skill (timers, refills) may come later; not now.
- **The trigger words are the same in both worlds**: `on EVENT`, `while COND`, `when COND` mean
  what they mean for a motion and for a reaction. Only the block's first word differs.

### 6.3 Indentation and scope

One rule: **a line governs what is indented under it, and nothing else.**

```
react on item.use                         # a reaction: its lines are under it
    at Muzzle                             # a place: the actions under it go there
        create muzzle_flash under Muzzle for 0.3
        sound gunshot volume -5 jitter 0.06 range 70
    at beam from Muzzle to end            # another place, for the action under it
        create tracer for 0.06
```

| Opens a scope | Its lines |
|---|---|
| a block (`motion`, `react`, `predict`, `hud`) | the block's |
| `if COND` / `else` | happen or not |
| `find ...`, `cast ...`, `grab ...` (**new**) | act on what it found or holds |
| `at PLACE`, `after S` (a delay), `chance P`, `as SUBJECT` (reactions) | settings for the actions under it |
| `every S` | repeat |
| `timeout` | what a wait that gave up runs |

- **Steps stay a flat sequence**: `wait ...` lines cut a block into steps, top to bottom, as
  `then when` does today. A step is not indented; what is under a `find` in it is.
- **Settings of a block** (`cooldown`, `uses`, `lasts`, `until`, `tags`, ...) are its first lines,
  before anything happens: as today.
- **Tabs or four spaces**, never mixed in one file. The formatter writes tabs.

### 6.4 Names: declared in the sheet

Today the names a sheet uses are declared in a mod's C++ and reach the editor through a server
build. After: a sheet declares what it introduces, the server reads the declarations from the mod's
item (it already reads the item's scenes and baked files when it starts), and the C++ looks them up.

```
field brawl.combo int                      # on the player (the default), every machine has it
field brawl.combo_left timer               # new: a float that counts down to 0 by itself, 1 a second
field gun.rounds int on item = magazine    # new: on the item itself; a new copy starts at the param
field team.id int shared                   # shared: every client has everyone's (the scoreboard)
field shop.coins int private               # only its player's client has it
event brawl.jab.hit
event item.use
action reload key R                        # a key a player presses (the server's ActionHandle)
status stunned                             # new: a timed flag on a player (6.7.6)
```

| Declaration | Today | After |
|---|---|---|
| `field NAME TYPE [on player\|item\|world] [shared\|private] [= VALUE]` | `declare.Field( name, type, scope, shared )` in C++; an item's own fields in `CbItem.fields` | the sheet. Types: `int`, `float`, `bool`, `timer` (**new**) |
| `event NAME` | `declare.Event` | the sheet |
| `action NAME key KEY` | `declare.Action( name, key )` | the sheet |
| `status NAME` | (no such thing: `sword.stunned` is a C++ field and timer) | the sheet |

- **The C++ adapts to the pack** ([the owner's rule](../ROADMAP.md#how-we-work): the pack is the source
  of truth). A mod's C++ finds a name with `declare.Field( "gun.rounds", ... )` as today (it returns
  the sheet's if the types agree, and a mistake names both if not), or with a lookup that never
  creates one (`Schema().FindField`), as the bots mod does.
- **The same name in two sheets** must agree in type and place; the server says so when it starts.
- **The editor** knows a sheet's own names as you type, with no server build. `cinderbox_names.cfg`
  still brings the names C++ declares that no sheet does (until none are left).

### 6.5 Parameters

```
param damage 25                            # a number, shown in the inspector of the node the sheet is on
param magazine 12
param fire_interval 0.2 s
param reload_time 1.5 s
param range 80 m
```

- **One place**: the gun's numbers move from `CbItem.properties` (the inspector's dictionary) into
  the sheet. The inspector shows them under the sheet, with their units and comments, and a scene
  may override a value for its own copy (the rifle uses the pistol's sheet with other numbers: 6.11).
- **Read everywhere**: a parameter is a name in every expression of its sheet (`cooldown fire_interval`,
  `change gun.rounds = magazine`), and the server reads it (`ctx.ItemProperty( kind, "gun.damage" )`
  finds `param damage` of the item `gun.pistol`'s sheet as `gun.damage`: the sheet's prefix is its
  item kind's mod).
- **Numbers, always**: a parameter is a constant, compiled in; nothing changes it while a game runs.
  A number a server wants to change at run time is a field the server sets (`frag.radius` today).
- **A server may override a parameter** (decided with D4, 2026-11-21): a server option names it
  (`--mod-option gun.rifle.magazine=50`), the server compiles the item's motions with it, and sends
  them in the schema as always, so every machine runs the server's numbers. The item and its hash
  do not change: players download nothing. Looks cannot be changed this way (they are the item's,
  and a server never sends files). It is one more named setting of a game ([Hytale proposal 6, kept
  open](../ROADMAP.md#postponed-kept-open)).

### 6.6 Expressions

The same expression language everywhere (a condition, a value, a HUD line, a state machine), with one
spelling:

| Write | Means | Was |
|---|---|---|
| `not a` / `!a`, `a and b` / `a && b`, `a or b` / `a \|\| b` | logic: both spellings, as today (decided D3, 2026-11-21) | unchanged |
| `known gun.ammo` | the server declared it (its mod runs) | `?gun.ammo` |
| `combat.health of target`, `combat.dead of holder` | another entity's field | `$other:combat.health`, `^^:combat.dead` |
| `local` | the viewer's own player is the subject (looks only) | `is_local` |
| `first_person` | the viewer looks out of its own eyes (looks only) | unchanged |
| `actor is player`, `actor is prop`, `holder is melee.bat` | what kind an entity is, or which item kind (**new** `is`) | the reaction settings `kind`, `template` |
| `ready Fire`, `cooldown Fire`, `uses Fire`, `active Fire` | a motion's state: ready to start, seconds of cooldown left, uses left, on now (6.7.5) | only `motion.NAME` (on now) |
| `remaining stunned` | seconds a status has left (6.7.6) | |
| everything else | as today: numbers, fields, `+ - * /`, comparisons, `( )`, `grounded`, `speed`, `held.jump`, `pressed.dash`, `using.gun.rifle`, `carried.KIND`, `hand.FIELD`, `event.value`, `time`, `distance`, `reach`, `curve(...)` | |

- **Precedence**, loosest first, unchanged: `or`, `and`, `not`, comparisons, `+ -`, `* /`, unary `-`.
  So `not a == 2` is `not (a == 2)`: the formatter writes the brackets.
- **`let NAME = EXPR`** (**new**): a name for a value, in a sheet (a constant) or a block (read when its
  step starts). The frag's falloff is written once:
  `let falloff = (distance < frag.radius) * (1 - distance / frag.radius)`.
- **Units** (decision D8, leaning yes): a number may carry `s`, `m`, `m/s`, `deg`; the compiler checks
  it against what the word takes (`cooldown 0.4 m` is an error) and drops it. Without a unit, the
  word's own (seconds for times, metres, m/s) as today.
- **Rates said**: in a `while` motion, `change flight.fuel -= 30 per second` (**new**: today `-=` in a
  `while` is silently per second and `=` is once). Without `per second`, once.

### 6.7 Motions (every simulation)

#### 6.7.1 Triggers and settings

```
motion NAME TRIGGER [if COND]
    SETTINGS
    LINES
```

| Trigger | Starts it | Was |
|---|---|---|
| `on use` | the item is used (the button with it in the hand, or its own key) | unchanged |
| `on press ACTION` | the action goes down | unchanged |
| `on release ACTION` (**new**) | it comes up; `held` reads how long it was down (a charge) | |
| `on tap ACTION` / `on hold ACTION S` (**new**) | released before S seconds / still down after S (S: `0.25 s` by default) | |
| `on EVENT` | the event was said at the owner (a server mod's, a clip's marker, another motion's) | unchanged |
| `while COND` | every tick it holds | unchanged |
| `when COND` | once, on the tick it turns true | unchanged |

| Setting | Means | Was |
|---|---|---|
| `cooldown S [shared NAME]` | seconds between two starts; `shared`: every motion with that name waits for it | (`shared` **new**) |
| `uses N [refill ground \| refill after S]` | charges | unchanged |
| `lasts S` | it ends S seconds after it starts | unchanged |
| `until COND` | it ends when this holds | unchanged |
| `tags NAME, ...` (**new**) | what kind of thing it is, for others to block or cancel (`attack`, `movement`) | |
| `blocked by NAME, ...` (**new**) | it does not start while one of these is on: a tag, a motion, a status | `if not motion.brawl.fist.Finisher` |
| `cancels NAME, ...` (**new**) | starting it ends these (a dodge cancels an attack's recovery) | |
| `for holder` / `for carrier` | in an item's sheet: whoever has it out (the default) / carries it | `item hand` / `item carried` |
| `on item` | the item runs it, where it lies, in one tick | `owner item` |

#### 6.7.2 Steps and waits

```
motion Swing on use
    cooldown 0.4 s
    emit sword.swing
    wait for sword.strike or 0.5 s           # the clip's marker, or go on after 0.5 s without it
    find box (1.2, 1.6, 2.4) at (0, 1, 1.3) turned look players max 2
        emit sword.hit value damage
        impulse 6 along facing
    wait for sword.done or 1 s
```

| Line | Means | Was |
|---|---|---|
| `wait S` | the next step starts S seconds after this one did | `after S` |
| `wait for COND` | ... once this holds | `then when COND` |
| `wait for COND or S` (**new**) | ... once this holds, or after S seconds, whichever first: **the step after runs either way** | `then when COND within S` + a `timeout` with the same lines again |
| `wait for COND within S` + `timeout` | ... once this holds; after S it gives up: the `timeout` lines run and the motion ends | unchanged |
| `end` | it ends here | unchanged |
| `if COND` / `else` | decided when its step starts | unchanged |
| `every S` | the lines under it, when the step starts and every S seconds while it is the step; no cap (P18): a repetition costs its own tick only | 64 at most |

#### 6.7.3 Effects

| Line | Means | Changes |
|---|---|---|
| `change FIELD = / += / -= VALUE [per second]` | a field: the player's, or the **item's** if it is declared `on item` (6.4) | item fields, `per second` (**new**) |
| `emit EVENT [value VALUE]` | an event at the owner (in a find: A the owner, B what it found, at it), with a value | `value` (**new**: P13) |
| `impulse`, `force`, `move`, `move for`, `link`, `launch`, `follow`, `remove self` | as today | |
| `break radius R [speed S] [seconds S] [at POINT]` | breakable walls near the owner, **or near a point** | `at` (**new**) |
| `apply STATUS for S [on WHO]` (**new**) | a status on the owner or on what a find found (6.7.6) | |
| `throw VALUE along DIR [marks EVENT for S]` (**new**) | an impulse that marks the body as thrown by the owner: if it strikes a player within S seconds, EVENT is said (A the thrower, B the one struck, value its speed) | |

#### 6.7.4 Finding, holding, and where

| Line | Means | Changes |
|---|---|---|
| `find SHAPE [players] [bodies] [self] [max N] [turned facing\|look\|none] [at POINT] [players first]` | what the shape overlaps when its step starts; lines under it once for each | `at POINT` beyond an offset; `players first` (**new**: P19) |
| `cast SHAPE range R [speed V] [holds ...] [along look\|facing]` | a line thrown along the look; lines under it act while it holds | in a later step too (planned today) |
| `grab SHAPE [players\|bodies] [max N] [at POINT]` (**new**) | like a find, but what it found is **held** while the step lasts (up to 8): the lasting lines under it (`force`, `move`, `link`) act on each, every tick; held bodies let go when the step ends or they leave the shape by more than its own size | |

**Points** (**new**: `at POINT`, planned today), where a find, a grab, a break or an effect happens:

| `POINT` | Is |
|---|---|
| `(x, y, z)` | metres right, up and ahead of the owner's feet (today's `at`) |
| `look N` | N metres along the look from the chest, stopped by the level: a point the player steers |
| `hit` | where the motion's cast holds |
| `NODE` | a marker of the sheet's scene, as drawn |
| `target` | what the find found (in its lines) |

**Who and which way**, as today (`on self | target | FIELD`, `along look | move | facing | up | to |
away | world (x, y, z) | NODE` [`turn (yaw, pitch)`]), plus **new**: `toward POINT` (from the target
to a point), `toward owner` (from the target back to the owner: a pull).

#### 6.7.5 A motion's state, readable

| Read | Is |
|---|---|
| `active Fire` | it is on now (today `motion.Fire`) |
| `ready Fire` | it could start now (not on, off cooldown, a use left; its `if` not asked) |
| `cooldown Fire` | seconds of cooldown left (0 when none) |
| `uses Fire` | uses left |
| `cooldown brawl.dash_strike.Use` | another item's motion, by its item kind (for a HUD, a condition) |

The simulation has all of it already (each motion's record: its last start, its uses); the
expression reader learns the names, and the HUD reads them through the frame like fields. **So
brawl's `cd_*` fields and `brawl_moves.rules` go** (P9, P10).

#### 6.7.6 Statuses (new)

A status is a named, timed flag on a player: `stunned`, `rooted`, `silenced`, `staggered`.
Declared in a sheet (`status stunned`), applied by a sheet (`apply stunned for 0.8 s on target`,
predicted: the one who hits sees it at once) or by a server mod (`ctx.ApplyStatus( target, stunned,
seconds )`, the sword's parry), read as a condition (`stunned`, `stunned of target`,
`remaining stunned`), blocking motions (`blocked by stunned`), and drawn by looks (`react while
stunned`).

- **Why not "a sheet may change another player's fields"** (the open question): a field can be health;
  a status is only a flag with a time, which the target's own motions read. Health stays a mod's.
- **Bounded**: a player holds up to 8 statuses at once (the oldest goes), as moves given are 6 today
  (decision D7: the newer replaces the same status, or the longer wins: leaning the longer).
- **It replaces**: `sword.stunned` (C++ field and timer), the guard's `move` while held stays a move.

#### 6.7.7 Combos: timed motions (decided D6, 2026-11-21)

No word of its own: a combo is motions and a `timer` field (6.4), which every machine runs.

```
field brawl.combo int
field brawl.combo_left timer                 # counts down by itself: the window

motion Jab on use if brawl.combo < 2 and not active Finisher
    cooldown 0.3 s
    blocked by stunned
    change brawl.combo += 1
    change brawl.combo_left = 1.5
    wait for brawl.fist.strike or 0.35 s
    find hitbox max 1                        # (`let hitbox = box ...`: the box written once)
        emit brawl.jab.hit
        impulse 4 along facing
    wait for brawl.fist.ready or 0.7 s

motion Finisher on use if brawl.combo >= 2 and not active Jab
    ...

motion ComboOver when brawl.combo_left <= 0 and brawl.combo > 0
    change brawl.combo = 0
```

- **It replaces** brawl's C++ counting of the window and three of `brawl_moves.rules`' count-down
  motions (the timer counts itself). What stays is what the fist says today, in fewer lines.
- **The timing will come from the clip** ([step 7](../ROADMAP.md#7-timing-from-the-clip)): a combo's
  window is best drawn on the animation (a "next hit may start" window, two markers), as fighting
  games author cancel windows. Today's `wait for brawl.fist.ready` is already that, with one marker;
  step 7's windows make the window's end a marker too, and the `1.5` above goes.

### 6.8 Reactions (the viewer)

```
react on item.use                          # subject: the holder (an item's sheet's default)
    at Muzzle
        create muzzle_flash under Muzzle for 0.3
        sound gunshot volume -5 jitter 0.06 range 70

react on item.hit if event.value > 0 and local
    at holder/Head
        sound hit_marker volume -6

react while melee.hot_left > 0
    set Model/Barrel surface_material_override/0:emission_energy_multiplier 4 blend 0.2
```

| Was | Is |
|---|---|
| `subject ^^` / `$at` / `$other` / `$local` / `$world` / `^` | `as holder` / `as actor` / `as target` / `as local` / `as world` / `as me`, a scope like `at`; the default from where the sheet sits (an item's: `holder`; world reactions: `actor`; a character's: `me`) |
| `side A\|B\|Either` | `as actor` / `as target` / `as either` |
| `kind prop`, `template bouncy_ball` | `if actor is prop`, `if actor is bouncy_ball` |
| `delay S` | `after S`, a scope |
| `chance P`, `cooldown S` | `chance P` (a scope), `cooldown S` (a setting of the block: not more often than) |
| `wait` / `wait off` | `server only` (a setting: act on the server's cue, not a predicted press) |
| `at here` | (gone: no `at` is "here") |
| `scene PATH [under NODE] [life S]` | `create NAME [under NODE] [for S]` (6.10) |
| `sound PATH ...` | `sound NAME ...` (6.10) |
| `play`, `set`, `call`, `shake`, `flash` | unchanged |
| paths `^^/RightHand`, `$other/Head`, `$local@pickup.target` | `holder/RightHand`, `target/Head`, `local@pickup.target` (an entity named by a field keeps `@`) |

### 6.9 HUD blocks

```
hud Skills/Dash                             # one control: its path in the HUD scene
    show if carried.brawl.dash_strike
    text "[{key:slot2}]  DASH STRIKE  READY" if ready brawl.dash_strike.Use
    text "[{key:slot2}]  DASH STRIKE  {cooldown brawl.dash_strike.Use}s" if not ready brawl.dash_strike.Use
    bind modulate:r = 1 - 0.45 * ready brawl.dash_strike.Use
```

The control's path is written once (today on every line). The lines are today's.

### 6.10 Assets and effects

- **Short names** (P15): `sound gunshot` looks for `assets/sfx/gunshot.wav` (or `.ogg`) in the
  sheet's own project; `create muzzle_flash` for `vfx/muzzle_flash.tscn`. A full `res://` path still
  works (a sound shared from elsewhere). The compiler checks that the file is there and says the
  nearest name if not.
- **`create`, not `scene`** (P16; the word is D5's): until 6b what it creates is a Godot scene the scene guard lets through,
  as today. After 6b the cooker turns the effect's scene into cooked parts (emitters, lights,
  meshes, sounds: a fixed list of kinds, [6b](../ROADMAP.md#6b-cooked-items)), the game draws them
  through the RenderingServer, and **no Godot scene reaches a player**. The modder keeps authoring
  the effect as a Godot scene in the editor: Godot's tools (the particle editor, the 3D view) are
  the point of authoring in Godot. The sheet never says how it is stored.
- **What a node is stays a node**: a shape a find uses, a marker, a path, a mesh a reaction lights.
  A sheet names them by path, as today.

### 6.11 One sheet for many items

```
# prefabs/gun.cbsheet: every gun's trigger, written once
cbsheet 1
param damage 10
param magazine 10
param fire_interval 0.2 s
param reload_time 1.5 s
param automatic 0
field gun.rounds int on item = magazine
...
motion Fire on use if gun.rounds > 0 and not gun.reloading
    ...
```
```
# prefabs/rifle.cbsheet
cbsheet 1
uses gun.cbsheet                             # its params, fields, motions and reactions
param damage 14                              # numbers of its own
param magazine 30
param fire_interval 0.1 s
param automatic 1
replace react on item.use                    # a block of its own instead of the shared one's
    ...
```

- **`uses FILE`** brings another sheet's blocks; `param` overrides its numbers; `replace` swaps a block
  by its name (a motion's NAME, or a reaction's first line). Not a chain: a sheet uses at most one.
- Or in the inspector: a scene's `CbItem` points at `gun.cbsheet` and overrides `damage` there,
  without a sheet of its own. Both compile to the same thing.
- **It replaces** the fist's four copies of its box (with `let` and 6.7.7), the pistol's and the
  rifle's three copied motions.

### 6.12 Text and rows from the server (Hytale proposal 3)

Decided with this step, shaped when it is built: a mod publishes rows for a list
(`ctx.SetRows( "shop.offers", ... )`, each row named fields), and text fields (`field motd text on
world`) sent like player names and private fields, outside the simulation's hash. The HUD's `CbList`
lists the rows; a label's format reads the text. The layout stays the look's. It removes the shop's
offers written twice (`hud_shop.tscn` and `kOffers` in `shop.cpp`).

### 6.13 Limits

| Limit | Today | After |
|---|---|---|
| things a find takes | 16 | 16 |
| things a grab holds | | 8 (**new**) |
| times an `every` repeats | 64 | none (each repetition is one tick's work) |
| `if`s a step decides | 16 | 16 |
| steps a motion has | 64 | 64 |
| moves a player is given at once | 6 | 6 |
| statuses a player has at once | | 8 (**new**) |
| a sheet's `uses` | | one, not chained |

## 7. Before and after: the shipped sheets

What the converter writes (**converted**: the same behaviour) and what a sheet becomes once the new
words are used (**rewritten**: the milestone that adds the word changes it, and its tests prove the
same behaviour).

### The pistol (rewritten: item fields, timers, params, short names)

Today, 81 lines and `gun.cpp` keeps the rounds and the reload:

```
motion Fire on use if gun.ammo > 0 and not gun.reloading
	cooldown 0.2
	change gun.ammo -= 1
	emit item.use
motion Dry on use if gun.ammo <= 0 and not gun.reloading
	cooldown 0.2
	change gun.reloading = 1
	emit item.use_failed
	emit item.reload
motion Reload on press reload if not gun.reloading and gun.ammo < gun.magazine
	change gun.reloading = 1
	emit item.reload
on item.use
	subject ^^
	scene res://vfx/muzzle_flash.tscn under Muzzle life 0.3
	at Muzzle
	sound res://assets/sfx/gunshot.wav volume -5 jitter 0.06 range 70
```

After (the gun's whole trigger, its rounds and its reload in the sheet; `gun.cpp` keeps the shot's
hit test, the damage and the holder's aim):

```
cbsheet 1

param damage 25
param magazine 12
param fire_interval 0.2 s
param reload_time 1.5 s
param range 80 m

field gun.rounds int on item = magazine     # the gun's own: half empty on the hip, half empty in any hand

motion Fire on use if gun.rounds > 0 and not active Reload
    cooldown fire_interval
    change gun.rounds -= 1
    emit item.use                           # the server hears it and shoots (posed hitboxes)

motion Dry on use if gun.rounds <= 0 and not active Reload
    cooldown fire_interval
    emit item.use_failed
    emit item.reload                        # the reload below hears it (the sound too)

motion ReloadKey on press reload if gun.rounds < magazine and not active Reload
    emit item.reload

motion Reload on item.reload if gun.rounds < magazine and not active Reload
    wait reload_time
    change gun.rounds = magazine            # put away before this, it is not reloaded: as today

react on item.use
    at Muzzle
        create muzzle_flash under Muzzle for 0.3
        sound gunshot volume -5 jitter 0.06 range 70

react on item.reach
    at beam from Muzzle to end
        create tracer for 0.06

react on item.hit if event.value == 0
    at point
        sound ricochet volume -16 jitter 0.3 range 30
        create bullet_spark for 1

react on item.hit if event.value > 0
    at point
        sound body_hit volume -8 jitter 0.15 range 40
        create hit_puff for 1

react on item.hit if event.value > 0 and local
    at holder/Head
        sound hit_marker volume -6
```

- **No copy on the player**: the HUD reads the gun's own rounds (`hand.gun.rounds`, as it can today)
  and whether it reloads (`active gun.pistol.Reload`); `gun.ammo`, `gun.magazine` and
  `gun.reloading` go, and so does `gun.cpp`'s correcting of them.
- **A reload is a motion that waits**: `wait reload_time` in its own steps, no timer field.
- **No new word for one motion starting another** (decided D9): it emits an event and the other
  starts `on` it, the tick after, as any event is heard. `emit` stays the one way a motion says
  something, to looks, to the server's C++, and to other motions.

### The fist (rewritten: timers, let, statuses)

Today, 39 lines in `fist.rules`, two guards on each other's motions, the box written four times,
the combo and its window in two C++ fields counted down by two motions in `brawl_moves.rules`.
After:

```
cbsheet 1
let hitbox = box (0.8, 1.2, 2.6) at (0, 1, 1.4) turned look
    ...
```

(The fist as timed motions is [§6.7.7](#677-combos-timed-motions-decided-d6-2026-11-21): two motions
and a timer, its box written once, no C++ window.)

### The dash strike and its HUD line (rewritten: readable cooldowns)

```
# today: dash_strike.rules (two blocks with a copied condition) and the C++ field brawl.cd_dash,
# counted down by brawl_moves.rules, read by dash_strike_hud.rules
use if brawl.cd_dash <= 0
	change brawl.cd_dash = 3
	...
```
```
# after: the motion's own cooldown, read by the HUD
use
    cooldown 3 s
    blocked by stunned
    emit brawl.dash_strike.started
    impulse 16 replace horizontal
    move for 0.25 friction 0
    cast ray range 4 along facing
        emit brawl.dash_strike.hit
        impulse 10 on target along facing
```
```
hud Line
    text "[{key:slot2}]  DASH STRIKE  READY" if ready brawl.dash_strike.Use
    text "[{key:slot2}]  DASH STRIKE  {cooldown brawl.dash_strike.Use}s" if not ready brawl.dash_strike.Use
    bind modulate:r = 1 - 0.45 * ready brawl.dash_strike.Use
    bind modulate:b = 1 - 0.45 * ready brawl.dash_strike.Use
```

`brawl_moves.rules`' three cooldown motions and brawl's `cd_*` fields go (the combo's are §6.7.7's).

### Flight (converted: words only)

```
cbsheet 1
field flight.on bool
field flight.fuel float
action fly key T

motion FlyOn on press fly if not flight.on
    change flight.on = 1
    emit flight.started
motion FlyOff on press fly if flight.on
    change flight.on = 0
    emit flight.stopped
motion Flying while flight.on
    move walk_speed 7 sprint_speed 12 air_control 1 gravity 0 air_friction 3 move_frame 1
motion Thrust while flight.jetpack_on and held.jump and not grounded and not flight.on and flight.fuel > 0
    change flight.fuel -= 30 per second
    emit flight.thrust
    force 26 along up
```

### World reactions (converted)

```
# today
on brawl.dash_strike.started
    subject $at
    at $at/RightHand
    sound res://assets/sfx/brawl_swing.wav volume -6 jitter 0.08 range 30
```
```
# after
react on brawl.dash_strike.started
    at actor/RightHand
        sound brawl_swing volume -6 jitter 0.08 range 30
```

### The frag (converted, then `let` and `wait ... or`)

```
motion Throwing on use
    for carrier
    cooldown 0.7 s
    lasts 0.7 s
    wait for frag.release or 0.3 s
    emit frag.thrown
    launch carried speed 15 lift 3 ahead 0.8 seconds 2.5

motion Blast on expired
    on item
    let falloff = (distance < frag.radius) * (1 - distance / frag.radius)
    emit frag.blast
    break radius frag.break_radius speed frag.push seconds frag.rubble_seconds
    find Blast bodies
        impulse frag.push * falloff along away
        impulse 0.35 * frag.push * falloff along up
```

### The owner's telekinesis (new words: grab, points, toward, statuses, throw marks)

The acceptance test of milestone M290: written by the owner's description (2026-11-21), with no
C++ but the damage.

```
cbsheet 1
status pulled
event tk.pulled
event tk.struck
param pull_speed 14 m/s
param zone_reach 6 m
param throw_speed 30 m/s

# On a player: pulled into melee range, staggered (no attacks while pulled).
motion Pull on use if not active Hold
    cooldown 4 s
    cast sphere 0.4 range 18 holds players
        emit tk.pulled
        apply pulled for 0.6 s on target           # the target's attacks are `blocked by pulled`
        force pull_speed kind velocity top pull_speed on target toward owner
    lasts 0.6 s

# On the world: a sphere the player steers pulls loose bodies (a wall's pieces, broken loose) to
# its middle while the right button is held.
motion Hold while held.use2
    break radius 2 speed 2 at look zone_reach
    grab sphere 3 bodies max 8 at look zone_reach
        force 40 kind velocity top 12 toward look zone_reach

# Left button while holding: what it holds is thrown, and hurts whoever it strikes.
motion Throw on use if active Hold
    grab sphere 3 bodies max 8 at look zone_reach
        throw throw_speed along look marks tk.struck for 2 s
```

The C++ is one line: `tk.struck` (A the thrower, B the one struck, value the speed) becomes
`combat.damage`. Open for the owner: how strong, how far, how it reads on screen.

## 8. The compiler

### 8.1 Shape

A library with no Godot types, like `src/expr`: `src/sheet/`.

```
text ──lex──> tokens (with INDENT / DEDENT, each with its line and column)
     ──parse──> syntax tree (every node keeps its span: errors point at the word)
     ──resolve──> names: the sheet's declarations, the engine's words, the server's schema if given
     ──check──> the placement table, limits, units, types, files that must exist
     ──lower──> four outputs:
                  motions   -> sim::MotionSet (the structs the simulation runs: motions.h)
                  reactions   -> ReactionSet (plain data: what CbReaction nodes hold today)
                  predictions -> PredictionSet
                  hud         -> HudBindings
                  + Declarations (fields, events, actions, statuses, params) for the schema
```

| Part | Lines (guess) | Replaces |
|---|---|---|
| lexer, indentation | 400 | `Split`, `Column` in `cue_rules.cpp` |
| parser, syntax tree | 1,500 | `ParseRules` (`cue_rules.cpp`), the sheet half of `sheet_motions.cpp` |
| resolver, checker (one placement table) | 1,200 | the placement table and refusals in `cue_rules.cpp`, the name checks in `cue_names.cpp` |
| lowering | 1,200 | `BakeSheetMotion`, and `CompileMotionSet` (`motions.cpp`) once the cfg text goes |
| formatter | 400 | |
| converter (version 0 to 1) | 500 | |
| tests | 1,500 | |

### 8.2 Who calls it

| Caller | What for |
|---|---|
| **the editor** (a thin GDExtension wrapper, `CbSheet` resource) | errors as you type, colours, names offered, the inspector's parameters; building the viewer's nodes from `ReactionSet` / `HudBindings` until 6b |
| **the server** | reading every mod item's sheets when it starts: declarations into the schema, motions into motion sets (sent to clients in the schema, as today), parameters into item properties |
| **the client** | the looks of the items it has: reactions, predictions, HUD (the motions come from the server's schema, so every machine runs the server's copy) |
| **the cooker** (6b) | the sheet source goes into the cooked item as text; the reader compiles it with this library: **the compiler is the reader for sheets**, fuzzed in CI with the rest |
| **`cb_sheet`** (a command line tool) | `check`, `fmt`, `convert`, `test` ([§11](#11-tools)) |

### 8.3 What travels

- **The sheet's source, not a compiled form** (decision D4, leaning source): it is small, a person can
  read what an item does, and the compiler is the checker anyway. The server compiles the motions
  and sends them in the schema as today, so every machine runs the server's copy and a client never
  runs a motion the server did not; each client compiles the looks (reactions, predictions, HUD)
  for itself, which only it sees.
- **The motions `.cfg` text goes**: it exists to carry what `BakeSheetMotion` made from the sheet to
  the server. When the server compiles sheets itself it has no use. One milestone keeps both and
  compares them (M283), then the `.cfg` path is deleted.
- **Hashes**: a sheet's text is part of the item (named by its SHA-256 today, by the manifest in 6b).
  A changed comment is a new item, as a changed scene is today.

### 8.4 Errors

Every error has a line, a column, a reason and, where there is one, the nearest right word:

```
prefabs/pistol.cbsheet:14:23: "gun.rouns" is not a field (did you mean gun.rounds?)
prefabs/pistol.cbsheet:20:5: "force" cannot stand in a find: there a line is change, emit, impulse, apply, throw or move for
prefabs/pistol.cbsheet:31:9: "0.2 m" is a distance; cooldown takes seconds
```

A sheet with errors compiles nothing (today a rule with a mistake is left out and the rest works);
the editor shows the errors as you type, so a published sheet never has one.

## 9. What the engine has to learn

What the new words need below the compiler, with the cost each adds to the simulation.

| Word | Engine change | Cost per tick |
|---|---|---|
| item fields (`on item`) | `change` writes an item entity's board (items have boards: `gun.rounds` is one today); conditions in an item's motions read the item's fields by name | none new |
| `timer` fields | a field type that the simulation counts down each tick (players' and items' boards) | one subtraction per timer field per entity that has one |
| `emit ... value` | the event record carries a value (it does: `ModEventRecord::value`); a find's emit fills it | none |
| motion state readable | the expression reader resolves `ready/cooldown/uses/active NAME` from each motion's record; the frame capture gives the HUD the local player's records | none in the simulation |
| statuses | a small list per player (up to 8: id, ends at tick), in the snapshot and the hash; `apply` from a find; `ctx.ApplyStatus` | a list walk when motions check `blocked by` |
| `tags`, `blocked by`, `cancels` | resolved at compile time into bit masks; starting a motion checks masks against the player's active motions and statuses | a mask test |
| `on release`, `on tap`, `on hold` | the press tracking motions have (`prevActions`) plus how long an action has been down (per player, per action: a small array) | none |
| `wait for X or S` | a step's wait with a time limit that goes on instead of giving up (one flag on the step) | none |
| `every` without a cap | the repetition already comes from time; drop the constant | none |
| `at POINT`, `look N` | finds and breaks take a centre other than the owner's feet; `look N` a ray cast to stop at the level | one ray per use |
| `grab` | a held set per motion (up to 8 NetIds) like `MotionHold` for a cast, in the snapshot | its lasting lines per held body |
| `toward POINT`, `toward owner` | two more directions in `MotionFrame` | none |
| `throw ... marks` | a thrown mark on a body (who, until when, which event): a component, snapshotted; impacts of marked bodies with players say the event | one check per impact of a marked body |
| `per second` | already how `while` changes work; the flag becomes explicit | none |

Every one goes through the determinism CI (six compilers) and the hash; new snapshot components
are registered with the snapshot (and the protocol version says so: [versions.md](versions.md)).

## 10. Moving every sheet over

1. **Same output first.** The new compiler reads version 0 (today's `.rules`) and produces exactly
   what `ParseRules` and `BakeSheetMotion` produce: every shipped sheet's `motions/*.cfg` byte for
   byte, every reaction's Dictionary equal. Golden files in the tests. Only then are the old
   parsers deleted (M283).
2. **The converter** (`cb_sheet convert`) writes version 1 from version 0, mechanically: block keywords,
   spellings, scopes from the "holds below" settings, short asset names, `wait for X or S` where a
   `timeout` repeats its step's lines exactly. The compiled output of the converted sheet is
   compared with the original's: the same, or the converter stops and says where.
3. **Every reference moves**: 91 files name `.rules` today, the sheets themselves among them (scenes, docs, tools, the export filter,
   the pack check, `.gitattributes`, the SDK's starters). One milestone, one search.
4. **Rewrites come later**, each with the word that makes it shorter (item fields, cooldowns,
   combos), each changing behaviour only where a test says it should.
5. **Version 0 is refused** once nothing ships it (before launch: no stranger's sheets exist yet).
6. **The SDK's starters** and the docs (`rules-language.md` becomes `sheets.md`; `looks.md`'s rules
   section points there) move with it.

## 11. Tools

| Tool | What |
|---|---|
| `cb_sheet check FILE...` | compiles, prints errors as `file:line:col: why`; non-zero on any |
| `cb_sheet fmt FILE...` | rewrites in the one style: tabs, block order, spacing, brackets where precedence surprises (`not (a == 2)`) |
| `cb_sheet convert FILE...` | version 0 to 1 ([§10](#10-moving-every-sheet-over)) |
| `cb_sheet test FILE.cbtest` | runs a motion on a stand-in simulation (a flat floor, a player, its items) with scripted input, and checks what happened (below) |
| the editor | the `.cbsheet` script editor: colours from the compiler's tokens, errors under the word as you type, names (the sheet's, the schema's) offered, a parameter's value and unit on hover; the inspector shows parameters |
| VS Code | a small grammar (colours) for people who write sheets outside Godot, and `cb_sheet check` as its problem matcher |

**A sheet test** (**new**), plain text beside the sheet, run in CI:

```
# prefabs/pistol.cbtest
test "a press fires, an empty magazine clicks and reloads"
    item gun.pistol in hand
    set gun.rounds of item = 1
    press use at 0.0
    expect event item.use at 0.0
    expect gun.rounds of item == 0 at 0.1
    press use at 0.5
    expect event item.use_failed at 0.5
    expect event item.reload at 0.5
    expect gun.rounds of item == 12 at 2.1
```

It runs the real simulation headless (as `cb_tests` does), so it tests what players will get.

## 12. Milestones, in order

Each is done when shipped content uses it and it survived a playtest ([the rule](../ROADMAP.md#how-we-work));
each says what its tests prove. Sizes are guesses, in the M-numbers' usual size.

| # | Milestone | What | Done when |
|---|---|---|---|
| **M283** (**done**, 2026-11-21) | **One compiler, today's language** | `src/sheet/`: lexer, parser, resolver, checker, lowering for version 0, producing what the two parsers produce; `ParseRules` and `BakeSheetMotion` become calls into it; golden tests of every shipped sheet | every shipped sheet's motions cfg is byte-identical and every reaction equal through the new compiler; the old parsing code deleted; the Godot checks pass |
| **M284** (**done**, 2026-11-21: the node kept, see below the table) | **The file and the node** | `CbSheet` resource (`.cbsheet`, loader and saver), the `sheet` property on `CbItem`, `CbMotionSet`, world reactions and HUD roots (each saying which blocks it allows); `CbRules` goes; publish, bake and the pack check read it | every scene points at its sheet by resource; a renamed sheet keeps working; `CbRules` gone from the code |
| **M285** (**done**, 2026-11-22: as built, below the table) | **Version 1: one spelling** | the language of [§6.1 to 6.3, 6.6 (words), 6.8, 6.9, 6.10](#6-the-language): block keywords, indentation scopes, `wait for X or S`, word anchors, `of`, `is`, words-only logic, short asset names, `create`; `cb_sheet convert` and `fmt`; every shipped sheet converted | every shipped sheet is version 1 and compiles to the same output as before; `rules-language.md` rewritten as `sheets.md` |
| **M286** (**done**, 2026-11-22) | **Names and parameters in the sheet** | [§6.4, 6.5](#64-names-declared-in-the-sheet): `field`, `event`, `action`, `param`; the server reads them from mod items; `CbItem.properties` become parameters; the C++ looks names up; the editor needs no server build for a sheet's own names | the gun, the bat, the sword, brawl and flight declare their names in their sheets; no mod declares a name a sheet declares with another type; the pistol's numbers are in its sheet only |
| **M287** (**done**, 2026-11-22: timers, `wait for X or S` anywhere, `emit ... value`, brawl's cooldowns and combo; an item's fields in its motions, readable motion state in motions (not the HUD yet), the guns' rounds and reload, the bat's heat) | **State in the sheet** | [§6.4 item fields, `timer`; 6.7.3 `emit value`; 6.7.5 readable motion state](#6-the-language); the pistol's rounds and reload (`emit`, `on EVENT`, `wait`), the bat's heat (`field melee.hot_left timer on item`, set in its find), brawl's cooldowns and combo window move into sheets | `gun.cpp` keeps only the shot, the damage and the aim; `melee.cpp` only damage and facing; `brawl_moves.rules` and brawl's `cd_*` fields gone; the HUD lines read cooldowns; the `rifle`, `melee`, `brawl` tests pass |
| **M288** (**done**, 2026-11-22; the fist's window was a timer in M287) | **How skills meet** | [§6.7.1 tags, `blocked by`, `cancels`, shared cooldowns, `on release / tap / hold`; 6.7.6 statuses; 6.7.7 combos as timed motions](#6-the-language) | the fist's window is a timer (no C++); the sword's stun is a status; the guard and the block block attacks by tag; the bots still fight (their test passes) |
| **M289** (**done**, 2026-11-22; a scene's own numbers in the inspector not made: a sheet of two lines does it) | **One sheet, many items** | [§6.11](#611-one-sheet-for-many-items): `uses`, parameter overrides, `replace`, `let` | the pistol and the rifle share `gun.cbsheet`; the fist's box written once |
| **M290** (**done**, 2026-11-22; the telekinesis is the brawl kit's sixth skill until the owner's look is made; `at target` and a cast in a later step not made) | **Where and on what** | [§6.7.4](#674-finding-holding-and-where): `of target`, points (`look N`, `hit`, markers), `find`/`break at`, `players first`, `grab`, `toward`, `throw ... marks`; a cast in a later step | the telekinesis sheet of §7 works in a playtest (the owner's weapon, or a test mod until the owner's look is there); the bat finds with `players first` in one find |
| **M291** | **Tools** | `cb_sheet test` and `.cbtest` files in CI; errors as you type in the editor; the formatter in the editor; the VS Code grammar | every shipped item has a `.cbtest`; a mistake typed in the editor is underlined before saving |
| **M292** | **The rest** | curves as resources (the frag's falloff the first user); text and rows from the server ([§6.12](#612-text-and-rows-from-the-server-hytale-proposal-3)) with the shop as its first user; version 0 refused | no `.rules` anywhere; the shop's offers written once |

**M284 as built** (2026-11-21): the `.cbsheet` file and the `CbSheet` resource (the extension's own
loader and saver), the editor's Sheet screen, and every scene holding its sheet as a resource; but
**`CbRules` stays**, holding the sheet (`sheet`) instead of naming it (`file`). A `sheet` property on
the roots would have needed new root classes for world reactions (a plain `Node3D` today) and HUDs
(a `CanvasLayer`), and `CbRules` is also the place the reactions it makes hang under (a `Node3D`:
where a rule with no place of its own puts things). Which blocks a root allows (a HUD's sheet with
a motion) is checked where the parser learns the block keywords (M285).

**M285 as built** (2026-11-22): version 1 is read by **lowering** it to version 0, line for line
(`sheet_v1.h`), so the two cannot mean different things and every shipped sheet converted
(`cb_sheet convert`) compiles to exactly what it did: the baked motions byte for byte, every
reaction's properties (compared with no line numbers and keys in order). The lexer, syntax tree and
resolver of §8.1 come with the words that need them (M286 on). In version 1 now: `cbsheet 1`;
`react`, `hud`; scopes `as`, `at`, `after`, `chance`; `cooldown`, `server only`; `if actor is prop`;
`create` and `sound` with short names; `shake`/`flash ... for S`; `wait for X [within S | or S]`,
`wait S`; `for holder|carrier`, `on item`; `known`, `of`, `local`, `active`; paths from `holder`,
`actor`, `target`, `me`, `local`, `world`. Not yet: `side`, `wait off`, `at here` (no shipped sheet
says them); `wait for X or S` only as a motion's last wait (the engine's flag that goes on after a
wait is M287's: the fist, the bat and the sword keep `within` and `timeout`); version 0's signs
inside a HUD format's quotes; a reaction's default subject from where its sheet sits (`as` is
written). Units (D8) and `per second` come with the declarations (M286). The sheet's uid: none
(a scene names a sheet by its path; an id in a mod's exported scene was a warning in the game).

**Order, and why.** M283 first: nothing else is safe while two parsers disagree. M284 and M285 before
any new word: new words go into the new language only. M286 before M287: state needs declarations.
M287 and M288 are where most C++ disappears (the slice's kit). M289 after them: sharing is easier
when the shared things are final. M290 is the owner's weapons. M291 can run beside M287 to M290
(the tests make those safer); listed here as one so it is not forgotten.

**What a playtest checks in this step**: the slice's kit, the sword, the bat and the guns feel the
same after every converted milestone (M283 to M286) and the same or better after the rewrites
(M287 to M289); the owner's weapons in M290.

## 13. Decisions for the owner

| # | Question | Options | Leaning |
|---|---|---|---|
| D1 | The simulated block's keyword | `ability` / keep `motion` | **decided (2026-11-21): `motion`**, for every simulated block (skills, refuels, rounds, blasts); a separate block type for non-skill state may come later |
| D2 | Who an event is about, as words | `actor` and `target` / `source` and `target` / `a` and `b` | **decided (2026-11-21): `actor` and `target`** (and `holder`, `me`, `local`, `world` for the other anchors) |
| D3 | Logic's symbols (`!`, `&&`, `\|\|`) | refused / kept beside the words | **decided (2026-11-21): kept**: `!a` and `not a` both, as today. (The path symbols `^^`, `$at` go with D2) |
| D4 | What an item carries | the sheet's source / a compiled form | **decided (2026-11-21): the source**, checked by the compiler on the modder's machine when the item is cooked, and again by the store and by every game that loads it; motions compiled once by the server, looks once by each client, when the item loads (well under a millisecond a sheet; nothing per tick). A server may override an item's parameters as options ([§6.5](#65-parameters)) |
| D5 | The word that puts a visual thing in the world (today `scene`) | `effect` / `spawn` / `create`; one word for sounds too, or `sound` kept | **decided (2026-11-21): `create`** for anything visual placed for a while (an effect, a prompt, a light), which may carry its own sounds; **`sound`** kept for a lone sound; `play` for animations. One word for everything is refused: `jump` is both `assets/sfx/jump.wav` and `vfx/jump.tscn` in the game today, and a sound's options are not a visual's |
| D6 | Combos as a word or as motions with timers | | **decided (2026-11-21): timed motions** (§6.7.7); their windows from the clip in step 7 |
| D7 | The same status applied twice | the newer replaces / the longer wins / they add | **decided (2026-11-21): the longer wins** (most statuses will be a server mod's: `ctx.ApplyStatus`) |
| D8 | Units on numbers (`0.4 s`, `16 m/s`) | optional and checked / not at all | **decided (2026-11-21): optional and checked**: the compiler checks a unit against the word (`cooldown 0.4 m` is an error) and changes no value; a number without one means what it means today |
| D9 | One motion starting another | `start NAME` / `trigger NAME` / `emit` and `on EVENT` | **decided (2026-11-21): no new word**: `emit` an event, the other motion starts `on` it (a tick later); `emit` stays. ("trigger" is already the docs' word for `on use`, `on press`, `while`) |
| D10 | A sheet that may say `apply` on another player | statuses only / fields too | **decided (2026-11-21): statuses only**; fields of another player (health) stay a server mod's. Not a cheat: the server runs the same sheet and its result wins; a client changing its copy only mispredicts its own screen |
| D11 | Tests beside sheets (`.cbtest`) | in step 6 / later | **decided (2026-11-21): in step 6** (M291) |
| D12 | Who plans and builds | | **decided (2026-11-21): Claude builds it** |

## 14. Risks

| Risk | What we do |
|---|---|
| **A converted sheet behaves differently** | compiled output compared for every shipped sheet at every conversion; golden files in CI; a playtest per milestone |
| **The step takes long** (ten milestones before 4c and the alpha) | M283 to M285 are mechanical; M286 to M290 each delete C++; the owner can stop after M288 and move M289 to M292 after the alpha |
| **New words cost rollback time** | every new piece of state is bounded (8 statuses, 8 held) and in the snapshot; the crowd bench (`--fake-players`) before and after M287 and M288 |
| **Editor integration** (a script editor for a non-script resource) | the editor already opens `.rules` coloured with names (M166 and on); `CbSheet` keeps that path |
| **Names in two places during the move** (C++ and sheets) | M286 makes a mismatch an error at server start; the bots mod's lookup pattern for C++ |
| **Modders' sheets break** | none exist outside this repository yet; version 0 is refused only once nothing ships it |
| **The design is wrong in a place** | each word comes with the shipped content that needs it, not before (the ROADMAP's rule); the version line lets a later version change a word |

---

## Appendix A: grammar

Version 1, in the style of today's (docs/rules-language.md). `{ }` many, `[ ]` optional, `|` one
of. INDENT and DEDENT are the lexer's (one rule, §6.3).

```
sheet       := "cbsheet" INT NEWLINE { decl | block }
decl        := "field" NAME type [ "on" ( "player" | "item" | "world" ) ] [ "shared" | "private" ] [ "=" expr ]
             | "event" NAME | "action" NAME "key" KEY | "status" NAME
             | "param" NAME number [ unit ] | "let" NAME "=" expr | "uses" PATH
             | "replace" block
type        := "int" | "float" | "bool" | "timer" | "text"
block       := motion | use | react | predict | hud
motion     := "motion" NAME trigger [ "if" expr ] NEWLINE INDENT { setting } body DEDENT
use         := "use" [ "if" expr ] NEWLINE INDENT { setting } body DEDENT
trigger     := "on use" | "on" ( "press" | "release" | "tap" ) ACTION | "on hold" ACTION [ number [ unit ] ]
             | "on" EVENT | "while" expr | "when" expr
setting     := "cooldown" amount [ "shared" NAME ] | "uses" INT [ "refill" ( "ground" | "after" amount ) ]
             | "lasts" amount | "until" expr | "tags" names | "blocked by" names | "cancels" names
             | "for" ( "holder" | "carrier" ) | "on item"
body        := { line } { wait { line } }
wait        := "wait" amount | "wait for" expr [ ( "or" | "within" ) amount ]
line        := "change" FIELD ( "=" | "+=" | "-=" ) amount [ "per second" ]
             | "emit" EVENT [ "value" amount ]
             | "impulse" amount { effect-option } | "force" amount { effect-option }
             | "move" [ "for" amount ] { PARAM amount } | "link" { link-option }
             | "launch" ( "new" KIND | "carried" ) { launch-option } | "follow" NODE "over" amount
             | "break" { break-option } | "apply" STATUS "for" amount [ "on" who ]
             | "throw" amount "along" direction [ "marks" EVENT "for" amount ]
             | "remove self" | "end" | "let" NAME "=" expr
             | "if" expr NEWLINE INDENT body DEDENT [ "else" NEWLINE INDENT body DEDENT ]
             | "every" amount NEWLINE INDENT body DEDENT
             | "timeout" NEWLINE INDENT body DEDENT
             | ( "find" | "grab" ) shape { find-option } NEWLINE INDENT { line } DEDENT
             | "cast" shape "range" number { cast-option } NEWLINE INDENT { line } DEDENT
react       := "react" ( "on" EVENT | "while" expr ) [ "if" expr ] NEWLINE INDENT { rsetting } { rline } DEDENT
rsetting    := "cooldown" amount | "server only"
rline       := ( "at" place | "as" subject | "after" amount | "chance" number ) NEWLINE INDENT { rline } DEDENT
             | "sound" ASSET { sound-option } | "create" ASSET [ "under" PATH ] [ "for" amount ]
             | "play" PATH ANIMATION | "set" PATH PROPERTY value [ "blend" amount ]
             | "call" PATH METHOD { value } | "shake" amount [ "for" amount ] | "flash" COLOR [ "for" amount ]
predict     := "predict" ACTION "as" EVENT [ "if" expr ] NEWLINE INDENT { "cooldown" amount | "held" | "change" FIELD op amount } DEDENT
hud         := "hud" PATH NEWLINE INDENT { "show" [ "if" expr ] | "text" STRING [ "into" PROPERTY ] [ "if" expr ]
             | "bind" PROPERTY "=" expr [ "if" expr ] } DEDENT
place       := "point" | "end" | "beam from" PATH [ "to end" ] | PATH | "follow" | "look" amount | "hit"
subject     := "me" | "holder" | "actor" | "target" | "either" | "local" | "world" | PATH
who         := "self" | "target" | FIELD
direction   := "look" | "move" | "facing" | "up" | "to" | "away" | "world" vec3 | NODE
             | "toward" ( point | "owner" ) [ "turn" vec2 ]
point       := vec3 | "look" amount | "hit" | NODE | "target"
amount      := number [ unit ] | expr                          (an expression up to the next option word)
unit        := "s" | "m" | "m/s" | "deg"
expr        := (the expression language: §6.6)
```

## Appendix B: every old word and its new one

| Version 0 (`.rules`) | Version 1 (`.cbsheet`) |
|---|---|
| (no header) | `cbsheet 1` |
| `on EVENT` / `while COND` (at the top of a sheet) | `react on EVENT` / `react while COND` |
| `show PATH ...`, `text PATH ...`, `bind PATH ...` | `hud PATH` with `show ...`, `text ...`, `bind ...` under it |
| `then when COND` | `wait for COND` |
| `after S` (a step) | `wait S` |
| `then when X within S` + `timeout` repeating the step's lines | `wait for X or S` |
| `item hand` / `item carried` | `for holder` / `for carrier` |
| `owner item` | `on item` |
| `found`, `hit` (who) | `target` |
| `world direction (x, y, z)` | `world (x, y, z)` |
| `!a`, `a && b`, `a \|\| b` | unchanged (D3: both spellings kept) |
| `?name`, `!?name` | `known name`, `not known name` |
| `^^:combat.dead`, `$other:combat.health` | `combat.dead of holder`, `combat.health of target` |
| `is_local` | `local` |
| `subject ^` / `^^` / `$at` / `$other` / `$local` / `$world` | `as me` / `as holder` / `as actor` / `as target` / `as local` / `as world` (or the sheet's default) |
| `side A` / `B` / `Either` | `as actor` / `as target` / `as either` |
| `kind prop`, `template NAME` | `if actor is prop`, `if actor is NAME` |
| `delay S` | `after S` (a scope) |
| `wait` / `wait off` | `server only` / (nothing) |
| `at here` | (nothing) |
| `at X` holding for the actions below it | `at X` with the actions indented under it |
| `scene res://vfx/NAME.tscn [under N] life S` | `create NAME [under N] for S` |
| `sound res://assets/sfx/NAME.wav ...` | `sound NAME ...` |
| `$at/RightHand`, `$other/Head`, `^^/Head` | `actor/RightHand`, `target/Head`, `holder/Head` |
| `motion.NAME` | `active NAME` (`motion.NAME` read until M285's conversion) |
| `change x -= 30` in a `while` (per second, silently) | `change x -= 30 per second` |
| `CbRules` node + `file` | the node's `sheet` property (`CbSheet` resource) |
| C++ `declare.Field/Event/Action` for a sheet's names | `field`, `event`, `action` in the sheet |
| `CbItem.properties` (`gun.damage`) | `param damage 25` |
| `CbItem.fields` (`gun.rounds: 12`) | `field gun.rounds int on item = magazine` |

## Appendix C: for whoever builds it

**Where things are today**

| What | File |
|---|---|
| the sheet parser for looks and the placement table | `src/godot/cue/cue_rules.cpp` (`ParseRules`, `LineRuleOf`, `Misplaced`, `ParsePart`, `ParseAt`) |
| a sheet's motion blocks baked to text | `src/godot/sheet_motions.cpp` (`BakeSheetMotion`), `src/godot/cinderbox_motion.cpp` (`CbMotionSet::Bake`) |
| the simulation's reading of the text | `src/sim/motions.cpp` (`CompileMotionSet`), the data model `src/sim/motions.h` |
| running motions | `src/sim/simulation.cpp` (`RunMotions`, `RunFind`, `RunBreaks`, `RunItemMotions`, `EndOf`, `ImpulseEnd`) |
| a motion's record | `src/sim/components.h` (`MotionSlot`, 28 bytes; `MotionState`) |
| expressions | `src/expr/expr.h`, `expr.cpp`; the simulation's copy in `src/sim/anim_graph.h` |
| names for the editor | `src/godot/cue/cue_names.cpp`, `cinderbox_names.cfg` written by the server build (`--dump-names`) |
| the viewer's nodes | `src/godot/cue/cue_reaction.cpp`, `cue_prediction.cpp`, the HUD's `CbFieldBinding` |
| the editor's script editor for sheets | `godot/addons/cinderbox_maps/` (the sheet editor, `check_sheet_editor.gd`) |
| a mod's item read by the server | `src/server/look_item.*`, `character_item.cpp`, `ServerOptions::lookItems`, `loadMotions` |
| the checks | `check_rules.gd`, `check_motion_blocks.gd` (+ `.rules`), `check_predictions.gd`, `check_names.gd`, `check_sheet_editor.gd`; `cb_tests`, `cb_net_tests` |

**Invariants to keep green at every milestone**

- The determinism CI (six compilers) and the reference hashes; `cb_tests`, `cb_net_tests`, the Godot
  checks, the sanitizers.
- Every published item republished when its sheet changes (`tools\publish_mod.ps1`); the rifle's
  files are local and git-ignored (licensed): its sheet too (`server_mods/gun/client/prefabs/rifle.rules`
  today).
- Protocol and replay versions raised, with a row in `docs/versions.md`, when the simulation's state
  or what travels changes (statuses, grab sets, thrown marks, the schema's form).
- The docs say what the code does: `docs/sheets.md` (from `rules-language.md`), `looks.md`,
  `motions.md`, `items.md`, `server-mods.md`, the SDK's README.

**How to know a conversion is right**: compile every shipped sheet with the old path and the new one
and compare the outputs (the cfg text and the reaction Dictionaries until M283 ends; the compiled
`MotionSet` and `ReactionSet` structures after), in a test that runs in CI.
