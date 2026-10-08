# Characters and animation

Characters, their state machines, and what poses a player. Part of the [manual](../README.md#the-manual).

## Characters

Everyone on a server plays as one character, chosen by the server:

```bash
cb_server                      # the default: mannequin, shipped with the game
cb_server --character robot    # a workshop item [--workshop DIR]
cb_server --character none     # the procedural placeholder rig
```

| Kind | Lives in | Server reads | Players need |
|---|---|---|---|
| shipped with the game (`mannequin`) | `godot/characters/<name>/` | `bin/characters/<name>/` (the build copies the baked files there) | nothing: it is in the base game |
| workshop item (`robot`) | `characters/<name>/client/` | the item's zip in the workshop | the exact item (by SHA-256) |

### The default character: the mannequin

The [Universal Animation Library](https://quaternius.com) mannequin by Quaternius (CC0), in
`godot/characters/mannequin/`:

- **Source**: `source/UAL1_Standard.glb`, the in-place version (the `_RM` files carry root motion,
  which the simulation does not want: it moves the player itself). Its import retargets the
  UE-style bones (`pelvis`, `spine_01`, `upperarm_r`) onto `SkeletonProfileHumanoid` with
  `source/bone_map.tres`.
- **State machine**: an ordinary `AnimationTree` in `character.tscn` (see
  [State machines](#state-machines)): locomotion, jumps, and an upper-body layer for the pistol
  (with a shot on `pistol.fired`) and the bat (with a swing that strikes on a marker; the bat's
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
mannequin, the robot and the placeholder rig alike.

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
import the same settings as `mannequin/source/UAL1_Standard.glb.import` (the bone map, and
`Sword_Attack` saved to `res://characters/ual_mannequin/animations/`), then:

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

### Making a character

1. Make an item project: `characters/<name>/client/` with `project.godot` and a "Mod" export
   preset (copy `characters/robot/client`). `tools\pack_mod.ps1` copies the Cinderbox viewer extension into
   it; do that once (or copy `godot/cinderbox.gdextension` and `godot/bin`) before opening it in
   the editor.
2. Import the model with Godot's humanoid retargeting: in the import dialog, Skeleton3D → Retarget
   → Bone Map with `SkeletonProfileHumanoid`, and the rest fixer's Apply Node Transforms and
   Normalize Position Tracks. Bones then have profile names (`Hips`, `Head`, `LeftUpperArm`...).
3. Make `res://characters/<name>/character.tscn`:
   - a `CbCharacter` root: `character_name`, and the paths to the `Skeleton3D`, the
     `AnimationPlayer` and the `AnimationTree`;
   - the `AnimationTree`: the character's state machine ([State machines](#state-machines)). Copy
     the robot's or the mannequin's to start: legs by speed and a jump on a base layer, the mods'
     stances on an upper-body layer;
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
   `include_filter` (see the robot's preset), then publish:

```powershell
powershell -ExecutionPolicy Bypass -File tools\publish_mod.ps1 -Character <name>
```

`characters/robot` is a complete example: a rigid robot, taller than the built-in rig, generated by
a script so a fresh clone can rebuild it (`make_character_example.gd`), then baked by the same code
as the button:

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_character_example.gd -- --out=<abs path>/characters/robot/client/characters/robot
```

## Animations

One system poses every player: the character's **state machine**, authored as a Godot
`AnimationTree`, baked, and run by the simulation ([State machines](#state-machines)). Its clips
are sampled with ozz into the pose every client draws and the server hit-tests. Nothing else poses
a player.

| Character | Its state machine |
|---|---|
| the mannequin, the robot, any workshop character | baked from the `AnimationTree` in its scene |
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
| states: `Animation` nodes, `BlendSpace1D`, `BlendSpace2D` (points are animations, play mode forward or backward) | clip states, blend states (phase-synced, so feet stay in step; 2D blends inside Godot's triangles) |
| transitions: Auto advance, advance condition, advance expression, priority, crossfade, Immediate / At End | the same (Sync switching becomes Immediate; crossfades are linear) |
| markers on animations | the mod event of the same name, from the player, when the clip passes it |
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
| a stance's name (`pistol`, `melee`) | true while any layer has it (mods' `SetStance`) |
| a mod event's name (`pistol.fired`, `attack`) | on the tick it is emitted at this player: its value (1 if the value is 0), so `attack` and `attack == 2` both work: a trigger |
| ... the same event, while already in the state it leads to | starts that state over, with the transition's crossfade: a shot fired during the last one's recoil plays the recoil again (the pistol fires every 0.2 s, its clip is 0.63 s). Godot has no transition from a state to itself, so the bake derives this from the transition you drew (`Pistol -> Shoot` on `pistol.fired`). The clip's own keys (a sound, a flash keyed inside it) fire again too, as long as that transition has a crossfade |
| a board field's name (`inventory.slot`) | the player's value (or the global one) |
| an item kind's name (`melee.bat`) | true while the player holds one, in any socket |

The grammar is the [one expression language](looks.md#expressions) (`and or not`, comparisons, arithmetic,
`?name`, parentheses). A name no mod declares reads as 0 (the server logs it). The bake fails with a reason for anything it cannot run (nested
state machines, other blend nodes, a missing animation).

A mod times its effect by the animation with `ctx.AnimationEmits( event )`: the melee mod hits on
the mannequin's `melee.strike` marker, and on its own timer for characters without one. To edit an
imported animation (add a marker or a track), save it to a file in the import settings (Save to
File, Keep Custom Tracks), as the mannequin does with `Sword_Attack`. Check markers after a
reimport: a reimport in Godot 4.7.1 kept the added track but dropped the marker (4.7.2 kept both).
The mannequin's generator puts its marker back whenever it bakes.

## Layers and stances

A mod says what a player is doing; the character says what that looks like.

| Who | Does | Example |
|---|---|---|
| a mod | declares names: `d.Layer( "upper" )`, `d.Stance( "pistol" )`, and sets a stance on a layer: `ctx.SetStance( player, layer, stance )` (a default `StanceHandle` clears it) | the pistol sets `pistol` on `upper` while it is out; the bat sets `melee`, and `melee_swing` for a swing |
| a character | reads stances by name in its state machine's conditions and layer weights | the mannequin's upper layer: `Rest -> Pistol` when `pistol`, weight `pistol or melee or melee_swing` |

- A stance is a name and nothing more: no clip is tied to it. A character whose machine never
  reads `pistol` simply does not change when the pistol comes out.
- Stances are part of the simulation's animation state, so everyone draws what they lead to and
  the server's hit tests use it.
- The mannequin and the robot have states for the shipped mods' stances; the placeholder rig has
  none (the engine carries no game content).

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
- With network delay, a swing the server started is first seen a little way in, and method and
  audio keys before that point do not fire (values do). Sounds that must not be missed belong to
  a [reaction](looks.md#reactions) on the mod's cue.
- Animation packs are bones only: their other tracks are not played.

The robot's bat swing has a fire trail and a whoosh made this way. Put the nodes the tracks reach
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
and the shot on `rifle.fired`).

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
`declare.ItemLayers( bat, declare.AnimPack( "melee.carry" ) )`: while a player holds one (in use, not stowed), the pack's layers play instead of the player's own of the same names,
and stop when it is dropped. A mod's own `SwapLayer` on the same layer wins while it lasts, so a
crouch still crouches with a bat in hand and the carry returns when the player stands up. The bat's
pack replaces `FullBody`: standing ready, a measured walk, the usual jog.

```sh
godot --headless --path godot --script res://addons/cinderbox_maps/make_carry_pack.gd -- --out=<abs>/server_mods/melee/client/anim/melee.carry
```
