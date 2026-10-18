extends SceneTree
## Checks what the map bake makes of a CbBreakable: a static template of its shapes and its look,
## placed where it stands; its CbProp children its pieces (rubble templates: Debris, each drawn by its
## own node), not the map's props; its own look leaves the pieces' meshes to them.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_map_breakable.gd
##
## Exit code 0 when everything held.

var _failures := 0


func _check(what: String, ok: bool, detail = "") -> void:
	print(("ok    " if ok else "FAIL  ") + what + ("" if ok or str(detail) == "" else ": " + str(detail)))
	if not ok:
		_failures += 1


func _box(parent: Node3D, size: Vector3, at: Vector3) -> void:
	var shape := BoxShape3D.new()
	shape.size = size
	var node := CollisionShape3D.new()
	node.shape = shape
	node.position = at
	parent.add_child(node)


func _mesh(parent: Node3D, size: Vector3, at: Vector3) -> void:
	var mesh := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = size
	mesh.mesh = box
	mesh.position = at
	parent.add_child(mesh)


func _initialize() -> void:
	var root := Node3D.new()
	root.add_child(CbSpawn.new())
	var ground := CbStatic.new()
	ground.size = Vector3(20, 1, 20)
	root.add_child(ground)
	# A wall 3 x 2 m of two shapes and a mesh, turned a quarter, with three pieces.
	var wall := CbBreakable.new()
	wall.name = "Wall"
	wall.transform = Transform3D(Basis(Vector3.UP, PI / 2), Vector3(4, 1, 0))
	_box(wall, Vector3(1.5, 2, 0.4), Vector3(-0.75, 0, 0))
	_box(wall, Vector3(1.5, 2, 0.4), Vector3(0.75, 0, 0))
	_mesh(wall, Vector3(3, 2, 0.4), Vector3())
	for i in 3:
		var piece := CbProp.new()
		piece.name = "Piece%d" % i
		piece.size = Vector3(1, 2, 0.4)
		piece.position = Vector3(-1 + i, 0, 0)
		_mesh(piece, Vector3(1, 2, 0.4), Vector3())
		wall.add_child(piece)
	root.add_child(wall)

	var result: Dictionary = CinderboxMapBaker.new().bake(root, "user://check_map_breakable.cbmap")
	_check("the bake succeeds", result["ok"], result["error"])
	_check("its pieces are not the map's props", int(result["props"]) == 0, result["props"])
	_check("one placement: the wall", int(result["instances"]) == 1, result["instances"])
	var walls := []
	var pieces := []
	for body in result.get("bodies", []):
		if int(body["pieces"]) > 0:
			walls.append(body)
		elif body["debris"]:
			pieces.append(body)
	_check("a breakable template: its two shapes, three pieces, its own look", walls.size() == 1 and walls[0]["shapes"] == 2 and
		walls[0]["pieces"] == 3 and walls[0]["look"] >= 0 and walls[0]["debris"] == false, walls)
	_check("three rubble templates, each drawn by its own node", pieces.size() == 3 and pieces.all(func(p): return p["look"] >= 0 and p["shapes"] == 1),
		pieces)
	var looks := {}
	for body in walls + pieces:
		looks[body["look"]] = true
	_check("the wall and each piece have looks of their own", looks.size() == 4, looks)
	_check("no warnings", result["warnings"].is_empty(), result["warnings"])

	root.free()
	print("%d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
