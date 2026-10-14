# Cinderbox — Roadmap

What comes next, in order, and why. [DESIGN.md](DESIGN.md) says how things work today;
[docs/HISTORY.md](docs/HISTORY.md) is what is done. A step that is done moves out of here.

## The order

| # | Milestone | In one line |
|---|---|---|
| 1 | [The SDK knows the server](#1-the-sdk-knows-the-server) | the editor knows the server's names: completed while typing, checked at publish |
| 2 | [Mod testing](#2-mod-testing) | the client starts a server by itself: one button from an edit to playing it |
| 3 | [Items that do things](#3-items-that-do-things) | an item is used by its slot's key; what it does is helper nodes under it; its state is its own; one tree per kind in the game |
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
| When | on a press, while conditions hold, on a mod event at the player (one a server mod's command or another motion recorded this tick) | on a clip's marker: its event is recorded after the tick's motions ran, and none hears it |
| Conditions | what a state machine reads (an item kind in the hand, or `carried.<kind>`; `motion.<name>`), `held.<action>`, `pressed.<action>`, `linked`, `link_distance` | |
| Does | parameters while it is on; changes of fields; an event; and its parts: a probe, impulses, forces (acceleration, newtons, toward a speed; ramped; reacting), links, a launch (an item of a kind thrown into the world: a grenade) | a lasting force as a C++ command; a test of one player hooked to another; [what is left of launches](#3-items-that-do-things) |
| On whom | the player, what the probe found, the entity a field names | |
| Weight | `mass`, a movement parameter: contacts, forces and ropes share by it | a knocked-down ragdoll state; carrying a player; structures that break |
| Timing | `cooldown`, `uses` with a refill, `duration`, `until` | |
| Editor | the nodes, their help, the bake, warnings | a preview panel; names checked at publish |

### Open questions

| Question | Leaning |
|---|---|
| Other players' presses are guessed by repeating their last input, so their dash is seen late and corrected | accept: it is what a jump does today, and the mirror fades the correction |
| A field both a motion and a server mod write | allowed today (commands apply first, then motions); the bake could name the fields a set writes, and the server warn when a mod `Set`s one every tick |
| A second jump that plays the jump's animation | works today: a motion's event is in the tick's events before the player's state machine runs, so a transition named like it is taken (as `item.use` is). The starter for it comes with step 3 |

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
| An item in the hand | **done (M133 to M135)**: `CbItem` Preview > Hold: a stand-in figure beside the item holds a copy of it, posed by the game's own code from a baked pack, grips solved; a First person panel beside the 3D view shows what the holder sees, with the item's view settings live; and a cue fired from the inspector plays in both (the pack's state machine, the item's reactions, the mod's world reactions) | cues with someone else in them (a hit); the pack chosen from a list; the character's own mesh |
| A prebuilt SDK | CI builds the extension, so a mod's look needs no compiler |
| A new template | the starters are the redesign's already (a `CbItem`, a motion, a HUD); what is old is the prose in `sdk/README.md` around them |

### 2. Mod testing

One step from an edit to playing it: the client starts the server.

| Piece | What |
|---|---|
| The client hosts | **done (M123)**: **Test locally** in the menu and `--host-local[=MODS]`: the game starts `cb_server`, joins it, and stops it when it leaves; the server also stops by itself when empty |
| From the SDK | **done (M125)**: a **Test mod** button in the editor's toolbar (in a mod's client project) and `tools\test_mod.ps1 -Mod <mod>`: save, build, publish, start the game hosting |
| Looks reload | **done (M132)**: publish a look while the server runs and everyone playing sees it, with no restart, when it changes nothing the simulation reads | a change the simulation reads (an item's body, a pack's state machine, a motion) still needs the server restarted: the hosted test could do that by itself; files a new look no longer has stay loaded until the game restarts |
| Bots | **done (M123)**: `--bots=N` starts `cb_bot` players on the hosted server | a count in the menu; bots that use what the mod under test adds |

### 3. Items that do things

**The idea** (rethought after M145): an item is authored in one scene and is everything about
itself: its body and look, how its key works, what it does, its state, its pose. The engine sorts
what is per copy (the body, meshes, markers), what is once per kind (reactions, predictions) and
what is baked for the simulation (what moves bodies, fields). A grappling hook should read the way
it is thought: an item in the left hand; on use, a line is thrown; where it hits, a rope binds the
player to the point; the player is pulled.

**Done on the way** (M142 to M145): an item's motions (`CbMotion` under a `CbItem`, `carried.<kind>`),
`motion.<name>` in conditions, the Keys page, and **overlays**: a pack's layer the character does not
have, laid over the whole body by its own weight while the item is carried (the boots' dash pose,
the hook's hang).

| # | Piece | What | Why this way |
|---|---|---|---|
| 1 | Slot keys and use modes | **done (M146)**: an item's key is its slot's; `use` In the hand / Press / Hold; slot keys sent held with the input, rebindable on the Keys page, suggested by a mod (`declare.SlotKey`) or the item (`slot_key`); `using.<kind>`; the dash is the boots' slot key (V) | an item's `CbMotion.key` and declared actions (M144) are gone |
| 2 | What an item does, as helpers | **done (M147)**: under a `CbItem`, a `CbProbe` (with `release` and `hold_seconds`), the effects and the new `CbMove`, nested as they hold, with the item's When used rules (`use_conditions`, `use_changes`, `use_cooldown`, `use_emits`), bake into one motion, `Use`; the hook and the boots are written so | the left hand (piece 5) is still to come: the hook is out of sight |
| 3 | An item's state | **done (M148)**: `CbItem.fields` (name and starting value; its type from the value): a field of every copy, started by the simulation whichever way a copy is made, read and written by mods, read by looks as `hand.<field>`; the gun's rounds are the gun's (`gun.rounds`) | a state machine or a motion cannot read the hand's fields yet; the gun's HUD still reads the player's `gun.ammo` (a prediction counts it down at once) |
| 4 | Copies on the floor are lean | **done (M149)**, in place of one tree per kind: a copy lying in the world keeps its look, its body and its reactions about itself; what is about a holder is taken out, and put back from its scene when somebody has it (with four of every item lying about: 57 listeners instead of 165) | one shared tree per kind was set aside: the copies somebody carries are few (a player's slots), each must place its effects at its own muzzle and in the first-person view, and the state is the item's fields now (M148) |
| 5 | The left hand | an item that stays in the left hand while the right holds another; its pose an overlay filtered to the left arm | the hook as it is thought of |
| 6 | Thrown from the hand | a launch can throw *the item in the hand*, from its socket, on a marker of the pack's throw clip | what is thrown is what was held; a count is step 4's stacks |
| 7 | An item's time is up | an event when an item with a lifetime ends, launched or not: a grenade goes off on a timer | a mod sees what a thrown item ran into (`ctx.Hits`), not that its time is up |
| 8 | Launches: every hit, fast and small | a launched item's own contacts always; continuous collision for kinds that ask | the hardest 8 impacts a tick are kept now; a rocket can pass a thin wall |
| 9 | Publish checks, changes | a name nobody declares stops the publish; a `min` / `max` on a change | |
| 10 | Item Preview, used | press the item's key in the editor's preview and watch what it does | last: **Test mod** (M125) is one button from an edit to playing it |

- **Not a route: reactions that give out cues.** A `CbReaction` runs in the game only; the body's
  animation is simulation state the server poses for hit tests.
- **What changes physics is baked**: the helpers are authoring; the simulation runs the data they
  bake to. Reactions and sounds are live nodes in the game. Both sit in one item scene.
- **Poses need clips**: the dash's and the hook's are placeholders (Sprint, Jump).

### 4. The inventory, finished

| Piece | Today | Would be |
|---|---|---|
| The screen | click an item, then a slot (M110) | a drag with the item under the cursor; right click drops; an icon on the `CbItem` |
| Stacks | one item a slot | a stack size on the `CbItem`, a count per slot |
| Containers | players only | slots on any entity: a chest, a shop, a corpse |
| Intents a mod declares | select, move and drop | "buy item 3", "vote 2": a `CbClick` sends it, the mod answers; no client code |
| The holster | **done (M137)**: on the `CbItem`, with the item's animation pack, for a mod that takes its items from its look | |
| An item's HUD | a scene of its mod's | in the item's own scene, shown while it is selected |
| The rules API | C++ in each mod | what a mod decides about containers, named |
| Checked across compilers | not yet | slots in the reference scenario |

### 5. The client, finished

| Candidate | Today | Would be |
|---|---|---|
| A wrong guess is taken back | a reaction played on a prediction the server never answers stays played | a prediction that expires stops what it started |
| A swing seen from its start | behind latency another player's swing is first seen a little way in, and keys before that point do not fire | the skipped keys fire on the first frame it is seen |
| Text nobody authored | player names only | chat, a team name, a message of the day (the parked `m101-text` branch has the mechanism) |
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
| Slots for what is worn | the dash boots take a hand's slot (the fifth): selected, they are held like a tool. Slots that are never in the hand belong to step 4 |
| `reconcile_every` 2 as the default | halves a client's rollback work with many players and latency (M139); others are seen up to 2 ticks later: to be looked at in play first |
| The characters' part of a step | as much as the physics at 64 players (0.4 ms, M140) and not yet looked into |
| Physics threads beside drawing | on by default (M141), not yet watched in the game: `--physics-threads=0` if frames suffer |
