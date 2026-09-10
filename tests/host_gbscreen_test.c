/* Host test for source/pdna_gbscreen.c's PURE half (U2a, the GB-screen shell) --
 * the charmap mapping gbscr_text()/gbscr_raw() use to decide FONT-vs-BLANK, and the
 * stretch-blit tables/formula blit_stretched() actually reads (docs/
 * GB-GAME-SCREENS-DESIGN.md sec 1.5). The impure half (gbscr_open/close/flush,
 * ROM I/O + VRAM blit) is compiled OUT here via -DPDNA_GBSCREEN_HOST_TEST --
 * see pdna_gbscreen.c's own top-of-file note.
 *
 *   cc -std=c11 -I source -DPDNA_GBSCREEN_HOST_TEST tests/host_gbscreen_test.c \
 *      source/pdna_gbscreen.c source/gb_edit.c source/gen1_save.c source/gen2_save.c \
 *      -o /tmp/hgbscr && /tmp/hgbscr
 *
 * Coverage:
 *   1) every printable ASCII a Gen-1/2 screen needs (A-Z, a-z, 0-9, the punctuation
 *      gb_char_encode() maps, the two-char PK/MN glyph, an apostrophe contraction)
 *      lands in ONE cell as GBSCR_SRC_FONT with the game's own charmap byte;
 *   2) a space becomes a GBSCR_SRC_BLANK cell, never a FONT tile (the design's own
 *      rule -- charmap 0x7F is a blank cell, not a font glyph to render);
 *   3) gbscr_y_dst0/gbscr_y_dst_count (D4 fix -- these are the tables
 *      blit_stretched() actually reads, not the old unused x_lut/y_lut pair):
 *      144 entries, counts in {1,2}, exactly 16 twos, and dst0[r]+count[r]
 *      tiles destination rows 0..159 with every row covered exactly once;
 *   4) the x formula blit_stretched() actually uses (dx0 = 3*(s>>1) + (s&1),
 *      dxn = (s&1) ? 2 : 1) tiles destination columns 0..239 exactly once over
 *      source s = 0..159;
 *   5) gbscr_raw() applies the identical space-is-blank rule to already-GB-encoded
 *      bytes, with no ASCII step.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gb_edit.h"
#include "pdna_gbscreen.h"

/* gb_edit.c pulls in data_tables.h for stat/exp/PP lookups used ONLY by its
 * gb_recalc_stats/gb_exp_for_level/gb_level_from_exp/gb_move_base_pp/gb_dv_effects_of/
 * gb_growth_rate helpers -- none of which this test exercises (it tests
 * gb_char_encode() and pdna_gbscreen's own pure half only). data_tables.c itself is
 * a GENERATED file (tools/gen_gbfields.py, needs upstream decomp symbol files this
 * repo does not vendor into every checkout) -- these link-only stubs stand in for
 * it, same technique other host tests that isolate one corner of a multi-concern
 * .c file already use. */
void pk_base_stats(uint16_t internal, uint8_t out[6]) { (void)internal; for (int i = 0; i < 6; i++) out[i] = 0; }
uint8_t pk_species_gender_ratio(uint16_t internal) { (void)internal; return 0; }
uint8_t pk_species_growth(uint16_t internal) { (void)internal; return 0; }
uint8_t pk_move_pp(uint16_t move_id) { (void)move_id; return 0; }
uint32_t pk_exp_for_level(uint8_t growth_rate, uint8_t level) { (void)growth_rate; (void)level; return 0; }
uint8_t pk_level_from_exp(uint8_t growth_rate, uint32_t exp) { (void)growth_rate; (void)exp; return 1; }

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
  } while (0)

static GbScreen mk(void) {
  GbScreen gs;
  memset(&gs, 0, sizeof gs);
  gs.ok = true;
  gs.gen = GB_GEN1;
  return gs;
}

static void check_char(const char* s, uint8_t want_byte) {
  GbScreen gs = mk();
  gbscr_text(&gs, 0, 0, s);
  CHECK(gs.src[0] == GBSCR_SRC_FONT, "'%s' -> src %d, want FONT", s, gs.src[0]);
  CHECK(gs.map[0] == want_byte, "'%s' -> byte 0x%02X, want 0x%02X", s, gs.map[0], want_byte);
}

int main(void) {
  /* 1) A-Z, a-z, 0-9 -- constants/charmap.asm's own ranges. */
  for (char c = 'A'; c <= 'Z'; c++) { char s[2] = {c, 0}; check_char(s, (uint8_t)(0x80 + (c - 'A'))); }
  for (char c = 'a'; c <= 'z'; c++) { char s[2] = {c, 0}; check_char(s, (uint8_t)(0xA0 + (c - 'a'))); }
  for (char c = '0'; c <= '9'; c++) { char s[2] = {c, 0}; check_char(s, (uint8_t)(0xF6 + (c - '0'))); }

  /* Punctuation this shell's screens actually write (design sec 1.1: NAME/, MONEY/,
   * TIME/, the money Yen sign, the time colon). */
  check_char("/", 0xF3);
  check_char(":", 0x9C);
  check_char(",", 0xF4);
  check_char(".", 0xE8);
  check_char("$", 0xF0);     /* the Poke Dollar, "$" spelling (gb_edit.c's own note) */
  check_char("'", 0xE0);
  check_char("-", 0xE3);
  check_char("!", 0xE7);
  check_char("?", 0xE6);

  /* Two-ASCII-char -> one glyph, one cell (the <PK>/<MN> tiles). */
  check_char("PK", 0xE1);
  check_char("MN", 0xE2);

  /* An apostrophe contraction, lowercase only ("'d" -> 0xBB in Gen 1). */
  check_char("'d", 0xBB);

  /* 2) space is BLANK, never a FONT tile with byte 0x7F. */
  {
    GbScreen gs = mk();
    gbscr_text(&gs, 0, 0, " ");
    CHECK(gs.src[0] == GBSCR_SRC_BLANK, "space -> src %d, want BLANK", gs.src[0]);
  }

  /* Multi-glyph string advances one CELL per glyph, including the 2-char ones. */
  {
    GbScreen gs = mk();
    gbscr_text(&gs, 0, 0, "A PK B");
    CHECK(gs.src[0] == GBSCR_SRC_FONT && gs.map[0] == 0x80, "cell0 'A'");
    CHECK(gs.src[1] == GBSCR_SRC_BLANK, "cell1 space");
    CHECK(gs.src[2] == GBSCR_SRC_FONT && gs.map[2] == 0xE1, "cell2 'PK' -> one cell");
    CHECK(gs.src[3] == GBSCR_SRC_BLANK, "cell3 space");
    CHECK(gs.src[4] == GBSCR_SRC_FONT && gs.map[4] == 0x81, "cell4 'B'");
  }

  /* 5) gbscr_raw: already-GB-encoded bytes, same space-is-blank rule, no ASCII step. */
  {
    GbScreen gs = mk();
    uint8_t bytes[3] = { 0x80, 0x7F, 0x81 };   /* GB-encoded "A", space, "B" */
    gbscr_raw(&gs, 0, 0, bytes, 3);
    CHECK(gs.src[0] == GBSCR_SRC_FONT && gs.map[0] == 0x80, "raw cell0");
    CHECK(gs.src[1] == GBSCR_SRC_BLANK, "raw cell1 (0x7F -> blank)");
    CHECK(gs.src[2] == GBSCR_SRC_FONT && gs.map[2] == 0x81, "raw cell2");
  }

  /* 5b) D2 (review): gbscr_raw blanks the 0x50 terminator AND every byte after
   * it, so a shorter new name fully erases a longer previous name's tail
   * instead of leaving stale glyphs on screen ("ASH" + 0x50 + stale "JACK"
   * used to read as "ASH<blank>JAC"). */
  {
    GbScreen gs = mk();
    /* "AB" (0x80,0x81) then terminator (0x50) then leftover "JACK" bytes from
     * a previous, longer name still sitting in the buffer. */
    uint8_t bytes[7] = { 0x80, 0x81, 0x50, 0x8A, 0x80, 0x82, 0x8A };
    gbscr_raw(&gs, 0, 0, bytes, 7);
    CHECK(gs.src[0] == GBSCR_SRC_FONT && gs.map[0] == 0x80, "term cell0 'A'");
    CHECK(gs.src[1] == GBSCR_SRC_FONT && gs.map[1] == 0x81, "term cell1 'B'");
    CHECK(gs.src[2] == GBSCR_SRC_BLANK, "term cell2 (0x50 itself -> blank)");
    CHECK(gs.src[3] == GBSCR_SRC_BLANK, "term cell3 (tail byte -> blank)");
    CHECK(gs.src[4] == GBSCR_SRC_BLANK, "term cell4 (tail byte -> blank)");
    CHECK(gs.src[5] == GBSCR_SRC_BLANK, "term cell5 (tail byte -> blank)");
    CHECK(gs.src[6] == GBSCR_SRC_BLANK, "term cell6 (tail byte -> blank)");
  }

  /* 5c) D2: no terminator at all (a full 11-byte field with no 0x50) leaves
   * every byte painted as its own font tile -- the blank-tail rule must not
   * fire on real content. */
  {
    GbScreen gs = mk();
    uint8_t bytes[3] = { 0x80, 0x81, 0x82 };   /* "ABC", no terminator */
    gbscr_raw(&gs, 0, 0, bytes, 3);
    CHECK(gs.src[0] == GBSCR_SRC_FONT && gs.map[0] == 0x80, "noterm cell0");
    CHECK(gs.src[1] == GBSCR_SRC_FONT && gs.map[1] == 0x81, "noterm cell1");
    CHECK(gs.src[2] == GBSCR_SRC_FONT && gs.map[2] == 0x82, "noterm cell2");
  }

  /* 3) gbscr_y_dst0/gbscr_y_dst_count (the tables blit_stretched() actually
   * reads): 144 entries, every count in {1,2}, exactly 16 twos, and the
   * (dst0[r], dst0[r]+count[r]) ranges tile destination rows 0..159 with
   * every row covered exactly once (a mutation to any entry here should be
   * caught: e.g. gbscr_y_dst0[7] = 99 breaks the tiling check below). */
  {
    int dup = 0;
    int covered[160] = {0};
    for (int r = 0; r < 144; r++) {
      int dst0 = gbscr_y_dst0[r], cnt = gbscr_y_dst_count[r];
      CHECK(cnt == 1 || cnt == 2, "y_dst_count[%d]=%d, want 1 or 2", r, cnt);
      if (cnt == 2) dup++;
      for (int k = 0; k < cnt; k++) {
        int d = dst0 + k;
        CHECK(d >= 0 && d < 160, "y_dst0[%d]+%d=%d out of destination range", r, k, d);
        if (d >= 0 && d < 160) covered[d]++;
      }
    }
    CHECK(dup == 16, "y_dst rows with count 2: %d, want 16", dup);
    for (int d = 0; d < 160; d++) {
      CHECK(covered[d] == 1, "destination row %d covered %d times, want exactly 1", d, covered[d]);
    }
  }

  /* 4) The x formula blit_stretched() actually uses: for source column
   * s = 0..159, dx0 = 3*(s>>1) + (s&1), dxn = (s&1) ? 2 : 1 -- tiles
   * destination columns 0..239 exactly once. */
  {
    int covered[240] = {0};
    for (int s = 0; s < 160; s++) {
      int g = s >> 1, p = s & 1;
      int dx0 = 3 * g + (p ? 1 : 0);
      int dxn = p ? 2 : 1;
      for (int k = 0; k < dxn; k++) {
        int d = dx0 + k;
        CHECK(d >= 0 && d < 240, "x formula s=%d +%d=%d out of destination range", s, k, d);
        if (d >= 0 && d < 240) covered[d]++;
      }
    }
    for (int d = 0; d < 240; d++) {
      CHECK(covered[d] == 1, "destination column %d covered %d times, want exactly 1", d, covered[d]);
    }
  }

  /* 6) U2b item 1: block byte-length table -- the exact numbers docs/
   * GB-GAME-SCREENS-DESIGN.md sec 2.1/2.2 give for each block/gen. */
  {
    CHECK(gbscr_block_bytes(GB_GEN1, GBSCR_SRC_FONT) == 1024, "G1 font bytes");
    CHECK(gbscr_block_bytes(GB_GEN2, GBSCR_SRC_FONT) == 1024, "G2 font bytes");
    CHECK(gbscr_block_bytes(GB_GEN1, GBSCR_SRC_TEXTBOX) == 512, "G1 textbox bytes");
    CHECK(gbscr_block_bytes(GB_GEN2, GBSCR_SRC_TEXTBOX) == 432, "G2 frames bytes");
    CHECK(gbscr_block_bytes(GB_GEN1, GBSCR_SRC_CARDFRAME) == 640, "G1 cardframe bytes");
    CHECK(gbscr_block_bytes(GB_GEN1, GBSCR_SRC_BADGES) == 1024, "G1 badges bytes");
    CHECK(gbscr_block_bytes(GB_GEN2, GBSCR_SRC_BADGES) == 704, "G2 badges bytes");
    CHECK(gbscr_block_bytes(GB_GEN1, GBSCR_SRC_PIC) == 0, "PIC has no rom_gbui block size");
    CHECK(gbscr_block_bytes(GB_GEN1, GBSCR_SRC_BLANK) == 0, "BLANK has no block size");
  }

  /* 7) U2b item 1: gbscr_block_off() picks the right RomGbUi field per gen --
   * Gen 1's TEXTBOX reads .textbox, Gen 2's reads .frames (there is no shared
   * "textbox" field -- see gbscr_tile_pixels()'s own comment in pdna_gbscreen.c). */
  {
    RomGbUi gu; memset(&gu, 0, sizeof gu);
    gu.font = 0x1000; gu.textbox = 0x2000; gu.frames = 0x3000;
    gu.cardframe = 0x4000; gu.badges = 0x5000;
    CHECK(gbscr_block_off(&gu, GB_GEN1, GBSCR_SRC_FONT) == 0x1000, "font off");
    CHECK(gbscr_block_off(&gu, GB_GEN1, GBSCR_SRC_TEXTBOX) == 0x2000, "G1 textbox off");
    CHECK(gbscr_block_off(&gu, GB_GEN2, GBSCR_SRC_TEXTBOX) == 0x3000, "G2 textbox off -> frames");
    CHECK(gbscr_block_off(&gu, GB_GEN1, GBSCR_SRC_CARDFRAME) == 0x4000, "cardframe off");
    CHECK(gbscr_block_off(&gu, GB_GEN1, GBSCR_SRC_BADGES) == 0x5000, "badges off");
    CHECK(gbscr_block_off(NULL, GB_GEN1, GBSCR_SRC_FONT) == 0, "NULL gu -> 0");
  }

  /* 8) U2b item 1: gbscr_mem_read() -- the memory-backed GbReadFn every repaint
   * goes through after gbscr_open(). Two synthetic blocks in one tail buffer,
   * at DIFFERENT (and non-contiguous) "ROM" offsets, so a bounds bug that reads
   * across block boundaries would be caught. */
  {
    uint8_t tail[64];
    for (int i = 0; i < 64; i++) tail[i] = (uint8_t)i;
    GbscrCache c;
    memset(&c, 0, sizeof c);
    c.tail = tail;
    c.blocks[0] = (GbscrBlock){ .rom_off = 0x1000, .ram_off = 0, .len = 16 };
    c.blocks[1] = (GbscrBlock){ .rom_off = 0x2000, .ram_off = 16, .len = 8 };
    c.nblocks = 2;

    uint8_t buf[16];
    CHECK(gbscr_mem_read(&c, 0x1000, buf, 16) && buf[0] == 0 && buf[15] == 15,
          "read block0 whole");
    CHECK(gbscr_mem_read(&c, 0x1004, buf, 4) && buf[0] == 4 && buf[3] == 7,
          "read block0 middle slice, correct ram offset");
    CHECK(gbscr_mem_read(&c, 0x2000, buf, 8) && buf[0] == 16 && buf[7] == 23,
          "read block1 whole, correct ram offset (16, not 0)");
    CHECK(!gbscr_mem_read(&c, 0x1000, buf, 17), "read past block0's own length refused");
    CHECK(!gbscr_mem_read(&c, 0x1010, buf, 1), "read one byte past block0's end refused");
    CHECK(!gbscr_mem_read(&c, 0x3000, buf, 1), "read outside every block refused");
    CHECK(!gbscr_mem_read(&c, 0x1000 - 1, buf, 2),
          "read straddling INTO block0 from before it refused (off < rom_off)");
    /* Overflow safety: a huge len must not wrap `b->len - rel` into a huge
     * unsigned value and pass the bounds check by accident. */
    CHECK(!gbscr_mem_read(&c, 0x1000, buf, 0xFFFFFFFFu), "huge len refused, no overflow");
    CHECK(!gbscr_mem_read(&c, 0x1010, buf, 0xFFFFFFFFu),
          "huge len at the block's exact END refused (rel+len WRAPS in a naive check)");

    GbscrCache empty; memset(&empty, 0, sizeof empty);
    CHECK(!gbscr_mem_read(&empty, 0x1000, buf, 1), "empty cache (no tail) refused");
  }

  /* 9) U2b/U2c review item 0c: gbscr_cache_plan()/gbscr_tail_need() -- the pure
   * layout planner a real gbscr_open() now calls before ever touching the ROM.
   * For every gen x every need_mask combination (0 = FONT only .. all three
   * bits set): sum(plan.blocks[].len) + ROM_GBUI_SCRATCH_MIN must equal
   * gbscr_tail_need() (the same arithmetic, derived two different ways), and
   * consecutive ram_offs must be EXACTLY cumulative (block i's ram_off ==
   * sum of every earlier block's len, block 0's ram_off == 0). */
  {
    RomGbUi gu; memset(&gu, 0, sizeof gu);
    gu.font = 0x1000; gu.textbox = 0x2000; gu.frames = 0x2500;
    gu.cardframe = 0x4000; gu.badges = 0x5000;

    const uint8_t gens[2] = { GB_GEN1, GB_GEN2 };
    for (int gi = 0; gi < 2; gi++) {
      uint8_t gen = gens[gi];
      for (uint16_t mask = 0; mask <= (GBSCR_NEED_TEXTBOX | GBSCR_NEED_CARDFRAME | GBSCR_NEED_BADGES); mask++) {
        GbscrCache plan;
        bool ok = gbscr_cache_plan(gen, mask, &gu, 65536u, &plan);
        CHECK(ok, "gen=%d mask=0x%x: cache_plan refused", gen, mask);
        if (!ok) continue;

        uint32_t sum = 0, cursor = 0;
        for (int i = 0; i < plan.nblocks; i++) {
          CHECK(plan.blocks[i].ram_off == cursor,
                "gen=%d mask=0x%x block %d: ram_off=%u, want cumulative %u",
                gen, mask, i, plan.blocks[i].ram_off, cursor);
          sum += plan.blocks[i].len;
          cursor += plan.blocks[i].len;
        }
        uint32_t need = gbscr_tail_need(gen, mask);
        CHECK(sum + ROM_GBUI_SCRATCH_MIN == need,
              "gen=%d mask=0x%x: sum(len)=%u + SCRATCH_MIN=%u != gbscr_tail_need()=%u",
              gen, mask, sum, ROM_GBUI_SCRATCH_MIN, need);
      }
    }

    /* Minor (U2c review): a "mutation check" used to live here that hand-built
     * its OWN `bad` GbscrCache and re-ran its OWN inline copy of the
     * cumulative-offset loop against it -- it never called gbscr_cache_plan()
     * at all, so it could only ever test itself, not the real function.
     * Deleted; the loop above (which DOES call gbscr_cache_plan() for every
     * gen/mask combination) is the real coverage. */
  }

  /* 9b (U3): the 5 Gen-2 card blocks added alongside BADGES above -- same
   * exhaustive plan-arithmetic check, but scoped to just the new bits (the
   * full cross product with TEXTBOX/CARDFRAME/BADGES would push some
   * combinations past GBSCR_MAX_BLOCKS' 7-block cap and legitimately refuse,
   * which the item-9 loop's unconditional CHECK(ok, ...) does not expect).
   * Also proves the EXACT combination pdna_gbtrainer_gen2_card() opens with
   * (TEXTBOX|FONTEXTRA|CARDGFX|LEADERS|BADGES|one of CARDPIC_M/F = 7 blocks
   * with FONT) fits GBSCR_MAX_BLOCKS exactly, on both generations. */
  {
    RomGbUi gu; memset(&gu, 0, sizeof gu);
    gu.font = 0x1000; gu.textbox = 0x2000; gu.frames = 0x2500;
    gu.cardframe = 0x4000; gu.badges = 0x5000;
    gu.fontextra = 0x6000; gu.leaders = 0x7000; gu.cardgfx = 0x8000;
    gu.cardpic_m = 0x9000; gu.cardpic_f = 0xA000;

    /* Every SUBSET of just the 5 new bits (32 combinations, max 5 optional +
     * FONT = 6 blocks (this sub-block alone never reaches the 7-block cap --
     * that combination is the SEPARATE check right below). A raw
     * integer range 0..OR-of-bits (item 9's own idiom above) does not work
     * here: these bit VALUES are higher than TEXTBOX/CARDFRAME/BADGES', so
     * the range 0..992 also walks every combination of those THREE older
     * bits too, some of which push nblocks past 6 -- a real refusal, not a
     * test bug, but not what this block means to exercise. */
    static const uint16_t kNewBits[5] = {
      GBSCR_NEED_FONTEXTRA, GBSCR_NEED_LEADERS, GBSCR_NEED_CARDGFX,
      GBSCR_NEED_CARDPIC_M, GBSCR_NEED_CARDPIC_F
    };
    const uint8_t gens[2] = { GB_GEN1, GB_GEN2 };
    for (int gi = 0; gi < 2; gi++) {
      uint8_t gen = gens[gi];
      for (int sub = 0; sub < 32; sub++) {
        uint16_t mask = 0;
        for (int b = 0; b < 5; b++) if (sub & (1 << b)) mask |= kNewBits[b];
        GbscrCache plan;
        bool ok = gbscr_cache_plan(gen, mask, &gu, 65536u, &plan);
        CHECK(ok, "gen=%d mask=0x%x (new bits): cache_plan refused", gen, mask);
        if (!ok) continue;
        uint32_t sum = 0, cursor = 0;
        for (int i = 0; i < plan.nblocks; i++) {
          CHECK(plan.blocks[i].ram_off == cursor,
                "gen=%d mask=0x%x block %d: ram_off=%u, want cumulative %u",
                gen, mask, i, plan.blocks[i].ram_off, cursor);
          sum += plan.blocks[i].len;
          cursor += plan.blocks[i].len;
        }
        uint32_t need = gbscr_tail_need(gen, mask);
        CHECK(sum + ROM_GBUI_SCRATCH_MIN == need,
              "gen=%d mask=0x%x (new bits): sum(len)=%u + SCRATCH_MIN=%u != gbscr_tail_need()=%u",
              gen, mask, sum, ROM_GBUI_SCRATCH_MIN, need);
      }

      /* the real Gen-2 card's own exact combination (7 blocks incl. FONT and
       * BADGES -- the page-2 badge-icon overlay). */
      uint16_t card_mask = GBSCR_NEED_TEXTBOX | GBSCR_NEED_FONTEXTRA | GBSCR_NEED_CARDGFX |
                           GBSCR_NEED_LEADERS | GBSCR_NEED_BADGES | GBSCR_NEED_CARDPIC_M;
      GbscrCache plan;
      bool ok = gbscr_cache_plan(gen, card_mask, &gu, 65536u, &plan);
      CHECK(ok, "gen=%d: the real Gen-2 card's own 7-block combo was refused", gen);
      CHECK(plan.nblocks == 7, "gen=%d: the real Gen-2 card's combo planned %d blocks, want 7",
            gen, plan.nblocks);
    }
  }

  /* Minor (U2c review): gbscr_pack_pic()/gbscr_unpack_pic_px() round trip --
   * both moved above this module's own tonc/FatFs boundary specifically so
   * this test can call them directly. A synthetic 4x3-tile (32x24 px) grid,
   * every pixel a distinct index derived from its own coordinates (mod 4),
   * must read back byte-for-byte through pack -> unpack. */
  {
    const int tiles_w = 4, tiles_h = 3, w = tiles_w * 8, h = tiles_h * 8;
    uint8_t px[32 * 24];
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        px[y * w + x] = (uint8_t)((x * 3 + y * 7) & 3);

    uint8_t packed[GBSCR_PIC_PACKED_BYTES];
    gbscr_pack_pic(px, w, h, tiles_w, tiles_h, packed);

    int mismatches = 0;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        if (gbscr_unpack_pic_px(packed, tiles_w, tiles_h, x, y) != px[y * w + x]) mismatches++;
    CHECK(mismatches == 0, "pack/unpack round trip: %d of %d pixels mismatched",
          mismatches, w * h);

    /* Out-of-range coordinates (past the tiles_w*8 x tiles_h*8 grid) must
     * return 0, not read past `packed`. */
    CHECK(gbscr_unpack_pic_px(packed, tiles_w, tiles_h, w, 0) == 0, "unpack past width -> 0");
    CHECK(gbscr_unpack_pic_px(packed, tiles_w, tiles_h, 0, h) == 0, "unpack past height -> 0");
    CHECK(gbscr_unpack_pic_px(packed, tiles_w, tiles_h, -1, 0) == 0, "unpack negative x -> 0");

    /* A tile beyond the declared tiles_w x tiles_h grid (but still inside the
     * full 7x7 GBSCR_PIC_TILES packed buffer) stays zero-filled by pack's own
     * unconditional memset(out, 0, ...) -- unpack must read that back as 0
     * too, so reading a wider tiles_w/tiles_h than what was actually packed
     * never surfaces stale data from a previous pack call. */
    uint8_t stack_px[8 * 8];
    memset(stack_px, 3, sizeof stack_px);   /* deliberately non-zero */
    uint8_t packed2[GBSCR_PIC_PACKED_BYTES];
    gbscr_pack_pic(stack_px, 8, 8, 1, 1, packed2);   /* only tile (0,0) is real */
    CHECK(gbscr_unpack_pic_px(packed2, 7, 7, 8, 0) == 0, "tile (1,0) beyond 1x1 -> 0");
  }

  if (g_fail) { printf("%d FAILED\n", g_fail); return 1; }
  printf("ALL PASSED (host_gbscreen_test)\n");
  return 0;
}
