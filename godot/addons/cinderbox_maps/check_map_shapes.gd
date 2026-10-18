extends SceneTree
## Checks what the map bake makes of a CbStatic's CollisionShape3D children: each a piece of its own,
## turned as the node is (roll too), sized by its shape and the scale above it; cylinders and convex
## polygons are hulls (their points scaled, two identical ones sharing a list); a flat hull, a shape a
## level cannot have yet and a disabled one are said and skipped; a CbStatic without children is its
## own box, as before.
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
	# Two drums alike (Godot's cylinder: radius 0.5, height 2), a gem and a flat one.
	var drum_node := _shape(rocks, "Drum", CylinderShape3D.new(), Transform3D(Basis(), Vector3(0, 0, -2)))
	_shape(rocks, "Drum2", CylinderShape3D.new(), Transform3D(Basis(), Vector3(2, 0, -2)))
	var gem := ConvexPolygonShape3D.new()
	gem.points = PackedVector3Array([Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1),
			Vector3(0, 0, -1)])
	var gem_node := _shape(rocks, "Gem", gem, Transform3D(Basis.from_euler(Vector3(0.3, 0.2, 0.1)), Vector3(-2, 1, -1)))
	var flat := ConvexPolygonShape3D.new()
	flat.points = PackedVector3Array([Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(1, 0, 1)])
	_shape(rocks, "Flat", flat, Transform3D())
	_shape(rocks, "Edge", WorldBoundaryShape3D.new(), Transform3D())
	var off := _shape(rocks, "Off", BoxShape3D.new(), Transform3D())
	off.disabled = true

	var result: Dictionary = CinderboxMapBaker.new().bake(root, "user://check_map_shapes.cbmap")
	_check("the bake succeeds", result["ok"], result["error"])
	var pieces: Array = result.get("pieces", [])
	_check("seven pieces: the floor, a box, a sphere, a capsule, two drums, a gem", pieces.size() == 7, pieces.size())
	_check("two hulls: the drums share theirs", int(result.get("hulls", 0)) == 2, result.get("hulls"))
	var warnings: Array = result["warnings"]
	var said := {"Flat": false, "Edge": false, "Off": false}
	for w in warnings:
		said["Flat"] = said["Flat"] or (w.contains("Flat") and w.contains("plane"))
		said["Edge"] = said["Edge"] or (w.contains("Edge") and w.contains("WorldBoundaryShape3D"))
		said["Off"] = said["Off"] or (w.contains("Off") and w.contains("disabled"))
	_check("a flat hull is said", said["Flat"], warnings)
	_check("a shape a level cannot have is said", said["Edge"], warnings)
	_check("the disabled shape is said", said["Off"], warnings)
	if pieces.size() != 7:
		root.free()
		quit(1)
		return

	var floor_piece: Dictionary = pieces[0]
	_check("a CbStatic without shapes is its own box", floor_piece["kind"] == "box" and _close(floor_piece["half_extents"], Vector3(10, 0.5, 10)),
		floor_piece)

	# Each piece where its node is, as Godot has it: centre, turn (the stored YXZ euler gives back the
	# node's own basis) and size (the shape's times the scale above it; a hull's bounds).
	for check in [[pieces[1], box_node, "box", Vector3(1, 0.5, 3)], [pieces[2], sphere_node, "sphere", Vector3(1.5, 0, 0)],
			[pieces[3], capsule_node, "capsule", Vector3(0.6, 1.4, 0)], [pieces[4], drum_node, "hull", Vector3(1, 2, 1)],
			[pieces[6], gem_node, "hull", Vector3(2, 2, 2)]]:
		var piece: Dictionary = check[0]
		var node: CollisionShape3D = check[1]
		var what: String = str(check[2]) + " (" + node.name + ")"
		var placed: Transform3D = rocks.transform * node.transform
		var turned := Basis.from_euler(piece["rotation"], EULER_ORDER_YXZ)
		var same_turn := true
		for axis in 3:
			same_turn = same_turn and _close(turned[axis], placed.basis.orthonormalized()[axis], 0.003)
		_check(what + ": its kind", piece["kind"] == check[2], piece["kind"])
		_check(what + ": where it is", _close(piece["center"], placed.origin), [piece["center"], placed.origin])
		_check(what + ": turned as it is", same_turn, [piece["rotation"], placed.basis.get_euler(EULER_ORDER_YXZ)])
		_check(what + ": its size", _close(piece["half_extents"], check[3]), piece["half_extents"])
	_check("the box keeps its roll", absf(float(pieces[1]["rotation"].z)) > 0.1, pieces[1]["rotation"])

	# A hull's points, in its piece's own frame with the scale in: the gem's are its own, doubled.
	var gem_points: PackedVector3Array = pieces[6].get("points", PackedVector3Array())
	var doubled := gem_points.size() == 6
	for i in mini(gem_points.size(), 6):
		doubled = doubled and _close(gem_points[i], gem.points[i] * 2.0)
	_check("the gem's points, scaled", doubled, gem_points)
	var drum_points: PackedVector3Array = pieces[4].get("points", PackedVector3Array())
	_check("a drum is 32 points, two rims of 16", drum_points.size() == 32, drum_points.size())
	_check("the drums share one hull", pieces[4].get("hull", -1) == pieces[5].get("hull", -2), [pieces[4].get("hull"), pieces[5].get("hull")])

	root.free()
	print("%d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
