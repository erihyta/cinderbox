# Many players: how Cinderbox scales, and what was measured

How many players a server holds, what limits it, and every measurement behind those answers.
[ROADMAP.md](../ROADMAP.md) says what comes next ([step 3](../ROADMAP.md#3-two-network-models) and
[step 3b](../ROADMAP.md#3b-many-players) are this work); [DESIGN.md](../DESIGN.md#netcode) says how
the netcode works; [docs/HISTORY.md](HISTORY.md) has each milestone in full. Moved here from the
roadmap on 2026-11-17 (M261.6); the measurements are as they were taken, each with its milestone.

## Where it stands (M261)

| | Today |
|---|---|
| Players a server takes | 1023 (`kMaxPlayers`; a slot is two bytes since M252) |
| Players fighting in one crowd at 60 Hz | **255**: the server's thread 15 to 17.5 ms a tick of its 16.7 (M258); each player 0.51 Mbit/s down (M257), 34 kbit/s up (M261) |
| The game in front of that crowd | 47 frames a second with 255 fighting in view (a recording, this laptop, M260); 139 with 96 live |
| 1023 in one crowd | joins and plays, below 60 Hz: the server's thread 55 ms a tick (M258), each player 1.7 to 1.9 Mbit/s down (M257, M260), the game 5 frames a second with all of them in view (M260) |
| The network model | the **hybrid** (`cb_server`'s default since 2026-11-01); the inputs model is kept for small servers |
| What each player is not sent | others' fields that are not shared (M255), others' items put away out of sight (M260), others' inputs (M253) |
| What each player is sent | every other character: near ones and those in view often, far ones about every 12th state (M257, M258's fix). **No fog of war**: a client knows where everyone is |

**The targets set for 1000 players in one area** (planned 2026-11-03), against what was reached:

| Target | Reached at 1023 | Met |
|---|---|---|
| Another character ≤ 10 B a state moving, ≤ 2 B still | 15.4 B at 255, 36.9 B at 1023, on the wire with what it holds (M260's bench); one standing still: nothing | no (moving) |
| ≤ 0.5 Mbit/s to each player | 1.67 to 1.91 Mbit/s (0.51 at 255) | no |
| ≤ 500 Mbit/s out of the server | 1.7 to 1.96 Gbit/s (130 Mbit/s at 255) | no |
| The server's work ≤ 16.7 ms a tick | 55 ms (15 to 17.5 at 255) | no |
| ≤ 6 ms a frame for 1000 characters' animation and drawing | 184 ms a frame, all of them in view (21.5 at 255) | no |

255 fighting players fit; 1000 in one fight is not reached, and what is left is listed in the
roadmap ([Later](../ROADMAP.md#a-thousand-in-one-crowd)).

## The two network models

| | Inputs | Hybrid (the default) |
|---|---|---|
| Physics (props, debris, ragdolls, items lying about, projectiles) | every client simulates it, rolled back with everything | every client simulates it once a tick from the server's events, a little in the past; never sent as states |
| Characters (players, NPCs, a driven prop) | every client simulates all of them from inputs, rolled back | the server's: a client predicts its own, and draws the rest from the server's states, interpolated, a little in the past |
| What crosses | every player's input, every tick | the physics events (a spawn, an impulse, a blast, a break, a body tossed), pushers' places, and the characters' states by priority (near ones and those in view more often) |
| A client's work grows with | the players and the rollback window | the physics bodies awake (sleeping ones cost little) |
| Your own effect on a body (a punch at a crate, a cannonball) | at once | a round trip and the buffer late (about 0.2 to 0.25 s at a 150 ms ping); the flash and the sound at once |
| Others' effect on you (a shove) | predicted from their last input | a round trip late |
| Fog of war | impossible | possible for characters (what is not sent cannot be seen; the physics world is everyone's), **not done**: every character is sent, far ones about every 12th state |
| For | fighting games, 10 to 30 players | the default; many players and big physics |

**How the hybrid works** is in [DESIGN.md](../DESIGN.md#netcode): the physics world a function of
its events and its pushers, run by each client a little in the past, each tick once; the
characters the server's, your own predicted.

### The hybrid's limitations, accepted

| Limitation | What it means |
|---|---|
| Your own physical effects are late | a round trip and the buffer: the muzzle flash and the sound at once, the crate flies later; your own throw is drawn at once (M249). A ball game played by bumping (Rocket League) belongs to the inputs model |
| Hooked to something that moves | your prediction reads it in the past: 0.4 m off on average while hooked to a shoved ball or a running player, corrected by each state (M249); on still things exact |
| A body is pushable or solid, not both | **pushed by characters** (a ball): a character walks through it and its pusher shoves it, in both models (a ball pit works, the balls scattering a round trip after you wade in). **Solid** (a crate, a wall piece, debris): it stops a character, who can stand on it; only events move it. Stood on from above when it weighs 1/16 of the character or more and is not too steep there (since 2026-11-01). Pushables are for ball pits and clutter; a game about pushing boxes is a mod's (2026-11-03). Lost from today: a heavy body slowing you (a pusher shoves anything pushable at full strength; a pusher with the player's mass, chasing the player, is possible later, but you would pass through what it fails to move: heavy things are better solid). Why: a walker that bumped into what it pushes would move by where the body is, and in the hybrid a client's predicted walker reads a world drawn in the past, so it would be corrected every tick of pushing. A grapple pulling a prop, a probe holding one, driving a crate and carrying change (M249) |
| You stand in the present on a world drawn in the past | settled debris is exact; on moving or collapsing debris the server corrects you. Later, if it matters: the server checks a player's move against the physics as that player saw it |
| The whole map's physics runs on every client | there is no interest management for physics: the weakest client's processor is the world's physics budget |
| Determinism stays absolute | CI's six builds check it; a desync costs a physics snapshot |
| Debris passes through characters | a falling crate pushes the player out; the player does not stop it |

**Decided with it (2026-10-28)**

- **One meaning in both models**: whether characters push a body is the body's own (`CbProp`'s
  flag), and it means the same on every server: no model switches it off, and a map plays the same
  whichever model a server runs. (A single rule, nothing pushable, was considered: it breaks ball
  pits.)
- **The flag's default**: solid. Walking through a crate looks wrong and loses standing on it;
  light things that should scatter (balls, cans) are marked.
- **Pushers' bandwidth**: accepted as above; it only costs while characters touch pushable things.
- **The model's name**: `hybrid`. M245's `state` model is gone (M248).

### Why the inputs model does not scale

In the inputs model every client steps the whole world, and steps it again for the whole rollback window whenever a remote player's input was not
what it guessed: nearly every tick, since mice turn every tick.

| | 64 players (M139) | 200 or more (an estimate) |
|---|---|---|
| One step | about 0.8 ms | about 2.5 ms or more |
| At a 200 ms round trip | about 16 steps again each tick: 87% of a core | about 40 ms of work in a 16.7 ms tick |
| The server | fine (it never rolls back) | still fine |
| Bandwidth | every input to every client | grows with the players squared, at the server |

In the hybrid a client steps the physics once a tick: 0.3 to 0.4 ms with 300 props in the air
(M140), whatever the players.

### Where the numbers come from

How many players in one place, and what other games do for them (the source of the targets):

| Players in one place | What works | Examples |
|---|---|---|
| up to about 32 | everyone simulating the whole world from inputs (the inputs model) | fighting games; For Honor's 4v4 |
| about 50 to 200 | the server's characters, sent by what is near, one server | Fortnite, PUBG, Apex |
| hundreds in one fight | the same, with far players sent rarely and coarsely, fewer collisions | PlanetSide 2 |
| thousands in one world | many servers per world: zones, copies of a zone, meshing | WoW, EVE (time dilation), Star Citizen |

- **What MMOs add for their numbers** is not a better prediction: sending only what is near,
  cheaper players (no collision between players, movement checked rather than simulated, 10 to 20
  ticks a second), hits decided by the server with lag compensation, and the world split across
  servers.
- **Physics is what makes Cinderbox different at scale**: MMOs mostly have no props or destruction.
  In the hybrid it costs processor time on every client, not bandwidth.
- **The cheap way to many players** is many servers of 50 to 100 per game (how Roblox scales a
  game), not one server of 1,000. One world across servers is [later](../ROADMAP.md#one-world-across-servers).
- **The inputs model, tuned** for small servers: `reconcile_every 2`; a better guess of a remote
  player's input (keep its mouse turning, rather than repeating the last input).

## The plan for many players (step 3b, 2026-11-03)

The most players a hybrid server can hold. Bandwidth first: at M250 every client was sent every
nearby character as the simulation's own record, so a crowd cost players squared.

**Decided (2026-11-03)**:

| Decision | Taken |
|---|---|
| The rates | the simulation stays at 60 Hz; a client gets one packet every 2 ticks (30 a second) |
| Mod fields | to their owner only; fields a character's state machine reads are shared by themselves; fields a look shows on others (a health bar over heads) are marked shared |
| The cap | 1023 players (a slot becomes two bytes) |
| The scope | the hybrid model only; the inputs model is kept as it is, for small servers |
| Pushers (2026-11-06) | stay exact: a place every tick (not sent every other tick and filled in: that changes how they push) |
| Drawing characters (2026-11-06; built in M260) | one way for every character, the own one too: a look made from its Godot scene (meshes skinned or on a bone, materials, layers, shadows, how the pose drives the bones, sockets), once per character, by the code the bake warns with; drawn through RenderingServer, no scene per player. Only what the look takes is drawn (scripts, other nodes, Godot animation on the skeleton, modifiers are not: the bake says so). Particles, lights and sounds a character has are nodes of its entity at their bones, for its animations' tracks; another player's item that only looks is its meshes, one that reacts keeps its scene. Shadows within 40 m. ozz stays on server and clients (the state machine is the simulation's; one sampler keeps hits where players are drawn) |
| How it is measured | the crowd bench (M251): fake players inside the server, sent what a player would be, counted; a seed plays the same session however slow the machine |

## Measured

The crowd bench (`cb_server --fake-players N --bench SEC`, M251) puts fake players in the server
and counts what each would be sent; `tools\crowd_client.ps1` (M259) puts the game in front of a
crowd; `cb_bot` through `cb_netsim` measures live players. Each section is as it was measured, on
this laptop unless it says otherwise.

### M250: states by distance, 255 players on a server

Measured on the hybrid (one machine, bots in four processes): the server's tick 0.6 ms at 16
players, 1.8 at 64, 3.5 at 128, 9 at 255 (of 16.7); each client downloads 0.55, 2.05, 4.1 and 3.9
Mbit/s (flat past 128: queuing), the server sends 0.13, 0.5 and 1 Gbit/s at 64, 128 and 255.
Bandwidth was the wall: every client got every character, about 130 bytes a character a state.
States by distance (each client its own character every state, the others as they come due):
43% smaller states with players 30 to 250 m apart; 64 bots in a crowd 82 Mbit/s out of the server
against 122. Lag compensation: 50 ms each way, a target strafing 8 m off, 14 of 14 shots hit (4 of
14 without).

### M251: the yardstick

**Measured by M251** (`cb_server --character mannequin --fake-players N --fake-radius 15
--fake-fight --bench 25`: the default mods, the players fighting with the brawl kit's fist in a
crowd 30 m across, a 100 ms round trip; 20 s counted after 5 s of joining; everything a player
would get, on the wire):

| | 32 players | 64 players | 255 players | 1023 players (M252; 50 m across) |
|---|---|---|---|---|
| To each player | 0.96 Mbit/s | 1.84 Mbit/s | 2.93 Mbit/s | 7.5 Mbit/s |
| Out of the server | 31 Mbit/s | 118 Mbit/s | 747 Mbit/s | 7.7 Gbit/s |
| Another character, on the wire (with what it holds and its input) | 96 B a state | 95 B | 112 B | 174 B |
| Characters and held items updated in a state (at most 512 entities) | 29 and 261 | 56 and 443 | of 2556 entities | 51 and 461, of 10236 |
| The head of every state (the globals: the event rings, the player list) | 572 B | 1007 B | (not kept) | 1043 B |
| Frames, to each player a tick (each sent three times) | 136 B | 448 B | 1966 B | 9968 B (pushers 3.9 KB a frame) |
| Server's work a tick (the tick, then sending; noisy on one machine) | 6.7 ms | 7.5 to 12.7 ms | 44 ms (27 + 17) | 567 ms (164 + 404) |

- **Where another character's bytes go** (changed, before packing, an update): its animation
  state 41 to 46 (every clock in it moves on every tick), the mover's internals 12 to 13, its place
  7, its velocity 3, its fields and motions 4 to 6; and each of the ~9 items it holds, its place
  again (7 each: `FollowHolders` copies the holder's onto every carried item). Packing squeezes the
  copies: held items are ~10% of the others' part, their inputs ~5%.
- **The head grows with what happens**: the event rings are in every state, so a fight's events
  (hurt, damage, a strike, a marker) cost every player ~1 KB a state at 64.
- **Frames are a third of the bytes at 255**: 798 B a tick alone, half of it pushers (23 a tick in
  a crowd near the sandbox's balls), the rest commands (10 events a tick) and physics events; then
  sent three times.
- **At 1023 (M252) frames are two thirds**: 4.7 KB a frame, 3.9 KB of it 215 pushers' places a
  tick, then sent about twice: pushers are M256's first target, not a maybe.
- **At 1023 the 512-entity budget starves**: 51 characters a state, so a character waits ~20
  states for its turn, its base ages out (24 states) and goes whole: 154 records whole a state.
  M257's byte budget, and records small enough that many fit (M253), are what fix it.
- **The server at 1023**: 567 ms a tick (34 times its budget): encoding states 346, planning 58,
  characters 43, mods 36, physics 23 (the bench's own fake inputs another 36).
- **Found and fixed** (M251): what a player was thought to have went out of NetId order once
  anything was gone (M250's states by distance: the gone ones were put first), and from then on
  every record went whole in every state and the list grew without end: 171 B per other character
  at 64 fighting players, 95 after the fix. And a frame holds 1024 commands: the rest were dropped
  without a word, so 255 players joining at once lost most of their start items; the rest waits
  for the next tick now, and the server says so.
- **Found and fixed** (M252): a character that fell below the kill plane was put back, and what it
  carried was destroyed with everything else below it (falling off the map emptied your hands and
  bag, and no mod was told); it follows its holder back now.
- **Found and fixed** (M252's review): two counts were still a byte, a frame's joins and leaves
  (256 at once broke the frame for everyone) and the names message (past 255 players, the wrong
  number of names); the map's bodies went past a full first frame. Protocol 86, replay 61.
- **Names go whole**: every join sends every player's name to every player, ~13 KB each at 1023
  (M255 sends the change only).

### After M253 and M254: puppets

**Measured after M253 and M254** (the same, `--bench 10`: 5 s counted):

| | 32 players | 64 players | 255 players | 1023 players (50 m across) |
|---|---|---|---|---|
| To each player | 0.35 Mbit/s (0.96) | 0.68 Mbit/s (1.84) | 2.0 Mbit/s (2.93) | 7.0 Mbit/s (7.5) |
| Out of the server | 11 Mbit/s (31) | 44 Mbit/s (118) | 509 Mbit/s (747) | 7.2 Gbit/s (7.7) |
| Another character, on the wire | 13.6 B a state (96) | 13.7 B (95) | 12.7 B (112) | 12.4 B (174) |
| Of a state, the head | 570 of 1095 B | 989 of 1931 B | 1061 of 4107 B | 1042 of 7491 B |
| Frames, of a player's bytes | 19% | 27% | 48% | 73% |
| Server's work a tick (the tick, then sending) | 1.1 ms | 2.2 ms | 10.5 ms (7.3 + 3.3) | 134 ms (39 + 95) |

- **A puppet's change is 11 B** before packing (a player standing still sends none): where it
  is (60% of changes), its facing and look (about half), its flags (16%), and a layer's new state
  (30%, 1.7 layers each: the fights). Its record's view changes by its fields alone (0.4 to 2 B);
  its held items are not sent at all while they stay as they were.
- **Found on the way**: a layer's start tick, as a OneShot that is not playing or a character
  dead or down has it (its clock stands), moved every tick, and three of them went in every
  update (26 B a change, 11 after).
- **What is left**: at 255 frames are half the bytes, at 1023 three quarters (4 KB of pushers a
  frame: M256); the head is half a state at 64 (M255); at 1023 the 512-entity budget still decides
  how many characters a state brings (M257); the server spends 95 ms a tick sending (planning 29,
  encoding 67: M257, M258).

### After M256: frames carry the physics only

**Measured after M256** (the same):

| | 32 players | 64 players | 255 players | 1023 players |
|---|---|---|---|---|
| To each player | 0.18 Mbit/s (0.35) | 0.33 Mbit/s (0.68) | 1.14 Mbit/s (2.0) | 3.35 Mbit/s (7.0) |
| Out of the server | 5.9 Mbit/s | 21 Mbit/s | 290 Mbit/s | 3.4 Gbit/s |
| A state | 592 B (head 39) | 1005 B | 3094 B | 6473 B |
| Frames, of a player's bytes | 17% | 24% | 31% | 51% |
| A frame alone (pushers of it) | 35 B (7) | 83 B (30) | 358 B (165) | 1713 B (1110) |

- **What went**: the event rings out of every state's head (~1 KB, half a state at 64); the
  commands clients skip (a third of a frame's, at 255); every pusher's place whole at the start of
  each packet; each frame's third packet, and half the packets.
- **What is left at 1023**: frames are half (224 pushers' places a tick, 5 B each: fighters in a
  crowd change direction all the time, so going on as they went predicts little); the states the
  other half, as many characters as the 512-entity budget lets (M257).
- **Server times**: not compared here. While this was measured the machine was busy (88% of its
  processor taken by other programs): main's own server took 76 ms a tick at 255, as this one did.

### After M255: only what others see

**Measured after M255** (the same): each player 0.16, 0.29, 0.99 and 3.05 Mbit/s at 32, 64, 255
and 1023; another character 10 B on the wire (others' cooldown timers no longer reach them). At
1023 the frames are 56% (pushers 65% of a frame, the mods' events 22%), the states 44%.

- **Far puppets are an interval late**: one sent less often than every other state is drawn from
  the one before to the last over the time between them, so it moves without jumps but up to 16
  ticks late; a shot at it 100 m off is tested as the shooter saw it, behind. M257 sends what is
  aimed at more often.

### After M257: a byte budget per client

**Measured after M257** (the same; the default budget, 256 kbit/s of others a player):

| | 32 players | 64 players | 255 players | 1023 players |
|---|---|---|---|---|
| To each player | 0.16 Mbit/s (0.16) | 0.26 Mbit/s (0.29) | 0.51 Mbit/s (0.99) | 1.91 Mbit/s (3.05) |
| Out of the server | 5.0 Mbit/s | 17 Mbit/s | 130 Mbit/s | 1.96 Gbit/s |
| Others updated a state, of | 27 of 31 | 51 of 63 | 41 of 254 | 42 of 1022 |
| Another character, on the wire | 11.2 B | 11.1 B | 15.6 B | 32.3 B |
| Events at characters a state (bytes) | 3.6 (36 B) | 8.5 (79 B) | 39 (334 B) | 92 (770 B) |
| Frames, of a player's bytes | 14% | 19% | 43% | 70% |

- **The budget decides from 255 on**: each other character is sent about 5 times a second at 255
  (near ones and those in view more), 1.2 at 1023. A character that waited longer than a base is
  kept goes against the record the player has (carried: 14.5 a state at 1023, where 3.2 still go
  whole: one whose last update was not acknowledged).
- **Events now reach the others**: what a character does itself (its motions, clip markers, using
  its slot's item) never reached another player's game in the hybrid; with its character now,
  once each. A mod's event at a character no longer goes to everyone in every frame twice.
- **What is left at 1023**: frames are 70%, 1110 B of a 1327 B frame its 224 pushers'
  places, each frame in two packets (pushers stay exact: your call); the server plans states for
  68 ms a tick, every client walking every entity (M258).

### After M258: the server's processor

**Measured after M258** (the server's own thread a tick, the bench's fake players' inputs left
out; this laptop's figures move by 10 to 20% from run to run):

| | 64 players | 255 players | 512 players | 1023 players |
|---|---|---|---|---|
| The server's thread a tick (M257) | 1.8 ms | 15 to 17.5 ms (22) | 28 ms | 55 ms (178) |
| Of it: the mods | 0.5 | 3.5 to 5.4 | 9 | 12.6 |
| The characters, the physics | 0.3, 0.4 | 2.7 to 3.2, 3.0 | 6.7, 3.4 | 10.2, 6.5 |
| Keeping a state | 0.3 | 2.0 | 4.0 | 9.6 |
| Planning and encoding the states (on the state threads, while the next tick runs) | 0.4 | 5.7 | 14.5 | 31.5 |

- **255 fighting players fit 60 Hz, just**: the server's thread 15 to 17.5 ms of the 16.7.
- **What went**: a state's updates on the server's thread (they are made while the next tick
  runs); a client's walk of ten thousand entities copying its records each state (in place now, by
  group); a third of a megabyte made for each message's compression; each player looking up its
  held items in every item's list; the expire mod making a map of every item each tick.
- **Found**: mods read the last tick's events from the ring of 32. At 1023 fighting players a tick
  says 114, so most hits hurt no one: mods read all the step said now (`RecentEvents`).
- **What is left at 1023**: the mods (inventory, frag and the layers each look at every player
  every tick: about 2 µs a player each), the motions' pass and the slots (one thread), keeping a
  state (its serialization), the physics; on the state threads, a client's walk of every entity (an
  interest grid would cut it where players are spread out; in a crowd every one is near).

### M259: the game in front of a crowd

**The game in front of a crowd** (M259, `tools\crowd_client.ps1`: a fight recorded with
`--record-view` and played by the game, 1280x720, this laptop; live, the game joined to a server of
fake players, which keeps up beside it to about 100):

| | 96 players, live | 255 players, a recording | 1023 players, a recording |
|---|---|---|---|
| A frame | 19 ms (53 a second) | 58 ms (17) | 840 ms (1) |
| The game's own thread | 7.3 ms | 26 ms | 600 ms |
| Of it: poses (characters posed of all) | 0.9 (56 of 97) | 2.0 (134 of 255) | 6.6 (446 of 1023) |
| The nodes (characters, items) | 3.3 | 9.2 | 31 |
| The events (a visual made or freed, a reaction) | 1.7 | 8.8 | 467 (530 visuals made a frame) |
| The skeletons (the pose put on each) | 1.5 | 3.9 | 14 |
| Nodes in the scene | 9,200 | 19,400 | 80,000 |

- **What went** (at 255, the same recording before and after): 170 ms a frame to 115 before the
  recording was made again: poses 7.8 ms to 4.0 (by distance and view), the HUD's labels 17 to 4
  and lists 11 to 7 (a scoreboard's rows, five labels a player, worked every frame while hidden; a
  slot asked every visual for its item); a live game's source hands it its frame swapped, not copied.
- **What is left is the drawing** (M260): every character a scene, every item it holds a node (at
  1023, a few hundred made and freed a frame, ~1 ms each), a skeleton each with its modifier, Godot's
  own work for 80,000 nodes. One baked look drawn through RenderingServer, with what a character
  holds as kinds in its puppet, is what removes them. The mods' reactions to events (sounds,
  effects) cost about 0.3 ms an event.

### After M260: one look for every character

**Measured after M260** (the same recordings and the same live crowd; in brackets, M259's):

| | 96 players, live | 255 players, a recording | 1023 players, a recording |
|---|---|---|---|
| A frame | 7.2 ms, 139 a second (19 ms, 53) | 21.5 ms, 47 (58 ms, 17) | 184 ms, 5 (840 ms, 1) |
| The game's own thread | 2.8 ms (7.3) | 11.2 ms (26) | 110 ms (600) |
| The events (a visual made or freed, a reaction) | | 2.8 (8.8) | 68 (467) |
| Nodes in the scene | 3,700 (9,200) | 7,900 (19,400) | 28,800 (80,000) |

| The crowd bench (`--bench 10`) | 255 players | 1023 players |
|---|---|---|
| What a client has of a state's entities | 778 of 2,556 (all of them) | 3,082 of 10,236 (all) |
| Planning a state, a tick | 2.0 ms (3.5) | 12.6 ms (20.6) |
| To each player | 512 kbit/s (the same) | 1.67 Mbit/s (the same) |

- **The drawing**: every body is its look's (one skeleton and one instance for the mannequin), not
  a scene of 40 nodes with a skeleton, a modifier and bone attachments; the shots of the same
  moments differ from M259's by a few pixels (poses timed by the clock). In first person the body's
  shadow now shows (the old body's did not, though it was meant to).
- **Items**: of nine a fighter carries, eight are put away out of sight: no longer sent to the
  others, nor drawn. Bytes are the same (unchanged items cost nothing since M253): what a client
  keeps and walks is a third, and the server plans each state in two thirds of the time.
- **What is left**: at 1023 recorded, the held bats made as they come into sight (a recording's
  frames a second apart: far more than in play), the HUD's and the mirror's work for the others'
  states; Godot's own drawing of a few hundred skinned characters. Pooling item scenes, and the far
  characters as a GPU crowd, if a measured target needs them.

### After M261: the uplink

**Measured after M261** (the uplink, live: 32 `cb_bot` players, 2 of them full clients, through
`cb_netsim` at 75 ms and 1% loss each way; what each sends, from `cb_bot`'s report, and what the
server takes, from its status line's new `in`):

| | Before (M260: 12 repeats) | After (by the loss) |
|---|---|---|
| Up, per player | 33 kbit/s (46 with UDP/IP) | 20 kbit/s (34 with UDP/IP) |
| Into the server, 32 players | 977 kbit/s | 645 kbit/s |
| Into the server, 1023 players (each the same) | ~47 Mbit/s | ~35 Mbit/s |
| Late inputs | 0.02% | 0.02% |

| Loss each way (`cb_netsim --loss`) | 1% | 5% | 12% |
|---|---|---|---|
| Counted by the server | 1% | 5% | 11 to 12% |
| Each input in | 4 packets | 5 | 7 to 8 |
| Late inputs (12 repeats at the same loss) | 0.00% | 0.1% | 0.3 to 0.5% (0.3 to 3.5%) |

- **ENet's loss figure** counts its own reliable packets: at 12% each way it said 0, and the
  repeats stayed at 4. The server counts the input packets that do not come (a sequence byte in
  each) and tells the client every two seconds.
- **Late inputs at heavy loss** are the same with 12 repeats: they come from elsewhere (the clock
  following lost frames), not from fewer repeats.
- **What is left**: a packet's headers (UDP and IPv4 28 bytes, ENet 12, of about 71) at 60 packets
  a second. An input packet every other tick would take a player's uplink to about 20 kbit/s and
  delay inputs by 8 ms on average: a choice, not taken.

## The milestones

Each in full in [docs/HISTORY.md](HISTORY.md#in-detail).

| Milestone | What |
|---|---|
| M244, M245 | the server-state model: measured, built and dropped (it streamed the physics world) |
| M246 | characters read the physics world and never write it: pushers, physics events |
| M247 | the physics world stands alone: two Box3D worlds, two NetId counters |
| M248 | the hybrid on the wire; `cb_server`'s default since 2026-11-01 |
| M249 | a player tossed; your own throws drawn at once |
| M250 | states by distance, 255 players, lag compensation (mods' casts and motions) |
| M251 | the crowd bench |
| M252 | 1023 players (a slot is two bytes) |
| M253, M254 | other characters as puppets; their animation run by each client |
| M255 | only what others see: unshared fields, names as changes |
| M256 | frames carry the physics only |
| M257 | a byte budget per client; events with their characters |
| M258 | the server's processor: states made while the next tick runs, characters on threads |
| M259 | the game in front of a crowd: measured, poses by distance and view |
| M260 | one look for every character, through the RenderingServer; others' items put away not sent |
| M261 | the uplink: repeats by the loss the server counts |
