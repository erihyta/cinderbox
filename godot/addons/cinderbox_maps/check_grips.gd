extends SceneTree
## Checks what an item (a CbItem, its body and the two markers it names as grips) bakes to, on scenes
## built here:
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
	var root := CbItem.new()
	root.name = "Item"
	root.kind = "test.item"
	var body := CollisionShape3D.new()
	body.name = "Body"
	var box := BoxShape3D.new()
	box.size = Vector3(0.04, 0.1, 0.3)
	body.shape = box
	body.transform = Transform3D(body_turn, Vector3(0, 0, -0.2))
	root.add_child(body)
	body.owner = root
	for spec in [[carry, "Carry", carry_turn], [other, "Other", Basis()]]:
		if spec[0] != null:
			var grip := Marker3D.new()
			grip.name = spec[1]
			grip.transform = Transform3D(spec[2], spec[0])
			root.add_child(grip)
			grip.owner = root
	if carry != null:
		root.carry_grip = NodePath("Carry")
	if other != null:
		root.other_hand = CbItem.OTHER_AT_MARKER
		root.other_grip = NodePath("Other")
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
	var baked: Dictionary = made[0].bake()
	_check("no carrying grip: the body is where the scene has it", _near(_line(baked.text, "center"), [0, 0, -0.2]), str(_line(baked.text, "center")))
	_check("no carrying grip: the other hand's grip too", _near(_line(baked.text, "grip"), [0, 0, 0.3, 0, 0, 0, 1, 0]), str(_line(baked.text, "grip")))
	_check("no carrying grip: the scene is drawn as it is", CbItem.carry_frame_under(made[0]).is_equal_approx(Transform3D()))
	made[0].free()

	# Carried 0.1 m behind the origin: everything is written from there.
	made = _item(Vector3(0, 0, 0.1), Vector3(0, 0, 0.3))
	baked = made[0].bake()
	_check("a carrying grip: the body is written from it", _near(_line(baked.text, "center"), [0, 0, -0.3]), str(_line(baked.text, "center")))
	_check("a carrying grip: the other hand's grip is written from it", _near(_line(baked.text, "grip"), [0, 0, 0.2]), str(_line(baked.text, "grip")))
	_check("a carrying grip: the scene is moved so it is in the socket",
		CbItem.carry_frame_under(made[0]).origin.is_equal_approx(Vector3(0, 0, 0.1)))
	made[0].free()

	# A model that points along +X: the carrying grip is turned to say so, and the body with it.
	var turn := Basis(Vector3.UP, PI / 2.0) # its -Z is the scene's -X ... the item points along -X
	made = _item(Vector3(0.1, 0, 0), Vector3(-0.3, 0, 0), turn, turn)
	made[1].position = Vector3(-0.2, 0, 0)
	baked = made[0].bake()
	_check("a turned carrying grip bakes (the body turned with it)", baked.error == "", baked.error)
	_check("... the body is ahead of the hand, along the item", _near(_line(baked.text, "center"), [0, 0, -0.3]), str(_line(baked.text, "center")))
	_check("... the other hand further along it", _near(_line(baked.text, "grip"), [0, 0, -0.4]), str(_line(baked.text, "grip")))
	made[0].free()

	# As animated: no place, only that the other hand keeps the animation's.
	made = _item(null, null)
	made[0].other_hand = CbItem.OTHER_AS_ANIMATED
	baked = made[0].bake()
	_check("as animated: the grip's last number says so", _line(baked.text, "grip").size() == 8 and int(_line(baked.text, "grip")[7]) == 2, str(_line(baked.text, "grip")))
	made[0].free()

	# What cannot be baked says why.
	# The carrying grip alone is turned: the body lies as the scene has it, and says how that is
	# turned in the frame the item is carried in (a quarter back about Y).
	made = _item(Vector3(0.1, 0, 0), null, turn, Basis())
	baked = made[0].bake()
	_check("a turned carrying grip alone bakes", baked.error == "", baked.error)
	_check("... the body where the scene has it, from the hand", _near(_line(baked.text, "center"), [0.2, 0, -0.1]), str(_line(baked.text, "center")))
	var body_turn := _line(baked.text, "turn")
	_check("... and how it is turned", body_turn.size() == 4 and absf(absf(body_turn[1]) - 0.7071) < 0.001 and absf(absf(body_turn[3]) - 0.7071) < 0.001, str(body_turn))
	made[0].free()
	made = _item(null, null)
	made[1].scale = Vector3(2, 1, 1)
	baked = made[0].bake()
	_check("a scaled body is refused", baked.text == "" and String(baked.error).contains("unscaled"), baked.error)
	made[0].free()
	# The other hand: free (no grip line), turned with its marker (1), at a marker nobody named.
	made = _item(null, Vector3(0, 0, 0.3))
	made[0].other_hand = CbItem.OTHER_FREE
	_check("a free other hand bakes no grip", _line(made[0].bake().text, "grip").is_empty(), made[0].bake().text)
	made[0].other_hand = CbItem.OTHER_AT_MARKER_TURNED
	_check("turned with its marker: the grip's last number says so", int(_line(made[0].bake().text, "grip")[7]) == 1, str(_line(made[0].bake().text, "grip")))
	made[0].other_grip = NodePath("Nowhere")
	baked = made[0].bake()
	_check("an other hand at a marker that is not there is refused", baked.text == "" and String(baked.error).contains("other_grip"), baked.error)
	made[0].other_grip = NodePath("Other")
	made[0].carry_grip = NodePath("Nowhere")
	baked = made[0].bake()
	_check("a carrying grip that is not there is refused", baked.text == "" and String(baked.error).contains("carry_grip"), baked.error)
	made[0].free()

	# The item itself: what the game reads, and what an item cannot do without.
	made = _item(null, null)
	made[0].display_name = "Test Item"
	made[0].mass = 2.5
	made[0].view_offset = Vector3(0, 0.05, 0.03)
	made[0].properties = {"pickup.hold_seconds": 0.5}
	made[0].scene_file_path = "res://prefabs/test_item.tscn"
	baked = made[0].bake()
	_check("an item bakes its mass and properties", _near(_line(baked.text, "mass"), [2.5]) and baked.text.contains("property pickup.hold_seconds 0.5"), baked.text)
	_check("... its scene, its name and its first-person view",
		baked.text.contains("scene res://prefabs/test_item.tscn\n") and baked.text.contains("name Test Item\n") and _near(_line(baked.text, "view"), [0, 0.05, 0.03]), baked.text)
	# How it is used and where it goes are the item's to say: properties the engine reads.
	_check("used by selecting, with no slot of its own: nothing is said", not baked.text.contains("property use") and not baked.text.contains("property slot"), baked.text)
	made[0].use = CbItem.USE_SLOT_KEY
	made[0].slot = 3
	baked = made[0].bake()
	_check("used by its slot's key, in the third slot", baked.text.contains("property use 1") and baked.text.contains("property slot 3"), baked.text)
	made[0].kind = "two words"
	_check("a kind of two words is refused", made[0].bake().text == "" and String(made[0].bake().error).contains("kind"), made[0].bake().error)
	made[0].kind = "test.item"
	made[1].free()
	_check("an item without a body is refused", made[0].bake().text == "" and String(made[0].bake().error).contains("CollisionShape3D"), made[0].bake().error)
	made[0].free()

	print("grips: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
