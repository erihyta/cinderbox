extends SceneTree
## Checks that CinderboxSkeleton retargets onto a humanoid skeleton that is not shaped like ours:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_retarget.gd
##
## It builds a Skeleton3D with Godot's SkeletonProfileHumanoid bone names in a T-pose rest, with
## long legs and short arms, then poses it from the simulation's animation state. What matters is
## that the character keeps its own proportions and its arms come down out of the T-pose, because
## that is what fails when a rig is driven by global poses instead of retargeted.

const SCALE_LEGS := 1.35
const SCALE_ARMS := 0.7

var skeleton: Skeleton3D
var rig
var holder: Node3D
var failures := 0
var step := 0


func _initialize() -> void:
	holder = Node3D.new()
	get_root().add_child(holder)

	skeleton = Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	holder.add_child(skeleton)
	_build_humanoid()

	rig = ClassDB.instantiate("CinderboxSkeleton")
	rig.name = "Rig"
	rig.draw_bone_boxes = false
	holder.add_child(rig)
	rig.skeleton_path = NodePath("../Skeleton3D")


## A T-posed humanoid: arms along X, legs down, with proportions unlike the placeholder rig's.
func _build_humanoid() -> void:
	var bones := [
		["Hips", "", Vector3(0, 0.95 * SCALE_LEGS, 0)],
		["Spine", "Hips", Vector3(0, 0.10, 0)],
		["Chest", "Spine", Vector3(0, 0.12, 0)],
		["UpperChest", "Chest", Vector3(0, 0.12, 0)],
		["Neck", "UpperChest", Vector3(0, 0.14, 0)],
		["Head", "Neck", Vector3(0, 0.08, 0)],
		["LeftShoulder", "UpperChest", Vector3(0.06, 0.10, 0)],
		["LeftUpperArm", "LeftShoulder", Vector3(0.12, 0, 0)],
		["LeftLowerArm", "LeftUpperArm", Vector3(0.27 * SCALE_ARMS, 0, 0)],
		["LeftHand", "LeftLowerArm", Vector3(0.25 * SCALE_ARMS, 0, 0)],
		["RightShoulder", "UpperChest", Vector3(-0.06, 0.10, 0)],
		["RightUpperArm", "RightShoulder", Vector3(-0.12, 0, 0)],
		["RightLowerArm", "RightUpperArm", Vector3(-0.27 * SCALE_ARMS, 0, 0)],
		["RightHand", "RightLowerArm", Vector3(-0.25 * SCALE_ARMS, 0, 0)],
		["LeftUpperLeg", "Hips", Vector3(0.10, -0.05, 0)],
		["LeftLowerLeg", "LeftUpperLeg", Vector3(0, -0.42 * SCALE_LEGS, 0)],
		["LeftFoot", "LeftLowerLeg", Vector3(0, -0.42 * SCALE_LEGS, 0)],
		["LeftToes", "LeftFoot", Vector3(0, -0.06, 0.12)],
		["RightUpperLeg", "Hips", Vector3(-0.10, -0.05, 0)],
		["RightLowerLeg", "RightUpperLeg", Vector3(0, -0.42 * SCALE_LEGS, 0)],
		["RightFoot", "RightLowerLeg", Vector3(0, -0.42 * SCALE_LEGS, 0)],
		["RightToes", "RightFoot", Vector3(0, -0.06, 0.12)],
	]
	for bone in bones:
		var index := skeleton.add_bone(bone[0])
		if bone[1] != "":
			skeleton.set_bone_parent(index, skeleton.find_bone(bone[1]))
		skeleton.set_bone_rest(index, Transform3D(Basis(), bone[2]))
	skeleton.reset_bone_poses()


func _bone_length(child: String) -> float:
	var index := skeleton.find_bone(child)
	var parent := skeleton.get_bone_parent(index)
	return skeleton.get_bone_global_pose(index).origin.distance_to(skeleton.get_bone_global_pose(parent).origin)


func _rest_length(child: String) -> float:
	var index := skeleton.find_bone(child)
	var parent := skeleton.get_bone_parent(index)
	return skeleton.get_bone_global_rest(index).origin.distance_to(skeleton.get_bone_global_rest(parent).origin)


func _check(label: String, ok: bool, detail: String) -> void:
	if not ok:
		failures += 1
	print("  %-34s %s  %s" % [label, "ok" if ok else "FAILED", detail])


func _process(_delta: float) -> bool:
	if step == 0:
		# Standing still.
		rig.apply_anim_state(0, 1.0, 0.0, 0.0)
		var shoulder := skeleton.get_bone_global_pose(skeleton.find_bone("LeftShoulder")).origin
		var hand := skeleton.get_bone_global_pose(skeleton.find_bone("LeftHand")).origin
		var rest_hand := skeleton.get_bone_global_rest(skeleton.find_bone("LeftHand")).origin
		_check("arms come down out of the T-pose", hand.y < shoulder.y - 0.2,
			"hand y %.3f vs shoulder y %.3f (rest hand y %.3f)" % [hand.y, shoulder.y, rest_hand.y])

		for bone in ["LeftLowerArm", "LeftHand", "LeftLowerLeg", "LeftFoot"]:
			var posed := _bone_length(bone)
			var rest := _rest_length(bone)
			_check("%s keeps its length" % bone, abs(posed - rest) < 0.001,
				"posed %.3f, rest %.3f" % [posed, rest])

		var hips := skeleton.get_bone_global_pose(skeleton.find_bone("Hips")).origin
		_check("hips scale with the character", hips.y > 0.95 * 1.1,
			"hips y %.3f for a %.2fx character" % [hips.y, SCALE_LEGS])
		step += 1
		return false

	if step == 1:
		# Mid-stride: the legs must actually swing apart.
		rig.apply_anim_state(0, 2.0, 0.25, 6.5)
		var left := skeleton.get_bone_global_pose(skeleton.find_bone("LeftFoot")).origin
		var right := skeleton.get_bone_global_pose(skeleton.find_bone("RightFoot")).origin
		_check("running swings the legs apart", abs(left.z - right.z) > 0.2,
			"left foot z %.3f, right foot z %.3f" % [left.z, right.z])
		step += 1
		return false

	if failures == 0:
		print("retargeting holds on a differently proportioned humanoid")
	else:
		printerr(failures, " checks failed")
	holder.free()
	quit(2 if failures > 0 else 0)
	return true
