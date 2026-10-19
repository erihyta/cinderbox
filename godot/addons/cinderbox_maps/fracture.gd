@tool
extends RefCounted
## Cuts a CbBreakable's shape into irregular chunks (Voronoi cells): each chunk a CbProp with a
## convex hull (ConvexPolygonShape3D) and a mesh of its own, the outside faces in the wall's own
## material and the cut faces in a darker one. Editor tooling (the Fracture button) and scripts
## (make_test_ground.gd) use it; nothing here runs in a game.
##
##   var made: Dictionary = Fracture.fracture(wall, 24, 1, 0.5)
##   made.pieces: the new CbProp nodes (not added to the wall), made.error: "" or why not.
##
## The wall's BoxShape3D and ConvexPolygonShape3D children are what is cut (each its share of the
## pieces, by size). A thin shape (a wall) is cut through: every chunk goes from face to face.
## `crowd` (0 to 1): the share of chunks crowded around the focus (a child Node3D named
## "FractureFocus", else the wall's centre), so they are small there and big farther out.
## The same settings always give the same chunks. Each chunk weighs by its size (`density`); the
## body settings are taken from a piece the wall had before, if any.

const GAP := 0.001 # each cut face moved in this much, so chunks never start overlapping
const EPS := 0.0001


static func fracture(wall: Node3D, count: int, seed: int, crowd: float) -> Dictionary:
	var sources := []
	for child in wall.get_children():
		if child is CollisionShape3D and not child.disabled:
			var planes := _shape_planes(child.shape)
			if planes.is_empty():
				return {"pieces": [], "error": "%s: only box and convex shapes can be cut" % child.name}
			sources.append({"node": child, "planes": planes})
	if sources.is_empty():
		return {"pieces": [], "error": "the wall has no CollisionShape3D to cut"}
	count = clampi(count, 2, 256)
	crowd = clampf(crowd, 0.0, 1.0)

	var focus := Vector3()
	var focus_node := wall.get_node_or_null("FractureFocus")
	if focus_node is Node3D:
		focus = focus_node.position

	# Each source its share of the chunks, by the volume of its bounds.
	var total := 0.0
	for source in sources:
		var points := Geometry3D.compute_convex_mesh_points(source.planes)
		var box := AABB(points[0], Vector3())
		for p in points:
			box = box.expand(p)
		source.box = box
		total += box.get_volume()
	var left := count
	for i in sources.size():
		var share: int = left if i == sources.size() - 1 else maxi(1, roundi(count * sources[i].box.get_volume() / total))
		share = mini(share, left - (sources.size() - 1 - i))
		sources[i].count = maxi(1, share)
		left -= sources[i].count

	var rng := RandomNumberGenerator.new()
	rng.seed = seed
	var outside := _outside_material(wall)
	var inside := _inside_material(outside)
	var pieces := []
	for source in sources:
		var at: Transform3D = source.node.transform
		var local_focus: Vector3 = at.affine_inverse() * focus
		var seeds := _seeds(source, rng, local_focus, crowd)
		var planes := []
		for plane in source.planes:
			planes.append(at * plane)
		var points := []
		for s in seeds:
			points.append(at * s)
		for i in points.size():
			var piece := _cell(points, i, planes, outside, inside)
			if piece != null:
				piece.name = "Piece%d" % pieces.size()
				pieces.append(piece)
	# The body settings of a piece it had before, if any (its weight by its size: mass 0, density).
	for child in wall.get_children():
		if child is CbProp:
			for piece in pieces:
				for setting in ["density", "friction", "bounce", "rolling", "drag"]:
					piece.set(setting, child.get(setting))
			break
	return {"pieces": pieces, "error": ""}


# A shape's faces as outward planes in its own frame; [] for a kind that cannot be cut.
static func _shape_planes(shape: Shape3D) -> Array:
	if shape is BoxShape3D:
		var h: Vector3 = shape.size * 0.5
		return [Plane(Vector3.RIGHT, h.x), Plane(Vector3.LEFT, h.x), Plane(Vector3.UP, h.y), Plane(Vector3.DOWN, h.y),
			Plane(Vector3.BACK, h.z), Plane(Vector3.FORWARD, h.z)]
	if shape is ConvexPolygonShape3D:
		return _hull_planes(Array(shape.points))
	return []


# The faces of the hull of `points`: every plane through three of them with all the rest behind it.
static func _hull_planes(points: Array) -> Array:
	var planes := []
	var n := points.size()
	if n < 4 or n > 64:
		return []
	for i in n:
		for j in range(i + 1, n):
			for k in range(j + 1, n):
				var normal: Vector3 = (points[j] - points[i]).cross(points[k] - points[i])
				if normal.length() < 1e-6:
					continue
				normal = normal.normalized()
				var d: float = normal.dot(points[i])
				var above := false
				var below := false
				for p in points:
					var side: float = normal.dot(p) - d
					above = above or side > EPS
					below = below or side < -EPS
				if above and below:
					continue
				var plane := Plane(-normal, -d) if above else Plane(normal, d)
				if not planes.any(func(q): return q.normal.is_equal_approx(plane.normal) and absf(q.d - plane.d) < EPS):
					planes.append(plane)
	return planes if planes.size() >= 4 else []


static func _inside(planes: Array, p: Vector3) -> bool:
	for plane in planes:
		if plane.normal.dot(p) > plane.d:
			return false
	return true


# Where the chunks' centres go, in the shape's frame.
static func _seeds(source: Dictionary, rng: RandomNumberGenerator, focus: Vector3, crowd: float) -> Array:
	var box: AABB = source.box
	# A thin shape is cut through: every seed on its middle plane, so each chunk spans it.
	var thin := -1
	var size := box.size
	var smallest := size.min_axis_index()
	if size[smallest] * 4.0 < size[(smallest + 1) % 3] and size[smallest] * 4.0 < size[(smallest + 2) % 3]:
		thin = smallest
	var reach := size.length() * 0.5
	var seeds := []
	var crowded := roundi(source.count * crowd)
	var tries := 0
	while seeds.size() < source.count and tries < source.count * 1000:
		tries += 1
		var p: Vector3
		if seeds.size() < crowded:
			var direction := Vector3(rng.randfn(), rng.randfn(), rng.randfn()).normalized()
			p = focus + direction * reach * rng.randf() * rng.randf()
		else:
			p = box.position + Vector3(rng.randf(), rng.randf(), rng.randf()) * size
		if thin >= 0:
			p[thin] = box.position[thin] + size[thin] * 0.5
		if not _inside(source.planes, p):
			continue
		if seeds.any(func(q): return q.distance_to(p) < 0.01):
			continue
		seeds.append(p)
	return seeds


# The chunk nearer seed i than to any other, inside the shape: a CbProp centred on it.
static func _cell(seeds: Array, i: int, outer: Array, outside: Material, inside: Material) -> Node3D:
	var planes := outer.duplicate()
	var cut := []
	for j in seeds.size():
		if j == i:
			continue
		var normal: Vector3 = (seeds[j] - seeds[i]).normalized()
		planes.append(Plane(normal, normal.dot((seeds[i] + seeds[j]) * 0.5) - GAP))
	var points := []
	for p in Geometry3D.compute_convex_mesh_points(planes):
		if not points.any(func(q): return q.distance_to(p) < EPS * 10.0):
			points.append(p)
	if points.size() < 4:
		return null
	var centre := Vector3()
	for p in points:
		centre += p
	centre /= points.size()

	var surfaces := [SurfaceTool.new(), SurfaceTool.new()] # the outside, the cut faces
	var used := [false, false]
	for s in surfaces:
		s.begin(Mesh.PRIMITIVE_TRIANGLES)
	for k in planes.size():
		var plane: Plane = planes[k]
		var face := []
		for p in points:
			if absf(plane.normal.dot(p) - plane.d) < EPS * 10.0:
				face.append(p)
		if face.size() < 3:
			continue
		var middle := Vector3()
		for p in face:
			middle += p
		middle /= face.size()
		var u := plane.normal.cross(Vector3.UP if absf(plane.normal.y) < 0.9 else Vector3.RIGHT).normalized()
		var v := plane.normal.cross(u)
		face.sort_custom(func(a, b): return atan2((a - middle).dot(v), (a - middle).dot(u)) < atan2((b - middle).dot(v), (b - middle).dot(u)))
		var which := 0 if k < outer.size() else 1
		used[which] = true
		var st: SurfaceTool = surfaces[which]
		# Godot's front faces are clockwise seen from outside; the sort went the other way round.
		for t in range(1, face.size() - 1):
			for p in [face[0], face[t + 1], face[t]]:
				st.set_normal(plane.normal)
				st.set_uv(_uv(p, plane.normal))
				st.add_vertex(p - centre)
	var mesh := ArrayMesh.new()
	for which in 2:
		if used[which]:
			surfaces[which].commit(mesh)
			mesh.surface_set_material(mesh.get_surface_count() - 1, outside if which == 0 else inside)

	var piece := CbProp.new()
	piece.position = centre
	var shape := ConvexPolygonShape3D.new()
	var local := PackedVector3Array()
	for p in points:
		local.append(p - centre)
	shape.points = local
	var collision := CollisionShape3D.new()
	collision.name = "Shape"
	collision.shape = shape
	piece.add_child(collision)
	var look := MeshInstance3D.new()
	look.name = "Mesh"
	look.mesh = mesh
	piece.add_child(look)
	return piece


# A texture laid on from the side the face looks most toward, in the wall's frame (so it runs on
# across chunks), a metre to one repeat.
static func _uv(p: Vector3, normal: Vector3) -> Vector2:
	var a := normal.abs()
	if a.x >= a.y and a.x >= a.z:
		return Vector2(p.z, -p.y)
	if a.y >= a.z:
		return Vector2(p.x, p.z)
	return Vector2(p.x, -p.y)


# The wall's own material: its first mesh's.
static func _outside_material(wall: Node3D) -> Material:
	for child in wall.get_children():
		if child is MeshInstance3D and child.mesh != null:
			if child.material_override != null:
				return child.material_override
			if child.get_surface_override_material(0) != null:
				return child.get_surface_override_material(0)
			if child.mesh.get_surface_count() > 0 and child.mesh.surface_get_material(0) != null:
				return child.mesh.surface_get_material(0)
	var plain := StandardMaterial3D.new()
	plain.albedo_color = Color(0.6, 0.6, 0.6)
	return plain


# The inside of what was cut: the wall's material, darker.
static func _inside_material(outside: Material) -> Material:
	if outside is BaseMaterial3D:
		var inner: BaseMaterial3D = outside.duplicate()
		inner.albedo_color = outside.albedo_color.darkened(0.3)
		return inner
	return outside
