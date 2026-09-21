/* Host (PC) test for rom_gbsprite — Gen-1/Gen-2 sprites located and served out of
 * REAL cartridge dumps. rom_gbsprite does all its I/O through the GbReadFn
 * callback, so the code under test is byte-for-byte the code that runs on the GBA.
 *
 *   cc -std=c11 -I source tests/host_romgbsprite_test.c source/rom_gbsprite.c source/gb_sprite_codec.c -o /tmp/hgbs && /tmp/hgbs
 *
 * WHAT MAKES THIS MORE THAN "two things I wrote agreeing"
 * -------------------------------------------------------
 * The G/S backup-mirror bug proved that a fixture built from the parser's own
 * constants can never falsify those constants. So the sprite checks here are
 * anchored on data that came from NEITHER this module nor module A's codec: the
 * decomps' own front.png files. Their pixels were hashed OUTSIDE this program
 * (assets/upstream/pokered  gfx/pokemon/front/NAME.png, and
 *  assets/upstream/pokecrystal  gfx/pokemon/NAME/front.png) with the same FNV-1a
 * over one index byte per pixel, row-major, 0 = lightest; the constants are pasted
 * in below. If a table address, the Gen-1 bank ladder, the Gen-2 PicsBanks map,
 * the tile order or the codec is wrong by one bit, the hash misses.
 *
 * Coverage:
 *   1) identification accepts all four dumps and REFUSES a Gen-3 .gba, a truncated
 *      image, a .sav, a too-small scratch and a corrupted boot logo;
 *   2) Gen 1: all 151 fronts AND all 151 backs resolve and decompress, in Red and
 *      in Yellow, each front agreeing with its base-stats dimension byte;
 *   3) Gen 2: all 250 non-Unown fronts + backs resolve in Gold and Crystal, and
 *      all 26 Unown letters resolve in both;
 *   4) eight Red sprites, seven Crystal sprites and four Unown letters (in BOTH
 *      Gen-2 carts) are pixel-identical to the decomps' PNGs;
 *   5) palettes: Gen-2 shinies match the decomp's own shiny.pal numbers, Gen 1
 *      answers the four DMG greys;
 *   6) rom_gbsprite_to_rgb15 produces ui_sprite()'s format and refuses a short
 *      destination;
 *   7) the location cache round-trips and a tampered cache is rejected;
 *   8) NEGATIVE CONTROLS — the harness corrupts bytes in flight and the module
 *      must notice. If those ever pass silently, this test is dead.
 *
 * ROMs are Guy's own dumps: they live OUTSIDE the repo and are never copied into
 * it, so a missing corpus SKIPS.
 *
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_gbsprite.h"

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
  uint32_t poison_off;      /* if non-zero, this byte is corrupted in flight  */
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

/* Exactly what hashed the decomp PNGs: one byte per pixel, 0..3, row-major. */
static uint32_t hash_px(const GbSprite* s) {
  uint32_t h = 0x811C9DC5u, n = (uint32_t)s->w * s->h;
  for (uint32_t i = 0; i < n; i++) { h ^= s->px[i]; h *= 0x01000193u; }
  return h;
}

/* --------------------------------------------- expectations from the DECOMPS */

/* pokered gfx/pokemon/front/NAME.png — Red and Blue share these. */
typedef struct { uint16_t dex; uint8_t wt, ht; uint32_t hash; const char* name; } G1Want;
static const G1Want RED_WANT[] = {
  {   1, 5, 5, 0xBAFA222Fu, "bulbasaur" },
  {   6, 7, 7, 0xF9643085u, "charizard" },
  {  25, 5, 5, 0x537F31F9u, "pikachu"   },
  {  94, 6, 6, 0xD6474916u, "gengar"    },
  { 112, 7, 7, 0x6924C15Eu, "rhydon"    },
  { 131, 7, 7, 0xC82BA370u, "lapras"    },
  { 150, 7, 7, 0x0A973E2Fu, "mewtwo"    },
  { 151, 5, 5, 0xD129F038u, "mew"       },  /* the standalone Red/Blue record */
};

/* pokecrystal gfx/pokemon/NAME/front.png, static frame only. GOLD IS NOT EXPECTED
 * TO MATCH THESE: Crystal redrew every mon front for its animations (measured —
 * all seven differ between Gold and Crystal), which is why Gold's pixel anchor is
 * the Unown table below instead. */
typedef struct { uint16_t dex; uint8_t sz; uint32_t hash; const char* name; } G2Want;
static const G2Want CRYSTAL_WANT[] = {
  {   1, 5, 0x83535821u, "bulbasaur" },
  {  25, 5, 0x68233481u, "pikachu"   },
  { 152, 5, 0x36C2395Fu, "chikorita" },
  { 196, 6, 0xEDCDA511u, "espeon"    },
  { 245, 7, 0x9BDC04EDu, "suicune"   },
  { 250, 7, 0x6422DA56u, "ho_oh"     },
  { 251, 5, 0x193EE783u, "celebi"    },
};

/* The Unown letters ARE shared artwork between G/S and Crystal (measured: 20 of
 * the 26 are byte-identical, and these four are among them), so BOTH Gen-2 carts
 * must reproduce the pokecrystal PNG exactly. That is the check that proves
 * Gold's UnownPicPointers table and its non-contiguous PicsBanks map are right. */
typedef struct { uint8_t form; uint8_t sz; uint32_t hash; char letter; } UnownWant;
static const UnownWant UNOWN_WANT[] = {
  {  0, 5, 0x707D5867u, 'A' },
  {  1, 5, 0x156112C4u, 'B' },
  {  2, 5, 0xCAB144B5u, 'C' },
  { 25, 5, 0x9B338960u, 'Z' },
};

/* pokecrystal gfx/pokemon/NAME/shiny.pal, RGB r,g,b -> r | g<<5 | b<<10 */
typedef struct { uint16_t dex; uint16_t c1, c2; const char* name; } ShinyWant;
static const ShinyWant SHINY_WANT[] = {
  {   1, 0x2F94u, 0x195Fu, "bulbasaur RGB 20,28,11 / 31,10,06" },
  {  25, 0x023Fu, 0x2C54u, "pikachu   RGB 31,17,00 / 20,02,11" },
  { 245, 0x7F19u, 0x6270u, "suicune   RGB 25,24,31 / 16,19,24" },
};

/* ---------------------------------------------------------------- buffers */

/* The two caller-owned buffers this module's contract asks for. On hardware the
 * GbSprite comes from app_arena_acquire() and the RGB15 destination is
 * mon_decomp; here they are plain statics because the host has the room. */
static GbSprite g_spr;
static uint16_t g_rgb[ROM_GBSPRITE_MAX_PIXELS];
static uint8_t  g_scratch[4096];

static int open_rom(RomGbSprite* gs, FileCtx* fc, const char* path) {
  fc->f = fopen(path, "rb");
  if (!fc->f) return 0;
  return rom_gbsprite_open(gs, file_read, fc, file_size(path), g_scratch, sizeof g_scratch, GB_ROM_NONE);
}

/* ------------------------------------------------------------------ Gen 1 */

static void run_gen1(const char* file, const G1Want* want, uint32_t nwant) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", ROMS, file);
  RomGbSprite gs; FileCtx fc; memset(&fc, 0, sizeof fc);
  if (!open_rom(&gs, &fc, path)) {
    if (!fc.f) { printf("  SKIP %s (not present)\n", file); return; }
    printf("  !! FAIL [%s] rom_gbsprite_open\n", file); g_fail++; g_check++;
    fclose(fc.f); return;
  }
  g_ran++;
  printf("  %-12s '%s'  gen %d   BaseStats 0x%05X   MewBaseStats %s0x%05X (bank 0x%02X)\n",
         file, gs.title, (int)gs.gen, gs.base_stats,
         gs.mew_stats ? "" : "in-table ", gs.mew_stats, gs.mew_bank);
  chk(file, "identified as Gen 1", gs.gen == GB_ROM_GEN1);

  /* 2) every species, both sides */
  int okf = 0, okb = 0;
  uint32_t dims[8][8]; memset(dims, 0, sizeof dims);
  RomGbPic p;
  for (uint16_t dex = 1; dex <= 151; dex++) {
    if (rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, dex, 0, &g_spr, &p)) {
      okf++;
      if (p.wt < 8 && p.ht < 8) dims[p.wt][p.ht]++;
    }
    if (rom_gbsprite_pic(&gs, ROM_GBSPRITE_BACK, dex, 0, &g_spr, &p)) okb++;
  }
  chk(file, "all 151 fronts resolve and decompress", okf == 151);
  chk(file, "all 151 backs resolve and decompress",  okb == 151);
  printf("      fronts %d/151  backs %d/151   front sizes:", okf, okb);
  for (int w = 1; w < 8; w++) for (int h = 1; h < 8; h++)
    if (dims[w][h]) printf(" %dx%d:%u", w, h, dims[w][h]);
  printf("\n");

  /* 4) against the decomp's PNGs */
  for (uint32_t i = 0; i < nwant; i++) {
    char what[96];
    if (!rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, want[i].dex, 0, &g_spr, &p)) {
      snprintf(what, sizeof what, "%s (dex %u) fetches", want[i].name, want[i].dex);
      chk(file, what, 0);
      continue;
    }
    snprintf(what, sizeof what, "%s is pixel-identical to the pokered PNG", want[i].name);
    chk(file, what, p.wt == want[i].wt && p.ht == want[i].ht && hash_px(&g_spr) == want[i].hash);
  }

  /* 5) Gen-1 palette is the four DMG greys, whatever `shiny` says */
  uint16_t pal[4], pal2[4];
  chk(file, "palette reads", rom_gbsprite_pal(&gs, 1, 0, pal));
  chk(file, "palette reads (the shiny argument is ignored on Gen 1)",
      rom_gbsprite_pal(&gs, 1, 1, pal2));
  chk(file, "the four DMG greys, light to dark",
      pal[0] == 0x7FFFu && pal[3] == 0x0000u && pal[1] > pal[2] &&
      memcmp(pal, pal2, sizeof pal) == 0);
  chk(file, "dex 0 and dex 152 are refused",
      !rom_gbsprite_pal(&gs, 0, 0, pal) && !rom_gbsprite_pal(&gs, 152, 0, pal));

  /* 6) RGB15 expansion */
  if (rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 25, 0, &g_spr, &p)) {
    chk(file, "to_rgb15 refuses a destination one pixel short",
        !rom_gbsprite_to_rgb15(&g_spr, pal, g_rgb, p.pixels - 1));
    chk(file, "to_rgb15 accepts the exact size",
        rom_gbsprite_to_rgb15(&g_spr, pal, g_rgb, p.pixels));
    uint32_t opaque = 0, bad = 0;
    for (uint32_t i = 0; i < p.pixels; i++)
      if (g_rgb[i]) { opaque++; if (!(g_rgb[i] & 0x8000u)) bad++; }
    chk(file, "every pixel is 0 or 0x8000|RGB15, and the sprite is not empty",
        bad == 0 && opaque > p.pixels / 8);
    chk(file, "the top-left pixel is background, i.e. transparent", g_rgb[0] == 0);
  }

  /* 6b) rom_gbsprite_pic_buf() + rom_gbsprite_to_rgb15_inplace() (gb_art_source.c's
   * mon_decomp-sharing path, E3 review fix) must agree PIXEL FOR PIXEL with the
   * GbSprite + two-buffer path above, for every front in the corpus -- including
   * whichever dex is 7x7 (56x56 = 3,136 px, the exact worst case the in-place
   * function's header comment proves is safe: reading px[i] before writing dst[i]
   * at i == 3,135, where they are the SAME byte). A silent off-by-one here would
   * corrupt exactly the largest, most visually obvious sprites. */
  {
    static uint8_t buf[8192];      /* mon_decomp's own size */
    int checked = 0, mismatches = 0;
    for (uint16_t dex = 1; dex <= 151; dex++) {
      RomGbPic pr, pb;
      if (!rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, dex, 0, &g_spr, &pr)) continue;
      if (!rom_gbsprite_to_rgb15(&g_spr, pal, g_rgb, pr.pixels)) continue;
      memset(buf, 0xAA, sizeof buf);   /* poison: a stale byte must not look right */
      int ok = rom_gbsprite_pic_buf(&gs, ROM_GBSPRITE_FRONT, dex, 0,
                                    buf + ROM_GBSPRITE_MAX_PIXELS, buf + 6272, &pb) &&
              rom_gbsprite_to_rgb15_inplace(buf, ROM_GBSPRITE_MAX_PIXELS, pb.w, pb.h, pal);
      checked++;
      if (!ok || pr.w != pb.w || pr.h != pb.h ||
          memcmp(g_rgb, buf, (size_t)pr.pixels * 2u) != 0)
        mismatches++;
    }
    printf("      in-place vs GbSprite path: %d checked, %d mismatch(es)\n", checked, mismatches);
    chk(file, "rom_gbsprite_pic_buf + to_rgb15_inplace match the GbSprite path for every front",
        checked == 151 && mismatches == 0);
  }

  /* 8) NEGATIVE CONTROLS. Corrupt dex 1's BASE_PIC_SIZE byte in flight: the
   *    picture the pointer leads to now disagrees with the base stats, which is
   *    exactly the signal a WRONG SPRITE BANK produces. If this "passes", the
   *    dimension assert is doing nothing and the module is unguarded. */
  fc.poison_off = gs.base_stats + 10;      /* BASE_PIC_SIZE */
  fc.poison_xor = 0x11;
  chk(file, "NEGATIVE: a pic whose geometry disagrees with the base stats is REFUSED",
      !rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 1, 0, &g_spr, &p));
  fc.poison_off = gs.base_stats + 12;      /* BASE_FRONTPIC high byte */
  fc.poison_xor = 0x80;                    /* pushes it out of the ROMX window */
  chk(file, "NEGATIVE: a front pointer outside [0x4000,0x8000) is REFUSED",
      !rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 1, 0, &g_spr, &p));
  fc.poison_off = 0;
  chk(file, "…and the very same fetch succeeds once the corruption is lifted",
      rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 1, 0, &g_spr, &p) &&
      (nwant == 0 || hash_px(&g_spr) == want[0].hash));

  /* 7) location cache */
  uint32_t live = 0;
  if (rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 1, 0, &g_spr, &p)) live = hash_px(&g_spr);
  RomGbSpriteLoc loc;
  rom_gbsprite_save_loc(&gs, &loc);
  RomGbSprite gs2;
  chk(file, "the location cache round-trips",
      rom_gbsprite_open_loc(&gs2, file_read, &fc, gs.size, g_scratch, sizeof g_scratch, &loc, GB_ROM_NONE) &&
      gs2.base_stats == gs.base_stats && gs2.mew_stats == gs.mew_stats);
  chk(file, "a cache-opened ROM renders the identical Bulbasaur",
      live && rom_gbsprite_pic(&gs2, ROM_GBSPRITE_FRONT, 1, 0, &g_spr, &p) &&
      hash_px(&g_spr) == live);
  RomGbSpriteLoc bad_loc = loc;
  bad_loc.base_stats ^= 0x20;
  chk(file, "a tampered cache is rejected and the full scan re-runs",
      rom_gbsprite_open_loc(&gs2, file_read, &fc, gs.size, g_scratch, sizeof g_scratch, &bad_loc, GB_ROM_NONE) &&
      gs2.base_stats == gs.base_stats);

  fclose(fc.f);
}

/* ------------------------------------------------------------------ Gen 2 */

static void run_gen2(const char* file, const G2Want* want, uint32_t nwant) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", ROMS, file);
  RomGbSprite gs; FileCtx fc; memset(&fc, 0, sizeof fc);
  if (!open_rom(&gs, &fc, path)) {
    if (!fc.f) { printf("  SKIP %s (not present)\n", file); return; }
    printf("  !! FAIL [%s] rom_gbsprite_open\n", file); g_fail++; g_check++;
    fclose(fc.f); return;
  }
  g_ran++;
  printf("  %-12s '%s'  gen %d   BaseData 0x%05X  PicPointers 0x%06X  "
         "Palettes 0x%05X  stored_lo 0x%02X\n",
         file, gs.title, (int)gs.gen, gs.base_data, gs.pic_ptrs, gs.palettes, gs.stored_lo);
  chk(file, "identified as Gen 2", gs.gen == GB_ROM_GEN2);

  /* 3) every species except Unown, both sides */
  int okf = 0, okb = 0, back_shape = 1;
  uint32_t sizes[8]; memset(sizes, 0, sizeof sizes);
  RomGbPic p;
  for (uint16_t dex = 1; dex <= 251; dex++) {
    if (dex == ROM_GBSPRITE_UNOWN_DEX) continue;
    if (rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, dex, 0, &g_spr, &p)) {
      okf++; if (p.wt < 8) sizes[p.wt]++;
    }
    if (rom_gbsprite_pic(&gs, ROM_GBSPRITE_BACK, dex, 0, &g_spr, &p)) {
      okb++;
      if (p.wt != 6 || p.ht != 6) back_shape = 0;
    }
  }
  chk(file, "all 250 non-Unown fronts resolve and decompress", okf == 250);
  chk(file, "all 250 non-Unown backs resolve and decompress",  okb == 250);
  chk(file, "every Gen-2 back pic is 6x6", back_shape);

  int oku = 0;
  for (uint8_t f = 0; f < ROM_GBSPRITE_UNOWN_FORMS; f++)
    if (rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, ROM_GBSPRITE_UNOWN_DEX, f, &g_spr, &p)) oku++;
  chk(file, "all 26 Unown letters resolve", oku == 26);

  printf("      fronts %d/250  backs %d/250  Unown %d/26   front sizes:", okf, okb, oku);
  for (int w = 1; w < 8; w++) if (sizes[w]) printf(" %dx%d:%u", w, w, sizes[w]);
  printf("\n      UnownPicPointers 0x%06X   PicsBanks:", gs.unown_ptrs);
  for (uint32_t i = 0; i < 24 && gs.bank_map[i]; i++)
    printf(" %02X>%02X", gs.stored_lo + i, gs.bank_map[i]);
  printf("\n");

  chk(file, "a form on a non-Unown species is refused",
      !rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 1, 3, &g_spr, &p));
  chk(file, "Unown letter 26 (there are only 26, A..Z) is refused",
      !rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, ROM_GBSPRITE_UNOWN_DEX, 26, &g_spr, &p));
  chk(file, "dex 0 and dex 252 are refused",
      !rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 0, 0, &g_spr, &p) &&
      !rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 252, 0, &g_spr, &p));

  /* 4) against the decomp's PNGs */
  for (uint32_t i = 0; i < nwant; i++) {
    char what[96];
    if (!rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, want[i].dex, 0, &g_spr, &p)) {
      snprintf(what, sizeof what, "%s (dex %u) fetches", want[i].name, want[i].dex);
      chk(file, what, 0);
      continue;
    }
    snprintf(what, sizeof what, "%s is pixel-identical to the pokecrystal PNG", want[i].name);
    chk(file, what, p.wt == want[i].sz && p.ht == want[i].sz && hash_px(&g_spr) == want[i].hash);
  }
  for (uint32_t i = 0; i < sizeof UNOWN_WANT / sizeof UNOWN_WANT[0]; i++) {
    char what[96];
    if (!rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, ROM_GBSPRITE_UNOWN_DEX,
                          UNOWN_WANT[i].form, &g_spr, &p)) {
      snprintf(what, sizeof what, "Unown %c fetches", UNOWN_WANT[i].letter);
      chk(file, what, 0);
      continue;
    }
    snprintf(what, sizeof what, "Unown %c is pixel-identical to the pokecrystal PNG",
             UNOWN_WANT[i].letter);
    chk(file, what, p.wt == UNOWN_WANT[i].sz && hash_px(&g_spr) == UNOWN_WANT[i].hash);
  }

  /* 5) palettes: the decomp's own shiny.pal numbers */
  uint16_t pal[4], npal[4];
  for (uint32_t i = 0; i < sizeof SHINY_WANT / sizeof SHINY_WANT[0]; i++) {
    char what[128];
    snprintf(what, sizeof what, "shiny palette matches shiny.pal: %s", SHINY_WANT[i].name);
    chk(file, what,
        rom_gbsprite_pal(&gs, SHINY_WANT[i].dex, 1, pal) &&
        pal[0] == 0x7FFFu && pal[1] == SHINY_WANT[i].c1 &&
        pal[2] == SHINY_WANT[i].c2 && pal[3] == 0x0000u);
  }
  chk(file, "normal and shiny differ for Bulbasaur",
      rom_gbsprite_pal(&gs, 1, 0, npal) && rom_gbsprite_pal(&gs, 1, 1, pal) &&
      memcmp(npal, pal, sizeof pal) != 0);
  int pal_ok = 1;
  for (uint16_t dex = 1; dex <= 251; dex++) {
    if (!rom_gbsprite_pal(&gs, dex, 0, npal) || !rom_gbsprite_pal(&gs, dex, 1, pal)) { pal_ok = 0; break; }
    if (npal[0] != 0x7FFFu || npal[3] != 0 || ((npal[1] | npal[2]) & 0x8000u)) { pal_ok = 0; break; }
  }
  chk(file, "all 251 normal + shiny palettes read and are well formed", pal_ok);

  /* 6) RGB15 expansion */
  if (rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 1, 0, &g_spr, &p) &&
      rom_gbsprite_pal(&gs, 1, 0, pal)) {
    chk(file, "to_rgb15 refuses a destination one pixel short",
        !rom_gbsprite_to_rgb15(&g_spr, pal, g_rgb, p.pixels - 1));
    chk(file, "to_rgb15 accepts the exact size",
        rom_gbsprite_to_rgb15(&g_spr, pal, g_rgb, p.pixels));
    uint32_t coloured = 0;
    for (uint32_t i = 0; i < p.pixels; i++)
      if (g_rgb[i] && (g_rgb[i] & 0x7FFFu) != 0x7FFFu) coloured++;
    chk(file, "the expansion actually uses the cartridge's colours", coloured > 0);
  }

  /* 6b) same in-place cross-check as the Gen-1 test, over every non-Unown front AND
   * all 26 Unown letters -- Gen 2's front sizes reach 7x7 too (see the "front
   * sizes" line above), so this corpus also exercises the i == 3,135 coincidence. */
  {
    static uint8_t buf[8192];
    int checked = 0, mismatches = 0;
    for (uint16_t dex = 1; dex <= 251; dex++) {
      if (dex == ROM_GBSPRITE_UNOWN_DEX) continue;
      RomGbPic pr, pb;
      if (!rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, dex, 0, &g_spr, &pr)) continue;
      if (!rom_gbsprite_pal(&gs, dex, 0, pal)) continue;
      if (!rom_gbsprite_to_rgb15(&g_spr, pal, g_rgb, pr.pixels)) continue;
      memset(buf, 0xAA, sizeof buf);
      int ok = rom_gbsprite_pic_buf(&gs, ROM_GBSPRITE_FRONT, dex, 0,
                                    buf + ROM_GBSPRITE_MAX_PIXELS, buf + 6272, &pb) &&
              rom_gbsprite_to_rgb15_inplace(buf, ROM_GBSPRITE_MAX_PIXELS, pb.w, pb.h, pal);
      checked++;
      if (!ok || pr.w != pb.w || pr.h != pb.h ||
          memcmp(g_rgb, buf, (size_t)pr.pixels * 2u) != 0)
        mismatches++;
    }
    for (uint8_t f = 0; f < ROM_GBSPRITE_UNOWN_FORMS; f++) {
      RomGbPic pr, pb;
      if (!rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, ROM_GBSPRITE_UNOWN_DEX, f, &g_spr, &pr)) continue;
      if (!rom_gbsprite_pal(&gs, ROM_GBSPRITE_UNOWN_DEX, 0, pal)) continue;
      if (!rom_gbsprite_to_rgb15(&g_spr, pal, g_rgb, pr.pixels)) continue;
      memset(buf, 0xAA, sizeof buf);
      int ok = rom_gbsprite_pic_buf(&gs, ROM_GBSPRITE_FRONT, ROM_GBSPRITE_UNOWN_DEX, f,
                                    buf + ROM_GBSPRITE_MAX_PIXELS, buf + 6272, &pb) &&
              rom_gbsprite_to_rgb15_inplace(buf, ROM_GBSPRITE_MAX_PIXELS, pb.w, pb.h, pal);
      checked++;
      if (!ok || pr.w != pb.w || pr.h != pb.h ||
          memcmp(g_rgb, buf, (size_t)pr.pixels * 2u) != 0)
        mismatches++;
    }
    printf("      in-place vs GbSprite path: %d checked, %d mismatch(es)\n", checked, mismatches);
    chk(file, "rom_gbsprite_pic_buf + to_rgb15_inplace match the GbSprite path (fronts + Unown)",
        checked == 276 && mismatches == 0);
  }

  /* 8) NEGATIVE CONTROLS */
  RomGbSprite gs3;
  fc.poison_off = gs.pic_ptrs + 200 * 6;   /* the Unown FF FF FF FF FF FF hole */
  fc.poison_xor = 0x01;
  chk(file, "NEGATIVE: open REFUSES a ROM whose Unown hole is not all-FF",
      !rom_gbsprite_open(&gs3, file_read, &fc, gs.size, g_scratch, sizeof g_scratch, GB_ROM_NONE));
  fc.poison_off = 0x104;                   /* the boot logo */
  chk(file, "NEGATIVE: open REFUSES a ROM whose boot logo is wrong",
      !rom_gbsprite_open(&gs3, file_read, &fc, gs.size, g_scratch, sizeof g_scratch, GB_ROM_NONE));
  fc.poison_off = gs.palettes + 9;         /* the high byte of a palette colour */
  fc.poison_xor = 0x80;                    /* sets bit 15, impossible in RGB15 */
  chk(file, "NEGATIVE: open REFUSES a palette table with bit 15 set",
      !rom_gbsprite_open(&gs3, file_read, &fc, gs.size, g_scratch, sizeof g_scratch, GB_ROM_NONE));
  fc.poison_off = 0;

  /* 7) location cache */
  uint32_t live = 0;
  if (rom_gbsprite_pic(&gs, ROM_GBSPRITE_FRONT, 1, 0, &g_spr, &p)) live = hash_px(&g_spr);
  RomGbSpriteLoc loc;
  rom_gbsprite_save_loc(&gs, &loc);
  chk(file, "the location cache round-trips",
      rom_gbsprite_open_loc(&gs3, file_read, &fc, gs.size, g_scratch, sizeof g_scratch, &loc, GB_ROM_NONE) &&
      gs3.pic_ptrs == gs.pic_ptrs && gs3.palettes == gs.palettes &&
      gs3.base_data == gs.base_data);
  chk(file, "a cache-opened ROM renders the identical Bulbasaur",
      live && rom_gbsprite_pic(&gs3, ROM_GBSPRITE_FRONT, 1, 0, &g_spr, &p) &&
      hash_px(&g_spr) == live);
  chk(file, "a cache-opened ROM still finds the Unown table",
      rom_gbsprite_pic(&gs3, ROM_GBSPRITE_FRONT, ROM_GBSPRITE_UNOWN_DEX, 0, &g_spr, &p) &&
      hash_px(&g_spr) == UNOWN_WANT[0].hash);
  RomGbSpriteLoc bad = loc;
  bad.palettes ^= 0x08;
  chk(file, "a tampered cache is rejected and the full scan re-runs",
      rom_gbsprite_open_loc(&gs3, file_read, &fc, gs.size, g_scratch, sizeof g_scratch, &bad, GB_ROM_NONE) &&
      gs3.palettes == gs.palettes);

  fclose(fc.f);
}

/* ------------------------------------------------------- identification only */

static void refuse(const char* label, const char* path, uint32_t size_override) {
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", label, path); return; }
  uint32_t sz = size_override ? size_override : file_size(path);
  RomGbSprite gs;
  chk(label, "is REFUSED by rom_gbsprite_open",
      !rom_gbsprite_open(&gs, file_read, &fc, sz, g_scratch, sizeof g_scratch, GB_ROM_NONE));
  fclose(fc.f);
}

int main(void) {
  printf("host_romgbsprite_test — Gen-1/2 sprites located in real cartridge dumps\n"
         "  codec: module A, source/gb_sprite_codec.c\n");

  printf("\n-- identification --\n");
  refuse("a Gen-3 .gba (Emerald)", GBAS "/Emerald.gba", 0);
  refuse("a Gen-3 .gba (FireRed)", GBAS "/FireRed.gba", 0);
  refuse("a half-truncated Red.gb", ROMS "/Red.gb", 0x80000);
  refuse("a Gen-1 .sav (not a ROM)", ROMS "/Red.sav", 0);
  {
    FileCtx fc; memset(&fc, 0, sizeof fc);
    fc.f = fopen(ROMS "/Crystal.gbc", "rb");
    if (fc.f) {
      RomGbSprite gs; uint8_t tiny[512];
      chk("a 512-byte scratch", "is REFUSED (below ROM_GBSPRITE_SCRATCH_MIN)",
          !rom_gbsprite_open(&gs, file_read, &fc, file_size(ROMS "/Crystal.gbc"),
                             tiny, sizeof tiny, GB_ROM_NONE));
      fclose(fc.f);
    }
  }

  printf("\n-- Gen 1 --\n");
  run_gen1("Red.gb", RED_WANT, sizeof RED_WANT / sizeof RED_WANT[0]);
  /* No pokeyellow checkout exists on this machine, so Yellow gets the structural
   * checks only — and it needs them: Yellow redrew most sprites (its Gengar is
   * 7x7 where Red's is 6x6) and, unlike Red/Blue, keeps Mew as an ordinary table
   * row with no bank special case. */
  run_gen1("Yellow.gb", NULL, 0);

  printf("\n-- Gen 2 --\n");
  run_gen2("Gold.gbc", NULL, 0);
  run_gen2("Crystal.gbc", CRYSTAL_WANT, sizeof CRYSTAL_WANT / sizeof CRYSTAL_WANT[0]);

  printf("\n%d checks, %d ROMs exercised, %d failures\n", g_check, g_ran, g_fail);
  if (!g_ran) { printf("NOTHING RAN (no ROMs present)\n"); return 0; }
  printf(g_fail ? "FAIL\n" : "ALL PASS\n");
  return g_fail ? 1 : 0;
}
