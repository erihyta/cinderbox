extends SceneTree
## Checks the fracture tool (fracture.gd): the chunks fill the wall and nothing more, each goes
## through a thin wall, the same settings give the same chunks, crowding makes them small at the
## focus, the faces look outward as Godot draws them, convex walls are cut too, and the map bakes
## each chunk as a hull.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_fracture.gd
##
## Exit code 0 when everything held.

const Fracture := preload("res://addons/cinderbox_maps/fracture.gd")

var _failures := 0


func _check(what: String, ok: bool, detail = "") -> void:
	print(("ok    " if ok else "FAIL  ") + what + ("" if ok or str(detail) == "" else ": " + str(detail)))
	if not ok:
		_failures += 1


func _wall(shape: Shape3D) -> CbBreakable:
	var wall := CbBreakable.new()
	var node := CollisionShape3D.new()
	node.name = "Shape"
	node.shape = shape
	wall.add_child(node)
	var look := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.7, 0.7, 0.68)
	mesh.material = material
	look.mesh = mesh
	wall.add_child(look)
	return wall


func _box(size: Vector3) -> BoxShape3D:
	var box := BoxShape3D.new()
	box.size = size
	return box


# A chunk's volume, from its mesh (tetrahedra from its centre), and its hull's bounds in the wall.
func _volume(piece: Node3D) -> float:
	var mesh: ArrayMesh = piece.get_node("Mesh").mesh
	var volume := 0.0
	for s in mesh.get_surface_count():
		var v: PackedVector3Array = mesh.surface_get_arrays(s)[Mesh.ARRAY_VERTEX]
		var index = mesh.surface_get_arrays(s)[Mesh.ARRAY_INDEX]
		var order := range(v.size()) if index == null or index.is_empty() else Array(index)
		for t in range(0, order.size(), 3):
			volume += absf(v[order[t]].dot(v[order[t + 1]].cross(v[order[t + 2]]))) / 6.0
	return volume


func _bounds(piece: Node3D) -> AABB:
	var points: PackedVector3Array = piece.get_node("Shape").shape.points
	var box := AABB(points[0] + piece.position, Vector3())
	for p in points:
		box = box.expand(p + piece.position)
	return box


# +1 when a triangle (a, b, c) with normal n is drawn as Godot draws its own meshes' outsides.
func _winding(a: Vector3, b: Vector3, c: Vector3, n: Vector3) -> int:
	return 1 if (b - a).cross(c - a).dot(n) > 0.0 else -1


func _mesh_windings(mesh: Mesh) -> Dictionary:
	var counts := {1: 0, -1: 0}
	for s in mesh.get_surface_count():
		var arrays := mesh.surface_get_arrays(s)
		var v: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var n: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
		var index = arrays[Mesh.ARRAY_INDEX]
		var order := range(v.size()) if index == null or index.is_empty() else Array(index)
		for t in range(0, order.size(), 3):
			counts[_winding(v[order[t]], v[order[t + 1]], v[order[t + 2]], n[order[t]])] += 1
	return counts


func _points(pieces: Array) -> Array:
	return pieces.map(func(p): return [p.position, p.get_node("Shape").shape.points])


func _initialize() -> void:
	var wall := _wall(_box(Vector3(4, 3, 0.4)))
	var made: Dictionary = Fracture.fracture(wall, 24, 7, 0.0)
	var pieces: Array = made.pieces
	_check("a 4 x 3 x 0.4 m wall cut into 24", made.error == "" and pieces.size() == 24, [made.error, pieces.size()])
	var volume := 0.0
	var inside := true
	var through := true
	for piece in pieces:
		volume += _volume(piece)
		var box := _bounds(piece)
		inside = inside and AABB(Vector3(-2.001, -1.501, -0.201), Vector3(4.002, 3.002, 0.402)).encloses(box)
		through = through and box.position.z < -0.199 and box.end.z > 0.199
	_check("the chunks fill the wall (4.8 m3, less the gaps)", absf(volume - 4.8) < 0.08, volume)
	_check("no chunk outside the wall", inside)
	_check("each chunk goes through the wall", through)
	var own := true
	var darker := false
	for piece in pieces:
		var mesh: ArrayMesh = piece.get_node("Mesh").mesh
		own = own and piece is CbProp and piece.get_node("Shape").shape is ConvexPolygonShape3D and mesh.get_surface_count() >= 1
		if mesh.get_surface_count() == 2:
			darker = darker or mesh.surface_get_material(1).albedo_color.v < mesh.surface_get_material(0).albedo_color.v
	_check("each chunk a hull and a mesh of its own", own)
	_check("the cut faces darker than the outside", darker)

	var godot_way: Dictionary = _mesh_windings(BoxMesh.new())
	var godot_sign := 1 if godot_way[1] > godot_way[-1] else -1
	var ours := {1: 0, -1: 0}
	for piece in pieces:
		var w := _mesh_windings(piece.get_node("Mesh").mesh)
		ours[1] += w[1]
		ours[-1] += w[-1]
	_check("every face turned outward as Godot's own meshes are", ours[-godot_sign] == 0 and ours[godot_sign] > 0, [godot_way, ours])

	var again: Dictionary = Fracture.fracture(wall, 24, 7, 0.0)
	_check("the same settings, the same chunks", _points(again.pieces) == _points(pieces))
	var other: Dictionary = Fracture.fracture(wall, 24, 8, 0.0)
	_check("another seed, other chunks", _points(other.pieces) != _points(pieces))

	# Crowded around a focus at the wall's right end: small chunks there, big ones at the left.
	var focus := Node3D.new()
	focus.name = "FractureFocus"
	focus.position = Vector3(1.6, 0, 0)
	wall.add_child(focus)
	var crowded: Array = Fracture.fracture(wall, 30, 3, 0.8).pieces
	var near := []
	var far := []
	for piece in crowded:
		(near if piece.position.distance_to(focus.position) < 1.0 else far).append(_volume(piece))
	var mean := func(a): return a.reduce(func(s, x): return s + x, 0.0) / maxf(1.0, a.size())
	_check("crowded: small near the focus, big away from it", near.size() > far.size() and mean.call(near) * 3.0 < mean.call(far),
		[near.size(), mean.call(near), far.size(), mean.call(far)])

	# A convex wall: a wedge.
	var wedge := ConvexPolygonShape3D.new()
	wedge.points = PackedVector3Array([Vector3(-1, -1, -1), Vector3(1, -1, -1), Vector3(-1, -1, 1), Vector3(1, -1, 1), Vector3(-1, 1, -1), Vector3(1, 1, -1)])
	var wedge_wall := _wall(wedge)
	var cut: Dictionary = Fracture.fracture(wedge_wall, 10, 1, 0.0)
	var wedge_volume := 0.0
	for piece in cut.pieces:
		wedge_volume += _volume(piece)
	_check("a wedge (a convex shape) cut into 10 that fill it (4 m3)", cut.error == "" and cut.pieces.size() == 10 and absf(wedge_volume - 4.0) < 0.06,
		[cut.error, cut.pieces.size(), wedge_volume])
	var ball := _wall(SphereShape3D.new())
	var sphere: Dictionary = Fracture.fracture(ball, 10, 1, 0.0)
	_check("a sphere is not cut, and says why", sphere.pieces.is_empty() and sphere.error.contains("box and convex"), sphere.error)

	# Baked: each chunk a hull template of its own, the wall the one placement.
	var root := Node3D.new()
	root.add_child(CbSpawn.new())
	var ground := CbStatic.new()
	ground.size = Vector3(20, 1, 20)
	root.add_child(ground)
	var baked := _wall(_box(Vector3(4, 3, 0.4)))
	baked.position = Vector3(0, 1.5, -5)
	for piece in Fracture.fracture(baked, 24, 7, 0.5).pieces:
		baked.add_child(piece)
	root.add_child(baked)
	var result: Dictionary = CinderboxMapBaker.new().bake(root, "user://check_fracture.cbmap")
	var walls := []
	var rubble := []
	for body in result.get("bodies", []):
		(walls if int(body["pieces"]) > 0 else rubble).append(body)
	_check("the bake succeeds, no warnings", result["ok"] and result["warnings"].is_empty(), [result["error"], result["warnings"]])
	var one_shape := rubble.all(func(b): return b["shapes"] == 1 and b["debris"])
	_check("the wall with 24 pieces, each a template of one shape", walls.size() == 1 and walls[0]["pieces"] == 24 and rubble.size() == 24 and
		one_shape, [walls, rubble.size()])
	# Cut chunks touch their neighbours face to face: every chunk at least one, so more links than
	# chunks less one; those along its foot hold it up.
	_check("its chunks touch (more links than chunks less one), some held at its foot", walls.size() == 1 and walls[0]["links"] >= 23 and
		walls[0]["held"] >= 2 and walls[0]["held"] < 24, walls)

	for list in [pieces, again.pieces, other.pieces, crowded, cut.pieces]:
		for piece in list:
			piece.free()
	wall.free()
	wedge_wall.free()
	ball.free()
	root.free()
	print("%d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
