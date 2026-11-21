# Writes the slice's first arena, godot/maps/arena.tscn (ROADMAP step 4; M278): a city block for 8 to 10
# players, big enough that they feel small. Plain shapes and flat colours, as a Roblox battlegrounds
# map is. Its walls that break (M280) are scenes of their own, godot/maps/arena_parts/*.tscn, each
# a design the arena places many times, cut into chunks by the fracture tool. Then bake it:
#
#   python tools/make_arena.py
#   godot --headless --path godot --script res://addons/cinderbox_maps/fracture_scenes.gd -- res://maps/arena_parts
#   powershell -ExecutionPolicy Bypass -File tools\bake_map.ps1 -Scene res://maps/arena.tscn
#
# A first draft to start from: once the scene is edited in Godot, edit it there; running this again
# writes it over (and the wall designs, uncut: cut them again). Everything is in metres, the map's
# middle at the origin, the ground's top at y = 0.
#
#   the park       the middle, 40 m across: grass, paths, a fountain, trees, benches
#   the ring road  around it, 12 m wide, sidewalks either side; streets from it to the four edges
#   towers         two of one design (NW, SE): 18 x 18 m, seven storeys; columns at the corners,
#                  floors and a core inside, and between them walls with windows that break, a
#                  storey and a side each (three designs of chunks, so neighbours differ)
#   garages        two of another (NE, SW): three open floors and a roof on pillars, ramps up the
#                  outside; the parapets, the rails and the ramps' outer wall break
#   cover          brick walls that break in the plazas beside the streets
#   the edge       a skyline of tall blocks all round: the city goes on, nobody leaves
#   spawns         the map's own in the park; "team1" on the ring road's west side, "team2" on its east
#                  side (the owner moved them in from the streets' ends, 2026-11-21)
#
# What breaks them in the team deathmatch: a body the brawl finisher throws (brawl.toss_break).

import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "godot", "maps", "arena.tscn")
PARTS = os.path.join(ROOT, "godot", "maps", "arena_parts")

MATERIALS = {
    "Grass": (0.36, 0.62, 0.30),
    "Road": (0.22, 0.23, 0.25),
    "Line": (0.95, 0.82, 0.30),
    "Sidewalk": (0.70, 0.68, 0.64),
    "Plaza": (0.60, 0.58, 0.55),
    "Path": (0.80, 0.72, 0.55),
    "Tower": (0.82, 0.74, 0.62),
    "TowerTrim": (0.55, 0.42, 0.36),
    "Concrete": (0.64, 0.65, 0.68),
    "ConcreteDark": (0.46, 0.47, 0.50),
    "Brick": (0.70, 0.34, 0.25),
    "Trunk": (0.42, 0.28, 0.18),
    "Leaves": (0.30, 0.56, 0.26),
    "LeavesLight": (0.42, 0.66, 0.30),
    "Water": (0.30, 0.58, 0.80),
    "Stone": (0.75, 0.75, 0.78),
    "Metal": (0.20, 0.21, 0.24),
    "Bench": (0.55, 0.36, 0.20),
    "Skyline1": (0.62, 0.66, 0.74),
    "Skyline2": (0.70, 0.64, 0.60),
    "Skyline3": (0.56, 0.60, 0.58),
}
EMISSIVE = {"Lamp": (1.0, 0.90, 0.62)}

sub = []  # sub_resource blocks
nodes = []  # node blocks
meshes = {}  # (kind, dims, material) -> id
shapes = {}
names = {}
ext = []  # ext_resource blocks (the wall designs)
designs = {}  # design name -> ext id


def f(v):
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def vec(v):
    return "Vector3(%s, %s, %s)" % (f(v[0]), f(v[1]), f(v[2]))


def material_blocks():
    for name, c in MATERIALS.items():
        sub.append('[sub_resource type="StandardMaterial3D" id="Mat_%s"]\nalbedo_color = Color(%s, %s, %s, 1)\nroughness = 0.9\n' % (name, f(c[0]), f(c[1]), f(c[2])))
    for name, c in EMISSIVE.items():
        sub.append('[sub_resource type="StandardMaterial3D" id="Mat_%s"]\nalbedo_color = Color(%s, %s, %s, 1)\nemission_enabled = true\nemission = Color(%s, %s, %s, 1)\nemission_energy_multiplier = 2.0\n'
                   % (name, f(c[0]), f(c[1]), f(c[2]), f(c[0]), f(c[1]), f(c[2])))


def box_mesh(size, mat):
    key = ("box", tuple(round(x, 4) for x in size), mat)
    if key not in meshes:
        mid = "Box_%d" % len(meshes)
        meshes[key] = mid
        sub.append('[sub_resource type="BoxMesh" id="%s"]\nmaterial = SubResource("Mat_%s")\nsize = %s\n' % (mid, mat, vec(size)))
    return meshes[key]


def sphere_mesh(radius, mat):
    key = ("sphere", round(radius, 4), mat)
    if key not in meshes:
        mid = "Sphere_%d" % len(meshes)
        meshes[key] = mid
        sub.append('[sub_resource type="SphereMesh" id="%s"]\nmaterial = SubResource("Mat_%s")\nradius = %s\nheight = %s\nradial_segments = 12\nrings = 6\n'
                   % (mid, mat, f(radius), f(2 * radius)))
    return meshes[key]


def cylinder_mesh(radius, height, mat):
    key = ("cyl", round(radius, 4), round(height, 4), mat)
    if key not in meshes:
        mid = "Cyl_%d" % len(meshes)
        meshes[key] = mid
        sub.append('[sub_resource type="CylinderMesh" id="%s"]\nmaterial = SubResource("Mat_%s")\ntop_radius = %s\nbottom_radius = %s\nheight = %s\nradial_segments = 10\n'
                   % (mid, mat, f(radius), f(radius), f(height)))
    return meshes[key]


def box_shape(size):
    key = tuple(round(x, 4) for x in size)
    if key not in shapes:
        sid = "Shape_%d" % len(shapes)
        shapes[key] = sid
        sub.append('[sub_resource type="BoxShape3D" id="%s"]\nsize = %s\n' % (sid, vec(size)))
    return shapes[key]


def unique(parent, name):
    key = (parent, name)
    n = names.get(key, 0)
    names[key] = n + 1
    return name if n == 0 else "%s%d" % (name, n + 1)


def transform(pos, yaw=0.0, pitch=0.0):
    # Yaw about Y, then pitch about the node's own X: a ramp tilted up along its length.
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    # basis = Ry * Rx (columns: x, y, z axes)
    xa = (cy, 0.0, -sy)
    ya = (sy * sp, cp, cy * sp)
    za = (sy * cp, -sp, cy * cp)
    return "Transform3D(%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)" % (
        f(xa[0]), f(ya[0]), f(za[0]), f(xa[1]), f(ya[1]), f(za[1]), f(xa[2]), f(ya[2]), f(za[2]), f(pos[0]), f(pos[1]), f(pos[2]))


def node(name, kind, parent, props=None, pos=None, yaw=0.0, pitch=0.0):
    name = unique(parent, name)
    lines = ['[node name="%s" type="%s" parent="%s"]' % (name, kind, parent)]
    if pos is not None and (any(abs(p) > 1e-9 for p in pos) or yaw or pitch):
        lines.append("transform = " + transform(pos, yaw, pitch))
    for k, v in (props or {}).items():
        lines.append("%s = %s" % (k, v))
    nodes.append("\n".join(lines) + "\n")
    return name if parent == "." else parent + "/" + name


def group(name, parent=".", pos=(0, 0, 0), yaw=0.0):
    return node(name, "Node3D", parent, None, pos, yaw)


def solid(name, parent, center, size, mat, yaw=0.0, pitch=0.0):
    """A box that is part of the level: collision and look."""
    path = node(name, "CbStatic", parent, {"size": vec(size)}, center, yaw, pitch)
    node("Mesh", "MeshInstance3D", path, {"mesh": 'SubResource("%s")' % box_mesh(size, mat)})
    return path


def deco(name, parent, center, size, mat, yaw=0.0):
    """A box that is only seen: road paint, windows."""
    return node(name, "MeshInstance3D", parent, {"mesh": 'SubResource("%s")' % box_mesh(size, mat)}, center, yaw)


# --- walls that break -------------------------------------------------------------------------
#
# A design is a CbBreakable scene of its own (godot/maps/arena_parts/<name>.tscn): a wall `length`
# along its X, `height` up its Y, `thick` through its Z, centred on its origin, with rectangular
# openings (windows, doors) cut out of it; its shapes and looks whole are the boxes around them. Its
# metadata says how the fracture tool cuts it (fracture_scenes.gd does): every placement shares the
# chunks.

CHUNK_DENSITY = 60.0  # kg per cubic metre: light, so what breaks flies


def wall_rects(length, height, openings):
    """The boxes a wall with openings is made of, as (x, y, width, height) about its centre: band by
    band up the wall (split where an opening starts or ends), the solid stretches across each.
    Openings are (x0, x1, y0, y1) from the wall's bottom left corner."""
    ys = sorted({0.0, height} | {y for o in openings for y in (o[2], o[3]) if 0.0 < y < height})
    rects = []
    for ya, yb in zip(ys, ys[1:]):
        gaps = sorted((o[0], o[1]) for o in openings if o[2] <= ya + 1e-6 and o[3] >= yb - 1e-6)
        x = 0.0
        for g0, g1 in gaps + [(length, length)]:
            if g0 - x > 1e-6:
                rects.append(((x + g0) / 2 - length / 2, (ya + yb) / 2 - height / 2, g0 - x, yb - ya))
            x = max(x, g1)
    return rects


def design(name, length, height, thick, mat, pieces, seed, held=1, openings=()):
    """Writes a wall design's scene (uncut) once; its ext_resource id for the arena."""
    if name in designs:
        return designs[name]
    c = MATERIALS[mat]
    dsub = ['[sub_resource type="StandardMaterial3D" id="Mat"]\nalbedo_color = Color(%s, %s, %s, 1)\nroughness = 0.9\n' % (f(c[0]), f(c[1]), f(c[2]))]
    dnodes = []
    title = "".join(part.capitalize() for part in name.split("_"))
    root = ['[node name="%s" type="CbBreakable"]' % title]
    if held != 1:
        root.append("held_at = %d" % held)
    root.append('metadata/cinderbox_fracture = {"crowd": 0.0, "density": %s, "pieces": %d, "seed": %d}' % (f(CHUNK_DENSITY), pieces, seed))
    dnodes.append("\n".join(root) + "\n")
    for k, (x, y, w, h) in enumerate(wall_rects(length, height, openings)):
        size = (w, h, thick)
        dsub.append('[sub_resource type="BoxShape3D" id="Shape_%d"]\nsize = %s\n' % (k, vec(size)))
        dsub.append('[sub_resource type="BoxMesh" id="Box_%d"]\nmaterial = SubResource("Mat")\nsize = %s\n' % (k, vec(size)))
        at = transform((x, y, 0.0))
        suffix = "" if k == 0 else str(k + 1)
        dnodes.append('[node name="Shape%s" type="CollisionShape3D" parent="."]\ntransform = %s\nshape = SubResource("Shape_%d")\n' % (suffix, at, k))
        dnodes.append('[node name="Mesh%s" type="MeshInstance3D" parent="."]\ntransform = %s\nmesh = SubResource("Box_%d")\n' % (suffix, at, k))
    os.makedirs(PARTS, exist_ok=True)
    header = "[gd_scene format=3]\n\n; A wall of the arena that breaks, written by tools/make_arena.py (%s); cut by fracture_scenes.gd.\n\n" % name
    with open(os.path.join(PARTS, name + ".tscn"), "w", encoding="utf-8", newline="\n") as out:
        out.write(header + "\n".join(dsub) + "\n" + "\n".join(dnodes))
    eid = "%d_%s" % (len(ext) + 1, name)
    ext.append('[ext_resource type="PackedScene" path="res://maps/arena_parts/%s.tscn" id="%s"]\n' % (name, eid))
    designs[name] = eid
    return eid


def place(name, parent, eid, center, yaw=0.0):
    """A placement of a wall design."""
    name = unique(parent, name)
    lines = ['[node name="%s" parent="%s" instance=ExtResource("%s")]' % (name, parent, eid)]
    lines.append("transform = " + transform(center, yaw))
    nodes.append("\n".join(lines) + "\n")
    return name if parent == "." else parent + "/" + name


# --- the level --------------------------------------------------------------------------------

HALF = 70.0  # the playable square's half size
RING_IN, RING_OUT = 24.0, 36.0  # the ring road's inner and outer edge (from the middle)
WALK = 4.0  # sidewalks
STREET = 6.0  # a street's half width


def ground():
    solid("Ground", ".", (0, -0.5, 0), (2 * HALF + 40, 1, 2 * HALF + 40), "Grass")


def roads():
    g = group("Roads")
    # The ring: four straight pieces (paint only: the ground is the floor).
    for i, (cx, cz, sx, sz) in enumerate([
        (0, (RING_IN + RING_OUT) / 2, 2 * RING_OUT, RING_OUT - RING_IN),
        (0, -(RING_IN + RING_OUT) / 2, 2 * RING_OUT, RING_OUT - RING_IN),
        ((RING_IN + RING_OUT) / 2, 0, RING_OUT - RING_IN, 2 * RING_IN),
        (-(RING_IN + RING_OUT) / 2, 0, RING_OUT - RING_IN, 2 * RING_IN),
    ]):
        deco("Ring", g, (cx, 0.02, cz), (sx, 0.04, sz), "Road")
    # The four streets out to the edge.
    length = HALF - RING_OUT
    mid = RING_OUT + length / 2
    for x, z, sx, sz in [(mid, 0, length, 2 * STREET), (-mid, 0, length, 2 * STREET), (0, mid, 2 * STREET, length), (0, -mid, 2 * STREET, length)]:
        deco("Street", g, (x, 0.02, z), (sx, 0.04, sz), "Road")
    # Dashed middle lines.
    ringMid = (RING_IN + RING_OUT) / 2
    t = -RING_OUT + 3
    while t < RING_OUT - 3:
        deco("Dash", g, (t, 0.05, ringMid), (2.0, 0.02, 0.2), "Line")
        deco("Dash", g, (t, 0.05, -ringMid), (2.0, 0.02, 0.2), "Line")
        if abs(t) < RING_IN - 3:
            deco("Dash", g, (ringMid, 0.05, t), (0.2, 0.02, 2.0), "Line")
            deco("Dash", g, (-ringMid, 0.05, t), (0.2, 0.02, 2.0), "Line")
        t += 5
    t = RING_OUT + 3
    while t < HALF - 2:
        for sx in (1, -1):
            deco("Dash", g, (sx * t, 0.05, 0), (2.0, 0.02, 0.2), "Line")
            deco("Dash", g, (0, 0.05, sx * t), (0.2, 0.02, 2.0), "Line")
        t += 5


def sidewalks():
    g = group("Sidewalks")
    h = 0.12
    # The park's edge: a frame just inside the ring.
    for cx, cz, sx, sz in [(0, RING_IN - WALK / 2, 2 * RING_IN, WALK), (0, -(RING_IN - WALK / 2), 2 * RING_IN, WALK),
                           (RING_IN - WALK / 2, 0, WALK, 2 * RING_IN - 2 * WALK), (-(RING_IN - WALK / 2), 0, WALK, 2 * RING_IN - 2 * WALK)]:
        solid("ParkEdge", g, (cx, h / 2, cz), (sx, h, sz), "Sidewalk")
    # The four corner blocks and the plazas beside the streets: paved, a step up from the road.
    lo, hi = RING_OUT, HALF
    for sx in (1, -1):
        for sz in (1, -1):
            # The corner block (a building stands on it).
            solid("Block", g, (sx * (lo + hi) / 2, h / 2, sz * (lo + hi) / 2), (hi - lo, h, hi - lo), "Plaza")
            # Beside the streets: from the street's edge to the corner block.
            solid("Plaza", g, (sx * (lo + hi) / 2, h / 2, sz * (STREET + lo) / 2), (hi - lo, h, lo - STREET), "Plaza")
            solid("Plaza", g, (sx * (STREET + lo) / 2, h / 2, sz * (lo + hi) / 2), (lo - STREET, h, hi - lo), "Plaza")


def park():
    g = group("Park")
    # Two paths across it.
    deco("Path", g, (0, 0.03, 0), (2 * (RING_IN - WALK), 0.04, 3.0), "Path")
    deco("Path", g, (0, 0.03, 0), (3.0, 0.04, 2 * (RING_IN - WALK)), "Path")
    # The fountain.
    f_ = group("Fountain", g)
    for i in range(4):
        a = i * math.pi / 2
        solid("Rim", f_, (math.sin(a) * 4.0, 0.4, math.cos(a) * 4.0), (8.6, 0.8, 0.6), "Stone", yaw=a)
    deco("Water", f_, (0, 0.35, 0), (7.6, 0.1, 7.6), "Water")
    solid("Column", f_, (0, 1.5, 0), (1.2, 3.0, 1.2), "Stone")
    solid("Bowl", f_, (0, 3.1, 0), (3.0, 0.4, 3.0), "Stone")
    # Trees, in four groves.
    spots = []
    for sx in (1, -1):
        for sz in (1, -1):
            for dx, dz in [(7, 7), (13, 6), (8, 13), (14, 14), (11, 10)]:
                spots.append((sx * dx, sz * dz))
    for i, (x, z) in enumerate(spots):
        t = node("Tree", "CbStatic", g, {"size": vec((0.6, 3.6, 0.6))}, (x, 1.8, z))
        node("Trunk", "MeshInstance3D", t, {"mesh": 'SubResource("%s")' % cylinder_mesh(0.3, 3.6, "Trunk")})
        big = 2.2 + 0.4 * ((i * 7) % 3)
        node("Leaves", "MeshInstance3D", t, {"mesh": 'SubResource("%s")' % sphere_mesh(big, "Leaves")}, (0, 2.6, 0))
        node("Leaves", "MeshInstance3D", t, {"mesh": 'SubResource("%s")' % sphere_mesh(big * 0.65, "LeavesLight")}, (0.6, 3.9, -0.4))
    # Benches along the paths.
    for x, z, yaw in [(6, 2.6, 0), (-6, 2.6, 0), (6, -2.6, math.pi), (-6, -2.6, math.pi), (2.6, 9, math.pi / 2), (-2.6, -9, -math.pi / 2)]:
        b = solid("Bench", g, (x, 0.25, z), (2.2, 0.5, 0.6), "Bench", yaw=yaw)
        deco("Back", b, (0, 0.45, 0.25), (2.2, 0.5, 0.1), "Bench")


def lamps():
    g = group("Lamps")
    spots = []
    edge = RING_OUT + 1.0
    for t in range(-32, 33, 16):
        spots += [(t, edge, math.pi), (t, -edge, 0), (edge, t, -math.pi / 2), (-edge, t, math.pi / 2)]
    for t in (44, 58):
        for s in (1, -1):
            spots += [(s * t, STREET + 1.0, math.pi), (s * t, -(STREET + 1.0), 0), (STREET + 1.0, s * t, -math.pi / 2), (-(STREET + 1.0), s * t, math.pi / 2)]
    for x, z, yaw in spots:
        if abs(x) == edge and abs(z) == edge:
            continue
        p = node("Lamp", "CbStatic", g, {"size": vec((0.25, 6.0, 0.25))}, (x, 3.12, z), yaw)
        node("Pole", "MeshInstance3D", p, {"mesh": 'SubResource("%s")' % cylinder_mesh(0.12, 6.0, "Metal")})
        node("Arm", "MeshInstance3D", p, {"mesh": 'SubResource("%s")' % box_mesh((0.12, 0.12, 1.6), "Metal")}, (0, 2.9, 0.75))
        node("Light", "MeshInstance3D", p, {"mesh": 'SubResource("%s")' % box_mesh((0.5, 0.18, 0.7), "Lamp")}, (0, 2.78, 1.4))


TOWER_W = 18.0  # the towers' side
TOWER_STOREY, TOWER_FLOORS = 3.5, 7
TOWER_SLAB, TOWER_COLUMN, TOWER_WALL = 0.4, 1.2, 0.5


def tower_designs():
    """A storey's wall between two corner columns: four windows (2 m wide, from 0.9 to 2.4 m up),
    in three designs of chunks; the entrance's, a door in the middle (3 m wide, 2.4 high) and a
    window either side."""
    length = TOWER_W - 2 * TOWER_COLUMN
    height = TOWER_STOREY - TOWER_SLAB
    window, sill, top = 2.0, 0.9, 2.4
    pier = (length - 4 * window) / 5
    windows = [(pier + i * (window + pier), pier + i * (window + pier) + window, sill, top) for i in range(4)]
    door = (length / 2 - 1.5, length / 2 + 1.5, 0.0, top)
    held = 7  # the floor, the slab above, the columns
    plain = [design("tower_wall_" + v, length, height, TOWER_WALL, "Tower", 44, seed, held, windows) for v, seed in (("a", 1), ("b", 2), ("c", 3))]
    entrance = design("tower_entrance", length, height, TOWER_WALL, "Tower", 44, 4, held, [windows[0], windows[3], door])
    return plain, entrance


def tower(name, center, yaw):
    """18 x 18 m, seven storeys of 3.5 m: columns at the corners, a floor at each storey with a core
    in the middle, and between them walls that break; the entrance on its local -Z side."""
    g = group(name, ".", center, yaw)
    w, base = TOWER_W, 0.12
    top = base + TOWER_FLOORS * TOWER_STOREY
    plain, entrance = tower_designs()
    for sx in (1, -1):
        for sz in (1, -1):
            solid("Column", g, (sx * (w - TOWER_COLUMN) / 2, (base + top) / 2, sz * (w - TOWER_COLUMN) / 2), (TOWER_COLUMN, top - base, TOWER_COLUMN), "TowerTrim")
    solid("Core", g, (0, (base + top) / 2, 0), (5.0, top - base, 5.0), "Concrete")
    for i in range(1, TOWER_FLOORS + 1):
        y = base + i * TOWER_STOREY
        solid("Floor", g, (0, y - TOWER_SLAB / 2, 0), (w, TOWER_SLAB, w), "TowerTrim")
    solid("Roof", g, (0, top + 0.25, 0), (w + 0.6, 0.5, w + 0.6), "TowerTrim")
    # The walls: a storey and a side each, flush with the outside.
    out = w / 2 - TOWER_WALL / 2
    for i in range(TOWER_FLOORS):
        y = base + i * TOWER_STOREY + (TOWER_STOREY - TOWER_SLAB) / 2
        for a in range(4):
            ang = a * math.pi / 2
            eid = entrance if (i == 0 and a == 2) else plain[(i + a) % 3]
            place("Wall", g, eid, (math.sin(ang) * out, y, math.cos(ang) * out), ang)
    # The entrance's canopy, over the door at the first floor's slab.
    solid("Canopy", g, (0, base + TOWER_STOREY - TOWER_SLAB / 2, -(w / 2 + 1.6)), (5.0, 0.3, 3.2), "TowerTrim")


def garage(name, center, yaw):
    """22 x 16 m: a floor at 0, two more and a roof on pillars, ramps up the outside (local +Z side),
    brick parapets that break along the other long side (local -Z) and the short sides."""
    g = group(name, ".", center, yaw)
    w, d = 22.0, 16.0
    storey = 4.5
    levels = 3  # the floors above the ground: 4.5, 9, 13.5 (the roof)
    base = 0.12
    parapet = design("garage_parapet", w / 2 - 0.4, 1.2, 0.35, "Brick", 14, 5)
    rail = design("garage_rail", d - 0.4, 1.2, 0.4, "ConcreteDark", 20, 6)
    side = [design("garage_side_" + v, 22.0, storey, 0.4, "ConcreteDark", 60, seed) for v, seed in (("a", 7), ("b", 8))]
    for i in range(1, levels + 1):
        y = base + i * storey
        solid("Slab", g, (0, y - 0.25, 0), (w, 0.5, d), "Concrete")
        # Parapets: bricks along the open long side, concrete rails on the short sides (they stop
        # short of the parapets' corner, so no two walls' chunks start inside each other).
        place("Parapet", g, parapet, (-w / 4, y + 0.6, -(d / 2 - 0.2)))
        place("Parapet", g, parapet, (w / 4, y + 0.6, -(d / 2 - 0.2)))
        for sx in (1, -1):
            place("Rail", g, rail, (sx * (w / 2 - 0.2), y + 0.6, 0.2), math.pi / 2)
    for x in (-w / 2 + 1, -w / 6, w / 6, w / 2 - 1):
        for z in (-d / 2 + 1, 0, d / 2 - 1):
            solid("Pillar", g, (x, base + levels * storey / 2 - 0.25, z), (0.8, levels * storey - 0.5, 0.8), "ConcreteDark")
    # Ramps up the +Z side, outside the slabs, in two lanes: switchbacks, one a storey, 14 m long for
    # 4.5 m up (18 degrees). Lane A (next to the slab) goes up toward +X, lane B back toward -X; a
    # landing across both lanes at each turn joins them to each other and to that floor's edge.
    run, width = 14.0, 4.0
    angle = math.atan2(storey, run)
    length = math.hypot(run, storey)
    lanes = (d / 2 + width / 2, d / 2 + width * 1.5)
    low, high = -4.0, 10.0  # where the ramps start and end along X
    for i in range(levels):
        y0 = base + i * storey
        going = 1 if i % 2 == 0 else -1
        z = lanes[i % 2]
        mid_x = (low + high) / 2
        solid("Ramp", g, (mid_x, y0 + storey / 2 - 0.2, z), (width, 0.4, length), "Concrete", yaw=going * math.pi / 2, pitch=-angle)
        # The landing at its top, across both lanes and onto the floor's edge.
        top_x = high if going == 1 else low
        landing_x = top_x + going * 1.0
        solid("Landing", g, (landing_x, y0 + storey - 0.25, d / 2 + width - 1.0), (4.0, 0.5, 2 * width + 2.0), "Concrete")
    # The outer lane's wall, all the way up: a storey each (22 m: high - low + 8).
    for i in range(levels):
        place("RampWall", g, side[i % 2], ((low + high) / 2, base + i * storey + storey / 2, d / 2 + 2 * width + 0.2))

    # A sign on the roof's edge.
    deco("Sign", g, (0, base + levels * storey + 1.8, -(d / 2 - 0.1)), (8.0, 1.6, 0.2), "TowerTrim")


def cover():
    g = group("Cover")
    # Brick walls in the plazas beside the streets: 6 m long, 2.5 m high, across the way.
    walls = [design("cover_wall_" + v, 6.0, 2.5, 0.35, "Brick", 24, seed) for v, seed in (("a", 9), ("b", 10))]
    for sx in (1, -1):
        for sz in (1, -1):
            place("CoverWall", g, walls[(sx + sz) // 2 % 2], (sx * 47, 1.37, sz * 20), math.pi / 2)
            place("CoverWall", g, walls[(sx - sz) // 2 % 2], (sx * 20, 1.37, sz * 47))
    # Low concrete barriers along the streets' ends.
    for s in (1, -1):
        solid("Barrier", g, (s * 52, 0.6, STREET - 1.5), (4.0, 1.2, 0.8), "Concrete")
        solid("Barrier", g, (s * 52, 0.6, -(STREET - 1.5)), (4.0, 1.2, 0.8), "Concrete")
        solid("Barrier", g, (STREET - 1.5, 0.6, s * 52), (0.8, 1.2, 4.0), "Concrete")
        solid("Barrier", g, (-(STREET - 1.5), 0.6, s * 52), (0.8, 1.2, 4.0), "Concrete")


def skyline():
    g = group("Skyline")
    seg = 20.0
    heights = [30, 22, 38, 26, 34, 20, 28, 36]
    mats = ["Skyline1", "Skyline2", "Skyline3"]
    k = 0
    span = HALF + 10
    t = -span
    while t < span:
        for side in range(4):
            hgt = heights[(k + side * 3) % len(heights)]
            mat = mats[(k + side) % 3]
            if side == 0:
                c, s = (t + seg / 2, hgt / 2, HALF + 5), (seg, hgt, 10)
            elif side == 1:
                c, s = (t + seg / 2, hgt / 2, -HALF - 5), (seg, hgt, 10)
            elif side == 2:
                c, s = (HALF + 5, hgt / 2, t + seg / 2), (10, hgt, seg)
            else:
                c, s = (-HALF - 5, hgt / 2, t + seg / 2), (10, hgt, seg)
            solid("Block", g, c, s, mat)
        k += 1
        t += seg


def spawns():
    node("Spawn", "CbSpawn", ".", {"radius": "4.0"}, (0, 1.5, -14))
    # Team 1 on the ring road's west side, facing the middle (east); team 2 on its east side, facing west.
    node("Team1", "CbSpawn", ".", {"radius": "3.5", "tag": '"team1"'}, (-29, 1.5, 0), math.pi / 2)
    node("Team2", "CbSpawn", ".", {"radius": "3.5", "tag": '"team2"'}, (29, 1.5, 0), -math.pi / 2)


def main():
    material_blocks()
    nodes.append('[node name="Arena" type="Node3D"]\n')
    ground()
    roads()
    sidewalks()
    park()
    lamps()
    corner = (RING_OUT + HALF) / 2
    # The towers' entrances face the streets their side starts on (the west's, the east's), as the
    # garages' ramps do.
    tower("TowerNW", (-corner, 0, corner), 0.0)
    tower("TowerSE", (corner, 0, -corner), math.pi)
    garage("GarageNE", (corner, 0, corner), math.pi)
    garage("GarageSW", (-corner, 0, -corner), 0.0)
    cover()
    skyline()
    spawns()
    header = "[gd_scene format=3]\n\n; The slice's first arena (ROADMAP step 4), written by tools/make_arena.py: see it for the layout.\n\n"
    with open(OUT, "w", encoding="utf-8", newline="\n") as out:
        out.write(header + "".join(ext) + "\n" + "\n".join(sub) + "\n" + "\n".join(nodes))
    print("wrote %s: %d nodes, %d resources, %d wall designs (cut them: fracture_scenes.gd)" % (OUT, len(nodes), len(sub), len(designs)))


main()
