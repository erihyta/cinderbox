# Playtest checklist

Roadmap [step 1](../ROADMAP.md#1-stabilize-m215-to-m234): M215 to M234 played together, under
latency and loss, by one person with two games. Part of the [manual](../README.md#the-manual).

## How

```powershell
powershell -ExecutionPolicy Bypass -File tools\playtest.ps1 [-Add MODS] [-Character NAME] [-Option NAME=VALUE]
```

| It starts | |
|---|---|
| `cb_server` | on `test_ground`, recording a replay and a view file |
| `cb_netsim` in front of it | +75 ms each way (a 150 ms round trip), 1% lost each way |
| two games, **A** (left) and **B** (right) | each with a config of its own (`user://playtest_A.cfg`), joining through the simulator |

- Play A with the mouse and keyboard, then click B's window and play B. The window you are not in
  keeps running: its player stands still.
- Closing both games ends the session: the server writes its recording, the replay is
  re-simulated (`cb_replay verify`: a desync shows even if nothing looked wrong), and the logs
  are searched for errors. Everything is in the folder it prints.
- `-Autoplay 90` runs it with nobody at the keys (both games play by themselves): what a script
  can see. This checklist is what it cannot.
- F1 is the debug HUD: ticks, round trip, rollbacks, the motion lines.

**What to write down**: what you did, what you saw, what you expected, the session's folder. A
problem is worth a line even when it is small: "B saw A's slam land a second late", "the crate
shook against the wall".

## 1. The default mods

`tools\playtest.ps1`

| # | Do | Expect |
|---|---|---|
| 1.1 | Both join | each sees the other, with names; F1's round trip about 150 ms |
| 1.2 | A: the fist (1), click three times at B | two jabs and a finisher on A's screen at once; B is pushed and loses health, the finisher throws B; B sees A's punches a little late but without jumps |
| 1.3 | A: dash strike (2), uppercut (3), slam (4, in the air) at B | each moves A at once; the slam's shockwave throws B; B's health drops |
| 1.4 | B: guard (hold 5) while A jabs | B slows; the jab takes a quarter |
| 1.5 | A: a skill again while it cools down, then F1 | the skill does not start; F1 says why (its cooldown or its `if`) (M215) |
| 1.6 | A: the bat (its key is on the hotbar), swing at B and at a crate | the swing plays at once with its sound; B is hit (health, sound), the crate flies; the bat glows after hitting B (it shows late: the server sets it, known) |
| 1.7 | A: the pistol at B's body, then the head | hit sounds, health drops more for the head; the ammo counts down at once; reload (R) |
| 1.8 | A: a frag (H) at `BrickWall` | it rolls, goes off after 2.5 s on both screens at once; the wall loses the pieces near it, the same on both screens; the rest stands |
| 1.9 | Kill B | B falls as a ragdoll, the same on both screens; B comes back after a moment |
| 1.10 | A: first person (`` ` ``), walk, shoot | the arms and the item in view, steady; shots go where the crosshair is |
| 1.11 | A: walk into `CrateA`, the balls | they are kicked away; `HeavyCrateA` hardly moves |

## 2. Bodies

`tools\playtest.ps1 -Add possess`

| # | Do | Expect |
|---|---|---|
| 2.1 | A: walk to the `Critter` the map places, press P | A drives the critter; A's old body stands where it was, still, on both screens; first person does nothing (the critter has no camera) (M218 to M220, M225) |
| 2.2 | A, as the critter: walk, jump, sprint | it walks with its own animations; the camera follows (M219) |
| 2.3 | A: P near A's old body | A drives it again, as before |
| 2.4 | A: the pistol out, P to another body, then back | the pistol was put away on the body A left (not dropped), and is still there when A comes back (M220) |
| 2.5 | A: P near `CrateA`, drive it | it walks upright; the camera stands further back for a bigger body; it does not go through walls or `HeavyCrateA`, slides along them, turns in a corner without sticking (M221, M225, M226) |
| 2.6 | A drives a crate into a gap narrower than it (a hole a frag made, or between two crates) | it stops at the gap, and comes out of it again |
| 2.7 | B shoots and punches the crate A drives | the crate is hit as its own shape |
| 2.8 | A: P away from the crate | it is a loose crate again, falls, can be kicked |
| 2.9 | A: die while driving the crate | the crate is a loose crate again; A comes back as itself |

## 3. Another character

`tools\playtest.ps1 -Character critter`

| # | Do | Expect |
|---|---|---|
| 3.1 | Both join | both are critters, drawn and animated as the critter (M216, M217) |
| 3.2 | A: the bat, the pistol | the critter does not hold them in a hand it lacks; skills (the fist's jab, the dash) still work (M219) |
| 3.3 | Kill B | no ragdoll (the critter has none): B goes, then comes back (M219) |
| 3.4 | A shoots B's tail, then its head | the hit zones are the critter's |

## 4. Waves of NPCs

`tools\playtest.ps1 -Add waves`, then `-Add waves -Option waves.first=32`

| # | Do | Expect |
|---|---|---|
| 4.1 | Wait | NPCs stand up on a ring, find their way around the walls and the tower to the nearest of A and B, and jab (M227 to M229) |
| 4.2 | Fight them | they are hurt, killed (ragdolls), credited to whoever killed them; the next wave comes, bigger (M224) |
| 4.3 | Blow a hole in a wall with a frag | NPCs still find a way (they do not know about the hole: the mesh is not mended, known) |
| 4.4 | B mid-wave: Esc, Leave server, then join again from the menu's recent servers | B joins into the wave from a snapshot: NPCs where they are, nothing missing, no desync |
| 4.5 | With 32 NPCs: F1 | the frame rate holds; rollbacks do not pile up |
| 4.6 | After the session: watch its view file to the end (the command is printed) | it plays to the end without a crash |

## After every session

- The script's verdict: the replay verified (no desyncs), the games exited cleanly, the logs clean.
- Anything odd: the session's folder and a line here or in the roadmap's step 1.
