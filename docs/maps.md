# Maps and entities

Levels authored in the Godot editor and baked for the server. Part of the [manual](../README.md#the-manual).

## Maps

A map is a Godot scene with three marker nodes in it, baked into a `.cbmap` file the server loads.
The markers carry the collision the simulation needs; everything else in the scene is the look.

| Node | What it becomes |
|---|---|
| `CbStatic` | Solid level geometry: floor, wall, ramp, step, platform. A box of `size` (full size in metres), or the [shapes](#collision-shapes) under it. |
| `CbProp` | A dynamic body the level starts with: a box or sphere, or the [shapes](#collision-shapes) under it, drawn by its own meshes. A [body](#a-body) like any other: `mass`, `density`, `friction`, `bounce`. Solid to characters unless `pushed_by_characters` ([who pushes what](#who-pushes-what)) |
| `CbBreakable` | A [wall that breaks](#walls-that-break): static and whole until a mod breaks it, then the `CbProp` pieces under it. |
| `CbSpawn` | Where players appear. One per map. |
| `CbCharacterBody` | A character's body standing where it is, facing where it faces, nobody driving it (a player may take it over, a mod may drive it). `character`: a character item's name (`critter`): the server loads it from its item and players download it. Spawned when the game starts. |
| `CbTemplate` | A named entity built from components (see Entities below). |
| `CbComponent` | One component on a template, with the fields the simulation defines. |
| `CbEntity` | Places a template in the level, or defines one inline from its own components. |

1. Make a scene in `godot/maps/`, for example `arena.tscn` (copy `example_arena.tscn` to start).
   `test_ground.tscn` is the arena opened onto bumpy terrain with a building-sized block on it, for
   trying movement, slopes and the camera, and a ball pit (a 5 x 5 m pen of 181 balls pushed by
   characters) south-east of the spawn. `make_test_ground.gd` makes it anew from the arena, without
   what was added by hand since (the pushed balls, the critter, the ball pit).
2. Place `CbStatic` nodes for the collision (boxes, or [shapes](#collision-shapes) under them), and
   your own meshes, lights and effects for the look.
3. Press **Bake Map** in the 3D toolbar, or run `tools\bake_map.ps1 -Scene res://maps/arena.tscn`.
4. Run the server on it: `cb_server --map arena` (it finds `maps/arena.cbmap` next to the server,
   in the working folder, or in a checkout's `godot/maps`), or name the file: `--map path\arena.cbmap`.

The server sends the baked map to every client when it joins, so clients always play the server's
level and can never disagree about it. Clients then look for `res://maps/<name>.tscn` to draw it; if
they do not have that scene, they draw the baked collision boxes instead, which is what the
built-in sandbox does.

Baked values, including authored component fields, are rounded to fixed-point (1/1024 m,
1/4096 rad) so a map is identical on every platform, and the order of the nodes in the scene is
the order entities are created in, which is part of the map's identity. See `src/sim/map.h` for
the format and `src/sim/reflect.h` for the component registry.

## Walls that break

A `CbBreakable` is a wall (or anything static) and what it breaks into, authored beforehand.

| Under it | Is |
|---|---|
| `CollisionShape3D` nodes | the wall's shapes while it is whole |
| meshes (and other look nodes) | how it looks whole; the pieces' meshes are not part of it |
| `CbProp` nodes | its pieces: each a body of its own (its shapes or its box, its meshes, its [body](#a-body) settings), at most 256 |

- **Whole**, it is static: it costs nothing in rollback, and nothing that bumps into it breaks it.
- **Broken** (a mod calls `ctx.Break`; the frag grenade's blast does): a **hole**. The pieces near
  the blast fly away from it; the rest **stand** where they were, static (they cost nothing either),
  until another blast takes them or nothing holds them up any more. Everyone sees the same pieces.
- **What holds it up**: `held_at` on the `CbBreakable` (Bottom by default; Top, Sides: a wall
  between pillars, hung from a ceiling) says which pieces hold it: those along that edge of its
  shapes. A standing piece stays up while pieces that touch it, one to the next, join it to a held
  one; after every break, any that nothing joins falls. Blow out the foot of an arch and the arch
  comes down.
- **Which pieces touch** is found by the bake: two pieces whose faces are within 3 cm of each other
  (bricks laid with a gap, chunks the Fracture tool cut). An edge or a corner alone does not hold.
- **Glued** (`glue`, newtons; 0: the static wall above): the wall never stands whole. From the
  start its pieces are bodies of their own, welded where they touch, the held ones static. A weld
  pulled, pushed or twisted harder than its glue breaks; a piece left with no weld is rubble. So a
  glued wall or tower sags, cracks and falls apart by its own physics: blow out its foot and what is
  above comes down under its own weight; a heavy ball cracks the welds it hits. A blast still frees
  the pieces near it. Each piece is a body that sleeps when still, so a glued wall costs a little
  more than a static one: for towers, bridges, wooden things, not every wall of a map.
- **Pieces are rubble**: solid ([who pushes what](#who-pushes-what)): players stand on them and are
  stopped by them, but never shove them. Blasts, shots and other bodies move them.
- The bake makes the wall a static template placed where it stands, and each piece a template of its
  own; its pieces are not the map's props. `test_ground.tscn` has two, 12 m north of the spawn:
  `BrickWall` (twelve bricks placed by hand) and `ConcreteWall` (28 chunks cut by the tool below);
  `GreatWall` across the north end, 11 m ahead of the spawn (24 x 7 m, 150 chunks); and
  `GluedTower` at its west side (2.5 x 6 x 2.5 m, 40 chunks welded at 20000 N).

### Cutting a wall into chunks

Pieces can be placed by hand (bricks, planks), or cut: select a `CbBreakable` and press
**Fracture** in the 3D toolbar.

| Setting | What |
|---|---|
| Pieces | how many chunks (2 to 256) |
| Seed | another number, other chunks; the same number, the same chunks |
| Crowd | 0 to 1: the share of chunks crowded around a child `Node3D` named `FractureFocus` (else the wall's centre): small chunks there, big ones farther out |

- It cuts the wall's `BoxShape3D` and `ConvexPolygonShape3D` shapes (others: it says why not) into
  irregular convex chunks (Voronoi cells). Each is a `CbProp` with a hull and a mesh of its own.
- A thin shape is cut through: every chunk goes from one face to the other.
- Outside faces get the wall's own material (its first mesh's); the cut faces a darker copy of it.
  Textures run on across chunks (laid on from the side each face looks toward, a metre a repeat).
- Each chunk is cut against its 40 nearest, then any other whose side a corner of it turns out to
  be on: exact, and a wall of 150 chunks is cut in a tenth of a second.
- It replaces the pieces the wall had, taking their body settings (`density`, `friction`, ...);
  each chunk weighs by its size. Undo puts the old pieces back. The settings are kept on the node
  for next time.
- Scripts can cut too: `preload("res://addons/cinderbox_maps/fracture.gd").fracture(wall, pieces, seed, crowd)`
  (`make_test_ground.gd` does).


## Who pushes what

Characters read the physics world and never write it themselves (M246; the hybrid network model
needs it: [ROADMAP step 3](../ROADMAP.md#3-two-network-models)). A body is one of two kinds, the
same on every server:

| | Solid (the default) | `pushed_by_characters` (a ball, a can) |
|---|---|---|
| A character walking into it | is stopped, and can stand on it | walks through it; its **pusher** (a capsule where it stands, in the physics world) shoves it |
| What moves it | skills, shots, blasts, other bodies | the same, and pushers |
| A falling one | pushes the character out, by its share of the two weights (a pebble hardly, a crate of the player's weight by half) | passes through the character |

- Rubble (a broken wall's pieces) and props spawned by mods are solid; items lying about and
  ragdolls are pushed by characters.
- Walking does not slow a character against what it pushes: a pusher shoves at its own speed. Heavy
  things are better solid.
- A plain `CbProp` with the flag (no shapes, no meshes) is baked as a template, not into the map's
  simple prop list. `test_ground`'s balls have it (the two by the spawn, the pit's); its crates do not.

## Collision shapes

Every body node takes Godot's own `CollisionShape3D` nodes as direct children; Godot's handles, and
**Mesh > Create Collision Shape** on a model, make them as usual.

| Node | Its shapes are |
|---|---|
| `CbStatic` | each a piece of level geometry of its own, where it is (its `size` is then not used) |
| `CbProp` | one body of all of them, a [body](#a-body) as its settings say; it falls and tumbles as one |
| `CbTemplate`, and a `CbEntity` with its own components | one body of all of them, in place of its `Shape` component |

A body's weight (`mass`, or `Material.mass` on a template) is the whole body's, however many shapes
it has. A ray, a shot or a hook that meets any of its shapes meets the one entity.

**Its look**: a `CbProp`, `CbTemplate` or inline `CbEntity` with meshes (or sprites, lights,
particles) of its own under it is drawn by them: each entity made from it is a copy of that node,
moved with the body, as big as the node stands in the map; the node itself is not drawn where it was
placed. A `CbProp` with meshes and no shapes is its own box or ball, drawn by its meshes. A template
with a `visual` keeps that prefab; one with neither draws its shape's default prefab. A plain
`CbProp` (no children) is the map's simple box or ball, as before.

| Shape | Baked as |
|---|---|
| `BoxShape3D` | a box, turned any way (yaw, pitch and roll) |
| `SphereShape3D` | a sphere (scaled unevenly: its largest scale, with a warning) |
| `CapsuleShape3D` | a capsule along its Y (scaled unevenly across: its larger scale, with a warning) |
| `CylinderShape3D` | a hull: two rims of 16 points (any scale kept: an oval drum stays oval) |
| `ConvexPolygonShape3D` | a hull of its points (4 to 1024; Box3D keeps at most 128 corners) |
| `ConcavePolygonShape3D` | a triangle mesh (a cave, an arch, stairs; up to 262,144 vertices). **Level geometry only** (a `CbStatic`'s) |
| `HeightMapShape3D` | a height field (terrain; up to 2049 points a side), scaled; a point that is not a number is a hole in the cells around it. **Level geometry only** |
| anything else | skipped, with a warning |

- A disabled `CollisionShape3D` is skipped, and so is a hull whose points lie in a plane.
- Triangle meshes and height fields never move: Box3D collides a moving body with one only when
  it is static. Under a `CbProp`, a template or an item they are skipped (or the item's bake stops)
  with a reason. A model's **Mesh > Create Collision Shape**, type **Trimesh**, makes one.
- The third-person camera stops at the level as it is: a hillside, a cave wall, a hull's own faces
  (the game's own copy of the level's static shapes, built once per map). Props never stop it.
- From a model: select its `MeshInstance3D` (under the `CbStatic`), **Mesh > Create Collision
  Shape**, placement **Sibling**, type **Single Convex** (one hull) or **Simplified Convex**. A
  concave model (an arch) is several hulls: **Multiple Convex** makes them.
- A hull's points are stored on the map's 1/1024 m grid, scaled; hulls alike are stored once.
  Every machine builds the same hull from them (the fourth reference run checks it on every build).
- A map drawn without its scene shows a hull as its bounding box. Watching a view file (which has
  no level of its own) the camera stops at boxes, balls, capsules and hulls' bounds only.
- Godot's warning on a `CollisionShape3D` ("must be a child of a physics body") does not apply here
  and can be ignored: Cinderbox's physics reads it, Godot's never runs.
- The bake says what it made of each node in the Bake Map output.
- Items keep their own shape settings (`item.cfg`) for now.

Turns are stored as yaw, pitch and roll (Godot's YXZ order) on the 1/4096 rad grid. Maps baked
before this (version 3: boxes without roll; version 4: no hulls; version 5: one shape per body;
version 6: no triangle meshes or height fields; version 7: no rolling or drag; version 8: no
breakables; version 9: no holes, so any break brings a whole wall down; version 10: no glue) still
load. A placed entity turns with roll too.

## A body

What a body is is said one way wherever a scene describes one (`CbBody`, the base of `CbProp` and
`CbItem`; a `CbTemplate` says the same three in its Material component):

| What | Meaning |
|---|---|
| `mass` | kilograms. 0: its volume times `density` (an item starts at 1) |
| `density` | kilograms per cubic metre, used when no mass is given: at 40, a crate 1 m across is 40 kg |
| `friction` | how it grips what it touches: 0 is ice, 0.6 the usual |
| `bounce` | how much of a hit it gives back: 0 none, 1 all of it |
| `rolling` | rolling resistance (balls, capsules): a ball on the ground slows by about this times gravity. 0 rolls on until something stops it. A map's `CbProp` starts at 0.05; an item at 0 (its mod's to tune) |
| `drag` | how fast its speed and spin fade, per second, in the air too: a beach ball 1, a stone 0 |

- Every simulation builds the same body from them: weight decides what a player's push, a hook's
  pull and a rope share with it; friction and bounce are the physics engine's.
- A prop's are baked into the `.cbmap` (in thousandths), an item's into its `items/<kind>.cfg`
  (`mass`, and `friction` / `bounce` / `rolling` / `drag` when they are not the usual). A template
  says rolling in its `Material`, drag as its `Body`'s damping.

## Entities and components

An entity is described by attaching components to it in the inspector, the same components the
simulation uses. Add a `CbTemplate`, give it `CbComponent` children, pick a component in each one,
and its fields appear:

| Component | Fields |
|---|---|
| `Shape` | `kind` (Box, Sphere, Capsule), `size`, `radius`, `height` (or `CollisionShape3D` children instead) |
| `Body` | `type` (Static, Kinematic, Dynamic), `gravity_scale`, `linear_damping`, `angular_damping` |
| `Material` | `density`, `friction`, `restitution`, `mass` (kg; 0: its volume times density), `rolling` |
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
