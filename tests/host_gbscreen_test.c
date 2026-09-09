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

  if (g_fail) { printf("%d FAILED\n", g_fail); return 1; }
  printf("ALL PASSED (host_gbscreen_test)\n");
  return 0;
}
