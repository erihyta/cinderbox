# Items

Held items, sockets, the inventory, grips, and items lying in the world. Part of the [manual](../README.md#the-manual).

## Held items and sockets

A weapon, a torch, a shield: a **held item** is a simulation entity of its own (NetId, board,
events), held by a player in a **socket**. One small command changes its state; everything it
looks like is authored in Godot.

| Piece | Where | What |
|---|---|---|
| socket | the character scene: a `CbSocket` under a `BoneAttachment3D` | where items go, in the item's frame (grip at the origin, pointing along -Z); `RightHand` and `LeftHand` exist on every character (made at the hands if the scene has none) |
| item kind | the mod: `declare.ItemKind( "melee.bat" )` | spawned with `ctx.SpawnItem( SlotTarget( slot ), kind, socket )`, addressed with `ItemTarget( slot, socket )` for `Set`, `Emit`, `Destroy` |
| the item | a scene whose root is a `CbItem` (`prefabs/bat.tscn`): the kind, the name, the body, the grips, the look ([Making an item](#making-an-item)) | drawn as the socket's child `Item` |
| item state and events | `CbReaction` nodes in the item scene | flames on its holder's `melee.swing`, glow while `melee.hot`, sparks on its holder's `melee.hit` (see [Reactions](looks.md#reactions)) |

One attack, resolved by what is held, with no client code:

```
server:    Emit( attack, SlotTarget( slot ), value )          one small command
character: Idle -> Heavy      [attack == 2]                    priority 0
           Idle -> BatSwing   [attack and melee.bat]           priority 1
           Idle -> SwordSlash [attack and melee.sword]         priority 1
           Idle -> Punch      [attack]                         priority 2
item:      its own CbReaction on "attack" (subject: its holder), if it has one
```

The body's choice runs in the simulation, so the server's hit tests follow it; each item decides
what an attack looks like on it.

The bat: the melee mod spawns a `melee.bat` when the bat is taken out. Its look is its own, three
reactions in `prefabs/bat.tscn`:

| Reaction | On | Does |
|---|---|---|
| `FlamesOnSwing` | its holder's `melee.swing` (predicted for your own swing) | plays the bat's `slash` animation (flames along the barrel, on and off) |
| `GlowWhileHot` | while `melee.hot` (set by a hit) | the barrel glows |
| `SparksOnHit` | its holder's `melee.hit` | a burst of sparks |

Nothing in the character knows about the bat: any character swings any item, and an item with no
reaction on the swing is simply quiet. The pistol works the same way.

A mod taking its item away destroys the NetId `ctx.HeldItem( slot, socket )` gives, not
`ItemTarget`: another mod may put its item in that socket in the same tick (a weapon swap), and
`ItemTarget` would find that one. What is in the hand decides: the melee mod swings any `melee.bat`
in the right hand and the pistol fires any `pistol.gun`, wherever it came from. Looks follow the
same rule: the pistol's HUD and its predicted shot ask `pistol.gun` (true while the player has one
in use), never which slot is out, so a picked-up pistol shows its ammo and a holstered one does not.

## The inventory

What a player carries is in numbered **slots**. The slots are the engine's: they are simulation
state, and what a player does with them is in its own input, so every simulation carries it out
and the player's own screen does not wait for the server. What a life is given and loses is a mod's.

| | The engine (mechanism) | A mod (rules) |
|---|---|---|
| How many slots | up to 36 a player | `declare.Slots( 4 )` (the most any mod asks for) |
| Select, move, drop | from the player's input, predicted | |
| In the hand | the selected slot's item; the others are stowed in their holsters | |
| Where a kind goes | its own slot, or the first free one | the item's `slot` and `holster` properties |
| What a life starts with, what a death drops | | the `inventory` mod |

| The player | What happens, on that tick |
|---|---|
| A number key (1 to 9) | that slot is selected: its item comes into the right hand, the one that was there goes to its holster. Nothing is made, destroyed or dropped |
| The selected slot's key again | empty hands |
| Moves one slot onto another | the two trade what they hold (either may be empty). The selected slot stays selected, so the hand follows |
| Drops a slot | its item is thrown out in front; the slot is empty |

| The server | What happens |
|---|---|
| Gives an item (`ctx.GiveItem`) | into its kind's own slot, or the first free one; with none free it lies on the floor |
| A player picks one up (`ctx.PickUpItem`) | the same, and it is selected: it comes out. With no slot for it, it stays where it lies |
| A kind's own slot is taken | what is there moves to a free slot, or drops |

- **Using an item** is the engine's too: see [Using an item](#using-an-item).
- **It is state**: an item knows its slot (`HeldItem::slot`), a player which slot is selected (`Slots`). Hashed, rolled back, in snapshots and recordings.
- **An intent is carried out once.** The input carries the intent and a count; a repeated or guessed input has the same count and does nothing again.
- **A dead player's** intents are ignored.
- **Without a mod that asks for slots** there are none, and a mod holds items in sockets itself (`ctx.HoldItem`, `ctx.StowItem`).

### Using an item

How a kind is used is its look's to say: `use` on its `CbItem`.

| `use` | The player | For |
|---|---|---|
| **Select it, then the use button** | its slot's key takes it out; the left mouse button uses it | a tool, a weapon |
| **Its slot key uses it** | its slot's key uses it where it is; nothing changes hands | a consumable, an ability (the grappling hook) |

Either way the item's mod is told the same thing, on the tick it happens:

```cpp
if ( ctx.Used( slot, m_gun ) ) { ... }      // used this tick, whichever way the kind is used
bool trigger = ctx.Using( slot, m_gun );     // the use button is down with one in the hand (automatic fire)
```

- **The use button is the engine's**, like jump and sprint: no mod declares a `fire` action any more.
- **`<kind>.used`**: where a mod declares that event (`declare.Event( "pistol.gun.used" )`), every simulation records it on the tick of the use, at the player, with the item as the other entity. The player's own simulation has it at once, so a `CbReaction` on it plays on the click with no `CbPrediction`.
- **What using it does** is the mod's: the pistol fires (`pistol.fired`), and that answer still comes from the server, so its look predicts it: a `CbPrediction` (action `use`) in the item's own scene.
- A dead or frozen player uses nothing; a held button is one use.

An item is **in use** (in a hand) or **stowed** (carried, in no hand). A stowed item is in no hand:
`HeldItem`, item layers, state machine conditions and item-kind conditions in looks do not see it.
It keeps its entity, its board and its look, and drops like any other.

Mods describe their items with properties and never touch the slots:

```cpp
m_bat = declare.ItemKind( "melee.bat" );
declare.ItemProperty( m_bat, "slot", 2.0f );                        // the engine's: its own slot (from 1); without it: the first free one.
                                                                    // Better said on the item: CbItem.slot (and CbItem.use)
declare.ItemProperty( m_bat, "holster", declare.Socket( "Back" ) ); // the engine's, optional: where it hangs while another slot is selected
declare.ItemProperty( m_bat, "inventory.start", 1.0f );             // the inventory mod's: every life starts with one
```

| For a mod (`Context`) | |
|---|---|
| `SlotCount()` | how many slots players have (0: none) |
| `SelectedSlot( player )` | which one is selected (-1: empty hands) |
| `SlotItem( player, slot )` | the item in one (0: empty) |
| `CarriedItems( player )` | everything carried, each with its `slot` |

The `inventory` mod's rules:

| Situation | What happens |
|---|---|
| A life starts | every item kind with `inventory.start` is given |
| Death | what the life started with is taken back; anything else carried drops where the player stood. The next life starts with the slot that was selected |
| Empty hands | freelook again (a weapon turns camera-facing on when it comes out) |

The mod's look (`server_mods/inventory/client`, a workshop item like the others) is a row of slots
along the bottom of the screen, all data: `inventory.item_N` holds the NetId of slot N's item, a
label shows `{look:inventory.item_N}` (what that item is called), and the selected slot
(`inventory.slot == N`; 0: empty hands) is highlighted. The mod publishes those fields from the
engine's state, so the row follows the hands a moment later; a look that reads the slots themselves
is the next step ([roadmap](../ROADMAP.md#1a-items-and-the-inventory-in-the-engine)).

**Holsters are optional, twice over.** The mod chooses whether its item has one, and the character
chooses whether it has that socket: a `CbSocket` node named like it (`Back`, `Hip`) under a
`BoneAttachment3D`, moved in the editor like the hand sockets. Without either, a stowed item is
simply out of sight. The mannequin has both: the bat hangs across the back, the pistol on the right hip.

## Both hands on an item

Where the hands hold an item is said by the item: its `CbItem` names two markers of its scene
(ordinary `Marker3D` nodes).

| On the `CbItem` | Means | Without it |
|---|---|---|
| `carry_grip` | the item is carried at this marker: the point is in the hand's socket (whichever hand the item is in: a mod holds it in `RightHand` or `LeftHand`), the marker's -Z along the fingers, +Y up | the item is carried at its scene's origin |
| `other_hand` | what the other hand does while it is empty (below) | **Free**: the item is one-handed |
| `other_grip` | the marker the other hand is held at | |

| `other_hand` | The other hand |
|---|---|
| **Free** | does what the animation does |
| **At the marker** | its arm is bent so that the wrist is at `other_grip`, wherever the carrying hand and the animation take the item; it keeps the turn its animation gives it (the rifle) |
| **At the marker and turned with it** | and the palm is turned as the marker is, as a hand carrying an item placed there would be |
| **As the animations have it** | stays where the item's animations have it relative to the carrying hand, place and turn; no marker. For animations made with both hands on the item (the pistol's, the bat's): they are kept exactly as they are, and kept together when the carrying arm is aimed up or down, where a free hand lets the two drift a few centimetres apart |

Move a marker, not the model: a mesh imported with its origin anywhere is held where its carrying
marker is.

| Step | What |
|---|---|
| Bake | with the item's body, in `items/<kind>.cfg`: the body's centre and the other hand's `grip` are written in the carrying hand's frame, so the server and the pose never see the scene's own origin; the server puts the grip in the schema, so every client has it. The viewer draws the scene moved so that the carrying marker is in the socket (and, lying in the world, where the body is) |
| Pose | last of all: the item is where the carrying hand ended up (after the aim), and the other arm is bent at the elbow and turned at the shoulder so its wrist is on the grip. The elbow stays on the side the animation had it; out of reach, the arm goes as far as it can |
| When | while the other hand is empty. Two items, one in each hand, are each carried one-handed |

- It is part of the pose: other players see it, and the server's hit tests pose the same arms.
- Fingers are the animation's: the solve places the wrist and turns the hand, it does not close it.
- The markers are baked with the item (its `CbItem`); the body may be turned any way, but not scaled.
- `CbGrip`, the node that used to mark a hand, is kept but unused: it is for hand placements to come (a ledge, a wheel).
- `check_grips.gd` checks what scenes built in code bake to.
- **To turn an item in the hand, turn its carrying marker**, and nothing else: the body stays where
  the scene has it (the bake writes how it is turned in the carried frame, a `turn` line, and the
  simulation lays the body down that way). The scene root's own transform is not used by the game
  (the editor warns if it is not zero): the root is what the game places.
- **What an item shows is the item's**: a reaction inside its scene, under a node at the place
  (the rifle's `Muzzle`), with subject `^^` (whoever holds it). A scene it adds goes under that
  node (`place` Parent: the flash), a beam starts there (`place_node = ..`: the tracer). Move or
  turn the item, its marker or that node, and the effect goes with it; nothing reads the hand.
- **A holster is not a hand**: in any other socket (`Back`, `Hip`) the scene is drawn as it is,
  from its origin, so turning the carrying marker does not turn the item on the back. The bat is
  carried across the fingers (its marker is turned a quarter) and hangs along the back as before.
- **Where the palms are is the character's**: its `CbSocket` nodes named `RightHand` and `LeftHand`
  (under the hand bones' `BoneAttachment3D`). They are baked into the character's `anim.cfg`
  (`socket.RightHand = ...`), because the pose needs them: the item's frame is the carrying hand's
  socket, and an aligned grip puts the other hand's socket on it. A character without those nodes
  gets a built-in palm.

| You changed | To see it |
|---|---|
| a character's hand sockets (or anything else in its scene) | save the scene: it bakes on save. For a character in the game's own project that is all; restart the server and the game. A character that is a workshop item: `tools\publish_mod.ps1 -Character <name>` |
| an item's scene (its grips, its body) | `tools\publish_mod.ps1 -Mod <mod>` (it bakes and installs the item), then restart the server |
| which character you are looking at | the server says, and it says it when it starts (`character: ual_mannequin, shipped with the game`) and the game's debug text does too (`animation: res://characters/...`). On a machine that has `ual_mannequin` built that is the default, not `mannequin`: edit that one's scene, or start `cb_server --character mannequin` |
| (in a checkout) | the server reads the game's own characters from `godot/characters/`, where saving bakes them; a build elsewhere reads the copies next to it (`bin/characters/`, made when it is built) |

## Items in the world

An item can also **lie in the world**: the same entity (NetId, board, look) with a physics body,
so it falls, tumbles, gets shot across the floor, and does so identically on every screen.

| Mod API | Does |
|---|---|
| the `CollisionShape3D` under the item's `CbItem` | its body in the world: a box or a sphere, placed from the grip (the bat: a 0.82 m box whose centre is 0.31 m in front of it), with the item's `mass`. Authored with Godot's shape gizmo, baked to `items/<kind>.cfg` |
| `declare.ItemKind( "x", BoxItem( half, center, mass ) )` | the same from code, for a mod without a look (`SphereItem` too); a baked body replaces it |
| `ctx.SpawnWorldItem( kind, grip, rotation, velocity )` | one on the floor |
| `ctx.DropItem( item, grip, rotation, velocity )` | out of the hand; thrown if it has a velocity |
| `ctx.PickUpItem( SlotTarget( slot ), item, socket )` | into a free socket (drop what is there first, in the same tick) |
| `ctx.ItemsNear( point, radius )`, `ctx.Items()`, `ctx.ItemKindOf( id )`, `ctx.ItemHolder( id )` | what lies around, nearest first; every item; what and whose |
| `ctx.SpawnItem` into a taken socket | drops what was there (it may be one someone picked up) |

### Making an item

One scene is one item, and its root says so:

```
Bat          CbItem             kind "melee.bat"  display_name "Bat"  mass 1.1        (prefabs/bat.tscn)
│                               properties { pickup.hold_seconds: 0.5 }
├── Body     CollisionShape3D   a BoxShape3D: its body when it lies in the world
├── Carry    Marker3D           where the carrying hand holds it: the item's carry_grip
├── Other    Marker3D           for the other hand: the item's other_grip
├── Handle…  MeshInstance3D     how it looks
└── Tip      Node3D             a place on it, with the CbReaction nodes of what it shows
```

| On the `CbItem` | Meaning |
|---|---|
| `kind` | the kind a server mod declares (`declare.ItemKind( "melee.bat" )`). One scene per kind; the baked file is named after it |
| `display_name` | what prompts and lists call it (`{look:field}`, an item's `{name}`) |
| `mass` | kg, when it lies in the world |
| `properties` | named numbers any server mod may read, e.g. `pickup.hold_seconds` = 0.5. They replace what the item's mod declared in code for the same name |
| `carry_grip`, `other_hand`, `other_grip` | [where the hands hold it](#both-hands-on-an-item) |
| `use`, `slot` | [how it is used](#using-an-item), and the slot it goes to (1 is the first; 0: the first free one) |
| `view_offset` | first person: how far the arms holding it are moved in the viewer's own view |
| **Bake item** (button), saving the scene | writes `res://items/<kind>.cfg` |

| Under it | Meaning |
|---|---|
| a `CollisionShape3D` | the body: a `BoxShape3D` or `SphereShape3D`, moved to where the shape's centre is. Not scaled |
| two `Marker3D`s | [where the hands hold it](#both-hands-on-an-item): the item's `carry_grip` and `other_grip` name them |
| anything else | its look: meshes, lights, particles; `CbReaction` nodes for what it shows (`subject` `^^`: whoever holds it); `CbPrediction` nodes for what its own use looks like on the click, which speak only for the copy in the viewer's hand ([Predictions](looks.md#predictions)) |

The baked file has two readers. The **server** takes the body, the grip and the properties from
it; the **game** takes the scene, the name and the first-person view. Nothing else says what an
item looks like: there is no entry for it in a reactions scene.


Publishing the mod bakes every item too (`tools\publish_mod.ps1`, or by hand below) and ships the
files in the item; the server reads them from there (`item melee.bat: body from mod melee's item
(box, 1.10 kg)`, `item melee.bat: pickup.hold_seconds = 0.5`). The baked files are committed with
the mod, so servers and tests built from source have them. A change reaches servers when the mod is
published again.

```sh
godot --headless --path server_mods/melee/client --script <repo>/godot/addons/cinderbox_maps/bake_items.gd
```

Who may pick up what, and when, is a mod's. The **pickup** mod is the example:

- Near an item (1.5 m along the ground, not behind you), its NetId goes on your board as
  `pickup.target`. **E** takes it, **G** throws what you hold. With the `inventory` mod running, a
  taken item goes to its slot (see [The inventory](#the-inventory)); without it there is only the
  right hand: what was there drops, and dying drops it.
- **Hold to pick up**: an item may take a moment: `pickup.hold_seconds` in its `CbItem`'s
  `properties` (the bat: 0.5; the pistol is a tap), or `declare.ItemProperty( kind,
  "pickup.hold_seconds", 0.5f )` in its mod. `pickup.hold` says how long the item in reach needs,
  and while E is held on it `pickup.since` is the tick the hold began (0: none). Letting go or
  losing the item starts over. The board changes when a hold starts and ends, not every tick.
- Its look is a proximity prompt, all data: a world reaction while `$local`'s `pickup.target` is set
  puts a `CbPromptLabel` on the item it names, `$local@pickup.target`, and moves it when that
  changes: `"[{key:pickup}]  Pick up {look:pickup.target}"` for a tap, `"Hold [...]"` with a bar
  that fills (`since_field = "pickup.since"`, `duration_field = "pickup.hold"`: the label counts from
  the game's clock) for an item that needs holding.

```
PickupReactions            (vfx/reactions_pickup.tscn)
├── Prompt      while  $local: pickup.target, pickup.hold == 0   prompt.tscn        "[E]  Pick up Pistol"
└── PromptHold  while  $local: pickup.target, pickup.hold > 0    prompt_hold.tscn   "Hold [E]  Pick up Bat" + bar
                both under $local@pickup.target
```

**Item properties** are how mods agree on what an item is like without knowing each other: a named
number on an item kind (`declare.ItemProperty( kind, name, value )`) that any mod reads with
`ctx.ItemProperty( kind, name, fallback )`. The melee mod says how long its bat takes; the pickup
mod is the one that cares.

The same pieces make any prompt: a mod puts an entity's NetId in a field, its look shows a
`CbPromptLabel` on `$local@thatfield`.

The **expire** mod keeps the floor clean: an item someone held and then left lying is removed after
`expire.seconds`; picking it up stops the clock. Items nobody ever held (a map's own, the seeded
ones) stay.
