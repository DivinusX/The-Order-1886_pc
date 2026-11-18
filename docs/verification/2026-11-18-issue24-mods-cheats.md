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
- Not yet qualified: per-cheat gameplay acceptance in a real session (does health hold, do
  rounds stop counting, do hits kill in one shot), the live toggle mid-chapter, Windows.
