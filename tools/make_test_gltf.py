"""Writes small glTF files that look like Blender exports of Mixamo clips.

Used to test tools/convert_animations.* end to end without real assets:
  - "Armature" node with scale 0.01, bones in centimetres (what Blender produces for Mixamo FBX)
  - a skin over mixamorig:* joints, no mesh
  - one animation named "mixamo.com" per file, with root motion on the hips

    python tools/make_test_gltf.py <output dir>
"""

import json
import math
import os
import struct
import sys

# name, parent, offset (cm)
JOINTS = [
    ("mixamorig:Hips", -1, (0, 95, 0)),
    ("mixamorig:Spine", 0, (0, 10, 0)),
    ("mixamorig:Spine1", 1, (0, 12, 0)),
    ("mixamorig:Spine2", 2, (0, 12, 0)),
    ("mixamorig:Neck", 3, (0, 14, 0)),
    ("mixamorig:Head", 4, (0, 8, 0)),
    ("mixamorig:HeadTop_End", 5, (0, 24, 0)),
    ("mixamorig:LeftArm", 3, (18, 10, 0)),
    ("mixamorig:LeftForeArm", 7, (0, -27, 0)),
    ("mixamorig:LeftHand", 8, (0, -25, 0)),
    ("mixamorig:RightArm", 3, (-18, 10, 0)),
    ("mixamorig:RightForeArm", 10, (0, -27, 0)),
    ("mixamorig:RightHand", 11, (0, -25, 0)),
    ("mixamorig:LeftUpLeg", 0, (10, -5, 0)),
    ("mixamorig:LeftLeg", 13, (0, -42, 0)),
    ("mixamorig:LeftFoot", 14, (0, -42, 0)),
    ("mixamorig:LeftToeBase", 15, (0, -6, 12)),
    ("mixamorig:RightUpLeg", 0, (-10, -5, 0)),
    ("mixamorig:RightLeg", 17, (0, -42, 0)),
    ("mixamorig:RightFoot", 18, (0, -42, 0)),
    ("mixamorig:RightToeBase", 19, (0, -6, 12)),
]

# clip: (duration, leg swing, arm swing, root speed cm/s)
CLIPS = {
    "idle": (2.0, 0.02, 0.02, 0.0),
    "walk": (1.0, 0.45, 0.35, 300.0),
    "run": (0.7, 0.8, 0.8, 650.0),
    "jump_start": (0.25, 0.3, 1.2, 0.0),
    "fall": (1.0, 0.2, 0.3, 0.0),
    "land": (0.3, 0.6, 0.4, 0.0),
}


def quat_x(angle):
    return (math.sin(angle / 2), 0.0, 0.0, math.cos(angle / 2))


def build(name, duration, leg, arm, root_speed):
    bin_data = bytearray()
    accessors = []
    buffer_views = []

    def add(values, comp_count, typ, minmax=False):
        flat = [v for tup in values for v in (tup if isinstance(tup, tuple) else (tup,))]
        offset = len(bin_data)
        bin_data.extend(struct.pack("<%df" % len(flat), *flat))
        while len(bin_data) % 4:
            bin_data.append(0)
        buffer_views.append({"buffer": 0, "byteOffset": offset, "byteLength": 4 * len(flat)})
        acc = {"bufferView": len(buffer_views) - 1, "componentType": 5126, "count": len(values), "type": typ}
        if minmax:
            acc["min"] = [min(values)]
            acc["max"] = [max(values)]
        accessors.append(acc)
        return len(accessors) - 1

    keys = 16
    times = [duration * i / (keys - 1) for i in range(keys)]
    t_acc = add(times, 1, "SCALAR", minmax=True)

    channels = []
    samplers = []

    def channel(node, path, values, typ):
        samplers.append({"input": t_acc, "output": add(values, 0, typ), "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})

    first_joint = 1  # node 0 is the armature
    swing = {"mixamorig:LeftUpLeg": -leg, "mixamorig:RightUpLeg": leg, "mixamorig:LeftArm": arm, "mixamorig:RightArm": -arm}
    for j, (jname, _, offset) in enumerate(JOINTS):
        node = first_joint + j
        if jname in swing:
            amp = swing[jname]
            channel(node, "rotation", [quat_x(amp * math.sin(2 * math.pi * t / duration)) for t in times], "VEC4")
        if jname == "mixamorig:Hips":
            # Root motion forward (+Z), which lock_root_xz must cancel.
            channel(node, "translation", [(0.0, 95.0, root_speed * t) for t in times], "VEC3")

    nodes = [{"name": "Armature", "children": [first_joint], "scale": [0.01, 0.01, 0.01]}]
    for j, (jname, parent, offset) in enumerate(JOINTS):
        children = [first_joint + k for k, (_, p, _) in enumerate(JOINTS) if p == j]
        node = {"name": jname, "translation": list(map(float, offset))}
        if children:
            node["children"] = children
        nodes.append(node)

    gltf = {
        "asset": {"version": "2.0", "generator": "cinderbox make_test_gltf.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": nodes,
        "skins": [{"joints": [first_joint + j for j in range(len(JOINTS))], "skeleton": first_joint}],
        "animations": [{"name": "mixamo.com", "channels": channels, "samplers": samplers}],
        "buffers": [{"byteLength": len(bin_data), "uri": name + ".bin"}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    return gltf, bin_data


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    for name, (duration, leg, arm, speed) in CLIPS.items():
        gltf, data = build(name, duration, leg, arm, speed)
        with open(os.path.join(out, name + ".gltf"), "w") as f:
            json.dump(gltf, f, indent=1)
        with open(os.path.join(out, name + ".bin"), "wb") as f:
            f.write(data)
    print("wrote %d test clips to %s" % (len(CLIPS), out))


if __name__ == "__main__":
    main()
