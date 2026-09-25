extends SceneTree
## Builds the default player character from the Universal Animation Library's mannequin (Quaternius,
## CC0) and bakes it:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_mannequin.gd
##
## The source is characters/mannequin/source/UAL1_Standard.glb (the in-place version; _RM has root
## motion, which the simulation does not want). Its import retargets it onto Godot's
## SkeletonProfileHumanoid with source/bone_map.tres, so bones have the profile's names. This script
## puts it under a CbCharacter with a CinderboxSkeleton, hitboxes sized from the skeleton's own bone
## lengths, and the clips the game uses, saves characters/mannequin/character.tscn and bakes it.
## Everything here is what an author would do by hand in the editor; open the scene and edit freely.

const SOURCE := "res://characters/mannequin/source/UAL1_Standard.glb"
const OUT_DIR := "res://characters/mannequin"

# The six clips the simulation's states play, and the stance clips the shipped mods use.
const CLIPS := {
	"idle": "Idle", "walk": "Walk", "run": "Jog_Fwd",
	"jump_start": "Jump_Start", "fall": "Jump", "land": "Jump_Land",
}
const STANCES := {
	"pistol": "Pistol_Idle",	  # upper body, one loop
	"melee_idle": "Sword_Idle",	  # full body; walking with the bat uses the normal walk
	"melee_swing": "Sword_Attack",
}

# zone, bone, radius; capsules run along the bone to its child (the profile's +Y), spheres sit on it.
const CAPSULES := [
	["torso", "Spine", 0.17], ["torso", "Chest", 0.18], ["torso", "UpperChest", 0.17],
	["arm", "LeftUpperArm", 0.065], ["arm", "LeftLowerArm", 0.055], ["arm", "RightUpperArm", 0.065], ["arm", "RightLowerArm", 0.055],
	["leg", "LeftUpperLeg", 0.09], ["leg", "LeftLowerLeg", 0.07], ["leg", "RightUpperLeg", 0.09], ["leg", "RightLowerLeg", 0.07],
]

var _root: CbCharacter
var _skeleton: Skeleton3D


func _initialize() -> void:
	var source := load(SOURCE) as PackedScene
	if source == null:
		printerr("cannot load ", SOURCE, " (import the project first)")
		quit(2)
		return

	_root = CbCharacter.new()
	_root.name = "Mannequin"
	_root.character_name = "mannequin"
	var model := source.instantiate()
	model.name = "Model"
	_root.add_child(model)
	model.owner = _root
	_skeleton = model.get_node("Armature/Skeleton3D") as Skeleton3D
	_root.skeleton_path = _root.get_path_to(_skeleton)
	_root.animation_player_path = _root.get_path_to(model.get_node("AnimationPlayer"))
	for clip in CLIPS:
		_root.set("clip_" + clip, CLIPS[clip])
	_root.stance_clips = STANCES
	_root.masks = {"upper": "Spine"}

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

	var packed := PackedScene.new()
	if packed.pack(_root) != OK:
		printerr("cannot pack the character")
		quit(2)
		return
	var scene_path := OUT_DIR.path_join("character.tscn")
	if ResourceSaver.save(packed, scene_path) != OK:
		printerr("cannot save ", scene_path)
		quit(2)
		return
	print("saved ", scene_path)

	var result: Dictionary = _root.bake_to(OUT_DIR)
	_root.free()
	if not result["ok"]:
		printerr("bake failed: ", result["error"])
		quit(1)
		return
	print("baked %d joints, %d clips, %d stance clips, %d hitboxes, %d companion clips into %s" % [result["joints"],
		result["clips"], result["stance_clips"], result["hitboxes"], result["companion_clips"], OUT_DIR])
	if result["warnings"] != "":
		print("warnings: ", result["warnings"])
	quit(0)


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
