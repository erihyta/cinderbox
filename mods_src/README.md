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
| `prefabs/<visual>.tscn` | entities from a map template | A template names the prefab it draws as. Unit-sized like the others: the client scales it to the shape the template authored. |
| `vfx/prop_spawn.tscn`, `vfx/prop_destroy.tscn` | prop spawned / removed | One-shot effects. Every `GPUParticles3D` in the scene is restarted; the node is freed after 3 s. |
| `vfx/jump.tscn`, `vfx/land.tscn` | a player jumps / lands | Placed at the player's feet. |
| `ui/hud.tscn` | HUD | Any `Control` tree. Optional labels with unique names `%Stats`, `%Banner` and `%Help` are filled by the game. |
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

## example_neon

This mod replaces the prop spawn effect with a magenta burst and gives box props a glowing cyan
material.
