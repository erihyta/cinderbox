"""Turns a scene's CbReaction and CbPrediction nodes into a rules sheet beside it (scene.rules) and one
CbRules node at its root. The scene is rewritten; look at both before you commit them.

    python tools/reactions_to_rules.py <project dir> <scene path inside it>

    python tools/reactions_to_rules.py server_mods/gun/client prefabs/pistol.tscn
"""
import io
import os
import posixpath
import re
import sys

project, scene = sys.argv[1], sys.argv[2]
path = os.path.join(project, scene)
raw = io.open(path, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in raw else "\n"
text = raw.replace("\r\n", "\n")

# Blocks: a header line and its lines, up to the next header.
parts = re.split(r"\n(?=\[)", text)
head_re = re.compile(r'^\[node name="([^"]+)" type="([^"]+)"(?: parent="([^"]*)")?')


def parse_value(v):
    v = v.strip()
    m = re.match(r'^NodePath\("(.*)"\)$', v)
    if m:
        return ("path", m.group(1))
    m = re.match(r'^PackedStringArray\((.*)\)$', v)
    if m:
        return ("list", [x.replace('\\"', '"') for x in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))])
    if v.startswith('"') and v.endswith('"'):
        return ("str", v[1:-1].replace('\\"', '"'))
    m = re.match(r'^Color\((.*)\)$', v)
    if m:
        return ("color", [float(x) for x in m.group(1).split(",")])
    m = re.match(r'^Vector3\((.*)\)$', v)
    if m:
        return ("vec", [float(x) for x in m.group(1).split(",")])
    if v in ("true", "false"):
        return ("bool", v == "true")
    return ("num", float(v))


def num(x):
    s = ("%.6f" % x).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


rules = []
kept = []
removed = 0
for part in parts:
    m = head_re.match(part)
    if not m or m.group(2) not in ("CbReaction", "CbPrediction"):
        kept.append(part)
        continue
    removed += 1
    name, cls, parent = m.group(1), m.group(2), m.group(3) or "."
    props = {}
    for line in part.split("\n")[1:]:
        if " = " in line and not line.startswith(";"):
            k, v = line.split(" = ", 1)
            props[k.strip()] = parse_value(v)
    me = name if parent == "." else parent + "/" + name

    def rel(p):
        # A path from the reaction, as a path from the scene's root (the sheet's node is under it).
        if p == "" or p[0] in "^$@":
            return p
        joined = posixpath.normpath(posixpath.join(me, p))
        if joined.startswith(".."):
            raise SystemExit("%s: %s climbs out of the scene" % (name, p))
        return "." if joined in (".", "") else joined

    def g(k, d=None):
        return props[k][1] if k in props else d

    def cond(lst):
        return " and ".join("(%s)" % c if " or " in c else c for c in lst)

    out = ["# " + name]
    if "editor_description" in props:
        out[0] += ": " + g("editor_description").replace("\n", " ")
    if cls == "CbPrediction":
        h = "predict %s as %s" % (g("action", ""), g("cue", ""))
        if g("conditions"):
            h += " if " + cond(g("conditions"))
        out.append(h)
        if "cooldown" in props:
            out.append("    cooldown " + num(g("cooldown")))
        if g("while_held"):
            out.append("    held")
        for c in g("changes", []):
            out.append("    change " + c)
        if g("stance"):
            out.append("    stance " + g("stance") + (" on " + g("stance_layer") if g("stance_layer") else ""))
        rules.append(out)
        continue
    when = int(g("when", 0))
    if when == 0:
        h = "on " + g("event", "")
        if g("conditions"):
            h += " if " + cond(g("conditions"))
    else:
        h = "while " + cond(g("conditions", []))
    out.append(h)
    if "subject" in props:
        out.append("    subject " + rel(g("subject")))
    if "event_side" in props:
        out.append("    side " + ["a", "b", "either"][int(g("event_side"))])
    if "subject_kind" in props:
        out.append("    kind " + g("subject_kind"))
    if "subject_template" in props:
        out.append("    template " + g("subject_template"))
    for k in ("delay", "chance", "cooldown"):
        if k in props:
            out.append("    %s %s" % (k, num(g(k))))
    if g("wait_for_server"):
        out.append("    wait")
    place = int(g("place", 0))
    at = ""
    if place == 1:
        at = "point"
    elif place == 2:
        at = "end"
    elif place == 3:
        at = "beam" + (" from " + rel(g("place_node")) if "place_node" in props else "")
    elif place == 4:
        at = rel(g("place_node", ""))
    elif place == 5:
        at = "follow"
    under = rel(g("scene_parent")) if "scene_parent" in props else ""
    # Under its parent: the parent, when that is not the scene's root (where the sheet's node is).
    if place == 0 and parent != ".":
        if "scene" in props and not under:
            under = parent
        if "sound" in props:
            at = parent
    if "offset" in props:
        at = (at or "") + " offset " + " ".join(num(x) for x in g("offset"))
    if at:
        out.append("    at " + at.strip())
    if "sound" in props:
        s = "    sound " + g("sound")
        for k, w in (("volume_db", "volume"), ("pitch_scale", "pitch"), ("pitch_jitter", "jitter"), ("max_distance", "range")):
            if k in props:
                s += " %s %s" % (w, num(g(k)))
        if "bus" in props:
            s += " bus " + g("bus")
        if "volume_expression" in props:
            s += " volume_by " + g("volume_expression")
        out.append(s)
    if "scene" in props:
        s = "    scene " + g("scene")
        if under:
            s += " under " + under
        if "scene_lifetime" in props:
            s += " life " + num(g("scene_lifetime"))
        out.append(s)
    if "animation" in props:
        s = "    play %s %s" % (rel(g("animation_player", "")), g("animation"))
        if "animation_off" in props:
            s += " off " + g("animation_off")
        out.append(s)
    if "property" in props:
        if "value_expression" in props:
            s = "    set %s %s = %s" % (rel(g("target", "")), g("property"), g("value_expression"))
        else:
            kind, v = props["value"]
            vt = (num(v) + (".0" if float(v).is_integer() else "")) if kind == "num" else ("true" if v else "false") if kind == "bool" else \
                "rgba(%s)" % ", ".join(num(x) for x in v) if kind == "color" else \
                "(%s)" % ", ".join(num(x) for x in v) if kind == "vec" else '"%s"' % v
            s = "    set %s %s %s" % (rel(g("target", "")), g("property"), vt)
            if "blend_time" in props:
                s += " blend " + num(g("blend_time"))
        out.append(s)
    if "method" in props:
        if "method_args" in props:
            raise SystemExit("%s: method_args not handled" % name)
        out.append("    call %s %s" % (rel(g("target", "")), g("method")))
    if "shake" in props:
        out.append("    shake " + num(g("shake")) + (" time " + num(g("shake_time")) if "shake_time" in props else ""))
    if "flash_color" in props:
        out.append("    flash rgba(%s)" % ", ".join(num(x) for x in g("flash_color")) + (" time " + num(g("flash_time")) if "flash_time" in props else ""))
    rules.append(out)

if removed == 0:
    raise SystemExit("nothing to convert in " + path)
stem = os.path.splitext(os.path.basename(scene))[0]
rules_rel = posixpath.join(posixpath.dirname(scene), stem + ".rules")
sheet = "# %s's reactions, as a rules sheet (CbRules: docs/looks.md, Rules sheets). Plain paths are\n# from the scene's root.\n\n" % os.path.basename(scene)
sheet += "\n\n".join("\n".join(r) for r in rules) + "\n"
io.open(os.path.join(project, rules_rel), "w", encoding="utf-8", newline="\n").write(sheet)
body = "\n".join(kept).rstrip("\n")
body += '\n\n[node name="Rules" type="CbRules" parent="."]\nfile = "res://%s"\n' % rules_rel
io.open(path, "w", encoding="utf-8", newline="").write(body.replace("\n", nl))
print("%s: %d nodes into %s" % (path, removed, rules_rel))
