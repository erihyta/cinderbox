extends RefCounted
## Where subscribed workshop items live on this machine.
##
## A server announces the items its mods need (a mod name and the SHA-256 of the item's pack). It
## never sends them: players subscribe to items, and they end up here. For now "the workshop" is a
## folder, filled by tools/publish_mod.ps1; Steam Workshop can replace this class later, with the
## rest of the game unchanged.
##
## Layout: <workshop>/<mod>/<sha256>.zip, where <workshop> is user://workshop or --workshop=DIR.


static func folder() -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--workshop="):
			return arg.substr(11)
	return "user://workshop"


## The installed pack for this exact item, or "" when it is not subscribed (or not identical).
static func find_item(mod: String, sha256: String) -> String:
	var path := folder().path_join(mod).path_join(sha256 + ".zip")
	if not FileAccess.file_exists(path):
		return ""
	# The name is only a hint: the content has to be the item the server announced.
	if FileAccess.get_sha256(path) != sha256:
		push_warning("workshop item %s at %s does not match its hash" % [mod, path])
		return ""
	return path
