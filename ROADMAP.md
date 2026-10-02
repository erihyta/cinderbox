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
| 1 | **A stream that feels local** | only for `--stream` clients: their own character answers a round trip late | nothing |

### 1. A stream that feels local

- **Who it is for**: clients that join with `--stream` and do not simulate. A normal client
  predicts its own movement already.
- **The problem**: a streaming client's presses, fields and upper body show at once (predictions),
  but its own character moves a round trip plus a frame late, and a late packet is a visible pause.
- **What**: a small mover for the own character only, run by the stream source from the player's
  input and corrected by the server's frames; and a short buffer of frames, so one that is late
  does not stall the picture.
- **Also**: smaller frames still (`cb_replay view` says where the bytes go): animation is 24
  bytes a player a frame, a rotation is 4 bytes whenever a body turned at all, an event is 44 bytes
  that are mostly zeros.
- **Done when**: with 100 ms of latency a streaming player's own movement starts within a frame,
  and 5% loss shows no pause.

## Housekeeping

Independent of the path; each is small.

| What | Note |
|---|---|
| A board sized by the schema | `kBoardSlots` is a wall that came back once already |
| Expressions in a prediction's `changes` | `pistol.ammo -= 1` takes a number; the right side could be an expression (`-= melee.cost`) |
| HUD nodes as reactions | `CbFieldBinding` and a While `CbReaction` with a `value_expression` do the same thing in two places; one node would need HUD scenes under a director |
| `simulation.cpp` in modules | characters, props, ragdolls, items and animation in one 2,100-line class |
| Repository | a LICENSE; `.uid` files committed; line endings settled in `.gitattributes`; the old `build/` folder in OneDrive |
