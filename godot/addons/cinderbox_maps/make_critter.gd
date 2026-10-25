extends SceneTree
## Builds the second example character item's scene, a critter, and bakes it:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_critter.gd -- --out=<folder>
##
## <folder> is characters/critter/client/characters/critter in this repository (an absolute path).
## The critter is what a character that is not a humanoid looks like: four legs and a tail on a
## skeleton of its own bone names (none of SkeletonProfileHumanoid's), its own clips and state
## machine, hitboxes on its bones, a low capsule, and a first-person camera on its skull. What only
## a humanoid has it does without: it falls as no ragdoll, it holds nothing (it has no hands), and
## animation packs made for humanoids do not fit it. Generated so a fresh clone can rebuild it;
## open it in Godot and edit it freely (saving the scene bakes it).
##
##   FullBody (state machine): Locomotion (1D blend space on forward_speed) <-> JumpStart / Fall / Land

const BONES := [
	# name, parent, offset from the parent (metres)
	["Root", "", Vector3(0, 0.55, 0)],
	["Body", "Root", Vector3(0, 0.05, 0)],
	["NeckBase", "Body", Vector3(0, 0.08, 0.36)],
	["Skull", "NeckBase", Vector3(0, 0.16, 0.12)],
	["Tail1", "Body", Vector3(0, 0.04, -0.38)],
	["Tail2", "Tail1", Vector3(0, 0.0, -0.26)],
	["LegFL1", "Body", Vector3(0.16, -0.06, 0.28)],
	["LegFL2", "LegFL1", Vector3(0, -0.24, 0)],
	["LegFR1", "Body", Vector3(-0.16, -0.06, 0.28)],
	["LegFR2", "LegFR1", Vector3(0, -0.24, 0)],
	["LegBL1", "Body", Vector3(0.16, -0.06, -0.28)],
	["LegBL2", "LegBL1", Vector3(0, -0.24, 0)],
	["LegBR1", "Body", Vector3(-0.16, -0.06, -0.28)],
	["LegBR2", "LegBR1", Vector3(0, -0.24, 0)],
]

# bone, centre (bone space), size, colour
const PARTS := [
	["Body", Vector3(0, 0, 0), Vector3(0.42, 0.28, 0.86), "fur"],
	["NeckBase", Vector3(0, 0.08, 0.04), Vector3(0.18, 0.2, 0.18), "fur"],
	["Skull", Vector3(0, 0.02, 0.06), Vector3(0.24, 0.2, 0.28), "fur"],
	["Skull", Vector3(0, -0.04, 0.22), Vector3(0.14, 0.1, 0.12), "dark"],
	["Skull", Vector3(0.07, 0.06, 0.18), Vector3(0.04, 0.04, 0.02), "eye"],
	["Skull", Vector3(-0.07, 0.06, 0.18), Vector3(0.04, 0.04, 0.02), "eye"],
	["Tail1", Vector3(0, 0, -0.13), Vector3(0.08, 0.08, 0.26), "fur"],
	["Tail2", Vector3(0, 0, -0.12), Vector3(0.06, 0.06, 0.24), "dark"],
	["LegFL1", Vector3(0, -0.12, 0), Vector3(0.1, 0.24, 0.1), "fur"],
	["LegFL2", Vector3(0, -0.12, 0.01), Vector3(0.08, 0.24, 0.1), "dark"],
	["LegFR1", Vector3(0, -0.12, 0), Vector3(0.1, 0.24, 0.1), "fur"],
	["LegFR2", Vector3(0, -0.12, 0.01), Vector3(0.08, 0.24, 0.1), "dark"],
	["LegBL1", Vector3(0, -0.12, 0), Vector3(0.1, 0.24, 0.1), "fur"],
	["LegBL2", Vector3(0, -0.12, 0.01), Vector3(0.08, 0.24, 0.1), "dark"],
	["LegBR1", Vector3(0, -0.12, 0), Vector3(0.1, 0.24, 0.1), "fur"],
	["LegBR2", Vector3(0, -0.12, 0.01), Vector3(0.08, 0.24, 0.1), "dark"],
]

# zone, bone, centre (bone space), shape: ["sphere", r] | ["capsule", r, height] | ["box", size]
const HITBOXES := [
	["head", "Skull", Vector3(0, 0.0, 0.08), ["sphere", 0.16]],
	["torso", "Body", Vector3(0, 0, 0), ["box", Vector3(0.44, 0.3, 0.9)]],
	["tail", "Tail1", Vector3(0, 0, -0.2), ["capsule", 0.06, 0.5]],
	["leg", "LegFL1", Vector3(0, -0.2, 0), ["capsule", 0.06, 0.5]],
	["leg", "LegFR1", Vector3(0, -0.2, 0), ["capsule", 0.06, 0.5]],
	["leg", "LegBL1", Vector3(0, -0.2, 0), ["capsule", 0.06, 0.5]],
	["leg", "LegBR1", Vector3(0, -0.2, 0), ["capsule", 0.06, 0.5]],
]

const COLORS := {
	"fur": Color(0.72, 0.5, 0.3),
	"dark": Color(0.36, 0.24, 0.16),
	"eye": Color(0.1, 0.1, 0.1),
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
	_root.name = "Critter"
	_root.character_name = "critter"
	_root.skeleton_path = NodePath("Model/Skeleton3D")
	_root.animation_player_path = NodePath("Model/AnimationPlayer")
	# It aims and looks with its neck and skull (no arms): the skull ends up on the line of sight.
	_root.aim_chain = "NeckBase:0.5 Skull:1"
	_root.aim_tip = "Skull"
	_root.look_chain = "NeckBase:0.5 Skull:0.5"
	# Its clips are made for these speeds.
	_root.movement = {"walk_speed": 2.0, "sprint_speed": 5.0}

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
		material.roughness = 0.8
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

	# Its eyes, on the skull, looking ahead (+Z): it may be seen in first person, from here.
	var camera := Camera3D.new()
	camera.name = "FirstPersonCamera"
	camera.transform = Transform3D(Basis(Vector3.UP, PI), Vector3(0, 0.08, 0.16))
	_own(_attachment(attachments, "Skull"), camera)

	# Its body: a low capsule, as a Godot character has its collision shape.
	var holder := CollisionShape3D.new()
	holder.name = "Capsule"
	var capsule := CapsuleShape3D.new()
	capsule.radius = 0.4
	capsule.height = 1.0
	holder.shape = capsule
	holder.position = Vector3(0, 0.5, 0)
	_own(_root, holder)

	var library := AnimationLibrary.new()
	library.add_animation("idle", _clip(2.0, true, _idle))
	library.add_animation("walk", _clip(0.8, true, _trot.bind(0.45, 0.03)))
	library.add_animation("run", _clip(0.5, true, _trot.bind(0.8, 0.07)))
	library.add_animation("jump_start", _clip(0.25, false, _crouch.bind(0.6)))
	library.add_animation("fall", _clip(1.0, true, _fall))
	library.add_animation("land", _clip(0.3, false, _crouch.bind(0.8)))
	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	player.root_node = NodePath("..")
	player.add_animation_library("", library)
	_own(model, player)
	_add_tree(player)

	# The game poses the skeleton from the simulation; the skeleton is exactly the baked one.
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


# --- The state machine -------------------------------------------------------------------------------

func _add_tree(player: AnimationPlayer) -> void:
	var base := AnimationNodeStateMachine.new()
	var line := AnimationNodeBlendSpace1D.new()
	line.min_space = -6.0
	line.max_space = 6.0
	for point in [[-5.0, "run", true], [-2.0, "walk", true], [0.0, "idle", false], [2.0, "walk", false], [5.0, "run", false]]:
		line.add_blend_point(_play(point[1], point[2]), point[0], -1, ("back_" if point[2] else "") + point[1])
	base.add_node("Locomotion", line, Vector2(300, 100))
	base.add_node("JumpStart", _play("jump_start"), Vector2(550, 0))
	base.add_node("Fall", _play("fall"), Vector2(800, 100))
	base.add_node("Land", _play("land"), Vector2(550, 220))
	_go(base, "Start", "Locomotion")
	_go(base, "Locomotion", "JumpStart", "jumped", 0.1)
	_go(base, "Locomotion", "Fall", "not grounded and airborne_time > 0.12", 0.15)
	_go(base, "JumpStart", "Land", "grounded and state_time > 0.1", 0.1)
	_go(base, "JumpStart", "Fall", "state_time > 0.25", 0.15)
	_go(base, "Fall", "Land", "grounded", 0.1)
	_go(base, "Land", "JumpStart", "jumped", 0.1)
	_go(base, "Land", "Locomotion", "state_time > 0.3 or speed > 1.5", 0.15)

	var root := AnimationNodeBlendTree.new()
	root.add_node("FullBody", base, Vector2(0, 0))
	root.connect_node("output", 0, "FullBody")

	var tree := AnimationTree.new()
	tree.name = "AnimationTree"
	tree.tree_root = root
	_own(_root, tree)
	tree.root_node = NodePath("../Model")
	tree.anim_player = tree.get_path_to(player)
	tree.active = false
	_root.animation_tree_path = _root.get_path_to(tree)
	_root.graph_inputs = {"FullBody/Locomotion/blend_position": "forward_speed"}


func _play(animation: String, backward := false) -> AnimationNodeAnimation:
	var node := AnimationNodeAnimation.new()
	node.animation = animation
	if backward:
		node.play_mode = AnimationNodeAnimation.PLAY_MODE_BACKWARD
	return node


func _go(machine: AnimationNodeStateMachine, from: String, to: String, when := "", xfade := 0.0) -> void:
	var t := AnimationNodeStateMachineTransition.new()
	t.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	if when.contains(" "):
		t.advance_expression = when
	elif when != "":
		t.advance_condition = when
	t.xfade_time = xfade
	machine.add_transition(from, to, t)


func _own(parent: Node, child: Node) -> void:
	parent.add_child(child)
	child.owner = _root


func _attachment(cache: Dictionary, bone: String) -> BoneAttachment3D:
	if not cache.has(bone):
		var attachment := BoneAttachment3D.new()
		attachment.name = "At_" + bone
		attachment.bone_name = bone
		attachment.bone_idx = _skeleton.find_bone(bone)
		attachment.transform = _skeleton.get_bone_global_rest(_skeleton.find_bone(bone))
		_own(_skeleton, attachment)
		cache[bone] = attachment
	return cache[bone]


# --- Clips: fn(phase in [0, 1)) -> { bone: Vector3 euler } and "root_y" for the body's height.

func _clip(length: float, loop: bool, pose: Callable) -> Animation:
	var animation := Animation.new()
	animation.length = length
	animation.loop_mode = Animation.LOOP_LINEAR if loop else Animation.LOOP_NONE
	var steps := int(ceil(length * 20.0))
	var tracks := {}
	var root_track := animation.add_track(Animation.TYPE_POSITION_3D)
	animation.track_set_path(root_track, NodePath("Skeleton3D:Root"))
	for i in steps + 1:
		var t := length * float(i) / float(steps)
		var p: Dictionary = pose.call(t / length)
		for bone in p:
			if bone == "root_y":
				continue
			if not tracks.has(bone):
				var track := animation.add_track(Animation.TYPE_ROTATION_3D)
				animation.track_set_path(track, NodePath("Skeleton3D:" + bone))
				tracks[bone] = track
			animation.rotation_track_insert_key(tracks[bone], t, Quaternion.from_euler(p[bone]))
		animation.position_track_insert_key(root_track, t, Vector3(0, 0.55 + p.get("root_y", 0.0), 0))
	return animation


func _idle(phase: float) -> Dictionary:
	var s := sin(phase * TAU)
	return {
		"Body": Vector3(0.02 * s, 0, 0),
		"Skull": Vector3(-0.05 * s, 0.15 * sin(phase * TAU * 0.5), 0),
		"Tail1": Vector3(0.2, 0.3 * s, 0),
		"Tail2": Vector3(0.1, 0.4 * s, 0),
		"root_y": 0.01 * s,
	}


# A trot: the legs in diagonal pairs.
func _trot(phase: float, swing: float, bob: float) -> Dictionary:
	var s := sin(phase * TAU)
	var c := cos(phase * TAU)
	return {
		"LegFL1": Vector3(-swing * s, 0, 0),
		"LegBR1": Vector3(-swing * s, 0, 0),
		"LegFR1": Vector3(swing * s, 0, 0),
		"LegBL1": Vector3(swing * s, 0, 0),
		"LegFL2": Vector3(swing * 0.8 * max(0.0, c), 0, 0),
		"LegBR2": Vector3(swing * 0.8 * max(0.0, c), 0, 0),
		"LegFR2": Vector3(swing * 0.8 * max(0.0, -c), 0, 0),
		"LegBL2": Vector3(swing * 0.8 * max(0.0, -c), 0, 0),
		"Tail1": Vector3(0.3, 0.2 * s, 0),
		"Skull": Vector3(0.05 * c, 0, 0),
		"root_y": -bob * abs(c),
	}


func _crouch(phase: float, depth: float) -> Dictionary:
	var k := sin(phase * PI) * depth
	return {
		"LegFL1": Vector3(-0.5 * k, 0, 0),
		"LegFR1": Vector3(-0.5 * k, 0, 0),
		"LegBL1": Vector3(0.5 * k, 0, 0),
		"LegBR1": Vector3(0.5 * k, 0, 0),
		"LegFL2": Vector3(0.9 * k, 0, 0),
		"LegFR2": Vector3(0.9 * k, 0, 0),
		"LegBL2": Vector3(-0.9 * k, 0, 0),
		"LegBR2": Vector3(-0.9 * k, 0, 0),
		"Tail1": Vector3(0.6 * k, 0, 0),
		"root_y": -0.12 * k,
	}


func _fall(phase: float) -> Dictionary:
	var s := sin(phase * TAU)
	return {
		"LegFL1": Vector3(-0.6, 0, 0.2 + 0.05 * s),
		"LegFR1": Vector3(-0.6, 0, -0.2 - 0.05 * s),
		"LegBL1": Vector3(0.6, 0, 0.2),
		"LegBR1": Vector3(0.6, 0, -0.2),
		"Tail1": Vector3(0.8, 0.2 * s, 0),
	}
