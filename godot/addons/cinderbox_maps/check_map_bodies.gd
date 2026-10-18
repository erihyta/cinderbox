extends SceneTree
## Checks what the map bake makes of bodies with CollisionShape3D children and looks of their own:
##   - a CbProp with shapes and a mesh: a template of its shapes, drawn by its own node ("#<n>"),
##     placed where it stands, turned with roll;
##   - a CbProp with only a mesh: its own box as the one shape, drawn by its node;
##   - a plain CbProp: the map's prop list, as before;
##   - a CbTemplate with shapes and a mesh, placed by a CbEntity: one template, its own look;
##   - an inline CbEntity with shapes: its components and its shapes;
##   - the look numbers are MapLookNodes' order, and a look copied has its mesh, not the preview.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_map_bodies.gd
##
## Exit code 0 when everything held.

var _failures := 0


func _check(what: String, ok: bool, detail = "") -> void:
	print(("ok    " if ok else "FAIL  ") + what + ("" if ok or str(detail) == "" else ": " + str(detail)))
	if not ok:
		_failures += 1


func _box(parent: Node3D, name: String, size: Vector3, at: Transform3D) -> CollisionShape3D:
	var shape := BoxShape3D.new()
	shape.size = size
	var node := CollisionShape3D.new()
	node.name = name
	node.shape = shape
	node.transform = at
	parent.add_child(node)
	return node


func _mesh(parent: Node3D) -> void:
	var mesh := MeshInstance3D.new()
	mesh.mesh = BoxMesh.new()
	parent.add_child(mesh)


func _body(bodies: Array, name: String) -> Dictionary:
	for body in bodies:
		if body["name"] == name:
			return body
	return {}


func _initialize() -> void:
	var root := Node3D.new()
	root.name = "BodiesMap"
	root.add_child(CbSpawn.new())
	var ground := CbStatic.new()
	ground.size = Vector3(20, 1, 20)
	root.add_child(ground)

	# Look node 0: a plain prop.
	var plain := CbProp.new()
	plain.name = "Plain"
	root.add_child(plain)
	# Look node 1: a table, two shapes and a mesh, scaled, turned with roll.
	var table := CbProp.new()
	table.name = "Table"
	table.mass = 20.0
	table.transform = Transform3D(Basis.from_euler(Vector3(0.1, 0.5, 0.25), EULER_ORDER_YXZ).scaled(Vector3(2, 2, 2)), Vector3(3, 1, 0))
	_box(table, "Top", Vector3(1, 0.1, 1), Transform3D(Basis(), Vector3(0, 0.5, 0)))
	_box(table, "Leg", Vector3(0.1, 1, 0.1), Transform3D())
	_mesh(table)
	root.add_child(table)
	# Look node 2: the same table elsewhere: its own template (its own look), the shape set shared.
	var table2 := table.duplicate() as CbProp
	table2.name = "Table2"
	table2.position = Vector3(-3, 1, 0)
	root.add_child(table2)
	# Look node 3: a crate with a mesh and no shapes: its own box is its shape.
	var crate := CbProp.new()
	crate.name = "Crate"
	crate.size = Vector3(0.5, 0.5, 0.5)
	_mesh(crate)
	root.add_child(crate)
	# Look node 4: a template with shapes and a mesh; 5: an entity placing it.
	var barrel := CbTemplate.new()
	barrel.name = "Barrel"
	var cylinder := CylinderShape3D.new()
	var barrel_shape := CollisionShape3D.new()
	barrel_shape.shape = cylinder
	barrel.add_child(barrel_shape)
	_mesh(barrel)
	root.add_child(barrel)
	var placed := CbEntity.new()
	placed.name = "PlacedBarrel"
	placed.template_name = "Barrel"
	placed.position = Vector3(0, 2, 4)
	root.add_child(placed)
	# Look node 6: an inline entity with a component and two shapes, no mesh.
	var inline_entity := CbEntity.new()
	inline_entity.name = "Ell"
	var component := CbComponent.new()
	component.component = 3 # None, Shape, Body, Material
	inline_entity.add_child(component)
	_box(inline_entity, "Long", Vector3(2, 0.2, 0.5), Transform3D())
	_box(inline_entity, "Up", Vector3(0.2, 1, 0.5), Transform3D(Basis(), Vector3(0.9, 0.5, 0)))
	inline_entity.position = Vector3(0, 3, -4)
	root.add_child(inline_entity)

	var looks: Array = []
	for node in [plain, table, table2, crate, barrel, placed, inline_entity]:
		looks.append(node)

	var result: Dictionary = CinderboxMapBaker.new().bake(root, "user://check_map_bodies.cbmap")
	_check("the bake succeeds", result["ok"], result["error"])
	_check("one plain prop in the prop list", int(result["props"]) == 1, result["props"])
	var bodies: Array = result.get("bodies", [])
	var t_table: Dictionary = _body(bodies, "Table")
	var t_table2: Dictionary = _body(bodies, "Table2")
	var t_crate: Dictionary = _body(bodies, "Crate")
	var t_barrel: Dictionary = _body(bodies, "Barrel")
	var t_ell: Dictionary = _body(bodies, "Ell")
	_check("the table: its two shapes, drawn by its node (look 1)", t_table.get("shapes") == 2 and t_table.get("look") == 1 and
		t_table.get("visual") == "#1", t_table)
	_check("the second table: drawn by its own node (look 2)", t_table2.get("look") == 2 and t_table2.get("shapes") == 2, t_table2)
	_check("the crate: its own box, drawn by its node (look 3)", t_crate.get("shapes") == 1 and t_crate.get("look") == 3, t_crate)
	_check("the barrel template: its cylinder, its own look (4)", t_barrel.get("shapes") == 1 and t_barrel.get("look") == 4, t_barrel)
	_check("the inline entity: two shapes, nothing to draw of its own", t_ell.get("shapes") == 2 and t_ell.get("look") == -1, t_ell)
	_check("five instances: two tables, the crate, the barrel and the inline entity",
		int(result["instances"]) == 5, result["instances"])

	# The bake numbered the look nodes as a client will find them.
	var found: Array = []
	_walk(root, found)
	_check("MapLookNodes' order is the scene's", found == looks, found)

	# What a client draws for "#1": a copy of the table's node, with its mesh, without the editor's preview.
	var copy := table.duplicate() as Node3D
	var meshes := 0
	for child in copy.get_children():
		if child is MeshInstance3D:
			meshes += 1
	_check("a copy of the table has its one mesh (the editor's preview is not a child)", meshes == 1, meshes)
	copy.free()

	root.free()
	print("%d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)


func _walk(node: Node, out: Array) -> void:
	if node is CbProp or node is CbTemplate or node is CbEntity:
		out.append(node)
	for child in node.get_children():
		_walk(child, out)
