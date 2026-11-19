# Issue #24 — Cheats in the in-game menu's new Mods section

Date: 2026-11-18. Branch: `fix/issue-24-cheats` (engine, from `aot-shader-cache` at e461b4e).

The request (https://github.com/DivinusX/a-port-of-the-order-1886-thats-actually-alive/issues/24):
"cheats feature... at insert button feature....for UNLIMITED HEALTH......UNLIMITED AMMO.....
ONE HIT KILL.....NO RELOAD....ALSO OTHERS IF POSSIBLE".

## What ships

F1 > **Mods** tab (the menu's second tab, next to Graphics; Q/E, L1/R1 or mouse to switch):
five toggles, applied to the loaded image while the game runs and kept in
`graphics.ini` (`cheats=<ids>`), reapplied at the next start. Each cheat is a handful of
instruction sites whose expected bytes are checked before any byte is written — the same
rule as the preparation recipes (#15): a build that differs is refused, the row says why.
The pages take their segment's own protection flags back after the swap
(`runtime_guest_patch` in `src/probe.c`).

| Row | Sites (APP_VER 01.02 RVAs) | Edit | Source |
| --- | --- | --- | --- |
| Infinite health | `0xa1059b` | `vmovss [r12+0xd0], xmm0` → store the max-health register instead | Wander_9.00 (GoldHEN CUSA00785 table), reworked cave-free |
| Infinite ammunition | `0x752542`, `0x55520f` | `sub eax, ebx` → nop | Fullcodes (GoldHEN CUSA00100 table) |
| No reload | `0x70c038` | `sub [rdx+0x310], eax` → nop | Fullcodes (GoldHEN CUSA00100 table) |
| Infinite throwables | `0x6e7c0b` | `dec eax` → nop | Fullcodes (GoldHEN CUSA00100 table) |
| Enemy one-hit kill | `0xd5e76e` | `vmovss xmm0, [rax+0xd0]` → store form | Fullcodes (GoldHEN CUSA00100 table) |

The health site and its clamp (`0xa10550`–`0xa1059b`: `vmaxss`/`vminss` against `+0xd8`/`+0xd4`)
were read from the decoded image with capstone; the table's own code-cave variants
(`0x7fc4bdc0`, `0x15b9f50`) fall in a hole between segments and cannot ship as image edits —
the same-size ModRM re-encodings replace them.

## Evidence

- Byte-for-byte site check against the user's real CUSA00785 dump through the production
  decode path (`order1886_patches.decode` over the 01.02 `eboot.bin`): all five recipes'
  expected bytes match; images that differ refuse (the `validate_recipe` message names the
  offset).
- `cheats-test` (new, in `build.sh --test`): state machine — Off/On/Unknown, refusal
  without half-patching, graphics.ini id round-trip, table well-formedness.
- `perf-overlay-test` extended for the settings side (graphics.ini gains `cheats=`).
- Python suite: 630 tests, the same two pre-existing errors as the base (cache-reuse and
  display-preparation tests; untouched by this change). `patch_fingerprint` now sorts by id —
  two selected recipes used to raise `TypeError` in `sorted`; covered by a new test.
- Headless boot (gamescope, RX 9070 XT, CUSA00785 dump, fresh state folder) with
  `cheats=unlimited-health,unlimited-ammo,no-reload` pre-saved — `logs/…-boot.log`:
  ```
  Quality: Medium preset, … cheats unlimited-health,unlimited-ammo,no-reload (…/graphics.ini)
  Cheats: unlimited-health applied at start.
  Cheats: unlimited-ammo applied at start.
  Cheats: no-reload applied at start.
  Mapped 27019324 bytes, 6 segments; applied 70255 relocations
  ```
  The game booted and rendered with the cheats in place; no GPU errors, no crash.
- Menu shape after owner review (screenshot): the F1 menu is two tabs — **Graphics** and
  **Mods** — switched with Q/E (keyboard), L1/R1 (pad) or mouse; the last tab is remembered
  (`menu_tab`, runtime.ini). Mods rows toggle live; the footer shows the `Tabs Q E` hint.
  Relaunched visibly with the same state folder: the three saved cheats again applied at
  start, confirmed in the new boot log.

## Rework after the owner's first playtest (same day)

First table shipped: health as an unconditional store-max at the ApplyHealth site, one-hit
kill as a load→store byte flip. The owner's report: the health edit also applied to
enemies (they stopped dying), not visibly to the player; one-hit kill did nothing.

That ApplyHealth routine (`0xa1051f`) is the generic per-entity path — the published
tables only force health behind a marker test (`cmp word [entity+0x30], 0xF520`). The
reworked table:

| Row | Sites | Edit |
| --- | --- | --- |
| Infinite health | `0xa1059b` + cave `0xf0102d` (39 B) | Wander_9.00's own logic re-hosted in-image: marker match → store 600.0f, otherwise the original `vmovss` — the player only |
| Infinite ammunition | `0x752542`, `0x55520f`, `0x70c038` | the three round-count edits merged into one row (the separate No-reload row is gone; its id is ignored by older graphics.ini files) |
| Infinite throwables | `0x6e7c0b` | unchanged |
| Enemy one-hit kill | `0x9f9240` + cave `0xf01060` (41 B) | TakeDamage hook: marker match → original `vsubss`, otherwise new health := 0 before the ApplyHealth vtable call |

Both caves live in the image's own int3 alignment sea (`0xf0102d..0xf010ab`, 126 bytes,
executable text, referenced by nothing); their expected bytes are the sea's `0xCC`s, so
toggling off hands the padding back exactly. Cave bytes assembled and disassembly-checked
against the real image; every site and displacement re-verified through the production
decode path. `cheats-test` passes against the new table.

## The title-demo crash is pre-existing

While bisecting, a deterministic `Fault (signal 11) at RIP 0x7cdbd05a5` appeared at the
title screen. Control run with the same build and **`cheats=` empty**: same log line,
same RIP (`logs/20261118T212224…-boot.log`, ~115 s). The cheat patches were never
applied in that run, so the instability is the engine's, at the title demo specifically
(the handoff's "PM4 type-1 crash is pre-existing" note matches). It does not gate this
feature; it belongs in its own issue.

## Health v1 crashed the boot in 9 seconds — clamp redesign

Owner playtest: toggling Infinite Health in F1 crashed the session within 4–8 s, twice,
with the same wild RIP as the pre-existing title crash. Boot-time timing made it
deterministic: `cheats=unlimited-health` from start → `Fault (signal 11) at RIP
0x7cdbd05a5` at **9 s, twice**; `cheats=` none → 115 s; one-hit-kill only → 77 s (inside
the title crash's 40–120 s noise band). The 600.0f overwrite — copied from Wander's
table, which behaves on PS4's laxer memory — clobbers spawn/initialization health values
on this runtime and the scripts then act on corrupted actors.

The health cave now clamps instead: for the marker entity, `vmaxss xmm0, xmm0,
[r12+0xd0]` keeps the higher of current and incoming value before the store. Damage is
ignored; healing, pickups and first-write spawn values pass through unchanged. 39-byte
cave at 0xf0102d re-assembled and capstone-verified; `cheats-test` passes; boot timing
with the clamp: recorded below when the run completes.

## v3 after the second playtest (same evening)

Owner: health and throwables did nothing; the Graphics tab still overflowed.

- (Corrected later, see "Final" below: `0x6e7c0b` is not dead code — `je 0x6e7c0b` at
  `0x6e7b7c` reaches it; it is the second throw path.) The decrement patched here is
  `dec dword [rdi+0x314]` at `0x6ed30d`, on the inventory manager at `[entity+0x950]`.
- The player's damage routine (function at `0x9f91df`, victim from `[r13+0xd8]`) keeps
  authoritative health in the 64-bit store `[rdi+0xf0]` and computes the ApplyHealth
  argument with `vsubss xmm0, xmm1, xmm0` at `0x9f9252` — four bytes replaced with
  `movss xmm0, xmm1`: damage never leaves the player, no cave, enemies untouched.
- One-hit kill now hooks the generic ApplyHealth store (`0xa1059b`, cave `0xf0102d`,
  60 B) with a damage guard: `vucomiss` incoming vs current, only a real decrease on a
  non-player entity zeroes health, so spawn writes and heals pass through.
- F1 menu scrolls: row positions precomputed, the focused row is kept inside the list
  area, scrolled-out rows are neither drawn nor clickable.

All seven sites re-verified byte-for-byte against the real image; `cheats-test` passes.
Commit `5785038`. Acceptance: owner relaunches with this build and re-tests in gameplay.

## Final (same night): v7 health, second throw path, one-hit kill rebuilt, menu redesign

Edition: **CUSA00785 – USA, updated to 1.02 (APP_VER 01.02)** — the owner's mandatory dump.

Method: a diagnostic build (not committed; kept as `~/Documents/PS4/order1886-cheats-with-diagnostics.patch`)
logged every ApplyHealth call (victim, caller, health, listener objects) and ran a memory
scanner with perf_event hardware write-watchpoints inside the game process.

- **Health v7.** The player and enemies share the health class and its SetHealth, so the v5
  vtable test changed nothing. The first health listener at `[comp+0x100]` tells them apart:
  every AI character carries the AI listener (first field `0xa3dd20`); the player carries a
  HUD listener that differs by chapter (`0x4c0f10`, `0x578650` — v6 keyed on the first and
  failed in another save). Protected = listener not the AI one and max health ≥ 250 (player
  450, enemies 100–125). Damage routine `0x9f924a` → cave `0xef9d30` (65 B, two edits).
- **Throwables.** Grenades behave like a weapon: HUD left = in hand, right = reserve
  (`[weapon+0x314]`). Besides `0x6ed30d`, the second throw path `dec eax` at `0x6e7c0b`
  (reached by `je` at `0x6e7b7c`) stores reserve − 1; patched to a 2-byte nop.
- **One-hit kill.** The old cave stored 0 and then stored the original xmm0 over it, so it
  never killed anything, and it keyed on the GoldHEN `0xF520` marker that this build does
  not carry. Rebuilt at `0xf01030` (73 B, two edits) with the v7 player test; the zero is
  the value stored. Jump at `0xa1059b`.
- `SetEnabled` writes caves before the jumps into them and restores in reverse.
- F1 menu redesigned (pills, switches, chips, description card); the Mods tab states that
  mods apply automatically and are re-applied at every start.

Owner acceptance in play (CUSA00785 1.02): infinite health, ammunition, throwables (aimed and
hip throws) and one-hit kill all confirmed; the session log showed every enemy hit ending at
0 hp while the player stayed at 450. Open: a health drop 450→297 once in a session that did
not pass through the damage routine or ApplyHealth (health regenerated); two GPU ring
timeouts at load time on cheats builds during the evening (devcoredumps kept), none in the
last sessions.
