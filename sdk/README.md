# Cinderbox SDK

The Godot project a mod's **look** is made in: its item scenes, effects, HUD, animations and state
machines. No game and no server are needed to author. The rules are the mod's C++ half
(`server_mods/<mod>/<mod>.cpp`).

## Start

| Step | Command |
|---|---|
| 1. Build the extension (once, and after engine changes) | `cmake --build --preset godot-export` |
| 2. Put it into this project | `powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -Setup` |
| 3. Make a mod's project from this one | `powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -New <mod>` |
| 4. Open `server_mods\<mod>\client` in Godot 4.7 and author | |
| 5. Publish | `powershell -ExecutionPolicy Bypass -File tools\publish_mod.ps1 -Mod <mod>` |
| 6. Restart the server and the game | |

`-Update <mod>` refreshes a project's extension and placeholder (close the project in Godot first).

## What is in the project

| Path | What | Shipped in the mod? |
|---|---|---|
| `prefabs/`, `vfx/`, `ui/`, `assets/` | the mod's scenes, effects, HUD, sounds and textures | yes |
| `animation_packs/` | the mod's animation pack scenes; `animation_packs/source/` for their models and clips | no (what they bake to is) |
| `anim/<pack>/` | baked animation packs: written on save and when publishing, never edited | yes |
| `motion_sets/` | the mod's motion scenes: `CbMotionSet` roots with `CbMotion` nodes and their parts ([docs/motions.md](../docs/motions.md)) | no (what they bake to is) |
| `motions/<set>.cfg` | baked motions: written on save and when publishing, never edited | yes |
| `items/<kind>.cfg` | baked item bodies: written when publishing, never edited | yes |
| `placeholder/` | the placeholder: a humanoid skeleton with no model (`skeleton.tscn`; the editor draws its bones) and its locomotion clips (`clips/`) | no |
| `cinderbox.gdextension`, `bin/` | the Cinderbox extension: the `Cb*` nodes, the bakers, the **Cue Preview** panel | no |
| `starters/` (in `sdk\` only) | what `-New` copies into a mod's project, with the mod's name in it | no |

- **Nothing needs listing.** The "Mod" export preset ships every resource except the folders
  marked "no".
- **Every mod's files share one `res://`** in the game: name files after the mod
  (`vfx/<mod>_flash.tscn`).
- **No scripts.** A mod's look is scenes and resources; the game refuses a pack with a script.

## Finding the nodes

The editor's **Create New Node** dialog lists the Cinderbox nodes a mod is made of under
**Favorites**, on its left: `CbReaction`, `CbPrediction`, `CbItem`, `CbLinkLook`,
`CbMotionSet`, `CbMotion`, `CbProbe`, `CbImpulse`, `CbForce`, `CbLink`, the HUD nodes (`CbList` and `CbKey` among them), and the character and map nodes. The extension puts them
there the first time a project is opened with it. One you take out of the favorites stays out.

## What `-New <mod>` makes

| File | Start from it to |
|---|---|
| `prefabs/<mod>.tscn` | make the item: a `CbItem` (its kind, name, mass), its body (a `CollisionShape3D`), two markers for where the hands hold it, a `Muzzle` for its effects |
| `vfx/reactions_<mod>.tscn` | predict a press (`CbPrediction`), react to events (`CbReaction`) |
| `ui/hud_<mod>.tscn` | show fields while the item is out (`CbFieldLabel`) |
| `animation_packs/<mod>_animations.tscn` | an animation pack: the default AnimationTree in full, replacing the upper body |
| `motion_sets/<mod>_moves.tscn` | add to how players move, predicted: a push on a press, with a charge and an event (a `CbMotion` with a `CbImpulse`) |

Files that already exist are kept. Delete the starters a mod does not need.

## The files that are not scenes

| File | Who writes it | What it is | Edit it? |
|---|---|---|---|
| `project.godot` | Godot | the project's settings. Only its name matters: the mod's look does not ship its project settings | in Project Settings |
| `export_presets.cfg` | the SDK | the "Mod" preset: what is packed into the mod. Every resource, except the folders the SDK owns (`exclude_filter`), plus the baked files (`include_filter`) | no: `-Update` replaces it |
| `cinderbox.gdextension` | the SDK | tells Godot where the Cinderbox extension's libraries are (`bin/`) for each platform | no |
| `*.import` (next to every model, sound, texture) | Godot | how that file is imported: for a model, its bone map, skeleton name and each clip's loop mode. This is what the Import dock and Advanced Import Settings edit | in the Import dock |
| `*.uid` | Godot | the file's id, so moving it does not break references | no |
| `items/<kind>.cfg` | publishing | an item as the **server** reads it (its box, mass, where the hands hold it, its properties) and as the game finds it (its scene, name, first-person view). Baked from the item scene's `CbItem`, on save and when publishing | no: edit the scene |
| `motions/<set>.cfg` | saving a motion set, publishing | the mod's motions as text: the press, the condition, the effects, the fields and the event. What every simulation runs | no: edit the scene |
| `anim/<pack>/graph.cfg` | saving a pack, publishing | the pack's state machines as text: clips, layers, states, transitions, conditions. What the simulation runs | no: edit the tree |
| `anim/<pack>/anim.cfg` | the same | which `.ozz` file is which clip, and the skeleton they were made on | no |
| `anim/<pack>/*.ozz` | the same | the skeleton and the clips, in the format the game plays | no |
| `..\client_item.cfg` (beside `client\`) | publishing | the published mod's SHA-256: the exact pack the server tells players they need | no |

- **Baked files are outputs.** They are overwritten on every save and publish; a change made in
  them is lost.
- **Publishing checks the pack** the way the game does when it loads it: a script, or a file outside
  the folders a mod may have, stops the publish with the reason.

## Animation packs

A pack is an ordinary Godot scene: a model, an `AnimationPlayer` and an `AnimationTree`, under a
`CbAnimPack` root. Select the root to read what the pack does (its Editor Description).

| Node | What to do with it |
|---|---|
| `CbAnimPack` (root) | `character_name` is the pack's name: the server mod asks for it by that name. `replaces` lists the layers it ships |
| `Skeleton3D` | the placeholder skeleton: what the clips play on. Characters get the clips fitted from it by bone name. Leave it |
| `AnimationPlayer` | the clips, by file: the placeholder's (`placeholder/clips/`) and yours |
| `AnimationTree` | the state machines: edit them as any Godot state machine |

**The default tree, as the starter has it**

| In the tree | What it is |
|---|---|
| `FullBody` | a state machine for the whole body: the locomotion |
| `UpperBody` | a state machine for the spine, arms and head, played over `FullBody` |
| `UpperBodyBlend` | the Blend2 that lays `UpperBody` over `FullBody`; its filter is which bones the upper body moves |

| Layer | State | Plays | Leaves when |
|---|---|---|---|
| `FullBody` | `Move` | idle, walk, jog, sprint, blended by forward speed | `jumped`; or in the air for 0.12 s |
| | `JumpStart` | the push off | landed; or after 0.3 s |
| | `InAir` | the fall loop | `grounded` |
| | `Land` | the landing | after 0.35 s, or moving faster than 1.5 m/s; or `jumped` |
| `UpperBody` | `Hold` | the placeholder's idle: put your clip here | never (add a state for a use) |

**Which layers a mod replaces** is the root's `replaces` list:

| `replaces` | The mod overrides | The rest of the tree |
|---|---|---|
| `UpperBody` (the starter) | only the upper body | is there to see your upper body over a moving character; not baked |
| `FullBody` | only the locomotion | not baked |
| both, or empty | both | |

**Rules that are not Godot's**

| Rule | Why |
|---|---|
| Transitions: Advance Mode **Auto**, with a condition or expression | the simulation runs the machine; nothing calls `travel()` |
| Conditions are the game's names: `forward_speed`, `speed`, `grounded`, `jumped`, `airborne_time`, `state_time`, a stance (`rifle`), a server mod's event (`rifle.fired`) | they are computed on the server and on every client |
| A blend space's position comes from **Graph Inputs** on the root (`FullBody/Move/blend_position = forward_speed`) | the same |
| The layer names are fixed: `FullBody`, `UpperBody` | a pack's layer replaces the character's layer of the same name |
| `UpperBodyBlend`'s amount is yours in the editor (1 to see the upper body); in the game the layer plays in full while the pack is swapped in | the server decides when |

**Your own clips**

1. Put the file (an FBX or glb with the animation) in `animation_packs/source/`.
2. Select it, open **Advanced Import Settings**:
   - on the `Skeleton3D` node, under Retarget: Bone Map, **New BoneMap**, and in it Profile, **New
     SkeletonProfileHumanoid**. Godot fills the map by the rig's bone names; check that the bones
     in the picture are green. Set Skeleton Name to `Skeleton3D`;
   - on the animation: its loop mode, and **Save to File** (a `.res` next to it, named as the clip
     should be called).
3. In the pack scene, select `AnimationPlayer`, and in the Animation panel: Animation, Manage
   Animations, the folder icon on the library: load the `.res`.
4. Use the clip in the tree, save the scene.

| If | Then |
|---|---|
| no bone map, or one made for another rig | the clip keeps its rig's bone names and **does nothing** on the skeleton, in the editor and in the game |
| it plays on `Skeleton3D` in the editor | it plays the same way in the game: characters get it fitted from this skeleton |
| you replace or delete a source file | the pack keeps working with the `.res` it has; the skeleton is the SDK's, never one of your files |
| you edit a clip in the AnimationPlayer | the `.res` is changed, and the next import of its source file overwrites it |

- **What the retarget does**: it renames the rig's bones to Godot's humanoid names (`Hips`, `Spine`,
  ...), turns every bone's rest to the profile's (Overwrite Axis) and rewrites the tracks to match,
  and drops what does not carry over (unmapped bones, positions other than the hips'). After it,
  one rotation means one movement on every humanoid skeleton, which is why a clip made on another
  rig plays on this one.

**The server's half** is two lines: `declare.AnimPack( "<mod>.animations" )`, and either
`declare.ItemLayers( kind, pack )` (plays while the item is held) or `ctx.SwapLayer`.

## Try things without a game

- **Cue Preview** (bottom panel): plays the open scene's reactions on stand-in players. Fire a cue
  by name, set any state name, pick the local player.
- **AnimationTree**: set a pack's tree Active in the editor to watch its states on the model.

## A new mod, both halves

```
powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -New mymod
```

| It makes | What |
|---|---|
| `server_mods/mymod/client/` | the look's project: an item (`prefabs/mymod.tscn`), reactions, a HUD, an animation pack, a motion |
| `server_mods/mymod/mymod.cpp` | the rules, in C++: it declares exactly the names those scenes use (`mymod.item`, `mymod.used`, `mymod_use`, `mymod.charges`, `mymod.moved`, `mymod.moves`, `mymod.animations`), gives every life the item in a fifth slot, and gives the motion's charges back |

| Then | Why |
|---|---|
| build the server (`cmake --build --preset clang-release`) | it finds the new mod, and writes its names for the editor |
| `tools\publish_mod.ps1 -Mod mymod` | bakes and packs the look; a name nobody declares stops it |
| build once more, run `cb_server` and the game | the server learns the item's id; the mod is in the game: slot 5, and Q |

- **The names are known to the editor** after the first build ([Names](../docs/looks.md#names)): properties that hold one name offer them, and a node warns about one nobody declares.
- Files that are already there are kept: `-New` on an existing mod only adds what is missing.

## Not here yet

| Missing | Until then |
|---|---|
| A preview of the item in a character's hand, with the grips solved and a pack playing | publish and look in the game |
| A prebuilt SDK download: the extension has to be built from source | build with the `godot-export` preset |
| Reloading a published look without restarting the server and the game | restart both |
| Maps: the Bake Map button is in the game's project (`godot/`), not here | make maps there |
