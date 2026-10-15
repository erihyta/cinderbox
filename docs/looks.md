# Looks

What players see and hear, as data: reactions, predictions, HUD nodes, and the packs that ship them. Part of the [manual](../README.md#the-manual).

## Effects

What plays when is data, not code: **world reactions**. A mod ships `res://vfx/reactions_<name>.tscn`,
a scene of `CbReaction` nodes, and the client loads every `res://vfx/reactions*.tscn` once. They are
the same node as the reactions inside entity scenes ([Reactions](#reactions)), with no entity of
their own: each names what it is about from the cue (`$at`, `$other`). Files add to each
other, so two mods add effects without fighting over one list; a mod replaces
`vfx/reactions.tscn` to change the game's own.

```
PistolReactions                      (prefabs/pistol.tscn)
├── Fired         on item.use     subject $at                         muzzle flash + gunshot at $at/RightHand
├── Kick          on item.use     subject $at      is_local           camera shake
├── Tracer        on item.use     subject $at                         beam from $at/RightHand to the cue's end
└── Hurt          on item.hit       subject $other   is_local           camera shake + red flash
```

What the pistol itself looks like is its own scene, whose root is a [`CbItem`](items.md#making-an-item). A `CbLinkLook` in a reactions scene says what the line of a [motion's probe](motions.md#the-rope-cblinklook) (a grappling hook's rope) is drawn as.

| Events | Carry |
|---|---|
| a mod's, by name (`item.hit`) | A (who it is about), B (the other one), `event.value`, the point and the end (where a shot ended) |
| `spawned`, `destroying` | the entity; its position (`spawned` only for entities that appear in play, not a world reset) |
| `jumped`, `landed`, `footstep` | the player; at its feet |
| `impact` | both bodies; `event.strength` (approach speed, m/s) |

What a world reaction adds to the [reaction fields](#reactions):

| Field | Meaning |
|---|---|
| `subject_kind`, `subject_template` | only for a player / prop / static / ragdoll / item, or entities from one map template |
| `cooldown` | shortest gap between two firings (twenty props landing at once play one sound) |
| `place`, `place_node`, `offset` | where its scene and sound go: under its parent, at the cue's **point** or **end**, a **beam** from `place_node` (or the point) to the end, **at** `place_node` (a socket: `$at/RightHand`, `$at/Head`), or **following** the subject |
| `sound`, `volume_db`, `pitch_scale`, `pitch_jitter`, `bus`, `max_distance` | a sound, once per firing |
| `shake`, `shake_time`, `flash_color`, `flash_time` | camera shake and a full-screen flash: the viewer's, so pair them with `is_local` or subject `local` |
| a `scene` a reaction adds | is an effect: nothing in it casts a shadow, whatever its scene says. Its lights light the world as usual |
| a reaction inside an item's scene whose subject is the holder (`^^`) | hears only while the item is in use. Put away (on the hip, on the back), it is silent: every item can answer the same cue (`item.use`) and only the one in the hand does. A reaction about the item itself (subject `^`, the default) is heard wherever the item is |
| a copy of an item lying in the world | is its look, its body and its reactions about itself (subject `^`): what is about a holder (a reaction whose subject starts with `^^`, a prediction, the motion nodes) is taken out while nobody has it, and put back from its scene when somebody picks it up. Twenty items on the floor cost their meshes, not their reactions |

Footsteps and impacts come from the simulation, not from the renderer guessing:
- A **footstep** is a stride, counted by distance walked, so the rate follows the speed on its own.
- An **impact** is a collision the physics engine reported above 1.5 m/s, carrying where it
  happened, both entities and how fast they were approaching. Two reactions with different
  `event.strength` conditions give a soft hit and a hard one different effects.

Both are part of the simulation's state, so they are identical on every machine, survive rollback,
and a client that skipped frames still sees them.

## Predictions

A mod's rules run on the server, so what your own press did comes back a round trip later. A
**`CbPrediction`** node in the look says what the server is going to answer, and the viewer shows
it at once:

```
PredictFire   CbPrediction   action "use"    cue "item.use"                (in prefabs/pistol.tscn, under its CbItem)
                             conditions gun.ammo > 0, !gun.reloading, !combat.dead
                             cooldown 0.19
                             changes    gun.ammo -= 1

PredictSwing  CbPrediction   action "use"    cue "melee.swing"   cooldown 0.58       (in prefabs/bat.tscn)
                             stance "melee_swing" on stance_layer "full"

PredictFire   CbPrediction   action "use"    cue "item.use"         (the rifle: automatic)
                             conditions gun.ammo > 0, !gun.reloading, !combat.dead
                             cooldown 0.1   while_held
                             changes    gun.ammo -= 1
```

| Step | What happens |
|---|---|
| You press the use button (`use`) and the conditions hold on your player | `item.use` plays for you now, with the same reactions everyone else's shot plays: one reaction per cue, none written twice |
| The prediction has `changes` | your own player's fields read the changed value at once: the ammo count drops on the click |
| The prediction has a `stance`, or your character's state machine reads the cue | your own upper body is shown ahead of the server: the swing or the recoil starts on the click |
| A reaction has `wait_for_server` on | it waits for the server's cue anyway |
| A reaction needs what only the server knows | it waits: placed at the cue's point, end or beam, a path starting with `$other`, or a condition on `event.*` (the tracer, the hit spark) |
| The server's `item.use` for you arrives (within a second) | it is the echo: what already played stays quiet, what waited plays now |
| No press was predicted | the server's cue plays in full, as for any other player |

| Field | Meaning |
|---|---|
| `action` | the action whose press is predicted (one a server mod declares) |
| `cue` | the cue the server sends for it: the same name, so the same reactions |
| `conditions` | the look's copy of the server's rule, read on your own player: [expressions](#expressions) |
| `cooldown` | the server's own rate (seconds between two predictions) |
| `while_held` | the action works for as long as it is held (automatic fire): the cue is predicted on the press and again every `cooldown` while it stays down and the conditions hold. Needs a `cooldown`: the time between two of the server's cues |
| `changes` | what the server's answer will change on your player: `gun.ammo -= 1`, `x += 2`, `x = 0`. Shown until the server's cue comes (its own value is in the same frame) or the prediction expires |
| `stance`, `stance_layer` | the stance the server's mod will set, and the layer it sets it on (`melee_swing` on `full`) |

- **Both halves are the modder's**: the server mod emits the cue, its look predicts it by name.
- **Where a prediction sits says whose it is.**

  | It sits | It speaks for | Its conditions |
  |---|---|---|
  | in the item's own scene, under its `CbItem` (the pistol's, the bat's) | the copy of that item the viewer has in the hand; not one that is put away, lying, or someone else's | need not name the item: `gun.ammo > 0`, not `gun.pistol, gun.ammo > 0` |
  | in a reactions scene, under the World | the viewer, whatever it holds | say when: `dash.charges > 0` |

  So an item's scene carries what using it looks like, and a hundred pistols in the world are one prediction. Reactions already work this way inside an item (`subject` `^^`: whoever holds it).
- **`use` is the engine's button** (the left mouse button): a prediction names it like an action a mod declared.
- **Held fire is two lines**: the server mod asks `ctx.Using( slot, kind )` where a press-based one
  asks `ctx.Used`, and the look's prediction is `while_held` with the mod's time between shots
  as its `cooldown`. The predictions are counted from the press, not from the frames, so they keep
  the server's rate at any frame rate (`gun.cpp`, `prefabs/rifle.tscn`).
- **A wrong guess**: a reaction that played is not taken back; changed fields and the led body
  go back to the server's when the prediction expires (a second). Keep the conditions as close to
  the server's rule as the board allows.
- **How the body is led**: the viewer runs your character's state machine forward from the state
  the server sent, with the predicted stance or event put in at the moment of the press, for the
  layers above the base (the legs follow movement, which is predicted already). When the server's
  answer arrives nothing jumps; afterwards the lead is given back slowly (the upper body plays 15%
  slower until it is level again). At most half a second ahead.
- **Looks only**: nothing is sent anywhere, and no rule runs on the client.
- **Not for movement**: a predicted cue cannot move the player. What a mod adds to movement is a
  [motion](motions.md), which the viewer's own simulation runs; the events it emits need no prediction.
- `CbDirector.explain_press( "use" )` says which predictions a press would make, or why not;
  `check_predictions.gd` drives one by hand.
- With 50 ms of delay each way, a click shows its shot 2 ms later and the ammo count at the next
  frame; the server's cue (the tracer) follows at about 250 ms. A swing's hand moves 70 ms after
  the click and the bat's flames light at 110 ms; the server's swing comes at about 220 ms.

### Example: a second action

The pistol's right mouse button casts a ray and puts a zone around the player it finds for 2
seconds. The whole feature is one function on the server and four nodes in the look:

| Where | What | Why there |
|---|---|---|
| `mark.cpp`, `Declare` | `declare.Action( "use2", "MouseRight" )`, events `item.use2` and `mark.found` | names the look can use; the key shows in the controls hint by itself |
| `mark.cpp` | on a press (once a second): `ctx.CastAim` (at what is under the crosshair, from the eye); `Emit( item.use2, caster, hit, eye, end )`; if it found a living player, `Emit( mark.found, caster, player )` | who is hit is the server's decision |
| look: `PredictScan` | `CbPrediction`: `use2` -> `item.use2` | your own click is heard at once |
| look: `Scan` | on `item.use2`, subject `$at`: a click at `$at/RightHand` | plays on the press for you, on the server's cue for everyone else |
| look: `ScanBeam` | on `item.use2`: `vfx/scan_beam.tscn` as a **beam** to the cue's end | needs the server's end point, so it waits for the server |
| look: `Marked` | on `mark.found`, subject `$other`: `vfx/mark_zone.tscn`, place **Follow the subject**, `scene_lifetime = 2` | the zone is a child of the marked player for 2 seconds, then freed |

The server keeps nothing about the zone: how it looks and how long it shows is the look's. If the
mark had to *do* something for those 2 seconds (slow the player), that would be a board field the
server sets and clears, and the zone a **While** reaction on it. Behind 50 ms each way: the click
at 1 ms, the server's events at about 160 ms, the zone on for 1.97 s.

## Expressions

Conditions and values are written in **one expression language**, the same text wherever it is
read: a reaction, a prediction, a HUD node, a character's state machine.

| Write | Means |
|---|---|
| `name` / `!name` (or `not name`) | the field is not zero / is zero |
| `?name` | the server declared the field (its mod is running); `!?name`: it did not (hide the pistol's scoreboard when deathmatch shows its own) |
| `gun.pistol` (an item kind) | the player holds one, in any socket: what a look should ask, not which loadout slot is out |
| `carried.gun.pistol` | the player has one, in the hand or put away |
| `using.gun.pistol` | the player uses one this tick (the button down with it in the hand, or its own key) |
| `hand.gun.rounds` | that field of the item in the player's hand (0 with none): `{hand.gun.rounds}` in a label |
| `motion.Hook`, `motion.grapple.hook.Use` | that motion of the player's is on (the node's name alone when it is the only one of that name) |
| `name == 2`, `!=`, `>`, `>=`, `<`, `<=` | a comparison: 1 or 0 (`true` / `false` are 1 / 0) |
| `+ - * /`, `-x`, `( )` | arithmetic; dividing by 0 gives 0 |
| `and` / `&&`, `or` / `\|\|` | both, either |
| a field against a field | `combat.health <= combat.max_health / 4` |

- **Precedence**, loosest first: `or`, `and`, `not`, comparisons, `+ -`, `* /`, unary `-`. So
  `!a == 2` is `not (a == 2)`, and `a or b and c` is `a or (b and c)`.
- **Truth**: anything that is not 0. A name nobody declared reads as 0, so looks for a mod that is
  not running never match. Text that does not parse is false, and the editor says why.
- **Who reads what**:

| Reader | Plain names are | Also |
|---|---|---|
| `CbReaction`, `CbPrediction` | the subject's state, then the world's | `is_local`, `first_person` (the viewer's own player, while the viewer looks out of its eyes), `event.value`, `event.strength`; a path and a colon reads another entity: `^^:combat.dead`, `$other:combat.health < 20` |
| HUD nodes | the local player's fields (private ones too), then the world's | item kinds |
| [State machines](characters.md#state-machines) | simulation values, stances, events, fields, item kinds | resolved once at bake; nothing private |

A list of conditions (`conditions`) holds when all of them do: `["gun.pistol", "!combat.dead"]`
is `gun.pistol and !combat.dead`.

What an entity looks like *while* something holds (a glowing bat) is authored inside its own scene,
with the same [`CbReaction` nodes](#reactions).

## Names

A look is written against names a server mod declares: `combat.health`, `melee.hit`, `dash`. They
are typed by hand, and one typed wrong used to read as 0 and say nothing. The editor knows them now.

| What | How |
|---|---|
| Where the names come from | every build of the server writes what every mod compiled into it declares to `cinderbox_names.cfg` in each project that authors a look (the game, the SDK, every mod's `client/`). By hand: `cb_server --dump-names FILE` |
| Choosing a name | a property that holds one name offers the ones there are: a reaction's `event`, a motion's `action` / `event` / `emits`, a launch's `item_kind`, a list's `event` / `item_kind`, a prediction's `action` / `cue`. Anything else can still be typed |
| A name nobody declares | the node says so in the scene tree (the yellow warning), with the closest name there is: `conditions: no mod declares "combat.ded" (did you mean combat.dead?)`. Checked: names, expressions, formats (`{...}`), `changes`, a click's `sets` |
| Publishing | `tools\publish_mod.ps1` checks every scene of the project the same way, and a name nobody declares stops it. By hand: `godot --headless --path <project> --script <repo>/godot/addons/cinderbox_maps/check_names.gd` |

- **Effects cast no shadows unless they say so**: a particle node's Cast Shadow is on by default in Godot, and a muzzle flash then throws square shadows on the ground. The shipped effects have it off (`cast_shadow = 0`); set it on your own.
- **A one-shot particle node is started with `restart`** (a `CbReaction` with `method = restart`, `delay` for the moment in the move). Switching `emitting` on from an animation does nothing while the last burst is still out, so quick repeats show none.
- **What a shot shows comes from the item**: a `Node3D` at the muzzle in the item's scene, with the flash and tracer reactions under it (`subject` `^^`, the tracer's `place_node` `..`). In first person it is then at the muzzle of the view model too.
- **What is not a mod's to declare is left alone**: the reader's own names (`speed`, `grounded`, `is_local`, `{name}`, `{rank}`), `ui.` values, `event.` and `slot.` names, the engine's cues (`footstep`, `impact`, `jumped`, ...), `held.jump`, and `{key:...}` (it may be a `CbKey` of the look's own).
- **Every compiled mod counts**, the ones a server would not run by default too: a look may be written for any of them.
- **Without the file nothing is offered and nothing is said**: a look can be written with no server at hand. The file is not shipped in a pack and not in the repository.
- For tools: `CbNames` (`is_known()`, `get_names( kind )`, `check_scene( root )`).

## HUD nodes

The HUD reads the board too, through script-free nodes any HUD scene can use:

| Node | Does |
|---|---|
| `CbFieldLabel` | a Label with a `text_format` (`"AMMO {gun.ammo} / {gun.magazine}"`), shown while its `conditions` hold. `{an expression}` works too: `"{combat.health * 100 / combat.max_health}%"`. With `choice_field` and `choices` it shows the line a number picks ([Saying things](#saying-things)) |
| `CbFieldBinding` | writes a field, or an expression over fields (`combat.health / combat.max_health`), into any property of its `target` (default: its parent), `value = field * multiply + add`; with conditions it hides the target while they fail. A `ProgressBar`'s `value` and `max_value`, a panel's `visible`, a colour. The property may be a part of one (`scale:x`, `modulate:a`). With a `text_format` it writes words instead (into `text`, or the property named): any node that shows text, a `Label3D` in the world too, can show the game's |
| `CbList` | a row per player, item, slot, or event that happened: its first child is the row as you designed it, copied for every entry; `where` filters, `sort_by` orders, `max_rows` cuts ([Lists](#lists)). Of **Events** (`event` `combat.killed`, each row for `seconds`) it is a kill feed: `{a}` and `{b}` in a row are what the event's two entities are called |
| `CbKey` | a key of the viewer's own, not one of the server's actions: `action` `scores`, `key` `Tab`. While it is held (or switched by each press) a value of the viewer's is 1: `ui.scores`. What that shows is whoever reads it: a `CbFieldBinding` with the condition `ui.scores` shows its target. With `cursor`, the mouse is the screen's meanwhile |
| `CbClick` | what a click on its parent does: sets values of the viewer's own (`ui.picked = slot.number`), asks something of the player's slots (select, move, drop) or of a mod (`mod_intent`) ([Screens](#screens)) |

Whose fields a HUD node reads is its **subject**: the local player, or inside a `CbList` row, that row's entity.

In formats, `{field}` is the subject's field, `{name}` what it is called (a player's name, an item's display name), `{rank}` its row's place in its list, `{name:field}` the
name of the player a field points at (`"{name:deathmatch.winner} WINS"`), `{look:field}` what the
entity a field points at is called (an item's `display_name`: `"Bat"`), and `{key:action}` the key
the player has that action bound to now (`"E"`, `"LMB"`: rebinding shows).

### Saying things

The words are the look's. The server says what is true, as a number or an event; the scene has the
words for it, with their font, place, sound and language. So a message can be reworded, restyled or
translated by a client mod, and the server never sends a string.

| The server says | The scene | Good for |
|---|---|---|
| a field is in a state (`deathmatch.phase == 1`) | a label or a whole panel with `conditions` | what holds for a while: a banner, a warning, a mode |
| one of several (`deathmatch.ending = 2`) | one `CbFieldLabel` with `choice_field` and a line per number in `choices` | a message picked from a list |
| something happened (`deathmatch.round_end`) | a `CbReaction` on the event: a scene for a few seconds, an animation, a sound | a moment: a kill, a capture, a round ending |

```
Remark   CbFieldLabel   conditions: deathmatch.phase == 1          (server_mods/deathmatch/client/ui/hud_deathmatch.tscn)
                        choice_field: deathmatch.ending
                        choices: "", "Time ran out", "A flawless round", "By a single point", "A clear win"
```

```cpp
// server_mods/deathmatch/deathmatch.cpp: which one, not what it says.
ctx.Set( 0, m_ending, second == 0 ? Flawless : lead <= 1 ? SinglePoint : ClearWin );
```

- **Line 0 is "nothing"**: fields start at 0, so leave the first line empty. An empty line, or a number past the last, hides the label.
- **A line is a format**: `"{name:deathmatch.winner} takes it"` works in a choice.
- **`choice_field` can be an expression**: `combat.health < 25` picks line 1 when it holds, line 0 when not.
- **`{choice}`** in `text_format` is the picked line, when there is more around it: `"-- {choice} --"`.
- **Text nobody could write beforehand** (what a player types, a host's message of the day) is not covered: nothing in the game makes such text yet. Player names are the one case, and have their own path.

### Lists

The server knows who is in the world and what its mods say about each of them; a `CbList` shows
that as rows, with no code. The scoreboard is one:

```
Scores        VBoxContainer                               (server_mods/deathmatch/client/ui/hud_deathmatch.tscn)
├── Key       CbKey          action "scores"  key "Tab"   ui.scores is 1 while it is held
├── Shown     CbFieldBinding conditions ui.scores          shows Scores while that holds
├── Header    HBoxContainer  plain Labels: Player, Score, Kills, Deaths
└── Rows      CbList         of Players   sort_by deathmatch.score
    └── Row   HBoxContainer  one row, as designed: copied for every player
        ├── Name       CbFieldLabel  "{name}"               conditions: not is_local
        ├── NameLocal  CbFieldLabel  "{name}" in gold       conditions: is_local
        ├── Score      CbFieldLabel  "{deathmatch.score}"
        └── ...
```

| Field | Meaning |
|---|---|
| `of` | **Players**; **Items** (every item in the world); **Items its subject holds** (the local player's, or under a player's row, that player's); **Slots of its subject** (a row per slot, empty ones too: `slot.number`, `slot.selected`, `slot.empty` are the row's) |
| `item_kind` | for items: only this kind (`gun.pistol`) |
| `where` | conditions on an entry: `team.id == 1`, `not combat.dead`, `not is_local` |
| `sort_by`, `descending` | an expression over an entry's fields; ties keep their order |
| `max_rows` | the first so many, after sorting (a top three) |
| `conditions` | on the list's own subject: whether it is shown at all |
| `vertical` | Godot's own: rows down, or across (a strip of icons) |

- **A row is any Control**: labels, a bar with a `CbFieldBinding`, an icon. Every HUD node in it reads that row's entity.
- **`is_local`** in a row's conditions is true on the viewer's own row.
- **A list in a row** lists for that row's entity: each player's row can show what that player carries.
- **What it cannot do yet**: text the server made up (fields are numbers), a row per value of a list on one entity, a grid whose columns size to their content (give cells a `custom_minimum_size`).

### Screens

A screen the player clicks on (an inventory, a shop, a team picker) is a scene too. Three things
make it one, and none is a script:

| Piece | What |
|---|---|
| A free cursor | a `CbKey` with `cursor`: while its value is 1 the mouse is the screen's, and the camera, the use button and the mods' actions do not hear it |
| Values of the viewer's own | names that start with `ui.` (`ui.picked`, `ui.tab`): no server and no simulation knows them. Conditions, formats (`{ui.picked}`) and bindings read them like fields; 0 until set. They are what a screen remembers |
| Clicks | a `CbClick` under any control: `conditions` (when it takes the click), `sets` (`ui.` values it changes), `intent` (what it asks of the player's slots), `mod_intent` with `intent_a`, `intent_b` (what it asks of a mod: `shop.buy`, offer 2; see [server-mods.md](server-mods.md#intents-a-mod-declares)) |
| Drags | a `CbDrag` under a slot's control: drag it onto another slot (move, merge), Shift-drag (half a stack), let go outside `inside` (drop). Godot's own drag and drop: a copy of the slot follows the cursor |
| Icons | a `CbItemIcon` (a `TextureRect`) in a row: the row's item's icon (`CbItem.icon`, or a picture the game takes of its scene) |
| Grids | `CbList.columns`: rows of that many, stacked (a `CbFieldBinding` can bind it to a field: `inventory.columns`) |
| Containers | `CbList` of "Slots of the container open to its subject": a chest's slots (rows name `slot.container`); `CbKey.opens`: turning it on opens the container the player looks at, off closes it; `container_open` in conditions while one is open |

The inventory screen (`server_mods/inventory/client/ui/hud_inventory.tscn`), on I:

```
Bag            PanelContainer
├── Key        CbKey       action "inventory"  key "I"  each press switches it  cursor   (ui.inventory)
├── Shown      CbFieldBinding   conditions ui.inventory: shows Bag
└── Rows       VBoxContainer
    ├── Title  Label
    └── Grid   CbList      of Slots
        ├── Slot     PanelContainer      one slot, as designed: copied for every slot, in lines of columns
        │   ├── Icon     CbItemIcon     the item's icon
        │   ├── Chosen   Panel          a frame (Shown: conditions slot.selected)
        │   ├── Number   CbFieldLabel   "{slot.number}", top left
        │   ├── Count    CbFieldLabel   "{slot.count}", bottom right, conditions slot.count > 1
        │   ├── Tip      CbFieldBinding "{name}" into tooltip_text
        │   └── Drag     CbDrag         inside Bag
        └── Columns  CbFieldBinding    inventory.columns into columns
```

- **An intent is the player's own input**: the move is carried out by every simulation, the player's own at once ([The inventory](items.md#the-inventory)).
- **The hotbar** is the same slot in a `CbList` across the bottom, its `max_rows` bound to `inventory.columns`: the first row.
- **The chest**: under the player's slots, a `Chest` part (a `CbList` of the open container's slots, the same cell) shown while `container_open`; the key opens the chest looked at.
- **What is not there yet**: an intent a mod declares ("buy item 3").

`CbPromptLabel` is the same in the world: a `Label3D` with a `text_format`, upright above its parent,
facing the camera, the same size at any distance, hidden while its text is empty; with a
`progress_field` it draws a bar under the text that fills as the field goes from 0 to 1. A reaction
puts it where it belongs (see [Items in the world](items.md#items-in-the-world)).

The pistol's HUD (`server_mods/gun/client/ui/hud_pistol.tscn`) is built from these: a health bar
(`ProgressBar` from `combat.health` and `combat.max_health`), ammo, reloading, crosshair, kills and
deaths, the kill feed and the scoreboard. None of it is script, so a client mod can restyle all of it.

`godot/vfx/reactions.tscn` is the game's own set; the pistol's look (predicted shots, tracers, hits,
reload, the pistol item's look) is `prefabs/pistol.tscn` in its workshop item, and being hurt
or dying looks the same for every weapon (`vfx/reactions_combat.tscn`, the combat mod's item); `mods_src/example_neon/vfx/reactions_neon.tscn` shows a mod adding three more,
including its own sound. All are edited in the Godot editor, as scenes.

The sounds in `godot/assets/sfx/` are placeholders in the same spirit as the procedural rig: short,
synthetic, and meant to be replaced. `tools/make_sfx.py` regenerates them.

## Reactions

A `CbReaction` node makes a scene react to the game, with no code, the way a Roblox script uses
its hierarchy: `^^/RightHand/Item` is "the item in my holder's right hand". It is part of a
standalone Godot addon (`src/godot/cue`, godot-cpp only): a **`CbDirector`** runs the reactions
under it from **cues** ("melee.hit" at an entity) and **entity state** ("melee.hot" = true). In
the game the director is the client's `World` node, and the client is only an adapter that tells
it what the simulation shows.

```
World (CbDirector)
├── player_0                       an entity: state {combat.health, inventory.slot, ...}
│   ├── (the character's scene)
│   ├── RightHand, LeftHand, Head  sockets: children of the entity in the game, on every rig
│   │   └── Item                   the held item, an entity too: state {melee.hot}
│   │       ├── Barrel, Sparks
│   │       ├── GlowWhileHot       CbReaction  while melee.hot          set Barrel emission = 4
│   │       └── SparksOnHit        CbReaction  on melee.hit, subject ^^  Sparks.restart()
├── player_1 ...                   players by slot; everything else <kind>_<net id>
├── Map                            the map's own scene
└── PistolReactions ...            world reactions (vfx/reactions*.tscn)
```

Every node path in a reaction is a Godot `NodePath`; its first name may be an **anchor**:

| Path | Means |
|---|---|
| `Barrel`, `../Sparks` | an ordinary path from the reaction |
| `^` | my entity: the nearest entity at or above the reaction (the default subject) |
| `^^`, `^^^` | the entity above that one (a held item's holder), and so on |
| `$at`, `$other` | the entities the cue names: who it is about (the attacker), the other one (the victim) |
| `$local` | the local player |
| `$world` | the World node (its state is the global board) |
| `$local@pickup.target`, `@field` | the entity whose NetId is in that entity's state field (`@field`: my entity's) |
| `^^/RightHand/Item`, `$other/Head` | an anchor, then an ordinary path from it |

A path that finds nothing (an empty hand) makes the reaction do nothing, and no path leaves the
World node: a workshop item cannot reach the game's HUD or menus.

| Field | Meaning |
|---|---|
| `when` | **On a cue** (once per cue) or **While** (its conditions hold) |
| `event` | On a cue: its name (`melee.hit`, `footstep`: see [Effects](#effects)). Your own press shows at once through a [prediction](#predictions) |
| `subject` | a path (default `^`): whose state plain condition names read |
| `event_side` | On a cue: **A**, the cue is at the subject (`melee.hit` is at the attacker); **B**, the subject is the other one (the victim); or **Either**. A subject starting with `$at` / `$other` matches every cue of the name |
| `subject_kind`, `subject_template` | only for a player / prop / static / ragdoll / item, or one map template (for an item: its kind, `melee.bat`) |
| `conditions` | [expressions](#expressions), all of which must hold; also `is_local`, `event.value`, `event.strength`. Plain names read the subject's state, then the world's; a path and a colon read another's: `!^^:combat.dead`, `$other:combat.health < 20`, `$world:deathmatch.round` |
| `delay`, `chance`, `cooldown` | cue reactions: act N seconds later, only sometimes (0-1), and not more often than every N seconds |
| `wait_for_server` | cue reactions: do not act on a [predicted](#predictions) press, act when the server's cue comes |
| `animation_player`, `animation` | play this animation from the start; `animation_off` when a While ends (without one, the animation stops) |
| `target`, `property`, `value` | set a property on a node; a While puts the old value back when it ends. Sub-paths work: `surface_material_override/0:albedo_color` |
| `value_expression` | instead of `value`: the property becomes an [expression](#expressions)'s value (`combat.health / combat.max_health`, `event.strength * 0.1`). A While keeps it up to date while it is on; a bool property gets true / false, an int a whole number. Reading `event.*` waits for the server's cue |
| `blend_time` | fade the property there (and back) instead of snapping: numbers, vectors, colours |
| `target`, `method`, `method_args` | call a method: `restart`, `play` `["slash"]`, `set_visible` `[false]` |
| `scene`, `scene_parent`, `scene_lifetime` | add a scene (under the reaction's parent by default); a cue's goes after `scene_lifetime` s, a While's when it ends |
| `place`, `place_node`, `offset` | where the scene and sound go: under its parent, at the cue's **point** or **end**, a **beam** from `place_node` (or the point) to the end, **at** `place_node` (`$at/RightHand`), or **following** the subject |
| `sound`, `volume_db`, `pitch_scale`, `pitch_jitter`, `bus`, `max_distance` | a sound, once per firing |
| `volume_expression` | how loud this firing is, as a factor on `volume_db`: `event.strength / 4` (0 plays nothing, 1 is `volume_db`, at most 4) |
| `shake`, `shake_time`, `flash_color`, `flash_time` | camera shake and a full-screen flash (the director's `screen_effect` signal): the viewer's, so pair them with `is_local` |

- **Presentation only**: nothing here changes the simulation. Everything that exists in the game
  (an item, a prop) is spawned by a server mod.
- **State is visible**: every entity node carries its state as `state` metadata, so the Remote
  inspector shows `melee.hot` changing live.
- **Sockets are children of the entity** in the game, whatever bone they were authored under, so
  paths through them are the same on every character. The character's animation tracks that
  reached an item through a socket's authored place (the bat's `slash`) are pointed at the new
  place when the game loads them.
- **A While follows its target**: when its path finds another node (a new item in the hand), it
  ends on the old one (puts values back) and starts on the new one.
- **Only listed methods**: `restart`, `play`, `play_backwards`, `stop`, `pause`, `queue`, `seek`,
  `advance`, `show`, `hide`, `set_visible`, `set_emitting`, `set_text`, `set_value`, `set_frame`,
  `set_modulate`, `set_volume_db`, `set_pitch_scale`, `set_speed_scale`. Anything else is not called
  (set it with `property` instead); the `script` property and metadata are never set. A scene a
  reaction adds goes through the scene guard first. A path or condition that does not parse is a
  configuration warning on the node (`^^combat.health` asks for its colon), and the game skips that
  reaction with a warning.
- **Nothing is left behind**: a While that leaves the tree while on (an item put away, a world scene
  reloaded) puts back what it set and frees its scene.
- **Help in the editor**: every group in the inspector starts with an info line (click the icon for
  more), every property has a hover text, and F1 on `CbReaction` opens its class reference. Anchors
  are typed into a path field through its ⋮ menu, Edit.
- **Resources are shared** between instances of a scene. A reaction that changes a material changes
  every copy, unless the material is **Local to Scene** (the bat's barrel is).
- **Rollback**: like an animation's other tracks, a cue reaction that already played is not taken back if a
  prediction turns out wrong.
- **Anything can drive a director**: `add_entity( node, kind, template )`, `set_state( node, {...} )`,
  `set_world_state`, `set_local`, `cue( name, at, other, { value, strength, point, end } )`,
  `press( action )`.
  `check_reactions.gd` drives one by hand, with no server:
  `godot --headless --path godot --script res://addons/cinderbox_maps/check_reactions.gd`.

### Rules sheets

Many reactions are one rule each: a node, an inspector, a dozen values. A **rules sheet** is the
same reactions as lines of text, in a `.rules` file beside the scene; a `CbRules` node points to it
(`file`). Logic is text; what is a place or a thing (a mesh to light up, a socket) stays a node,
and a rule names it by its path.

```
# vfx/brawl.rules: a thud where each hit lands
on brawl.jab.hit
    subject $at
    at point
    sound res://assets/sfx/bat_hit.wav volume -6 jitter 0.1 range 40

while melee.hot
    set Barrel surface_material_override/0:emission_energy_multiplier 4 blend 0.2
```

| Line | Means |
|---|---|
| `on <event> [if <expression>]`, `while <expression>` | a rule starts: when, as a reaction's |
| `subject`, `side`, `kind`, `template`, `delay`, `chance`, `cooldown`, `wait`, `at ... [from] [offset]` | its settings, for every action under it |
| `sound`, `scene`, `play`, `set`, `call`, `shake`, `flash` | its actions: each becomes one `CbReaction` |
| `predict <action> as <cue> [if <expression>]`, then `cooldown`, `held`, `change`, `stance ... on ...` | a [prediction](#predictions): one `CbPrediction` |

```
# prefabs/pistol.rules
predict use as item.use if gun.ammo > 0 and !gun.reloading and !combat.dead
    cooldown 0.19
    change gun.ammo -= 1

on item.use
    subject ^^
    scene res://vfx/muzzle_flash.tscn under Muzzle life 0.3
    scene res://vfx/tracer.tscn at beam from Muzzle life 0.06
    sound res://assets/sfx/gunshot.wav at Muzzle volume -5 jitter 0.06 range 70
```

- **Every action is a `CbReaction`** under the `CbRules` node (never saved with the scene), so the
  director, the guard and the Cue Preview see nothing new.
- **Plain paths** are from the node `CbRules` is under, and climb no higher than the scene.
- **Mistakes say their line**: on the node in the editor, and at publish (a name no mod declares:
  `res://vfx/brawl.rules line 22: event: no mod declares "brawl.upercut.hit" (did you mean
  brawl.uppercut.hit?)`). A rule with one is left out; the rest works.
- **The editor opens `.rules`** in its script editor; packs ship them (`*.rules` in the export
  filter; the game's pack check allows them as data).
- **Every look of the game is written so** (M166): the game's own `vfx/reactions.rules`, the
  pistol's and the bat's sheets, and every mod's `reactions_<mod>.rules`: 48 nodes became 13 sheets.
- **A setting on an action's own line** (`at`, `delay`, `chance`, `cooldown`, `wait`) is that
  action's, over the rule's: one rule plays a flash, a tracer and a shot, each where it belongs.
  Not after an expression (`= ...`, `volume_by`), which runs to the end of the line.
  `python tools/merge_rules.py <sheet>` merges rules that start the same way into one so.
- **`under` attaches, `at` places**: `scene ... under Muzzle` rides on the muzzle; `at Muzzle` puts
  it there, in the world, once. With neither, it goes where the `CbRules` node is (a `Node3D`).
- **Converting a scene**: `python tools/reactions_to_rules.py <project> <scene>` writes its sheet
  and leaves one `CbRules` node (each rule keeps the node's name as a comment).
- `check_rules.gd` checks it: `godot --headless --path godot --script res://addons/cinderbox_maps/check_rules.gd`.

### Cue Preview (editor)

Every project with the Cinderbox extension has a **Cue Preview** bottom panel: it plays the edited
scene's reactions with no game running. The scene is copied onto a small stage (unsaved edits
included; the edited scene is never touched), next to two stand-in players with `RightHand`,
`LeftHand` and `Head` sockets:

| The scene is | It goes | Found by |
|---|---|---|
| a held item (the bat) | `player_0/RightHand/Item`, so `^^` is player_0 | default |
| a character | `player_0` itself | it has a `Skeleton3D` |
| world reactions (`vfx/reactions*.tscn`) | under the World | subjects start with `$`, nothing to draw |

The mode can be picked by hand, and **Reload** copies the scene again after edits.

- **Fire a cue**: its name (the scene's own cues are listed, and the game's), `$at` and `$other`
  (player_0, player_1, the item), `value` and `strength`. The cue's point is `$other`'s chest.
  Below, every reaction listening for it says what it did, or why not ("the subject (player_1) is
  not at ($at) in this cue", "condition \"melee.hot\" is false", "cooling down").
- **State**: every name the scene's conditions read gets a field, on player_0, player_1, the item
  or the world (0 is false). The bat: `melee.hot` = 1 lights the barrel, 0 puts it out.
- **local**: which stand-in is the viewer (`$local`, `is_local`).
- Screen effects flash and shake the preview; sounds play. Each control has an info icon.
- Like the game's other bottom panels, it only draws while it is showing (opening a character
  brings up the Animation panel instead).

## Client mods

Client mods are cosmetic Godot resource packs (`.zip`). A mod can replace or add:
- entity visuals in `prefabs/`;
- effects in `vfx/`, and world reactions as `vfx/reactions_<name>.tscn`;
- sounds and other shared files in `assets/`;
- the HUD in `ui/`;
- map visuals in `maps/` (the scene named after the map the server runs);

Client mods and workshop items cannot contain code, and a scene without code cannot act like code.
Two checks:

| When | What | Checked by |
|---|---|---|
| Before a pack loads | only known kinds of files in these folders (and Godot's converted copies of them), redirects that stay inside the pack, no compressed resources, no resource that names a script type or a script file | `boot.gd`; `check_mod_validator.gd` runs it on real packs and hostile ones |
| Before a scene is used | only listed node classes (meshes, particles, lights, sounds, animation, UI controls, the `Cb*` nodes: never an `HTTPRequest`, a `Window`, a camera), no script, no signal wired to a method, no node path that leaves the scene, animations that call only listed methods | the scene guard (`src/godot/cue/cue_guard.h`); `check_guard.gd` |

A scene the guard refuses is not instantiated (a warning says what it found); the game draws its
plain stand-in instead. Gameplay stays in the simulation and in the server's mods, so a pack cannot
change it. What no check can promise is that Godot's own parsers are safe against a deliberately
malformed file, so packs are still something to take from people you trust.

1. Create a Godot project under `mods_src/<name>`. Copy `mods_src/example_neon` as a starting point.
2. Put your files at the same paths the game uses, for example `vfx/prop_spawn.tscn`.
3. List them in the project's "Mod" export preset.
4. Run `tools\pack_mod.ps1 -Project mods_src\<name>` to create `mods\<name>.zip`.

The game loads packs from these places, in this order:
1. `<game folder>/mods`
2. `user://mods`
3. each `--mods=DIR`

Within a folder, packs load in alphabetical order, and a later pack overrides an earlier one.
[mods_src/README.md](../mods_src/README.md) lists the files the game looks up.
