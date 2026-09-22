extends SceneTree
## Checks that a player prefab's AnimationTree follows the simulation's animation state:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_animtree.gd -- <pack.zip|res://path>
##
## Without an argument it checks the game's own res://prefabs/player.tscn. Any prefab that has a
## CinderboxAnimator can be checked this way, which is a quicker loop than joining a server.

var animator
var tree: AnimationTree
var node: Node3D
var failures := 0
var index := 0
var bailed := false

# mode, mode_time, locomotion_phase, ground_speed, expected state, expected blend
var cases := [
	[0, 0.0, 0.0, 0.0, "locomotion", 0.0],
	[0, 1.0, 0.3, 3.0, "locomotion", 3.0],
	[0, 2.0, 0.6, 6.5, "locomotion", 6.5],
	[1, 0.1, 0.0, 0.0, "jump", 0.0],
	[2, 0.4, 0.0, 0.0, "fall", 0.0],
	[3, 0.2, 0.0, 0.0, "land", 0.0],
	[0, 0.5, 0.2, 3.0, "locomotion", 3.0],
]


func _initialize() -> void:
	var target := "res://prefabs/player.tscn"
	var args := OS.get_cmdline_user_args()
	if args.size() > 0 and args[0] != "":
		if args[0].ends_with(".zip"):
			if not ProjectSettings.load_resource_pack(args[0], true):
				printerr("cannot load pack ", args[0])
				bailed = true
				quit(2)
				return
		else:
			target = args[0]

	var scene := load(target) as PackedScene
	if scene == null:
		printerr("cannot load ", target)
		bailed = true
		quit(2)
		return
	node = scene.instantiate()
	get_root().add_child(node)
	animator = node.find_children("*", "CinderboxAnimator", true, false).pop_front()
	if animator == null:
		printerr(target, " has no CinderboxAnimator (it is posed with ozz instead)")
		bailed = true
		quit(2)
		return
	tree = animator.get_node(animator.animation_tree_path)
	print("checking ", target)


func _process(_delta: float) -> bool:
	if bailed:
		return true
	if animator == null or index >= cases.size():
		if failures == 0:
			print("animation tree follows the simulation")
		else:
			printerr(failures, " mismatches")
		if node != null:
			node.free()
		quit(2 if failures > 0 else 0)
		return true

	var c = cases[index]
	# A few frames, because a transition blends rather than snapping.
	for i in 8:
		animator.apply_state(c[0], c[1], c[2], c[3])
	var state: String = animator.get_current_state()
	var blend: float = tree.get(animator.speed_parameter)
	var ok: bool = state == c[4] and abs(blend - c[5]) < 0.01
	if not ok:
		failures += 1
	print("  mode %d speed %.1f -> state '%s' blend %.2f  %s" % [c[0], c[3], state, blend, "ok" if ok else "MISMATCH"])
	index += 1
	return false
