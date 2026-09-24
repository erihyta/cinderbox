extends SceneTree
## Builds the example character item's scene and bakes it:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_character_example.gd -- --out=<folder>
##
## <folder> is characters/robot/client/characters/robot in this repository (an absolute path). The
## scene is what an author normally gets by importing a model with Godot's humanoid retargeting (a
## Skeleton3D with SkeletonProfileHumanoid bone names and an AnimationPlayer), plus the Cinderbox
## nodes: a CbCharacter at the root, a CinderboxSkeleton driving the skeleton, and CbHitbox zones.
## It is generated so a fresh clone can rebuild it; open it in Godot and edit it freely, then press
## Bake on the CbCharacter.
##
## The robot is rigid boxes on bones and deliberately built differently from the built-in rig:
## taller, longer arms, its own clips. That makes it plain in game and in hit tests which one is in use.

const BONES := [
	# name, parent, offset from the parent (metres)
	["Hips", "", Vector3(0, 1.02, 0)],
	["Spine", "Hips", Vector3(0, 0.10, 0)],
	["Chest", "Spine", Vector3(0, 0.14, 0)],
	["UpperChest", "Chest", Vector3(0, 0.14, 0)],
	["Neck", "UpperChest", Vector3(0, 0.16, 0)],
	["Head", "Neck", Vector3(0, 0.09, 0)],
	["LeftShoulder", "UpperChest", Vector3(0.08, 0.12, 0)],
	["LeftUpperArm", "LeftShoulder", Vector3(0.14, 0, 0)],
	["LeftLowerArm", "LeftUpperArm", Vector3(0, -0.30, 0)],
	["LeftHand", "LeftLowerArm", Vector3(0, -0.28, 0)],
	["RightShoulder", "UpperChest", Vector3(-0.08, 0.12, 0)],
	["RightUpperArm", "RightShoulder", Vector3(-0.14, 0, 0)],
	["RightLowerArm", "RightUpperArm", Vector3(0, -0.30, 0)],
	["RightHand", "RightLowerArm", Vector3(0, -0.28, 0)],
	["LeftUpperLeg", "Hips", Vector3(0.11, -0.05, 0)],
	["LeftLowerLeg", "LeftUpperLeg", Vector3(0, -0.46, 0)],
	["LeftFoot", "LeftLowerLeg", Vector3(0, -0.46, 0)],
	["LeftToes", "LeftFoot", Vector3(0, -0.05, 0.13)],
	["RightUpperLeg", "Hips", Vector3(-0.11, -0.05, 0)],
	["RightLowerLeg", "RightUpperLeg", Vector3(0, -0.46, 0)],
	["RightFoot", "RightLowerLeg", Vector3(0, -0.46, 0)],
	["RightToes", "RightFoot", Vector3(0, -0.05, 0.13)],
]

# bone, centre (bone space), size, colour
const PARTS := [
	["Hips", Vector3(0, 0.02, 0), Vector3(0.34, 0.2, 0.22), "dark"],
	["Chest", Vector3(0, 0.12, 0), Vector3(0.44, 0.42, 0.26), "body"],
	["Head", Vector3(0, 0.13, 0), Vector3(0.24, 0.26, 0.26), "body"],
	["Head", Vector3(0, 0.15, 0.12), Vector3(0.2, 0.06, 0.04), "visor"],
	["LeftUpperArm", Vector3(0, -0.15, 0), Vector3(0.11, 0.3, 0.11), "body"],
	["LeftLowerArm", Vector3(0, -0.14, 0), Vector3(0.1, 0.28, 0.1), "dark"],
	["LeftHand", Vector3(0, -0.06, 0), Vector3(0.09, 0.12, 0.06), "body"],
	["RightUpperArm", Vector3(0, -0.15, 0), Vector3(0.11, 0.3, 0.11), "body"],
	["RightLowerArm", Vector3(0, -0.14, 0), Vector3(0.1, 0.28, 0.1), "dark"],
	["RightHand", Vector3(0, -0.06, 0), Vector3(0.09, 0.12, 0.06), "body"],
	["LeftUpperLeg", Vector3(0, -0.23, 0), Vector3(0.15, 0.46, 0.15), "body"],
	["LeftLowerLeg", Vector3(0, -0.23, 0), Vector3(0.12, 0.46, 0.12), "dark"],
	["LeftFoot", Vector3(0, -0.03, 0.05), Vector3(0.12, 0.07, 0.24), "body"],
	["RightUpperLeg", Vector3(0, -0.23, 0), Vector3(0.15, 0.46, 0.15), "body"],
	["RightLowerLeg", Vector3(0, -0.23, 0), Vector3(0.12, 0.46, 0.12), "dark"],
	["RightFoot", Vector3(0, -0.03, 0.05), Vector3(0.12, 0.07, 0.24), "body"],
]

# zone, bone, centre (bone space), shape: ["sphere", r] | ["capsule", r, height] | ["box", size]
const HITBOXES := [
	["head", "Head", Vector3(0, 0.13, 0), ["sphere", 0.15]],
	["torso", "Chest", Vector3(0, 0.1, 0), ["capsule", 0.21, 0.62]],
	["torso", "Hips", Vector3(0, 0.02, 0), ["box", Vector3(0.36, 0.24, 0.24)]],
	["arm", "LeftUpperArm", Vector3(0, -0.15, 0), ["capsule", 0.07, 0.36]],
	["arm", "LeftLowerArm", Vector3(0, -0.19, 0), ["capsule", 0.065, 0.42]],
	["arm", "RightUpperArm", Vector3(0, -0.15, 0), ["capsule", 0.07, 0.36]],
	["arm", "RightLowerArm", Vector3(0, -0.19, 0), ["capsule", 0.065, 0.42]],
	["leg", "LeftUpperLeg", Vector3(0, -0.23, 0), ["capsule", 0.1, 0.56]],
	["leg", "LeftLowerLeg", Vector3(0, -0.25, 0), ["capsule", 0.08, 0.58]],
	["leg", "RightUpperLeg", Vector3(0, -0.23, 0), ["capsule", 0.1, 0.56]],
	["leg", "RightLowerLeg", Vector3(0, -0.25, 0), ["capsule", 0.08, 0.58]],
]

const COLORS := {
	"body": Color(0.62, 0.66, 0.72),
	"dark": Color(0.27, 0.3, 0.36),
	"visor": Color(1.0, 0.55, 0.15),
}

var _root: Node3D
var _skeleton: Skeleton3D


func _initialize() -> void:
	var out_dir := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out_dir = arg.substr(6)
	if out_dir == "":
		printerr("usage: -- --out=<folder>")
		quit(2)
		return
	DirAccess.make_dir_recursive_absolute(out_dir)

	_root = CbCharacter.new()
	_root.name = "Robot"
	_root.character_name = "robot"
	_root.skeleton_path = NodePath("Model/Skeleton3D")
	_root.animation_player_path = NodePath("Model/AnimationPlayer")

	var model := Node3D.new()
	model.name = "Model"
	_own(_root, model)

	_skeleton = Skeleton3D.new()
	_skeleton.name = "Skeleton3D"
	for bone in BONES:
		var index := _skeleton.add_bone(bone[0])
		if bone[1] != "":
			_skeleton.set_bone_parent(index, _skeleton.find_bone(bone[1]))
		_skeleton.set_bone_rest(index, Transform3D(Basis(), bone[2]))
	_skeleton.reset_bone_poses()
	_own(model, _skeleton)

	var attachments := {}
	for part in PARTS:
		var mesh := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = part[2]
		var material := StandardMaterial3D.new()
		material.albedo_color = COLORS[part[3]]
		material.metallic = 0.6 if part[3] != "visor" else 0.0
		material.roughness = 0.45
		if part[3] == "visor":
			material.emission_enabled = true
			material.emission = COLORS["visor"]
		box.material = material
		mesh.mesh = box
		mesh.position = part[1]
		mesh.name = "%s_%d" % [part[0], _attachment(attachments, part[0]).get_child_count()]
		_own(_attachment(attachments, part[0]), mesh)

	for hitbox in HITBOXES:
		var node := CbHitbox.new()
		node.zone = hitbox[0]
		var spec: Array = hitbox[3]
		match spec[0]:
			"sphere":
				var sphere := SphereShape3D.new()
				sphere.radius = spec[1]
				node.shape = sphere
			"capsule":
				var capsule := CapsuleShape3D.new()
				capsule.radius = spec[1]
				capsule.height = spec[2]
				node.shape = capsule
			"box":
				var cube := BoxShape3D.new()
				cube.size = spec[1]
				node.shape = cube
		node.position = hitbox[2]
		node.name = "Hitbox_%s" % hitbox[0]
		_own(_attachment(attachments, hitbox[1]), node)

	var library := AnimationLibrary.new()
	library.add_animation("idle", _clip(2.0, true, _idle))
	library.add_animation("walk", _clip(1.0, true, _stride.bind(0.5, 0.4, 0.03, 0.05)))
	library.add_animation("run", _clip(0.7, true, _stride.bind(0.9, 0.8, 0.07, 0.2)))
	library.add_animation("jump_start", _clip(0.25, false, _crouch.bind(0.6)))
	library.add_animation("fall", _clip(1.0, true, _fall))
	library.add_animation("land", _clip(0.3, false, _crouch.bind(0.8)))
	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	player.root_node = NodePath("..")
	player.add_animation_library("", library)
	_own(model, player)

	# The game poses the skeleton from the simulation; the skeleton is exactly the baked one, so no
	# retargeting.
	var driver := CinderboxSkeleton.new()
	driver.name = "CinderboxSkeleton"
	driver.skeleton_path = NodePath("../Model/Skeleton3D")
	driver.retarget = false
	driver.draw_bone_boxes = false
	driver.use_slot_color = false
	_own(_root, driver)

	var packed := PackedScene.new()
	if packed.pack(_root) != OK:
		printerr("cannot pack the character")
		quit(2)
		return
	var scene_path := out_dir.path_join("character.tscn")
	var err := ResourceSaver.save(packed, scene_path)
	if err != OK:
		printerr("cannot save ", scene_path, ": ", err)
		quit(2)
		return
	print("saved ", scene_path)

	var result: Dictionary = _root.bake_to(out_dir)
	_root.free()
	if not result["ok"]:
		printerr("bake failed: ", result["error"])
		quit(1)
		return
	print("baked %d joints, %d clips, %d hitboxes into %s" % [result["joints"], result["clips"], result["hitboxes"], out_dir])
	if result["warnings"] != "":
		print("warnings: ", result["warnings"])
	quit(0)


func _own(parent: Node, child: Node) -> void:
	parent.add_child(child)
	child.owner = _root


func _attachment(cache: Dictionary, bone: String) -> BoneAttachment3D:
	if not cache.has(bone):
		var attachment := BoneAttachment3D.new()
		attachment.name = "At_" + bone
		attachment.bone_name = bone
		attachment.bone_idx = _skeleton.find_bone(bone)
		# Where the bone rests, so the parts look right in the editor before any pose.
		attachment.transform = _skeleton.get_bone_global_rest(_skeleton.find_bone(bone))
		_own(_skeleton, attachment)
		cache[bone] = attachment
	return cache[bone]


# --- Clips: fn(phase in [0, 1)) -> { bone: Vector3 euler } and "hips_y" for the hips' height.

func _clip(length: float, loop: bool, pose: Callable) -> Animation:
	var animation := Animation.new()
	animation.length = length
	animation.loop_mode = Animation.LOOP_LINEAR if loop else Animation.LOOP_NONE
	var steps := int(ceil(length * 20.0))
	var tracks := {}
	var hips_track := animation.add_track(Animation.TYPE_POSITION_3D)
	animation.track_set_path(hips_track, NodePath("Skeleton3D:Hips"))
	for i in steps + 1:
		var t := length * float(i) / float(steps)
		var p: Dictionary = pose.call(t / length)
		for bone in p:
			if bone == "hips_y":
				continue
			if not tracks.has(bone):
				var track := animation.add_track(Animation.TYPE_ROTATION_3D)
				animation.track_set_path(track, NodePath("Skeleton3D:" + bone))
				tracks[bone] = track
			animation.rotation_track_insert_key(tracks[bone], t, Quaternion.from_euler(p[bone]))
		animation.position_track_insert_key(hips_track, t, Vector3(0, 1.02 + p.get("hips_y", 0.0), 0))
	return animation


func _idle(phase: float) -> Dictionary:
	var s := sin(phase * TAU)
	return {
		"Chest": Vector3(0.02 * s, 0, 0),
		"Head": Vector3(-0.03 * s, 0.08 * sin(phase * TAU * 0.5), 0),
		"LeftUpperArm": Vector3(0, 0, 0.06 + 0.02 * s),
		"RightUpperArm": Vector3(0, 0, -0.06 - 0.02 * s),
		"hips_y": 0.01 * s,
	}


func _stride(phase: float, legs: float, arms: float, bob: float, lean: float) -> Dictionary:
	var s := sin(phase * TAU)
	var c := cos(phase * TAU)
	return {
		"Spine": Vector3(lean, 0.1 * s, 0),
		"LeftUpperLeg": Vector3(-legs * s, 0, 0),
		"RightUpperLeg": Vector3(legs * s, 0, 0),
		"LeftLowerLeg": Vector3(legs * 0.9 * max(0.0, c), 0, 0),
		"RightLowerLeg": Vector3(legs * 0.9 * max(0.0, -c), 0, 0),
		"LeftUpperArm": Vector3(arms * s, 0, 0.08),
		"RightUpperArm": Vector3(-arms * s, 0, -0.08),
		"LeftLowerArm": Vector3(-0.3 - arms * 0.5, 0, 0),
		"RightLowerArm": Vector3(-0.3 - arms * 0.5, 0, 0),
		"hips_y": -bob * abs(c),
	}


func _crouch(phase: float, depth: float) -> Dictionary:
	var k := sin(phase * PI) * depth
	return {
		"Spine": Vector3(0.3 * k, 0, 0),
		"LeftUpperLeg": Vector3(-0.9 * k, 0, 0),
		"RightUpperLeg": Vector3(-0.9 * k, 0, 0),
		"LeftLowerLeg": Vector3(1.6 * k, 0, 0),
		"RightLowerLeg": Vector3(1.6 * k, 0, 0),
		"LeftFoot": Vector3(-0.7 * k, 0, 0),
		"RightFoot": Vector3(-0.7 * k, 0, 0),
		"LeftUpperArm": Vector3(-0.6 * k, 0, 0.2),
		"RightUpperArm": Vector3(-0.6 * k, 0, -0.2),
		"hips_y": -0.22 * k,
	}


func _fall(phase: float) -> Dictionary:
	var s := sin(phase * TAU)
	return {
		"LeftUpperArm": Vector3(0, 0, 1.3 + 0.1 * s),
		"RightUpperArm": Vector3(0, 0, -1.3 - 0.1 * s),
		"LeftUpperLeg": Vector3(-0.3 + 0.1 * s, 0, 0),
		"RightUpperLeg": Vector3(0.1 - 0.1 * s, 0, 0),
		"LeftLowerLeg": Vector3(0.5, 0, 0),
		"RightLowerLeg": Vector3(0.3, 0, 0),
	}
