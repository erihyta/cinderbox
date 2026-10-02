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

The viewer and source split is done, and the client that simulates is the only kind (streaming
clients were removed in M68). Nothing is planned in order; these are what is left to make that
client better, from DESIGN.md's known limits. Each would be a milestone of its own.

| Candidate | Today | Would be |
|---|---|---|
| **A wrong guess is taken back** | a reaction played on a prediction the server never answers stays played | a prediction that expires stops what it started (sounds, scenes) |
| **A swing seen from its start** | behind latency another player's swing is first seen a little way in, and animation keys before that point do not fire | the keys that were skipped fire on the first frame it is seen |
| **Checked pack layers** | an animation pack's layer may read what the lead cannot predict | the bake says so |

## Housekeeping

Independent of the path; each is small.

| What | Note |
|---|---|
| A board sized by the schema | `kBoardSlots` is a wall that came back once already |
| Expressions in a prediction's `changes` | `pistol.ammo -= 1` takes a number; the right side could be an expression (`-= melee.cost`) |
| HUD nodes as reactions | `CbFieldBinding` and a While `CbReaction` with a `value_expression` do the same thing in two places; one node would need HUD scenes under a director |
| `simulation.cpp` in modules | characters, props, ragdolls, items and animation in one 2,100-line class |
| Repository | a LICENSE; `.uid` files committed; line endings settled in `.gitattributes`; the old `build/` folder in OneDrive |
