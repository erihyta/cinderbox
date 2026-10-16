# Cinderbox — Roadmap

What comes next, in order, and why. [DESIGN.md](DESIGN.md) says how things work today;
[docs/HISTORY.md](docs/HISTORY.md) is what is done. A step that is done moves out of here.

## The order

| # | Milestone | In one line |
|---|---|---|
| 1 | [The SDK knows the server](#1-the-sdk-knows-the-server) | the editor knows the server's names: completed while typing, checked at publish |
| 2 | [Mod testing](#2-mod-testing) | the client starts a server by itself: one button from an edit to playing it |
| 3 | [Items that do things](#3-items-that-do-things) | an item is used from the hand or by its own key; what it does is helper nodes under it; its state is its own; one tree per kind in the game |
| 4 | [The inventory, finished](#4-the-inventory-finished) | drag, icons, stacks, chests, intents a mod declares, and slots as places; last, slots checked across compilers |
| 5 | [Logic as text, places as nodes](#5-logic-as-text-places-as-nodes) | a scene's rules (when, if, change, emit) as lines in one sheet; nodes only for what is a place or a thing |
| 6 | [The client, finished](#6-the-client-finished) | what is left of the client's known limits, typed text, the game script in parts |
| 7 | [Rules without a relink](#7-rules-without-a-relink) | a server mod's rules as a script the server loads, not C++ compiled into it |

- **Why this order** (rethought after M118). The pieces a game is made of are there now: movement
  (motions), things that fly (launches), items and slots, a HUD and a world that read the server's
  state. What is slow is *making* something with them: a name typed wrong is silent, and trying an
  edit means publishing, restarting a server and joining it by hand. So the next two steps are the
  loop itself; every step after them is authored faster for it.
- **Why they are not last any more**: they wrap the client and the scenes, so they waited until
  those stopped changing. The redesign that broke scenes (M104 to M117: items, slots, the HUD's
  nodes, bodies, sizes) is over.
- **Why logic as text comes next** (decided after M161): one `CbReaction` is one rule, so a gun
  that reacts to eight things is eight nodes and eight inspector panels. That is the wall a bigger
  mod hits first, before C++ does. It is not replaced by a script: what a player sees at once (a
  dash, a throw) must be in the shared simulation, so it stays declarative and baked; only how it
  is written changes. It comes before the client's leftovers because every step after it is
  authored with it.
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
| 1 | Slot keys and use modes | **done (M146)**: an item's key is its slot's; `use` In the hand / Press / Hold; slot keys sent held with the input, rebindable on the Keys page, suggested by a mod (`declare.SlotKey`) or the item (`slot_key`); `using.<kind>`; the dash is the boots' slot key (V). Replaced in M161: slots' keys only select, an item has its own key (`use_key`) | an item's `CbMotion.key` and declared actions (M144) are gone |
| 2 | What an item does, as helpers | **done (M147)**: under a `CbItem`, a `CbProbe` (with `release` and `hold_seconds`), the effects and the new `CbMove`, nested as they hold, with the item's When used rules (`use_conditions`, `use_changes`, `use_cooldown`, `use_emits`), bake into one motion, `Use`; the hook and the boots are written so | the left hand (piece 5) is still to come: the hook is out of sight |
| 3 | An item's state | **done (M148)**: `CbItem.fields` (name and starting value; its type from the value): a field of every copy, started by the simulation whichever way a copy is made, read and written by mods, read by looks as `hand.<field>`; the gun's rounds are the gun's (`gun.rounds`) | a state machine or a motion cannot read the hand's fields yet; the gun's HUD still reads the player's `gun.ammo` (a prediction counts it down at once) |
| 4 | Copies on the floor are lean | **done (M149)**, in place of one tree per kind: a copy lying in the world keeps its look, its body and its reactions about itself; what is about a holder is taken out, and put back from its scene when somebody has it (with four of every item lying about: 57 listeners instead of 165) | one shared tree per kind was set aside: the copies somebody carries are few (a player's slots), each must place its effects at its own muzzle and in the first-person view, and the state is the item's fields now (M148) |
| 5 | The left hand | **done (M150, M151)**: `CbItem.hand` Left keeps an item in the left hand while it is carried, beside what the slots' hand holds; a pack may bring several overlays. The grapple is not held (M151): as Attack on Titan's gear, its line is tied at the waist and the hook pulls, so the hands are free; the left arm only throws (a 0.45 s `Throwing` motion, its pack's left-arm overlay), and a two-handed gun lets go meanwhile | as in The Finals; the throw's clip is a placeholder (`Jump`'s left arm) |
| 6 | Thrown from the hand | **done (M154)**: `CbLaunch.what` "the one the player carries" (`launch_carried`) throws that copy, its fields and all; a motion hears an event said after the motions (a clip's marker) the tick after, so a throw is released on its clip's marker. The grenade is so: carried in slot 6, its key (H) plays the left arm's throw, the marker lets go, the mod refills it | what is thrown is what was held; it starts ahead of the look, not at the hand (the simulation has no pose); a count is step 4's stacks |
| 7 | An item's time is up | **done (M155)**: when a launched item's seconds are out, the simulation says `<kind>.expired` (where a mod declares it) at where it is, with who threw it, then removes it; predicted like the rest. The `frag` mod's grenade goes off on it (a fuse); the impact grenade (`grenade`) is kept, switched off | an item that was not launched has no lifetime yet; a fuse that starts when the pin is pulled (a cook) is not there |
| 8 | Launches: every hit, fast and small | **done (M156)**: an item's own impacts are kept first, all of them, ahead of the hardest 8 a tick; `CbItem.bullet` makes a kind a Box3D bullet in the world. Measured first: the level's walls already stopped a 120 m/s dart (Box3D's continuous collision against static bodies); a thin loose panel did not, and now stops a bullet | softer than 1.5 m/s is not an impact; Box3D's own note: a bullet may still miss a fast target that moves into it |
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
| The screen | **done (M158)**: a grid of `inventory.columns` x `inventory.rows` (8 x 4), the first row the hotbar; drag to move or merge (`CbDrag`), Shift-drag for half a stack (the `Split` intent), drag out to drop; icons (`CbItem.icon`, or a picture of its scene, `CbItemIcon`) | a split of one; a slot's item shown while dragged over; a key for the bag's slots |
| Stacks | **done (M157)**: `CbItem.stack`; a stack is one item with a count (`HeldItem::count`); given or picked up, onto a stack with room; a throw splits one off; `slot.count` in the HUD, `CarriedItem::count` for mods; the frag grenade stacks 3 | dropping or moving part of a stack; a mod that takes one off without throwing it |
| Containers | **done (M159)**: a `Container` component (slots of an entity's own); opened by looking at it within reach with the inventory key (the Open intent), its slots beside the player's, moved between by the same drags (`kContainerSlotBit`); `declare.ContainerTemplate`, `ctx.PutInContainer`; the chest mod | a corpse's bag (undecided: the ragdoll carries it, or a box where it fell) |
| Slots are places | **done (M161)**, asked for as in Minecraft: a slot's key selects it whatever it holds (1 to 9, 0); any item is used from the hand; a kind's `slot` is where it goes if free, nothing is pushed out; an item's own key (`use_key`, the action `<kind>.use`) uses the first copy carried; two of a kind are two items, each used on its own | an off hand (Minecraft's F) |
| Intents a mod declares | **done (M160)**: `declare.Intent`, `ctx.Intents` (once, on the tick it arrives); `CbClick.mod_intent` with two numbers; the shop mod (`shop.buy`, coins a private field) | predicted answers; numbers wider than a byte |
| The holster | **done (M137)**: on the `CbItem`, with the item's animation pack, for a mod that takes its items from its look | |
| Checked across compilers | **done (M162)**: a third reference run with slots (players coming and going, items given, keys, moves, splits, drops, own keys, the use button, chests); every build agrees | |

- **Moved out**: an item's own HUD goes to [step 5](#5-logic-as-text-places-as-nodes) (it is
  authored with the sheet); what a mod decides about containers, named, goes to
  [step 7](#7-rules-without-a-relink) (it is a rules API).

### 5. Logic as text, places as nodes

Split by kind, don't replace. A scene keeps its nodes for what is **a place or a thing**: where a
sound plays, which particle scene, which mesh glows, the hook's line, a socket. Its **logic** (when,
if, change, emit, play, spawn) moves into one text property, a sheet of rules, in the line format
the bakes already write (`motions/*.cfg`) and the expressions `src/expr` already reads. A rule names
nodes by their path, as a `CbReaction` does today.

```
on melee.hit if $other:combat.health < 50
    spawn res://vfx/spark.tscn at event.point lifetime 1
    play AnimationPlayer glow
while melee.hot
    set Barrel surface_material_override/0:emission_energy_multiplier 4
```

| # | Piece | What | Why |
|---|---|---|---|
| 1 | Measure first: a skill kit | **done (M163)**: the `brawl` mod, [measured below](#the-skill-kit-measured) | the pain decides which sheet comes first (reactions, motions or the HUD), not a guess |
| 2 | The sheet | **done (M165)**: a `.rules` file beside the scene, a `CbRules` node pointing to it; each action becomes a `CbReaction` under it when the scene is used; the nodes keep working beside it | chosen over a text property in the scene: a real editor, clean diffs |
| 3 | Checked as nodes are | **done (M165)** at publish and on the node: every name at its line, with "did you mean"; not yet: completion while typing, a mistake shown in the script editor | what nodes give for free today (step 1) is the real cost of text, not the parser |
| 4 | Reactions as rules | **done (M165, M166)**: reactions and predictions (`predict ... as ...`) | the soup is worst here: a gun's look is a dozen reactions |
| 5 | An item's use as rules | the When used rules and the motions under a `CbItem` as lines; the probe, the link and the force stay nodes where they are things with a place | the skill kit's motions |
| 6 | An item's own HUD | **done (M169)**: `CbItem.hud` and `hud_shows` (in the hand, while carried): shown once for the viewer's own player; the pistol's second use, the kit's skills each their own line. Not yet: drawn on the item's slot | from step 4; written with the sheet |
| 6b | A HUD's sheet | **done (M168)**: `show`, `text`, `bind` lines on a HUD's controls (a `CbFieldBinding` each; `hides` off lets several take turns at one label); the kit's HUD: 20 nodes into 7 and a sheet. Not for a list's rows (they are copied per entry) | the HUD was the measure's second pain: 18 nodes for 4 lines |
| 7 | The mods, converted | **reactions done (M166)**: every look's reactions and predictions are sheets (48 nodes into 13 sheets, each checked to do exactly what its nodes did); `tools/reactions_to_rules.py` converts a scene. Not yet: the HUDs, an item's use and motions (pieces 5 and 6 first); the rifle (licensed, local) | the examples are what modders copy |

- **What it is not**: a script. Nothing in a sheet runs on its own; it is baked like the nodes, and
  the security model (no code in a look) holds.

#### The skill kit, measured

`server_mods/brawl` (M163; on by default since M165, alone: `--mods inventory,combat,brawl`): a fist with a three-hit
combo, a dash strike (X), an uppercut (K), a ground slam (L), a guard (hold Alt), cooldowns on the
HUD. Made with today's nodes; `net_brawl` plays it.

| Part | Nodes | Values set | What they are |
|---|---|---|---|
| The five skills | 31 | 106 | an item each; probes, impulses, moves, two motions |
| Their clocks | 6 | 16 | a While motion per cooldown and two for the combo's fade |
| The HUD | 20 | 18 | two labels per skill (READY, or the seconds), each with a binding |
| The reactions | 9 | 59 | one per event: four hits and four whooshes that differ only in event and volume |
| The rules (C++) | | 138 lines | damage, the guard, the slam's landing |

| What hurt | Where | What would take it away |
|---|---|---|
| One node per event | reactions: 30% of every value set, for 8 sounds | **piece 4 first**: reactions as rules, a line each |
| A label cannot choose its text | the HUD: 18 nodes for 4 lines | a sheet for the HUD (or text that says "READY" when a value is 0) |
| No cooldown to read | 3 While motions, 3 fields, a condition and a change on each skill: only to show seconds | a skill's cooldown as a value (`cooldown.<kind>`), kept by the simulation |
| No timer | the combo's fade: 2 motions and a field | a field that counts down by itself |
| Damage is not a node | every hit is an event the C++ turns into `combat.damage` | the rules API (step 7), or a damage the probe says |
| No "who is near", no "on landing" | the slam's shockwave is C++, so it is not predicted | a sphere probe; a motion `when` landed |
| No mod can change another's damage | the guard only softens the kit's own hits | a damage step between mods (step 7) |
| Keys | free letters are few (X, K, L, Alt); a Roblox fighting game uses 1 to 4 to *use* a skill | **done (M164)**: `CbItem.slot_key_uses`, per item; the kit's skills are on 2 to 5 |

- **Found and fixed on the way** (the engine): a probe that does not fly never said its
  `hold_emits`; a probe refused anything nearer than 1 m (a rope's rule: a punch whiffed at punching
  distance); `using.<kind>` was never set for looks, and a HUD did not know `carried.<kind>`,
  `using.<kind>` or `motion.<name>` (the docs said it did); tests ran every disabled mod.
- **Not measured**: clicks in the editor. The scenes were written as text; the counts above are what
  the inspector would have taken one by one.
- **A long sheet** that wants functions or loops is the sign for [a script for looks](#later-not-scheduled), later.

### 6. The client, finished

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

### 7. Rules without a relink

Today a server mod is C++ compiled into `cb_server`: a compiler, a relink and a restart for every
change of a rule. Rules run on the server only, so nothing about them has to be deterministic or
shared with clients.

| Piece | What |
|---|---|
| A script the server loads | a mod's rules in a sandboxed script (the same `Declare` / `Start` / `Tick` and the same `Context` verbs), loaded from the mod's folder at start |
| Reloaded while it runs | a changed rule is taken up without restarting the server: with step 2, an edit to a rule is seconds from being played |
| C++ stays | for what a script is too slow for; both are mods to everything else |
| The SDK writes it | step 1's scaffold writes the script, not a `.cpp` |
| What a mod decides, named | the containers' rules (who may open one, what it takes), from step 4: a rules API, not C++ in each mod |

## Later, not scheduled

| What | Needs |
|---|---|
| Keys the server says | the server decides each slot's key (and the actions'), and clients take them on connect (today the player's own Keys page wins over a mod's suggestion) |
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
