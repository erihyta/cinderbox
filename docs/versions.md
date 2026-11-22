# Format versions

What each version of the network protocol, the replay file and the view file changed, newest first.
Moved out of the version constants' comments in M267: the code says what is, this file how it came to be.
Raising a version adds its row here, in the same commit.

Earlier versions were not written down: the protocol's 1 to 3, the replay's 1, the view file's 1 to 3.

## The network protocol

`kProtocolVersion` in `src/net/protocol.h`. A server refuses a client of any other version (and any other build fingerprint).

| Version | What changed |
|---|---|
| 99 | Two snapshot components: what a player's grab holds (`MotionGrab`) and the bodies it threw marked (`ThrownMarks`); a motion's finds and breaks at a point (`look D`, `hit`), players first, grabs, their lines' per-thing conditions (`only`); `toward`, `marks`, `hold_apply`; expressions read `player` and what was found (`$other:FIELD`) (M290) |
| 98 | Statuses (the schema's list; a player's motion state carries up to 8, and when each of its first 16 actions went down; the command `ApplyStatus`); a motion's tags, what blocks it, what it cancels, a shared cooldown; triggers release, tap, hold; `apply` (M288) |
| 97 | A motion may be an item's (`item KIND hand|carried`): its lines read and change that item's own fields (`item:FIELD`); a motion's state is readable (`ready.X`, `cooldown.X`, `uses.X`); an item's timers count down; an item's field may be shared (M287) |
| 96 | A field may be a timer (the schema's scope byte, bit 0x40: it counts down to 0 by itself, a second a second); a motion's wait may go on when it gives up (`goon`); an emit may carry a value (M287) |
| 95 | A body that breaks a wall it strikes makes its hole around where it struck (from half a metre behind until now: a thick wall's pieces were mostly out of reach; M280) |
| 94 | Maps of version 13: places a mod names (a `CbSpawn`'s tag: `team1`), after the bodies (M277) |
| 93 | A toss's ragdoll is hit as a killed one's is (Toss: a, b the hit, c what it breaks; a ragdoll event's `breaking`), and a kill while down hits the ragdoll it keeps (M275) |
| 92 | An input packet's acknowledgements as varints behind its newest tick, its first input with how far back the player saw the others (it went without, and read 0: M261) |
| 91 | What a character carries in its puppet, by kind; another's items put away out of sight not sent (M260) |
| 90 | A puppet's update may go against its record as the client has it in its newest state (its base too old; M257) |
| 89 | Names as changes (MsgPlayerNames::whole), a field's shared bit in the schema, others' unshared fields left out (M255) |
| 88 | A frame batch's first pushers' places against the frame before (M256) |
| 87 | Other characters as puppets (M253: a few numbers each, their records without what the client keeps of its own; their layers' states, M254), no inputs of others in the states |
| 86 | A frame's joins and leaves counted in a varint, the names in two bytes (more than 255 at once broke them) |
| 85 | 1023 players (a slot is two bytes wherever it is sent: a frame's joins and leaves, the welcome, names, text, an item target's socket above it, a character's record) |
| 84 | The config's lag compensation (a motion's finds and probes meet the others as the player saw them) |
| 83 | An input says how far back the player saw the others (seenBack: lag compensation) |
| 82 | The hybrid's states by distance (updates: what is left out the client keeps) |
| 81 | 255 players (a frame says which inputs changed in as many bytes as it needs) |
| 80 | A pushable too steep to stand on there is walked through, not ground (it ran apart from a server of 79) |
| 79 | The mover finds the ground with a sphere cast (it ran apart from a server of 78) |
| 78 | An Enter event says the prop's owner and its end (its category in kind) |
| 77 | Toss, a ragdoll event that is a toss or keeps a toss's ragdoll (M249) |
| 76 | The hybrid network model in place of the state model (a frame says what it carries: inputs, physics events, pushers' places, the physics checksum; the characters' states; an input's state acknowledgement) |
| 75 | Two network models (the welcome and the info say which; State) |
| 74 | Bodies a mod drives (Drive, ModDriven in the snapshot) |
| 73 | Props a slot drives (DrivenProp in the snapshot) |
| 72 | Bodies nobody drives (IdleBody in the snapshot), SpawnBody and Possess |
| 71 | A character's ragdoll (the schema) |
| 70 | A character's first person (the schema) |
| 69 | Characters per body (the schema's list of characters, each its state machine, capsule and movement; CharacterKind; SetCharacter) |
| 68 | Amounts that are expressions in a motion set (a move's, a change's, a launch's, a link's, a force's top), a change in a find, MoveValues |
| 67 | Impulses of momentum and a step's repeat (every) in a motion set |
| 66 | An item's own motions (owner item), break and remove in a motion set |
| 65 | Markers, turns, a launch's start, paths and follow in a motion set |
| 64 | Curves and expressions in a motion set (an effect's strength), time, distance and reach |
| 63 | A probe's shape (probe_shape, in a motion set) |
| 62 | A motion's finds (shapes in a motion set), movement parameters given for a while (MoveEffects in the snapshot) |
| 61 | A motion's steps, guards, ends and edge (motion sets of version 3; its slot 28 bytes) |
| 60 | Glued breakables (maps of version 11) |
| 59 | Items that break walls (their break speed, hole, through and smash event in the schema) |
| 58 | Holes in breakables (a wall piece left standing, a map of version 10, Break with a radius) |
| 57 | What a probe may hold on to (holds, in a motion set) |
| 56 | The Break command, maps of version 9 (breakables) |
| 55 | An item's rolling and drag (the schema), maps of version 8 (a prop's), max_slope |
| 54 | An item's body of several shapes (the schema); maps of version 4 to 6 (pieces of any shape, hulls, bodies of several shapes) |
| 53 | Asking a server about itself before joining (InfoRequest, Info) |
| 52 | Typed text (Text, both ways) |
| 51 | An item its slot's key uses |
| 50 | A slot is a place (items used from the hand, or by a kind's own key) |
| 49 | Intents a mod declares (in the schema, in the input) |
| 48 | Containers (the Open and Close intents, a player's open one in its slots) |
| 47 | The Split intent |
| 46 | Stacks (a kind's in the schema, an item's count in the frame) |
| 45 | An item thrown fast (a bullet) in the schema |
| 44 | An item's time is up (its event in the schema) |
| 43 | Launch_carried in a motion, an event said after the motions |
| 42 | A line in flight in the snapshot (a projectile) |
| 41 | An item's hand in the schema |
| 40 | The slot keys in the input, a slot's suggested key in the schema |
| 39 | An input packet's later ticks as changes, a pitch difference in a frame |
| 38 | A look published again while the server runs (Items) |
| 37 | What is loose gives way to a player by its weight; a throw moves a whole ragdoll |
| 36 | Launches (a motion makes an item) |
| 35 | An item's friction and bounce, a map prop's body |
| 34 | State sized by the mods (how many fields; the config says) |
| 33 | The use button, how an item kind is used |
| 32 | Slots (a player's intent in its input, an item's slot, the config's slots) |
| 31 | (unused: texts, parked) |
| 30 | Mass; motions as triggers and effects (probes, forces, links, targets) |
| 29 | One more movement parameter (airborne) |
| 28 | Tethers (a component, a line of a motion set) |
| 27 | Two more movement parameters (air_friction, move_frame) |
| 26 | Motion sets in the schema |
| 25 | Movement parameters (the config, SetMove) |
| 24 | An item body's turn |
| 23 | Grips as animated |
| 22 | Grips in the schema |
| 21 | The view mode in the input |
| 20 | No streaming clients |
| 19 | Private fields |
| 18 | One animation system |
| 17 | Streaming clients |
| 16 | 32 board slots |
| 15 | Stowed items |
| 14 | Items in the world |
| 13 | Animation packs |
| 12 | Held items |
| 11 | State machines in the schema |
| 10 | Characters shipped with the game |
| 9 | Layers and stances |
| 8 | Facing, legs |
| 7 | Aim in the pose |
| 6 | Character |
| 5 | Names |
| 4 | Pitch, mod actions, commands, mod schema |

## Replay files

`kVersion` in `src/net/replay.cpp` (`cb_server --record`, `cb_replay`). A replay of another version is refused.

| Version | What changed |
|---|---|
| 62 | Toss's hit and what it breaks (a, b, c as the protocol's 93); a kill while down hits the ragdoll it keeps |
| 61 | A frame's joins and leaves counted in a varint |
| 60 | 1023 players (two-byte slots: the frames' joins and leaves, a character's record) |
| 59 | The config's lag compensation (a motion's finds and probes meet the others as the player saw them) |
| 58 | An input's seenBack |
| 57 | 255 players |
| 56 | A pushable too steep to stand on there is walked through |
| 55 | The mover finds the ground with a sphere cast |
| 54 | Toss, Downed and Tossed in the snapshot |
| 53 | A frame says what it carries (its inputs, physics events, pushers' places, a physics checksum) |
| 52 | Bodies a mod drives |
| 51 | Props a slot drives |
| 50 | Bodies nobody drives, SpawnBody and Possess |
| 49 | A character's ragdoll |
| 48 | A character's first person |
| 47 | Characters per body |
| 46 | Amounts that are expressions, a change in a find, MoveValues |
| 45 | Impulses of momentum, every |
| 44 | An item's own motions |
| 43 | Markers, turns, paths in a motion set |
| 42 | Curves and expressions in a motion set |
| 41 | A probe's shape |
| 40 | A motion's finds, MoveEffects |
| 39 | A motion's steps (its slot is 28 bytes, a set of version 3) |
| 38 | Glued breakables |
| 37 | Items that break walls |
| 36 | Holes in breakables |
| 35 | What a probe may hold on to |
| 34 | The Break command, breakables in the map |
| 33 | Rolling and drag (an item's, a prop's), max_slope |
| 32 | An item's body of several shapes (the schema), maps of version 4 to 6 |
| 31 | An item its slot's key uses (the schema) |
| 30 | Slots keeps the actions held |
| 29 | Intents a mod declares |
| 28 | Containers |
| 27 | A stack moved onto its kind merges, half of one splits |
| 26 | Stacks |
| 25 | Bullets, an item's hits kept first |
| 24 | An item's time is up says so |
| 23 | An item thrown from the hand, motions hear what was said after them a tick late |
| 22 | A line that flies is a projectile, and one at nothing is thrown (M152, M153) |
| 21 | The slot keys in the input |
| 20 | The mover and loose bodies, a throw on a ragdoll |
| 19 | Launches (a motion makes an item) |
| 18 | An item's friction and bounce, a map prop's body |
| 17 | State sized by the mods (the config says how many fields) |
| 16 | How an item kind is used |
| 15 | Slots (the input's intent, an item's slot) |
| 14 | (unused) |
| 13 | Mass, motion effects |
| 12 | One more movement parameter |
| 11 | Tethers |
| 10 | Two more movement parameters |
| 9 | Motion sets in the schema |
| 8 | Movement parameters in the config |
| 7 | An item body's turn |
| 6 | Grips in the schema |
| 5 | One animation system |
| 4 | Commands and mod schema |
| 3 | Baked map |
| 2 | Per-field input encoding |

## View files

`kVersion` in `src/present/view_file.cpp` (`--view-out`: what the viewer draws, recorded).

| Version | What changed |
|---|---|
| 10 | What a player carries, by kind (M260) |
| 9 | Two-byte slots (M252) |
| 8 | The container open to a player |
| 7 | A player's motions that are on |
| 6 | Boards as long as the server's fields |
| 5 | Slots in an entity's record |
| 4 | Links in an entity's record |
