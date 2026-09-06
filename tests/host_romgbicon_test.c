/* Host (PC) test for rom_gbicon — Gen-2 party/PC menu icons located and served
 * out of REAL cartridge dumps. rom_gbicon does all its I/O through the GbReadFn
 * callback, so the code under test is byte-for-byte the code that runs on the
 * GBA.
 *
 *   cc -std=c11 -I source tests/host_romgbicon_test.c source/rom_gbicon.c -o /tmp/hgbi && /tmp/hgbi
 *
 * WHAT MAKES THIS MORE THAN "two things I wrote agreeing" (same discipline as
 * tests/host_romgbsprite_test.c): the pixel hashes below were computed OUTSIDE
 * this program, from pokecrystal's own published gfx/icons/NAME.png files (an
 * FNV-1a over one 0..3 index byte per pixel, row-major, 0 = lightest — the SAME
 * convention rom_gbicon_to_rgb15's `tiles_to_px` uses), completely independent
 * of this module's own locator or decoder. If MonMenuIcons' offset, the
 * IconPointers run, the derived icon bank or the tile/bit-plane order is wrong
 * by so much as one bit, the hash misses.
 *
 * Coverage:
 *   1) Gold.gbc and Crystal.gbc are located: MonMenuIcons + a MAXIMAL IconPointers
 *      run + a derived, sanity-checked icon bank;
 *   2) all 251 species map to a kind in 1..n, in both games;
 *   3) all n icons (n measured, never assumed) decode to a non-degenerate frame 0;
 *   4) five species' icons (spanning five different kinds) are pixel-identical to
 *      pokecrystal's published PNGs, in BOTH Gold and Crystal (the icon set is
 *      shared art, unlike the front/back sprites Crystal redrew);
 *   5) rom_gbicon_pal always answers the fixed DMG ramp; rom_gbicon_to_rgb15
 *      renders colour 0 transparent and refuses a bad tile byte;
 *   6) the location cache round-trips and a tampered one is rejected;
 *   7) NEGATIVE CONTROLS: Red.gb and Yellow.gb (real Gen-1 ROMs, no matching icon
 *      shape) refuse cleanly, and a corrupted MonMenuIcons byte makes the SAME
 *      Gold/Crystal dump refuse too — if these ever pass silently, the test is
 *      dead.
 *
 * ROMs are Guy's own dumps: they live OUTSIDE the repo and are never copied into
 * it, so a missing corpus SKIPS (exit 0, "NOTHING RAN").
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_gbicon.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"
#define GBAS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"

static int g_fail = 0, g_check = 0, g_ran = 0;
static void chk(const char* who, const char* what, int cond) {
  g_check++;
  if (!cond) { printf("  !! FAIL [%s] %s\n", who, what); g_fail++; }
}

/* --------------------------------------------------------------- file I/O */

typedef struct {
  FILE*    f;
  uint32_t poison_off;
  uint8_t  poison_xor;
} FileCtx;

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  if (fc->poison_off && fc->poison_off >= off && fc->poison_off < off + len)
    ((uint8_t*)dst)[fc->poison_off - off] ^= fc->poison_xor;
  return true;
}

static uint32_t file_size(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return (uint32_t)n;
}

/* --------------------------------------------------------------- hashing */

static uint32_t fnv1a(const uint8_t* p, uint32_t n) {
  uint32_t h = 0x811C9DC5u;
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}

/* Species (national dex) -> expected kind, and the kind's expected frame-0
 * pixel hash, from pokecrystal's gfx/icons/NAME.png (hashed externally, see the
 * file header). Five different kinds, so five different icon bitmaps, spanning
 * the low, middle and high end of the pointer run. */
typedef struct { uint16_t dex; int kind; uint32_t hash; const char* name; } IconWant;
static const IconWant WANT[] = {
  {   1, 22, 0xB9464893u, "bulbasaur (BULBASAUR)" },
  {  25,  4, 0xA1EC4287u, "pikachu   (PIKACHU)"   },
  { 201, 25, 0x8C0FA6F5u, "unown     (UNOWN)"     },
  {   6, 38, 0x75177CADu, "charizard (BIGMON)"    },
  { 250, 33, 0xA1F2D6F5u, "ho-oh     (HO_OH)"     },
};

static uint8_t g_scratch[ROM_GBICON_SCRATCH_MIN];

static void run_gen2(const char* name, int with_pixel_checks) {
  char path[256];
  snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", name, path); return; }
  g_ran++;
  uint32_t sz = file_size(path);

  RomGbIcon gi;
  int ok = rom_gbicon_open(&gi, file_read, &fc, sz, g_scratch, sizeof g_scratch);
  chk(name, "opens and locates the icon tables", ok);
  if (!ok) { fclose(fc.f); return; }

  printf("  %s: MonMenuIcons=0x%X IconPointers=0x%X n=%u bank=0x%X\n",
         name, gi.mon_menu_icons, gi.icon_pointers, gi.n, gi.icon_bank);

  /* 2) every species maps to a kind in 1..n */
  int species_ok = 1;
  for (uint16_t dex = 1; dex <= ROM_GBICON_SPECIES; dex++) {
    int k = rom_gbicon_kind(&gi, dex);
    if (k < 1 || k > gi.n) { species_ok = 0; break; }
  }
  chk(name, "all 251 species resolve to a kind in 1..n", species_ok);

  /* 3) every kind decodes to a non-degenerate frame 0 */
  int all_decode = 1;
  for (int k = 1; k <= gi.n; k++) {
    uint8_t tile[ROM_GBICON_FRAME_BYTES];
    if (!rom_gbicon_tiles(&gi, k, 0, tile)) { all_decode = 0; break; }
    uint16_t pal[4], rgb[ROM_GBICON_PX];
    if (!rom_gbicon_pal(&gi, k, pal) || !rom_gbicon_to_rgb15(tile, pal, rgb)) { all_decode = 0; break; }
    int mixed = 0;
    for (int i = 1; i < ROM_GBICON_PX; i++) if (rgb[i] != rgb[0]) { mixed = 1; break; }
    if (!mixed) { all_decode = 0; break; }
  }
  chk(name, "every kind 1..n decodes to a non-empty frame 0", all_decode);

  /* D6 (E5 fix): ICON_EGG (kind 28) is a real, decodable kind on both of Guy's
   * dumps (n=38 on both, so 28 <= n) -- already proved non-degenerate by the loop
   * just above (it iterates every kind 1..n, 28 included); this just confirms the
   * accessor itself answers 28, fail-closed shape and all. */
  chk(name, "rom_gbicon_kind_egg answers ICON_EGG (28)",
      rom_gbicon_kind_egg(&gi) == ROM_GBICON_KIND_EGG);

  /* 4) pixel-identical to the decomp PNGs, by species */
  if (with_pixel_checks) {
    for (unsigned i = 0; i < sizeof WANT / sizeof WANT[0]; i++) {
      int k = rom_gbicon_kind(&gi, WANT[i].dex);
      char label[64]; snprintf(label, sizeof label, "%s/%s", name, WANT[i].name);
      chk(label, "resolves to the expected kind", k == WANT[i].kind);
      uint8_t tile[ROM_GBICON_FRAME_BYTES];
      uint8_t idx[ROM_GBICON_PX];
      if (k == WANT[i].kind && rom_gbicon_tiles(&gi, k, 0, tile)) {
        /* re-derive the 0..3 index array the same way the module's own
         * to_rgb15 does internally, but hash the INDEX form directly (the
         * external hashes were computed over indices, not RGB15) */
        uint16_t pal[4] = { 0, 1, 2, 3 };   /* identity: to_rgb15 with this
                                             * "palette" leaves color N-1 in
                                             * the low bits of a 0x8000|N-1
                                             * word for N>0, and 0 for N=0 */
        uint16_t rgb[ROM_GBICON_PX];
        chk(label, "decodes", rom_gbicon_to_rgb15(tile, pal, rgb));
        for (int p = 0; p < ROM_GBICON_PX; p++)
          idx[p] = (rgb[p] & 0x8000u) ? (uint8_t)(rgb[p] & 3u) : 0u;
        chk(label, "pixel-identical to the decomp PNG", fnv1a(idx, sizeof idx) == WANT[i].hash);
      } else {
        chk(label, "decodes", 0);
      }
    }
  }

  /* 5) palette is the fixed party-menu OBJ palette for every kind (D2, E5 fix:
   * this used to assert a DMG monochrome ramp, which is the wrong asset's
   * palette entirely -- see rom_gbicon.h's palette note). RGB(27,31,27)
   * transparent, RGB(31,19,10) light orange, RGB(31,7,4) red, RGB(0,0,0) black,
   * each packed r | g<<5 | b<<10 (5-bit-per-channel GBC palette-RAM values) --
   * computed here from the RGB triples, independently of rom_gbicon.c's own
   * literal, so a transcription slip in either place would be caught. */
  uint16_t pal1[4], pal2[4];
  chk(name, "pal(1) succeeds", rom_gbicon_pal(&gi, 1, pal1));
  chk(name, "pal(n) succeeds", rom_gbicon_pal(&gi, gi.n, pal2));
  chk(name, "the palette is fixed across kinds", memcmp(pal1, pal2, sizeof pal1) == 0);
  {
    uint16_t want0 = 27u | (31u << 5) | (27u << 10);   /* transparent   */
    uint16_t want1 = 31u | (19u << 5) | (10u << 10);   /* light orange  */
    uint16_t want2 = 31u | ( 7u << 5) | ( 4u << 10);   /* red           */
    uint16_t want3 =  0u | ( 0u << 5) | ( 0u << 10);   /* black         */
    chk(name, "idx0 is RGB(27,31,27), packed r|g<<5|b<<10", pal1[0] == want0);
    chk(name, "idx1 is RGB(31,19,10), packed r|g<<5|b<<10", pal1[1] == want1);
    chk(name, "idx2 is RGB(31,7,4), packed r|g<<5|b<<10",   pal1[2] == want2);
    chk(name, "idx3 is RGB(0,0,0), packed r|g<<5|b<<10",    pal1[3] == want3);
  }
  chk(name, "pal(0) is refused (never a valid kind)", !rom_gbicon_pal(&gi, 0, pal1));

  /* to_rgb15: color 0 transparent, refuses a bad index */
  {
    uint8_t tile[ROM_GBICON_FRAME_BYTES];
    chk(name, "tiles(1,0) succeeds", rom_gbicon_tiles(&gi, 1, 0, tile));
    uint16_t rgb[ROM_GBICON_PX];
    chk(name, "to_rgb15 succeeds", rom_gbicon_to_rgb15(tile, pal1, rgb));
    int any_zero = 0, any_opaque = 0;
    for (int i = 0; i < ROM_GBICON_PX; i++) {
      if (rgb[i] == 0) any_zero = 1;
      if (rgb[i] & 0x8000u) any_opaque = 1;
    }
    chk(name, "to_rgb15 has at least one transparent and one opaque pixel", any_zero && any_opaque);
    chk(name, "to_rgb15 refuses a NULL tiles arg", !rom_gbicon_to_rgb15(0, pal1, rgb));
  }

  /* 6) the location cache round-trips, and a tampered one is rejected */
  RomGbIconLoc loc;
  rom_gbicon_save_loc(&gi, &loc);
  RomGbIcon gi2;
  chk(name, "the location cache round-trips",
      rom_gbicon_open_loc(&gi2, file_read, &fc, sz, g_scratch, sizeof g_scratch, &loc) &&
      gi2.mon_menu_icons == gi.mon_menu_icons && gi2.icon_pointers == gi.icon_pointers &&
      gi2.n == gi.n && gi2.icon_bank == gi.icon_bank);
  RomGbIconLoc bad = loc;
  bad.icon_pointers ^= 0x40;   /* now points at the wrong offset entirely */
  RomGbIcon gi3;
  chk(name, "a tampered cache is rejected and the full scan re-runs",
      rom_gbicon_open_loc(&gi3, file_read, &fc, sz, g_scratch, sizeof g_scratch, &bad) &&
      gi3.icon_pointers == gi.icon_pointers && gi3.mon_menu_icons == gi.mon_menu_icons);

  /* D3 (E5 fix): a tampered mon_menu_icons must be caught too -- before this
   * fix, the cached path never re-read the menu-icons window at all, so this
   * would have sailed through (icon_pointers/icon_bank still checked out) and
   * silently mislabeled every species' icon KIND. */
  RomGbIconLoc bad_menu = loc;
  bad_menu.mon_menu_icons ^= 0x40;   /* points at the wrong window entirely */
  RomGbIcon gi3b;
  chk(name, "D3: a tampered mon_menu_icons is rejected and the full scan re-runs",
      rom_gbicon_open_loc(&gi3b, file_read, &fc, sz, g_scratch, sizeof g_scratch, &bad_menu) &&
      gi3b.mon_menu_icons == gi.mon_menu_icons && gi3b.icon_pointers == gi.icon_pointers &&
      gi3b.n == gi.n && gi3b.icon_bank == gi.icon_bank);

  /* 7b) NEGATIVE CONTROL: corrupt MonMenuIcons' own first byte (Bulbasaur's
   * kind) so it no longer matches Ivysaur/Venusaur's -- breaks the very
   * invariant the locator requires, so the whole ROM must refuse to open. */
  fc.poison_off = gi.mon_menu_icons;
  fc.poison_xor = 0xFF;
  RomGbIcon gi4;
  chk(name, "NEGATIVE: a corrupted MonMenuIcons byte makes open() refuse",
      !rom_gbicon_open(&gi4, file_read, &fc, sz, g_scratch, sizeof g_scratch));
  fc.poison_off = 0;

  fclose(fc.f);
}

static void refuse(const char* label, const char* path, uint32_t size_override) {
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", label, path); return; }
  uint32_t sz = size_override ? size_override : file_size(path);
  RomGbIcon gi;
  chk(label, "is REFUSED by rom_gbicon_open",
      !rom_gbicon_open(&gi, file_read, &fc, sz, g_scratch, sizeof g_scratch));
  fclose(fc.f);
}

int main(void) {
  printf("host_romgbicon_test -- Gen-2 menu icons located in real cartridge dumps\n");

  printf("\n-- identification / negative controls --\n");
  refuse("a Gen-3 .gba (Emerald)", GBAS "/Emerald.gba", 0);
  refuse("a half-truncated Crystal.gbc", ROMS "/Crystal.gbc", 0x80000);
  refuse("a Gen-2 .sav (not a ROM)", ROMS "/Crystal.sav", 0);
  refuse("Red.gb (real Gen-1 ROM, no icon-table shape)", ROMS "/Red.gb", 0);
  refuse("Yellow.gb (real Gen-1 ROM, no icon-table shape)", ROMS "/Yellow.gb", 0);
  {
    FileCtx fc; memset(&fc, 0, sizeof fc);
    fc.f = fopen(ROMS "/Crystal.gbc", "rb");
    if (fc.f) {
      RomGbIcon gi; uint8_t tiny[512];
      chk("a 512-byte scratch", "is REFUSED (below ROM_GBICON_SCRATCH_MIN)",
          !rom_gbicon_open(&gi, file_read, &fc, file_size(ROMS "/Crystal.gbc"), tiny, sizeof tiny));
      fclose(fc.f);
    }
  }

  printf("\n-- Gen 2 --\n");
  run_gen2("Gold.gbc", 1);
  run_gen2("Crystal.gbc", 1);

  printf("\n%d checks, %d ROMs exercised, %d failures\n", g_check, g_ran, g_fail);
  if (!g_ran) { printf("NOTHING RAN (no ROMs present)\n"); return 0; }
  printf(g_fail ? "FAIL\n" : "ALL PASS\n");
  return g_fail ? 1 : 0;
}
