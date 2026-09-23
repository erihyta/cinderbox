# Client mod projects

These are **client** mods: cosmetic packs for the Godot client that players install themselves. The
game's rules are server mods, compiled into `cb_server`, and each server mod's own look is a workshop
item (`server_mods/<mod>/client/`, see the README's Workshop items section). A client mod loads after
both, so it can restyle the base game and any item, but never change what the rules do.

Each folder here is a small Godot project that is packed into a cosmetic mod with
`tools\pack_mod.ps1 -Project mods_src\<name>`. The result is `mods\<name>.zip`.

A mod works by putting a file at the same `res://` path as a file of the game (`godot/`). When the
pack is loaded, the game uses the mod's file instead. New files can go in the same folders.

## Rules

The game checks each pack before it loads it, against an allowlist. A pack is refused if:
- it contains a file outside `prefabs/`, `vfx/`, `ui/`, `maps/`, `assets/` or Godot's converted
  copies (`.godot/exported`, `.godot/imported`), or a kind of file not on the list (scenes,
  resources, textures, samples, audio, fonts);
- a `.remap` or `.import` points anywhere but the pack's own converted files;
- a resource is compressed (its contents could not be checked);
- a resource names a script type (`GDScript`, `Script`, `GDExtension`, ...) or a script file (`.gd`, `.cs`).

The mod projects' own settings and caches (`project.binary`, `uid_cache.bin`) are removed by the pack
tool, so a mod never replaces the game's project settings.

Add every file you want to ship to the "Mod" export preset (`export_presets.cfg`, `export_files`).
Godot converts scenes and imported assets into its runtime formats when packing.

## Files the game looks up

| Path | Used for | Notes |
|---|---|---|
| `prefabs/player.tscn` | each player | The root sits at the feet and faces +Z. It may contain a `CinderboxSkeleton`, which poses it with ozz (bone boxes, or a `Skeleton3D` driven by Mixamo bone names, with `use_slot_color` for the player's colour), or a `CinderboxAnimator`, which drives an `AnimationTree` from the same state, or both. |
| `prefabs/prop_box.tscn` | box props | Unit cube (1×1×1). The node is scaled to the prop's size. |
| `prefabs/prop_sphere.tscn` | sphere props | Unit-diameter sphere, scaled like the box. |
| `prefabs/static_box.tscn` | level geometry | Unit cube, scaled to each wall, ramp, step or platform. |
| `prefabs/<visual>.tscn` | entities from a map template | A template names the prefab it draws as. Unit-sized like the others: the client scales it to the shape the template authored. |
| `prefabs/ragdoll.tscn` | ragdolls (optional) | Posed like a player; without it the player prefab is used. It needs a `CinderboxSkeleton` to be posed. |
| `vfx/bindings*.tres` | what plays on which event | A `CbEffectTable`: scenes, sounds, camera shake and screen flashes, plus state bindings (held items, aimed arms). Every file matching this name is loaded, so ship `vfx/bindings_<yourmod>.tres` and your effects are added to the game's instead of replacing them. Bindings can react to the server mods' events and actions by name and check their fields. |
| `vfx/bindings_pistol.tres`, `ui/hud_pistol.tscn`, `prefabs/pistol.tscn`, `vfx/muzzle_flash.tscn`, `vfx/tracer.tscn`, `vfx/bullet_spark.tscn`, `vfx/hit_puff.tscn` | the pistol's look | These come with the pistol's workshop item; a client mod can override any of them at the same path, because player mods load after items. The tracer is one metre long along -Z; beam bindings stretch it. |
| `assets/...` | sounds and other shared files | A binding can name any stream your mod ships, for example `res://assets/sfx/<yours>.wav`. |
| `vfx/prop_spawn.tscn`, `vfx/prop_destroy.tscn` | prop spawned / removed | One-shot effects used when no binding matches. Every `GPUParticles3D` in the scene is restarted; the node is freed after its lifetime. |
| `vfx/jump.tscn`, `vfx/land.tscn` | a player jumps / lands | Placed at the player's feet. |
| `ui/hud.tscn` | the game's HUD | Any `Control` tree. Optional nodes with unique names `%Stats`, `%Banner`, `%Help` and `%Name` (a LineEdit) are filled by the game. |
| `ui/hud_<mod>.tscn` | a mod's HUD | Laid over the game's HUD. Build it from ordinary controls plus `CbFieldLabel`, `CbFieldBinding`, `CbEventFeed` and `CbScoreboard`, which read the server mods' fields and events by name. |
| `maps/<name>.tscn` | the level's visuals | The scene the server's map was baked from, named after it. A mod can replace it to re-skin a level. |

Prop prefabs whose root has the metadata `tint_by_net_id = true` get a per-prop color from the game.
Leave it out to keep your own material.

Prefabs are purely visual. Collision, movement and timing come from the simulation, so physics
bodies in a prefab are not used for gameplay. Jolt bodies may still be used for debris and similar
effects that only exist on one client.

A map mod replaces only how a level looks. The collision comes from the baked map the server sends,
so moving a wall in a map mod moves the picture and not the wall: players still collide with the
server's geometry. Changing a level for real means baking a new map and running a server on it (see
the Maps section of the README).

## Authoring Cinderbox resources

Effect bindings (and any other Cinderbox type) need the game's extension present while the mod
project is open and while it is packed. `tools\pack_mod.ps1` copies `cinderbox.gdextension` and
`godot\bin` into the mod project for you and keeps them out of the pack; to edit them in the
editor, copy those two by hand into your mod project as well. They are developer files: a pack that
contained one would be refused.

## example_neon

This mod replaces the prop spawn effect with a magenta burst, gives box props a glowing cyan
material, and adds three effect bindings of its own (`vfx/bindings_neon.tres`): a landing puff and
blip only the local player gets, using a sound the mod ships itself; a burst when a `heavy_crate`
is destroyed; and a magenta screen flash when the local player spawns something.

## example_animtree

This mod replaces the player with one animated by Godot instead of ozz: an `AnimationPlayer` with
idle, walk, run, jump, fall and land clips, an `AnimationTree` whose state machine holds a 1D blend
space over ground speed, and a `CinderboxAnimator` that lets the simulation drive both. Like every
mod it contains no code.

Its prefab was generated by `godot/addons/cinderbox_maps/make_animtree_example.gd` so it stays in
step with the simulation's animation tuning, but it is an ordinary scene: open it and change it.
