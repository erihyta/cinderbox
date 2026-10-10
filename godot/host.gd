extends RefCounted
## A server of the player's own, for trying things: the game starts cb_server, joins it, and stops
## it when it leaves. One step from an edit to playing it.
##
##   the menu's "Test locally" button, or on the command line (after `--`):
##   --host-local             start a server with every mod it runs by default, and join it
##   --host-local=dash,grenade   only these mods (cb_server --mods)
##   --bots=N                 N bots on it too (cb_bot), so a mod can be tried alone
##   --server-exe=PATH        which cb_server (default: the one next to the game, else the newest
##                            build of this checkout)
##   --server-arg=...         one more argument for the server (--server-arg=--map=maps/x.cbmap)
##
## The server is the real one, on this machine, on a port of its own: what is tried is what a
## player would get. It also stops by itself once nobody is connected for a while (cb_server
## --exit-when-empty), so one the game could not stop (a crash) does not stay.

const FIRST_PORT := 27700
const PORTS := 200
## How long the server waits with nobody connected before it stops by itself.
const EMPTY_SECONDS := 20

## These outlive the scene, which is reloaded when a server is left.
static var _server_pid := -1
static var _bot_pid := -1
static var port := 0


## cb_server, or "" when there is none to start.
static func find_server(args: Dictionary) -> String:
	var name := "cb_server.exe" if OS.get_name() == "Windows" else "cb_server"
	if args.has("server-exe"):
		return String(args["server-exe"]) if FileAccess.file_exists(args["server-exe"]) else ""
	# An exported game: the server ships next to it.
	var beside := OS.get_executable_path().get_base_dir().path_join(name)
	if FileAccess.file_exists(beside):
		return beside
	# A checkout: the newest of the builds (they are outside the source tree).
	var root := ""
	if OS.get_name() == "Windows":
		root = OS.get_environment("LOCALAPPDATA").path_join("cinderbox-build/cinderbox")
	else:
		root = OS.get_environment("HOME").path_join(".cache/cinderbox-build/cinderbox")
	var newest := ""
	var newest_time := 0
	if DirAccess.dir_exists_absolute(root):
		for preset in DirAccess.get_directories_at(root):
			var exe := root.path_join(preset).path_join("bin").path_join(name)
			if FileAccess.file_exists(exe) and FileAccess.get_modified_time(exe) > newest_time:
				newest_time = FileAccess.get_modified_time(exe)
				newest = exe
	return newest


## Starts a server (and bots). {"port": P} when it runs, {"error": why} when it does not.
static func start(args: Dictionary) -> Dictionary:
	stop()
	var exe := find_server(args)
	if exe == "":
		return {"error": "No cb_server to start: build it (cmake --build --preset clang-release), or pass --server-exe=PATH."}
	# A port nobody is likely to have: a different one each time, so a server that is still
	# closing is not in the way.
	port = FIRST_PORT + (randi() % PORTS)
	var server_args := PackedStringArray(["--port", str(port), "--quiet", "--exit-when-empty", str(EMPTY_SECONDS)])
	var mods := String(args.get("host-local", ""))
	if mods != "":
		server_args.append_array(PackedStringArray(["--mods", mods]))
	if args.has("server-arg"):
		for part in String(args["server-arg"]).split(" ", false):
			server_args.append(part)
	_server_pid = OS.create_process(exe, server_args)
	if _server_pid <= 0:
		_server_pid = -1
		return {"error": "Could not start %s." % exe}
	print("hosting: %s on port %d (pid %d)%s" % [exe, port, _server_pid, "" if mods == "" else ", mods " + mods])
	var bots := int(args.get("bots", "0"))
	if bots > 0:
		var bot_exe := exe.get_base_dir().path_join("cb_bot.exe" if OS.get_name() == "Windows" else "cb_bot")
		if FileAccess.file_exists(bot_exe):
			_bot_pid = OS.create_process(bot_exe, PackedStringArray(["--port", str(port), "--count", str(bots), "--duration", "86400", "--stagger", "400"]))
			print("hosting: %d bots (pid %d)" % [bots, _bot_pid])
	return {"port": port}


## True while the server this game started is still running.
static func running() -> bool:
	return _server_pid > 0 and OS.is_process_running(_server_pid)


## True when this game started a server (running or not).
static func hosting() -> bool:
	return _server_pid > 0


static func stop() -> void:
	if _bot_pid > 0:
		if OS.is_process_running(_bot_pid):
			OS.kill(_bot_pid)
		_bot_pid = -1
	if _server_pid > 0:
		if OS.is_process_running(_server_pid):
			OS.kill(_server_pid)
			print("hosting: stopped the server (pid %d)" % _server_pid)
		_server_pid = -1
