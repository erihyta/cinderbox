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
| Viewer | the `cinderbox` extension: `CinderboxClient` (nodes, poses, reactions, HUD), authoring nodes. No simulation, no networking | `src/godot/` |
| Peer | the `cinderbox_peer` extension: `CinderboxPeer`, the sources that simulate | `src/godot/peer/` |
| Live source | connection, prediction, rollback on its own thread (peer) | `src/client/live_source.*` |
| Replay source | a recording re-simulated on its own thread (peer) | `src/client/replay_source.*` |
| View file source | a file of frames played back, no simulation (viewer) | `src/present/view_file.*` |
| Stream source | frames sent by a server, input sent up; nothing predicted, shown only what the mods allow (stream) | `src/stream/stream_source.*` |
| Stream | the `cinderbox_stream` extension: `CinderboxStream`, a source the server sends frames to. Networking, no simulation | `src/godot/stream/`, `src/stream/` |
| Any object | `take( whole ) -> PackedByteArray`: a script can be a source | `src/godot/object_source.*` |

Everything below adds a source or changes what a frame carries. The viewer stays the same.

## Steps

Each step is a milestone of its own, and each leaves the game playable.

| # | Step | Why | Needs |
|---|---|---|---|
| 1 | **Predicted mods** | your own actions answer at once, and a mod's command stops being a rollback | nothing |
| 2 | **Private fields** | secrets that are not physical (a role, a hand of cards) | nothing |
| 3 | **A stream that feels local** | a streaming client's own character answers a round trip late | nothing |
| 4 | **One condition language** | reactions and the HUD read the game the same way | nothing |

### 1. Predicted mods

- **The problem**: a mod's command is something no client could predict, so every board write is a
  rollback, and feedback that cannot wait (`pressed:fire`) restates the server's rule in the look:
  `pistol.ammo > 0`, `!pistol.reloading`, `cooldown = 0.19`. Two copies drift (the server refuses
  to fire while frozen; the reaction does not know).
- **What**: the mods also run on a client that simulates (the peer), for the ticks the server has
  not confirmed, and their commands go into the predicted frame. When the server's frame arrives
  with the same commands, nothing is re-simulated; when it differs, the server wins, as for a
  mispredicted input. A client's mods need not be deterministic: being wrong costs a correction.
- **The rule that makes it work**: a predicted mod keeps its state on the board, where rollback
  restores it. Today mods keep state in members and a flecs world that is never rolled back (the
  pistol's ammo and next-shot tick).
- **What stays the server's**: randomness, and anything another player's press causes (a client
  only knows others' last inputs). Timers predict fine.
- **Which mods**: the ones compiled into the game. A server mod the client does not have is not
  predicted and works as it does today. Shipping other people's rules to clients needs a sandbox
  (a later step, or the declared-data version: guards and small effects in the schema).
- **Not for streaming clients**: they have no simulation.
- **Done when**: the pistol's ammo, fire rate and reload are predicted, `FirePredicted` is a plain
  reaction on `pistol.fired`, and the pickup test's rollback count drops again.

### 2. Private fields

- **What**: `declare.Field( name, type, BoardScope::Private )`. The value is never in the
  simulation, never hashed, never in a recording's frames. The server puts it in the owner's
  `ViewFrame` only (a side list next to the board, same names, same conditions in looks).
- **For live clients too**: it rides the reliable channel to its owner, outside the frame batches.
- **Done when**: a test mod gives each player a secret number; each HUD shows its own; a bot that
  dumps everything it receives never sees another player's.

### 3. A stream that feels local

- **The problem**: a streaming client predicts nothing, so its own character moves a round trip
  plus a frame late, and a late packet is a visible pause.
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
