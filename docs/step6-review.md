# Step 6 review (M283 to M292), checked

Written 2026-11-26 at `3251afc` (branch `step6b-plan`), after step 6 and before 6b. It is an honest
review of step 6 and the project around it, with every claim checked against the code. Claude built
step 6 (decision D12), so much of this is a review of its own work.

**Status words used below**

| Status | Means |
|---|---|
| **Confirmed** | checked in the code, a tool, or a test run; the evidence is given |
| **Corrected** | the first review said it differently; the corrected version is here |
| **Withdrawn** | the first review was wrong; kept here so the mistake is visible |
| **New** | found while checking |

## Contents

1. [How it was checked](#1-how-it-was-checked)
2. [In short](#2-in-short)
3. [Corrections to the first review](#3-corrections-to-the-first-review)
4. [What is good](#4-what-is-good)
5. [Step 6: the compiler](#5-step-6-the-compiler)
6. [Step 6: shipped sheets against step 6's own fixes](#6-step-6-shipped-sheets-against-step-6s-own-fixes)
7. [Step 6: gaps and stale docs](#7-step-6-gaps-and-stale-docs)
8. [Step 6: the language, more intuitive](#8-step-6-the-language-more-intuitive)
9. [Step 6: engine limits](#9-step-6-engine-limits)
10. [Step 6: process](#10-step-6-process)
11. [The rest of the project](#11-the-rest-of-the-project)
12. [What to do, in order](#12-what-to-do-in-order)
13. [Decisions for the owner](#13-decisions-for-the-owner)
14. [Appendix: the experiments](#appendix-the-experiments)

---

## 1. How it was checked

- Read `docs/step6-plan.md`, `docs/step6b-plan.md`, `ROADMAP.md`, `docs/sheets.md`, `docs/HISTORY.md`.
- Read the code: `src/sheet/`, `src/sim/motions.cpp`, `src/sim/anim_graph.cpp`,
  `src/server/game_server.cpp`, `src/server/character_item.cpp`, `src/server/mod_api.cpp`,
  `src/server/sheet_test.cpp`, `src/godot/sheet_motions.cpp`, the shipped `.cbsheet`, `.cbtest` and
  `motions/*.cfg` files.
- Rebuilt `cb_sheet` at HEAD (clang-release).
- Ran every shipped `.cbtest`: **23 tests in 16 files, all pass**.
- Ran `cb_sheet check` on 14 small sheets written to probe the language ([appendix](#appendix-the-experiments)).
- Tried one proposed fix for real: in a throwaway git worktree, re-baked the brawl mod with
  Godot 4.7.2 headless (`bake_items.gd`), then ran its sheet test. The baseline bake matched the
  committed files byte for byte. The worktree was then removed; the working tree was never touched.

## 2. In short

- **The foundations are strong**: determinism CI, the decision records, the security story, headless
  sheet tests, a careful migration from version 0.
- **Step 6 shipped the language, not the compiler the plan described.** Version 1 is rewritten as
  text into version 0, parsed by one 1,366-line function, baked to `.cfg` text, and parsed again by
  the simulation. The server runs the baked `.cfg`, not the sheet.
- **The shipped sheets mostly don't use step 6's own fixes**: hand-made cooldown timers, numbers
  written twice, `as holder` on 13 of 14 item reactions.
- **Several step 6 words have no shipped user**, and no step 6 milestone records a playtest. The
  roadmap's own rule asks for both.
- **Do a small cleanup and a "step 6.5" (the real compiler) before 6b**, because 6b makes the sheet
  compiler the reader for strangers' content.

## 3. Corrections to the first review

| # | First review said | Status | What is true |
|---|---|---|---|
| C1 | `dash_strike` has a hidden order dependency: two motions on `use` both check `brawl.cd_dash <= 0`, and the first sets it | **Withdrawn** | Conditions read the board as it was when the tick began (`src/sim/motions.cpp:1867-1869`, since M291). Order does not matter. What is left is the repeated condition (§6, S3) |
| C2 | Removing the copied `timeout` in the fist, bat and sword is "one line each" | **Corrected** | The rewrite bakes fine, but it **changes timing when the clip's markers never come**. Old: after the timeout's find the motion ends. New: it also waits out the recovery (`ready or 0.7`). The fist's own test fails: `fist.cbtest:16: brawl.finisher.hit was not said from 1.4 to 2 s (never)`. It is a decision, not a cleanup ([D-1](#13-decisions-for-the-owner)) |
| C3 | CI runs "six compilers" | **Corrected** | Six builds: Clang, GCC and MSVC on Windows; GCC and Clang on Linux; Clang on macOS arm64 (`.github/workflows/determinism.yml:28-33`) |
| C4 | `create X under Muzzle` against `at Muzzle` are two ways to say where | **Revisited** (the owner's question, 2026-11-26) | They are two meanings: `under P` makes it a child of P (it follows P and turns with it); `at P` takes P's *position* once and leaves it in the world (no rotation). But the split is poor: `at follow` also makes a child (of the subject), overlapping `under`; `under` exists only on `create`; and `at P` with `create ... under Q` drops `under` silently (a placed thing is put under the director: `cue_reaction.cpp:822-866`; `cb_sheet check` passes it). **Decided: one way to say where**, `at PLACE` plus a word for riding along ([step 6.5](step6.5-plan.md), M296) |
| C5 | The frag's falloff needs `let` as a value | **Corrected** | Today's `let` (text replacement) can already name it; the frag just doesn't use it |
| C6 | Booleans are written as 1 and 0 | **Corrected** | The expression reader knows `true` and `false` (`src/expr/expr.cpp:443`); only the shipped sheets write `= 1`. Style, not a missing feature |
| C7 | "Every item has a sheet test" | **Corrected** | Every item sheet does (16 of 16). The motion sets (`dash_moves`, `flight_moves`) have none |
| C8 | The server "trusts" the modder's `.cfg` | **Corrected** | It runs it, and the simulation's compiler still checks it. The point stands as "the sheet a reader sees is not what runs" (§5, A4) |

## 4. What is good

| What | Evidence |
|---|---|
| Determinism held to bit-exact across three compiler families and three OSes, with reference hashes in CI | `determinism.yml`, `tests/reference_hashes.txt` |
| A tick's cost is bounded on purpose (finds, steps, ifs, repeats) | `src/sim/motions.h:233-235`, `:415` |
| The decision records: "Routes not taken", "Decided earlier", decision tables, "the code wins" | `ROADMAP.md` |
| A simple security story: no scripts, data that is checked, cooked items to close the pack hole | `ROADMAP.md` 6b, `docs/step6b-plan.md` |
| `.cbtest`: plain-text tests that run the real server headless, in CI | `src/server/sheet_test.cpp`, `tests/net_test.cpp:9099`; 23 of 23 pass at HEAD |
| A careful migration: the converter refuses to write a sheet that would mean something else; version 0 is refused where content comes in | `src/sheet/sheet_v1.h` (`ConvertToVersion1`), `src/server/mod_api.cpp:73` |
| The language reads much better: `react`/`motion` say who runs a block; words instead of `^^:`, `$at`; statuses; `uses`; `let` | the shipped sheets |
| The telekinesis is 26 lines of sheet (comments aside) for a pull, a hold, a wall break and a throw | `server_mods/brawl/client/prefabs/telekinesis.cbsheet` |
| The housekeeping list names its own debt honestly | `ROADMAP.md`, Housekeeping |

## 5. Step 6: the compiler

| # | Finding | Status | Evidence |
|---|---|---|---|
| A1 | The plan promised one pipeline: tokens with indentation, a syntax tree, name resolution, checks, then lowering straight to `sim::MotionSet`, with the `.cfg` text gone | Confirmed | `docs/step6-plan.md` §8.1, §8.3 |
| A2 | What was built is a chain of text rewrites: `ComposeSheet` (pastes in the `uses` sheet) → `ApplyLets` (word replacement) → `LowerVersion1` (text to version 0) → `ParseVersion0` → `BakeSheetMotion` (writes `.cfg` text) → the simulation's `CompileMotionSet` parses that text again | Confirmed | `src/sheet/sheet_v1.cpp:934, 1423, 1609, 1724`; `src/sheet/sheet_bake.cpp:477, 1062`; `src/sim/motions.cpp:280` |
| A3 | The long functions grew instead of going: `ParseVersion0` is **1,366 lines** (the old `ParseRules` was 1,198); `CompileMotionSet` went from **957 to 1,243**; `motions.cpp` from 1,728 to 2,276 lines | Confirmed | `sheet_rules.cpp:821-2186`; `git show 551687a^` |
| A4 | **The server reads one sheet from two places**: its declarations from the sheet's text, its motions from the baked `motions/<set>.cfg` in the item. Nothing checks that the two agree | Confirmed | `src/server/mod_api.cpp:79`; `src/server/game_server.cpp:98-116`; `src/server/character_item.cpp:551` |
| A5 | The 6b plan keeps both: a cooked item carries `motions/<kind>.cfg` "unchanged" *and* the sheet's source. That contradicts D4 ("the item carries the source; motions compiled once by the server") and step 6 §8.3. **Nuance found when planning 6.5**: the `.cfg` also holds the scene's parts the sheet names, already resolved (`find Blast` is `shape 8 0 0 0`), which a server without Godot cannot read from a scene. So the fix is "the sheet plus its parts as data", not "the source only" | Confirmed (6b plan since updated) | `docs/step6b-plan.md` (as written 2026-11-26); `server_mods/frag/client/motions/frag.grenade.cfg` |
| A6 | `cb_sheet check` only parses. A scene's nodes, and everything the simulation's compiler checks, are the bake's (the editor's). So the command line, the VS Code problem matcher and the Sheet screen's mistakes pass things the bake later refuses. CI is safe: its Godot job re-bakes every mod and diffs the result | **New** | `src/sheet/cb_sheet.cpp:13-14, 134-150`; `scripts/godot_checks.sh:70-86` |
| A7 | `.cbtest` runs the **committed bake**, not the sheet. Edit a sheet without re-baking in Godot, and `cb_sheet test` tests the old motions | **New** | `src/server/sheet_test.cpp:675` (`LoadMotionsFolder`) |
| A8 | `let` is text replacement with no check on keywords. `let up = 5` turns `along up` into `along 5`, read as a marker named "5", and `cb_sheet check` passes it. `let range = 2` gives a confusing grammar error, not "range is a word of the language" | Confirmed (experiment) | `sheet_v1.cpp:934-974`; [appendix](#appendix-the-experiments) |
| A9 | Errors name a line, not a word | Confirmed | `ROADMAP.md` leftovers, Sheets and HUDs |
| A10 | The plan's rule "a sheet with errors compiles nothing" is not kept everywhere: an unknown name in `blocked by` passes `cb_sheet check` and is only a log line when the server starts | Confirmed (experiment) | `motions.cpp:1668`; `game_server.cpp:111-116` |

**Why it matters for 6b.** The 6b plan says the sheet compiler is the reader for sheets, fuzzed in
CI. Fuzzing a chain of text rewrites finds crashes, but it can't tell you what the language
*means*. Once strangers write version 1 sheets, the rewrite chain's quirks become the de facto spec.

## 6. Step 6: shipped sheets against step 6's own fixes

| # | Finding | Status | Evidence |
|---|---|---|---|
| S1 | **The copied timeout (P11) is still in the fist (twice), the bat and the sword.** The frag uses `wait for X or S`, which works anywhere since M287. The copy is not pure duplication, though: it also means "without the marker, skip the recovery" (C2) | Confirmed, corrected | `fist.cbsheet:32-40, 47-57`; `bat.cbsheet`; `sword.cbsheet`; `docs/sheets.md:242-247` |
| S2 | **The brawl skills keep hand-made cooldowns** (`field brawl.cd_X timer`, `if brawl.cd_X <= 0`, `change brawl.cd_X = N`) in `dash_strike`, `slam` and `uppercut`. P10 and M287's own "done when" said "brawl's `cd_*` fields gone; the HUD lines read cooldowns". The cause is a listed leftover: a HUD cannot read `cooldown NAME` | Confirmed | `docs/step6-plan.md` §12 (M287 row); `ROADMAP.md` leftovers |
| S3 | `dash_strike` writes the same condition on two motions. The roadmap's own step 6 row ("a cast and the owner's own effects in one step: `dash_strike.cbsheet` needs two blocks with a copied condition") was not done | Confirmed | `dash_strike.cbsheet:12, 20`; `ROADMAP.md:394` |
| S4 | The dash strike's HUD colours by arithmetic: `bind modulate:r = 1 - 0.45 * (brawl.cd_dash <= 0)` | Confirmed | `brawl/client/ui/dash_strike_hud.cbsheet` |
| S5 | **Numbers written twice (P8)**: `rounds = 12` and `magazine = 12` (pistol), the same with 30 (rifle). A field's start value cannot name a param (tried: refused) | Confirmed (experiment) | `pistol.cbsheet`, `rifle.cbsheet` |
| S6 | Numbers written twice: the pistol's predicted mark uses `cooldown 0.95` beside `param mark.seconds 1 s`, which the mark mod reads | **New** | `pistol.cbsheet:36-37`; `server_mods/mark/mark.cpp:59` |
| S7 | The frag's falloff is still written twice, though `let` exists and the roadmap lists it ("the frag's falloff is written out twice") | Confirmed | `frag.cbsheet`, Blast; `ROADMAP.md:393` |
| S8 | **`as holder` on 13 of 14 reactions in item sheets.** The plan's "defaults from where the sheet sits" was not built, so every item reaction is nested three deep | Confirmed | `pistol` 8/8, `sword` 3/3, `bat` 2/3; `sheet_v1.cpp:745-755` (no default subject) |
| S9 | Units used inconsistently: `cooldown 0.2` (gun), `0.3` (bat), `0.4` (sword), `4 s` (telekinesis) | Confirmed | the sheets |
| S10 | `at look brawl.tk.reach` written four times in the telekinesis (break, grab, toward, find); `let` would do it today | Confirmed | `telekinesis.cbsheet` |
| S11 | `Dry` and `Reload` repeat the same three reload lines; a motion cannot have two triggers (tried: refused) | Confirmed (experiment) | `gun.cbsheet`; [appendix](#appendix-the-experiments) |

## 7. Step 6: gaps and stale docs

Gaps the plan or the roadmap listed for step 6 that are neither done nor in the leftovers:

| # | Gap | Status | Evidence |
|---|---|---|---|
| G1 | `per second` (said rates) is not a word. `change f.x -= 30 per second` passes `cb_sheet check` (the value is taken as an expression's text) and is not implemented | Confirmed (experiment) | `ROADMAP.md:391`; `sheet_bake.cpp:203-224`; `docs/sheets.md:498` |
| G2 | `every` is still capped at 64 (P18). The plan's §9 said "drop the constant" | Confirmed | `motions.h:415`; `rifle.cbsheet` comment; `ROADMAP.md:369` |
| G3 | "One rule for expressions: one word bare, anything longer in brackets" is not enforced: `impulse 2 * 3 along facing` passes `cb_sheet check` | Confirmed (experiment) | `ROADMAP.md:392` |
| G4 | A reaction's default subject from where its sheet sits | Confirmed | S8 |
| G5 | The `.cfg` path was meant to go | Confirmed | A4, A5 |
| G6 | `CbItem` still has `use_conditions`, `use_changes`, `use_cooldown` and `use_emits` in the inspector: a second place for an item's use, beside its sheet. No shipped scene sets them. (`IsItem()` uses `use_conditions` as its test for a `CbItem`, so it needs another test first) | Confirmed | `src/godot/cinderbox_item_look.cpp:127-138`; `src/godot/sheet_motions.cpp:102-124` |

Stale text:

| # | Where | Says | True |
|---|---|---|---|
| T1 | `ROADMAP.md:82` | step 6 **started** | done (the same cell says so) |
| T2 | `ROADMAP.md:399` | "the VS Code grammar not made" | made in M291: `tools/vscode-cbsheet/` |
| T3 | `ROADMAP.md:880` | `ParseRules` 1,198 lines "(step 6 replaces it)", `CompileMotionSet` 957 | `ParseVersion0` 1,366; `CompileMotionSet` 1,243 |
| T4 | Every baked `motions/*.cfg` (17 of 17) | "Baked from the CbMotion nodes ... Edit the item's scene and bake again" | baked from the sheet; motion nodes deleted in step 3c. Written by `cinderbox_item_look.cpp:1346`, `cinderbox_motion.cpp:67` |
| T5 | `docs/sheets.md:398` | example `impulse 9 * curve(Falloff, reach) along away` | `curve()` is refused (the same page, line 392). `cb_sheet check` passes the example anyway |
| T6 | `sword.cbsheet` | `# SparksOnHit` sits above `Reeling` | a converter leftover; the reaction is further down |
| T7 | `bat.cbsheet` | `# FlamesOnSwing` with nothing under it | a heading left behind |
| T8 | `ROADMAP.md`, step 6, "Found by step 5" | the "Today" column | describes the code before step 6 |

## 8. Step 6: the language, more intuitive

| # | Today | Problem | Change | Status |
|---|---|---|---|---|
| L1 | `blocked by punch, guard, pulled` | Tags, motions and statuses share one namespace, resolved in that order (tag, then motion, then status). A typo is a log line | Declare tags (`tag punch`) as statuses are declared; an unknown name is an error in `cb_sheet check` | Confirmed (`motions.cpp:1650-1668`) |
| L2 | `force 60 kind velocity top 12 toward ...` | "60" is a gain toward the speed `top`; nobody can guess that | `steer toward POINT speed 12 strength 60` (or similar) | Confirmed (`docs/sheets.md:304`) |
| L3 | `react on item.hit if event.value > 0` | The event's value (damage) stands for "hit a player". **Edge case**: `--mod-option gun.zone.<zone>=0` makes a player hit read as a wall (ricochet, no hit marker) | A word for what was hit that doesn't change the subject; today `is` must sit under `as SUBJECT` | **New** (`gun.cpp:248-269`; `sheet_v1.cpp:757-763`) |
| L4 | `when using.gun.rifle` in the rifle's own sheet | The item names its own kind; a third automatic gun would copy and edit it | A kind-free word: "this item is being used" | Confirmed |
| L5 | `gun.reloading` in motions, `hand.gun.reloading` in looks | Two names for one field | One name; the compiler knows the context | Confirmed (`docs/sheets.md:89`) |
| L6 | `field gun.rounds int on item = 12` written again in the pistol | Overriding a start value by declaring the field again | `field ... = PARAM` (S5), or a `set` line | Confirmed |
| L7 | `param inventory.start 1` in every item | Whether every life starts with an item is the game mode's decision, not the item's ("gameplay rules belong to mods") | Move it to the preset or mode mods | Confirmed (`docs/items.md:237, 251`) |
| L8 | `param item.ruled 1` (bat, sword; the gun's C++) | A flag dressed as a number. Proposal: a sheet with a use motion already says the item has rules of its own; the compiler could derive it | Derive it, or name it as a flag | Confirmed (`server-mods.md:83`; `item.cpp:65`) |
| L9 | One trigger per motion | `Dry` and `Reload` copy the reload | `motion Reload on press reload, on item.use_failed` | Confirmed (refused today) |
| L10 | `along world (0, -1, 0)` | No `down`. `along down` is read as a marker named "down" and passes `cb_sheet check` | Add `down` (and `back`?) | Confirmed (`sheet_rules.cpp:403, 452`) |
| L11 | The telekinesis's `Throw` runs a fresh `find` | It throws whatever is in the sphere, not what `Hold` grabbed | `throw held ...`: act on the grab's set | Confirmed |
| L12 | `change gun.reloading = 1` | Booleans as numbers | Write `true`/`false`; the formatter could | Corrected (C6) |
| L13 | `active X` is capped at the first 64 motions (E2); `ready`/`cooldown`/`uses X` are not | Two rules for one family of words | One rule | **New** (`anim_graph.cpp:206, 228`) |
| L14 | `create X under P`, `at P`, `at follow` | Where and "rides along" said in two syntaxes; `under` dropped silently inside an `at` (C4) | `at PLACE` only, plus a word for riding along | **New** (the owner, 2026-11-26) |

## 9. Step 6: engine limits

| # | Limit | Why it bites | Status | Evidence |
|---|---|---|---|---|
| E1 | **Tags are global across every mod a server runs, 32 at most.** A tag past 32 is silently dropped from the motion that has it; a motion blocked by it only gets a log line | More mods, fewer tags left; which tag loses depends on load order | Confirmed | `motions.cpp:1627-1668` (cap at `:1634`) |
| E2 | **`active X` / `motion.X` reads a 64-bit mask over the server's global motion list.** Motion 65 and up reads as 0, with a warning | The shipped mods already have **37** motions; under twice today's content, some items' `active` silently stops working, depending on which mods load | Confirmed | `anim_graph.cpp:228-238`; `motions.cpp:1581-1590`; count of `motion` lines in `server_mods/*/client/motions/*.cfg` |
| E3 | `every` repeats 64 times at most | an automatic gun with more than 64 rounds | Confirmed | G2 |

## 10. Step 6: process

| # | Finding | Status | Evidence |
|---|---|---|---|
| P-1 | Ten milestones in about 26 hours: M283 committed 2026-11-21 22:35, M292 merged 2026-11-23 00:17 | Confirmed | `git log` |
| P-2 | The roadmap's rule: "a milestone is done when shipped content uses it **and it survived a playtest**". No step 6 entry in `HISTORY.md` records a playtest; every "Checked" is automated (bakes, golden files, tests). (The owner may have played without writing it down) | Confirmed | `ROADMAP.md:62`; `docs/HISTORY.md:638-660` |
| P-3 | **Words with no shipped user**: `on tap`, `on hold`, `on release`, `cancels`, shared cooldowns, `remaining`, `ready X`, `cooldown X`, `uses X`. None appears in a shipped sheet or a `.cbtest`; they are tested only in C++ at the `.cfg` level | Confirmed | grep of every `.cbsheet` and `.cbtest`; `tests/test_main.cpp:4739-4838` |
| P-4 | The pattern: new words were added, but the migrations that would use them were not finished (S1, S2, S5, S7, S8, S10) | Confirmed | §6 |
| P-5 | The slice (the game) was paused for steps 5, 6 and 6b; step 1's checklist by hand is still "to do" | Confirmed | `ROADMAP.md:75`, step 4 row |

## 11. The rest of the project

| # | Finding | Status | Evidence | Change |
|---|---|---|---|---|
| R1 | Very large files: `simulation.cpp` 7,823 lines, `game_server.cpp` 3,368 | Confirmed | `wc -l` | the split the housekeeping list already proposes |
| R2 | Tests in two files: `tests/test_main.cpp` 14,954 lines, `tests/net_test.cpp` 9,158 | Confirmed | `wc -l` | one file per area (mechanical; faster builds, easier to find) |
| R3 | The docs work against "fast to scan": 8,650 lines in `docs/`, `ROADMAP.md` and `DESIGN.md`; a table cell of 174 words (`docs/sheets.md:89`); history entries of 300 words in one paragraph (`HISTORY.md:449`, M137) | Confirmed | `wc` | one fact per cell; detail under headings; shorter history entries |
| R4 | Milestone numbers inside content and code: 12 in 7 shipped sheets, about 58 in `simulation.cpp`. Modders will copy the sheets | Confirmed | grep `\bM[0-9]{2,3}\b` | comments say what and why; history stays in git and `HISTORY.md` |
| R5 | Desync traps still open: typed commands, "what a follower skips" in one place | Confirmed | `ROADMAP.md`, Housekeeping | before Luau binds the API (step 10) |

## 12. What to do, in order

**Taken up as [step 6.5](step6.5-plan.md)** (the owner, 2026-11-26), between step 6 and 6b, in
another order: the front end first (M293), so each later change is made once, on the tree. Fixed
with the plan: T1, T2, T3 (`ROADMAP.md`). T8 stays: that table is about what step 5 found.

| # | What | Size | Risk |
|---|---|---|---|
| 1 | **Small cleanup**: HUDs read `ready`/`cooldown`, then delete `brawl.cd_*` (S2, S3, S4); a field's start value from a param (S5); `cooldown mark.seconds` in the prediction, if a prediction may read a param (S6); `let` for the frag's falloff and the telekinesis point (S7, S10); `as holder` as the default in item sheets (S8); units everywhere (S9); delete `CbItem.use_*` (G6); fix T1 to T8 | one milestone | low: the `.cbtest` files cover it |
| 2 | **Hard errors**: unknown names in `blocked by`/`cancels`; a `let` that shadows a word; an unknown `along` word unless the scene has that marker; `per second` and `curve()` refused by `cb_sheet check`, not only by the bake | small | low |
| 3 | **Make `cb_sheet check` and `cb_sheet test` honest**: check runs the simulation's compiler on what it can without a scene; test compiles the sheet instead of reading the committed bake (A6, A7) | small to medium | low |
| 4 | **Step 6.5, the real compiler**: tokens with indentation, a syntax tree with spans, one resolver, lowering straight to `sim::Motion`; delete `LowerVersion1`'s text path, `ParseVersion0`, the `.cfg` text and its loader; the server compiles the item's sheet (A1 to A5). Then fix the 6b plan to carry the source only | large | medium: the 23 sheet tests, the golden files and the CI re-bake are the safety net |
| 5 | **Remove the global caps**: motion state indexed per player or per item instead of a 64-bit global mask (E2); tags per mod, or an error at the cap (E1); `every` without a cap (E3) | medium | medium: touches the hash; the reference hashes will move |
| 6 | **The language changes in §8**, a few per milestone, each with a `.cbtest` | small each | low |
| 7 | **Use or drop the unused words** (P-3): one shipped user each, or say in `sheets.md` that they are reserved | small | none |
| 8 | **Split the tests and `simulation.cpp`; trim the docs** (R1 to R4) | medium | none (a pure move; the hashes prove it) |
| 9 | **Run the playtest gate**, then go back to the slice | — | — |

## 13. Decisions for the owner

D-2 was taken (step 6.5 before 6b, 2026-11-26). D-1 and D-3 moved to step 6.5's decisions (F3, F4).

| # | Question | Options | Leaning |
|---|---|---|---|
| D-1 | The fist, bat and sword without their clip's markers (C2, S1) | (a) `wait for X or S` everywhere: without markers a swing also waits out its recovery (the fist's combo test changes); (b) keep the copies; (c) a new word for "if the strike's wait gave up, end here" | (a): the recovery is the same with and without the pack, and the copies go. Update `fist.cbtest` |
| D-2 | Step 6.5 before 6b (§12, item 4) | before 6b / as 6b's first milestone / not at all | before 6b: the format is meant to be settled with a version (G7) before strangers write it |
| D-3 | `inventory.start` (L7) | the item's / the game mode's | the game mode's |
| D-4 | The unused words (P-3) | keep, with a shipped user each / reserve them in the docs / remove | a shipped user each, when the slice needs one |

## Appendix: the experiments

Probe sheets run through `cb_sheet check` (rebuilt at HEAD):

| Sheet (body) | Result | Means |
|---|---|---|
| `let up = 5` then `impulse 4 along up` | passes | the let replaced a word of the language; `along 5` is read as a marker named "5" |
| `let range = 2` then `cast ray range 4 along facing` | error: the `cast` line's grammar | the right error would be "range is a word of the language" |
| `blocked by atack, guardd` | passes | typos in names are not checked |
| `cooldown 0.4 m` | error: "0.4 m is a distance (m); cooldown takes a time (s)" | units work (M286) |
| `change a.on = true` | passes | `true` is known |
| `impulse 4 along down` | passes | no `down`; read as a marker |
| `change f.x -= 30 per second` | passes | not a word; the value is taken as text |
| `param g.mag 12` then `field g.rounds int on item = g.mag` | error: the field grammar | a field's start value must be a number |
| `motion Reload on press reload, on item.use_failed` | error: the motion grammar | one trigger per motion |
| `every 0.1` then `emit e.x` | passes | (the 64 cap is the simulation's) |
| `wait for e.a or 0.35`, a find, `wait for e.b or 0.7` | passes | `or` works in the middle of a motion |
| `impulse 2 * 3 along facing` | passes | bare expressions are allowed |
| `impulse 9 * curve(Falloff, reach) along away` | passes | `curve()` is refused only later |

The fist with `wait for X or S` (a throwaway worktree, Godot 4.7.2 headless, `bake_items.gd`):

- The baseline bake at HEAD reproduced every committed file (no diff).
- After the rewrite, the bake succeeded. `brawl.fist.cfg` lost both copied finds and their
  `timeout`, and gained `goon` after each wait.
- `fist.cbtest`: "a jab hurts the player ahead" **passes**; "three in a row: the third is the
  finisher" **fails**. The test server has no animation pack, so no markers come, and each jab now
  lasts 0.35 + 0.7 s instead of 0.35 s. The press at 0.7 s comes while the first jab is still on, so
  the press at 1.4 s is only the second jab, not the finisher.
