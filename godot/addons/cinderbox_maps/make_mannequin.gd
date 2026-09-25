extends SceneTree
## Builds the default player character from the Universal Animation Library's mannequin (Quaternius,
## CC0) and bakes it:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_mannequin.gd
##
## If characters/mannequin/character.tscn already exists, this only bakes it again, so edits made in
## the editor stay. Add `-- --force` to build it from scratch (that discards them).
##
## The source is characters/mannequin/source/UAL1_Standard.glb (the in-place version; _RM has root
## motion, which the simulation does not want). Its import retargets it onto Godot's
## SkeletonProfileHumanoid with source/bone_map.tres, so bones have the profile's names. This script
## puts it under a CbCharacter with a CinderboxSkeleton and hitboxes sized from the skeleton's own
## bone lengths, and authors its state machine as an ordinary AnimationTree:
##
##   UpperBlend (Blend2, filter: the spine and everything above it)
##     Base:  Locomotion (1D blend space on forward_speed) <-> JumpStart / Fall / Land
##     Upper: Rest, Pistol <-> Shoot (on pistol.fired), Ready <-> Swing (the melee mod's stances)
##
## The swing (Sword_Attack, saved to animations/ by the glb's import settings so it can be edited)
## gets a "melee.strike" marker where the hand is fastest, which the melee mod hits on. (The bat's
## fire is the melee mod's own look, not the character's.) Everything here is what an author would
## do by hand in the editor; open the scene and edit freely.
##
## `-- --pack=source` builds characters/ual_mannequin/ from the library's paid Source version instead
## (put UAL1.glb in characters/ual_mannequin/source/ and give its import the same settings as the
## Standard one). It has jogs in eight directions, so its locomotion is a 2D blend space on
## move_right / move_forward and its legs are not turned. That folder is git-ignored: the paid pack,
## and everything baked from it, stays out of the repository.

const PACKS := {
	"standard": {
		"source": "res://characters/mannequin/source/UAL1_Standard.glb",
		"out": "res://characters/mannequin",
		"name": "mannequin",
	},
	"source": {
		"source": "res://characters/ual_mannequin/source/UAL1.glb",
		"out": "res://characters/ual_mannequin",
		"name": "ual_mannequin",
	},
}
var SOURCE: String
var OUT_DIR: String
var SWING: String
var _pack := "standard"
# The hand fire earlier versions keyed on the swing (the bat's own look has it now).
const OLD_FIRE_PATH := "Armature/Skeleton3D/At_RightHand/HandFire"
# The right hand's socket, and the AnimationPlayer of whatever item is held there, as the
# character's animations see them (from the model's root).
const HELD_ITEM_PLAYER := "Armature/Skeleton3D/At_RightHand/RightHand/Item/AnimationPlayer"
const SLASH_TIME := 0.2 # Sword_Attack: the swing comes forward from here

# The six built-in clips, used only if the state machine is removed (animation_tree_path cleared).
const CLIPS := {
	"idle": "Idle", "walk": "Walk", "run": "Jog_Fwd",
	"jump_start": "Jump_Start", "fall": "Jump", "land": "Jump_Land",
}

# Standard: blend space points at the speeds the clips actually cover (measured from their foot
# travel), so the feet stay planted: walking backwards plays the same clips in reverse.
const LOCOMOTION := [
	[-3.0, "Jog_Fwd", true], [-0.9, "Walk", true], [0.0, "Idle", false],
	[0.9, "Walk", false], [3.0, "Jog_Fwd", false], [5.0, "Sprint", false],
]
# Source: the eight jog directions on a circle at the game's jog speed (x = right, y = forward), a
# walk inside it and the sprint ahead of it. The measured directions: _L is forward-left, Left and
# Right are straight sideways.
const JOG := 3.0
const DIAGONAL := JOG * 0.70710678
const LOCOMOTION_2D := [
	[Vector2(0, 0), "Idle"], [Vector2(0, 1.0), "Walk"], [Vector2(0, 6.5), "Sprint"],
	[Vector2(0, JOG), "Jog_Fwd"], [Vector2(-DIAGONAL, DIAGONAL), "Jog_Fwd_L"], [Vector2(DIAGONAL, DIAGONAL), "Jog_Fwd_R"],
	[Vector2(-JOG, 0), "Jog_Left"], [Vector2(JOG, 0), "Jog_Right"],
	[Vector2(0, -JOG), "Jog_Bwd"], [Vector2(-DIAGONAL, -DIAGONAL), "Jog_Bwd_L"], [Vector2(DIAGONAL, -DIAGONAL), "Jog_Bwd_R"],
]
const STRIKE_TIME := 0.4 # Sword_Attack: the hand is fastest here

# zone, bone, radius; capsules run along the bone to its child (the profile's +Y), spheres sit on it.
const CAPSULES := [
	["torso", "Spine", 0.17], ["torso", "Chest", 0.18], ["torso", "UpperChest", 0.17],
	["arm", "LeftUpperArm", 0.065], ["arm", "LeftLowerArm", 0.055], ["arm", "RightUpperArm", 0.065], ["arm", "RightLowerArm", 0.055],
	["leg", "LeftUpperLeg", 0.09], ["leg", "LeftLowerLeg", 0.07], ["leg", "RightUpperLeg", 0.09], ["leg", "RightLowerLeg", 0.07],
]

var _root: CbCharacter
var _skeleton: Skeleton3D


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--pack="):
			_pack = arg.substr(7)
	if not PACKS.has(_pack):
		printerr("unknown pack ", _pack, " (standard or source)")
		quit(2)
		return
	SOURCE = PACKS[_pack]["source"]
	OUT_DIR = PACKS[_pack]["out"]
	SWING = OUT_DIR.path_join("animations/Sword_Attack.res")
	var scene_path := OUT_DIR.path_join("character.tscn")
	if ResourceLoader.exists(scene_path) and not ("--force" in OS.get_cmdline_user_args()):
		var existing := (load(scene_path) as PackedScene).instantiate() as CbCharacter
		print("baking the existing ", scene_path, " (--force rebuilds it)")
		_mark_swing() # a reimport of the glb may have dropped the marker
		_bake(existing)
		return

	var source := load(SOURCE) as PackedScene
	if source == null:
		printerr("cannot load ", SOURCE, " (import the project first)")
		quit(2)
		return

	_root = CbCharacter.new()
	_root.name = "Mannequin"
	_root.character_name = PACKS[_pack]["name"]
	# Directional clips walk sideways by themselves; without them the hips turn toward the travel.
	_root.turn_legs = _pack != "source"
	# Its strafe clips turn the hips and chest toward the travel; the chest should face the camera.
	_root.face_forward = _pack == "source"
	var model := source.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE) # as the editor does: saves only what differs
	model.name = "Model"
	_root.add_child(model)
	model.owner = _root
	# Editable Children: what is added under the model (the hitboxes) is saved with the
	# scene, and shows in the editor.
	_root.set_editable_instance(model, true)
	_skeleton = model.get_node("Armature/Skeleton3D") as Skeleton3D
	var player := model.get_node("AnimationPlayer") as AnimationPlayer
	_root.skeleton_path = _root.get_path_to(_skeleton)
	_root.animation_player_path = _root.get_path_to(player)
	for clip in CLIPS:
		_root.set("clip_" + clip, CLIPS[clip])

	# The game poses this skeleton from the simulation; it is exactly the baked one, so no retargeting.
	var driver := CinderboxSkeleton.new()
	driver.name = "CinderboxSkeleton"
	driver.draw_bone_boxes = false
	driver.retarget = false
	driver.use_slot_color = false
	_root.add_child(driver)
	driver.owner = _root
	driver.skeleton_path = driver.get_path_to(_skeleton)

	_add_hitboxes()
	_add_sockets()
	_mark_swing()
	_add_tree(player)

	var packed := PackedScene.new()
	if packed.pack(_root) != OK:
		printerr("cannot pack the character")
		quit(2)
		return
	if ResourceSaver.save(packed, scene_path) != OK:
		printerr("cannot save ", scene_path)
		quit(2)
		return
	print("saved ", scene_path)
	_root.free()
	# Bake what was saved, the way the editor's button would.
	_bake((load(scene_path) as PackedScene).instantiate() as CbCharacter)


func _bake(character: CbCharacter) -> void:
	var result: Dictionary = character.bake_to(OUT_DIR)
	character.free()
	if not result["ok"]:
		printerr("bake failed: ", result["error"])
		quit(1)
		return
	print("baked %d joints, %d clips, %d hitboxes, %d companion clips into %s" % [result["joints"], result["clips"],
		result["hitboxes"], result["companion_clips"], OUT_DIR])
	if result["warnings"] != "":
		print("warnings: ", result["warnings"])
	quit(0)


# --- The state machine -------------------------------------------------------------------------------

func _add_tree(player: AnimationPlayer) -> void:
	var base := AnimationNodeStateMachine.new()
	var locomotion: AnimationRootNode
	if _pack == "source":
		var plane := AnimationNodeBlendSpace2D.new()
		plane.min_space = Vector2(-4, -4)
		plane.max_space = Vector2(4, 7)
		for point in LOCOMOTION_2D:
			plane.add_blend_point(_clip(point[1]), point[0], -1, point[1])
		locomotion = plane
	else:
		var line := AnimationNodeBlendSpace1D.new()
		line.min_space = -4.0
		line.max_space = 6.0
		for point in LOCOMOTION:
			var label: String = ("Back" if point[2] else "") + point[1]
			line.add_blend_point(_clip(point[1], point[2]), point[0], -1, label)
		locomotion = line
	base.add_node("Locomotion", locomotion, Vector2(300, 100))
	base.add_node("JumpStart", _clip("Jump_Start"), Vector2(550, 0))
	base.add_node("Fall", _clip("Jump"), Vector2(800, 100))
	base.add_node("Land", _clip("Jump_Land"), Vector2(550, 220))
	_go(base, "Start", "Locomotion")
	_go(base, "Locomotion", "JumpStart", "jumped", 0.1)
	_go(base, "Locomotion", "Fall", "not grounded and airborne_time > 0.12", 0.2)
	_go(base, "JumpStart", "Land", "grounded and state_time > 0.1", 0.1)
	_go(base, "JumpStart", "Fall", "state_time > 0.3", 0.2)
	_go(base, "Fall", "Land", "grounded", 0.1)
	_go(base, "Land", "JumpStart", "jumped", 0.1)
	_go(base, "Land", "Locomotion", "state_time > 0.35 or speed > 1.5", 0.25)

	# The upper body follows the mods' stances (and the pistol's shots).
	var upper := AnimationNodeStateMachine.new()
	upper.add_node("Rest", _clip("Idle"), Vector2(300, 100))
	upper.add_node("Pistol", _clip("Pistol_Idle"), Vector2(550, 0))
	upper.add_node("Shoot", _clip("Pistol_Shoot"), Vector2(800, 0))
	upper.add_node("Ready", _clip("Sword_Idle"), Vector2(550, 220))
	upper.add_node("Swing", _clip("Sword_Attack"), Vector2(800, 220))
	_go(upper, "Start", "Rest")
	_go(upper, "Rest", "Pistol", "pistol", 0.15)
	_go(upper, "Rest", "Ready", "melee", 0.15)
	_go(upper, "Rest", "Swing", "melee_swing", 0.05)
	_go(upper, "Pistol", "Shoot", "pistol.fired", 0.05)
	_go(upper, "Shoot", "Pistol", "", 0.15, true)
	_go(upper, "Pistol", "Ready", "melee", 0.15)
	_go(upper, "Pistol", "Rest", "not pistol", 0.15)
	_go(upper, "Shoot", "Rest", "not pistol", 0.15)
	_go(upper, "Ready", "Swing", "melee_swing", 0.05)
	_go(upper, "Ready", "Pistol", "pistol", 0.15)
	_go(upper, "Ready", "Rest", "not melee and not melee_swing", 0.15)
	_go(upper, "Swing", "Ready", "not melee_swing and melee", 0.25)
	_go(upper, "Swing", "Rest", "not melee_swing and not melee", 0.25)

	var blend := AnimationNodeBlend2.new()
	blend.filter_enabled = true
	var spine := _skeleton.find_bone("Spine")
	for bone in range(_skeleton.get_bone_count()):
		var b := bone
		while b >= 0 and b != spine:
			b = _skeleton.get_bone_parent(b)
		if b == spine:
			blend.set_filter_path(NodePath("Armature/Skeleton3D:" + _skeleton.get_bone_name(bone)), true)

	var root := AnimationNodeBlendTree.new()
	root.add_node("Base", base, Vector2(0, 0))
	root.add_node("Upper", upper, Vector2(0, 200))
	root.add_node("UpperBlend", blend, Vector2(250, 100))
	root.connect_node("UpperBlend", 0, "Base")
	root.connect_node("UpperBlend", 1, "Upper")
	root.connect_node("output", 0, "UpperBlend")

	var tree := AnimationTree.new()
	tree.name = "AnimationTree"
	tree.tree_root = root
	_root.add_child(tree)
	tree.owner = _root
	tree.root_node = NodePath("../Model")
	# Tracks with one-shot properties (a particle's "emitting"): set them when keys pass, not every frame.
	tree.callback_mode_discrete = AnimationMixer.ANIMATION_CALLBACK_MODE_DISCRETE_DOMINANT
	tree.anim_player = tree.get_path_to(player)
	_root.animation_tree_path = _root.get_path_to(tree)
	# What drives the tree's numbers, as the simulation computes them.
	_root.graph_inputs = {
		"Base/Locomotion/blend_position": "move_right, move_forward" if _pack == "source" else "forward_speed",
		"UpperBlend/blend_amount": "pistol or melee or melee_swing",
	}


func _clip(animation: String, backward := false) -> AnimationNodeAnimation:
	var node := AnimationNodeAnimation.new()
	node.animation = animation
	if backward:
		node.play_mode = AnimationNodeAnimation.PLAY_MODE_BACKWARD
	return node


# A transition taken by itself once its condition holds: a single name is Godot's advance
# condition, anything longer its advance expression.
func _go(machine: AnimationNodeStateMachine, from: String, to: String, when := "", xfade := 0.0, at_end := false) -> void:
	var t := AnimationNodeStateMachineTransition.new()
	t.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	if when.contains(" "):
		t.advance_expression = when
	elif when != "":
		t.advance_condition = when
	t.xfade_time = xfade
	if at_end:
		t.switch_mode = AnimationNodeStateMachineTransition.SWITCH_MODE_AT_END
	machine.add_transition(from, to, t)


# --- The swing: a marker for the server ------------------------------------------------------------

# Sword_Attack is saved to its own file by the glb's import settings (Save to File, Keep Custom
# Tracks), which is how an imported animation becomes editable. A track added to it survives
# reimports; a reimport in Godot 4.7.1 dropped the marker, so this puts it back when it is missing.
func _mark_swing() -> void:
	var swing := load(SWING) as Animation
	if swing == null:
		printerr("no ", SWING, ": the glb's import must save Sword_Attack to that file")
		return
	if not swing.has_marker("melee.strike"):
		swing.add_marker("melee.strike", STRIKE_TIME)
	var old := swing.find_track(NodePath(OLD_FIRE_PATH + ":emitting"), Animation.TYPE_VALUE)
	if old >= 0:
		swing.remove_track(old)
	# The held item swings with it: its own "slash" (a bat's trail, a sword's glint), whatever the
	# mod put in the hand. A Godot Animation Playback track on the socket's item.
	var held := swing.find_track(NodePath(HELD_ITEM_PLAYER), Animation.TYPE_ANIMATION)
	if held < 0:
		held = swing.add_track(Animation.TYPE_ANIMATION)
		swing.track_set_path(held, NodePath(HELD_ITEM_PLAYER))
		swing.animation_track_insert_key(held, SLASH_TIME, "slash")
	if ResourceSaver.save(swing, SWING) != OK:
		printerr("cannot save ", SWING)


# --- Sockets ----------------------------------------------------------------------------------------

# RightHand and LeftHand, where held items go. The game would make them at the hands anyway; here
# they are real nodes, so the swing's track can reach what the right hand holds, and an author can
# move them. The frame is the one items are made in: the grip on the palm, the item along -Z (the
# way the hand points it), which is the placeholder rig's hand frame turned onto this skeleton.
func _add_sockets() -> void:
	var grip := Transform3D(Basis.from_euler(Vector3(deg_to_rad(-90), deg_to_rad(180), 0)), Vector3(0, -0.06, 0))
	for hand in ["RightHand", "LeftHand"]:
		var bone := _skeleton.find_bone(hand)
		var finger := _skeleton.find_bone(hand.replace("Hand", "MiddleProximal"))
		var rest := _skeleton.get_bone_global_rest(bone)
		var along := (_skeleton.get_bone_global_rest(finger).origin - rest.origin).normalized()
		var frame := Basis(rest.basis.orthonormalized().inverse() * Basis(Quaternion(Vector3(0, -1, 0), along)))
		var attachment := _skeleton.get_node_or_null("At_" + hand) as BoneAttachment3D
		if attachment == null:
			attachment = BoneAttachment3D.new()
			attachment.name = "At_" + hand
			attachment.bone_name = hand
			_skeleton.add_child(attachment)
			attachment.owner = _root
			attachment.transform = rest
		var socket := CbSocket.new()
		socket.name = hand
		socket.transform = Transform3D(frame, Vector3.ZERO) * grip
		attachment.add_child(socket)
		socket.owner = _root


# --- Hitboxes ----------------------------------------------------------------------------------------

func _add_hitboxes() -> void:
	for spec in CAPSULES:
		var bone := _skeleton.find_bone(spec[1])
		var children := _skeleton.get_bone_children(bone)
		# Along the bone to its (first) child: the profile points +Y from a bone to its child.
		var length: float = _skeleton.get_bone_rest(children[0]).origin.length() if children.size() > 0 else 0.2
		var capsule := CapsuleShape3D.new()
		capsule.radius = spec[2]
		capsule.height = length + 2.0 * spec[2]
		_hitbox(spec[0], spec[1], capsule, Vector3(0, length * 0.5, 0))
	var head := SphereShape3D.new()
	head.radius = 0.12
	_hitbox("head", "Head", head, Vector3(0, 0.09, 0.02))
	var hips := BoxShape3D.new()
	hips.size = Vector3(0.34, 0.2, 0.24)
	_hitbox("torso", "Hips", hips, Vector3(0, 0.03, 0))


func _hitbox(zone: String, bone: String, shape: Shape3D, offset: Vector3) -> void:
	var attachment := _skeleton.get_node_or_null("At_" + bone) as BoneAttachment3D
	if attachment == null:
		attachment = BoneAttachment3D.new()
		attachment.name = "At_" + bone
		attachment.bone_name = bone
		_skeleton.add_child(attachment)
		attachment.owner = _root
		attachment.transform = _skeleton.get_bone_global_rest(_skeleton.find_bone(bone))
	var node := CbHitbox.new()
	node.name = "Hitbox_" + zone
	node.zone = zone
	node.shape = shape
	node.position = offset
	attachment.add_child(node)
	node.owner = _root
