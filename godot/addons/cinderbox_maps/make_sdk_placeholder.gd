extends SceneTree
## Makes the SDK's placeholder from the game's mannequin (CC0): a skeleton with no model, and its
## locomotion clips as files.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_sdk_placeholder.gd
##
##   sdk/placeholder/skeleton.tscn    a Skeleton3D with the humanoid profile's bone names, at rest
##   sdk/placeholder/clips/<clip>.res  Idle, Walk, Jog_Fwd, Sprint, Jump_Start, Jump, Jump_Land
##
## A pack's scene instances the skeleton (the editor draws its bones) and plays clips on it; the
## bake records it as the skeleton the clips were made on. The clips address it as %Skeleton3D, as
## Godot's retargeting import writes them. Run this again only to change what the SDK ships.

const SOURCE := "res://characters/mannequin/source/UAL1_Standard.glb"
const CLIPS := ["Idle", "Walk", "Jog_Fwd", "Sprint", "Jump_Start", "Jump", "Jump_Land"]
const LOOPS := ["Idle", "Walk", "Jog_Fwd", "Sprint", "Jump"]


func _find(node: Node, type: String) -> Node:
	if node.is_class(type):
		return node
	for child in node.get_children():
		var found := _find(child, type)
		if found != null:
			return found
	return null


func _initialize() -> void:
	var out := ProjectSettings.globalize_path("res://").path_join("../sdk/placeholder").simplify_path()
	DirAccess.make_dir_recursive_absolute(out.path_join("clips"))
	var model: Node = (load(SOURCE) as PackedScene).instantiate()
	var source := _find(model, "Skeleton3D") as Skeleton3D
	var player := _find(model, "AnimationPlayer") as AnimationPlayer

	# The bones alone: names, parents, rests. No mesh, no skin.
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	skeleton.motion_scale = source.motion_scale
	for bone in source.get_bone_count():
		skeleton.add_bone(source.get_bone_name(bone))
	for bone in source.get_bone_count():
		skeleton.set_bone_parent(bone, source.get_bone_parent(bone))
		skeleton.set_bone_rest(bone, source.get_bone_rest(bone))
	skeleton.reset_bone_poses()
	var packed := PackedScene.new()
	var failed := packed.pack(skeleton) != OK or ResourceSaver.save(packed, out.path_join("skeleton.tscn")) != OK
	print("skeleton.tscn: %d bones%s" % [skeleton.get_bone_count(), " NOT saved" if failed else ""])

	for clip in CLIPS:
		var animation := player.get_animation(clip).duplicate(true) as Animation
		animation.loop_mode = Animation.LOOP_LINEAR if clip in LOOPS else Animation.LOOP_NONE
		var kept := 0
		for track in range(animation.get_track_count() - 1, -1, -1):
			var path := String(animation.track_get_path(track))
			var colon := path.find(":")
			var bone := path.substr(colon + 1) if colon >= 0 else ""
			if skeleton.find_bone(bone) < 0:
				animation.remove_track(track)
				continue
			animation.track_set_path(track, NodePath("%Skeleton3D:" + bone))
			kept += 1
		var saved := ResourceSaver.save(animation, out.path_join("clips").path_join(clip + ".res"))
		failed = failed or saved != OK
		print("clips/%s.res: %d tracks%s" % [clip, kept, "" if saved == OK else " NOT saved"])
	skeleton.free()
	model.free()
	quit(1 if failed else 0)
