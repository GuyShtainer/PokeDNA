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

## Usage

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
required flags on `celldiff.py`; `--mgba-vendor` on `oracle.py` falls back to
`$GB_ROUNDTRIP_MGBA` (same env var `tools/gb_roundtrip.py` itself honors) and is
required only if that is unset. Neither script embeds any ROM, save, or pixel data;
point them at your own dumps.
