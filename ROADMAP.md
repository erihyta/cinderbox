# Cinderbox — Roadmap

What comes next, in order, and why. [DESIGN.md](DESIGN.md) says how things work today;
[docs/HISTORY.md](docs/HISTORY.md) is what is done. A step that is done moves out of here.

## The order

| # | Milestone | In one line |
|---|---|---|
| 1 | [Anything can be shown](#1-anything-can-be-shown) | what the server knows reaches the screen through scenes, with no client code |
| 1b | [Motions in the editor](#1b-motions-in-the-editor) | a preview, checks at publish, the body follows a motion |
| 2 | [The client, finished](#2-the-client-finished) | what is left of the client's known limits |
| 3 | [The SDK knows the server](#3-the-sdk-knows-the-server) | the Godot modding project scaffolds the server half and knows its names |
| 4 | [Mod testing](#4-mod-testing) | the client starts a server by itself: one button from an edit to playing it |

Done before these: movement parameters (M92), motions on a press (M93: a dash, a double jump) and
motions that hold (M94: flight, a jetpack, a glide), and weight with probes, forces and links (M99: a grappling hook;
[docs/motions.md](docs/motions.md)). Step 1b finishes that idea. Steps 3 and 4 are last on
purpose: they wrap the client, so they wait until it stops changing.

## Where motions stand

A `CbMotion` says: *on a press, on a cue, or while conditions hold, do this to the player and to what it reaches.* It is baked, sent in
the schema and run by every simulation, so a player's own are predicted and rolled back
([docs/motions.md](docs/motions.md)).

| | Today (M99) | Still to come |
|---|---|---|
| When | on a press, while conditions hold, on a mod event at the player | |
| Conditions | what a state machine reads, `held.<action>`, `pressed.<action>`, `linked` | `motion.<name>` |
| Does | parameters while it is on; changes of fields; an event; and its parts: a probe, impulses, forces (acceleration, newtons, toward a speed; ramped; reacting), links | a lasting force as a C++ command; a test of one player hooked to another |
| On whom | the player, what the probe found, the entity a field names | |
| Weight | `mass`, a movement parameter: contacts, forces and ropes share by it | a knocked-down ragdoll state; carrying a player; structures that break |
| Timing | `cooldown`, `uses` with a refill, `duration`, `until` | |
| Editor | the nodes, their help, the bake, warnings | a preview panel; names checked at publish |

### Open questions

| Question | Leaning |
|---|---|
| Other players' presses are guessed by repeating their last input, so their dash is seen late and corrected | accept: it is what a jump does today, and the mirror fades the correction |
| A field both a motion and a server mod write | allowed today (commands apply first, then motions); the bake could name the fields a set writes, and the server warn when a mod `Set`s one every tick |
| A second jump that plays the jump's animation | the character's tree enters its jump state on the motion's event; a starter for it comes with step 1b |

### Routes not taken

| Route | Why not |
|---|---|
| The viewer guesses the command (`CbPrediction` with a push) | the viewer has no simulation: a guessed push cannot collide, and a pull would fight every frame's correction |
| Server mods also run on clients | tried as M55 and dropped: rules would ship to clients and every mod would need to be deterministic |
| A built-in dash, a built-in grapple | rules in the engine; mods could only tune what the engine thought of |

## The steps

### 1. Anything can be shown

The presentation sandbox: the server says what is true (fields, events), a scene in a mod's pack
says how it looks. No client code, and nothing in the engine that knows what a score is.

| Piece | Today | Would be |
|---|---|---|
| Lists | **done (M100)**: `CbList`, a row per player or item, designed as a scene; `CbShowKey` | a headless check that drives one; a grid whose columns size themselves |
| Saying things | **done (M102)**: the words are the look's; a label's `choices` are picked by a number the server sets, a `CbReaction` shows a scene on an event | choices on other nodes (a prompt, a texture or a scene picked by a number) |
| Text nobody authored | player names only | what a player or a host types (chat, a team name, a message of the day): the parked `m101-text` branch has the mechanism (a text field, `ctx.SetText`) |
| Lists on one entity | a field holds one value | array fields, or a `CbList` over the values of a field family |
| One body description | `CbProp`, `CbTemplate` + `CbComponent` and `CbItemBody` each describe a dynamic body their own way; none exposes density, friction or bounce | one shared body (shape, mass or density, friction, restitution) for props, items and templates; `CbItemBody` keeps the grip offset, `properties` move off it |
| HUD keys | `CbShowKey` adds an action of the viewer's own | they appear in key rebinding with the server's actions |

### 1b. Motions in the editor

| Piece | What |
|---|---|
| Motion Preview | a panel like Cue Preview: a capsule on a small stage, press an action, set a field, watch the path |
| Publish checks | a motion that reads a name nobody declares, or writes a field it may not, stops the publish |
| The body follows | `motion.<name>` and a motion's event in state machine conditions; the viewer's lead covers them |
| Animation packs | a pack's layer can be asked for by a motion while it is on (a flight pose) |

### 2. The client, finished

From DESIGN.md's known limits. Each is small; together they are "the client is near done".

| Candidate | Today | Would be |
|---|---|---|
| A wrong guess is taken back | a reaction played on a prediction the server never answers stays played | a prediction that expires stops what it started |
| A swing seen from its start | behind latency another player's swing is first seen a little way in, and keys before that point do not fire | the skipped keys fire on the first frame it is seen |
| Checked pack layers | a pack's layer may read what the lead cannot predict | the bake says so |
| Key rebinding | actions are bound to the keys mods suggest | a page in the settings |
| A server list | join by address | recent and favourite servers with their mods and players |

### 3. The SDK knows the server

Today the Godot modding project knows nothing of the mod's C++ half: names are typed twice and a
wrong one is silent. This step joins the two.

| Piece | What |
|---|---|
| The server half is scaffolded | `sdk.ps1 -New <mod>` also writes `server_mods/<mod>/<mod>.cpp`: declarations that match the starter scenes |
| The schema reaches the editor | `cb_server --dump-schema` writes the fields, events, actions, stances and item kinds; the extension completes them in `CbReaction`, `CbPrediction`, `CbMotion` and tree conditions, and warns on unknown ones |
| Publishing checks names | a look that names what its mod does not declare stops the publish |
| An item in the hand | a preview of the item on the placeholder skeleton, grips solved, a pack playing |
| A prebuilt SDK | CI builds the extension, so a mod's look needs no compiler |

### 4. Mod testing

One step from an edit to playing it: the client starts the server.

| Piece | What |
|---|---|
| The client hosts | "Test" in the menu and `--host-local`: the game starts `cb_server` with the chosen mods, joins it, and stops it when it leaves |
| From the SDK | a **Test mod** button: publish the look, build the server if the `.cpp` changed, start the game hosting |
| Looks reload | a published look is taken up without restarting the server or the game |
| Bots | `--bots N` on the hosted server, so a mod can be tried alone |

- It waits for step 2: it wraps the menu, joining and pack loading, which should be settled first.

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
| Limits on `changes` | a While's per-second change is not clamped: a tank fills to a little over full. A `min` / `max` on the change, or expressions on its right side |
| Expressions in `changes` | `pistol.ammo -= 1` takes a number; the right side could be an expression. Shared by `CbPrediction` and `CbMotion` |
| HUD nodes as reactions | `CbFieldBinding` and a While `CbReaction` with a `value_expression` do the same thing in two places |
| The rest of `simulation.cpp` in modules | props, ragdolls, items and animation; the mover went first (M92) |
| Tools on every platform | the publish, pack and SDK scripts are PowerShell |
| The old `build/` folder in the checkout | builds go outside the source tree now; the folder is ignored and can be deleted |
