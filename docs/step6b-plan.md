# Step 6b: cooked items, planned

Written 2026-11-26 (after step 6, M292; it comes after [step 6.5](step6.5-plan.md), M293 to M297, so its
milestones are M298 to M305), for the owner and for whoever builds it. Nothing here is
built yet; [ROADMAP.md step 6b](../ROADMAP.md#6b-cooked-items) is the decision (2026-11-17) and this
is the plan behind it. Where this file and the code disagree about what exists today, the code wins.

**In one paragraph.** A mod's look reaches players today as a Godot resource pack: binary scenes,
imported sounds, `.remap` files, loaded with `load_resource_pack` and Godot's resource loader. A
pack can run code before any check of ours sees it (M261.9), and every item a player sees is a
tree of nodes (a bat is 15). Step 6b replaces the pack with a **cooked item**: data our own small
reader checks (meshes, GPU-ready textures, sounds, ozz clips, sheets, and the scene's parts as kinds
from a fixed list with named fields), drawn through the RenderingServer with no node per entity.
Godot's editor stays the authoring tool; the cooker runs there, when publishing. The base game
stays a Godot pack (it is ours), but draws its own looks the same way. When the last milestone is
done, no stranger's bytes go through a Godot loader, and nodes no longer grow with players.

## Contents

1. [What does not change](#1-what-does-not-change)
2. [Where it stands](#2-where-it-stands)
3. [Goals and non-goals](#3-goals-and-non-goals)
4. [The item format](#4-the-item-format)
5. [The reader](#5-the-reader)
6. [The cooker](#6-the-cooker)
7. [Drawing a cooked look](#7-drawing-a-cooked-look)
8. [Effects: particles, lights, sounds, tracks](#8-effects-particles-lights-sounds-tracks)
9. [Reactions on cooked parts](#9-reactions-on-cooked-parts)
10. [HUDs](#10-huds)
11. [Delivery and the store](#11-delivery-and-the-store)
12. [The map](#12-the-map)
13. [Moving over](#13-moving-over)
14. [Milestones, in order](#14-milestones-in-order)
15. [Decisions for the owner](#15-decisions-for-the-owner)
16. [Risks](#16-risks)

---

## 1. What does not change

- **Authoring is Godot's editor**: a modder still makes a `CbItem` scene with meshes, materials,
  particles, a sheet, a HUD scene, and presses Publish. What a scene may contain does not shrink
  without a reason the editor says (the bake already warns what it cannot draw).
- **The simulation's data is as it is**: `items/<kind>.cfg`, `motions/<owner>.cfg` (since step 6.5's
  M294 the parts of the scene a sheet's motions name, not motions: the server compiles the sheets), the ozz
  skeletons and clips, a character's graph. They move into the cooked item unchanged; the server
  reads them as it does today (it already never uses Godot for them).
- **Sheets are text** (step 6, D4): the item carries the source; every reader compiles it.
- **Items are named by SHA-256** and a server lists the ones it needs, as today.
- **Not Steam**; the store is ours (2026-11-17).

## 2. Where it stands

**What an item is today** (published zips, 2026-11-26):

| Mod | Files | Bytes | In it |
|---|---|---|---|
| gun (with the local rifle) | 73 | 210 KB | item and motion cfgs, a pack's ozz, imported `.sample` sounds and their `.import`, exported `.scn` scenes with `.remap`, sheets |
| brawl | 58 | 141 KB | six skills' scenes, five HUD scenes, sounds, sheets |
| sword | 22 | 106 KB | the blade, its pack, its reactions scene, four sounds |
| melee | 17 | 83 KB | the bat, its pack, its reactions scene, two sounds |

**What the mods' scenes use** (every `.tscn` under `server_mods/*/client` and `characters/*/client`):

| Kind | Count | | Kind | Count |
|---|---|---|---|---|
| `MeshInstance3D` | 57 | | `GPUParticles3D` (+ `ParticleProcessMaterial`) | 13 |
| `StandardMaterial3D` | 62 | | `AnimationPlayer` / `AnimationLibrary` / `Animation` | 11 / 11 / 28 |
| `BoxMesh` / `QuadMesh` / `SphereMesh` / `CylinderMesh` / `TorusMesh` | 47 / 13 / 4 / 4 / 1 | | `OmniLight3D` | 2 |
| `Node3D` / `Marker3D` / `BoneAttachment3D` | 24 / 8 / 13 | | `Gradient` / `GradientTexture1D` / `Curve` / `CurveTexture` | 3 / 3 / 2 / 1 |
| `CollisionShape3D` / `Area3D` (+ shapes) | 19 / 2 | | HUD: `CanvasLayer`, `Label`, `PanelContainer`, `VBox`/`HBox`, `ProgressBar`, `StyleBoxFlat`, `LabelSettings` | 18, 18, 9, 13, 2, 13, 32 |
| `CbItem`, `CbRules` (its sheet's path), `CbAnimPack` | 16, 36, 9 | | HUD: `CbFieldLabel`, `CbFieldBinding`, `CbList`, `CbKey`, `CbClick`, `CbDrag`, `CbItemIcon`, `CbPromptLabel`, `CbTextLog`, `CbTextEntry` | 53, 18, 9, 5, 3, 3, 3, 2, 1, 1 |
| animation trees (`AnimationTree`, state machines, blend trees) | 10 + | | `Skeleton3D` (+ `CinderboxSkeleton`, `CbCharacter`) | 12 (+1, +1) |

No mod uses a texture image yet (colours, emission and gradients only), no shader, no 3D model
file but the critter character's. Sounds are short WAV files, imported as `.sample`.

**What reads a scene in the game today**: `ItemLook::FromScene` and `CharacterLook::FromScene`
(`src/godot/look.h`), from a scene the pack's loader built; `CbDirector` instantiates an item's
scene when it has reactions (and the viewer's own items, and items lying in the world); HUD scenes
are instantiated whole; `boot.gd` / `game.gd` check a pack (`check_mod`, hardened in M262) and
`load_resource_pack` it.

**Measured** (M261.9): a bat is 15 nodes; at 255 fighting players bats are half of the scene's
7,900 nodes; at 1023, making and freeing what comes into sight is 68 ms of a 184 ms frame.

## 3. Goals and non-goals

| Goal | Done when |
|---|---|
| No stranger's bytes reach a Godot loader | `load_resource_pack` and `ResourceLoader` never see a mod item; `check_mod` is deleted |
| The reader is the boundary | every check of [§5](#5-the-reader) holds against a fuzzer in CI; a hand-made item can only be refused or drawn |
| Nodes never grow with players | the crowd bench's node count is flat from 16 to 1023 players (`tools\crowd_client.ps1`) |
| One drawing path | the editor's previews, the base game's own looks and mods' looks are drawn by the same code from the same cooked data |
| Nothing a player sees changes by accident | each shipped item, cooked, is what its scene was: the same meshes' arrays, materials' fields, parts' places and sounds' samples, compared per item in the Godot checks (they run headless: no screenshots there; a screenshot compare in a playtest) |

**Not in 6b**: modders' own shaders (refused, decided 2026-11-21: materials come from shaders we
ship, later); 3D model formats on a player's machine (Godot imports on the modder's); a mod's own
map (maps are the base game's today, [§12](#12-the-map)); the online store's hosting
([§11](#11-delivery-and-the-store), a decision); removing GDScript from the shipped game
(postponed, step 0).

## 4. The item format

One file per item, `ITEM.cbitem`, named by its SHA-256 as today's zip is:

```
"CBIT" u32 format_version
manifest (a small table: see below)
chunk*  (kind u32, size u64, hash 32 bytes, bytes): each chunk's size and hash are in the manifest first
```

| Manifest field | |
|---|---|
| format version, the engine's version it was cooked for, the mod's name | Hytale proposal 7's "what an item was made for" |
| the names it reads (fields, events, actions, statuses, item kinds) | what the editor's names check saw: a server can say early what it lacks |
| each chunk: kind, name, size, SHA-256 | sizes before any byte is read; a chunk whose hash differs is refused |
| budgets used (triangles, vertices, texture bytes, sound seconds, parts) | the store refuses past its budgets before anyone downloads |

| Chunk kind | Holds | Today |
|---|---|---|
| `simcfg` | `items/<kind>.cfg`, `motions/<owner>.cfg` (its sheets' parts, M294), a pack's `graph.cfg` / `anim.cfg`, a character's files | the same files in the zip |
| `ozz` | a skeleton or a clip, ozz's own archive | the same |
| `sheet` | a sheet's text, by its `res://` path (a `uses` is found by it) | the same |
| `mesh` | vertex arrays (positions, normals, tangents, UVs, colours, bones, weights) and indices, per surface; an AABB | `Mesh` resources inside `.scn` |
| `material` | a fixed field list of `StandardMaterial3D` ([§7](#7-drawing-a-cooked-look)), texture references by chunk | inside `.scn` |
| `texture` | GPU-ready blocks (E3), mips, its size | none yet (gradients, curves: small arrays, in the part that uses them) |
| `sound` | PCM or Ogg Vorbis (E2) | `.sample` (Godot's) |
| `look` | a scene's parts: a tree of kinds from a fixed list, each with named fields and a transform ([§6](#6-the-cooker)) | the `.scn` itself |
| `hud` | a HUD scene's controls, as a fixed list ([§10](#10-huds)) | a `.scn` |
| `tracks` | an animation's tracks on parts' fields ([§8](#8-effects-particles-lights-sounds-tracks)) | `Animation` inside `.scn` |

Why one file, not loose files by hash: one download, one hash for the server to name, one place
for the caps; the store may still serve a chunk alone later (E1).

## 5. The reader

`src/item/` (plain C++, no Godot types, like `src/expr` and `src/sheet`): opens a `.cbitem` from
bytes, checks everything before giving anything out, and hands the game plain structs. The game
turns them into RenderingServer resources; nothing else parses an item.

| It checks | |
|---|---|
| sizes first | the manifest's sizes against the file's; each against a cap (a chunk, an item, a count of each kind) before allocating |
| hashes | each chunk's SHA-256 against the manifest |
| meshes | vertex and index counts capped; every index below the vertex count; every float finite; arrays of the lengths the counts say; bone indices below the skeleton's count |
| textures | a block format from the list, a size whose bytes match it and its mips, a cap on width and height |
| sounds | PCM: a header from the list and a length; Ogg Vorbis: one decoder (stb_vorbis), fuzzed with the reader, decoded to PCM before Godot sees it |
| ozz archives | ozz's own reader, fuzzed (it has a version tag and sizes) |
| sheets | step 6's compiler, as text |
| parts | each a kind from the fixed list; each field a name the kind has, of its type, in its range; a parent before its child; references (a mesh, a material) to chunks that exist |
| HUDs | each control a kind from the fixed list, the same way |
| everything | a total cap per item; refused whole on the first problem, with why |

**Fuzzed**: a fuzzer target (libFuzzer, clang, Linux CI) over the whole reader with a corpus of
every shipped item cooked, plus a short deterministic mutation run in `cb_tests` on every platform
(E8). A crash or a hang is a red CI.

## 6. The cooker

In the extension (`src/godot/cook/`), run by Publish (`publish_mod.ps1` / the editor's button) on
the modder's machine, after Godot has imported everything. It replaces today's pack export, the
autobake's scene reading, and the scene reading in `ItemLook` / `CharacterLook`. Its output goes
through the reader before it is written, so the cooker cannot make what a game would refuse.

| A scene's node | Cooked as | Its fields |
|---|---|---|
| `MeshInstance3D` (any `Mesh`: primitives become arrays here) | `mesh` part | mesh, materials per surface, override, overlay, cast shadows, layers, visible |
| `Node3D`, `Marker3D` | `node` part (a place, a name for paths) | transform, visible |
| `BoneAttachment3D` | `bone` part | bone name |
| `GPUParticles3D` + `ParticleProcessMaterial` | `emitter` part | the process material's fields from a fixed list (direction, spread, velocities, gravity, scale and colour over life as small arrays, emission shape), amount, lifetime, one shot, explosiveness, draw pass mesh and material |
| `OmniLight3D`, `SpotLight3D` | `light` part | colour, energy, range, attenuation, angle, shadow |
| `AudioStreamPlayer3D` | `sound` part | sound, volume, range, autoplay |
| `AnimationPlayer` | `tracks` | value tracks on parts' fields from the fixed list, method tracks of the fixed methods (an emitter's `restart`, a part's `set_visible`), audio tracks |
| `CollisionShape3D`, `Area3D`, `CbHitbox` | simulation data, as today's bake | |
| `CbRules` | `sheet` (a path: the sheet's text is cooked by it) | |
| `CbItem`, `CbAnimPack`, `CbCharacter`, skeletons | as today's bakes, and a character's skinned meshes and sockets as `CharacterLook` takes them | |
| anything else | **not cooked**: the editor says so on the node (a configuration warning) and at Publish, as the bake says today what it cannot draw | |

## 7. Drawing a cooked look

The game makes RenderingServer resources from the reader's structs once per item (meshes, Godot
`StandardMaterial3D`s built by our code from the field list: Godot's own shader, ours), and draws
each entity's look as instances: `DrawnItem` and `DrawnBody` (M260) take cooked looks instead of
scene readers, and **every** item is drawn so: in another's hand, in the viewer's own (first
person too), holstered, lying in the world, in the editor's previews. `CbDirector` no longer
instantiates an item's scene.

| Material fields (first) | albedo colour and texture, metallic, roughness, emission (colour, energy, texture), normal map, transparency (alpha, scissor, depth prepass), unshaded, cull mode, billboard, vertex colour as albedo |
|---|---|

## 8. Effects: particles, lights, sounds, tracks

- **Emitters**: a RenderingServer particles instance with a `ParticleProcessMaterial` our code
  builds from the cooked fields (Godot's own shader); gradients and curves as small textures our
  code makes from cooked arrays.
- **Lights**: RenderingServer lights, shadows only near (as bodies' are).
- **Sounds**: a fixed pool of voices (say 32 3D players, nearest and loudest kept); a sound is
  played from PCM our reader gave (`AudioStreamWAV` from bytes we checked).
- **Tracks**: our own small player of value tracks on parts' fields and the fixed methods, sampled
  per frame for the entities in view (what `CbTrackPlayer` does for a character's effects today).
- **`create NAME`** (a reaction's effect, `vfx/NAME.tscn`): a cooked `look` of its own, drawn for
  its seconds like an item.

## 9. Reactions on cooked parts

A sheet's reaction names a node by path (`Model/Sparks`) and a property or method by Godot's name
(`surface_material_override/0:emission_energy_multiplier`, `restart`). Cooked, paths resolve to
part indices when the item loads, and properties and methods to the fixed fields:

| Today (Godot) | Cooked |
|---|---|
| `set PATH PROPERTY VALUE` on any node's any property | a field of the part's kind (`visible`, a material's `emission_energy`, `albedo_color`, a light's `energy`, an emitter's `emitting`, ...); another property is a mistake the cooker and the editor say |
| `call PATH METHOD` | the fixed methods (`restart`, `set_visible`, `play`) |
| `play PATH ANIMATION` | the cooked tracks |
| `create`, `sound`, `shake`, `flash` | as in §8, and the screen's own |

The reactions then run in the presentation's flecs world (the mirror), per entity, from the
compiled sheet: hand-placed `CbReaction` nodes stay for authoring and the Cue Preview, which cook
on the fly. Today's `CbReaction`-per-action nodes under each item instance go.

## 10. HUDs

A HUD is a fixed set of controls per screen, not per player (a scoreboard's rows are only those in
view), so nodes are allowed there; what is refused is Godot building them from a stranger's bytes.
Cooked as a `hud` chunk: each control a kind from a fixed list (`CanvasLayer`, `Control`,
`Label`, `Panel`, `PanelContainer`, `MarginContainer`, `VBox`/`HBoxContainer`, `GridContainer`,
`ProgressBar`, `TextureRect`, and our HUD nodes) with its layout, theme overrides (`StyleBoxFlat`,
`LabelSettings` fields) and fields; our code builds the controls (E6).

## 11. Delivery and the store

- **The game** fetches what a server names (today: from a folder store, `workshop.gd`,
  `workshop_fetch.gd`): declared size checked before downloading, the hash after, cached by hash.
  Unchanged but for the file being a `.cbitem` and read by the reader, not `load_resource_pack`.
- **The store**: static files by SHA-256 over HTTPS; every version kept; uploads checked by the
  same reader and the budgets before anyone can download. Where it is hosted, and how a modder
  uploads (an account, a key), is E10. Until then: the folder store, and `cb_store` serving a folder
  over HTTP for tests and LAN play.
- **Compressed** (Zstd) once items grow (E9).

## 12. The map

Maps are the base game's today (`godot/maps`, trusted: in its pack) and no mod ships one, so a map
is not a stranger's bytes yet. Cooking the map (statics, lights, environment, lightmaps: the largest
part) is for when a mod can bring one: proposed as its own step after 6b (E11). What 6b keeps
open: the map's visual is a scene the base game loads; nothing in 6b may assume a map is a pack.

## 13. Moving over

Only official items exist (the alpha loads official items only), so the switch is milestone by
milestone, each moving one part of an item from the pack to the cooked file:

1. A `.cbitem` holds the simulation data and sheets, and (until the last milestone) the pack for
   what is not cooked yet: the server reads only the cooked chunks from the first milestone on.
2. Looks, then effects, then reactions, then HUDs leave the pack.
3. The last milestone removes the pack from the item, `load_resource_pack` for items and
   `check_mod`.

Every milestone republishes every mod, and its Godot checks compare each item, cooked, with its
scene as Godot reads it (meshes' arrays, materials' fields, parts' places, sounds' samples); a
playtest compares what is on the screen.

## 14. Milestones, in order

| # | Milestone | What | Done when |
|---|---|---|---|
| **M298** | **The format and the reader** | `src/item/`: the `.cbitem` container, the manifest, caps and hashes, mesh, material, part, sound (PCM) and ozz chunks; `cb_item` (dump, check); the fuzz target and a deterministic run in `cb_tests` | a hand-made item of each kind of mistake is refused with why; the fuzzer runs in CI |
| **M299** | **Cooked simulation data** | the cooker in the extension writes `.cbitem` with every mod's simulation chunks and sheets (and today's pack inside, for the rest); Publish writes it; the server and the tests read only cooked chunks (no zip folders); `client_item.cfg` names the `.cbitem` | every mod published as `.cbitem`; every test passes reading them |
| **M300** | **Item looks cooked** | meshes, materials, parts; `ItemLook` from cooked data; every item drawn through the RenderingServer, the viewer's own and those lying in the world too; the editor's item preview drawn the same way | a bat, held by 255 players, is no node; each shipped item, cooked, compares equal to its scene |
| **M301** | **Characters cooked** | a character item's skinned meshes, skeleton, sockets; `CharacterLook` from cooked data; the base game's characters through the same cooker (at export) | the critter and the mannequin drawn from cooked data; the character checks pass |
| **M302** | **Effects** | emitters, lights, sounds (the voice pool), tracks, `create` effects cooked; the reaction scenes (`vfx/`) cooked | the sword's sparks, the bat's glow, the guns' muzzle flashes and every shipped sound from cooked items |
| **M303** | **Reactions on parts** | reactions from the compiled sheet in the mirror, per entity, on part indices and fixed fields; `CbReaction` nodes only in the editor | an item with reactions is no node in the game; the Cue Preview runs the same code |
| **M304** | **HUDs cooked** | the `hud` chunk; our code builds the controls | every shipped HUD from cooked data |
| **M305** | **No packs** | the pack leaves the item; `load_resource_pack` and `check_mod` go for mod content; players' own restyles are cooked items too; the store check (`cb_store`, the reader and budgets on upload) | no Godot loader sees a mod's bytes; the crowd bench's node count is flat |

## 15. Decisions for the owner

| # | Question | Options | Leaning |
|---|---|---|---|
| E1 | The item's shape | one `.cbitem` file / loose files named by hash with a manifest | **decided (the owner, 2026-11-27): one file** (one download, one hash to name, one place for caps); the store may serve chunks alone later |
| E2 | Sounds | PCM (16-bit, as today's WAVs, bigger) / Ogg Vorbis (smaller, a decoder to fuzz: stb_vorbis) / both | **PCM first** (today's sounds are short; no decoder on the boundary); Ogg Vorbis when an item's sounds grow |
| E3 | Texture formats | desktop blocks (BC1, BC3, BC4, BC5, BC7) / those and mobile (ETC2, ASTC) | **desktop only** (no mobile build planned); no mod has a texture yet |
| E4 | Materials | a `StandardMaterial3D` field list now, our own shipped shaders later / our shaders now | **the field list now** (Godot's own shader; the shipped-shaders plan of 2026-11-21 later) |
| E5 | Particles | `GPUParticles3D` cooked as emitter fields / our own particle system | **cooked fields** on Godot's particles (its shader is ours to trust) |
| E6 | HUDs | a fixed list of controls our code builds / Godot scenes behind today's guard until a later step | **the fixed list** (the controls the mods use are few, [§2](#2-where-it-stands)) |
| E7 | What an item's animation may move | parts' fixed fields and methods / any property | **the fixed fields**: what is not one is said by the editor |
| E8 | Fuzzing | libFuzzer job in CI and a deterministic run in `cb_tests` / only the deterministic run | **decided (the owner, 2026-11-27): both** (the job on Linux clang, a few minutes; the run everywhere) |
| E9 | Compression | none now / Zstd now | **decided (the owner, 2026-11-27): none now** (items are 1.5 to 210 KB); Zstd with the store |
| E10 | The store's hosting and uploads | a CDN and an upload service now / the folder store and a LAN `cb_store` now, hosting with launch | **later**, with how players find servers (undecided, 2026-11-17) |
| E11 | The map | in 6b / its own step when a mod can bring a map | **its own step** ([§12](#12-the-map)) |
| E12 | Who builds | | Claude, as step 6 |

## 16. Risks

| Risk | Answer |
|---|---|
| A look changes when cooked (a material field missed) | the per-item compare in the Godot checks; a playtest after each looks milestone; the editor's preview draws the cooked look, so a modder sees what players will |
| The fixed lists are too small for a modder | the editor says what is not cooked on the node; a kind or a field is added in a milestone, with the reader's check |
| The reader has a hole | fuzzed in CI; small; no Godot types; sizes before bytes; the store and the game run the same one |
| The step is long (eight milestones) | M298 and M299 change nothing a player sees; M300 alone removes most nodes (held items); the owner can stop after M302 and keep packs for HUDs behind the guard |
| Godot's RenderingServer particles or lights behave differently from nodes | the same server draws both; the compare says |
