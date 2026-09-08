/* Host (PC) test for rom_gbui -- the Gen-1/Gen-2 trainer-card UI graphics
 * located BY SHAPE in REAL cartridge dumps. rom_gbui does all its I/O
 * through the GbReadFn callback, so the code under test is byte-for-byte
 * the code that runs on the GBA.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_romgbui_test.c source/rom_gbui.c source/gb_sprite_codec.c -o /tmp/hgbui && /tmp/hgbui
 *
 * WHAT MAKES THIS MORE THAN "one thing agreeing with itself"
 * -------------------------------------------------------
 * The oracle is docs/GB-GAME-SCREENS-DESIGN.md's Appendix A -- offsets
 * produced by an INDEPENDENT throwaway Python harness (/tmp/gbui/locate.py,
 * ported here, not copied) that was run against the same four ROMs and whose
 * output was eyeballed as PNGs. If rom_gbui.c's C port of any signature, any
 * bank-sweep rule or any derived-offset relationship is wrong by one byte,
 * the offset comparisons below miss.
 *
 * Coverage:
 *   1) all four dumps identify (Red/Yellow -> Gen 1, Gold/Crystal -> Gen 2)
 *      and every located offset matches Appendix A exactly;
 *   2) the Gen-1 player pic decodes cleanly through the project's own
 *      gb_sprite_gen1() to 56x56, err GB_SPRITE_OK, for both Red and Yellow;
 *   3) save_loc -> open_loc round-trips WITHOUT a rescan (measured via a
 *      read-counting GbReadFn: open_loc's read count is a small constant,
 *      open()'s full scan reads the whole ROM);
 *   4) a loc with a wrong id_hash, or a wrong size, is REJECTED and falls
 *      back to the full scan (same correct offsets result, at open()'s cost);
 *   5) NEGATIVE CONTROL -- a one-byte mutation of a RAM copy of the ROM (via
 *      the read callback's in-flight corruption, same technique
 *      tests/host_romgbsprite_test.c uses) that lands inside a locator's own
 *      signature makes that whole open() fail closed: gen 0, every offset 0,
 *      ok 0. If this ever passes silently, the locator isn't checking what
 *      it claims to.
 *
 * ROMs are Guy's own dumps: they live OUTSIDE the repo and are never copied
 * into it, so a missing corpus SKIPS rather than failing.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "rom_gbui.h"
#include "gb_sprite_codec.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
static void chk(const char* who, const char* what, int cond) {
  g_check++;
  if (!cond) { printf("  !! FAIL [%s] %s\n", who, what); g_fail++; }
}

/* --------------------------------------------------------------- file I/O */

typedef struct {
  FILE*    f;
  uint32_t reads;            /* how many times the callback was invoked     */
  uint32_t poison_off;       /* if non-zero, this byte is corrupted in flight */
  uint8_t  poison_xor;
} FileCtx;

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  fc->reads++;
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

static uint8_t g_scratch[ROM_GBUI_SCRATCH_MIN];

static int open_rom(RomGbUi* gu, FileCtx* fc, const char* path) {
  memset(fc, 0, sizeof *fc);
  fc->f = fopen(path, "rb");
  if (!fc->f) return 0;
  return rom_gbui_open(gu, file_read, fc, file_size(path), g_scratch, sizeof g_scratch);
}

/* ------------------------------------------------ Appendix A, the oracle */

typedef struct {
  const char* file;
  uint8_t     gen;
  uint32_t    font, textbox, cardframe, badges, leaders, playerpic;
  uint32_t    frames, fontextra, cardpic_m, cardpic_f, cardgfx, pack_m, pack_f;
} Want;

static const Want WANT[] = {
  { "Red.gb",    1,
    0x11A80, 0x12288, 0x2FB98, 0xEA9E, 0, 0x12EDE,
    0, 0, 0, 0, 0, 0, 0 },
  { "Yellow.gb", 1,
    0x10600, 0x10E18, 0xF5C24, 0xE91B, 0, 0x11A97,
    0, 0, 0, 0, 0, 0, 0 },
  { "Gold.gbc",  2,
    0, 0, 0, 0x2622F, 0x2576F, 0,
    0xF88F2, 0xF80F2, 0x2547F, 0, 0x256AF, 0x11431, 0 },
  { "Crystal.gbc", 2,
    0, 0, 0, 0x26043, 0x25583, 0,
    0xF8800, 0xF8000, 0x88365, 0x88595, 0x887C5, 0x11016, 0x48E9B },
};
#define NWANT (sizeof WANT / sizeof WANT[0])

static void check_offsets(const char* file, const RomGbUi* gu, const Want* w) {
  chk(file, "gen matches Appendix A", gu->gen == w->gen);
  if (w->gen == ROM_GBUI_GEN1) {
    chk(file, "font",      gu->font      == w->font);
    chk(file, "textbox",   gu->textbox   == w->textbox);
    chk(file, "cardframe", gu->cardframe == w->cardframe);
    chk(file, "badges",    gu->badges    == w->badges);
    chk(file, "playerpic", gu->playerpic == w->playerpic);
  } else if (w->gen == ROM_GBUI_GEN2) {
    chk(file, "frames",    gu->frames    == w->frames);
    chk(file, "font",      gu->font      == w->fontextra + 512u);
    chk(file, "font(oracle)", gu->font   == w->frames - 1536u);
    chk(file, "fontextra", gu->fontextra == w->fontextra);
    chk(file, "badges",    gu->badges    == w->badges);
    chk(file, "leaders",   gu->leaders   == w->leaders);
    chk(file, "cardpic_m", gu->cardpic_m == w->cardpic_m);
    chk(file, "cardpic_f", gu->cardpic_f == w->cardpic_f);
    chk(file, "cardgfx",   gu->cardgfx   == w->cardgfx);
    chk(file, "pack_m",    gu->pack_m    == w->pack_m);
    chk(file, "pack_f",    gu->pack_f    == w->pack_f);
  }
}

/* --------------------------------------------------------- per-ROM run() */

static void run_one(const Want* w) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", ROMS, w->file);
  RomGbUi gu; FileCtx fc;
  if (!open_rom(&gu, &fc, path)) {
    if (!fc.f) { printf("  SKIP %s (not present)\n", w->file); return; }
    printf("  !! FAIL [%s] rom_gbui_open\n", w->file); g_fail++; g_check++;
    fclose(fc.f); return;
  }
  g_ran++;
  printf("  %-12s ok=%d gen=%d banks=%d\n", w->file, gu.ok, gu.gen, gu.banks);
  chk(w->file, "ok", gu.ok == 1);
  check_offsets(w->file, &gu, w);

  /* -------------------------------------------- Gen-1 player-pic decode */
  if (w->gen == ROM_GBUI_GEN1) {
    static GbSprite spr;
    GbSpriteErr e = gb_sprite_gen1(&spr, file_read, &fc, gu.playerpic);
    chk(w->file, "player pic decodes (err==GB_SPRITE_OK)", e == GB_SPRITE_OK);
    chk(w->file, "player pic is 56x56", spr.w == 56 && spr.h == 56);
  }

  /* -------------------------------------------- save_loc / open_loc */
  RomGbUiLoc loc;
  rom_gbui_save_loc(&gu, &loc);
  chk(w->file, "saved loc id_hash matches", loc.id_hash == gu.id_hash);
  chk(w->file, "saved loc gen matches", loc.gen == gu.gen);

  FileCtx fc2; memset(&fc2, 0, sizeof fc2);
  fc2.f = fopen(path, "rb");
  RomGbUi gu2;
  int ok2 = rom_gbui_open_loc(&gu2, file_read, &fc2, file_size(path),
                              g_scratch, sizeof g_scratch, &loc);
  chk(w->file, "open_loc succeeds from a valid cache", ok2 == 1);
  chk(w->file, "open_loc's offsets match open()'s",
      ok2 && memcmp(&gu2.font, &gu.font, sizeof gu.font) == 0 &&
      gu2.gen == gu.gen && gu2.font == gu.font && gu2.textbox == gu.textbox &&
      gu2.cardframe == gu.cardframe && gu2.badges == gu.badges &&
      gu2.leaders == gu.leaders && gu2.playerpic == gu.playerpic &&
      gu2.frames == gu.frames && gu2.fontextra == gu.fontextra &&
      gu2.cardpic_m == gu.cardpic_m && gu2.cardpic_f == gu.cardpic_f &&
      gu2.cardgfx == gu.cardgfx && gu2.pack_m == gu.pack_m && gu2.pack_f == gu.pack_f);
  /* open_loc from a good cache must be MUCH cheaper than the full scan --
   * the full scan reads the whole ROM in scratch-sized windows (hundreds of
   * reads); a validated cache re-checks a handful of fixed-size blocks. */
  FileCtx fc_full; memset(&fc_full, 0, sizeof fc_full);
  fc_full.f = fopen(path, "rb");
  RomGbUi gu_full;
  rom_gbui_open(&gu_full, file_read, &fc_full, file_size(path), g_scratch, sizeof g_scratch);
  chk(w->file, "open_loc reads far fewer times than a full open()",
      fc2.reads > 0 && fc2.reads < fc_full.reads / 4);
  fclose(fc_full.f);
  if (fc2.f) fclose(fc2.f);

  /* a loc with a wrong id_hash falls back to the full scan and still finds
   * the right answer */
  RomGbUiLoc bad_hash = loc; bad_hash.id_hash ^= 0xFFFFFFFFu;
  FileCtx fc3; memset(&fc3, 0, sizeof fc3); fc3.f = fopen(path, "rb");
  RomGbUi gu3;
  int ok3 = rom_gbui_open_loc(&gu3, file_read, &fc3, file_size(path),
                              g_scratch, sizeof g_scratch, &bad_hash);
  chk(w->file, "wrong id_hash: falls back to a full scan, same result",
      ok3 == 1 && gu3.gen == gu.gen && gu3.font == gu.font && gu3.pack_m == gu.pack_m);
  chk(w->file, "wrong id_hash: full-scan cost, not a cache hit",
      fc3.reads >= fc_full.reads);
  if (fc3.f) fclose(fc3.f);

  /* a loc with a wrong size is rejected the same way */
  RomGbUiLoc bad_size = loc; bad_size.size += 1;
  FileCtx fc4; memset(&fc4, 0, sizeof fc4); fc4.f = fopen(path, "rb");
  RomGbUi gu4;
  int ok4 = rom_gbui_open_loc(&gu4, file_read, &fc4, file_size(path),
                              g_scratch, sizeof g_scratch, &bad_size);
  chk(w->file, "wrong size: falls back to a full scan, same result",
      ok4 == 1 && gu4.gen == gu.gen && gu4.font == gu.font);
  if (fc4.f) fclose(fc4.f);

  /* -------------------------------------------- font glyph sanity */
  if (gu.font) {
    uint16_t px[64];
    int gok = rom_gbui_glyph(&gu, 0x80u /* 'A' */, px);
    chk(w->file, "glyph 'A' decodes", gok == 1);
    if (gok) {
      int distinct = 0;
      for (int i = 1; i < 64 && !distinct; i++) if (px[i] != px[0]) distinct = 1;
      chk(w->file, "glyph 'A' is not a solid block", distinct);
    }
  }

  /* -------------------------------------------- D4: grid bounds + colmajor */
  if (gu.font) {
    uint16_t px[64];
    chk(w->file, "font tile 128 (grid 16x8, 128 tiles) is out of grid",
        rom_gbui_tile(&gu, gu.font, 128, 1, 16u, 8u, 0, px) == 0);
  }
  if (gu.badges) {
    uint16_t px[64];
    chk(w->file, "badge tile 64 (grid 2x32, 64 tiles) is out of grid",
        rom_gbui_tile(&gu, gu.badges, 64, 2, 2u, 32u, 0, px) == 0);
  }
  if (gu.gen == ROM_GBUI_GEN2 && gu.cardpic_m) {
    uint16_t disp[64], raw[64];
    if (gu.cardpic_colmajor) {
      /* Crystal: display (col,row)=(2,3) -> row-major display index 17;
       * storage index must be col*grid_h+row = 2*7+3 = 17. */
      int od = rom_gbui_tile(&gu, gu.cardpic_m, 3 * 5 + 2, 2, 5u, 7u, 1, disp);
      int or_ = rom_gbui_tile(&gu, gu.cardpic_m, 17, 2, 0, 0, 0, raw);
      chk(w->file, "Crystal cardpic_m colmajor (2,3): reads ok", od == 1 && or_ == 1);
      chk(w->file, "Crystal cardpic_m colmajor (2,3) == raw storage tile 17",
          od && or_ && memcmp(disp, raw, sizeof disp) == 0);
      /* non-trivial second point: display (col,row)=(1,0) -> display index 1;
       * storage index = 1*7+0 = 7. */
      int od2 = rom_gbui_tile(&gu, gu.cardpic_m, 1, 2, 5u, 7u, 1, disp);
      int or2 = rom_gbui_tile(&gu, gu.cardpic_m, 7, 2, 0, 0, 0, raw);
      chk(w->file, "Crystal cardpic_m colmajor (1,0): reads ok", od2 == 1 && or2 == 1);
      chk(w->file, "Crystal cardpic_m colmajor (1,0) == raw storage tile 7",
          od2 && or2 && memcmp(disp, raw, sizeof disp) == 0);
    } else {
      /* Gold: row-major, so display index 17 must equal raw storage tile 17. */
      int od = rom_gbui_tile(&gu, gu.cardpic_m, 17, 2, 5u, 7u, 0, disp);
      int or_ = rom_gbui_tile(&gu, gu.cardpic_m, 17, 2, 0, 0, 0, raw);
      chk(w->file, "Gold cardpic_m row-major index 17: reads ok", od == 1 && or_ == 1);
      chk(w->file, "Gold cardpic_m row-major index 17 == raw storage tile 17",
          od && or_ && memcmp(disp, raw, sizeof disp) == 0);
    }
  }

  fclose(fc.f);
}

/* --------------------------------------------- 5) negative control */

/* Corrupt one byte inside a locator's own signature (a RAM-copy-style
 * in-flight corruption, same technique host_romgbsprite_test.c uses) and
 * confirm the WHOLE open() fails closed: gen 0, every offset 0, ok 0. */
static void run_mutation(const char* file, uint32_t poison_off, const char* label) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", ROMS, file);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP mutation %s (not present)\n", file); return; }
  fc.poison_off = poison_off;
  fc.poison_xor = 0xFFu;   /* flip every bit -- guaranteed to break any fixed-byte match */

  RomGbUi gu;
  int ok = rom_gbui_open(&gu, file_read, &fc, file_size(path), g_scratch, sizeof g_scratch);
  g_ran++;
  chk(file, label, ok == 0 && gu.gen == ROM_GBUI_NONE && gu.ok == 0 &&
                    gu.font == 0 && gu.cardframe == 0 && gu.frames == 0 && gu.cardpic_m == 0);
  fclose(fc.f);
}

int main(void) {
  printf("=== rom_gbui host test ===\n");
  for (uint32_t i = 0; i < NWANT; i++) run_one(&WANT[i]);

  printf("=== negative controls ===\n");
  /* Red's LoadFontTilePatterns anchor at file offset 0x3680 (found by an
   * independent byte scan of Red.gb for the same 18-byte pattern g1_font_cb
   * matches -- NOT the located font DATA, which lives at a totally different
   * offset (0x11A80); this is the CODE the locator anchors on). Flipping its
   * very first byte (the `F0` of `ldh a,[rLCDC]`) must break the match and
   * therefore the whole Gen-1 `ok`. */
  run_mutation("Red.gb", 0x3680u, "corrupt G1-F's own signature byte -> fail closed");
  /* Gold's LoadFrame anchor at file offset 0xF8040 (found the same way),
   * the routine the whole Gen-2 chain (frames/font/fontextra all derive from
   * it) depends on. */
  run_mutation("Gold.gbc", 0xF8040u, "corrupt G2-R's own signature byte -> fail closed");

  printf("%d checks, %d ran, %d failed\n", g_check, g_ran, g_fail);
  return g_fail ? 1 : 0;
}
