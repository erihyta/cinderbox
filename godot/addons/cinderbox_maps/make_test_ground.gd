extends SceneTree
## Makes res://maps/test_ground.tscn and bakes it: everything example_arena.tscn has (a copy, so the
## two can change apart), its east wall opened onto bumpy terrain 40 m across, with a building-sized
## block (12 x 36 x 12 m) standing on it. The balls roll to a stop (rolling 0.05; the spawnable
## bouncy ball 0.03). Two walls a grenade breaks: one of twelve bricks, one of concrete cut into 28
## chunks by the fracture tool.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_test_ground.gd
##
## Then play it: cb_server --map test_ground (or the game's --host-local "--server-arg=--map test_ground").
## Running it again makes the scene anew from the arena: edits made to test_ground.tscn are lost.

const Fracture := preload("res://addons/cinderbox_maps/fracture.gd")
const OUT_SCENE := "res://maps/test_ground.tscn"
const OUT_MAP := "res://maps/test_ground.cbmap"
const TERRAIN_MESH := "res://maps/test_ground_terrain.res"
# The terrain: 81 by 81 points half a metre apart, east of the arena (x 20.5 to 60.5, z -20 to 20).
const POINTS := 81
const SPACING := 0.5
const ORIGIN := Vector3(40.5, 0.0, 0.0)


func _height(x: float, z: float) -> float:
	# Hills and dips up to about 3 m, fading to 0 at the arena's edge (its west side) so one walks on.
	var bumps := 1.6 * sin(x * 0.31) * cos(z * 0.27) + 0.7 * sin(x * 0.83 + z * 0.51) + 0.9 * cos(z * 0.19 - x * 0.12)
	var from_edge := clampf((x + 20.0) / 6.0, 0.0, 1.0)
	return (bumps + 1.2) * from_edge * from_edge * (3.0 - 2.0 * from_edge)


func _material(color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	return material


func _initialize() -> void:
	var arena := load("res://maps/example_arena.tscn") as PackedScene
	var root := arena.instantiate() as Node3D
	root.name = "TestGround"
	var east := root.get_node_or_null("WallEast")
	if east != null:
		root.remove_child(east)
		east.free()

	# The terrain: its collision a height map, its look the same heights as triangles.
	var ground := CbStatic.new()
	ground.name = "Terrain"
	ground.position = ORIGIN
	root.add_child(ground)
	var heights := PackedFloat32Array()
	for z in POINTS:
		for x in POINTS:
			heights.append(_height((x - (POINTS - 1) / 2) * SPACING, (z - (POINTS - 1) / 2) * SPACING))
	var map := HeightMapShape3D.new()
	map.map_width = POINTS
	map.map_depth = POINTS
	map.map_data = heights
	var shape := CollisionShape3D.new()
	shape.name = "Shape"
	shape.shape = map
	shape.scale = Vector3(SPACING, 1.0, SPACING)
	ground.add_child(shape)
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var half := (POINTS - 1) / 2
	var at := func(x: int, z: int) -> Vector3: return Vector3((x - half) * SPACING, heights[z * POINTS + x], (z - half) * SPACING)
	for z in POINTS - 1:
		for x in POINTS - 1:
			for p in [[x, z], [x + 1, z], [x, z + 1], [x + 1, z], [x + 1, z + 1], [x, z + 1]]:
				st.add_vertex(at.call(p[0], p[1]))
	st.index()
	st.generate_normals()
	# (Its mesh in a binary file of its own: as text in the scene it is a megabyte.)
	ResourceSaver.save(st.commit(), TERRAIN_MESH)
	var look := MeshInstance3D.new()
	look.name = "Mesh"
	look.mesh = load(TERRAIN_MESH)
	var grass := _material(Color(0.36, 0.52, 0.3))
	grass.cull_mode = BaseMaterial3D.CULL_DISABLED
	look.material_override = grass
	ground.add_child(look)

	# The tower: a block the size of a building, on the terrain (its foot below the ground there).
	var tower := CbStatic.new()
	tower.name = "Tower"
	var foot := 46.0
	tower.position = Vector3(foot, 18.0 + _height(foot - ORIGIN.x, -8.0) - 1.0, -8.0)
	tower.size = Vector3(12, 36, 12)
	root.add_child(tower)
	var block := MeshInstance3D.new()
	block.name = "Mesh"
	var box := BoxMesh.new()
	box.size = Vector3(12, 36, 12)
	box.material = _material(Color(0.55, 0.52, 0.5))
	block.mesh = box
	tower.add_child(block)

	# A wall that breaks (a frag grenade's blast breaks it): 4 x 3 m, twelve bricks of 1 x 1 x 0.4 m.
	var wall := CbBreakable.new()
	wall.name = "BrickWall"
	wall.position = Vector3(0, 1.5, -12)
	var whole := BoxShape3D.new()
	whole.size = Vector3(4, 3, 0.4)
	var whole_shape := CollisionShape3D.new()
	whole_shape.name = "Shape"
	whole_shape.shape = whole
	wall.add_child(whole_shape)
	var whole_look := MeshInstance3D.new()
	whole_look.name = "Mesh"
	var whole_mesh := BoxMesh.new()
	whole_mesh.size = Vector3(4, 3, 0.4)
	whole_mesh.material = _material(Color(0.62, 0.32, 0.24))
	whole_look.mesh = whole_mesh
	wall.add_child(whole_look)
	var brick_mesh := BoxMesh.new()
	brick_mesh.size = Vector3(0.98, 0.98, 0.4)
	brick_mesh.material = _material(Color(0.62, 0.32, 0.24))
	var dark := BoxMesh.new()
	dark.size = Vector3(0.98, 0.98, 0.4)
	dark.material = _material(Color(0.5, 0.25, 0.19))
	for row in 3:
		for column in 4:
			var brick := CbProp.new()
			brick.name = "Brick%d_%d" % [row, column]
			brick.size = Vector3(0.98, 0.98, 0.4)
			brick.position = Vector3(-1.5 + column, -1.0 + row, 0)
			var look_brick := MeshInstance3D.new()
			look_brick.name = "Mesh"
			look_brick.mesh = brick_mesh if (row + column) % 2 == 0 else dark
			brick.add_child(look_brick)
			wall.add_child(brick)
	root.add_child(wall)

	# A concrete wall cut by the fracture tool (the Fracture button does the same): 5 x 3.5 x 0.5 m in
	# 28 irregular chunks, crowded toward its middle.
	var concrete := CbBreakable.new()
	concrete.name = "ConcreteWall"
	concrete.position = Vector3(-8, 1.75, -12)
	var slab := BoxShape3D.new()
	slab.size = Vector3(5, 3.5, 0.5)
	var slab_shape := CollisionShape3D.new()
	slab_shape.name = "Shape"
	slab_shape.shape = slab
	concrete.add_child(slab_shape)
	var slab_look := MeshInstance3D.new()
	slab_look.name = "Mesh"
	var slab_mesh := BoxMesh.new()
	slab_mesh.size = slab.size
	slab_mesh.material = _material(Color(0.66, 0.66, 0.63))
	slab_look.mesh = slab_mesh
	concrete.add_child(slab_look)
	var chunks: Dictionary = Fracture.fracture(concrete, 28, 5, 0.4)
	for chunk in chunks.pieces:
		chunk.density = 60.0
		concrete.add_child(chunk)
	concrete.set_meta("cinderbox_fracture", {"pieces": 28, "seed": 5, "crowd": 0.4})
	root.add_child(concrete)

	# Balls that roll to a stop: the props (a CbProp starts at 0.05) and the spawnable template's.
	var material := root.get_node_or_null("Templates/bouncy_ball/Material")
	if material != null:
		material.set("rolling", 0.03)

	for child in root.get_children():
		_own(child, root)
	var packed := PackedScene.new()
	packed.pack(root)
	var saved := ResourceSaver.save(packed, OUT_SCENE)
	var result: Dictionary = CinderboxMapBaker.new().bake(root, OUT_MAP)
	print("saved %s (%s); baked %s: %s statics, %s props, %s templates, %s bytes %s" % [OUT_SCENE, error_string(saved), OUT_MAP, result.get("statics"),
		result.get("props"), result.get("templates"), result.get("bytes"), result.get("error")])
	if result.get("warnings", []).size() > 0:
		print("warnings: ", result["warnings"])
	root.free()
	quit(0 if saved == OK and result.get("ok", false) else 1)


func _own(node: Node, owner: Node) -> void:
	node.owner = owner
	for child in node.get_children():
		_own(child, owner)
