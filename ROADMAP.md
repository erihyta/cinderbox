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
| 3 | [Vehicles](#3-vehicles) | bodies joined in a template, a seat, input routed to motors |
| 4 | [Authoring, finished](#4-authoring-finished) | the editor's leftovers: names in expressions, a prebuilt SDK, an item used in its preview, publish checks |

- **Why loose ends first**: each is small, and each is felt in play: a ball that never stops, a
  player who runs up a 60° slope, a rifle held wrong while dashing, a camera that looks through a
  hillside.
- **Why walls before vehicles**: the shapes work (M183 to M187) was for them; what is left is the
  fractured asset and the swap, and the physics questions are answered already (below).
- **Why vehicles next**: the other big "a sandbox should have this"; they need joints in
  templates, which walls held together by glue would use too.
- **Authoring after**: it waits until the content above settles, as the client did.

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
| Glue | not done: with step 3's joints. The plan: blocks held by Box3D's weld joints with a breaking force, for things that should wobble before they break: the joints of step 3 |

### 3. Vehicles

| Piece | What |
|---|---|
| Joints in templates | a template of several bodies (each its shapes) joined by Box3D's joints (revolute, prismatic, weld, wheel), authored as nodes in the scene |
| A seat | a player sits: the mover is off, the player is carried by the body and drawn in its pose |
| Input to motors | as a motion routes input to the mover, a vehicle routes it to its joints' motors and its forces; the rules (who may drive, damage) are a mod's |
| Platform carry | standing on a moving deck carries the player (its velocity and turn): the mover needs it for a vehicle you stand on, and ships later |

### 4. Authoring, finished

| Piece | What |
|---|---|
| Names inside expressions | completion offers names in a property that holds one and in a sheet; not inside a condition or a format in the inspector |
| A prebuilt SDK | CI builds the extension, so a mod's look needs no compiler |
| Publish checks a change | a `min` / `max` on a change |
| An item used in its preview | press the item's key in the editor's preview and watch what it does |
| The preview, fuller | cues with someone else in them (a hit); the pack chosen from a list; the character's own mesh |
| A rules change restarts the test | publishing a look while the server runs takes effect at once unless the simulation reads it (an item's body, a pack's state machine, a motion): then the hosted test could restart the server itself |
| The SDK's prose | `sdk/README.md` around the starters is from before the redesign |

## Parked

Decided to wait; each says what it waits for.

| What | Waits for |
|---|---|
| Rules without a relink | as long as possible (asked). A server mod's rules as a sandboxed script the server loads and reloads (the same `Declare` / `Start` / `Tick` and `Context` verbs), C++ kept for what is too slow; the SDK's scaffold would write the script. Rules run on the server only, so any language would do |
| How players find servers | undecided: Steam, a website's "play" link that starts the game, a game's own menu or a desktop shortcut. Until then: join by address, Test locally, a server's mods listed before joining |
| Steam Workshop as the store | the decision above; the store is a folder today |
| Ships | Box3D growing what they lack (buoyancy) rather than more patches (two today: `cmake/patches/`). In the simulation, so everyone predicts them: platform carry (step 3), several shapes per body (done), a buoyancy component with a wave function the water's look shares |
| **The player's body, for motions** | a mod that wants it. Small engine pieces, each in the simulation (so a motion using them is predicted like any other), asked about after M200. In the order they would likely be wanted: |
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
| Sheets and HUDs | a sheet for a list's rows; an item's HUD drawn on its slot; the mods' HUDs, item uses and motions converted to sheets (their reactions are) |
| Motions | a lasting force as a C++ command; a test of one player hooked to another; a knocked-down ragdoll state; carrying a player; a preview panel |
| Animation | a state's playback speed (the bat's whole swing in 0.6 s, a slow-motion finisher; markers move with it); the dash's and the hook's poses are placeholders |
| Feet on the ground | foot IK, **in the shared pose**, beside the aim chain: each foot's ground found by a ray in the simulation (the server's for hit tests, the client's prediction for drawing: the same shapes, so the same answer), the hips lowered to the lower foot, two-bone IK for each leg. Then the legs' hitboxes are where the feet are drawn. Not Godot's IK nodes: they would run after the pose (visual only, and the bake warns: the legs have hitboxes), and Godot's physics has no level to find (collision is Box3D's) |
| The client | keys the server says: a server maps keys to what they do, an item's own key included (its action `<kind>.use`), sent once at join; a game's "Q is the dash strike" whatever slot it is in. Costs nothing: the client already turns keys into action bits each frame, only the table's source changes. A player may still rebind (the Keys page, per server), the server's map being the start; cameras a mod places (a round's end, a kill cam); a script for looks (sandboxed, for what fields, intents and clicks cannot say; not visual scripting) |
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
