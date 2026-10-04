# Cinderbox — Roadmap

What comes next, in order, and why. [DESIGN.md](DESIGN.md) says how things work today;
[docs/HISTORY.md](docs/HISTORY.md) is what is done. A step that is done moves out of here.

## The order

| # | Milestone | In one line |
|---|---|---|
| 1 | [Motions: while](#1-motions-while) | flight, a glide, a jetpack: motions that hold while conditions do |
| 2 | [Motions: tethers](#2-motions-tethers) | a grappling hook: the throw and the pull, predicted |
| 3 | [Motions in the editor](#3-motions-in-the-editor) | a preview, checks at publish, the body follows a motion |
| 4 | [The client, finished](#4-the-client-finished) | what is left of the client's known limits |
| 5 | [The SDK knows the server](#5-the-sdk-knows-the-server) | the Godot modding project scaffolds the server half and knows its names |
| 6 | [Mod testing](#6-mod-testing) | the client starts a server by itself: one button from an edit to playing it |

Done before these: movement parameters (M92) and motions on a press (M93: a dash, a double jump;
[docs/motions.md](docs/motions.md)). Steps 1 to 3 finish that idea. Steps 5 and 6 are last on
purpose: they wrap the client, so they wait until it stops changing.

## Where motions stand

A `CbMotion` says: *on a press, if conditions hold, do this to the mover.* It is baked, sent in
the schema and run by every simulation, so a player's own are predicted and rolled back
([docs/motions.md](docs/motions.md)).

| | Today (M93) | Still to come |
|---|---|---|
| When | on a press: a mod's action, `jump`, `sprint` | **While** conditions hold; **On a cue** (a mod event at the player) |
| Conditions | what a state machine reads | `held:<action>`, `motion.<name>` |
| Does | an impulse; parameters for a duration; changes of fields; an event | per-second impulses and changes; `move_frame`; a tether |
| Timing | `cooldown`, `uses` with a refill, `duration` | |
| Editor | the nodes, their help, the bake, warnings | a preview panel; names checked at publish |

### What mods would write with the rest

| Move | Motion | Server mod |
|---|---|---|
| Flight | while `flight.on`; parameters `gravity = 0`, `air_control = 1`, `move_frame = look` | toggles `flight.on` |
| Glide | while `held:jump and not grounded and vertical_speed < 0`; parameters `gravity = 2`, `max_fall = 3` | none |
| Jetpack | while `held:jump and jetpack.fuel > 0`; impulse 25 per second **up**; changes `jetpack.fuel -= 20 per second` | refills fuel on the ground |
| Grappling hook | on a press of `fire`, `grapple.gun`; tether range 40, travel 60 m/s, pull 30; ends when `fire` is let go | owns the item |

### Open questions

| Question | Leaning |
|---|---|
| Other players' presses are guessed by repeating their last input, so their dash is seen late and corrected | accept: it is what a jump does today, and the mirror fades the correction |
| A field both a motion and a server mod write | allowed today (commands apply first, then motions); the bake could name the fields a set writes, and the server warn when a mod `Set`s one every tick |
| A motion the server starts (a knockback that locks control) | a `StartMotion` command, after step 1: not predicted, but simulated from then on |
| A grapple onto a limb | decided: no. The anchor is on the capsule; hitboxes are the server's |
| A second jump that plays the jump's animation | the character's tree enters its jump state on the motion's event; a starter for it comes with step 3 |

### Routes not taken

| Route | Why not |
|---|---|
| The viewer guesses the command (`CbPrediction` with a push) | the viewer has no simulation: a guessed push cannot collide, and a pull would fight every frame's correction |
| Server mods also run on clients | tried as M55 and dropped: rules would ship to clients and every mod would need to be deterministic |
| A built-in dash, a built-in grapple | rules in the engine; mods could only tune what the engine thought of |

## The steps

### 1. Motions: while

| Piece | What |
|---|---|
| `when` | a choice on the node: **On a press** (today's), **While**, **On a cue** |
| While motions | on for as long as their conditions hold; their `parameters` hold that long |
| `held:<action>` | an action's held state as a condition |
| `move_frame = look` | a new movement parameter: WASD along the camera, pitch included (flight, swimming); today it is always the ground plane |
| Per-second values | `impulse ... per second`, `changes ... per second` |
| Examples | flight, a glide, a jetpack with fuel |

- **Done when**: flying into a wall, landing and running out of fuel are all without corrections for the local player.

### 2. Motions: tethers

| Field | Meaning |
|---|---|
| `tether_range` | how far the ray from the line of sight reaches (the two traces `CastAim` does, in the simulation) |
| `tether_travel` | the hook's speed: it holds after distance / speed, and the look draws it flying meanwhile |
| `tether_length` | the rope: beyond it the outward part of the velocity is taken away (a swing); 0: no rope |
| `tether_pull`, `tether_reel` | acceleration toward the anchor; metres of rope taken in a second |
| `tether_ends` | conditions that let it go (`!held:fire`), besides the anchor disappearing |

- The simulation gets a ray of its own for it: against the world, props and players' capsules.
- An anchor on a prop is a point on that body: the pull moves both.
- The viewer is told the tether (`ViewFrame`), and a reaction can place a beam to it (`place`: **Tether**).
- **Done when**: a `grapple` mod swings and reels with no corrections for the local player at 100 ms, and two players pulling one crate agree.

### 3. Motions in the editor

| Piece | What |
|---|---|
| Motion Preview | a panel like Cue Preview: a capsule on a small stage, press an action, set a field, watch the path |
| Publish checks | a motion that reads a name nobody declares, or writes a field it may not, stops the publish |
| The body follows | `motion.<name>` and a motion's event in state machine conditions; the viewer's lead covers them |
| Animation packs | a pack's layer can be asked for by a motion while it is on (a flight pose) |

### 4. The client, finished

From DESIGN.md's known limits. Each is small; together they are "the client is near done".

| Candidate | Today | Would be |
|---|---|---|
| A wrong guess is taken back | a reaction played on a prediction the server never answers stays played | a prediction that expires stops what it started |
| A swing seen from its start | behind latency another player's swing is first seen a little way in, and keys before that point do not fire | the skipped keys fire on the first frame it is seen |
| Checked pack layers | a pack's layer may read what the lead cannot predict | the bake says so |
| Key rebinding | actions are bound to the keys mods suggest | a page in the settings |
| A server list | join by address | recent and favourite servers with their mods and players |

### 5. The SDK knows the server

Today the Godot modding project knows nothing of the mod's C++ half: names are typed twice and a
wrong one is silent. This step joins the two.

| Piece | What |
|---|---|
| The server half is scaffolded | `sdk.ps1 -New <mod>` also writes `server_mods/<mod>/<mod>.cpp`: declarations that match the starter scenes |
| The schema reaches the editor | `cb_server --dump-schema` writes the fields, events, actions, stances and item kinds; the extension completes them in `CbReaction`, `CbPrediction`, `CbMotion` and tree conditions, and warns on unknown ones |
| Publishing checks names | a look that names what its mod does not declare stops the publish |
| An item in the hand | a preview of the item on the placeholder skeleton, grips solved, a pack playing |
| A prebuilt SDK | CI builds the extension, so a mod's look needs no compiler |

### 6. Mod testing

One step from an edit to playing it: the client starts the server.

| Piece | What |
|---|---|
| The client hosts | "Test" in the menu and `--host-local`: the game starts `cb_server` with the chosen mods, joins it, and stops it when it leaves |
| From the SDK | a **Test mod** button: publish the look, build the server if the `.cpp` changed, start the game hosting |
| Looks reload | a published look is taken up without restarting the server or the game |
| Bots | `--bots N` on the hosted server, so a mod can be tried alone |

- It waits for step 4: it wraps the menu, joining and pack loading, which should be settled first.

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
| A board sized by the schema | `kBoardSlots` is a wall that came back once already; motions declare more fields |
| More than 16 motions | `kMaxMotions` is the size of a component every player carries; sized by the schema, like the board |
| Expressions in `changes` | `pistol.ammo -= 1` takes a number; the right side could be an expression. Shared by `CbPrediction` and `CbMotion` |
| HUD nodes as reactions | `CbFieldBinding` and a While `CbReaction` with a `value_expression` do the same thing in two places |
| The rest of `simulation.cpp` in modules | props, ragdolls, items and animation; the mover went first (M92) |
| Tools on every platform | the publish, pack and SDK scripts are PowerShell |
| The old `build/` folder in the checkout | builds go outside the source tree now; the folder is ignored and can be deleted |
