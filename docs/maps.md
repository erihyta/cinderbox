# Maps and entities

Levels authored in the Godot editor and baked for the server. Part of the [manual](../README.md#the-manual).

## Maps

A map is a Godot scene with three marker nodes in it, baked into a `.cbmap` file the server loads.
The markers carry the collision the simulation needs; everything else in the scene is the look.

| Node | What it becomes |
|---|---|
| `CbStatic` | A solid box: floor, wall, ramp, step, platform. `size` is the full size in metres. |
| `CbProp` | A dynamic box or sphere the level starts with. A [body](#a-body) like any other: `mass`, `density`, `friction`, `bounce` |
| `CbSpawn` | Where players appear. One per map. |
| `CbTemplate` | A named entity built from components (see Entities below). |
| `CbComponent` | One component on a template, with the fields the simulation defines. |
| `CbEntity` | Places a template in the level, or defines one inline from its own components. |

1. Make a scene in `godot/maps/`, for example `arena.tscn` (copy `example_arena.tscn` to start).
2. Place `CbStatic` boxes for the collision, and your own meshes, lights and effects for the look.
3. Press **Bake Map** in the 3D toolbar, or run `tools\bake_map.ps1 -Scene res://maps/arena.tscn`.
4. Run the server on it: `cb_server --map godot\maps\arena.cbmap`.

The server sends the baked map to every client when it joins, so clients always play the server's
level and can never disagree about it. Clients then look for `res://maps/<name>.tscn` to draw it; if
they do not have that scene, they draw the baked collision boxes instead, which is what the
built-in sandbox does.

Baked values, including authored component fields, are rounded to fixed-point (1/1024 m,
1/4096 rad) so a map is identical on every platform, and the order of the nodes in the scene is
the order entities are created in, which is part of the map's identity. See `src/sim/map.h` for
the format and `src/sim/reflect.h` for the component registry.

## A body

What a body is is said one way wherever a scene describes one (`CbBody`, the base of `CbProp` and
`CbItem`; a `CbTemplate` says the same three in its Material component):

| What | Meaning |
|---|---|
| `mass` | kilograms. 0: its volume times `density` (an item starts at 1) |
| `density` | kilograms per cubic metre, used when no mass is given: at 40, a crate 1 m across is 40 kg |
| `friction` | how it grips what it touches: 0 is ice, 0.6 the usual |
| `bounce` | how much of a hit it gives back: 0 none, 1 all of it |

- Every simulation builds the same body from them: weight decides what a player's push, a hook's
  pull and a rope share with it; friction and bounce are the physics engine's.
- A prop's are baked into the `.cbmap` (in thousandths), an item's into its `items/<kind>.cfg`
  (`mass`, and `friction` / `bounce` when they are not the usual).

## Entities and components

An entity is described by attaching components to it in the inspector, the same components the
simulation uses. Add a `CbTemplate`, give it `CbComponent` children, pick a component in each one,
and its fields appear:

| Component | Fields |
|---|---|
| `Shape` | `kind` (Box, Sphere, Capsule), `size`, `radius`, `height` |
| `Body` | `type` (Static, Kinematic, Dynamic), `gravity_scale`, `linear_damping`, `angular_damping` |
| `Material` | `density`, `friction`, `restitution` |
| `Velocity` | `linear`, `angular` the entity starts with |
| `Prop` | `lifetime_seconds` (0 keeps it forever); makes it count against the prop caps |

Those fields come from the simulation's own registry (`src/sim/reflect.h`), so adding a field there
makes it appear in the editor with no Godot-side code. A component an author does not attach is
left at the engine's default, which is also how a map baked before a field existed still loads.

- `visual` on a template names the prefab clients draw for it: `res://prefabs/<visual>.tscn`.
  Without it, the shape's default prefab is used.
- `spawnable` marks the one template the spawn button (F) creates. Anything a player spawns still
  expires and counts against the prop caps, whatever the template says.
- A `CbEntity` with a `template_name` places that template. A `CbEntity` with its own
  `CbComponent` children defines a template just for itself, and identical ones are shared.
- A template with `Body.type = Static` becomes level geometry: it never falls and the kill plane
  ignores it.

Templates are part of the map, so they travel to clients with it and the server can spawn them at
runtime. They are initial values only: a template says what an entity starts as, never how it
behaves. Behaviour stays in the simulation.
