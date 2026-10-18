extends SceneTree
## Checks what an item's bake makes of its CollisionShape3D nodes (CbItem.bake):
##   - one box, unscaled: the "shape box" lines items have always had (its file, and hash, unchanged);
##   - one box scaled, or a capsule: a "part" line (the scale in its size);
##   - a box, a turned capsule and a cylinder: three parts, in the frame the item is carried in, the
##     cylinder a hull of 32 points;
##   - a shape no body can have stops the bake and says which.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_item_parts.gd
##
## Exit code 0 when everything held.

var _failures := 0


func _check(what: String, ok: bool, detail = "") -> void:
	print(("ok    " if ok else "FAIL  ") + what + ("" if ok or str(detail) == "" else ": " + str(detail)))
	if not ok:
		_failures += 1


func _item(shapes: Array) -> CbItem:
	var item := CbItem.new()
	item.kind = "test.thing"
	for spec in shapes:
		var node := CollisionShape3D.new()
		node.shape = spec[0]
		node.transform = spec[1]
		item.add_child(node)
	return item


func _lines(text: String, start: String) -> Array:
	var out := []
	for line in text.split("\n"):
		if line.begins_with(start):
			out.append(line)
	return out


func _box(size: Vector3) -> BoxShape3D:
	var box := BoxShape3D.new()
	box.size = size
	return box


func _initialize() -> void:
	# One unscaled box: the format items have always had.
	var plain := _item([[_box(Vector3(0.1, 0.1, 0.8)), Transform3D(Basis(), Vector3(0, 0, -0.3))]])
	var baked: Dictionary = plain.bake()
	_check("one box bakes", baked["error"] == "", baked["error"])
	_check("as it always did: shape, half, centre, no part", baked["text"].contains("shape box\nhalf 0.0500 0.0500 0.4000\ncenter 0.0000 0.0000 -0.3000\n") and
		_lines(baked["text"], "part ").is_empty(), baked["text"])
	plain.free()

	# One box scaled: a part, the scale in its size.
	var scaled := _item([[_box(Vector3(0.1, 0.1, 0.8)), Transform3D(Basis().scaled(Vector3(2, 1, 1)), Vector3())]])
	baked = scaled.bake()
	var parts := _lines(baked["text"], "part ")
	_check("one box scaled: a part", parts.size() == 1 and parts[0].begins_with("part box 0.1000 0.0500 0.4000 "), parts)
	_check("and no shape line", _lines(baked["text"], "shape ").is_empty(), baked["text"])
	scaled.free()

	# A hammer: a head, a handle (a capsule turned to lie along -Z) and a drum.
	var capsule := CapsuleShape3D.new()
	capsule.radius = 0.03
	capsule.height = 0.66
	var cylinder := CylinderShape3D.new()
	cylinder.radius = 0.05
	cylinder.height = 0.1
	var hammer := _item([
		[_box(Vector3(0.24, 0.1, 0.1)), Transform3D(Basis(), Vector3(0, 0, -0.62))],
		[capsule, Transform3D(Basis(Vector3(1, 0, 0), PI / 2), Vector3(0, 0, -0.3))],
		[cylinder, Transform3D(Basis(), Vector3(0.2, 0, -0.62))]])
	hammer.mass = 2.0
	baked = hammer.bake()
	_check("the hammer bakes", baked["error"] == "", baked["error"])
	parts = _lines(baked["text"], "part ")
	_check("three parts: a box, a capsule, a hull", parts.size() == 3 and parts[0].begins_with("part box ") and
		parts[1].begins_with("part capsule ") and parts[2].begins_with("part hull "), parts)
	if parts.size() == 3:
		var capsule_words: PackedStringArray = parts[1].split(" ")
		# part capsule <r> <half segment> 0 <centre> <turn x y z w>: turned a quarter about X.
		_check("the capsule: its radius and half segment, its centre, a quarter turn about X",
			capsule_words[2] == "0.0300" and capsule_words[3] == "0.3000" and capsule_words[7] == "-0.3000" and
			absf(float(capsule_words[8]) - 0.70711) < 0.001 and absf(float(capsule_words[11]) - 0.70711) < 0.001, parts[1])
		var hull_words: PackedStringArray = parts[2].split(" ")
		_check("the drum: a hull of 32 points", hull_words.size() > 13 and hull_words[12] == "32" and hull_words.size() == 13 + 32 * 3,
			hull_words.size())
	_check("its weight", baked["text"].contains("mass 2.000\n"), baked["text"])
	hammer.free()

	# A shape no body can have.
	var odd := _item([[_box(Vector3(0.1, 0.1, 0.1)), Transform3D()], [WorldBoundaryShape3D.new(), Transform3D()]])
	baked = odd.bake()
	_check("a WorldBoundaryShape3D stops the bake, said", baked["error"].contains("WorldBoundaryShape3D"), baked["error"])
	odd.free()

	print("%d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
