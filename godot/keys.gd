extends RefCounted
## The keys: the game's own (game_*), the server's mods' actions (cb_<action>), the slots' (1 to 9,
## 0) and the help line that says them. All are InputMap actions, so the Keys page of the settings
## rebinds them like any other.
##
## The server's mods declare their actions and suggest a key for each. They become InputMap actions
## named cb_<action>, so the usual Godot input remapping works on them too.

const ACTION_PREFIX := "cb_"

## The game's own keys: InputMap actions like the mods' (game_*), so the Keys page rebinds them too.
const GAME_KEYS := [
	["game_forward", "W", "Forward"], ["game_back", "S", "Back"], ["game_left", "A", "Left"], ["game_right", "D", "Right"],
	["game_jump", "Space", "Jump"], ["game_sprint", "Shift", "Sprint"], ["game_use", "MouseLeft", "Use what is in the hand"],
	["game_first_person", "QuoteLeft", "First person"], ["game_shoulder", "Z", "Switch shoulder"],
]

var game: Node
## [{ name, bit, key }] from the server's mods.
var actions: Array = []
## The local player's slots, as their keys are bound (game_slot_1 ...): how many.
var slot_keys := 0


func _init(owner: Node) -> void:
	game = owner


func bind_game_keys() -> void:
	for key in GAME_KEYS:
		game.menu.bind_key(key[0], key[1], key[2], "Game")


func bind_actions() -> void:
	# Only the last server's: a HUD's own keys (CbKey) have the prefix too, and stay. The key is
	# the one the mod suggests, or the player's own (the Keys page of the settings).
	game.menu.forget_keys("This server")
	slot_keys = 0 # (the slots' keys are bound again once the player has slots)
	actions = game.client.get_actions()
	for action in actions:
		game.menu.bind_key(ACTION_PREFIX + String(action["name"]), String(action["key"]), action_label(String(action["name"])).capitalize(), "This server")
	print("mod actions: ", ", ".join(actions.map(func(a): return "%s (%s)" % [a["name"], game.menu.key_text(ACTION_PREFIX + String(a["name"]))])))
	update_help()


## The slots' keys, once the player has slots (and again when their number changes): 1 to 9 and 0
## for the first ten, as in Minecraft; the player's own on the Keys page wins. A slot is a place:
## its key selects it, whatever it holds. (The bag's slots past the tenth are reached with the
## mouse, on the inventory screen. An item's own key, "H" for a grenade, is a mod action.)
func bind_slot_keys() -> void:
	var count: int = game.client.get_slot_count(game.client.get_local_net_id()) if game._joined else 0
	if count == slot_keys or count <= 0:
		return
	game.menu.forget_keys("Slots")
	for i in mini(count, 10):
		game.menu.bind_key("game_slot_%d" % (i + 1), str(i + 1) if i < 9 else "0", "Slot %d" % (i + 1), "Slots")
	slot_keys = count
	var bound := []
	for i in mini(count, 10):
		bound.append(game.menu.key_text("game_slot_%d" % (i + 1)))
	print("slot keys: ", " ".join(bound))
	update_help()


## The server's actions held now, a bit each.
func action_bits() -> int:
	var bits := 0
	for action in actions:
		if Input.is_action_pressed(ACTION_PREFIX + String(action["name"])):
			bits |= 1 << int(action["bit"])
	return bits


## The bit of one of the server's actions (0: the server has no such action).
func action_bit(name: String) -> int:
	for action in actions:
		if action["name"] == name:
			return 1 << int(action["bit"])
	return 0


## The slots' keys held now, a bit each (slot 1: bit 0).
func slot_key_bits() -> int:
	var bits := 0
	for i in mini(slot_keys, 16):
		var slot_action := "game_slot_%d" % (i + 1)
		if InputMap.has_action(slot_action) and Input.is_action_pressed(slot_action):
			bits |= 1 << i
	return bits


## What a mod action is called on the Keys page and in the help line: an item's own key
## ("frag.grenade.use") by its item ("frag grenade").
func action_label(name: String) -> String:
	return name.trim_suffix(".use").replace(".", " ").replace("_", " ")


func update_help() -> void:
	if game.hud == null:
		return
	var help := game.hud.get_node_or_null("%Help") as Label
	if help == null:
		return
	if game._replay != "":
		help.text = "Space pause   Left/Right -/+5 s   Up/Down speed   , . step   Home restart   N next player   Mouse orbit   Wheel zoom   ` first person   Z shoulder   Esc menu   F1 stats"
		return
	# (The keys as they are bound now: the player's own, where one was chosen.)
	var menu = game.menu
	var text := "%s%s%s%s move   %s sprint   %s jump" % [menu.key_text("game_forward"), menu.key_text("game_left"), menu.key_text("game_back"),
		menu.key_text("game_right"), menu.key_text("game_sprint"), menu.key_text("game_jump")]
	for action in actions:
		text += "   %s %s" % [menu.key_text(ACTION_PREFIX + String(action["name"])), action_label(String(action["name"]))]
	# The HUDs' own keys (the inventory, the scores, the chat), once their screens are there.
	var server := actions.map(func(a): return ACTION_PREFIX + String(a["name"]))
	for action in menu.screen_keys():
		if not server.has(action):
			text += "   %s %s" % [menu.key_text(action), String(action).trim_prefix(ACTION_PREFIX).replace("_", " ")]
	help.text = text + "   Mouse orbit   Wheel zoom   %s first person   %s shoulder   Esc menu   F1 stats" % [menu.key_text("game_first_person"),
		menu.key_text("game_shoulder")]
