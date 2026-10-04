# Cinderbox SDK

The Godot project a mod's **look** is made in: its item scenes, effects, HUD, animations and state
machines. No game and no server are needed to author; the rules are the mod's C++ half
(`server_mods/<mod>/<mod>.cpp`).

## Start

| Step | Command |
|---|---|
| 1. Build the extension (once, and after engine changes) | `cmake --build --preset godot-export` |
| 2. Fill this project | `powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -Setup` |
| 3. Make a mod's project from it | `powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -New <mod>` |
| 4. Open `server_mods\<mod>\client` in Godot 4.7 and author | |
| 5. Publish | `powershell -ExecutionPolicy Bypass -File tools\publish_mod.ps1 -Mod <mod>` |
| 6. Restart the server and the game | |

`-Update <mod>` refreshes an existing project's SDK parts (after the extension is rebuilt).

## What is in an SDK project

| Path | What | Shipped? |
|---|---|---|
| `cinderbox.gdextension`, `bin/` | the Cinderbox extension: the `Cb*` nodes, the bakers, the **Cue Preview** panel | no |
| `addons/cinderbox_maps/` | the editor addon (the Bake Map button) | no |
| `characters/mannequin/` | the placeholder character: the CC0 mannequin, its clips and its state machine (locomotion, pistol, melee), at the path the game has it | no |
| `anim_src/` | the mod's animation pack scenes and their source files (models, clips) | no (their bake is) |
| `prefabs/`, `vfx/`, `ui/`, `assets/`, `maps/` | the mod's scenes, effects, HUD, sounds | yes |
| `items/<kind>.cfg` | item bodies, baked from the item scenes when publishing | yes |
| `anim/<pack>/` | animation packs, baked from `anim_src/` on save and when publishing | yes |

The "Mod" export preset ships every resource except the SDK's own, so new files need no listing.
Every mod's files are mounted on the same `res://`: name them after the mod (`vfx/<mod>_flash.tscn`).

## What `-New <mod>` makes

| File | Start from it to |
|---|---|
| `prefabs/<mod>.tscn` | model the item: its body (`CbItemBody`), where the hands hold it (`CbGrip`), a `Muzzle` for its effects |
| `vfx/reactions_<mod>.tscn` | say what the item looks like (`CbItemLook`), predict a press (`CbPrediction`), react to events (`CbReaction`) |
| `ui/hud_<mod>.tscn` | show fields while the item is out (`CbFieldLabel`) |
| `anim_src/<mod>_hold.tscn` | an animation pack for the **upper body** (`<mod>.hold`): a hold pose and a use |
| `anim_src/<mod>_walk.tscn` | an animation pack for the **base layer** (`<mod>.walk`): the mannequin's locomotion, to change |

Files that already exist are kept. Delete the starters a mod does not need.

## Animations and state machines

- **A pack is a `CbAnimPack` scene**: a model on a humanoid skeleton, its `AnimationPlayer`, and an
  `AnimationTree`. Edit the tree as any Godot state machine; transitions use Advance Mode Auto and a
  condition or expression made of the game's names (`forward_speed`, `grounded`, a stance, an event
  such as `<mod>.used`).
- **The layer's name decides what it replaces.** A tree whose root is a state machine is `Base`. A
  blend tree with one state machine named `Upper` on its output is `Upper`; with no bone filter of
  its own it plays through the character's.
- **Saving the scene bakes it** into `anim/<pack>/`. Publishing bakes again.
- **The server's half is two lines**: `declare.AnimPack( "<mod>.hold" )`, and either
  `declare.ItemLayers( kind, pack )` (plays while the item is held) or `ctx.SwapLayer`.
- **Your own clips**: import the model with Godot's humanoid retarget (Bone Map, Skeleton Name
  `Skeleton3D`, Overwrite Axis on), add them to the pack's `AnimationPlayer` in a new library, and
  use them in the tree as `<library>/<clip>`. Characters with other skeletons get them fitted by
  bone name.
- **The starters play the mannequin's own clips** (`Idle`, `Walk`, `Jog_Fwd`, `Sprint`, `Jump*`,
  `Pistol_Idle`, `Pistol_Shoot`, `Sword_*`).

## Try things without a game

- **Cue Preview** (bottom panel): plays the open scene's reactions on stand-in players. Fire a cue
  by name, set any state name, pick the local player.
- **AnimationTree**: set a pack's tree active in the editor to watch its states on the model.

## Not here yet

| Missing | Until then |
|---|---|
| The names a server mod declares (fields, events, actions, stances) are not known to the editor: nothing completes or checks them | type them as in the mod's `.cpp`; a wrong name is silent |
| A preview of the item in the character's hand, with the grips solved and a pack playing | publish and look in the game |
| A scaffold for the server half (`<mod>.cpp`) | copy `server_mods/rifle/rifle.cpp` |
| The game's pack check (no scripts, allowed folders and node types) at publish time | the game refuses a bad pack when it loads it |
| A prebuilt SDK download: the extension has to be built from source | build with the `godot-export` preset |
| Reloading a published look without restarting the server and the game | restart both |
