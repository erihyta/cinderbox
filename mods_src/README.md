# Mod projects

Each folder here is a small Godot project that is packed into a cosmetic mod with
`tools\pack_mod.ps1 -Project mods_src\<name>`. The result is `mods\<name>.zip`.

A mod works by putting a file at the same `res://` path as a file of the game (`godot/`). When the
pack is loaded, the game uses the mod's file instead. New files can go in the same folders.

## Rules

The game checks each pack before it loads it. A pack is refused if:
- it contains a file outside `prefabs/`, `vfx/`, `ui/`, `maps/`, `assets/` or Godot's
  import folders;
- it contains a script (`.gd`, `.cs`), a native library, `.gdextension`, `.json`, or a nested pack;
- one of its resources references a script or a GDExtension.

The mod projects' own settings and caches (`project.binary`, `uid_cache.bin`) are removed by the pack
tool, so a mod never replaces the game's project settings.

Add every file you want to ship to the "Mod" export preset (`export_presets.cfg`, `export_files`).
Godot converts scenes and imported assets into its runtime formats when packing.

## Files the game looks up

| Path | Used for | Notes |
|---|---|---|
| `prefabs/player.tscn` | each player | The root sits at the feet and faces +Z. It may contain a `CinderboxSkeleton` (engine class, not a script): the skeleton draws bone boxes and/or drives a `Skeleton3D` given by `skeleton_path` using Mixamo bone names. With `use_slot_color`, it takes the player's slot color. |
| `prefabs/prop_box.tscn` | box props | Unit cube (1×1×1). The node is scaled to the prop's size. |
| `prefabs/prop_sphere.tscn` | sphere props | Unit-diameter sphere, scaled like the box. |
| `prefabs/static_box.tscn` | level geometry | Unit cube, scaled to each wall, ramp, step or platform. |
| `vfx/prop_spawn.tscn`, `vfx/prop_destroy.tscn` | prop spawned / removed | One-shot effects. Every `GPUParticles3D` in the scene is restarted; the node is freed after 3 s. |
| `vfx/jump.tscn`, `vfx/land.tscn` | a player jumps / lands | Placed at the player's feet. |
| `ui/hud.tscn` | HUD | Any `Control` tree. Optional labels with unique names `%Stats`, `%Banner` and `%Help` are filled by the game. |

Prop prefabs whose root has the metadata `tint_by_net_id = true` get a per-prop color from the game.
Leave it out to keep your own material.

Prefabs are purely visual. Collision, movement and timing come from the simulation, so physics
bodies in a prefab are not used for gameplay. Jolt bodies may still be used for debris and similar
effects that only exist on one client.

`maps/` is reserved. Levels are defined by the simulation today; a map format shared with the server
is future work.

## example_neon

This mod replaces the prop spawn effect with a magenta burst and gives box props a glowing cyan
material.
