# Cinderbox — Roadmap

What comes next, in order, and why. [DESIGN.md](DESIGN.md) says how things work today;
[docs/HISTORY.md](docs/HISTORY.md) is what is done. Rewritten after M187: the old seven steps are
done or parked, and what they left is listed by area below.

## Where it stands (M187)

| Area | What there is |
|---|---|
| The simulation | deterministic on six compilers and three OSes (four reference runs, cross-loaded snapshots); rollback, prediction, a lead for the viewer's own player, a catch-up for others' swings |
| Movement | motions (presses, holds, events; impulses, forces, probes, links, launches), movement parameters, weight |
| Items | one scene each: body (any shapes), look, key, use rules, fields, pose, left hand, throws, stacks; slots as places; the inventory screen; chests; intents and a shop |
| Looks | rules sheets (reactions, predictions, a HUD's lines, an item's use), checked while typing; animation packs and overlays; companion tracks |
| Maps | `CollisionShape3D` children for everything: boxes, balls, capsules, hulls on any body; triangle meshes and terrain for the level; props and templates drawn by their own nodes; `--map NAME` |
| The client | the menu, Test locally, a server's mods before joining, typed text, a wrong guess taken back |
| Modding | the SDK knows the server's names; Test mod; looks republished while the server runs; C++ server mods |

## Next, in order

A proposal: the order is open to change. Each row is a milestone or two.

| # | What | In one line |
|---|---|---|
| 1 | [Loose ends](#1-loose-ends) | **done (M190 to M192)**: rolling and drag on bodies, slopes too steep to walk, the arms belong to what is held, the camera against meshes and terrain, the two network tests that miss a tick |
| 2 | [Destructible walls](#2-destructible-walls) | **done (M195 to M198)**: a wall breaks into pieces authored beforehand, by what is made to break it; shared by everyone |
| 3 | [Rules as a language](#3-rules-as-a-language) | **done (M204 to M211)**: the rules sheet grows into a small language the simulation runs: everything that moves on screen predicted, a mod's C++ left with what only the server decides. [The reference](docs/rules-language.md) |
| 4 | [A player's slot changes nothing](#4-a-players-slot-changes-nothing) | **done (M212)**: every player's motions in one pass, every player's move in another; what a motion does to another player lands on the same tick, after the target's own |
| 5 | [The language made regular](#5-the-language-made-regular) | **done (M213 to M215)**: one table of where each line may stand, one word per role, one origin for shapes, `use` a motion, every amount an expression; [the reference](docs/rules-language.md) rewritten from what is built; the motion lines in the debug HUD |
| 6 | [Characters per entity](#6-characters-per-entity) | **mostly done (M216, M217)**: the server's list of characters, each body its own state machine, hitboxes, capsule and movement, in the simulation, on the server and in the client's drawing. Left: the camera per scene; where shots start |
| 7 | [Any skeleton](#7-any-skeleton) | the humanoid's features become what a character has, not what every one must; characters with no skeleton |
| 8 | [The animation tree, wider](#8-the-animation-tree-wider) | what a Godot developer reaches for next (nested state machines, OneShot, Add, TimeScale, crossfade curves), and layers that are the look's alone |
| 9 | [Entities and who drives them](#9-entities-and-who-drives-them) | possession: a player is a controller that drives a body; one `Spawn` for every scene; an NPC is a body a mod drives |
| 10 | [Finding the way](#10-finding-the-way) | a navigation mesh on the server, for a mod's brains |
| 11 | [The proof: waves](#11-the-proof-waves) | NPCs that walk to the nearest player and swing at it, waves that grow |

Steps 4 and 5 of the last version (vehicles, authoring finished) are [Later](#later), with the
building system.

- **Why loose ends first**: each is small, and each is felt in play: a ball that never stops, a
  player who runs up a 60° slope, a rifle held wrong while dashing, a camera that looks through a
  hillside.
- **Why walls before vehicles**: the shapes work (M183 to M187) was for them; what is left is the
  fractured asset and the swap, and the physics questions are answered already (below).
- **Why vehicles next**: the other big "a sandbox should have this"; they need joints in
  templates, which walls held together by glue would use too.
- **Why the language before vehicles** (decided after M202): what a mod's C++ does to bodies is
  seen a lead late (brawl's slam throws people with `ctx.Push`; a slow is `ctx.SetMove`), because
  no node could say it: no "when it lands", no "everyone near this point". Adding one node per case
  would not end; a few general words do. Vehicles' input-to-motors is then written in it too.
- **Why the slot order first** (decided after M211): small, and a fighting kit leans on it: every
  `find` that shoves another player depended on who joined first.
- **Why the language made regular before anything new** (decided after M211): every word added
  since M204 brought its own rule of where it may stand; each new one would add more. Cleaning it
  while eight milestones old is cheap; the mods and the SDK's starter are the only sheets.
- **Why characters before NPCs** (decided after M215): an NPC wants a body of its own (a zombie,
  a dragon, a chicken), and authoring one should be what a Godot developer does for any character:
  a scene with its skeleton, its animation tree, its hitboxes, made from zero in the editor. Today a
  server has one character, humanoid. So the body comes first, then who drives it.
- **Why possession, not an NPC type**: a player is a controller (its keys, its name, its score);
  what it drives is a body (an entity whose scene is a character). An NPC is then nothing new: a body
  a mod drives. The same split gives spectators, Prop Hunt (a player drives a crate), and mind
  control. (Not vehicles: a possessed body moves by the character controller; a vehicle is a body
  of motors, so a player sits in it and keeps its own.)
- **Vehicles and authoring later**: the core engine first; the nodes (motion nodes and the rest)
  are reworked once it is complete, and kept as they are until then.

### 1. Loose ends

| Piece | What |
|---|---|
| Things that roll stop | **done (M190)**: `rolling` and `drag` on every body (`CbBody`, a template's `Material` and `Body`, an item's file); a map's `CbProp` starts at rolling 0.05. The case: a ball rolling without slipping loses nothing to friction (that is physics), so it rolls until a wall stops it. Box3D has rolling resistance (spheres, capsules) and damping; a body gets `rolling` and `drag` beside `friction` and `bounce` (a beach ball: much drag; a bowling ball: little). Defaults of 0 keep every reference hash |
| Slopes too steep to walk | **done (M190)**: `max_slope`, 50°: steeper ground (a ramp, a ball's side) slides the player down. The case: measured (M188): there is no limit. The mover stands wherever its ray down finds ground, so a player stands still on 70° and runs up 60° at nearly full speed; nothing slides. A movement parameter `max_slope` (the ground's normal from that ray; Unity's default is 45°, Unreal's 44.8°; ours would start at 50°): steeper ground does not hold the player up, so it slides down along the slope, cannot walk or jump up it, and lands on what is below. A mod can change it (a climbing game, ice). Every map today is walkable below it (the sandbox's steepest ramp is 45.8°, the arena's 20°), so the reference hashes should not move; a box's edge under the ray is the case to check |
| The arms belong to what is held | **done (M191)**: the held item's arms are put back over an overlay, shoulder down; only an overlay whose filter names a hand takes it. The case: an overlay (the boots' dash pose, the hook's hang) is laid over the whole body after the held item's layer, so it repaints the arms that hold a two-handed rifle, and the left hand is pulled to a rifle that is no longer where the pose put it. The rule: what the hands hold wins the arms. An overlay still poses the legs and the body; the arms keep the item's pose, unless the overlay says it takes a hand (the grapple's throw does: then that hand lets go, as now). In the shared pose, so hitboxes follow; the dash and the hook poses are placeholders either way |
| The camera against surfaces | **done (M192)**: the live and replay sources keep a static copy of the level (`LevelQuery`) and answer the camera with a sphere cast in it; a view file keeps the frame's test. The case: the camera stops at boxes, balls, capsules and hulls' bounds; a mesh's or terrain's bounds would be a whole hillside, so it skips them and can look through a cave wall. It could cast its ray in the client's own simulation, which has every shape exactly |
| Tests that miss a tick | **done (M190)**: they record the server's state at every tick it runs (its tick observer). The case: `net_flight` and `net_grapple` look up exact ticks they sampled from a real-time run and now and then miss one (fails locally about one run in three or five; CI has caught it once on macOS). They should record every tick |

### 2. Destructible walls

**Kept apart from the sandbox**: a wall is static, so it costs nothing in rollback, and only what
is made to destroy (a charge, a rocket) breaks it, not any loose body that bumps into it.

| Piece | What |
|---|---|
| A fractured asset | **done (M195, M196)**: `CbBreakable`, its `CbProp` children its pieces; the Fracture button cuts a wall into irregular chunks (Voronoi cells, convex hulls). The plan: a wall and its pieces authored beforehand (as The Finals bakes how a wall shatters): each piece a body of its own shapes (M185), drawn by its own node |
| The swap | **done (M195)**: `ctx.Break`; the frag grenade breaks walls near it. The plan: a destruction event takes the wall out of the simulation and puts its pieces in, moving as the blast says; shared by everyone like a ragdoll |
| Who may move the pieces | **done (M195)**: `CatDebris`. The plan: a collision filter, not mass: a player's capsule is kinematic, which the solver treats as infinitely heavy, so no mass keeps it from shoving debris. Debris gets a category the capsule's shape does not collide with, while the mover's ground and wall casts still include it: players stand on rubble and are stopped by it, but cannot push it; shots move nothing unless a mod pushes |
| Settling | **done**: Box3D's own sleeping (on by default; the simulation adds nothing of its own). Turning a long-asleep piece static was **dropped**: a sleeping body is already skipped by the solver, and a static one would still be an entity in every snapshot, so it would save next to nothing. A mod can have pieces go after a while (`seconds`) |
| Holes, not the whole wall | **done (M198)**: a blast takes out the pieces near it; the rest stand, static, while touching pieces join them to the held ones (`held_at`); the rest fall |
| Glue | **done (M202)**: `CbBreakable.glue`: its pieces bodies from the start, welded (Box3D's weld joints) where they touch, the held ones static; a weld past its glue breaks, so it sags, cracks and falls by its own physics |

### 3. Rules as a language

[docs/rules-language.md](docs/rules-language.md) is the grammar (what is built is marked). In short: a `motion`
block in a `.rules` sheet, run by every simulation, with triggers (`on use`, `on press`, `on EVENT`,
`while`, and `when`: once, as an expression turns true), waits (`then when`, `after`, `every`),
queries (`find` everything inside a shape, `cast` a shape along a direction; a block runs per
target found), effects on the owner or each target, and numbers that are expressions, with Godot
`Curve`s read in them. Shapes, markers and paths stay nodes, drawn in the 3D view; the sheet names
them. The motion nodes keep working and get nothing new.

| Piece | What |
|---|---|
| Blocks | **done (M204)**: `motion NAME TRIGGER`, `when`, `then when ... within`, `after`, `timeout`, `if` / `else`, `end`; today's `use` sheets unchanged; the brawl slam's landing is its sheet's |
| Finding | **find done (M205)**: `find SHAPE`, its shape an `Area3D` or `CollisionShape3D` of the scene (a convex one made from a mesh too) or written inline; a block per thing found; `away`; `move for` given to a player found (a slow, a root); the brawl slam's shockwave is its sheet's. **cast done (M206)**: the probe in a sheet, a line, a ball or a drawn shape swept along the look; the grapple is a sheet. `distance` / `reach` come with curves |
| Curves and markers | **curves done (M207)**: an impulse's and a force's strength as an expression; `curve(Name, x)` from a `CbCurve`; `time`, and `distance` / `reach` in a find. **markers done (M208)**: directions and launch points from `Marker3D`s, `turn (yaw, pitch)`, `follow` a `Path3D` |
| Items run motions | **done (M209)**: `owner item`: an item in the world runs its own sheet's motion when its event is said at it (`on expired`); `break`, `remove self`; the frag grenade's blast predicted (its numbers the mod's options, as fields) |
| Physics words | **done (M210)**: `impulse ... kind momentum` (N·s, so weight counts) beside the change of velocity; `every S` (the rest of a step, again and again) |
| The mods converted | **done (M211)**: brawl, frag, cannon, grapple, flight, dash and the grenade mod are sheets; their C++ left with damage, charges and rules; the SDK's starter motion set is a sheet |

- **Not "server mods on clients"** (a route not taken, below): the language is data in the look,
  which clients have already, and deterministic and bounded by design. A mod's C++ stays on the
  server.
- **The parked pieces become words**: touching a wall is a condition (`wall`) and a direction, a
  teleport a statement, passing through a movement parameter.

### 4. A player's slot changes nothing

**Done (M212).** Before, `MoveCharacters` ran each player's motions and then its mover, slot by
slot: a push from a higher slot reached a lower one a tick late, a `find` saw lower slots already
moved, and a target's own lunge (`replace horizontal`) wiped out a push from a lower slot only. Now
every player's motions run first, on the world as the tick found it; what they do to other players
is kept and lands after the targets' own; then everyone moves. `motion_order` swaps two players'
slots and checks they end where they did, to the bit.

### 5. The language made regular

What M204 to M211 built works, but each word came with its own rule of where it may stand and how
its numbers are written, and [docs/rules-language.md](docs/rules-language.md) mixes the built and
the planned (and contradicts itself). The sheets that exist are the mods' and the SDK's starter, so
changing the language now costs little; old spellings stay accepted for a while and a tool rewrites
sheets.

| Piece | Today | After |
|---|---|---|
| One rule for numbers | an impulse's and a force's strength take a bare expression up to the first option word; a launch's speed only in brackets (`speed (EXPR)`); `break` a number, a name or brackets; `move`, `lift`, `ahead`, `seconds` plain numbers | **done (M214)**: every amount; the motion's times, counts, directions and a cast's range and speed stay numbers (they shape the motion, or are needed before anything is read), and a move's value is read when its step starts. The plan was: every number may be an expression: bare up to the next option word, or in brackets, everywhere (settings like `cooldown` read when the motion starts) |
| One table of where a line may stand | each statement has its own rule, said in its own error: `cast` only in the first step, not under an `if`, once; `every` once a step, not under an `if`, only one-off lines after it; a find's lines only `impulse`, `move for`, `emit`; an item's motion no steps, `cast`, `cooldown`, `uses`, `lasts`, `until`, `move`, `end`, nor an `if` on its first line | **done (M213)**, the relaxing in part: `change` in a find is in; `if` around `every` or a `cast` is not (an `every` is the rest of its step, a cast is the motion's, not a step's), nor `launch` in a find. The plan was: the places a line can be (a step, an `if`, after `every`, in a find, in a cast, an item's motion) and what each allows: one table in the code that the parser's errors and the reference are both made from. Then relax what is cheap: `if` around `every` and `cast` (an `if` is decided when its step starts, and a cast's step is the first), `change` and `launch` in a find |
| One word per role | the thing a block is about is `found` in a find and `hit` in a cast (the reference says `target`) | **done (M213)**: `target` in both (`found`, `hit` still read); `$target:FIELD` when fields of the target come |
| One spelling per direction | `along world direction (0, -1, 0)` in the sheets, `world (x,y,z)` in the reference; a find's `along facing` turns its shape, where `along` is a direction everywhere else | **done (M213)**: `along world (x, y, z)`; a find's shape turned by `turned facing\|look\|none` |
| One origin for shapes | a drawn find shape around the player's feet; one written in the line (`sphere 3`) around its middle, a metre up; a cast's around the line's tip | **done (M213)**: every find shape at the feet, facing as the find says, with `at (x, y, z)` to move it (no shipped sheet writes one inline); a cast's at its tip, as now |
| An item's motion is one instant step | its limits listed one by one | **done (M213)**: said once: an item's motion happens in one tick (so no waits, nothing lasting, no cast); it has no look or keys (so directions are `up`, `world`, `away`, `to`) |
| `use` is a motion | `use` stays beside `motion NAME on use`, its own thing | **done (M213)**: `use [if ...]` is exactly `motion Use on use [if ...]` |
| The reference | built and planned mixed, the status table contradicting itself (curves "planned" and "built"), `at POINT` and `PATH at T` shown as if they work | **done (M215)**: `docs/rules-language.md` lists only what works, one table per statement with where it may stand; what is planned moves to a short list at the end |
| A motion inspector | nothing shows why a motion did not happen | **done (M215)**: the debug HUD (F1) lists each motion that is on (its step, what it waits for, how long, what its cast holds) and the last refused start with its reason. The plan was: the debug HUD (F1) gets a row per player: each motion that is on, its step, since when, what it waits for, its target; a refused start says which condition failed |

### 6. Characters per entity

A character is a scene authored in the Godot editor, from zero, as a Godot developer would author
any character (a player, a monster, a pet): a model on a `Skeleton3D`, an `AnimationPlayer`, an
`AnimationTree`, `CbHitbox` and `CbSocket` nodes. Saving it bakes it, as today. What changes: a
server has many, and every body says which it is.

| Piece | What |
|---|---|
| Characters are kinds | **done (M216, M217)**, as `declare.Character`; a client draws each body as its own (M217). The plan: a character scene is a kind by its name (`zoo.dragon`), in a mod's look as an item is, or a character item; the server announces every character it uses (players download what they lack, by its hash, as today) and the schema lists them |
| Each body its own | **done (M216)** on the server and in every simulation (`CharacterKind`, `SetCharacter`). The plan: a character entity says its kind; the simulation runs that kind's state machine for it and the server hit-tests that kind's hitboxes; its animation state is sized by its own layers |
| Its capsule | **done (M216)**: a `CapsuleShape3D` under the `CbCharacter`. The plan: its height and radius from the scene (today one size for everyone): the mover keeps the feet where they are |
| Its camera | **every body is seen from behind** (third person), its distance and height from the scene; **first person only where the scene has a first-person camera** (a node placed at the eyes): without one the view key does nothing. The mannequin gets one, so it plays as now |
| Where shots start | the first-person camera's place if the scene has one, else an eye marker, else the top of its capsule (today: the head, a humanoid's) |
| The game's own | the mannequin and the placeholder rig stay what a server uses when it names none |

### 7. Any skeleton

What assumes a humanoid becomes what a character has. A humanoid has all of it; a dragon brings its
own skeleton, state machine, hitboxes and sockets, and has only what fits.

| Piece | Today, for everyone | After |
|---|---|---|
| The ragdoll | eleven bodies from a fixed humanoid pose | a humanoid's as now; another skeleton's authored on its scene (bodies on bones, joints), or none: it does not fall as a ragdoll |
| Hands and holding | sockets by name, two-handed grips, the arms put back over an overlay | a character holds in the sockets it has; one with none holds nothing (a mod's `Give` refuses) |
| Animation packs | retargeted by humanoid bone names | they fit the skeletons that have those names; another skeleton's are its own |
| The first-person body | the arms and legs copies, the torso hidden | the humanoid's, where the scene has a first-person camera and a humanoid skeleton |
| The look and aim chains | named bones (already per character) | as now |
| Hit zones | `head`, `chest`, ... | names a character gives its hitboxes; a mod reading a zone handles one that is not there |
| No skeleton at all | the placeholder rig is a skeleton | a character that is a mesh and its capsule: no pose, no state machine, hit-tested as its capsule (a ball game, a top-down shooter) |

### 8. The animation tree, wider

The state machine runs in the simulation (its state decides the pose, the pose the hitboxes, and the
server hit-tests those), so it is baked into a deterministic subset of Godot's `AnimationTree`. The
subset grows toward what a Godot developer reaches for, and what only the eye needs is let loose.

| Piece | What |
|---|---|
| More of the tree | nested state machines, `OneShot`, `Add`, `TimeScale` (a state's playback speed), crossfade curves; each baked and run the same everywhere |
| Layers that are the look's | a layer a character marks as its look's runs on the client alone, with all of Godot's `AnimationTree` (a face, breathing, a tail's wag): it may not move a bone that has a hitbox or say a marker a rule hears, and the bake refuses it if it does |

### 9. Entities and who drives them

Possession, as Unreal has it (a controller possesses a pawn): the keys are the controller's,
always the same (move, look, jump, sprint, the actions); what they do comes with the body (its
motions, its sheets, its items: a chicken's jump can be a flap). The client stays what it is: it
shows what the simulation says, from the server's frames and the player's own input.

| Piece | What |
|---|---|
| One spawn | `ctx.Spawn( kind, position, yaw, velocity )`: whatever the scene of that kind is: an item lying in the world, a map's template (named, no longer numbered), a character. `Give( holder, kind, socket )` puts an item in a hand (today's `SpawnItem`). The old verbs stay a while as thin wrappers |
| Possession | `ctx.Possess( slot, entity )`: the player of that slot drives that body from the next tick; the camera follows (the viewer already follows its own entity). A joining player is given a body of the server's character, as now; a mod may give it another (a class, a dragon form) |
| What is whose | **the body's**: its fields (health), its items and inventory, its motions, its ragdoll. **The controller's**: its name, its score, its private fields, its keys. What you see is what it has; who you are stays with you |
| The body left behind | stays in the world: idle, driven by a mod, or removed, as the mod says |
| Driving without a player | `ctx.Drive( entity, input )`: the same input a player's keys make, each tick, in the frame as a list of what changed (one sender, so cheap). **An NPC is a body a mod drives**: no type of its own, not under the 64 players' arrays; moved in the same two passes as players, after them, by NetId; a client predicts it as another player (its last input again) |
| Rules see bodies | `Kill`, `Respawn`, `Push`, `SetMove`, fields, events, hit tests work on any body; a `find` gains `npcs` beside `players`; the combat mod hurts and kills any body |
| Not predicted at first | a possession is the server's, so it is seen a lead late (the old body moves on a moment); a sheet's `possess` could predict it later |
| What it opens | NPCs, spectators (a controller with no body, or a free camera's), Prop Hunt, mind control. A body possessed moves by the character controller; a vehicle does not, so it keeps its seat |

### 10. Finding the way

Server only, so it need not be deterministic: a navigation mesh made from the map's level when the
server starts (Recast/Detour), `ctx.FindPath( from, to )`, and a mod's helpers to walk it.

### 11. The proof: waves

A `waves` mod: NPCs of their own character that walk to the nearest player and swing at it, waves
that grow. Tested in a network test (hashes equal, no desyncs) and with bots; how many NPCs a client
carries, measured. Later: a brain in a sheet (deterministic, so every simulation runs it and an
NPC's moves are predicted exactly, at a rollback cost).

## Later

Postponed after M211 (the core engine first). In the order they would likely come back.

### Vehicles

| Piece | What |
|---|---|
| Joints in templates | a template of several bodies (each its shapes) joined by Box3D's joints (revolute, prismatic, weld, wheel), authored as nodes in the scene |
| A seat | a player sits: the mover is off, the player is carried by the body and drawn in its pose. (a seated player keeps its body; the vehicle reads its keys) |
| Input to motors | as a motion routes input to the mover, a vehicle routes it to its joints' motors and its forces; the rules (who may drive, damage) are a mod's |
| Platform carry | standing on a moving deck carries the player (its velocity and turn): the mover needs it for a vehicle you stand on, and ships later |
| A body's own controls (an idea, later) | every body says how it is driven: the character controller (the mover, as today), a vehicle's (its motors and forces), others. Possessing a body takes its controls with it: the same keys drive a character on foot and a car its motors, and a seat could become a possession. Waits for step 9 (possession) and vehicles |

### Authoring, finished

| Piece | What |
|---|---|
| Names inside expressions | completion offers names in a property that holds one and in a sheet; not inside a condition or a format in the inspector |
| A prebuilt SDK | CI builds the extension, so a mod's look needs no compiler |
| Publish checks a change | a `min` / `max` on a change |
| An item used in its preview | press the item's key in the editor's preview and watch what it does |
| The preview, fuller | cues with someone else in them (a hit); the pack chosen from a list; the character's own mesh |
| A rules change restarts the test | publishing a look while the server runs takes effect at once unless the simulation reads it (an item's body, a pack's state machine, a motion): then the hosted test could restart the server itself |
| The SDK's prose | `sdk/README.md` around the starters is from before the redesign |
| The nodes reworked | the authoring nodes (the motion nodes among them) are kept as they are, not maintained, until the core engine is complete; then reworked together |

### Building

Players placing pieces into the world (walls, ramps, floors), the way a base is built.

| Piece | What |
|---|---|
| Placing | a sheet statement that places a template on a grid in front of the player (snapped, turned in steps), so the placer's own simulation shows it on the press, as a launch does |
| The preview | the ghost of the piece where it would go, drawn by the viewer from the same cast the placement uses |
| Holding together | pieces joined where they touch (glue's weld joints, M202), each breakable; what loses its hold falls |
| Rules | what a piece costs, who may build where, how many: a mod's |
| Staying | a server mod saves and loads what was built (server only, not the simulation's concern) |
| The cost | every piece is state in every snapshot: a cap per player and per server, and static pieces where possible |

### A second server model (considered)

Asked after M211: a model for many more players, chosen by the server's owner. Not scheduled; what
it would be:

| | Today's model (lockstep, rollback) | The second model |
|---|---|---|
| Other players | simulated from their inputs, predicted, rolled back | shown from the server's states, interpolated (a little in the past); only the local player predicted |
| Props | moved by bodies and by actions | moved only by actions (a charge, a blast, a rocket, a swing that hits): a player's body does not push or wake them. The props are then a function of the actions alone, so every client simulates them from the actions the server confirms (with the tick and the place) and rolls back only when one arrives late, not every tick |
| Two players on one prop | both push it, each predicted | neither pushes it with its body; both hit it, and the two hits land in tick order everywhere |
| Player against player | bodies collide; a shove is predicted on the shover's screen | no collision between players; a shove is the server's, felt by the victim when it arrives (as in Roblox) |
| What scales | 64 players: every client steps everyone | the players a client sees (area of interest) and the actions, not every input of every player |

- **The same core**: the simulation, the sheets, the mods and the viewer are shared; only who is
  simulated and what the frames carry differ. A server says which model it runs; the client picks
  the source for it (the viewer already does not know where frames come from).
- **What it costs a game**: bodies that shove props (kicking a crate, a ball rolled by walking into
  it) and predicted knockback on other players; a remote player's swing shows a little after its
  effect on a prop (two timelines: players in the past, props in the present).
- **Open**: a prop pulled by a player (a grapple's rope) couples the two every tick: its puller's
  positions would be sent as actions while it pulls; far props (outside the area of interest) are
  either all simulated (cheap when few and asleep) or kept by region with a snapshot on entering.

## Parked

Decided to wait; each says what it waits for.

| What | Waits for |
|---|---|
| Rules without a relink | as long as possible (asked). A server mod's rules as a sandboxed script the server loads and reloads (the same `Declare` / `Start` / `Tick` and `Context` verbs), C++ kept for what is too slow; the SDK's scaffold would write the script. Rules run on the server only, so any language would do |
| How players find servers | undecided: Steam, a website's "play" link that starts the game, a game's own menu or a desktop shortcut. Until then: join by address, Test locally, a server's mods listed before joining |
| Steam Workshop as the store | the decision above; the store is a folder today |
| Ships | Box3D growing what they lack (buoyancy) rather than more patches (two today: `cmake/patches/`). In the simulation, so everyone predicts them: platform carry (vehicles, later), several shapes per body (done), a buoyancy component with a wave function the water's look shares |
| **The player's body, for motions** | a mod that wants it; once step 3 is in, each is a word of the language ([the language's planned list](docs/rules-language.md#planned)). Small engine pieces, each in the simulation (so a motion using them is predicted like any other), asked about after M200. In the order they would likely be wanted: |
| · Touching a wall | the mover already meets walls when it clips the velocity; it would say so: a condition (`wall`: touching one, beside or ahead) and a frame along it (`along_wall`, `off_wall`) for forces and impulses. Wallrunning, wall jumps, a climb that knows where the wall ends (no wall ahead: over the ledge), a ledge grab |
| · Teleport | a motion effect that sets where the player is (`CbTeleport`): to the target (what the probe found, or a field's entity) at an offset in its frame (1.2 m behind it, facing its back), or to the probe's point. Blink, "behind you", swapping places, a return-to-mark |
| · What the body passes through | movement parameters: `pass_players`, `pass_props`, `pass_level` (0 or 1): the mover ignores those (the player's own capsule too, so others' casts pass it). A dash that goes through the target and ends behind it (the fast-move blink, without a teleport), phasing through crowds, a ghost or a spectator flying through walls (`pass_level` with `move_frame` 1) |
| · Not hittable | a movement parameter `hittable` (0 or 1): shots, probes and swings pass the player's hitboxes. A dodge roll's invulnerable frames, a phase skill. (Damage itself is the combat mod's; this is whether anything reaches the body) |
| · The capsule's size | movement parameters for its height and radius, so a crouch fits under a low gap (the sneak mod's crouch only lowers the pose), a shrink or grow potion, a slide under a door. The mover keeps the feet where they are and does not stand up where the full height would not fit |
| · Keep speed, steer | a motion effect that turns the velocity toward a frame without changing its size (and trades height for speed: a dive speeds up, a climb slows). An elytra-like glide (today's forces push toward a set speed, whatever the pitch), a grind rail, a ricochet |
| · Hidden from others | a field the viewer reads to fade a player's body (and its held item) for everyone but the player itself. Invisibility, a cloak, a spectator. (Presentation only: the body still collides and is hit, unless the pieces above say otherwise) |

## Leftovers, by area

What the old steps left, not scheduled. A row moves up into "Next" when it is wanted.

| Area | What |
|---|---|
| Items | a state machine or a motion reading the hand's fields; a lifetime for an item that was not launched; a fuse that starts when the pin is pulled (a cook); bots that use what a mod adds; a bot count in the menu |
| The inventory | a split of one; a slot's item shown while dragged over; a key for the bag's slots; dropping or moving part of a stack; a mod taking one off a stack without throwing it; an off hand (Minecraft's F); a corpse's bag (undecided: the ragdoll carries it, or a box where it fell); predicted answers to a mod's intents; numbers wider than a byte in an intent |
| Sheets and HUDs | a sheet for a list's rows; an item's HUD drawn on its slot; the mods' HUDs, item uses and motions converted to sheets (their reactions are; step 3 does the motions) |
| Motions | a lasting force as a C++ command; a test of one player hooked to another; a knocked-down ragdoll state; carrying a player; a preview panel |
| Animation | a state's playback speed (the bat's whole swing in 0.6 s, a slow-motion finisher; markers move with it); the dash's and the hook's poses are placeholders |
| Feet on the ground | foot IK, **in the shared pose**, beside the aim chain: each foot's ground found by a ray in the simulation (the server's for hit tests, the client's prediction for drawing: the same shapes, so the same answer), the hips lowered to the lower foot, two-bone IK for each leg. Then the legs' hitboxes are where the feet are drawn. Not Godot's IK nodes: they would run after the pose (visual only, and the bake warns: the legs have hitboxes), and Godot's physics has no level to find (collision is Box3D's) |
| The client | keys the server says: a server maps keys to what they do, an item's own key included (its action `<kind>.use`), sent once at join; a game's "Q is the dash strike" whatever slot it is in. Costs nothing: the client already turns keys into action bits each frame, only the table's source changes. A player may still rebind (the Keys page, per server), the server's map being the start; cameras a mod places (a round's end, a kill cam). **The UI stays presentation-only** (decided after M211): a screen shows what fields say, is moved through with input, and sends intents from its buttons and boxes (as a Minecraft server's menus are chests). What that cannot say becomes a widget the engine gives (a text box, a slider, a drag, a placement preview), not a script |
| Games | teams and spectators (mods, once private fields and the board cover them); lists on one entity (array fields, or a `CbList` over a field family) |

## Routes not taken

| Route | Why not |
|---|---|
| Server mods also run on clients | tried as M55 and dropped: rules would ship to clients and every mod would need to be deterministic |
| A built-in dash, a built-in grapple | rules in the engine; mods could only tune what the engine thought of |
| The viewer guesses a command (`CbPrediction` with a push) | the viewer has no simulation: a guessed push cannot collide, and a pull would fight every frame's correction |
| Separate physics for the map and the game | there is one world; a map's bodies and a mod's are authored the same way (M185) |
| Godot's physics at runtime | collision is Box3D's on the server and in every client's prediction; Godot's `CollisionShape3D` is only how shapes are authored |
| Hitboxes as physics bodies | a player is one kinematic capsule in the physics; the hitboxes are shapes on the shared pose that hit tests ask (shots, probes, a melee swing). As bodies, each bone would be moved every tick (15 per player), copied in every snapshot, and shove props with its limbs. Ragdolls are bodies already, when a player falls |

## Housekeeping

Independent of the path; each is small.

| What | Note |
|---|---|
| The rest of `simulation.cpp` in modules | props, ragdolls, items and animation; the mover went first (M92) |
| Tools on every platform | the publish, pack and SDK scripts are PowerShell |
| A motion's field and a mod's | both may write one (commands first, then motions); the server could warn when a mod sets every tick a field a motion writes |
| Slots for what is worn | the dash boots take a slot: selected, they are held like a tool |
| `reconcile_every` 2 as the default | halves a client's rollback work with many players and latency (M139); others are seen up to 2 ticks later: to be looked at in play first |
| The characters' part of a step | as much as the physics at 64 players (0.4 ms, M140) and not yet looked into |
| Physics threads beside drawing | on by default (M141), not yet watched in the game: `--physics-threads=0` if frames suffer |
| The old `build/` folder in the checkout | builds go outside the source tree; the folder is ignored and can be deleted |
