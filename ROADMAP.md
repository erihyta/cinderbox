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
| Any object | `take( whole ) -> PackedByteArray`: a script can be a source | `src/godot/object_source.*` |

Everything below adds a source or changes what a frame carries. The viewer stays the same.

## Steps

Each step is a milestone of its own, and each leaves the game playable.

| # | Step | Why | Needs |
|---|---|---|---|
| 1 | **Stream source** | a client that does not simulate; fog of war | nothing |
| 2 | **Private fields** | secrets that are not physical (a role, a hand of cards) | nothing |
| 3 | **Predicted rules as data** | a rule is written once, and your own actions are predicted | nothing |
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

### 3. Predicted rules as data

- **The problem**: a mod's command is something no client could predict, so every board write is a
  rollback, and feedback that cannot wait (`pressed:fire`) restates the server's rule in the look:
  `pistol.ammo > 0`, `!pistol.reloading`, `cooldown = 0.19`. Two copies drift (the server refuses
  to fire while frozen; the reaction does not know).
- **What**: a mod declares an action's guard and small effects once, and they travel in the schema:

  ```cpp
  declare.Action( "fire", "MouseLeft" )
      .When( "pistol.gun and pistol.ammo > 0 and not pistol.reloading and not frozen" )
      .Every( 0.2f )
      .Add( m_ammo, -1 )
      .Emit( m_fired );
  ```

  Every simulation runs them, so they are predicted and rolled back like movement. The expression
  evaluator already exists (the baked state machines use it). The server keeps what needs
  judgement: who was hit, who dies.
- **Then**: `FirePredicted` becomes a plain reaction on `pistol.fired`, and `pressed:` is only for
  looks that want the raw key.
- **Done when**: the pistol's ammo, fire rate and reload are declared, its look has no restated
  rule, and the pickup test's rollback count drops again.
- **Decide first**: this puts a mod's *predictable* rules on clients as data. If "clients never
  learn the rules" matters more than predicted actions, skip this step.

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
