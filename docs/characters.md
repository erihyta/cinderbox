# Characters and animation

Characters, their state machines, and what poses a player. Part of the [manual](../README.md#the-manual).

## Characters

A server has a **list of characters**: its own (below), those its mods declare, and those its
map's bodies are (`CbCharacterBody`, [maps](maps.md)). Every body is one of them, with its own
capsule, state machine, hitboxes and movement ([Characters per body](#characters-per-body)).
Players do not bring characters of their own. The server's own:

```bash
cb_server                      # the default: ual_mannequin where it is built (below), else mannequin
cb_server --character critter  # a workshop item [--workshop DIR]
cb_server --character none     # the procedural placeholder rig
```

| Kind | Lives in | Server reads | Players need |
|---|---|---|---|
| shipped with the game (`mannequin`) | `godot/characters/<name>/` | `bin/characters/<name>/` (the build copies the baked files there) | nothing: it is in the base game |
| workshop item (`critter`) | `characters/<name>/client/` | the item's zip in the workshop | the exact item (by SHA-256) |

### Characters per body

*Built in the simulation and on the server (M216), drawn so by the client (M217), with its own
eyes and first person (M218).*

| Piece | How |
|---|---|
| A mod's character | `declare.Character( "zoo.dragon" )`: a `CbCharacter` scene in the mod's client project, baked into `characters/<name>/` of its item (as a character item's is). Players download it with the mod's item |
| A body becomes it | `ctx.SetCharacter( SlotTarget( slot ), dragon )`: from the next tick, every simulation runs that body as it; an invalid handle gives the server's own back |
| Its capsule | a `CollisionShape3D` with a `CapsuleShape3D` right under the `CbCharacter`, as a Godot character has its collision shape: its radius and whole height (none: 0.3 m round, 1.6 m tall). When a body changes character its feet stay where they were |
| Its state machine | its own: the body's layers start over when it changes. Animation packs are fitted to each character's skeleton |
| Its hitboxes | the server poses and hit-tests each body as its own character |
| Its movement | its `movement` (walk speed and the rest), over the server's for its bodies |
| In the schema | `ModSchema::characters`: [0] the server's own, then the mods'; each its name, mod, state machine, capsule and movement |
| Its eyes | a `Camera3D` in its scene at the eyes (the **first-person camera**: `first_person_camera`, or the scene's one camera); under a `BoneAttachment3D` it rides on that bone. **First person only with one**: without, the view key does nothing and a first-person view is the view from behind in every simulation. An `Eye` marker (a `Node3D` named Eye, or `eye`) says where the eyes are without allowing first person |
| Where shots start | its eyes as posed (the camera's, else the Eye marker's place); else its head; else the top of its capsule. A first-person view looks out from the camera's height above the feet |
| Drawn | each body by its own character's `character.tscn` (its `ragdoll.tscn` when it falls), posed on its skeleton by its state machine, its feet where its capsule has them; a body that changes character is drawn again by the new one's scene, with what it holds. One whose files cannot be read is drawn with the server's own character's look (a warning), still moving as its own |

### Bodies nobody drives, and possession

*M220.* A player's slot drives a body; the body is what the mod gives it. Joining spawns the
server's own character and drives it, as before.

| | |
|---|---|
| A body nobody drives | `ctx.SpawnCharacter( character, feet, yaw )`, or a body its player left: no controller, no animation state; a physics body of its capsule reaching down to its feet, its rotation locked, so it stands, falls, is pushed and settles (the physics engine sleeps it). Drawn as its character in its rest pose, hit as its capsule. It costs what a prop costs |
| Possessing one | `ctx.Possess( slot, body )`: the body gets the controller (its state machine starts over), the slot drives it from the next tick; the camera follows. The body left stands where it was; what it held is put away, not dropped, and stays its own |
| What goes with the slot | its keys, its name; what is the body's stays with it (its fields, its items, its character) |
| A prop as a body (M221) | `ctx.Possess( slot, prop )` on a loose prop (`ctx.CanPossess` says which): its own physics body turns kinematic and keeps its shapes, upright, facing as it faced; the controller walks it with a capsule fitted to it, its feet at the prop's bottom; its bounds go along and stop its move where they would meet a wall, a player or another driven prop, so a big box does not fit a small doorway. It is drawn as itself, hit as its own shapes, seen from behind only (no first person), the camera as far back as it is big. Left, or its player gone, it is a loose body again where it stands, with the speed it had |
| A body the map places | a `CbCharacterBody` in the map's scene (test_ground has a critter): its character's item is announced, and the body stands there from the first tick, nobody's |
| A driven prop's bounds (M225) | they keep out of the world, static bodies, players and other driven props, moving and turning: a turn that would put a corner in a wall is not taken, a move stops where they would meet something and slides along it; a crate and a player do not shove each other (as two players do not) |
| A body a mod drives (M222) | `ctx.Drive( body, input )`: an NPC. The same input a player's keys make; the body moves in the same passes as the players, after them, in NetId order; every simulation goes on with its last input until a change arrives (a `Drive` command, sent only when it changes), so a client predicts it as it predicts another player. Hit by its hitboxes like a player. It uses items as a player does (M228): `ctx.GiveItem`, its slot's key in `slotKeys` to select one, `BtnUse` to use it (an item kind's sheets run for it); the combat mod hurts and kills it, at full health from the start |
| Its way about (M227) | the server's navigation mesh, made from everything static when the game starts: `ctx.FindPath`, `NearestWalkable`, `WalkToward` give the input that walks a body there ([server-mods.md](server-mods.md)). The `waves` mod is the example: NPCs with the brawl fist that find the nearest player and jab it |
| Not yet | a `CbCharacter` with no skeleton; an item as a body; NPCs using the C++ melee and gun kits (they read players' slots only) |

### The default character: the mannequin

The [Universal Animation Library](https://quaternius.com) mannequin by Quaternius (CC0), in
`godot/characters/mannequin/`:

- **Source**: `source/UAL1_Standard.glb`, the in-place version (the `_RM` files carry root motion,
  which the simulation does not want: it moves the player itself). Its import retargets the
  UE-style bones (`pelvis`, `spine_01`, `upperarm_r`) onto `SkeletonProfileHumanoid` with
  `source/bone_map.tres`.
- **State machine**: an ordinary `AnimationTree` in `character.tscn` (see
  [State machines](#state-machines)): locomotion, jumps, and an upper-body layer for the pistol
  (with a shot on `item.use`) and the bat (with a swing that strikes on a marker; the bat's
  fire is the melee mod's own look).
- **Hitboxes**: capsules along the spine, arms and legs sized from the bone lengths, a head
  sphere, a hips box.
- **Edit it** in the editor like any scene: saving the scene bakes it. The generator that made
  it only re-bakes an existing scene; `-- --force` builds it from scratch (and discards edits):

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_mannequin.gd
```

Items held in a hand (the pistol, the bat) use a hand frame that is the same on every rig
(`AnimSet::AttachFrame`), so an item scene made for the hand socket sits the same on the
mannequin and the placeholder rig alike.

### The paid animation pack (local only)

The library's paid **Source** version has 120 animations, among them jogs in eight directions. It
must never reach GitHub (the repository is public), so neither it nor anything baked from it is
committed:

| Where | What | In git |
|---|---|---|
| `Universal Animation Library[Source]/` | the purchase | ignored |
| `godot/characters/ual_mannequin/` | `source/UAL1.glb`, the scene, the baked clips | ignored |
| `make_mannequin.gd --pack=source` | how to build it | committed (names only) |

To build it: copy `Unreal-Godot/UAL1.glb` to `godot/characters/ual_mannequin/source/`, give its
import the same settings as `mannequin/source/UAL1_Standard.glb.import` (the bone map), then:

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_mannequin.gd -- --pack=source --force
```

Its locomotion is a 2D blend space: idle in the middle, the eight jogs on a circle at the game's
jog speed (3 m/s), a walk inside and the sprint ahead; `turn_legs` is off and `face_forward` on. `cb_server` picks
`ual_mannequin` by default where it is built, and `mannequin` everywhere else; its test
(`ual_mannequin`) skips itself when the character is not there, as on CI.

### Workshop characters

A workshop character is a **workshop item**, like a mod's look: the server announces it by name and SHA-256
and players must already have that exact item (a missing one refuses the join). It is shipped in
Godot's runtime format, and **nothing is imported or converted when a player joins**: the item
already holds what the game needs, baked in the editor.

| In the item, under `characters/<name>/` | Made by | Read by |
|---|---|---|
| `character.tscn`: the model, a `CbCharacter` at the root, a `CinderboxSkeleton`, `CbHitbox` zones | the author | clients (the player prefab) |
| `skeleton.ozz`, `clip_<n>.ozz` (one per animation the tree plays), `anim.cfg`, `graph.cfg` | the bake (on save, and when the item is packed) | clients (poses) and the server (the state machine, hit tests) |
| `hitboxes.cfg` | the bake | the server |

With `--character none`, players use the procedural placeholder rig, which has default zones
(head, torso, arm, leg).

### A character that is not a humanoid

Any skeleton bakes and plays (M219). What a humanoid has (Godot's `SkeletonProfileHumanoid` bone
names, as its retargeting gives them) it gets; another skeleton does without, and the rest is the same.

| | A humanoid | Another skeleton (`characters/critter`: four legs, a tail) |
|---|---|---|
| Its state machine, clips, hitboxes, capsule, movement, eyes | its own | its own |
| Aim and look chains, the eye's bone | bones by profile name | bones by their own names (`NeckBase:0.5 Skull:1`) |
| Dying with a ragdoll | the game's ragdoll | none: a kill that asks for one leaves none (`CharacterInfo::ragdoll`) |
| Animation packs | fitted by bone names | a pack that fits too few of its bones (under half) is not swapped in: its own layer plays on (`CharacterInfo::packs`) |
| Items | held in its hand sockets | still its own (in its slots: skills work), drawn only in a socket it has |
| First person | arms and legs drawn for the eye | its body hidden from the eye, the view from its camera |

A skeleton with some of a humanoid's bones and not the rest is warned about at the bake (most likely
an import that lost them). `make_critter.gd` builds the critter, a script so a fresh clone can
rebuild it, then baked by the same code as the button:

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_critter.gd -- --out=<abs path>/characters/critter/client/characters/critter
```

### Making a character

1. Make an item project: `characters/<name>/client/` with `project.godot` and a "Mod" export
   preset (copy `characters/critter/client`). `tools\pack_mod.ps1` copies the Cinderbox viewer extension into
   it; do that once (or copy `godot/cinderbox.gdextension` and `godot/bin`) before opening it in
   the editor.
2. Import the model with Godot's humanoid retargeting: in the import dialog, Skeleton3D → Retarget
   → Bone Map with `SkeletonProfileHumanoid`, and the rest fixer's Apply Node Transforms and
   Normalize Position Tracks. Bones then have profile names (`Hips`, `Head`, `LeftUpperArm`...).
3. Make `res://characters/<name>/character.tscn`:
   - a `CbCharacter` root: `character_name`, and the paths to the `Skeleton3D`, the
     `AnimationPlayer` and the `AnimationTree`;
   - the `AnimationTree`: the character's state machine ([State machines](#state-machines)). Copy
     the mannequin's to start: legs by speed and a jump on a base layer, and an
     upper-body layer for items' packs to swap (the mannequin's is a rest, weight 0);
   - the imported model under it, with the `Skeleton3D` at the root's origin, unrotated and
     unscaled (the bake checks this);
   - a `CinderboxSkeleton` whose `skeleton_path` points at the `Skeleton3D`, with `retarget` off
     (the baked skeleton is that skeleton) and `draw_bone_boxes` off;
   - `BoneAttachment3D` nodes with `CbHitbox` children: sphere, capsule or box shapes, each with a
     `zone` ("head", "torso", "arm", "leg", or your own);
   - the aim chain on the `CbCharacter`: `aim_chain` (bones with weights, turned in order, e.g.
     `UpperChest:0.3 RightUpperArm:1`) and `aim_tip` (the bone that ends up on the line of sight);
     `look_chain`, the bones that bend with the camera's pitch ([Facing](#facing)).
     The default, `RightUpperArm:1` to `RightHand`, points the right arm.
   - its eyes: a `Camera3D` at them (under the head's `BoneAttachment3D`, looking the way the
     body faces) if it may be seen in first person; else, optionally, a `Node3D` named `Eye`
     there, for where its shots start. The mannequin's rides 9 cm above its head bone;
   - `movement`, if the character moves in its own way: [movement parameters](server-mods.md#movement-parameters)
     by name, `walk_speed` = 2.4 for a walk clip made at that speed. They win over the server's
     own; what is left out is the server's. A name that is no parameter, or a value outside its
     range, is a warning on the node and stops the bake.
4. Save the scene. That bakes it: the `.ozz` files, `anim.cfg`, `graph.cfg` and `hitboxes.cfg` are written next
   to the scene (clips are sampled at `sample_rate`, 30 Hz). Publishing bakes it again from the
   scene that ships, so an item is never stale. The **Bake character** button on the
   `CbCharacter` does the same by hand (for an animation saved to its own file, which a scene save
   does not see). A bake that changes nothing writes nothing.
5. List `character.tscn` in the preset's `export_files` and the baked files in its
   `include_filter` (see the critter's preset), then publish:

```powershell
powershell -ExecutionPolicy Bypass -File tools\publish_mod.ps1 -Character <name>
```

`characters/critter` is a complete example ([A character that is not a humanoid](#a-character-that-is-not-a-humanoid)).

## Animations

One system poses every player: the character's **state machine**, authored as a Godot
`AnimationTree`, baked, and run by the simulation ([State machines](#state-machines)). Its clips
are sampled with ozz into the pose every client draws and the server hit-tests. Nothing else poses
a player.

| Character | Its state machine |
|---|---|
| the mannequin, the critter, any workshop character | baked from the `AnimationTree` in its scene |
| the placeholder rig (`--character none`) | built into the engine: one layer (idle, walk and run by speed, a jump), six procedural clips, no assets |

The placeholder rig is drawn as one box per bone, with the bone names of Godot's
`SkeletonProfileHumanoid`: `Hips`, `Spine`, `Chest`, `UpperChest`, `Neck`, `Head`,
`Left/RightShoulder`, `UpperArm`, `LowerArm`, `Hand`, `UpperLeg`, `LowerLeg`, `Foot`, `Toes`. That
is the profile Godot retargets imported characters onto, so a character imported the normal way
is driven with no mapping of our own.

### Godot animation on players: cosmetic only

**One rule:** the ozz pose, computed from the simulation's state, is the only thing that places a
player's body. It is the pose the server poses hitboxes with, so a body drawn any other way would
be shot where it is not. Godot animation may add to a player what the pose leaves alone: faces,
fingers, props, materials, effects.

This is enforced, not just advised. A `CinderboxSkeleton` driving a `Skeleton3D` gives it a
`CbPoseModifier` as its first skeleton modifier, which applies the ozz pose again after any
`AnimationPlayer` or `AnimationTree` has run. Modifiers after it (look-at, spring bones) run on top of
the pose; the character bake warns about them and about an `AnimationTree`, naming the bones that
have hitboxes.

What a character's animations do besides moving bones (particles, sounds, lights) stays in the
animations and plays in step with the pose: see
[An animation's other tracks](#an-animations-other-tracks).

```sh
# check that the ozz pose wins over Godot animation on a driven skeleton
godot --headless --path godot --script res://addons/cinderbox_maps/check_pose_wins.gd
```

### Ragdolls

A `Kill` command can leave a ragdoll: eleven Box3D bodies joined by cone-and-twist and hinge joints,
built from a fixed standing pose in the simulation (`src/sim/ragdoll.h`). It is simulation state,
identical on every machine, pushable, shootable, and it piles up with props and other ragdolls.

Clients draw it with the player's own prefab (or `prefabs/ragdoll.tscn` if there is one): every joint
of the skeleton follows the nearest body part, so any character works, and the pose the player was
last drawn in is blended into the ragdoll over 0.15 s, so there is no snap.

### Driving an imported character with ozz

Point a `CinderboxSkeleton` at a `Skeleton3D` and it retargets onto it: each bone is rotated
relative to **its own rest**, so the character keeps its proportions, and the hips move by an
amount scaled to its height. Our placeholder rests with its arms down while the humanoid profile
rests in a T-pose, and the difference between those two postures is bridged when the skeleton is
bound, so arms end up down rather than sticking out.

Turn `retarget` off to force every bone to exactly where our rig has it, which only makes sense for
a character built to our proportions.

```sh
# a humanoid with long legs and short arms, posed from the simulation's animation state
godot --headless --path godot --script res://addons/cinderbox_maps/check_retarget.gd
```

## State machines

A character's animation logic is authored as a Godot `AnimationTree`, the normal way, and baked.
Every character has one: it is the only thing that chooses what a player's body plays. The
simulation runs the baked machine every tick, so the server's hit tests and every screen agree and
rollback replays it exactly; the tree itself never runs in the game.

| In the tree | Baked as |
|---|---|
| root: a state machine, or a blend tree of state machines stacked with `Blend2` nodes | layers (as many as the tree stacks); a `Blend2`'s filter is the layer's bone mask |
| an `Add2` in that blend tree, its add input a state machine | a layer added on what is below it (its pose's difference from the rest), by its `add_amount`: an expression in `graph_inputs` (`"Lean/add_amount": "speed > 2"`), else the tree's number; its filter is its mask |
| a `OneShot` in that blend tree, its shot input an animation (through a `TimeScale` for another speed) | a layer that plays the animation once from the tick its request turns true: `"Kick/request": "attack"` in `graph_inputs` (needed). Asked again while it plays, it starts over without dropping out; held true, it plays once. Its fade in and fade out times, its filter, its mix mode (Add: added, as an Add2's); its markers fire as a state's. Not baked: autorestart, fade curves (warned) |
| states: `Animation` nodes, `BlendSpace1D`, `BlendSpace2D` (points are animations, play mode forward or backward) | clip states, blend states (phase-synced, so feet stay in step; 2D blends inside Godot's triangles) |
| a state that is a `BlendTree`: one of those, through a `TimeScale` node, into its output | the state at that speed: the node's `scale`, or an expression for it in `graph_inputs` (`"Swing/Scale/scale": "attack_speed"`), read every tick, held between 0 and 16. Its markers come with it (a swing at half speed hits half as soon), and an At End transition waits for the slower clip |
| an `Animation` node's custom timeline, stretched (`stretch_time_scale`) | the clip played in the timeline's length: the bat's whole swing in 0.6 s whatever the clip's own length (with a TimeScale too, both) |
| transitions: Auto advance, advance condition, advance expression, priority, crossfade, Immediate / At End | the same (Sync switching becomes Immediate) |
| a transition's `xfade_curve` | the fade through the curve (17 samples, read between them): an ease in, a snap at the end |
| a state that is itself a `StateMachine` (nested, any depth) | its states flattened into the layer under its name (`Combat/Slash`): a transition into it goes to its start; one out of it leaves from each of its states, an At End one from where it reaches its End (on that transition's condition too); its own transitions are tried after its parent's on equal priorities. Its parameters keep their paths (`"FullBody/Combat/Aim/blend_position"`). A grouped machine is baked as a nested one (warned) |
| markers on animations | the mod event of the same name, from the player, when the clip passes it (a motion hears it the tick after, in a trigger, a wait or an `if`: the grenade's release, the fist's strike and ready) |
| every other track (particles, sounds, lights) | stays in the animation, played by clients in step ([An animation's other tracks](#an-animations-other-tracks)) |

Set it up on the `CbCharacter`:

- `animation_tree_path`: the tree. Its `root_node` should be the model (tracks' paths start there).
- `graph_inputs`: what drives the tree's numbers, by parameter path:
  `"FullBody/Locomotion/blend_position": "forward_speed"`, `"UpperBodyBlend/blend_amount": "pistol or melee"`.
  A 2D blend space takes two expressions, x then y: `"move_right, move_forward"`.
- `turn_legs`: on (the default), the hips turn toward the direction of travel so a forward walk
  goes sideways. Turn it off for a character with its own directional clips (strafes).
- `face_forward`: off by default. On, the spine is turned back by however much the clips turned the
  hips, so the chest faces where the body faces (strafe clips often turn the torso toward the
  travel); the head keeps looking ahead. The legs still run where they run.

Conditions and expressions read simulation values, never scripts:

| Name | Value |
|---|---|
| `speed`, `forward_speed` | smoothed ground speed (m/s); negative forward_speed while backing up |
| `move_forward`, `move_right` | smoothed ground velocity in the body's frame (m/s): for directional blend spaces |
| `vertical_speed`, `grounded`, `airborne_time`, `jumped` | the body's movement (`jumped`: on the tick of a jump) |
| `aiming`, `backward`, `state_time` | a mod's Aim; walking backwards; seconds in the current state |
| a stance's name (`gun`, `melee`) | true while any layer has it (mods' `SetStance`) |
| a mod event's name (`item.use`, `attack`) | on the tick it is emitted at this player: its value (1 if the value is 0), so `attack` and `attack == 2` both work: a trigger |
| ... the same event, while already in the state it leads to | starts that state over, with the transition's crossfade: a shot fired during the last one's recoil plays the recoil again (the pistol fires every 0.2 s, its clip is 0.63 s). Godot has no transition from a state to itself, so the bake derives this from the transition you drew (the pistol pack's `Hold -> Shoot` on `item.use`). The clip's own keys (a sound, a flash keyed inside it) fire again too, as long as that transition has a crossfade |
| a board field's name (`inventory.slot`) | the player's value (or the global one) |
| an item kind's name (`melee.bat`) | true while the player holds one, in any socket |
| `carried.` and an item kind (`carried.dash.boots`) | true while the player has one, in the hand or put away |
| `using.` and an item kind (`using.gun.rifle`) | true while the player uses one: in the hand with the use button down, or by its own key (the press; while held, for one used while held) |
| `motion.` and a motion's name (`motion.Hook`, `motion.grapple.hook.Use`) | true while that [motion](motions.md) of the player's is on: a pose for as long as a line holds, a flight lasts ([an item's overlay](#animation-packs) weighs by it). The node's name alone when no other motion has it, else the set's and the node's. A server's first 64 motions can be asked for |

The grammar is the [one expression language](looks.md#expressions) (`and or not`, comparisons, arithmetic,
`?name`, parentheses). A name no mod declares reads as 0 (the server logs it). The bake fails with a reason for anything it cannot run (other
blend nodes, a missing animation). It warns about what it bakes otherwise: a
custom timeline that is not stretched, its start offset, a negative speed (it plays as stopped).

A mod times its effect by the animation with `ctx.AnimationEmits( event )`: the melee mod hits on
the `melee.strike` marker of its pack's `Sword_Attack`, and on its own timer where no marker is. To
edit an imported animation (add a marker or a track), save it to a file in the import settings
(Save to File, Keep Custom Tracks). Check markers after a reimport: a reimport in Godot 4.7.1 kept
the added track but dropped the marker (4.7.2 kept both); the bake warns when a clip lost one.

## Layers and stances

A mod says what a player is doing; the character says what that looks like.

| Who | Does | Example |
|---|---|---|
| a mod | declares names: `d.Layer( "upper" )`, `d.Stance( "gun" )`, and sets a stance on a layer: `ctx.SetStance( player, layer, stance )` (a default `StanceHandle` clears it) | the gun mod sets `gun` on `upper` while a gun is out; the bat sets `melee`, and `melee_swing` for a swing |
| a character | reads stances by name in its state machine's conditions and layer weights | an upper layer weighted by `gun or melee or melee_swing` |

- A stance is a name and nothing more: no clip is tied to it. A character whose machine never
  reads `gun` simply does not change when a gun comes out.
- Stances are part of the simulation's animation state, so everyone draws what they lead to and
  the server's hit tests use it.
- Items bring their looks as [packs](#animation-packs): the mannequin's upper layer is only a rest
  (weight 0), which a held bat or gun swaps for its pack's. The placeholder rig has no states
  for stances (the engine carries no game content).

## Aiming

Aiming is part of the pose. A mod sends `ctx.Aim( player, true )` (the pistol does while it is out),
and the pose turns the character's aim chain toward where the player looks, relative to its body.
Every client draws that and every server hit test uses it, so a raised arm can be hit where it is
seen. Respawning keeps the aim; only the mod lets it go.

## Facing

| Mode | The body | Set by |
|---|---|---|
| Freelook (default) | turns toward where the player walks; the camera looks around freely | nothing |
| Camera-facing | faces where the camera looks, every tick (a shooter's stance) | `ctx.FaceCamera( player, true )`, or the player looking in first person (whatever it holds) |

The pistol and the bat switch to camera-facing while they are out. In camera-facing the **upper
body follows the camera's pitch**: looking down bows the character, looking up leans it back.

| | |
|---|---|
| What bends | the character's look chain: `Spine:0.2 Chest:0.2 UpperChest:0.2 Neck:0.2 Head:0.2` by default, each joint turning by its share of the pitch (the chest ends up at 0.6 of it, the head at all of it) |
| When | only while camera-facing; it comes and goes over 0.2 s, so drawing a weapon while looking at the floor does not snap |
| Who sees it | everyone, and the server: it is part of the pose, so a bowed head is where a headshot has to go |
| With a gun | the aim chain runs after it: the chest takes most of the pitch and the arm only the rest |
| Per character | `look_chain` on the `CbCharacter` (bones with shares; empty turns it off); a rig without some of the default bones bends at the ones it has |

The legs are not part of it: in camera-facing they still walk where the player goes: the hips turn toward the direction of travel (up to 90
degrees) and the spine turns back, and moving away from the facing plays the walk cycle backwards.
This works with any character's forward clips, no strafe clips needed.

## An animation's other tracks

A character's animations are ordinary Godot animations, authored once in its `AnimationPlayer`:
bone tracks next to any other track. One rule says where each track goes:

| Tracks | Become | Played by |
|---|---|---|
| position / rotation / scale of the `Skeleton3D`'s bones | ozz clips (the bake) | the pose (drawn by clients, hit-tested by the server) |
| everything else: value (`emitting`, `visible`, `light_energy`, material colours), method, audio, animation | nothing: they stay in the animation | each client, in step with the pose |

So a flame on the swing is two keys on the swing animation (`Flame:emitting` on at 0.1 s, off at
0.34 s), and a whoosh is an audio key, all in Godot's animation editor. There is no second file
and nothing to bake for them: when a character is first drawn, the game copies the non-bone
tracks out of its `AnimationPlayer` and plays them, per channel (the base locomotion, each stance
layer), at the clip and time the pose is playing.

- Values land exactly; method and audio keys fire once (a rollback that replays a moment does not
  fire it again; a long jump such as a join fires nothing).
- A channel whose clip is bones only returns to the `RESET` animation's values.
- No scripts: tracks call built-in methods (`restart`, `play`) or set properties.
- Looks only: the server never sees these tracks.
- **Seen from its start.** With network delay another player's swing reaches you late: the server's
  state is already some way into it. It is drawn from its start anyway and played faster until it
  is level, so it never pops from standing to mid-swing, and its tracks follow the drawn clip:
  every key fires, in order, with the pose it was keyed on.

  | First seen | Drawn |
  |---|---|
  | more than a tick and a half and up to 0.5 s in, a clip that does not loop | from 0, level with the state 0.1 s later (a 0.25 s gap: about 3.5 times as fast) |
  | a frame in (your own player, or no delay) | as it is; the keys before that point fire at once |
  | further in (a join), a loop, a blend space | as it is; values only |

  Looks only: the server hit-tests the state, not the drawn pose, so for that 0.1 s a hit can land
  a little ahead of the drawn swing. A correction that leaves a clip and comes straight back to
  the same play of it goes on where it was: it never starts over, and nothing fires twice.
- Animation packs are bones only: their other tracks are not played.

A swing can have a fire trail and a whoosh made this way. Put the nodes the tracks reach
(particles, lights, an `AudioStreamPlayer3D`) in the character scene, and list the sounds in the
item's export preset.

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/check_track_player.gd
```

## Animation packs

A mod can ship **layers** of an AnimationTree and swap a player's own layer of the same name for them:
a crouch walk for `FullBody`, a swim, a limp. The character keeps its other layers (the pistol still aims
while crouched).

| Step | Where |
|---|---|
| author | a `CbAnimPack` scene in the mod's client project: a model on a humanoid-profile skeleton, its AnimationPlayer, an AnimationTree whose state machines are named like the characters' layers; **Bake** writes `anim/<pack>/` |
| declare | `declare.AnimPack( "sneak.crouch" )` in the server mod |
| swap | `ctx.SwapLayer( SlotTarget( slot ), pack, "FullBody" )`, back with `ctx.RestoreLayer( ..., "FullBody" )` |
| fit | the game rebuilds the pack's clips for each character's skeleton by profile bone names, once (as they are when the skeleton is the same) |

**Layers.** A character's tree has `FullBody` (the whole body: its locomotion) and `UpperBody` over it
(spine, arms, head), laid on by a Blend2 with a bone filter (`UpperBodyBlend`). A pack replaces the
layers of the same names:

| The pack's scene | Replaces |
|---|---|
| a tree whose root is a state machine | `FullBody` |
| the whole tree (`FullBody`, `UpperBody`, the Blend2), `replaces = [UpperBody]` | `UpperBody` only: the full body is there to see the upper body over it in the editor, and is not baked |
| the whole tree, `replaces` empty or both names | both |
| a blend tree with one state machine named `UpperBody` on its output | `UpperBody`, through the character's own bone filter |

The SDK's starter pack is the second row. The rifle's hold is the fourth (two states: the idle,
and the shot on `item.use`).

The swap is simulation state: the server's hit tests and every client pose the swapped layer, and a
rollback replays it. The server reads the pack from the mod's workshop item; its graph travels in the
schema; each player's pack clips come from the item they subscribed to.

Retargeting assumes characters imported the Godot way: the humanoid bone map, the rest fixer's
**Overwrite Axis**, and **Fix Silhouette** when rest shapes differ (T-pose vs A-pose), so a joint's
turn from rest means the same on every skeleton. `sneak` is the example: hold C to crouch.

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_sneak_pack.gd -- --out=<abs>/server_mods/sneak/client/anim/sneak.crouch
```

**Items bring layers.** A mod can tie a pack to an item kind,
`declare.ItemLayers( bat, declare.AnimPack( "melee.hold" ) )`: while a player holds one (in use, not stowed), the pack's layers play instead of the player's own of the same names,
and stop when it is dropped. A mod's own `SwapLayer` on the same layer wins while it lasts.

| Item | Pack | Layer | States |
|---|---|---|---|
| pistol | `gun.pistol_hold` (`server_mods/gun/client/animation_packs/pistol_hold.tscn`) | `UpperBody` | `Hold`; `Shoot` on `item.use`, back at its end |
| bat | `melee.hold` (`server_mods/melee/client/animation_packs/bat_hold.tscn`) | `UpperBody` | `Ready`; `Swing` on the `melee.swing` event (the look predicts it), started over by the next one, back to `Ready` on the clip's `melee.done` marker (1.1 s, the end of the follow-through). `Sword_Attack` carries `melee.strike` (the hit) and `melee.done` (the end): the mod times the swing by the same markers, so the look and the rule cannot drift apart, and nothing waits for the server's word that a swing ended |
| rifle | `gun.rifle_hold` (its look is not in the repository) | `UpperBody` | `Hold`; `Fire` on `item.use` |
| the brawl fist | `brawl.fists` (`server_mods/brawl/client/animation_packs/fists.tscn`) | `UpperBody` | `Ready`; `Jab` and `Cross` on the sheet's `Jab` motion by `brawl.combo` (1, then 2), `Finisher` (the cross at 0.75 speed: a TimeScale, eased in by a crossfade curve) on its `Finisher`; back to `Ready` at their ends. Both clips carry `brawl.fist.strike`, the frame the fist is furthest out (0.2 s, 0.27 s): the sheet hits there (M237); and `brawl.fist.ready`, the fist mostly back (0.55 s, 0.75 s): the next punch may start from there, so spamming plays whole punches (M240). While the fist is out the upper body is the pack's, so the walk's arm swing does not show (the pack's own locomotion arms: to come) |

All four are upper-body packs: the legs keep the character's own walk, so a crouch (`sneak` swaps
`FullBody`) plays under a held bat. Their clips are files in the mod (`animation_packs/source/*.res`).
The pistol's and the bat's come from the mannequin (CC0):

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_weapon_clips.gd
```

A marker in a pack's clip emits its event like a character's (`ctx.AnimationEmits` counts packs).

**Overlays: a pose an item brings.** A pack's layer that the character does not have (any name but
the character's own: `Dash`, `Hang`) is an overlay. After its own layers a player has
`kOverlaySlots` (3) more, and every item it **carries** (in the hand or put away) whose pack has
overlays takes one for each, in the order of the items (a pack's in its own order). An overlay is
laid over the whole body (or its own bone filter) with its own weight, which says when it shows:

| Item | Pack | Overlay | Weight (its Blend2's graph input) |
|---|---|---|---|
| the dash boots | `dash.boots_dash` (`server_mods/dash/client/animation_packs/boots_dash.tscn`) | `Dash` | `motion.dash.boots.Use`: while the dash is on |
| the grappling hook (out of sight; its line tied at the waist) | `grapple.hook_arm` (`server_mods/grapple/client/animation_packs/hook_arm.tscn`) | `Grapple`, filtered to the left arm: the throw | `motion.grapple.hook.Throwing`: 0.45 s from the throw; the pull needs no hands |

- **Authored like an upper-body pack**: a Blend2 whose blend input is the overlay's state machine,
  `Replaces` naming the overlays, and each Blend2's `blend_amount` in `graph_inputs` as its weight.
  The Blend2's filter is the overlay's bone filter (the grapple's: the left shoulder, arm, hand
  and fingers); later overlays are laid over earlier ones.
- **What the hands hold wins the arms**: an overlay poses the body, and the arms of what the
  player holds (its upper-body layer) are put back over it, shoulder down. A dash's whole-body pose
  bends the body and moves the legs; a two-handed rifle stays held as its pack holds it.
- **A hand an overlay names lets go**: an overlay whose bone filter lists a hand (the grapple's and
  the grenades' throws: the left shoulder, arm, hand and fingers) takes that arm while it is more
  than half in, and a two-handed item is carried with the other hand meanwhile (a rifle during the
  grapple's throw), on every machine and in hit tests alike. An overlay without a filter takes no
  hand.
- **Its weight eases in and out** like a stance (0.2 s), and the pose is the simulation's: hit tests
  see it, and the player's own is predicted with the motion.
- **A clip that starts over on each use**: the overlay's state machine waits in one state and goes
  to the one that plays on the motion (`Ready` to `Throw` on `motion.grapple.hook.Throwing`, back on
  `state_time > 0.7`, after the weight is out).
- **No swap at the moment it matters**: the pack is in its slot as long as the item is carried, at
  weight 0; a motion turns it up the tick it starts.
- **The clips** are the SDK placeholder's (`Sprint`, `Jump`'s left arm for the grapple: CC0),
  stand-ins until better ones are put in those scenes.
The default characters still have pistol and bat states of their own: they play only on a server
whose look has no such pack.
