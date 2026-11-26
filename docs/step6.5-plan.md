# Step 6.5: the sheet compiler, for real, planned

Written 2026-11-26, after step 6 (M292) and before 6b. It comes from
[the step 6 review](step6-review.md): step 6 shipped the language, but not the compiler its plan
described, and the shipped sheets mostly don't use step 6's own fixes. 6b makes the sheet compiler
the reader for strangers' content, so this step comes first.
[ROADMAP.md step 6.5](../ROADMAP.md#65-the-sheet-compiler-for-real) is the summary. Where this file
and the code disagree, the code wins.

**The owner's rule for this step (2026-11-26):** before every change, reconsider whether it is the
right call. Each change below says why it is worth doing; [§5](#5-reconsidered-and-dropped) lists
what was dropped.

## Contents

1. [What does not change](#1-what-does-not-change)
2. [Goals and non-goals](#2-goals-and-non-goals)
3. [The design](#3-the-design)
4. [Milestones, in order](#4-milestones-in-order)
5. [Reconsidered and dropped](#5-reconsidered-and-dropped)
6. [Decisions for the owner](#6-decisions-for-the-owner)
7. [Risks](#7-risks)

---

## 1. What does not change

- Everything in [step 6 §1](step6-plan.md#1-what-a-sheet-is-for-what-does-not-change): simulated blocks
  are deterministic, bounded, rolled back and run everywhere; looks never change the simulation; no
  code from a stranger runs.
- **Version 1 stays the format.** This step changes how it is read, not what a shipped sheet means,
  except where a milestone says so (M296, M297), each with its test.
- The expression language (`src/expr`) and the simulation's motion runner (`RunMotions`) stay.
- The rule dictionaries (`Rule`, `Dict`) stay as the compiler's output for now: the look's nodes
  read them until 6b replaces the nodes.

## 2. Goals and non-goals

**Goals**

| # | Goal | From the review |
|---|---|---|
| G1 | **One front end**: tokens with line and column, a syntax tree, lowering from the tree. No text rewritten into other text | A1, A2, A3, A8 |
| G2 | **One source for an item's motions**: the server compiles the item's sheet; items stop carrying baked `motions/*.cfg` | A4, A5, A7 |
| G3 | **Mistakes found where they are typed**: `cb_sheet check`, the Sheet screen and the VS Code problem matcher say what the bake says, at the word | A6, A9, A10, L1 |
| G4 | **Limits that do not shrink as mods are added** | E1, E2, E3 |
| G5 | **A regular language**: one way to say where, defaults from where a sheet sits, no number written twice | §8 of the review |
| G6 | **Shipped sheets that show the right way**, since modders will copy them | §6 of the review |

**Non-goals**

- No new gameplay words beyond what the review found missing.
- No binary format for motions between the server and clients (the text is ours once the server
  writes it: [§5](#5-reconsidered-and-dropped)).
- No change to how reactions are drawn (6b).

## 3. The design

### 3.1 The front end (M293)

```
text ──lex──> tokens (line, column; INDENT / DEDENT; comments kept for the formatter)
     ──parse──> syntax tree: the version line, declarations, lets, uses, blocks;
                a block's lines, each with its words and the lines under it
     ──compose──> the used sheet's tree, with this sheet's declarations and replaced blocks
     ──resolve──> lets (by whole words; a word of the language cannot be a let's name),
                  params, item fields, units (checked, dropped)
     ──lower──> the rules of today (Rule dictionaries): motions, reactions, predictions, HUD lines
```

- **Proved the same**: every shipped sheet, the checks' sheets and the SDK's starters lower to the
  rules they lower to today. The golden files (`tests/golden/sheets`) hold every rule with its line
  numbers, so they must not change; the CI's re-bake must show no difference.
- **Deleted**: `LowerVersion1` and its text helpers (`ExprToV0`, `ApplyLets`, `WithoutUnits`, ...),
  `ParseVersion0` (1,366 lines), version 0 and the converter (`cb_sheet convert`, the Sheet screen's
  Convert): decision F1.
- **Errors** say `line:column: why`, and the Sheet screen underlines the word.
- **Expressions** stay text inside the tree, as tokens; lowering writes them in the expression
  language's own spelling (`^^:`, `?`, `item:`), which is the engine's form and no longer a sheet's.

### 3.2 The server compiles the sheet (M294)

Today a bake turns a sheet's motions into `motions/<kind>.cfg` on the modder's machine, and the
server runs that file. The file holds two things: the sheet's logic, and **the scene's parts the
sheet names**, already resolved (`find Blast` became `shape 8 0 0 0`, `along Muzzle` a direction).

After:

| Who | Does |
|---|---|
| The bake (editor, publish) | writes the parts a sheet names into `items/<kind>.cfg`: each shape (sphere, box, capsule, hull points), marker (origin and forward), path (its samples), by its node path. Data only, no logic |
| The server | compiles each item's sheet against its parts (`BakeSheetMotion` with a `Scene` read from those lines) and sends the motion text in the schema, as today |
| `cb_sheet test`, `cb_sheet check` | do the same, so a sheet that was edited and not baked is what they run and check |
| The item | carries the sheet and its parts; no `motions/` folder |

- `CbItem`'s `use_conditions`, `use_changes`, `use_cooldown` and `use_emits` go (no shipped scene
  sets them; the sheet's `use` block says it).
- A motion set (`CbMotionSet`, the dash's and flight's moves) is a sheet with no parts.
- The 6b plan changes with it: the cooked item's `simcfg` chunk carries parts, not motions.

### 3.3 Limits (M295)

| Today | After |
|---|---|
| `active X` reads a 64-bit mask of the server's first 64 motions (`anim_graph.cpp:228`) | read from the motion state, as `ready X` is: any motion |
| Tags: 32 for the whole server, the rest dropped silently (`motions.cpp:1634`) | as many as the motions name (a mask per motion set, or a list); never dropped silently |
| `every` repeats 64 times at most (`kMotionRepeats`) | no count: a repetition costs only its own tick (step 6's P18) |

These change what the simulation hashes: the reference hashes are refreshed, with every compiler
agreeing, as in M287 and M289.

### 3.4 The language, made regular (M296)

| Change | Why it is worth it |
|---|---|
| **One way to say where**: `at PLACE` only; `attached` makes what is created ride along with that place. `create X under P` becomes `at P attached`; `at follow` becomes `at holder attached` (or the subject). Placed without `attached`, it takes the node's position *and* rotation at that moment | today `under` (a child of a node) and `at` (a point, position only) are two syntaxes; `at follow` overlaps `under`; and `at X` with `create ... under Y` drops `under` silently (`cue_reaction.cpp:822-866`). Decided by the owner, 2026-11-26 (the word: F2) |
| `as holder` is the default subject in an item's sheet | 13 of 14 item reactions write it today |
| A field's start value may name a param: `field gun.rounds int on item = gun.magazine` | the pistol and the rifle write their magazine twice |
| Tags are declared (`tag punch`), and every name in `blocked by` and `cancels` is checked when the sheet compiles | a typo is a log line today; tags, motions and statuses share a namespace |
| A motion may have more than one trigger: `motion Reload on press reload, on item.use_failed` | `Dry` and `Reload` copy three lines |
| `down` (and `back`) as directions | `along down` is read as a marker named "down" |
| `per second` said, in a `while` motion's changes | today `-=` in a `while` is per second without saying so |
| An item's field has one name in motions and in looks | `gun.reloading` in one, `hand.gun.reloading` in the other |
| A HUD reads `ready X`, `cooldown X`, `uses X` and statuses | the reason brawl kept its `cd_*` timers |
| A cast and the owner's own effects in one step | the dash strike needs two motions with a copied condition (step 6's list) |
| A word for "this item is being used", not `using.gun.rifle` | the rifle names its own kind |
| `steer toward POINT speed S strength A` for `force A kind velocity top S` (the old spelling still read) | nobody can guess what the first number is |

### 3.5 Shipped sheets on the new words (M297)

- brawl's `cd_*` timers go; the HUD lines read the motions' cooldowns.
- The dash strike in one motion; the pistol's and rifle's numbers once; the prediction's mark
  cooldown from `mark.seconds`.
- The fist, bat and sword without their copied `timeout` lines, as F3 decides.
- Units everywhere; `let` where a phrase repeats (the frag's falloff, the telekinesis's point).
- Comments say what and why, not milestone numbers.
- `docs/sheets.md` follows: the new words, no example that is refused.

## 4. Milestones, in order

| # | Milestone | Done when |
|---|---|---|
| **M293** | **One front end** ([§3.1](#31-the-front-end-m293)) | every shipped, check and starter sheet lowers to the same rules (golden files unchanged, CI's re-bake unchanged); the 23 sheet tests pass; `LowerVersion1`, `ParseVersion0`, version 0 and `convert` deleted; errors with columns; a `let` named like a word of the language is refused |
| **M294** | **The server compiles the sheet** ([§3.2](#32-the-server-compiles-the-sheet-m294)) | no mod has a `motions/` folder; the server, `cb_sheet test` and `cb_sheet check` compile sheets against baked parts; `CbItem.use_*` gone; the tests and the bots pass |
| **M295** | **Limits** ([§3.3](#33-limits-m295)) | `active` reads any motion; no tag dropped; `every` without a cap; reference hashes refreshed, every compiler agreeing |
| **M296** | **The language, made regular** ([§3.4](#34-the-language-made-regular-m296)) | each change has a `.cbtest` or a golden file; `sheets.md` updated |
| **M297** | **Shipped sheets on the new words** ([§3.5](#35-shipped-sheets-on-the-new-words-m297)) | no `cd_*` field in brawl; no number written twice in the guns; a playtest by hand of the kit, the sword, the bat and the guns |

**Order, and why.** M293 first: every later change is made once, on the tree, not in a text
rewrite that is about to go. M294 next: it decides what an item carries, which 6b builds on. M295
before M296, so that the new words (`tag`, `active` in HUDs) do not inherit the old caps. M297 last,
when the words it uses exist.

**After this step**, 6b's milestones follow (renumbered: M298 to M305).

## 5. Reconsidered and dropped

| Was proposed | Dropped because |
|---|---|
| Lowering straight to `sim::Motion`, no motion text at all | once the server writes the motion text itself, the text is an internal format between our server and our clients, not a modder's file. Replacing it buys nothing a player or a modder sees; the 1,243-line `CompileMotionSet` stays as that format's reader |
| Fixing the baked `.cfg` headers ("Baked from the CbMotion nodes") | the files go in M294 |
| Replacing the rule dictionaries with typed structs | the look's nodes read them until 6b, which replaces the nodes |
| A new word so a timed-out marker can skip the recovery | only if the owner picks F3 (c); (a) needs no word |

## 6. Decisions for the owner

| # | Question | Options | Leaning |
|---|---|---|---|
| F1 | Version 0 and the converter | delete / keep | **delete**: refused where content comes in since M292; no stranger has a sheet; the redesign may break mods |
| F2 | The word for riding along with a place | `attached` / `riding` / `follow` | **`attached`** (the engines' word; `follow` is already a motion's word for a path) |
| F3 | The fist, bat and sword when the clip's markers never come (the review's D-1) | (a) `wait for X or S` everywhere: a swing also waits out its recovery; (b) keep the copies; (c) a word to end early | **(a)**, with `fist.cbtest` updated |
| F4 | `inventory.start` (the review's D-3) | the item's / the game mode's | **the game mode's**, in M297 |
| F5 | `item.ruled` | a param / derived from a sheet with a use motion | **derived**, in M296 |

## 7. Risks

| Risk | Answer |
|---|---|
| M293 changes a rule nobody notices | the golden files hold every rule with its line numbers; the CI re-bakes every mod and diffs; `Meaning` compares every sheet |
| Words no shipped sheet uses lose their meaning in the rewrite | M293 adds a check sheet that uses every word of `sheets.md` (tap, hold, release, cancels, shared cooldowns, remaining) and records its golden file *before* the rewrite |
| M294 breaks published items | every mod is baked and republished in the milestone, as M285 did |
| The step grows | M296's rows are independent: one that is not worth its cost moves to the leftovers |
