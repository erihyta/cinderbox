# Makes the SDK's placeholder model: the CC0 mannequin with its locomotion clips and nothing else.
#
#   python tools/make_sdk_placeholder.py
#
# Reads godot/characters/mannequin/source/UAL1_Standard.glb (Universal Animation Library, Standard,
# CC0) and writes sdk/placeholder/mannequin.glb with only the animations in KEEP; the data of the
# others is dropped from the file, not just their names. Run it again only to change the list.

import json
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "godot", "characters", "mannequin", "source", "UAL1_Standard.glb")
TARGET = os.path.join(ROOT, "sdk", "placeholder", "mannequin.glb")
KEEP = ["Idle_Loop", "Walk_Loop", "Jog_Fwd_Loop", "Sprint_Loop", "Jump_Start", "Jump_Loop", "Jump_Land"]


def main():
    data = open(SOURCE, "rb").read()
    magic, version, _ = struct.unpack("<4sII", data[:12])
    assert magic == b"glTF" and version == 2
    json_length, json_type = struct.unpack("<I4s", data[12:20])
    assert json_type == b"JSON"
    gltf = json.loads(data[20 : 20 + json_length])
    at = 20 + json_length
    bin_length, bin_type = struct.unpack("<I4s", data[at : at + 8])
    assert bin_type == b"BIN\x00" and len(gltf["buffers"]) == 1 and "images" not in gltf
    old = data[at + 8 : at + 8 + bin_length]

    animations = [a for a in gltf["animations"] if a["name"] in KEEP]
    assert len(animations) == len(KEEP), [a["name"] for a in animations]
    gltf["animations"] = animations

    # The accessors still in use: meshes, skins, the kept animations.
    used = set()
    for mesh in gltf.get("meshes", []):
        for primitive in mesh["primitives"]:
            used.update(primitive["attributes"].values())
            if "indices" in primitive:
                used.add(primitive["indices"])
            for target in primitive.get("targets", []):
                used.update(target.values())
    for skin in gltf.get("skins", []):
        if "inverseBindMatrices" in skin:
            used.add(skin["inverseBindMatrices"])
    for animation in animations:
        for sampler in animation["samplers"]:
            used.add(sampler["input"])
            used.add(sampler["output"])

    # Their data, packed again; everything is renumbered.
    accessor_at = {}
    view_at = {}
    accessors = []
    views = []
    packed = bytearray()
    for index in sorted(used):
        accessor = dict(gltf["accessors"][index])
        assert "sparse" not in accessor
        if "bufferView" in accessor:
            v = accessor["bufferView"]
            if v not in view_at:
                view = dict(gltf["bufferViews"][v])
                start = view.get("byteOffset", 0)
                while len(packed) % 4:
                    packed.append(0)
                view["byteOffset"] = len(packed)
                packed += old[start : start + view["byteLength"]]
                view_at[v] = len(views)
                views.append(view)
            accessor["bufferView"] = view_at[v]
        accessor_at[index] = len(accessors)
        accessors.append(accessor)
    while len(packed) % 4:
        packed.append(0)

    for mesh in gltf.get("meshes", []):
        for primitive in mesh["primitives"]:
            primitive["attributes"] = {k: accessor_at[v] for k, v in primitive["attributes"].items()}
            if "indices" in primitive:
                primitive["indices"] = accessor_at[primitive["indices"]]
            if "targets" in primitive:
                primitive["targets"] = [{k: accessor_at[v] for k, v in t.items()} for t in primitive["targets"]]
    for skin in gltf.get("skins", []):
        if "inverseBindMatrices" in skin:
            skin["inverseBindMatrices"] = accessor_at[skin["inverseBindMatrices"]]
    for animation in animations:
        for sampler in animation["samplers"]:
            sampler["input"] = accessor_at[sampler["input"]]
            sampler["output"] = accessor_at[sampler["output"]]
    gltf["accessors"] = accessors
    gltf["bufferViews"] = views
    gltf["buffers"] = [{"byteLength": len(packed)}]

    text = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    total = 12 + 8 + len(text) + 8 + len(packed)
    os.makedirs(os.path.dirname(TARGET), exist_ok=True)
    with open(TARGET, "wb") as out:
        out.write(struct.pack("<4sII", b"glTF", 2, total))
        out.write(struct.pack("<I4s", len(text), b"JSON"))
        out.write(text)
        out.write(struct.pack("<I4s", len(packed), b"BIN\x00"))
        out.write(packed)
    print("%s: %d animations, %d of %d accessors, %.1f MB (from %.1f)" % (TARGET, len(animations), len(accessors), len(used) and len(json.loads(data[20 : 20 + json_length])["accessors"]), total / 1e6, len(data) / 1e6))


main()
