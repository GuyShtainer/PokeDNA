# gb_oracle — an independent Game Boy screen pixel/tilemap oracle

(BACKLOG #97. Originated as the U4 review's own `docs/shots/rvu4/{oracle.py,celldiff.py}`
— kept here as durable tooling for the *next* GB-screen review, generalized just enough
to run from any clone instead of one reviewer's own machine.)

Two scripts, two independent halves of the same proof:

- **`oracle.py`** boots a REAL Game Boy Pokemon ROM+save in mGBA (via
  `tools/gb_roundtrip.py`'s own driver), navigates to a nav target (default `START >
  ITEM`), and dumps ground truth: the composed 20x18 tilemap (resolved through the
  *actual* LCDC bits the ROM sets, not an assumed addressing mode), a screenshot, and
  a `<tag>.json` grid file. This never reads a single PokeDNA source file — it is
  ground truth from the cartridge, full stop.

- **`celldiff.py`** diffs one of those `<tag>_grid.json` files against a **compiled
  harness that runs PokeDNA's own shipped C painter functions** — not a hand-retyped
  Python model of what they're supposed to draw, the actual compiled output of the
  actual functions, so the comparison can never drift from what ships. Mismatches are
  bucketed by screen region (outside the drawn box, name-text cells with no names
  table this slice, a known blink cell, etc.) so a reviewer can eyeball which
  differences are expected deviations and which are real bugs.

## Building a harness

`celldiff.py` needs a small host binary that calls the real painter functions
(`g1bag_border()`/`g1bag_paint_list()` for the Gen-1 bag screen, or whatever the next
screen's own paint functions are) against minimal stand-ins for `GbScreen`/
`gbscr_cell()`/`gbscr_text()` (these never touch tonc/GBA headers — pure C, same rule
every PokeDNA core follows — so they link into a plain host binary with zero porting).

There is no single "the" harness — each screen's own paint functions need their own
thin `main()` that loads a save, calls the paint function(s) with a given
`(top, sel)`, and prints the resulting 20x18 grid **resolved to absolute Game Boy tile
ids** the same way `oracle.py`'s own grid is (`SRC_TEXTBOX` tile *t* → `0x60 + t`,
`SRC_FONT` tile *b* → *b* as-is, `SRC_BLANK` → `0x7F`). Sketch:

```c
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
/* ... minimal GbScreen/gbscr_cell/gbscr_text stand-ins ... */
#include "body.inc"   /* sed the real paint function(s) out of the shipped .c file */

int main(int argc, char** argv) {
  /* argv[1] = save path, argv[2] = top, argv[3] = sel (or whatever the
   * screen under test actually takes) */
  GbScreen gs = {0}; gs.ok = true;
  the_screen_border(&gs);
  the_screen_paint_list(&gs, /* ... */);
  for (int y = 0; y < 18; y++) {
    printf("   [");
    for (int x = 0; x < 20; x++) {
      int i = y * 20 + x, abs;
      switch (gs.src[i]) {
        case GBSCR_SRC_TEXTBOX: abs = 0x60 + gs.map[i]; break;
        case GBSCR_SRC_FONT:    abs = gs.map[i]; break;
        default:                abs = 0x7F; break;
      }
      printf("0x%02x%s", abs, x == 19 ? "" : ",");
    }
    printf("],\n");
  }
  return 0;
}
```

Build it with any C compiler (`clang -O0 -o harness harness.c`) — it is a throwaway,
per-review build artifact, not something this directory ships (no compiled binaries,
no `body.inc` snippets of any given review's source, checked in here).

**WARNING (U5 D1, found by review-opus ac9ffc0):** the `SRC_TEXTBOX` tile *t* →
`0x60 + t` identity above is a **Gen-1-only** convention — `oracle.py`'s own grid
resolves `GBSCR_SRC_TEXTBOX` through `gu->textbox` (Gen 1's real 2bpp textbox block)
for a Gen-1 capture, but Gen 2 has no such block: `pdna_gbscreen.c`'s own
`gbscr_tile_pixels()` reads `GBSCR_SRC_TEXTBOX` cells through `gu->frames` on Gen 2
(9 frames x 6 tiles, 1bpp, G2-R) instead. A Gen-2 harness/sketch using the `0x60+t`
shortcut will silently compare the WRONG tile identity and can miss a real defect
(this is exactly how U5's own first draft shipped Gen 1's box-corner indices, 25-31,
against a Gen-2 screen that never had them). **Never use the `0x60+t` identity for a
Gen-2 `SRC_TEXTBOX` cell** — resolve it by PIXELS instead: read the six candidate
tiles of `RomGbUi.frames`'s frame 0 (linear index 0..5) via `rom_gbui_tile()` and
compare pixel-for-pixel against the captured cell, the same way `celldiff.py` already
treats `SRC_PIC`/codec cells it cannot express as one flat index.

## `--selftest` and `--demo` (BACKLOG #97 follow-up)

A U2c/U3-review finding motivated two more `celldiff.py` modes: a from-scratch GB-screen
comparator can ship as a **dead stub** — a diff loop that looks right but never actually
appends a mismatch — with nothing mechanical catching it. `celldiff.py`'s own `run()`
loop was checked and does append correctly, but nothing PROVED that, and nothing kept it
proving it on every future change.

```sh
# No ROM/save/mGBA needed -- CI-safe, registered in tests/run_host_tests.py's PY_TESTS
# (via the thin argv-free wrapper tests/host_gb_oracle_selftest_test.py, since PY_TESTS
# entries run with no extra argv).
python3 tools/gb_oracle/celldiff.py --selftest

# The real end-to-end pipeline: Gen-2 trainer card (Gold) + Gen-1 Item bag border (Red).
python3 tools/gb_oracle/celldiff.py --demo --gb-roms roms/gb
```

`--selftest` builds two synthetic 20x18 grids that agree everywhere except one
deliberately-injected cell, runs `diff_grids()` (the same function `run()` calls), and
FAILS unless the reported mismatch set is exactly that one cell — plus a negative half
(two identical grids must report zero mismatches, so a comparator that reports
*something* regardless of input can't pass by accident).

`--demo` is the genuinely real thing, not a smoke test: for each of two screens it (1)
compiles a small throwaway host harness (`harness_g2card.c` / `harness_g1bag.c`, checked
in next to this file) that calls the SHIPPED, UNMODIFIED cell geometry —
`source/g2card_cells.c`'s `g2card_build_upper_cells()` for the Gen-2 trainer card's 51
always-shown static cells, and `source/pdna_gbbag.c`'s `g1bag_border()` for the Gen-1
bag's 90 frame cells (sed-extracted fresh every run, so it can never drift from what
ships) — and resolves every cell to a **real ROM file byte offset** via `rom_gbui_open()`
+ `gbscr_block_off()`, the exact two functions the shipped painter itself uses; (2) boots
the real ROM+save in mGBA (`gb_roundtrip.GbDriver`) and captures the real VRAM tilemap +
per-tile pixel data (`oracle.py`'s own `compose()`/`tile_px()`); (3) diffs BY CELL
POSITION, pixel-for-pixel (never by a flat tile-id identity — see the WARNING above),
and lists every mismatching `(row, col)` with the ROM offset it expected, not just a
count. Run against Guy's own corpus (2026-09-15): **zero mismatches on both screens**
(51 Gen-2 upper-card cells, 90 Gen-1 bag-border cells) — the shipped painter's static
geometry produces bit-identical pixels to the real cartridge.

Both cell sets were picked to need NO save parsing (Gold's protagonist card pic and
Red's bag frame are both save-independent), which is why `--demo` only needs `--gb-roms`
(`Gold.gbc`/`Gold.sav`/`Red.gb`/`Red.sav`) and `--mgba-vendor`/`$GB_ROUNDTRIP_MGBA` — no
`--harness`/`--grids-dir` juggling.

## Named accepted deviations

`ACCEPTED_DEVIATIONS` in `celldiff.py` is a small `{screen_tag: {(row, col): "name"}}`
table — item 4 of the BACKLOG #97 brief. A mismatch at a listed cell still shows up in
the report (never silently dropped) but tagged `ACCEPTED: <name>` instead of landing in
an unexplained bucket, e.g. the original Red-bag list comparator's down-scroll marker at
`(row 11, col 18)`, which blinks every 32 VBlanks and legitimately differs depending on
capture phase. Both `--demo` screens compare only save/time-independent cells, so their
own tables are empty — any mismatch there would be a real defect, not a known deviation.

## Usage (the original `--harness` mode)

```sh
# Ground truth from the real cartridge:
python3 tools/gb_oracle/oracle.py --rom roms/gb/Red.gb --sav roms/gb/Red.sav \
    --out docs/shots/my-review --tag red_row1

# Compare the shipped painter's OWN compiled output against that ground truth:
python3 tools/gb_oracle/celldiff.py --harness /tmp/my-harness/harness \
    --grids-dir docs/shots/my-review --gb-roms roms/gb
```

Neither script guesses a ROM-corpus / grids-directory / mGBA-vendor path from its own
location — a checkout that is not nested exactly like one reviewer's own machine makes
that silently wrong rather than loudly missing (this repo's own docs describe the usual
layout: `docs/kb/pokemon/`, `gba-toolkit/roms.sh`). `--gb-roms`/`--grids-dir` are
required in `--harness` mode (`--demo` needs only `--gb-roms`); `--mgba-vendor` falls
back to `$GB_ROUNDTRIP_MGBA` (same env var `tools/gb_roundtrip.py` itself honors) and is
required only if that is unset. No script here embeds any ROM, save, or pixel data;
point them at your own dumps.
