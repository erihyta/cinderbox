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
| `items/<kind>.cfg` | baked item bodies: written when publishing, never edited | yes |
| `placeholder/` | the placeholder model: the CC0 mannequin on a humanoid skeleton, with its locomotion clips | no |
| `cinderbox.gdextension`, `bin/` | the Cinderbox extension: the `Cb*` nodes, the bakers, the **Cue Preview** panel | no |
| `starters/` (in `sdk\` only) | what `-New` copies into a mod's project, with the mod's name in it | no |

- **Nothing needs listing.** The "Mod" export preset ships every resource except the folders
  marked "no".
- **Every mod's files share one `res://`** in the game: name files after the mod
  (`vfx/<mod>_flash.tscn`).
- **No scripts.** A mod's look is scenes and resources; the game refuses a pack with a script.

## What `-New <mod>` makes

| File | Start from it to |
|---|---|
| `prefabs/<mod>.tscn` | model the item: its body (`CbItemBody`), where the hands hold it (`CbGrip`), a `Muzzle` for its effects |
| `vfx/reactions_<mod>.tscn` | say what the item looks like (`CbItemLook`), predict a press (`CbPrediction`), react to events (`CbReaction`) |
| `ui/hud_<mod>.tscn` | show fields while the item is out (`CbFieldLabel`) |
| `animation_packs/<mod>_animations.tscn` | an animation pack: the default AnimationTree in full, replacing the upper body |

Files that already exist are kept. Delete the starters a mod does not need.

## Animation packs

A pack is an ordinary Godot scene: a model, an `AnimationPlayer` and an `AnimationTree`, under a
`CbAnimPack` root. Select the root to read what the pack does (its Editor Description).

| Node | What to do with it |
|---|---|
| `CbAnimPack` (root) | `character_name` is the pack's name: the server mod asks for it by that name. `replaces` lists the layers it ships |
| `Model` | the skeleton the clips were made on. Characters with other skeletons get the clips fitted by bone name |
| `AnimationPlayer` | the clips |
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

1. Put the model and clips in `animation_packs/source/`. Import with Godot's humanoid retarget:
   a Bone Map, Skeleton Name `Skeleton3D`, Overwrite Axis on.
2. In the pack scene, make `Model` an instance of your model, add your clips to an
   `AnimationPlayer` (Root Node: `Model`), and point the root's Animation Player and the tree's at it.
3. Use the clips in the tree, save. `server_mods/rifle/client/animation_packs/rifle_hold.tscn` is one
   made this way (it is local: the rifle's look is not in the repository).

**The server's half** is two lines: `declare.AnimPack( "<mod>.animations" )`, and either
`declare.ItemLayers( kind, pack )` (plays while the item is held) or `ctx.SwapLayer`.

## Try things without a game

- **Cue Preview** (bottom panel): plays the open scene's reactions on stand-in players. Fire a cue
  by name, set any state name, pick the local player.
- **AnimationTree**: set a pack's tree Active in the editor to watch its states on the model.

## Not here yet

| Missing | Until then |
|---|---|
| The names a server mod declares (fields, events, actions, stances) are not known to the editor: nothing completes or checks them | type them as in the mod's `.cpp`; a wrong name is silent |
| A preview of the item in a character's hand, with the grips solved and a pack playing | publish and look in the game |
| A scaffold for the server half (`<mod>.cpp`) | copy `server_mods/rifle/rifle.cpp` |
| The game's pack check (no scripts, allowed folders and node types) at publish time | the game refuses a bad pack when it loads it |
| A prebuilt SDK download: the extension has to be built from source | build with the `godot-export` preset |
| Reloading a published look without restarting the server and the game | restart both |
| Maps: the Bake Map button is in the game's project (`godot/`), not here | make maps there |
