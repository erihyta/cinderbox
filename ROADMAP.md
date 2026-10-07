# Cinderbox — Roadmap

What comes next, in order, and why. [DESIGN.md](DESIGN.md) says how things work today;
[docs/HISTORY.md](docs/HISTORY.md) is what is done. A step that is done moves out of here.

## The order

| # | Milestone | In one line |
|---|---|---|
| 1 | [The walls come down](#1-the-walls-come-down) | how many fields, motions, layers and actions a server has is its mods' business, not a constant's |
| 2 | [Fewer nodes](#2-fewer-nodes) | one label, one list, one way to drive a property, one body description |
| 3 | [Things that fly](#3-things-that-fly) | `CbLaunch`: a predicted projectile, as a part of a motion |
| 4 | [The inventory, finished](#4-the-inventory-finished) | drag, icons, stacks, chests, and intents a mod declares |
| 5 | [Motions and the body](#5-motions-and-the-body) | the body follows a motion; a preview; checks at publish |
| 6 | [The client, finished](#6-the-client-finished) | what is left of the client's known limits, typed text, the game script in parts |
| 7 | [The SDK knows the server](#7-the-sdk-knows-the-server) | the Godot modding project scaffolds the server half and knows its names |
| 8 | [Mod testing](#8-mod-testing) | the client starts a server by itself: one button from an edit to playing it |

- **Why this order**: 1 and 2 break file formats and scenes, so they go while breaking is allowed (the
  redesign: example mods and the SDK template may break or go). 3 to 5 are what a game needs next.
  7 and 8 wrap the client, so they wait until it stops changing.
- **Done before these**: movement parameters (M92), motions (M93, M94, M99), lists and keys in the
  HUD (M100, M102), items and slots in the engine with an inventory screen (M104 to M110), the
  grapple as a zip line (M112). [docs/HISTORY.md](docs/HISTORY.md).

## Where motions stand

A `CbMotion` says: *on a press, on a cue, or while conditions hold, do this to the player and to what it reaches.* It is baked, sent in
the schema and run by every simulation, so a player's own are predicted and rolled back
([docs/motions.md](docs/motions.md)).

| | Today (M112) | Still to come |
|---|---|---|
| When | on a press, while conditions hold, on a mod event at the player | |
| Conditions | what a state machine reads, `held.<action>`, `pressed.<action>`, `linked`, `link_distance` | `motion.<name>` |
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
| A second jump that plays the jump's animation | the character's tree enters its jump state on the motion's event; a starter for it comes with step 5 |

### Routes not taken

| Route | Why not |
|---|---|
| The viewer guesses the command (`CbPrediction` with a push) | the viewer has no simulation: a guessed push cannot collide, and a pull would fight every frame's correction |
| Server mods also run on clients | tried as M55 and dropped: rules would ship to clients and every mod would need to be deterministic |
| A built-in dash, a built-in grapple | rules in the engine; mods could only tune what the engine thought of |

## The steps

### 1. The walls come down

A server mod's own code may do as much work as the host's machine allows: it runs on the server
only, and nothing measures it. What is capped is the state every client carries, hashes and rolls
back. Some of those caps are costs; others are only the size somebody gave an array.

| Limit | Today | Why it is there | Would be |
|---|---|---|---|
| Fields per scope | 32 (`kBoardSlots`) | a block of that size on every entity | sized by what the mods declare |
| Motions | 16 (`kMaxMotions`) | a block of that size on every player | sized by the schema |
| Animation layers | 4 (`kMaxAnimLayers`) | 40 bytes a layer on every player, and a state machine sampled and blended per player per tick | sized by the schema; the cost is the blending, and the host's to spend |
| Mod actions | 16 | a bit each, in the input of every tick | 32 |
| Commands a tick | 1,024 | they travel in the frame everyone gets | stays: a guard |
| Slots a player | 36 | the input's format | stays |
| Item kinds, stances, sockets, packs | about 255 | ids of one byte | stays |
| Props | `--props-per-player`, `--props-global` | already the host's choice | stays |

- **Animations were never limited**: a pack ships as many clips and states as it likes. A *layer*
  is one more state machine running on every player at once (it moves the hitboxes, so every
  simulation runs it). Four is a wall of the first kind.
- **What it costs**: the protocol, replay and view file versions; new reference hashes.
- **What stays true afterwards**: more state on entities is more for every client to hash and
  resimulate. The server says how much at start (bytes per player, per entity), so a host sees it.

### 2. Fewer nodes

The extension has about 35 node classes. Several do the same thing in two places.

| Overlap | Today | Would be |
|---|---|---|
| Two labels | `CbFieldLabel`, `CbPromptLabel` | `CbFieldLabel`: `{key:pickup}` in its format, a subject path, a progress it can show |
| Two lists | `CbList`, `CbEventFeed` | `CbList` of **Events**: a kill feed is a designed row like any other |
| Two ways to drive a property | `CbFieldBinding`, a While `CbReaction` with `value_expression` | the `CbReaction`; `CbFieldBinding` goes |
| A key that shows one thing | `CbShowKey` | a key that sets a `ui.` value (hold or toggle); showing is a condition on it, so one key can drive anything |
| Conditions, five times | each HUD node declares and evaluates its own | one base: subject, conditions, format |
| Three body descriptions | `CbProp`, `CbTemplate` + `CbComponent`, `CbItem` | one shared body (shape, mass or density, friction, bounce); the item keeps what is an item's |

- **Kept as it is**: `CbGrip` (a bare marker today; hand placement will want it), the motion nodes,
  the character nodes.
- **After it**: the HUD is four nodes (label, list, click, key) and a reaction.
- **Also here**: `cinderbox_client.cpp` (2,700 lines) in parts: the frame source, the HUD's values,
  items.

### 3. Things that fly

A probe is a line: it has no body, nobody sees it coming, nothing can step out of its way.

| Piece | What |
|---|---|
| `CbLaunch` | a part of a motion, next to `CbProbe`: throws a body (a scene's, an item kind's) from a socket, at a speed, with gravity or without |
| Predicted | every simulation launches it on the tick of the press, like a dash: the thrower sees it leave at once |
| What it hits | an event at the point, with what was hit and how fast; the mod decides what that means (damage, a blast, a sticky hook) |
| Its life | seconds, bounces, or until it hits; then it is removed, or left as a prop or an item |
| The look | the body's own scene, with its reactions (a trail, a blast on the event) |
| Examples | a grenade for the pistol mod; a thrown bat; the grapple's hook as a thing that flies |

### 4. The inventory, finished

| Piece | Today | Would be |
|---|---|---|
| The screen | click an item, then a slot (M110) | a drag with the item under the cursor; right click drops; an icon on the `CbItem` |
| Stacks | one item a slot | a stack size on the `CbItem`, a count per slot |
| Containers | players only | slots on any entity: a chest, a shop, a corpse |
| Intents a mod declares | select, move and drop | "buy item 3", "vote 2": a `CbClick` sends it, the mod answers; no client code |
| The holster | declared in C++ | on the `CbItem` |
| An item's HUD | a scene of its mod's | in the item's own scene, shown while it is selected |
| The rules API | C++ in each mod | what a mod decides about containers, named |
| Checked across compilers | not yet | slots in the reference scenario |

### 5. Motions and the body

| Piece | What |
|---|---|
| The body follows | `motion.<name>` and a motion's event in state machine conditions; the viewer's lead covers them. The grapple and the dash get a pose |
| Animation packs | a pack's layer can be asked for by a motion while it is on (a flight pose) |
| Motion Preview | a panel like Cue Preview: a capsule on a small stage, press an action, set a field, watch the path |
| Publish checks | a motion that reads a name nobody declares, or writes a field it may not, stops the publish |
| Changes | a `min` / `max` on a change (a tank fills to a little over full today); an expression on its right side |

### 6. The client, finished

| Candidate | Today | Would be |
|---|---|---|
| A wrong guess is taken back | a reaction played on a prediction the server never answers stays played | a prediction that expires stops what it started |
| A swing seen from its start | behind latency another player's swing is first seen a little way in, and keys before that point do not fire | the skipped keys fire on the first frame it is seen |
| Text nobody authored | player names only | chat, a team name, a message of the day (the parked `m101-text` branch has the mechanism) |
| Key rebinding | keys are the ones mods suggest | a page in the settings: the server's actions, the HUD's keys, the engine's own |
| A server list | join by address | recent and favourite servers with their mods and players |
| Checked pack layers | a pack's layer may read what the lead cannot predict | the bake says so |
| The game script | `game.gd` is nearly 1,000 lines: input, the camera, the unattended run, the view probe | scripts of their own, all still in Godot |

- **The camera stays Godot's**: a `Camera3D` the game's script places, not the extension's. That
  keeps the next thing open: [cameras a mod places](#later-not-scheduled).

### 7. The SDK knows the server

Today the Godot modding project knows nothing of the mod's C++ half: names are typed twice and a
wrong one is silent. This step joins the two.

| Piece | What |
|---|---|
| The schema reaches the editor | `cb_server --dump-schema` writes the fields, events, actions, stances and item kinds; the extension completes them in conditions and warns on unknown ones |
| The server half is scaffolded | `sdk.ps1 -New <mod>` also writes `server_mods/<mod>/<mod>.cpp`: declarations that match the starter scenes |
| Publishing checks names | a look that names what its mod does not declare stops the publish |
| An item in the hand | a preview of the item on the placeholder skeleton, grips solved, a pack playing |
| A prebuilt SDK | CI builds the extension, so a mod's look needs no compiler |
| A new template | the old one is from before the redesign |

### 8. Mod testing

One step from an edit to playing it: the client starts the server.

| Piece | What |
|---|---|
| The client hosts | "Test" in the menu and `--host-local`: the game starts `cb_server` with the chosen mods, joins it, and stops it when it leaves |
| From the SDK | a **Test mod** button: publish the look, build the server if the `.cpp` changed, start the game hosting |
| Looks reload | a published look is taken up without restarting the server or the game |
| Bots | `--bots N` on the hosted server, so a mod can be tried alone |

## Later, not scheduled

| What | Needs |
|---|---|
| Cameras a mod places | a camera or a path in a mod's scene that a reaction makes the view for a while (a round's end, a kill cam, a scenic shot), with the game's own camera taking over again afterwards |
| Vehicles | joints in the component registry and templates of several bodies; a seat (the mover off, the player carried); input routed to motors, as a motion routes it to the mover |
| A script for looks | a sandboxed one, for what fields, intents and clicks cannot say; not visual scripting |
| Server mods without a relink | mods as libraries or scripts: they need no determinism, so any language works |
| Teams, spectators | mods, once private fields and the board cover them |
| Lists on one entity | array fields, or a `CbList` over the values of a field family |

## Housekeeping

Independent of the path; each is small.

| What | Note |
|---|---|
| The rest of `simulation.cpp` in modules | props, ragdolls, items and animation; the mover went first (M92) |
| Tools on every platform | the publish, pack and SDK scripts are PowerShell |
| A motion's field and a mod's | both may write one (commands first, then motions); the server could warn when a mod sets every tick a field a motion writes |
| The old `build/` folder in the checkout | builds go outside the source tree now; the folder is ignored and can be deleted |
