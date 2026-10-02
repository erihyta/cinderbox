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
| Stream source | frames sent by a server, input sent up; nothing predicted, shown only what the mods allow (stream) | `src/stream/stream_source.*` |
| Stream | the `cinderbox_stream` extension: `CinderboxStream`, a source the server sends frames to. Networking, no simulation | `src/godot/stream/`, `src/stream/` |
| Any object | `take( whole ) -> PackedByteArray`: a script can be a source | `src/godot/object_source.*` |

The viewer takes two things the same way: what a source says happened, and what its own player
pressed. A look answers both (`CbReaction`, `CbPrediction`), so your own actions show at once
with no rule on the client. The long road is a viewer that can show a whole sandbox that way.

## Steps

Each step is a milestone of its own, and each leaves the game playable.

| # | Step | Why | Needs |
|---|---|---|---|
| 1 | **Predicted state in the viewer** | a press changes what you see beyond effects: the HUD, the body | nothing |
| 2 | **Private fields** | secrets that are not physical (a role, a hand of cards) | nothing |
| 3 | **A stream that feels local** | a streaming client's own character answers a round trip late | nothing |
| 4 | **One condition language** | reactions and the HUD read the game the same way | nothing |

### 1. Predicted state in the viewer

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
  - later, movement, for sources that do not simulate (step 3).
- **How it stays honest**: each change is held against the frame that answers it. When the
  server's cue comes, the server's value replaces it; when none comes within the wait, it is put
  back. The viewer never tells a source what it predicted.
- **Not this**: running a mod's rules on clients. The look says what the server will answer; the
  server alone decides.
- **Done when**: the pistol's ammo count and the bat's swing pose follow the click, a refused
  press puts both back, and the look still has no code.

### 2. Private fields

- **What**: `declare.Field( name, type, BoardScope::Private )`. The value is never in the
  simulation, never hashed, never in a recording's frames. The server puts it in the owner's
  `ViewFrame` only (a side list next to the board, same names, same conditions in looks).
- **For live clients too**: it rides the reliable channel to its owner, outside the frame batches.
- **Done when**: a test mod gives each player a secret number; each HUD shows its own; a bot that
  dumps everything it receives never sees another player's.

### 3. A stream that feels local

- **The problem**: a streaming client's presses show at once (predictions), but its own character
  moves a round trip plus a frame late, and a late packet is a visible pause.
- **What**: a small mover for the own character only, run by the stream source from the player's
  input and corrected by the server's frames; and a short buffer of frames, so one that is late
  does not stall the picture.
- **Also**: smaller frames still (DESIGN.md, M53 and M54, say where the bytes go): animation is 24
  bytes a player a frame, a rotation is 4 bytes whenever a body turned at all, an event is 44 bytes
  that are mostly zeros.
- **Done when**: with 100 ms of latency a streaming player's own movement starts within a frame,
  and 5% loss shows no pause.

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
