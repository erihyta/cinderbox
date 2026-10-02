# Cinderbox — Roadmap

Where the client is going, in order. [DESIGN.md](DESIGN.md) says how things work today; this file
says what comes next and why. A step that is done moves out of here and into DESIGN.md.

## The idea

A **viewer** draws frames and never asks where they came from. A **source** produces them.

```
source  ──ViewFrame──>  viewer        the world at one tick, who is who, how the source is
source  <──input─────   viewer        the local player's input, for sources that play one
source  <──control───   viewer        named commands with a number ("pause" 1, "skip" -5)
```

| Piece | Today | File |
|---|---|---|
| The protocol | `ViewFrame`, `ViewSource`; as bytes wherever a library or a file is between the two | `src/present/view.h`, `view_codec.*` |
| Viewer | the `cinderbox` extension: `CinderboxClient` (nodes, poses, reactions, predictions, HUD), authoring nodes. No simulation, no networking | `src/godot/` |
| Peer | the `cinderbox_peer` extension: `CinderboxPeer`, the sources that simulate | `src/godot/peer/` |
| Live source | connection, prediction, rollback on its own thread (peer) | `src/client/live_source.*` |
| Replay source | a recording re-simulated on its own thread (peer) | `src/client/replay_source.*` |
| View file source | a file of frames played back, no simulation (viewer) | `src/present/view_file.*` |
| Any object | `take( whole ) -> PackedByteArray`: a script can be a source | `src/godot/object_source.*` |

The viewer takes two things the same way: what a source says happened, and what its own player
pressed. A look answers both (`CbReaction`, `CbPrediction`), so your own actions show at once
with no rule on the client. The long road is a viewer that can show a whole sandbox that way.

## Steps

Each step is a milestone of its own, and each leaves the game playable.

| # | Step | Why | Needs |
|---|---|---|---|
| 1 | **Stream source** | a client that does not simulate; fog of war | nothing |
| 2 | **Private fields** | secrets that are not physical (a role, a hand of cards) | nothing |
| 3 | **Predicted state in the viewer** | a press changes what you see beyond effects: the HUD, the body | nothing |
| 4 | **One condition language** | reactions and the HUD read the game the same way | nothing |

### 1. Stream source

- **What**: the server captures a `ViewFrame` per streaming client and sends the bytes; the client
  sends input up as it does now. No simulation, no prediction, no rollback on that client.
- **Where it lives**: a third kind of source object next to `CinderboxPeer`, small enough to be its
  own extension (ENet and the codec, no simulation), so a streaming client ships without the peer.
- **What a frame costs**: compact packets at 20 frames a second are 67 kbit/s for 4 players, 429
  for 32 and 671 for 64 (DESIGN.md, M53; the aim was 300 for 32). `cb_replay view` says where the
  bytes go. What is left to take, in the order of what it would save:
  - each client is sent only what is near it (the same hook that hides things for fog of war);
  - animation: 24 bytes a player a frame, a third of it the mask of which values changed;
  - rotations: 4 bytes whenever a body turned at all; a turn since the last frame fits in less;
  - events: 44 bytes each, most of them zeros.
- **For**: spectators, weak machines, and servers whose game does not need predicted physics
  (cards, boards, turn-based).
- **Fog of war**: the server filters each client's frame through a mod hook
  (`bool Visible( viewer, entity )`). Only streaming clients can be kept in the dark: a client that
  simulates the world has the world.
- **Honest cost**: without prediction, the local player moves a round trip late. A small local
  mover for the own character is a later step, not part of this one.
- **Done when**: a client joins with `--stream`, plays with the server's mods and looks, and a test
  mod hides an entity from one player and not the other.

### 2. Private fields

- **What**: `declare.Field( name, type, BoardScope::Private )`. The value is never in the
  simulation, never hashed, never in a recording's frames. The server puts it in the owner's
  `ViewFrame` only (a side list next to the board, same names, same conditions in looks).
- **For live clients too**: it rides the reliable channel to its owner, outside the frame batches.
- **Done when**: a test mod gives each player a secret number; each HUD shows its own; a bot that
  dumps everything it receives never sees another player's.

### 3. Predicted state in the viewer

- **Today**: a `CbPrediction` plays a cue's effects at once (DESIGN.md, M56). What the server
  *changes* still waits for its frame:

  | You press | Shown at once | Shown a round trip later |
  |---|---|---|
  | fire, pistol out | flash, gunshot, camera kick | the ammo count, the tracer, the hit |
  | fire, bat out | the swing's sound | the body's swing pose |

- **What**: a prediction can also say what it changes on the viewer's own player until the server
  speaks, as data in the look:
  - a field: `pistol.ammo - 1` (the HUD and conditions read the predicted value);
  - a stance or an animation state (the swing starts now);
  - later, movement, for sources that do not simulate (a stream).
- **How it stays honest**: each change is held against the frame that answers it. When the
  server's cue comes, the server's value replaces it; when none comes within the wait, it is put
  back. The viewer never tells a source what it predicted.
- **Not this**: running a mod's rules on clients. The look says what the server will answer; the
  server alone decides.
- **Done when**: the pistol's ammo count and the bat's swing pose follow the click, a refused
  press puts both back, and the look still has no code.

### 4. One condition language

- **What**: reactions (`src/godot/cue`) and HUD nodes (`src/present/fields`) parse conditions
  separately today. One parser, with `or`, arithmetic, field-to-field comparisons, and a way to
  pass a value through (`volume_db = event.strength * 2`, a bar from a field) on `CbReaction`.
- **Done when**: `CbFieldBinding` is a `CbReaction` with a value expression, and one test file
  covers the grammar.

## Housekeeping

Independent of the path; each is small.

| What | Note |
|---|---|
| DESIGN.md by subsystem, not by milestone | sections from early milestones describe things later ones replaced (effect bindings, the loadout) |
| A board sized by the schema | `kBoardSlots` is a wall that came back once already |
| A `combat` mod | health, death and respawn live in `pistol.cpp`; melee hurts only because the pistol mod runs |
| One animation system | the mode controller, stances and `CinderboxAnimator` predate the baked state machines |
| `simulation.cpp` in modules | characters, props, ragdolls, items and animation in one 2,100-line class |
| The raylib client | the Godot client now plays recordings; decide whether `cb_client` stays as a debug tool or goes |
| Repository | a LICENSE; `.uid` files committed; line endings settled in `.gitattributes`; the old `build/` folder in OneDrive |
