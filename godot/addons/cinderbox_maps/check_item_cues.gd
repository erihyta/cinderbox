extends SceneTree
## Checks that an item's own reactions answer a cue at its holder only while the item is in use:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_item_cues.gd
##
## A player holds one item and has another put away; both scenes have a reaction on the same cue
## ("item.use", subject ^^). Only the one in the hand plays. A reaction of an item about the item
## itself (subject ^) is not silenced, and a world reaction hears the cue as ever.

var _failed := 0
var _world: CbDirector
var _player: Node3D
var _held: Node3D
var _stowed: Node3D


func _check(what: String, ok: bool) -> void:
	print("%s %s" % ["ok  " if ok else "FAIL", what])
	if not ok:
		_failed += 1


func _item(world: CbDirector, socket: Node3D, kind: String, in_use: bool, id: int) -> Node3D:
	var item := Node3D.new()
	item.name = "Item"
	socket.add_child(item)
	world.add_entity(item, "item", kind, id)
	world.set_state(item, {"in_use": in_use, "hot": true})
	var target := Node3D.new()
	target.name = "Target"
	item.add_child(target)
	var on_use := CbReaction.new()
	on_use.name = "OnUse"
	on_use.event = "item.use"
	on_use.subject = NodePath("^^")
	on_use.target = NodePath("../Target")
	on_use.method = "hide"
	item.add_child(on_use)
	var own := CbReaction.new()
	own.name = "Own"
	own.event = "item.heated"
	own.target = NodePath("../Target")
	own.method = "hide"
	item.add_child(own)
	return item


func _initialize() -> void:
	var world := CbDirector.new()
	world.name = "World"
	world.auto_update = false
	get_root().add_child(world)
	var player := Node3D.new()
	player.name = "player_0"
	world.add_child(player)
	world.add_entity(player, "player", "", 1)
	world.set_state(player, {})
	var hand := Node3D.new()
	hand.name = "RightHand"
	player.add_child(hand)
	var hip := Node3D.new()
	hip.name = "Hip"
	player.add_child(hip)
	var held := _item(world, hand, "test.rifle", true, 2)
	var stowed := _item(world, hip, "test.pistol", false, 3)
	var heard := CbReaction.new()
	heard.name = "Heard"
	heard.event = "item.use"
	heard.subject = NodePath("$at")
	heard.target = NodePath("$at")
	heard.method = "show"
	world.add_child(heard)
	_world = world
	_player = player
	_held = held
	_stowed = stowed


## (Once the tree runs: nodes added before that have not entered it, and no reaction is known.)
func _process(_delta: float) -> bool:
	var world := _world
	var player := _player
	var held := _held
	var stowed := _stowed
	var why: Dictionary = world.explain("item.use", player, null, {})
	var of_held := ""
	var of_stowed := ""
	var of_world := ""
	for path in why:
		if String(path).contains("RightHand"):
			of_held = why[path]
		elif String(path).contains("Hip"):
			of_stowed = why[path]
		elif String(path).ends_with("Heard"):
			of_world = why[path]
	print("  in the hand: %s\n  put away:    %s\n  the world's: %s" % [of_held, of_stowed, of_world])
	_check("the item in the hand answers its holder's cue", not of_held.contains("put away") and of_held != "")
	_check("the item put away does not", of_stowed.contains("put away"))
	_check("a world reaction hears it as ever", not of_world.contains("put away") and of_world != "")
	world.cue("item.use", player, null, {})
	_check("... and played: the held item's target is hidden, the other's is not",
		not (held.get_node("Target") as Node3D).visible and (stowed.get_node("Target") as Node3D).visible)
	# About the item itself (subject ^, the default): heard put away too.
	var own: Dictionary = world.explain("item.heated", stowed, null, {})
	var said := ""
	for path in own:
		if String(path).contains("Hip"):
			said = own[path]
	_check("a put-away item's reaction about itself is not silenced", not said.contains("put away") and said != "")
	# Taken out: it answers.
	world.set_state(stowed, {"in_use": true})
	why = world.explain("item.use", player, null, {})
	for path in why:
		if String(path).contains("Hip"):
			of_stowed = why[path]
	_check("taken out, it answers", not of_stowed.contains("put away"))
	print("%d failed" % _failed)
	quit(1 if _failed > 0 else 0)
	return true
