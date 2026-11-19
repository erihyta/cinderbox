# What Cinderbox can learn from Hytale's mods and plugins

An analysis written on 2026-11-11 for the project's owner, who had looked at how Hytale's mods
and server plugins work and asked what Cinderbox can learn from them. Cinderbox is not voxel
based; the comparison is of the modding model, not of the content.

| Read this as | |
|---|---|
| Status | written as proposals; **decided by the owner on 2026-11-17**: 1 and 7 before launch, 5 in roadmap step 6 and 3 with it, 2, 4 and 6 postponed and kept open ([ROADMAP.md](../ROADMAP.md#from-the-hytale-comparison)). How Hytale delivers assets was looked at again after M261.9's pack finding: [Asset delivery, looked at again](#asset-delivery-looked-at-again-2026-11-17) |
| The goal it serves | the "presentation client": one client that every server drives with data and that runs no mod's code ([DESIGN.md](../DESIGN.md): "Clients present, they do not decide") |
| Cinderbox's side | first written from the roadmap and the manual, then checked against the code at commit `cd6f9ce` (M261) on 2026-11-14. Three statements were wrong and are corrected: [Checked against the code](#checked-against-the-code) |
| Hytale's side | the official strategy post and patch notes, and community docs built on the decompiled server: [How sure this is](#how-sure-this-is) |
| The docs | lines of the manual that were behind the code were fixed on 2026-11-15: [Stale lines fixed in the docs](#stale-lines-fixed-in-the-docs) |

## In short

Cinderbox is already built on Hytale's model: rules on the server, one client that no mod
changes, and prediction from data instead of client code. It is ahead of Hytale in four places
(how abilities are predicted, the UI, keys, trust) and behind in how much a server can tell the
client to present. The three gaps most worth acting on are the camera, text and tables that come
from the server, and whether a player's own packs may override a server's look.

## How Hytale's modding works

| Piece | What |
|---|---|
| The principle | "Server-side first": every mod runs on the host. "One community, one client": no client mods, so a player joins any modded server with the same client |
| Server plugins | Java `.jar` files with the whole server API: an ECS, events, commands, packets in and out |
| Data assets | JSON: blocks, items, NPCs, world generation, interactions. Typed, with inheritance |
| Art assets | models, textures, sounds (Blockbench is the modelling tool) |
| Interactions | what an item's use does, as a JSON chain of typed steps (`Simple`, `Condition`, `Charging`, `Chaining`, `Selector`, `DamageEntity`, `Replace`, ...). The client runs the same chain for its effects while the server decides |
| Custom UI | `.ui` markup files in the mod; the server builds and changes a page with commands and gets events back |
| Delivery | the server sends its assets to a joining client; since Update 6 (August 2026) an in-game mod browser installs mods and worlds |
| Scripting | no text scripting language; a visual one is planned, for designers, with plugins adding nodes |
| Singleplayer | a local server for one player |

## How the two compare

| What | Hytale | Cinderbox | Ahead |
|---|---|---|---|
| Rules | Java plugins on the server, plus JSON assets | C++ compiled into the server; Luau planned (roadmap step 10, not started) | Hytale today: no build needed |
| Predicted abilities | "interactions": JSON chains the client runs for effects while the server decides, kept in step by counters per chain | sheets: the same lines on the server and in the player's own game ([below](#how-a-sheet-is-predicted-by-network-model)) | Cinderbox: the rule itself is predicted, by one mechanism for every sheet |
| UI | `.ui` markup; the server pushes every change and gets events back | scenes that read fields in the player's own game; clicks are intents in the input. Only a mod's answer to an intent takes a round trip | Cinderbox |
| Keys | a fixed set (Primary, Secondary, Ability1-3, Use, Dodge) | mods declare actions with suggested keys (64 at most), rebindable | Cinderbox |
| Camera | the server sends settings: distance, top-down, side view, cursor, smoothing | the game's own: four views (`ViewMode`), the player's choice | Hytale |
| Text and lists | the server can put any text or rows on screen | fields are numbers (Int, Float, Bool); a mod can say lines of text on a channel (`ctx.Say`, shown by a `CbTextLog`); lists are of players, items, slots, a container's slots, events | Hytale |
| Getting content | the server sends its assets when you join; a mod browser since Update 6 | workshop items by SHA-256, never over the game connection; the store is a folder (to be Cinderbox's own, with cooked items: 2026-11-17) | Cinderbox in design, Hytale in what exists |
| A player's own packs | none: "one community, one client" | load last and may replace any file of a look: maps, characters, effects, HUDs | open (point 1) |
| Trust | plugins are unrestricted Java; the client runs no mod code | looks are checked for scripts (bypassed until M262 by an escaped name, M261.9; shaders pass: point 7); rules to be sandboxed Luau | the server: Cinderbox, in design. The client: Hytale, today |

### How a sheet is predicted, by network model

| | Inputs model | Hybrid (`cb_server`'s default) |
|---|---|---|
| Your own sheet, on your own character (a dash, a field, a cooldown) | run at once, rolled back when wrong | run at once (`Simulation::StepCharacters`), made the server's again by each state (`HybridSession::Reconcile`) |
| Your own sheet, on a body or another player (a push, a break) | run at once, bit for bit the server's | not applied by your game: it comes with the server's events, a round trip and the physics' buffer later. A throw is drawn at once (`OwnLaunches`) |
| Another player's sheet | run by your game from its inputs | not run: its events and its puppet come from the server |

So "exact" is true of the inputs model only. What holds in both: one general correction for every
sheet, and no sync protocol per ability.

## What to take

### 1. Let a server refuse players' own packs

- Hytale's rule is that the server owns what is seen.
- In Cinderbox a player's packs load after the server's items and replace their files
  (`godot/game.gd`, `ProjectSettings.load_resource_pack( mod, true )`). They may replace a map's
  scene, a character's scene, any effect or HUD (`MODDABLE_PREFIXES` in `godot/boot.gd`). In a
  brawler that is see-through walls and glowing enemies with no cheat program.
- Nothing lets a server say no: neither its info answer (`MsgInfo`) nor the schema has a field
  for it.
- Counter-Strike has `sv_pure` for this: the server says none, only listed hashes, or any. It
  fits `MsgInfo`, next to the items.

### 2. The camera as data

- Hytale's server picks top-down with a cursor, side view, isometric or a locked pitch by sending
  settings.
- It is the one large piece of presentation a Cinderbox mod cannot touch: the views are four
  constants (`ViewMode` in `src/sim/types.h`), placed by `godot/view_camera.gd`, and every camera
  in a look is removed before the scene is used (`RemoveCameras` in `cue_guard.cpp`). The roadmap
  has it only under leftovers ("cameras a mod places"). The rig decides what kind of game a
  server is.
- One constraint Hytale lacks: the server works out where a shot's line of sight starts from the
  view (`Context::ViewPosition`). A rig's numbers therefore have to reach the schema, baked from
  the look.

### 3. Text and tables from the server, layout still the look's

- Hytale can show a shop, a quest or a vote because the server writes into the UI.
- Cinderbox's shop has its offers written twice: three rows in `hud_shop.tscn` ("Frag grenade
  1 coin") and three prices in `shop.cpp` (`kOffers`). An intent carries two bytes.
- A mod can already say text: lines on a channel (`ctx.Say`), shown by a `CbTextLog`. What is
  missing is text as a field (in a label's format, in a row) and rows of the server's own.
- Take the data, not the mechanism: rows a mod publishes that a `CbList` lists, and text fields
  sent the way names and private fields are (outside the simulation's hash).

### 4. Sheets on things in the world

- Hytale's interactions also start from use on a block or creature, entering a volume, a
  projectile's hit or bounce, equip and unequip. Update 5 added a trigger volume tool.
- A Cinderbox motion starts on a press, while or when a condition holds, or on an event, and its
  owner is a player or a kind of item (`Motion::When`, `ownerItem` in `src/sim/motions.h`).
  No map node is a trigger region or something to use; the only "use what you look at" in the
  engine is opening a container (`SlotIntent::Open`).
- Doors, buttons, jump pads and trigger regions as sheets on map templates would let a map maker
  work with no C++. In the hybrid, what such a sheet does to your own character is predicted.

### 5. One sheet for many items, and words for how skills interact

- Hytale shares one attack chain across every sword and lets each item replace named parts of it.
- It also has words for what Cinderbox writes by hand: which chains block or interrupt which, a
  combo with a window, hold time deciding the outcome, tap against hold, a cooldown shared by id.
- `fist.rules` writes the same `find` block four times and guards its two motions with
  `not motion.brawl.fist.Finisher`; its combo's window is a field a `while` motion counts down.
- None of step 6 is built yet (no `param`, `let`, `.cbsheet` in the parsers). When it is, add a
  sheet shared across items with per-item parameters, and `blocked by`, a combo and a charge as
  words.

### 6. A game as one file

- In Hytale a world is something you install from the mod browser and make copies of, and it
  carries its mods.
- Cinderbox's game is `cb_server`'s options and nothing else (`--map`, `--mods`, `--mod-option`,
  `--character`, `--move`, `--net-model`); how players find servers is parked.
- The file can be decided before the store is: map, items by hash, options, the network model,
  the pack policy from point 1. Test locally, a play link and a host would all open it.

### 7. Three smaller ones for "Before launch"

- **What an item was made for.** Update 6 moved Hytale's protocol from `hytale/2` to `hytale/3`
  and every plugin needed a rebuild. In Cinderbox baked motions and graphs carry a format version
  that is checked when read (`cinderbox_motions 3`, `cinderbox_graph 1`), but the item itself is a
  name and a hash: nothing says which engine or Godot version it was exported for, or which mods'
  names it reads. (2026-11-17: the cooked item's manifest carries them, [ROADMAP.md](../ROADMAP.md#6b-cooked-items).)
- **Rules from a store.** Hytale has to warn that "a world can carry mods that run code on your
  computer". Rules should not travel through a store before the Luau sandbox holds.
- **Shaders.** A pack may carry them: the validator scans resources for script types only
  (`_check_resource` in `godot/boot.gd`). Hytale's community Q&A notes say it refuses custom
  shaders until it can sandbox them. A decision for the list that has "the pack loaders fuzzed"
  (since 2026-11-17: decided with the cooked-item reader).

## What not to take

| Hytale's way | Why not |
|---|---|
| Server-pushed UI | every change is a message from the server, and pages need "loading states while waiting for server responses" |
| Assets sent on join | Update 7 had to make servers declare asset sizes and cap one asset at 256 MiB. A community plugin exists only to skip the transfer for returning players, and claims joins otherwise take minutes |
| Its sync of predicted chains | a community bug list records drift ("Client/Server desync 3 != 0, 2987 != 2733"), data dropped on reordered packets, and kicks when the client answers late. Charging is decided by the client. Hytale's own guide gives the case for sheets: "Both client and server execute the same interaction definition, so they stay in sync natively" |
| A fixed set of keys | no way was found for a plugin to add one; a guide gets an extra key by intercepting packets for hotbar slot 9 |

## Asset delivery, looked at again (2026-11-17)

Asked after M261.9 found that a script hidden in a Godot pack runs when the game loads it: how
Hytale sends assets, whether it compresses them, and what Cinderbox should take.

| | Hytale | How sure |
|---|---|---|
| Delivery | the server sends its assets when a player joins: each declared in `AssetInitialize`, then sent in parts (`AssetPart`, `AssetFinalize`) | `AssetInitialize` is in the official Update 7 notes; the other names are from a community packet reference seen in search results only |
| Limits | since Update 7 a server must declare each asset's true size; the client disconnects a server that sends more; one asset is at most 256 MiB | the official Update 7 notes, this part read in summary |
| Compression and transport | Zstd per kind of packet, over QUIC | community protocol docs, seen in search results only (the pages did not load) |
| Returning players | a plugin, HyTurbo, exists to skip sending again what a player has: "Join times drop from minutes to mere seconds" | its author's claim, on its page |
| Formats | JSON data, Blockbench models (`.blockymodel`), PNG, OGG, `.ui` files | the official strategy post; a community page |
| Code on the client | none: "we don't intend to support any client mods"; the client is compiled C# and cannot be modded | the official post; a community page |

Two others, for scale: Roblox converts every upload into formats of its own before it reaches its
content servers, and hands out each asset by ID from a CDN (search results only). A Minecraft Java
server names a resource pack (JSON, PNG, OGG in a zip) by a URL, with an optional SHA-1.

**What it means for Cinderbox:**

- Hytale's assets are safe because they are data its own code reads, and its client runs no mod
  code; how they travel has nothing to do with it. A Cinderbox pack is Godot objects that Godot's
  loader builds, scripts included: M261.9's hole.
- **Taken (2026-11-17)**: mod content as cooked items that our own reader reads, never Godot's
  loader ([ROADMAP.md](../ROADMAP.md#6b-cooked-items)); sizes declared before any byte, with a cap
  per file.
- **Still not taken**: assets sent on join. A store by hash already spares players Hytale's wait.
- **Compression**: not needed yet (items are 1.5 to 88 KB); Zstd for downloads once items grow.
- **Steam, considered and not taken** (2026-11-17): the store is Cinderbox's own. (The Steam
  Workshop serves only an item's latest version, and runs nothing on its side to check an upload.)

Sources, opened and read: [Hytale modding strategy and status](https://hytale.com/news/2025/11/hytale-modding-strategy-and-status);
[Update 7, modder-facing patch notes](https://hytale.com/news/2026/10/modder-facing-patch-notes-update-7);
[The Hytale Client](https://hytalemodding.dev/en/docs/established-information/client);
[HyTurbo](https://www.curseforge.com/hytale/mods/hyturbo);
[Godot proposal #4925](https://github.com/godotengine/godot-proposals/issues/4925), a loader that skips
scripts, still open; [Nodecraft: adding a resource pack](https://nodecraft.com/support/games/minecraft/adding-a-resource-pack-to-a-minecraft-server).
Seen in search results only: [a Hytale packet reference](https://hytale-docs-mcp.craftserve.com/protocol/reference/packets),
[Roblox's asset usage guide](https://create.roblox.com/docs/cloud/guides/usage-assets).

## Checked against the code

At commit `cd6f9ce` (M261, protocol 92); line numbers are that commit's.

| Statement | Where | Holds |
|---|---|---|
| A player's packs load after the server's items and replace files | `godot/game.gd:540`, `godot/boot.gd:69` | yes |
| They may replace a map's scene, effects, HUDs | `godot/boot.gd:17`, `src/godot/cinderbox_client.cpp:2039` (the map) | yes, and wider than first written: a character's scene too (`cinderbox_client.cpp:699`) |
| A server cannot refuse them | `src/net/protocol.h:110` (`MsgInfo`), `src/sim/mod_schema.h:243` (`ModSchema`) | yes: no such field |
| The camera is the game's: four views | `src/sim/types.h:186`, `godot/view_camera.gd` | yes |
| A look cannot bring a camera | `src/godot/cue/cue_guard.cpp:392` | yes |
| The server starts a shot's line of sight from the view | `src/server/mod_api.h:504`, `src/sim/types.h:182` | yes |
| Fields are numbers | `src/sim/mod_schema.h:24` | yes: Int, Float, Bool |
| The server sends no text | `src/server/mod_api.h:631` (`Say`), `src/godot/cinderbox_text.h:75` | **no**: lines on a channel exist. Corrected in the table and point 3 |
| Lists are of fixed kinds | `src/godot/cinderbox_hud.cpp:472` | yes: six |
| An intent carries two bytes | `src/sim/types.h:211` | yes |
| The shop's offers are fixed in the scene | `server_mods/shop/client/ui/hud_shop.tscn:80`, `server_mods/shop/shop.cpp:36` | yes, and written in both |
| A motion's triggers and owners | `src/sim/motions.h:333`, `:386` | yes |
| No trigger region or usable thing in a map | the node classes of `src/godot/`, `src/sim/reflect.cpp`, `src/sim/types.h:141` | yes |
| No shared sheets, `param`, `let` | the keywords of `src/godot/cue/cue_rules.cpp` and `src/sim/motions.cpp` | yes |
| Sheets are predicted exactly | `src/sim/hybrid_session.h:6`, `src/sim/simulation.h:382` | **only in the inputs model**. Corrected: [by network model](#how-a-sheet-is-predicted-by-network-model) |
| The hybrid is the default | `src/server/main.cpp:460` | yes |
| A game is a command line | `src/server/main.cpp` | yes: options only, no file |
| An item carries only a hash | `src/sim/mod_schema.h:69`, `tools/publish_mod.ps1:55` | yes for the item; **baked motions and graphs carry a version** (`src/sim/motions.cpp:368`, `src/sim/anim_graph.cpp:773`). Corrected in point 7 |
| No file crosses the game connection | `src/net/protocol.h:36` (`MsgType`) | yes |
| A HUD reads fields in the player's own game | `src/godot/cinderbox_client_values.cpp:269` | yes |
| Mods declare keys | `godot/keys.gd`, `src/sim/mod_schema.h:55`, `src/sim/types.h:165` | yes |
| Luau, `.cbsheet` and avatars are not started | no match in `src/`, `cmake/`, `godot/*.gd` | yes |
| A pack may carry shaders | `godot/boot.gd:148` | read, not tried with a pack |

## Stale lines fixed in the docs

Fixed on 2026-11-15, each after reading the code it describes. "First check" rows were found on
2026-11-14; the rest turned up in the same passages while fixing.

| Doc | It said | It says now | Found |
|---|---|---|---|
| `DESIGN.md`, the principles and the diagram | server and clients "exchange inputs and commands, never state" | the physics world is never sent as state; the hybrid sends the characters' states | while fixing |
| `DESIGN.md`, the channels | a client sends "the last 12 ticks of input"; no private fields, text or states | repeats by the loss the server counts (4 at least, M261); private fields, typed text; the hybrid's states | while fixing |
| `DESIGN.md`, Netcode | protocol 69, replay version 47, in `replay.cpp` | 92 and 61 at M261, in `src/net/replay.h` | first check |
| `DESIGN.md`, the codec | packets "CBV4" | "CBV7" (`src/present/view_codec.cpp:16`) | while fixing |
| `DESIGN.md`, the viewer protocol | "Every client simulates." | what a client simulates in each model | first check |
| `DESIGN.md`, limits: players with latency | read as every server's | the inputs model's only | first check |
| `DESIGN.md`, limits: movement | another's motion is seen "when its input arrives" | also the hybrid's: with its character's update, and your own motion's effect on others with the server's events | while fixing |
| `DESIGN.md`, limits: mods | "a board has 32 names per scope" | as many as the mods declare (`kFieldLimit`, 16 bits) | while fixing |
| `DESIGN.md`, limits: private fields | "each simulates the whole world, so there is no fog of war" | no fog of war in either model, and what the hybrid leaves out | first check |
| `DESIGN.md`, limits: large files | `simulation.cpp` 6,000 lines, the client in four parts | 7,000, five parts, the tests 13,800 | while fixing |
| `README.md` | milestones M1 to M249 are done | M261 | first check |
| `ROADMAP.md`, housekeeping | `tests/test_main.cpp` about 12,500 lines | 13,800 | while fixing |
| `docs/looks.md`, Saying things | nothing in the game makes typed text yet | text channels, and what they cannot do (a field, a row) | first check |
| `docs/looks.md`, Screens | an intent a mod declares is "not there yet" | it is there; what is missing (a predicted answer, numbers past a byte) | first check |
| `docs/looks.md`, `mods_src/README.md` | a scene with a camera is refused | a `Camera3D` is accepted and taken out | first check |
| `docs/looks.md`, Client mods | (silent) | a pack may replace a character's files; a server cannot refuse players' packs yet | while fixing |
| `mods_src/README.md`, Rules | `characters/`, `anim/`, `items/`, `motions/` "for baked data only" | all nine folders take scenes and resources; baked data in those four; `.rules` allowed | while fixing |
| `mods_src/README.md`, the files | `prefabs/player.tscn` is each player | a row for `characters/<name>/character.tscn`; the prefab is the built-in rig's | first check |
| `mods_src/README.md`, the files | the pistol's `vfx/reactions_pistol.tscn`, `ui/hud_pistol.tscn` | `vfx/reactions_gun.tscn`, `ui/hud_gun.tscn`, `ui/pistol_hud.tscn` | while fixing |
| `mods_src/README.md`, the files | `%Name`, a LineEdit in the HUD | gone: the name is typed in the menu | while fixing |
| `mods_src/README.md`, the files | HUD nodes `CbEventFeed`, `CbScoreboard` | `CbList` and the other HUD nodes | first check |

**Left alone**

- `src/present/view_codec.h:19`: its comment still says "CBV4" where the constant is "CBV7". Code,
  not docs.
- `ROADMAP.md`, "Where it stands (M234)" and "What the review found": dated to M234 on purpose.
- **Since (M261.6)**: the "CBV4" comment is fixed; the roadmap was rewritten after M261 (its M234
  sections are gone, what they found that still holds kept), its step 3 and 3b measurements moved
  to [docs/scaling.md](scaling.md), and this document's seven proposals are placed in it as the
  owner decided ([ROADMAP.md](../ROADMAP.md#from-the-hytale-comparison)), with what keeps the
  postponed ones open.
- **Not a full audit.** Only the passages around the lines above were read against the code.
  `docs/characters.md`, `items.md`, `motions.md`, `maps.md`, `testing.md`, `playtest.md`,
  `server-mods.md` and `sdk/README.md` were not.

## How sure this is

- **Cinderbox**: every statement in the table above was read in the code. None was run.
- **Hytale, official**: the strategy post and the patch notes.
- **Hytale, community**: the interaction, prediction, camera and packet details are from docs
  built on the decompiled server. The server's source, promised within two months of launch, was
  not public by mid-October 2026 according to a third-party wiki.
- **Not read**: three doc sites would not load, so the join-time asset transfer rests on the
  patch notes and one plugin's page, not on the protocol reference.

## Sources

- [Hytale Modding Strategy and Status](https://hytale.com/news/2025/11/hytale-modding-strategy-and-status)
- [Update 5 patch notes](https://hytale.com/news/2026/5/update-5-patch-notes)
- [Update 6 patch notes](https://hytale.com/news/2026/8/update-6-patch-notes)
- [Update 6 hotfixes](https://hytale.com/news/2026/8/2026-08-27-hotfixes-update-6)
- [Modder-facing patch notes, Update 7](https://hytale.com/news/2026/10/modder-facing-patch-notes-update-7)
- [The Hytale Client](https://hytalemodding.dev/en/docs/established-information/client)
- [Developer Q&A insights](https://hytalemodding.dev/en/docs/established-information/developer-qa-insights)
- [Interaction reference](https://hytalemodding.dev/en/docs/server/interaction-reference)
- [Item interaction guide](https://hytalemodding.dev/en/docs/guides/plugin/item-interaction)
- [Player input guide](https://hytalemodding.dev/en/docs/guides/plugin/player-input-guide)
- [Packet interception (hotbar actions)](https://hytalemodding.dev/en/docs/guides/plugin/customizing-hotbar-actions)
- [Listening to packets](https://hytalemodding.dev/en/docs/guides/plugin/listening-to-packets)
- [Customizing camera controls](https://hytalemodding.dev/en/docs/guides/plugin/customizing-camera-controls)
- [Custom UI](https://hytalemodding.dev/en/docs/official-documentation/custom-ui)
- [Interaction system overview (doctale)](https://doctale.dev/plugin-development/interactions/overview/)
- [Hytale core bugs (hyfixes)](https://github.com/John-Willikers/hyfixes/blob/main/HYTALE_CORE_BUGS.md)
- [HyTurboLoad plugin page](https://www.curseforge.com/hytale/mods/hyturbo)
