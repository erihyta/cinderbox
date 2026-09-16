# Player animations

The client looks for `anim.cfg` in this folder. If there isn't one, it uses the built-in
placeholder rig: a blocky skeleton with Mixamo joint names and procedural clips.

The game needs six clips:

| Clip | When it plays | Loops |
|---|---|---|
| `idle` | standing still | yes |
| `walk` | 1D blend: idle → walk at 3 m/s | yes, phase-synced with `run` |
| `run` | walk → run at 6.5 m/s (sprint) | yes, phase-synced with `walk` |
| `jump_start` | right after a jump | no |
| `fall` | airborne | yes |
| `land` | right after touching down | no |

## From Mixamo to the game

1. **Download from Mixamo.**
   - Use FBX Binary, and pick *Without Skin* (or with skin; the game only uses the skeleton).
   - Download one clip per file and name the files `idle`, `walk`, `run`, `jump_start`, `fall` and `land`.
   - Tick **In Place** for walk and run if Mixamo offers it. Otherwise `lock_root_xz` removes the forward drift.
   - Use the same character for every clip so the skeletons match.
2. **Convert each FBX to glTF in Blender** (4.x):
   - *File → Import → FBX*. Leave the defaults.
   - *File → Export → glTF 2.0*. Choose format *glTF Binary (.glb)* and save as `<clip>.glb` (for example `walk.glb`).
   - In the export options, under *Include*, select only the armature (*Limit to: Selected Objects*, with the armature selected). Under *Animation*, keep *Export animations* enabled.
   - Start a new scene (*File → New → General*, then delete the default cube) before importing the next clip, so each `.glb` holds exactly one clip.
3. **Convert to ozz** (this builds `skeleton.ozz`, the six `<clip>.ozz` files, and `anim.cfg`):
   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\convert_animations.ps1 -Source C:\path\to\glb\folder
   ```
   ```sh
   tools/convert_animations.sh /path/to/glb/folder
   ```
   The converter, `gltf2ozz`, is built together with the project and ends up in the build's `bin`
   folder. The skeleton comes from the first clip found.
4. **Preview:**
   ```sh
   cb_client --anim-viewer
   ```
   The viewer shows every clip plus a live idle → walk → run speed sweep. The arrow keys rotate
   and change speed, and Space pauses. Missing clips are labelled and show the rest pose.

## anim.cfg

```
skeleton = skeleton.ozz
idle = idle.ozz            # any clip line can be left out
walk = walk.ozz
run = run.ozz
jump_start = jump_start.ozz
fall = fall.ozz
land = land.ozz
scale = 0.01               # optional; rigs taller than 20 units are assumed to be in centimetres
lock_root_xz = true        # pin the root joint horizontally (clips with root motion)
```

Every clip must be built for the same skeleton; a clip with a different joint count is ignored.

## Matching foot speed

Walk and run share one cycle phase that the simulation advances. For feet not to slide, the cycle
lengths in `src/sim/anim_controller.h` must match your clip lengths:

```cpp
inline constexpr float kWalkCycleSeconds = 1.0f; // length of walk.ozz
inline constexpr float kRunCycleSeconds = 0.7f;  // length of run.ozz
```

These are simulation constants, so changing them changes the determinism reference hashes.
Regenerate the reference with `cb_tests --dump tests/reference_hashes.txt`. Also make sure one full
cycle in each clip contains two steps and that both clips start on the same foot.

## Where the files are loaded from

In this order:
1. `--assets DIR`
2. `./assets/anim`
3. `assets/anim` next to the executable
4. this folder in the source tree

`--procedural-anim` forces the placeholder rig.
