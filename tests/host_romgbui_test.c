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
 *   6) BACKLOG #71 (MUT-D5b) -- a shifted-but-recomputed-check loc (off[]
 *      shifted, `check` recomputed to match, exactly what a corrupted-yet-
 *      internally-consistent cache record would contain) is REJECTED by
 *      anchor re-derivation, not just accepted because the shifted block
 *      still happens to look font/tile-shaped: off[FONT]+16, off[FONT]+4096,
 *      a Gen-2 badges shift, and a poisoned anchor BYTE in the ROM itself
 *      (not the cache) all force a rescan; a genuine cache hit still costs a
 *      small constant number of reads (reported before/after this fix).
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

/* Independent re-implementation of rom_gbui.c's static loc_check(), so this
 * test can produce a loc whose `check` is CORRECTLY recomputed after
 * corrupting off[] -- exactly what an attacker (or bit rot plus a naive
 * re-save) would produce, and exactly the shape id_hash/size/check alone
 * cannot catch (BACKLOG #71 MUT-D5b). Deliberately duplicated rather than
 * exposed from rom_gbui.c: this is testing the PUBLIC contract (a corrupted
 * loc must be rejected), not reusing the implementation's own internals. */
static uint32_t test_fnv1a(const uint8_t* p, uint32_t n, uint32_t h) {
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}
static uint32_t test_loc_check(const RomGbUiLoc* l) {
  uint32_t h = 0x811C9DC5u;
  for (uint32_t i = 0; i < ROM_GBUI_OFF_COUNT; i++) {
    uint8_t b[4] = { (uint8_t)l->off[i], (uint8_t)(l->off[i] >> 8),
                      (uint8_t)(l->off[i] >> 16), (uint8_t)(l->off[i] >> 24) };
    h = test_fnv1a(b, 4, h);
  }
  for (uint32_t i = 0; i < ROM_GBUI_ANCH_COUNT; i++) {
    uint8_t b[4] = { (uint8_t)l->anchor[i], (uint8_t)(l->anchor[i] >> 8),
                      (uint8_t)(l->anchor[i] >> 16), (uint8_t)(l->anchor[i] >> 24) };
    h = test_fnv1a(b, 4, h);
  }
  return h;
}

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
  uint32_t    g1_keyitems;   /* BACKLOG #99: 0 = not expected to locate       */
} Want;

static const Want WANT[] = {
  { "Red.gb",    1,
    0x11A80, 0x12288, 0x2FB98, 0xEA9E, 0, 0x12EDE,
    0, 0, 0, 0, 0, 0, 0,
    0xE799 },
  { "Yellow.gb", 1,
    0x10600, 0x10E18, 0xF5C24, 0xE91B, 0, 0x11A97,
    0, 0, 0, 0, 0, 0, 0,
    0xE6DD },
  { "Gold.gbc",  2,
    0, 0, 0, 0x2622F, 0x2576F, 0,
    0xF88F2, 0xF80F2, 0x2547F, 0, 0x256AF, 0x11431, 0,
    0 },
  { "Crystal.gbc", 2,
    0, 0, 0, 0x26043, 0x25583, 0,
    0xF8800, 0xF8000, 0x88365, 0x88595, 0x887C5, 0x11016, 0x48E9B,
    0 },
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
    chk(file, "g1_keyitems (BACKLOG #99, located by shape)",
        gu->g1_keyitems == w->g1_keyitems);
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

  /* -------------------------------------------- BACKLOG #99: g1_keyitems */
  if (w->gen == ROM_GBUI_GEN1 && w->g1_keyitems != 0) {
    /* Independent re-derivation: read the table's own 15 bytes straight off
     * disk (not through rom_gbui.c) and bit-test them by hand, exactly the
     * shape IsKeyItem_ itself uses -- (id-1)>>3 / (id-1)&7, LSB-first. If
     * this disagrees with rom_gbui_g1_key_item() anywhere in 1..120, either
     * the cached bits or the exposed function's own math is wrong. */
    uint8_t tbl[15];
    FILE* rf = fopen(path, "rb");
    fseek(rf, (long)w->g1_keyitems, SEEK_SET);
    size_t nrd = fread(tbl, 1, sizeof tbl, rf);
    fclose(rf);
    chk(w->file, "g1_keyitems: read the 15-byte table", nrd == sizeof tbl);
    int mismatches = 0;
    for (unsigned id = 1; id <= 120; id++) {
      unsigned i = id - 1;
      bool want_bit = ((tbl[i >> 3] >> (i & 7u)) & 1u) != 0;
      bool got = rom_gbui_g1_key_item(&gu, (uint8_t)id);
      if (want_bit != got) mismatches++;
    }
    chk(w->file, "g1_keyitems: rom_gbui_g1_key_item matches an independent byte-for-byte re-decode (ids 1..120)",
        mismatches == 0);
    /* Out-of-table ids never claim a false positive. */
    chk(w->file, "g1_keyitems: id 0 is never a key item", !rom_gbui_g1_key_item(&gu, 0));
    chk(w->file, "g1_keyitems: id 121 (beyond the code-bound table) is never a key item",
        !rom_gbui_g1_key_item(&gu, 121));
    chk(w->file, "g1_keyitems: id 255 is never a key item", !rom_gbui_g1_key_item(&gu, 255));
    /* Public facts (badges, TOWN MAP, BICYCLE, SAFARI BALL, POKeDEX set;
     * POTION, POKe BALL clear) via the SHIPPED function, same set
     * g1_keyitems_verify() itself checks structurally at locate time. */
    for (unsigned id = 21; id <= 28; id++)
      chk(w->file, "g1_keyitems: badge is a key item", rom_gbui_g1_key_item(&gu, (uint8_t)id));
    chk(w->file, "g1_keyitems: TOWN MAP (5) is a key item", rom_gbui_g1_key_item(&gu, 5));
    chk(w->file, "g1_keyitems: BICYCLE (6) is a key item", rom_gbui_g1_key_item(&gu, 6));
    chk(w->file, "g1_keyitems: SAFARI BALL (8) is a key item", rom_gbui_g1_key_item(&gu, 8));
    chk(w->file, "g1_keyitems: POKeDEX (9) is a key item", rom_gbui_g1_key_item(&gu, 9));
    chk(w->file, "g1_keyitems: POKe BALL (4) is not a key item", !rom_gbui_g1_key_item(&gu, 4));
    chk(w->file, "g1_keyitems: POTION (20) is not a key item", !rom_gbui_g1_key_item(&gu, 20));
    chk(w->file, "g1_keyitems: ESCAPE ROPE (29) is not a key item", !rom_gbui_g1_key_item(&gu, 29));
  } else if (w->gen == ROM_GBUI_GEN1) {
    chk(w->file, "g1_keyitems: NULL gu is never a key item (fail-closed)",
        !rom_gbui_g1_key_item(NULL, 21));
  }

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

  /* D9: a corrupted off[] survives id_hash/size AND its own field's
   * structural re-verification (font+16 still passes font_verify) -- only
   * the `check` FNV over off[] catches it. Must fall back to a full scan
   * and still land on the correct offsets. */
  RomGbUiLoc bad_off = loc; bad_off.off[ROM_GBUI_OFF_FONT] += 16;
  FileCtx fc5; memset(&fc5, 0, sizeof fc5); fc5.f = fopen(path, "rb");
  RomGbUi gu5;
  int ok5 = rom_gbui_open_loc(&gu5, file_read, &fc5, file_size(path),
                              g_scratch, sizeof g_scratch, &bad_off);
  chk(w->file, "corrupted off[] (stale check): falls back to a full scan, same result",
      ok5 == 1 && gu5.gen == gu.gen && gu5.font == gu.font);
  chk(w->file, "corrupted off[]: full-scan cost, not a cache hit",
      fc5.reads >= fc_full.reads);
  if (fc5.f) fclose(fc5.f);

  /* -------------------------------------------- BACKLOG #71 (MUT-D5b) */
  /* Sanity: our re-implementation of the check hash must agree with the
   * shipped one on an UNCORRUPTED loc, or every test below is meaningless. */
  chk(w->file, "test_loc_check agrees with the shipped loc_check on a clean loc",
      test_loc_check(&loc) == loc.check);

  /* (a)/(b): shift off[FONT] by +16 and by +4096, recomputing `check` each
   * time so id_hash/size/check alone cannot catch it -- only anchor
   * re-derivation's exact fileoff(bank,addr)==off comparison can. Must
   * rescan (full-scan read cost) and still land on the correct offsets. */
  {
    static const uint32_t shifts[2] = { 16u, 4096u };
    for (int si = 0; si < 2; si++) {
      RomGbUiLoc bad = loc;
      bad.off[ROM_GBUI_OFF_FONT] += shifts[si];
      bad.check = test_loc_check(&bad);
      FileCtx fcx; memset(&fcx, 0, sizeof fcx); fcx.f = fopen(path, "rb");
      RomGbUi gux;
      int okx = rom_gbui_open_loc(&gux, file_read, &fcx, file_size(path),
                                  g_scratch, sizeof g_scratch, &bad);
      char l1[96], l2[96];
      snprintf(l1, sizeof l1,
              "font off+%u (recomputed check): rescans, correct offsets", shifts[si]);
      snprintf(l2, sizeof l2,
              "font off+%u (recomputed check): full-scan cost, not a cache hit", shifts[si]);
      chk(w->file, l1, okx == 1 && gux.gen == gu.gen && gux.font == gu.font);
      chk(w->file, l2, fcx.reads >= fc_full.reads);
      if (fcx.f) fclose(fcx.f);
    }
  }

  /* -------------------------------------------- BACKLOG #99: mutations */
  if (w->gen == ROM_GBUI_GEN1 && w->g1_keyitems != 0) {
    /* (1) shift the cached table pointer by 16 (check recomputed to match,
     * same MUT-D5b shape as the font attack above): anchor_g1_keyitems()'s
     * own operand math still lands on the ORIGINAL offset, disagreeing with
     * the shifted off[] -> the whole Gen-1 record is rejected and a full
     * rescan lands back on the correct (unshifted) table. */
    RomGbUiLoc bad_ptr = loc;
    bad_ptr.off[ROM_GBUI_OFF_G1_KEYITEMS] += 16u;
    bad_ptr.check = test_loc_check(&bad_ptr);
    FileCtx fcx; memset(&fcx, 0, sizeof fcx); fcx.f = fopen(path, "rb");
    RomGbUi gux;
    int okx = rom_gbui_open_loc(&gux, file_read, &fcx, file_size(path),
                                g_scratch, sizeof g_scratch, &bad_ptr);
    chk(w->file, "g1_keyitems off+16 (recomputed check): rescans, correct table offset",
        okx == 1 && gux.gen == gu.gen && gux.g1_keyitems == gu.g1_keyitems);
    chk(w->file, "g1_keyitems off+16 (recomputed check): full-scan cost, not a cache hit",
        fcx.reads >= fc_full.reads);
    if (fcx.f) fclose(fcx.f);

    /* (2) forge one bit of the table ITSELF (not the cache): flip the badge-21
     * bit in the ROM (byte 2, bit 4 of the 15-byte table). g1_keyitems_verify()
     * must reject -- both on the cached-loc revalidation path (whole Gen-1
     * record rescans) and on the rescan's own fresh locate() (the table is
     * genuinely corrupted, so it comes back unlocated: g1_keyitems == 0, but
     * every REQUIRED Gen-1 field -- font/textbox/cardframe/badges/playerpic,
     * untouched by this poison -- still locates fine, so `ok` stays 1). */
    FileCtx fcy; memset(&fcy, 0, sizeof fcy); fcy.f = fopen(path, "rb");
    fcy.poison_off = w->g1_keyitems + 2u; fcy.poison_xor = 0x10u;  /* clears badge id 21's bit */
    RomGbUi guy;
    int oky = rom_gbui_open_loc(&guy, file_read, &fcy, file_size(path),
                                g_scratch, sizeof g_scratch, &loc);
    chk(w->file, "g1_keyitems: one forged bit rejects the cached table (rescans)",
        oky == 1 && guy.ok == 1 && guy.gen == ROM_GBUI_GEN1);
    chk(w->file, "g1_keyitems: forged-bit rescan comes back unlocated (fails closed, not a guess)",
        oky == 1 && guy.g1_keyitems == 0);
    chk(w->file, "g1_keyitems: forged-bit rescan still finds every REQUIRED Gen-1 field",
        oky == 1 && guy.font == gu.font && guy.textbox == gu.textbox &&
        guy.cardframe == gu.cardframe && guy.badges == gu.badges &&
        guy.playerpic == gu.playerpic);
    if (fcy.f) fclose(fcy.f);
  }

  /* (c): same attack against a Gen-2 badges offset. */
  if (w->gen == ROM_GBUI_GEN2) {
    RomGbUiLoc bad = loc;
    bad.off[ROM_GBUI_OFF_BADGES] += 16u;
    bad.check = test_loc_check(&bad);
    FileCtx fcx; memset(&fcx, 0, sizeof fcx); fcx.f = fopen(path, "rb");
    RomGbUi gux;
    int okx = rom_gbui_open_loc(&gux, file_read, &fcx, file_size(path),
                                g_scratch, sizeof g_scratch, &bad);
    chk(w->file, "Gen-2 badges off+16 (recomputed check): rescans, correct offsets",
        okx == 1 && gux.gen == gu.gen && gux.badges == gu.badges);
    chk(w->file, "Gen-2 badges off+16 (recomputed check): full-scan cost",
        fcx.reads >= fc_full.reads);
    if (fcx.f) fclose(fcx.f);
  }

  /* (d): the loc record itself is UNTOUCHED (still the correct, valid
   * check) -- instead one byte of the ANCHOR's own signature is flipped in
   * the ROM. revalidate_loc() must refuse (the anchor no longer matches its
   * signature), forcing a rescan; since the byte is really corrupted, the
   * rescan itself then fails closed too (this is genuine bit rot, not a
   * cache problem -- the correct behaviour is REJECT, not "recover"). */
  {
    uint32_t font_anchor = loc.anchor[ROM_GBUI_ANCH_FONT];
    uint32_t frames_anchor = loc.anchor[ROM_GBUI_ANCH_FRAMES];
    uint32_t poison = (w->gen == ROM_GBUI_GEN1) ? font_anchor : frames_anchor;
    chk(w->file, "have a non-zero anchor to poison", poison != 0);
    FileCtx fcx; memset(&fcx, 0, sizeof fcx); fcx.f = fopen(path, "rb");
    fcx.poison_off = poison; fcx.poison_xor = 0xFFu;
    RomGbUi gux;
    int okx = rom_gbui_open_loc(&gux, file_read, &fcx, file_size(path),
                                g_scratch, sizeof g_scratch, &loc);
    chk(w->file, "poisoned anchor byte in the ROM: cache rejected (rescans, then fails closed)",
        okx == 0 && gux.ok == 0 && gux.gen == ROM_GBUI_NONE);
    chk(w->file, "poisoned anchor byte: NOT a cheap cache hit (rescan cost, not ~20 reads)",
        fcx.reads > 30u);
    if (fcx.f) fclose(fcx.f);
  }

  /* (e): a genuine cache hit is still cheap. Report the exact number so a
   * future change to the anchor set can be compared against this fix's
   * own before/after (measured separately: Gen 1 21->26, Gen 2 Gold 13->17,
   * Gen 2 Crystal 13->18 reads -- one extra short read per stored anchor). */
  printf("  %-12s cache-hit reads=%u (full scan=%u)\n", w->file, fc2.reads, fc_full.reads);
  chk(w->file, "cache-hit read count stays a small constant (<=32)", fc2.reads <= 32u);

  /* -------------------------------------------- BACKLOG #82: reject other-gen slots */
  /* Gen-1 record with a corrupted Gen-2-only slot (frames=0xDEADBEEF): should
   * rescan and fall back to a full scan, since the loc is rejected. */
  if (w->gen == ROM_GBUI_GEN1) {
    RomGbUiLoc bad_gen2 = loc;
    bad_gen2.off[ROM_GBUI_OFF_FRAMES] = 0xDEADBEEFu;
    bad_gen2.check = test_loc_check(&bad_gen2);
    FileCtx fcx; memset(&fcx, 0, sizeof fcx); fcx.f = fopen(path, "rb");
    RomGbUi gux;
    int okx = rom_gbui_open_loc(&gux, file_read, &fcx, file_size(path),
                                g_scratch, sizeof g_scratch, &bad_gen2);
    chk(w->file, "Gen-1 with Gen-2-only slot (frames): cache rejected, rescans",
        okx == 1 && gux.gen == gu.gen && gux.font == gu.font);
    chk(w->file, "Gen-1 with Gen-2-only slot (frames): full-scan cost",
        fcx.reads >= fc_full.reads);
    if (fcx.f) fclose(fcx.f);
  }

  /* Gen-2 record with a corrupted Gen-1-only slot (cardframe=0xDEADBEEF): should
   * rescan and fall back to a full scan, since the loc is rejected. */
  if (w->gen == ROM_GBUI_GEN2) {
    RomGbUiLoc bad_gen1 = loc;
    bad_gen1.off[ROM_GBUI_OFF_CARDFRAME] = 0xDEADBEEFu;
    bad_gen1.check = test_loc_check(&bad_gen1);
    FileCtx fcx; memset(&fcx, 0, sizeof fcx); fcx.f = fopen(path, "rb");
    RomGbUi gux;
    int okx = rom_gbui_open_loc(&gux, file_read, &fcx, file_size(path),
                                g_scratch, sizeof g_scratch, &bad_gen1);
    chk(w->file, "Gen-2 with Gen-1-only slot (cardframe): cache rejected, rescans",
        okx == 1 && gux.gen == gu.gen && gux.font == gu.font);
    chk(w->file, "Gen-2 with Gen-1-only slot (cardframe): full-scan cost",
        fcx.reads >= fc_full.reads);
    if (fcx.f) fclose(fcx.f);
  }

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
