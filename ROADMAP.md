# Cinderbox — Roadmap

What comes next, in order, and why. [DESIGN.md](DESIGN.md) says how things work today;
[docs/HISTORY.md](docs/HISTORY.md) is what is done. A step that is done moves out of here.

## The order

| # | Milestone | In one line |
|---|---|---|
| 1 | [Motions: on a press](#1-motions-on-a-press) | `CbMotion`: a dash and a double jump, predicted |
| 2 | [Motions: while](#2-motions-while) | flight, a glide, a jetpack: parameters that hold while conditions do |
| 3 | [Motions: tethers](#3-motions-tethers) | a grappling hook: the throw and the pull, predicted |
| 4 | [Motions in the editor](#4-motions-in-the-editor) | a preview, checks at publish, the body and the looks follow a motion |
| 5 | [The client, finished](#5-the-client-finished) | what is left of the client's known limits |
| 6 | [The SDK knows the server](#6-the-sdk-knows-the-server) | the Godot modding project scaffolds the server half and knows its names |
| 7 | [Mod testing](#7-mod-testing) | the client starts a server by itself: one button from an edit to playing it |

Movement parameters, the step before these, are done (M92:
[docs/server-mods.md](docs/server-mods.md#movement-parameters)). Steps 1 to 4 are one idea, below. Steps 6 and 7 are last on purpose: they wrap the client, so they
wait until it stops changing.

## The problem motions solve

| Today | Why |
|---|---|
| WASD, sprint and jump are predicted | they are the simulation's: every client runs them from its own input |
| Anything a mod adds to movement is not | a mod's `Push` is a command: it exists only in the server's frame, a round trip later |
| A pull is the worst case | a command every tick: the client is corrected every tick and its own position trails by the lead |

A dash that starts 150 ms after the key, or a grapple that tugs in steps, is not a look problem.
`CbPrediction` cannot fix it: the viewer has no simulation, so a guessed push cannot collide with a
wall. Movement is predicted only when **the simulation itself** computes it from input.

## The idea: `CbMotion`, a reaction for the mover

A `CbReaction` says: *on a cue, or while conditions hold, do this to the look.*
A `CbMotion` says: *on a press, or while conditions hold, do this to the mover.*
Same shape, same expression language, same editor. The difference is where it runs.

| | `CbReaction` | `CbMotion` |
|---|---|---|
| Answers | what the look does | what the mover does |
| Authored | a node in the mod's look | a node in `motions/<mod>.tscn`, in the same Godot project |
| When | **On a cue**, **While** | **On a press**, **On a cue**, **While** |
| Conditions | expressions over entity state | the same language over simulation values (the state machine's names) |
| Timing | `cooldown`, `delay`, `chance` | `cooldown`, `uses`, `duration` |
| Does | animation, property, scene, sound | impulse, parameters, tether; changes fields; emits an event |
| Runs in | the viewer | every simulation: baked, sent in the schema, like a state machine |
| Rolled back | no | yes |

```
CbMotion nodes (Godot) ──bake──> motions/<set>.cfg ──> the server reads it from the mod's item
                                                              │ the schema
        your press ──> your simulation runs the motion now <──┤
                       the server runs the same motion     <──┘   same input, same state, same result
```

- **Why it is predicted**: a press is already in `PlayerInput`, and the client already simulates
  its own input ahead of the server. A motion is a function of input and state, so the predicted
  ticks contain it. No command is involved, so there is nothing to wait for.
- **Why it is not a rule on the client**: it is the path the `AnimationTree` already takes.
  Authored in Godot, baked to text, names resolved at bake, run by the simulation as a small
  program. The server reads its own copy; a client cannot change what it was sent.
- **Rules stay in the mod**: the server mod declares the action and the fields, and decides who may
  (`dash.charges`, `flight.on`, an item kind). The motion is the mechanism it switches on.

### The node (proposal)

| Field | Meaning |
|---|---|
| `when` | **On a press** (an action goes down), **On a cue** (a mod event at this player), **While** (its conditions hold) |
| `action` | an action a mod declared (`dash`), or the engine's `jump` / `sprint` |
| `conditions` | expressions: `grounded`, `airborne_time`, `speed`, `vertical_speed`, board fields, item kinds, stances, `held:<action>`, `motion.<name>` |
| `cooldown` | seconds between two uses |
| `uses`, `refill` | how many before a refill: **on the ground** (a double jump: 1) or every N seconds |
| `duration` | how long a pressed motion stays on (a dash's 0.2 s without friction) |
| `impulse`, `impulse_frame` | a change of velocity along the **look**, the **move** input (the facing when there is none), **up**, the **world**, or the **tether** |
| `replace` | which part of the velocity the impulse replaces instead of adding to: none, vertical, all |
| `parameters` | [movement parameters](docs/server-mods.md#movement-parameters) while it is on: `gravity = 0`, `air_control = 1`, `max_speed = 12`, `move_frame = look` |
| `tether_*` | range, travel speed, rope length, pull, reel: see [step 3](#3-motions-tethers) |
| `changes` | board fields, as `CbPrediction.changes` writes them: `dash.charges -= 1`, `jetpack.fuel -= 20 per second` |
| `emits` | a mod event (`dash.started`): reactions play on it, state machines enter on it, server mods hear it |

### What mods would write

| Move | Motion | Server mod |
|---|---|---|
| Dash | on a press of `dash`; `dash.charges > 0 and !combat.dead`; cooldown 1; impulse 12 along **move**; duration 0.2 with `friction = 0`; changes `dash.charges -= 1`; emits `dash.started` | declares `dash` and `dash.charges`, refills charges |
| Double jump | on a press of `jump`; `!grounded`; uses 1, refill on the ground; impulse 6.5 **up**, replace vertical; emits `jumped` | none, or a field that switches it on |
| Flight | while `flight.on`; parameters `gravity = 0`, `air_control = 1`, `move_frame = look` | toggles `flight.on` |
| Glide | while `held:jump and !grounded and vertical_speed < 0`; parameters `gravity = 2`, `max_fall = 3` | none |
| Jetpack | while `held:jump and jetpack.fuel > 0`; impulse 25 per second **up**; changes `jetpack.fuel -= 20 per second` | refills fuel on the ground |
| Grappling hook | on a press of `fire`, `grapple.gun`; tether range 40, travel 60 m/s, pull 30; ends when `fire` is let go | owns the item |

### What the simulation gains

| Piece | Note |
|---|---|
| Reads action bits | only through baked motions; today it never looks at them |
| `MotionState` per player | per motion: last use, uses left, on since; one tether (what it holds, where, how long). Fixed size, hashed, rolled back |
| Field writes from inside | `changes` write the board in the simulation; today only `Set` commands do. Commands still apply first |
| A ray of its own | for the tether: against the world, props and players' capsules. Not hitboxes: poses are not in the rolled-back state |
| `motion.<name>` | true while a motion is on: a state machine enters a dash clip on it, a look draws a rope on it |

### Open questions

| Question | Leaning |
|---|---|
| Other players' presses are guessed by repeating their last input, so their dash is seen late and corrected | accept: it is what a jump does today, and the mirror fades the correction |
| A field both a motion and a server mod write | allow; the bake names the fields a set writes, and the server warns when a mod `Set`s one every tick |
| A motion the server starts (a knockback that locks control) | a `StartMotion` command, after step 2: not predicted, but simulated from then on |
| A grapple onto a limb | decided: no. The anchor is on the capsule; hitboxes are the server's |
| Reactions on a motion's event | need no `CbPrediction`: the event is in the predicted simulation and a rollback un-counts it |

### Routes not taken

| Route | Why not |
|---|---|
| The viewer guesses the command (`CbPrediction` with a push) | the viewer has no simulation: a guessed push cannot collide, and a pull would fight every frame's correction |
| Server mods also run on clients | tried as M55 and dropped: rules would ship to clients and every mod would need to be deterministic |
| A built-in dash, a built-in grapple | rules in the engine; mods could only tune what the engine thought of |

## The steps

### 1. Motions: on a press

| Piece | What |
|---|---|
| `CbMotion` (the viewer extension) | the node, its inspector help, its bake to `motions/<set>.cfg` on save and at publish |
| `declare.Motions( "dash.moves" )` | the server reads the set from the mod's item and puts it in the schema |
| The simulation | compiles the set as it does a state machine; runs on-press motions before the mover: `impulse`, `replace`, `cooldown`, `uses`, `duration`, `changes`, `emits` |
| Examples | a `dash` mod; a double jump in it |

- **Done when**: with 100 ms each way, the local player's dash starts on the tick of the press and the client is not corrected for it (`net_dash`: no rollback on the dasher's own state); recordings replay it.

### 2. Motions: while

| Piece | What |
|---|---|
| While motions | on for as long as their conditions hold; `parameters` laid over the player's own (above a mod's `SetMove`, while the motion lasts) |
| `held:<action>` | an action's held state as a condition |
| `move_frame = look` | a new parameter: WASD along the camera, pitch included (flight, swimming); today it is always the ground plane |
| Per-second values | `impulse ... per second`, `changes ... per second` |
| Examples | flight, a glide, a jetpack with fuel |

- **Done when**: flying into a wall, landing and running out of fuel are all without corrections for the local player.

### 3. Motions: tethers

| Field | Meaning |
|---|---|
| `tether_range` | how far the ray from the line of sight reaches (the two traces `CastAim` does, in the simulation) |
| `tether_travel` | the hook's speed: it holds after distance / speed, and the look draws it flying meanwhile |
| `tether_length` | the rope: beyond it the outward part of the velocity is taken away (a swing); 0: no rope |
| `tether_pull`, `tether_reel` | acceleration toward the anchor; metres of rope taken in a second |
| `tether_ends` | conditions that let it go (`!held:fire`), besides the anchor disappearing |

- An anchor on a prop is a point on that body: the pull moves both.
- The viewer is told the tether (`ViewFrame`), and a reaction can place a beam to it (`place`: **Tether**).
- **Done when**: a `grapple` mod swings and reels with no corrections for the local player at 100 ms, and two players pulling one crate agree.

### 4. Motions in the editor

| Piece | What |
|---|---|
| Motion Preview | a panel like Cue Preview: a capsule on a small stage, press an action, set a field, watch the path |
| Publish checks | a motion that reads a name nobody declares, or writes a field it may not, stops the publish |
| The body follows | `motion.<name>` and a motion's event in state machine conditions; the viewer's lead covers them |
| Animation packs | a pack's layer can be asked for by a motion while it is on (a flight pose) |

### 5. The client, finished

From DESIGN.md's known limits. Each is small; together they are "the client is near done".

| Candidate | Today | Would be |
|---|---|---|
| A wrong guess is taken back | a reaction played on a prediction the server never answers stays played | a prediction that expires stops what it started |
| A swing seen from its start | behind latency another player's swing is first seen a little way in, and keys before that point do not fire | the skipped keys fire on the first frame it is seen |
| Checked pack layers | a pack's layer may read what the lead cannot predict | the bake says so |
| Key rebinding | actions are bound to the keys mods suggest | a page in the settings |
| A server list | join by address | recent and favourite servers with their mods and players |

### 6. The SDK knows the server

Today the Godot modding project knows nothing of the mod's C++ half: names are typed twice and a
wrong one is silent. This step joins the two.

| Piece | What |
|---|---|
| The server half is scaffolded | `sdk.ps1 -New <mod>` also writes `server_mods/<mod>/<mod>.cpp`: declarations that match the starter scenes |
| The schema reaches the editor | `cb_server --dump-schema` writes the fields, events, actions, stances and item kinds; the extension completes them in `CbReaction`, `CbPrediction`, `CbMotion` and tree conditions, and warns on unknown ones |
| Publishing checks names | a look that names what its mod does not declare stops the publish |
| An item in the hand | a preview of the item on the placeholder skeleton, grips solved, a pack playing |
| A prebuilt SDK | CI builds the extension, so a mod's look needs no compiler |

### 7. Mod testing

One step from an edit to playing it: the client starts the server.

| Piece | What |
|---|---|
| The client hosts | "Test" in the menu and `--host-local`: the game starts `cb_server` with the chosen mods, joins it, and stops it when it leaves |
| From the SDK | a **Test mod** button: publish the look, build the server if the `.cpp` changed, start the game hosting |
| Looks reload | a published look is taken up without restarting the server or the game |
| Bots | `--bots N` on the hosted server, so a mod can be tried alone |

- It waits for step 5: it wraps the menu, joining and pack loading, which should be settled first.

## Later, not scheduled

| What | Needs |
|---|---|
| Vehicles | joints in the component registry and templates of several bodies; a seat (the mover off, the player carried); input routed to motors, as a motion routes it to the mover |
| Server mods without a relink | mods as libraries or scripts: they need no determinism, so any language works |
| Teams, spectators | mods, once private fields and the board cover them |

## Housekeeping

Independent of the path; each is small.

| What | Note |
|---|---|
| A board sized by the schema | `kBoardSlots` is a wall that came back once already; motions will declare more fields |
| Expressions in `changes` | `pistol.ammo -= 1` takes a number; the right side could be an expression. Shared by `CbPrediction` and `CbMotion` |
| HUD nodes as reactions | `CbFieldBinding` and a While `CbReaction` with a `value_expression` do the same thing in two places |
| The rest of `simulation.cpp` in modules | props, ragdolls, items and animation; the mover went first (M92) |
| Tools on every platform | the publish, pack and SDK scripts are PowerShell |
| The old `build/` folder in the checkout | builds go outside the source tree now; the folder is ignored and can be deleted |
