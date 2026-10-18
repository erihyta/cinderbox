extends SceneTree
## Checks what the map bake makes of level surfaces under a CbStatic:
##   - a ConcavePolygonShape3D (here Godot's own trimesh of a PlaneMesh): a triangle mesh, its
##     triangles facing the way the plane does (counter-clockwise for Box3D), its corners shared;
##   - a HeightMapShape3D: a height field from its corner (Godot's is centred), scaled, a point that
##     is not a number a hole in the four cells around it;
##   - either under a CbProp, or in an item, is refused and said (Box3D moves no body of one).
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_map_surfaces.gd
##
## Exit code 0 when everything held.

var _failures := 0


func _check(what: String, ok: bool, detail = "") -> void:
	print(("ok    " if ok else "FAIL  ") + what + ("" if ok or str(detail) == "" else ": " + str(detail)))
	if not ok:
		_failures += 1


func _shape(parent: Node, shape: Shape3D, at := Transform3D()) -> CollisionShape3D:
	var node := CollisionShape3D.new()
	node.shape = shape
	node.transform = at
	parent.add_child(node)
	return node


func _initialize() -> void:
	var root := Node3D.new()
	root.add_child(CbSpawn.new())
	var level := CbStatic.new()
	level.name = "Ground"
	level.position = Vector3(10, 0, 0)
	root.add_child(level)
	# A 4 by 4 plane, two by two quads, as Godot makes its trimesh.
	var plane := PlaneMesh.new()
	plane.size = Vector2(4, 4)
	plane.subdivide_width = 1
	plane.subdivide_depth = 1
	_shape(level, plane.create_trimesh_shape())
	# A 5 by 5 height map, scaled 2 across and 3 up, its middle point a hole.
	var map := HeightMapShape3D.new()
	map.map_width = 5
	map.map_depth = 5
	var data := PackedFloat32Array()
	for z in 5:
		for x in 5:
			data.append(NAN if x == 2 and z == 2 else 0.1 * x)
	map.map_data = data
	_shape(level, map, Transform3D(Basis().scaled(Vector3(2, 3, 2)), Vector3(0, 0, 20)))
	# Neither may move.
	var prop := CbProp.new()
	prop.name = "Lump"
	_shape(prop, plane.create_trimesh_shape())
	root.add_child(prop)

	var result: Dictionary = CinderboxMapBaker.new().bake(root, "user://check_map_surfaces.cbmap")
	_check("the bake succeeds", result["ok"], result["error"])
	var mesh: Dictionary = {}
	var field: Dictionary = {}
	for piece in result.get("pieces", []):
		if piece["kind"] == "mesh":
			mesh = piece
		elif piece["kind"] == "height_field":
			field = piece
	_check("a triangle mesh", mesh.size() > 0, result.get("pieces"))
	if mesh.size() > 0:
		var vertices: PackedVector3Array = mesh["vertices"]
		var indices: PackedInt32Array = mesh["indices"]
		_check("eight triangles on nine shared corners", indices.size() == 24 and vertices.size() == 9, [indices.size(), vertices.size()])
		var up := true
		for t in range(0, indices.size(), 3):
			var a := vertices[indices[t]]
			var n := (vertices[indices[t + 1]] - a).cross(vertices[indices[t + 2]] - a)
			up = up and n.y > 0.0
		_check("every triangle counter-clockwise seen from above (it faces up, as the plane does)", up)
		_check("where the CbStatic is", (mesh["center"] as Vector3).is_equal_approx(Vector3(10, 0, 0)), mesh["center"])
	_check("a height field", field.size() > 0, result.get("pieces"))
	if field.size() > 0:
		_check("5 by 5 points, scaled 2 across and 3 up", field["count"] == Vector2i(5, 5) and (field["scale"] as Vector3).is_equal_approx(Vector3(2, 3, 2)),
			[field["count"], field["scale"]])
		_check("from its corner: Godot's is centred on its node", (field["center"] as Vector3).is_equal_approx(Vector3(10 - 4, 0, 20 - 4)), field["center"])
		_check("the point that is not a number: the four cells around it are holes", field["holes"] == 4, field["holes"])
	var said := false
	for w in result["warnings"]:
		said = said or (w.contains("Lump") and w.contains("level geometry only"))
	_check("a concave polygon on a CbProp is skipped and said", said, result["warnings"])

	var item := CbItem.new()
	item.kind = "test.lump"
	_shape(item, plane.create_trimesh_shape())
	var baked: Dictionary = item.bake()
	_check("an item of a concave polygon does not bake, said", baked["text"] == "" and String(baked["error"]).contains("level geometry only"), baked["error"])
	item.free()

	root.free()
	print("%d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
