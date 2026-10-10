extends SceneTree
## Puts the mannequin's weapon clips (CC0) into the mods that use them, as files on the SDK's
## placeholder skeleton, the way make_sdk_placeholder.gd makes the SDK's locomotion clips:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_weapon_clips.gd
##
##   server_mods/pistol/client/animation_packs/source/   Pistol_Idle.res, Pistol_Shoot.res
##   server_mods/melee/client/animation_packs/source/    Sword_Idle.res, Sword_Attack.res
##
## The packs that play them are scenes in those projects (animation_packs/*.tscn), edited in the
## editor. Run this again only to take the clips from the source again: it overwrites the files,
## and what was edited in them.

const SOURCE := "res://characters/mannequin/source/UAL1_Standard.glb"
const PLACEHOLDER := "../sdk/placeholder/skeleton.tscn"
# mod, clip, loops, markers ({name: seconds})
const CLIPS := [
	["pistol", "Pistol_Idle", true, {}],
	["pistol", "Pistol_Shoot", false, {}],
	["melee", "Sword_Idle", true, {}],
	# The frame the swing connects: the melee mod hits on it.
	["melee", "Sword_Attack", false, {"melee.strike": 0.4}],
]


func _find(node: Node, type: String) -> Node:
	if node.is_class(type):
		return node
	for child in node.get_children():
		var found := _find(child, type)
		if found != null:
			return found
	return null


func _initialize() -> void:
	var root := ProjectSettings.globalize_path("res://").path_join("..").simplify_path()
	var model: Node = (load(SOURCE) as PackedScene).instantiate()
	var skeleton := _find(model, "Skeleton3D") as Skeleton3D
	var player := _find(model, "AnimationPlayer") as AnimationPlayer
	var failed := false
	for entry in CLIPS:
		var out := root.path_join("server_mods/%s/client/animation_packs/source" % entry[0])
		DirAccess.make_dir_recursive_absolute(out)
		var animation := player.get_animation(entry[1]).duplicate(true) as Animation
		animation.loop_mode = Animation.LOOP_LINEAR if entry[2] else Animation.LOOP_NONE
		var kept := 0
		for track in range(animation.get_track_count() - 1, -1, -1):
			var path := String(animation.track_get_path(track))
			var colon := path.find(":")
			var bone := path.substr(colon + 1) if colon >= 0 else ""
			if skeleton.find_bone(bone) < 0:
				animation.remove_track(track)
				continue
			# As Godot's retargeting import writes them: the pack's skeleton by its unique name.
			animation.track_set_path(track, NodePath("%Skeleton3D:" + bone))
			kept += 1
		for marker in animation.get_marker_names():
			animation.remove_marker(marker)
		for marker in entry[3]:
			animation.add_marker(marker, entry[3][marker])
		var saved := ResourceSaver.save(animation, out.path_join(entry[1] + ".res"))
		failed = failed or saved != OK
		print("%s: %s.res, %d tracks%s" % [entry[0], entry[1], kept, "" if saved == OK else " NOT saved"])
	model.free()
	quit(1 if failed else 0)
