extends SceneTree
## Checks packs the way the game does before it loads them (boot.gd), run by tools/pack_mod.ps1 on
## what it just packed:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_pack.gd -- <pack.zip>...
##
## A pack with a script, a file outside the moddable folders, or anything else the game refuses
## fails here, when it is made, instead of when a player joins. Exit code 1 if a pack is refused.

const Boot := preload("res://boot.gd")


func _initialize() -> void:
	var refused := 0
	for path in OS.get_cmdline_user_args():
		var problem: String = Boot.check_mod(path)
		if problem != "":
			refused += 1
			printerr("the game would refuse %s: %s" % [path.get_file(), problem])
	quit(1 if refused > 0 else 0)
