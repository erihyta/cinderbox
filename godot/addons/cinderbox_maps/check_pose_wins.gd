extends SceneTree
## Checks that the ozz pose wins over Godot animation on a driven skeleton:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_pose_wins.gd
##
## A Skeleton3D with the placeholder rig's head chain is animated by an AnimationPlayer that
## swings the head around, and driven by a CinderboxSkeleton. After every skeleton update (when
## modifiers have run, which is what gets drawn) the head must be where the ozz pose puts it, not
## where the animation put it. Exit code 1 if not. With -- --no-modifier the pose modifier is switched
## off, and the check must fail (the animation wins): a test of the test.

const BONES := [
	["Hips", "", Vector3(0, 0.95, 0)],
	["Spine", "Hips", Vector3(0, 0.10, 0)],
	["Chest", "Spine", Vector3(0, 0.12, 0)],
	["UpperChest", "Chest", Vector3(0, 0.12, 0)],
	["Neck", "UpperChest", Vector3(0, 0.14, 0)],
	["Head", "Neck", Vector3(0, 0.08, 0)],
]

var _skeleton: Skeleton3D
var _driver: CinderboxSkeleton
var _frames := 0
var _checked := 0
var _worst := 0.0


func _initialize() -> void:
	var root := Node3D.new()
	_skeleton = Skeleton3D.new()
	_skeleton.name = "Skeleton3D"
	for bone in BONES:
		var index := _skeleton.add_bone(bone[0])
		if bone[1] != "":
			_skeleton.set_bone_parent(index, _skeleton.find_bone(bone[1]))
		_skeleton.set_bone_rest(index, Transform3D(Basis(), bone[2]))
	_skeleton.reset_bone_poses()
	root.add_child(_skeleton)

	# Godot animation fighting for the head: a fast, large swing.
	var animation := Animation.new()
	animation.length = 1.0
	animation.loop_mode = Animation.LOOP_LINEAR
	var track := animation.add_track(Animation.TYPE_ROTATION_3D)
	animation.track_set_path(track, NodePath("Skeleton3D:Head"))
	animation.rotation_track_insert_key(track, 0.0, Quaternion.from_euler(Vector3(1.2, 0, 0)))
	animation.rotation_track_insert_key(track, 0.5, Quaternion.from_euler(Vector3(-1.2, 0.8, 0)))
	animation.rotation_track_insert_key(track, 1.0, Quaternion.from_euler(Vector3(1.2, 0, 0)))
	var library := AnimationLibrary.new()
	library.add_animation("swing", animation)
	var player := AnimationPlayer.new()
	player.root_node = NodePath("..")
	root.add_child(player)
	player.add_animation_library("", library)

	_driver = CinderboxSkeleton.new()
	_driver.draw_bone_boxes = false
	_driver.retarget = false
	root.add_child(_driver)
	_driver.skeleton_path = _driver.get_path_to(_skeleton)

	get_root().add_child(root)
	player.play("swing")
	_skeleton.skeleton_updated.connect(_on_updated)


func _process(_delta: float) -> bool:
	_frames += 1
	# The simulation's state for this frame (the placeholder set, idle).
	_driver.apply_anim_state(0, float(_frames) / 60.0, 0.0, 0.0)
	if "--no-modifier" in OS.get_cmdline_user_args():
		for child in _skeleton.get_children(true):
			if child is CbPoseModifier:
				child.active = false
	if _frames < 30:
		return false
	# One modifier, however many frames the pose was applied in.
	var modifiers := 0
	for child in _skeleton.get_children(true):
		if child is CbPoseModifier:
			modifiers += 1
	var ok := _checked >= 10 and _worst < 0.001 and modifiers == 1
	if modifiers != 1:
		print("expected one CbPoseModifier, found %d" % modifiers)
	print("pose wins over animation: %s (%d updates checked, worst head offset %.4f m)" % ["ok" if ok else "FAIL", _checked, _worst])
	quit(0 if ok else 1)
	return true


func _on_updated() -> void:
	if _frames < 5:
		return
	var head := _skeleton.find_bone("Head")
	var drawn := _skeleton.get_bone_global_pose(head)
	var wanted: Transform3D = _driver.get_joint_global_transform("Head")
	# Compare the bone's Y axis: the animation tilts it, the idle pose keeps it nearly upright.
	var offset := (drawn.basis.y.normalized() - wanted.basis.y.normalized()).length()
	_worst = max(_worst, offset)
	_checked += 1
