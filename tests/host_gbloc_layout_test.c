/* host_gbloc_layout_test.c -- BACKLOG #68b review D3: permanent layout proof for the
 * three GB "loc" cache structs (RomGbSpriteLoc, RomGbIconLoc, RomGbUiLoc). tools/
 * fuse_gb.py's LOC payload is a raw byte copy of whichever of these structs
 * gbloc_driver.c wrote (source/fused_gb.c's fused_gb_loc() then hands the GBA build
 * a pointer straight at those bytes, reinterpreted as the struct) -- if the host
 * compiler (which built gbloc_driver.c, since it runs at fuse time on a Mac/Linux
 * box) and the ARM compiler (which reads the bytes back on real hardware/an
 * emulator) ever disagree about a field's offset or the struct's total size, every
 * fused LOC record silently means the wrong bytes with no error anywhere -- the
 * exact class of bug BACKLOG #68b review D3 hunted for and disproved by hand
 * (compiling matching _Static_assert lines with both cc and arm-none-eabi-gcc). This
 * file makes that check permanent instead of a one-off review artifact.
 *
 * Two ways to compile it, both must pass with zero errors:
 *
 *   HOST (wired into tests/run_host_tests.py's host_*_test.c glob):
 *     cc -std=c11 -Wall -I source tests/host_gbloc_layout_test.c -o /tmp/hgbloc && /tmp/hgbloc
 *
 *   ARM (devkitARM cross compiler -- syntax/layout check only, no link, no run;
 *   wired into the Makefile's `delta-gb` target and the `loc-layout` target):
 *     /opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc -mcpu=arm7tdmi -mthumb \
 *       -mthumb-interwork -I source -fsyntax-only tests/host_gbloc_layout_test.c
 *
 * The offsets/sizes below are literal numbers (not re-derived from offsetof() in a
 * loop) precisely so the file expresses "the layout must be EXACTLY this", not "the
 * layout must be internally self-consistent" -- a compiler that silently changed
 * field order or padding would still be self-consistent with itself.
 */
#include <stddef.h>
#include <stdio.h>

#include "rom_gbsprite.h"
#include "rom_gbicon.h"
#include "rom_gbui.h"

/* ---- RomGbSpriteLoc (260 B) -- source/rom_gbsprite.h ---------------------------- */
_Static_assert(sizeof(RomGbSpriteLoc) == 260, "RomGbSpriteLoc size drifted");
_Static_assert(offsetof(RomGbSpriteLoc, id_hash)    == 0,  "RomGbSpriteLoc.id_hash");
_Static_assert(offsetof(RomGbSpriteLoc, size)       == 4,  "RomGbSpriteLoc.size");
_Static_assert(offsetof(RomGbSpriteLoc, gen)        == 8,  "RomGbSpriteLoc.gen");
_Static_assert(offsetof(RomGbSpriteLoc, mew_bank)   == 9,  "RomGbSpriteLoc.mew_bank");
_Static_assert(offsetof(RomGbSpriteLoc, stored_lo)  == 10, "RomGbSpriteLoc.stored_lo");
_Static_assert(offsetof(RomGbSpriteLoc, pad)        == 11, "RomGbSpriteLoc.pad");
_Static_assert(offsetof(RomGbSpriteLoc, base_stats) == 12, "RomGbSpriteLoc.base_stats");
_Static_assert(offsetof(RomGbSpriteLoc, mew_stats)  == 16, "RomGbSpriteLoc.mew_stats");
_Static_assert(offsetof(RomGbSpriteLoc, base_data)  == 20, "RomGbSpriteLoc.base_data");
_Static_assert(offsetof(RomGbSpriteLoc, pic_ptrs)   == 24, "RomGbSpriteLoc.pic_ptrs");
_Static_assert(offsetof(RomGbSpriteLoc, unown_ptrs) == 28, "RomGbSpriteLoc.unown_ptrs");
_Static_assert(offsetof(RomGbSpriteLoc, palettes)   == 32, "RomGbSpriteLoc.palettes");
_Static_assert(offsetof(RomGbSpriteLoc, dex_order)  == 36, "RomGbSpriteLoc.dex_order");
_Static_assert(sizeof(((RomGbSpriteLoc*)0)->dex_order) == 190,
               "RomGbSpriteLoc.dex_order size");
_Static_assert(offsetof(RomGbSpriteLoc, bank_map)   == 226, "RomGbSpriteLoc.bank_map");
_Static_assert(sizeof(((RomGbSpriteLoc*)0)->bank_map) == 32,
               "RomGbSpriteLoc.bank_map size (ROM_GBSPRITE_BANKMAP drifted)");

/* ---- RomGbIconLoc (20 B) -- source/rom_gbicon.h --------------------------------- */
_Static_assert(sizeof(RomGbIconLoc) == 20, "RomGbIconLoc size drifted");
_Static_assert(offsetof(RomGbIconLoc, id_hash)         == 0,  "RomGbIconLoc.id_hash");
_Static_assert(offsetof(RomGbIconLoc, size)            == 4,  "RomGbIconLoc.size");
_Static_assert(offsetof(RomGbIconLoc, mon_menu_icons)  == 8,  "RomGbIconLoc.mon_menu_icons");
_Static_assert(offsetof(RomGbIconLoc, icon_pointers)   == 12, "RomGbIconLoc.icon_pointers");
_Static_assert(offsetof(RomGbIconLoc, n)               == 16, "RomGbIconLoc.n");
_Static_assert(offsetof(RomGbIconLoc, icon_bank)       == 17, "RomGbIconLoc.icon_bank");
_Static_assert(offsetof(RomGbIconLoc, pad)             == 18, "RomGbIconLoc.pad");
_Static_assert(sizeof(((RomGbIconLoc*)0)->pad) == 2, "RomGbIconLoc.pad size");

/* ---- RomGbUiLoc (116 B) -- source/rom_gbui.h ------------------------------------
 * BACKLOG #71 grew this from 68 to 108 B: 10 code-anchor file offsets
 * (anchor[ROM_GBUI_ANCH_COUNT]) were inserted between off[] and check so
 * revalidate_loc() can re-derive each field EXACTLY from the ROM's own code
 * bytes instead of trusting a structurally-plausible-but-corrupted off[]. */
_Static_assert(sizeof(RomGbUiLoc) == 116, "RomGbUiLoc size drifted");  /* #99: +1 off + +1 anchor (the Gen-1 key-item table) */
_Static_assert(offsetof(RomGbUiLoc, id_hash) == 0,  "RomGbUiLoc.id_hash");
_Static_assert(offsetof(RomGbUiLoc, size)    == 4,  "RomGbUiLoc.size");
_Static_assert(offsetof(RomGbUiLoc, gen)     == 8,  "RomGbUiLoc.gen");
_Static_assert(offsetof(RomGbUiLoc, pad)     == 9,  "RomGbUiLoc.pad");
_Static_assert(sizeof(((RomGbUiLoc*)0)->pad) == 3,  "RomGbUiLoc.pad size");
_Static_assert(offsetof(RomGbUiLoc, off)     == 12, "RomGbUiLoc.off");
_Static_assert(sizeof(((RomGbUiLoc*)0)->off) == 56, "RomGbUiLoc.off size (off[14])");
_Static_assert(offsetof(RomGbUiLoc, anchor)  == 68, "RomGbUiLoc.anchor");
_Static_assert(sizeof(((RomGbUiLoc*)0)->anchor) == 44,
               "RomGbUiLoc.anchor size (anchor[ROM_GBUI_ANCH_COUNT], COUNT==11)");
_Static_assert(offsetof(RomGbUiLoc, check)   == 112, "RomGbUiLoc.check");

int main(void) {
  _Static_assert(ROM_GBUI_ANCH_COUNT == 11, "ROM_GBUI_ANCH_COUNT drifted");
  printf("host_gbloc_layout_test: ALL OK "
         "(RomGbSpriteLoc=%zu RomGbIconLoc=%zu RomGbUiLoc=%zu)\n",
         sizeof(RomGbSpriteLoc), sizeof(RomGbIconLoc), sizeof(RomGbUiLoc));
  return 0;
}
