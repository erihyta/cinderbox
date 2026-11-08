extends SceneTree
## Checks what the game takes from a character's scene to draw it (M260, look.h: CbCharacter.describe_look):
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_look.gd
##
## - a mesh on a bone is drawn (rigid on it), a hidden one is not;
## - particles, lights and sounds are effects: nodes of the entity's at their bones, for the tracks;
## - an animation's tracks are kept from the entity: into an effect by its name, into an item by its
##   socket's ("RightHand/Item/..."); a track into anything else is not played, and said so;
## - the sockets are the scene's, and the hands and the head every character has;
## - what the look does not take is said (a script, a Sprite3D, a skeleton modifier);
## - the mannequin is one skinned mesh, and nothing of it is left out.
## Exit code 1 if anything is wrong.

var failures := 0


func _check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		print("FAIL: ", what)
	else:
		print("ok: ", what)


func _initialize() -> void:
	var root := CbCharacter.new()
	root.name = "Critter"
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	skeleton.add_bone("Hips")
	skeleton.add_bone("RightHand")
	skeleton.set_bone_parent(1, 0)
	root.add_child(skeleton)
	var driver := CinderboxSkeleton.new()
	driver.name = "CinderboxSkeleton"
	driver.skeleton_path = NodePath("../Skeleton3D")
	driver.draw_bone_boxes = false
	root.add_child(driver)

	var at_hand := BoneAttachment3D.new()
	at_hand.name = "At_RightHand"
	at_hand.bone_name = "RightHand"
	skeleton.add_child(at_hand)
	var socket := CbSocket.new()
	socket.name = "RightHand"
	at_hand.add_child(socket)
	var claw := MeshInstance3D.new()
	claw.name = "Claw"
	claw.mesh = BoxMesh.new()
	at_hand.add_child(claw)
	var hidden := MeshInstance3D.new()
	hidden.name = "Hidden"
	hidden.mesh = BoxMesh.new()
	hidden.visible = false
	root.add_child(hidden)
	var glow := OmniLight3D.new()
	glow.name = "Glow"
	at_hand.add_child(glow)
	var sprite := Sprite3D.new()
	sprite.name = "Badge"
	root.add_child(sprite)
	var scripted := Node3D.new()
	scripted.name = "Scripted"
	var script := GDScript.new()
	script.source_code = "extends Node3D\n"
	script.reload()
	scripted.set_script(script)
	root.add_child(scripted)

	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	root.add_child(player)
	player.root_node = NodePath("..")
	var swing := Animation.new()
	swing.length = 0.5
	var energy := swing.add_track(Animation.TYPE_VALUE)
	swing.track_set_path(energy, NodePath("Skeleton3D/At_RightHand/Glow:light_energy"))
	swing.track_insert_key(energy, 0.1, 4.0)
	var item := swing.add_track(Animation.TYPE_ANIMATION)
	swing.track_set_path(item, NodePath("Skeleton3D/At_RightHand/RightHand/Item/AnimationPlayer"))
	swing.track_insert_key(item, 0.0, "slash")
	var lost := swing.add_track(Animation.TYPE_VALUE)
	swing.track_set_path(lost, NodePath("Badge:modulate"))
	swing.track_insert_key(lost, 0.0, Color.RED)
	var library := AnimationLibrary.new()
	library.add_animation("Swing", swing)
	player.add_animation_library("", library)

	var look: Dictionary = root.describe_look()
	print(look)
	_check(int(look.get("meshes", 0)) == 1, "the claw on the hand is drawn, the hidden mesh is not")
	_check(int(look.get("skinned", -1)) == 0, "the claw is rigid on its bone")
	_check(Array(look.get("effects", [])) == ["Glow"], "the light is an effect")
	var sockets: Array = Array(look.get("sockets", []))
	_check(sockets.has("RightHand") and sockets.has("LeftHand") and sockets.has("Head"), "the scene's socket, and the hands and head")
	var tracks: Array = Array(look.get("tracks", []))
	_check(tracks.has("Swing: Glow:light_energy"), "the light's track plays on the effect by its name")
	_check(tracks.has("Swing: RightHand/Item/AnimationPlayer"), "the item's track plays through the socket")
	_check(tracks.size() == 2, "the track into the sprite is not played")
	var not_drawn := "; ".join(PackedStringArray(look.get("not_drawn", [])))
	_check(not_drawn.contains("Badge (Sprite3D)"), "the sprite is said not to be drawn")
	_check(not_drawn.contains("Scripted (a script)"), "the script is said not to run")
	_check(not_drawn.contains("Badge:modulate"), "the lost track is said")
	root.free()

	var mannequin: Node = (load("res://characters/mannequin/character.tscn") as PackedScene).instantiate()
	var drawn: Dictionary = mannequin.describe_look()
	print(drawn)
	_check(int(drawn.get("skinned", 0)) == 1 and int(drawn.get("meshes", 0)) == 1, "the mannequin is one skinned mesh")
	_check(PackedStringArray(drawn.get("not_drawn", [])).is_empty(), "nothing of the mannequin is left out")
	mannequin.free()

	print("check_look: ", "ok" if failures == 0 else "%d failed" % failures)
	quit(1 if failures > 0 else 0)
