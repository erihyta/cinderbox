# Cinderbox — Roadmap

What comes next, in order, and why. [DESIGN.md](DESIGN.md) says how things work today;
[docs/HISTORY.md](docs/HISTORY.md) is what is done. A step that is done moves out of here.

## The order

| # | Milestone | In one line |
|---|---|---|
| 1 | [The SDK knows the server](#1-the-sdk-knows-the-server) | the editor knows the server's names: completed while typing, checked at publish |
| 2 | [Mod testing](#2-mod-testing) | the client starts a server by itself: one button from an edit to playing it |
| 3 | [Motions and the body](#3-motions-and-the-body) | the body follows a motion; a preview; what is left of launches |
| 4 | [The inventory, finished](#4-the-inventory-finished) | drag, icons, stacks, chests, and intents a mod declares |
| 5 | [The client, finished](#5-the-client-finished) | what is left of the client's known limits, typed text, the game script in parts |
| 6 | [Rules without a relink](#6-rules-without-a-relink) | a server mod's rules as a script the server loads, not C++ compiled into it |

- **Why this order** (rethought after M118). The pieces a game is made of are there now: movement
  (motions), things that fly (launches), items and slots, a HUD and a world that read the server's
  state. What is slow is *making* something with them: a name typed wrong is silent, and trying an
  edit means publishing, restarting a server and joining it by hand. So the next two steps are the
  loop itself; every step after them is authored faster for it.
- **Why they are not last any more**: they wrap the client and the scenes, so they waited until
  those stopped changing. The redesign that broke scenes (M104 to M117: items, slots, the HUD's
  nodes, bodies, sizes) is over.
- **Why rules get a step**: C++ compiled into the server is the highest wall a modder meets, and
  only the server runs rules, so they need no determinism: any language would do.
- **Done before these**: movement parameters (M92), motions (M93, M94, M99), lists and keys in the
  HUD (M100, M102, M114, M116), items and slots in the engine with an inventory screen (M104 to
  M110), the grapple as a zip line (M112), state sized by the mods (M113), one body description
  (M115), launches (M118). [docs/HISTORY.md](docs/HISTORY.md).

## Where motions stand

A `CbMotion` says: *on a press, on a cue, or while conditions hold, do this to the player and to what it reaches.* It is baked, sent in
the schema and run by every simulation, so a player's own are predicted and rolled back
([docs/motions.md](docs/motions.md)).

| | Today (M118) | Still to come |
|---|---|---|
| When | on a press, while conditions hold, on a mod event at the player | |
| Conditions | what a state machine reads, `held.<action>`, `pressed.<action>`, `linked`, `link_distance` | `motion.<name>` |
| Does | parameters while it is on; changes of fields; an event; and its parts: a probe, impulses, forces (acceleration, newtons, toward a speed; ramped; reacting), links, a launch (an item of a kind thrown into the world: a grenade) | a lasting force as a C++ command; a test of one player hooked to another; [what is left of launches](#3-motions-and-the-body) |
| On whom | the player, what the probe found, the entity a field names | |
| Weight | `mass`, a movement parameter: contacts, forces and ropes share by it | a knocked-down ragdoll state; carrying a player; structures that break |
| Timing | `cooldown`, `uses` with a refill, `duration`, `until` | |
| Editor | the nodes, their help, the bake, warnings | a preview panel; names checked at publish |

### Open questions

| Question | Leaning |
|---|---|
| Other players' presses are guessed by repeating their last input, so their dash is seen late and corrected | accept: it is what a jump does today, and the mirror fades the correction |
| A field both a motion and a server mod write | allowed today (commands apply first, then motions); the bake could name the fields a set writes, and the server warn when a mod `Set`s one every tick |
| A second jump that plays the jump's animation | the character's tree enters its jump state on the motion's event; a starter for it comes with step 3 |

### Routes not taken

| Route | Why not |
|---|---|
| The viewer guesses the command (`CbPrediction` with a push) | the viewer has no simulation: a guessed push cannot collide, and a pull would fight every frame's correction |
| Server mods also run on clients | tried as M55 and dropped: rules would ship to clients and every mod would need to be deterministic |
| A built-in dash, a built-in grapple | rules in the engine; mods could only tune what the engine thought of |

## The steps

### 1. The SDK knows the server

Today the Godot modding project knows nothing of the mod's C++ half: names are typed twice and a
wrong one is silent. This step joins the two.

| Piece | What |
|---|---|
| The schema reaches the editor | **done (M119)**: the server's build writes every mod's names (`cb_server --dump-names`); a property that holds one name offers them, and a node warns about one nobody declares, with the closest there is |
| The server half is scaffolded | **done (M121)**: `sdk.ps1 -New <mod>` also writes `server_mods/<mod>/<mod>.cpp`, declaring exactly the names the starter scenes use; built, published and run as it is made |
| Publishing checks names | **done (M119)**: `check_names.gd` runs over every scene before a look is packed |
| Names inside expressions | completion is for properties that hold one name; inside a condition or a format a name is checked but not offered while typing: an editor for expressions |
| Names of other servers | the file is what *this* checkout's server compiles; a look for a mod built elsewhere needs that server's file |
| An item in the hand | a preview of the item on the placeholder skeleton, grips solved, a pack playing |
| A prebuilt SDK | CI builds the extension, so a mod's look needs no compiler |
| A new template | the starters are the redesign's already (a `CbItem`, a motion, a HUD); what is old is the prose in `sdk/README.md` around them |

### 2. Mod testing

One step from an edit to playing it: the client starts the server.

| Piece | What |
|---|---|
| The client hosts | "Test" in the menu and `--host-local`: the game starts `cb_server` with the chosen mods, joins it, and stops it when it leaves |
| From the SDK | a **Test mod** button: publish the look, build the server if the `.cpp` changed, start the game hosting |
| Looks reload | a published look is taken up without restarting the server or the game |
| Bots | `--bots N` on the hosted server, so a mod can be tried alone |

### 3. Motions and the body

| Piece | What |
|---|---|
| The body follows | `motion.<name>` and a motion's event in state machine conditions; the viewer's lead covers them. The grapple and the dash get a pose |
| Animation packs | a pack's layer can be asked for by a motion while it is on (a flight pose) |
| Motion Preview | a panel like Cue Preview: a capsule on a small stage, press an action, set a field, watch the path |
| Publish checks | a motion that reads a name nobody declares, or writes a field it may not, stops the publish |
| Changes | a `min` / `max` on a change (a tank fills to a little over full today); an expression on its right side |
| Launches: from the hand | a launch starts ahead of where the look starts; from a socket (`RightHand`), with a throw's pose, it leaves the hand |
| Launches: a fuse | a mod sees what a thrown item ran into (`ctx.Hits`), not that its time is up: an event when a launched item expires, so a grenade can go off on a timer |
| Launches: every hit | the simulation keeps the hardest 8 impacts of a tick; a soft touch in a busy scene may be missed. A launched item's own contacts, always |
| Launches: fast and small | an item's body is an ordinary one: at rocket speeds a thin wall can be passed through. Continuous collision for launched kinds that ask |
| Launches: ammunition | a motion's `uses` refill by time; throwing *the item in the hand* (a count that goes down) is a rule a mod writes with a field today |

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

### 5. The client, finished

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

### 6. Rules without a relink

Today a server mod is C++ compiled into `cb_server`: a compiler, a relink and a restart for every
change of a rule. Rules run on the server only, so nothing about them has to be deterministic or
shared with clients.

| Piece | What |
|---|---|
| A script the server loads | a mod's rules in a sandboxed script (the same `Declare` / `Start` / `Tick` and the same `Context` verbs), loaded from the mod's folder at start |
| Reloaded while it runs | a changed rule is taken up without restarting the server: with step 2, an edit to a rule is seconds from being played |
| C++ stays | for what a script is too slow for; both are mods to everything else |
| The SDK writes it | step 1's scaffold writes the script, not a `.cpp` |

## Later, not scheduled

| What | Needs |
|---|---|
| Cameras a mod places | a camera or a path in a mod's scene that a reaction makes the view for a while (a round's end, a kill cam, a scenic shot), with the game's own camera taking over again afterwards |
| Vehicles | joints in the component registry and templates of several bodies; a seat (the mover off, the player carried); input routed to motors, as a motion routes it to the mover |
| A script for looks | a sandboxed one, for what fields, intents and clicks cannot say; not visual scripting |
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
