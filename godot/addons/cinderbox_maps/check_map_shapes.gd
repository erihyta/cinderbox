extends SceneTree
## Checks what the map bake makes of a CbStatic's CollisionShape3D children: each a piece of its own,
## turned as the node is (roll too), sized by its shape and the scale above it; shapes a level cannot
## have yet are said and skipped; a CbStatic without children is its own box, as before.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_map_shapes.gd
##
## Exit code 0 when everything held.

var _failures := 0


func _check(what: String, ok: bool, detail = "") -> void:
	print(("ok    " if ok else "FAIL  ") + what + ("" if ok or str(detail) == "" else ": " + str(detail)))
	if not ok:
		_failures += 1


func _shape(parent: Node3D, name: String, shape: Shape3D, at: Transform3D) -> CollisionShape3D:
	var node := CollisionShape3D.new()
	node.name = name
	node.shape = shape
	node.transform = at
	parent.add_child(node)
	return node


func _close(a: Vector3, b: Vector3, by := 0.002) -> bool:
	return (a - b).length() < by


func _initialize() -> void:
	var root := Node3D.new()
	root.name = "ShapesMap"
	var spawn := CbSpawn.new()
	root.add_child(spawn)

	# A plain CbStatic: its own box.
	var floor_box := CbStatic.new()
	floor_box.name = "Floor"
	floor_box.size = Vector3(20, 1, 20)
	floor_box.position = Vector3(0, -0.5, 0)
	root.add_child(floor_box)

	# A CbStatic turned and scaled, with shapes of every kind under it.
	var rocks := CbStatic.new()
	rocks.name = "Rocks"
	rocks.size = Vector3(99, 99, 99) # not used: it has shapes
	rocks.transform = Transform3D(Basis.from_euler(Vector3(0, deg_to_rad(30), 0)).scaled(Vector3(2, 2, 2)), Vector3(5, 0, 0))
	root.add_child(rocks)
	var box := BoxShape3D.new()
	box.size = Vector3(1, 0.5, 3)
	var rolled := Transform3D(Basis.from_euler(Vector3(deg_to_rad(10), deg_to_rad(-40), deg_to_rad(35)), EULER_ORDER_YXZ), Vector3(1, 1, 0))
	var box_node := _shape(rocks, "Slab", box, rolled)
	var sphere := SphereShape3D.new()
	sphere.radius = 0.75
	var sphere_node := _shape(rocks, "Ball", sphere, Transform3D(Basis(), Vector3(0, 2, 0)))
	var capsule := CapsuleShape3D.new()
	capsule.radius = 0.3
	capsule.height = 2.0
	var capsule_node := _shape(rocks, "Pillar", capsule, Transform3D(Basis.from_euler(Vector3(0, 0, deg_to_rad(90))), Vector3(-1, 0, 1)))
	_shape(rocks, "Drum", CylinderShape3D.new(), Transform3D())
	var off := _shape(rocks, "Off", BoxShape3D.new(), Transform3D())
	off.disabled = true

	var result: Dictionary = CinderboxMapBaker.new().bake(root, "user://check_map_shapes.cbmap")
	_check("the bake succeeds", result["ok"], result["error"])
	var pieces: Array = result.get("pieces", [])
	_check("four pieces: the floor, a box, a sphere, a capsule; the cylinder and the disabled one skipped", pieces.size() == 4, pieces.size())
	var warnings: Array = result["warnings"]
	var said_cylinder := false
	var said_disabled := false
	for w in warnings:
		said_cylinder = said_cylinder or (w.contains("Drum") and w.contains("CylinderShape3D"))
		said_disabled = said_disabled or (w.contains("Off") and w.contains("disabled"))
	_check("the cylinder is said (not a level shape yet)", said_cylinder, warnings)
	_check("the disabled shape is said", said_disabled, warnings)
	if pieces.size() != 4:
		root.free()
		quit(1)
		return

	var floor_piece: Dictionary = pieces[0]
	_check("a CbStatic without shapes is its own box", floor_piece["kind"] == "box" and _close(floor_piece["half_extents"], Vector3(10, 0.5, 10)),
		floor_piece)

	# Each piece where its node is, as Godot has it: centre, turn (the stored YXZ euler gives back the
	# node's own basis) and size (the shape's times the scale above it).
	for check in [[pieces[1], box_node, "box", Vector3(1, 0.5, 3)], [pieces[2], sphere_node, "sphere", Vector3(1.5, 0, 0)],
			[pieces[3], capsule_node, "capsule", Vector3(0.6, 1.4, 0)]]:
		var piece: Dictionary = check[0]
		var node: CollisionShape3D = check[1]
		var placed: Transform3D = rocks.transform * node.transform
		var turned := Basis.from_euler(piece["rotation"], EULER_ORDER_YXZ)
		var same_turn := true
		for axis in 3:
			same_turn = same_turn and _close(turned[axis], placed.basis.orthonormalized()[axis], 0.003)
		_check(check[2] + ": its kind", piece["kind"] == check[2], piece["kind"])
		_check(check[2] + ": where it is", _close(piece["center"], placed.origin), [piece["center"], placed.origin])
		_check(check[2] + ": turned as it is", same_turn, [piece["rotation"], placed.basis.get_euler(EULER_ORDER_YXZ)])
		_check(check[2] + ": its size", _close(piece["half_extents"], check[3]), piece["half_extents"])
	_check("the box keeps its roll", absf(float(pieces[1]["rotation"].z)) > 0.1, pieces[1]["rotation"])

	root.free()
	print("%d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
