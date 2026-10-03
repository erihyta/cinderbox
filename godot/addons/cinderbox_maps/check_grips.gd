extends SceneTree
## Checks what an item's CbGrip markers bake to, on scenes built here:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_grips.gd
##
## Without a carrying grip the item is carried at its origin; with one, the body and the other
## hand's grip are written in the carrying grip's frame, and the viewer's scene is moved so that
## grip is in the socket. Exit code 0 when everything held.

var _failures := 0


func _check(what: String, ok: bool, detail: String = "") -> void:
	print("  %-66s %s  %s" % [what, "ok  " if ok else "FAIL", detail])
	if not ok:
		_failures += 1


## An item: a body 0.2 m ahead of the origin, and the grips asked for.
func _item(carry: Variant, other: Variant, carry_turn := Basis(), body_turn := Basis()) -> Array:
	var root := Node3D.new()
	root.name = "Item"
	var body := CbItemBody.new()
	body.name = "Body"
	var box := BoxShape3D.new()
	box.size = Vector3(0.04, 0.1, 0.3)
	body.shape = box
	body.transform = Transform3D(body_turn, Vector3(0, 0, -0.2))
	root.add_child(body)
	body.owner = root
	for spec in [[carry, CbGrip.HAND_CARRYING, "Carry", carry_turn], [other, CbGrip.HAND_OTHER, "Other", Basis()]]:
		if spec[0] != null:
			var grip := CbGrip.new()
			grip.name = spec[2]
			grip.hand = spec[1]
			grip.transform = Transform3D(spec[3], spec[0])
			root.add_child(grip)
			grip.owner = root
	return [root, body]


func _line(text: String, key: String) -> PackedFloat64Array:
	var out := PackedFloat64Array()
	for line in text.split("\n"):
		if line.begins_with(key + " "):
			for word in line.substr(key.length() + 1).split(" ", false):
				out.append(word.to_float())
	return out


func _near(values: PackedFloat64Array, want: Array) -> bool:
	if values.size() < want.size():
		return false
	for i in want.size():
		if absf(values[i] - float(want[i])) > 0.001:
			return false
	return true


func _initialize() -> void:
	# Carried at the origin: as before.
	var made := _item(null, Vector3(0, 0, 0.3))
	var baked: Dictionary = made[1].bake()
	_check("no carrying grip: the body is where the scene has it", _near(_line(baked.text, "center"), [0, 0, -0.2]), str(_line(baked.text, "center")))
	_check("no carrying grip: the other hand's grip too", _near(_line(baked.text, "grip"), [0, 0, 0.3, 0, 0, 0, 1, 0]), str(_line(baked.text, "grip")))
	_check("no carrying grip: the scene is drawn as it is", CbGrip.carry_frame_under(made[0]).is_equal_approx(Transform3D()))
	made[0].free()

	# Carried 0.1 m behind the origin: everything is written from there.
	made = _item(Vector3(0, 0, 0.1), Vector3(0, 0, 0.3))
	baked = made[1].bake()
	_check("a carrying grip: the body is written from it", _near(_line(baked.text, "center"), [0, 0, -0.3]), str(_line(baked.text, "center")))
	_check("a carrying grip: the other hand's grip is written from it", _near(_line(baked.text, "grip"), [0, 0, 0.2]), str(_line(baked.text, "grip")))
	_check("a carrying grip: the scene is moved so it is in the socket",
		CbGrip.carry_frame_under(made[0]).origin.is_equal_approx(Vector3(0, 0, 0.1)))
	made[0].free()

	# A model that points along +X: the carrying grip is turned to say so, and the body with it.
	var turn := Basis(Vector3.UP, PI / 2.0) # its -Z is the scene's -X ... the item points along -X
	made = _item(Vector3(0.1, 0, 0), Vector3(-0.3, 0, 0), turn, turn)
	made[1].position = Vector3(-0.2, 0, 0)
	baked = made[1].bake()
	_check("a turned carrying grip bakes (the body turned with it)", baked.error == "", baked.error)
	_check("... the body is ahead of the hand, along the item", _near(_line(baked.text, "center"), [0, 0, -0.3]), str(_line(baked.text, "center")))
	_check("... the other hand further along it", _near(_line(baked.text, "grip"), [0, 0, -0.4]), str(_line(baked.text, "grip")))
	made[0].free()

	# What cannot be baked says why.
	made = _item(Vector3(0.1, 0, 0), null, turn, Basis())
	baked = made[1].bake()
	_check("a body not turned as the item is carried is refused", baked.text == "" and String(baked.error).contains("carried"), baked.error)
	made[0].free()
	made = _item(Vector3(0, 0, 0.1), Vector3(0, 0, 0.3))
	var extra := CbGrip.new()
	extra.hand = CbGrip.HAND_CARRYING
	made[0].add_child(extra)
	extra.owner = made[0]
	baked = made[1].bake()
	_check("two grips for one hand are refused", baked.text == "" and String(baked.error).contains("one CbGrip"), baked.error)
	made[0].free()

	print("grips: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
