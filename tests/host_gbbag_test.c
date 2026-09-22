/* source/gb_bag.c -- the pure-C bag / PC-item-store core -- under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbbag_test.c \
 *      source/gb_bag.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbb && /tmp/hgbb
 *
 * The corpus is Guy's own cartridge dumps (BACKLOG #49; docs/GEN12-PARITY-DESIGN.md).
 * They live OUTSIDE the repo (gitignored at gba-toolkit/roms/gb/) and are read only
 * here -- every mutation happens on an in-memory copy -- so a missing corpus SKIPS
 * rather than fails, but a corpus that IS present must match the design doc's own
 * VERIFIED numbers (Appendix A) exactly, not approximately.
 *
 * D3(d) mid-write failure test: SKIPPED (stop-licence). The prescription was to
 * shrink the session's `len` so the last pocket's write lands past the end of the
 * image. Traced against source/gb_session.c/gb_session.h: (1) for Gen 2 sessions
 * (Gold/Crystal), gbs_write_field's Gen-2 arm calls g2w_write_range(&s->g2w, ...),
 * which never consults s->len again -- g2w_begin() captured its OWN copy of the
 * length at gbs_open() time (gb_session.c:69), so mutating s->len post-open has
 * zero effect on Gen-2 writes. (2) For Gen 1 sessions (Red/Yellow),
 * gbs_write_field does pass s->len through to gen1_write_range_ex, but the only
 * check that consults it is `if (len < GEN1_SAVE_SIZE) return GEN1W_ERR_SIZE;`
 * (source/gen1_write.c:769) -- a blanket floor on the whole file, not a per-field
 * off+n bound (the actual range check is against fixed constants
 * GEN1_SUM_FIRST/GEN1_SUM_LAST, independent of len). Shrinking s->len below
 * GEN1_SAVE_SIZE therefore fails EVERY subsequent gbs_write_field call
 * identically, including the FIRST pocket (Items) gbb_write touches -- it cannot
 * selectively fail only the last pocket. Making only the last write fail would
 * require editing production code (e.g. temporarily corrupting g2w's internal
 * length copy, or the field table), which the brief said not to do. No other
 * caller-visible knob makes a single field write fail without editing production
 * code, so (d) is skipped rather than forced.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "gb_bag.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)
#define CHECKF(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint8_t g_img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_orig[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];
static uint8_t g_protect[sizeof g_img];   /* 1 = this byte is allowed to have changed */

static uint32_t load(const char* file) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", ROMS, file);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint32_t n = (uint32_t)fread(g_img, 1, sizeof g_img, f);
  fclose(f);
  memcpy(g_orig, g_img, n);
  return n;
}

static GbGame session_game(const GbSession* s) {
  if (s->gen == GB_GEN1) return GBF_G_RED;
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

/* Lowest valid id NOT already occupying a slot in `list`, or 0 if every id in the
 * game's whole item-id space is somehow already present (never true in practice --
 * the id space is 0x01..0xBE/0xFA, far larger than any pocket's cap). Used so the
 * round-trip/single-pocket tests can insert a genuinely NEW entry regardless of
 * what the real corpus save already happens to hold in that pocket. */
static uint8_t pick_unused_id(GbGame g, const GbBagList* list) {
  for (uint16_t id = 1; id <= gbb_max_item_id(g); id++) {
    bool used = false;
    for (int i = 0; i < list->count; i++)
      if (list->entries[i].id == (uint8_t)id) { used = true; break; }
    if (!used) return (uint8_t)id;
  }
  return 0;
}

/* Insert into a quantity-bearing pocket that may already be AT its cap (the real
 * corpus's PC item store is 50/50 on both Red.sav and Gold.sav) -- if there is
 * room, insert an unused id (a genuinely new entry); if the pocket is already
 * full, merge a fixed delta into whatever its first entry already holds (exactly
 * the merge behaviour gbb_insert documents). Returns the id inserted/merged and
 * the QTY IT MUST READ BACK AS via `*want_qty_out`, so the caller does not have to
 * guess whether the corpus forced a merge. */
static uint8_t qty_pocket_edit(GbGame g, GbBagList* list, int cap, uint8_t delta,
                               uint8_t* want_qty_out) {
  uint8_t id;
  uint32_t base_qty = 0;
  if (list->count < cap) {
    id = pick_unused_id(g, list);
  } else {
    id = list->entries[0].id;
    base_qty = list->entries[0].qty;
  }
  uint32_t sum = base_qty + delta;
  *want_qty_out = (uint8_t)(sum > GBB_QTY_CAP ? GBB_QTY_CAP : sum);
  return id;
}

/* ---------------------------------------------------------------- A: reads
 *
 * docs/GEN12-PARITY-DESIGN.md Appendix A is ABRIDGED for the bag/PC listings ("...").
 * Where it prints a full run (Gold's Key items and Balls pockets) this checks the
 * exact list; everywhere else it checks the count and the LISTED PREFIX (as far as
 * the doc actually shows), plus id/qty plausibility bounds for every entry read. */

static void plausible(const char* file, GbGame g, const GbBagList* list, int cap) {
  CHECKF(list->count <= cap, "%s: pocket count %u over its cap %d", file, list->count, cap);
  for (int i = 0; i < list->count; i++) {
    uint8_t id = list->entries[i].id;
    CHECKF(id >= 1 && id <= gbb_max_item_id(g), "%s: entry %d id 0x%02X out of range",
          file, i, id);
    CHECKF(list->entries[i].qty >= 1 && list->entries[i].qty <= 255,
          "%s: entry %d qty implausible", file, i);
  }
}

static void expect_red_or_yellow(const char* file, uint8_t expect_gen,
                                 int bag_count, int pc_count,
                                 const uint8_t* bag_ids, const uint8_t* bag_qty, int bag_n,
                                 const uint8_t* pc_ids, const uint8_t* pc_qty, int pc_n) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  printf("  -- %s (%u bytes)\n", file, (unsigned)len);

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(s.gen == expect_gen, "%s: expected gen %d, got %d", file, expect_gen, s.gen);
  GbGame g = session_game(&s);

  GbBag bag;
  CHECKF(gbb_read(&s, &bag), "%s: gbb_read", file);

  CHECKF(bag.pockets[GBB_POCKET_ITEMS].count == bag_count,
        "%s: bag count %u != %d", file, bag.pockets[GBB_POCKET_ITEMS].count, bag_count);
  CHECKF(bag.pockets[GBB_POCKET_PC].count == pc_count,
        "%s: PC count %u != %d", file, bag.pockets[GBB_POCKET_PC].count, pc_count);
  plausible(file, g, &bag.pockets[GBB_POCKET_ITEMS], GBB_CAP_ITEMS);
  plausible(file, g, &bag.pockets[GBB_POCKET_PC], GBB_CAP_PC);

  for (int i = 0; i < bag_n; i++) {
    CHECKF(bag.pockets[GBB_POCKET_ITEMS].entries[i].id == bag_ids[i],
          "%s: bag[%d] id 0x%02X != 0x%02X", file, i,
          bag.pockets[GBB_POCKET_ITEMS].entries[i].id, bag_ids[i]);
    CHECKF(bag.pockets[GBB_POCKET_ITEMS].entries[i].qty == bag_qty[i],
          "%s: bag[%d] qty %u != %u", file, i,
          bag.pockets[GBB_POCKET_ITEMS].entries[i].qty, bag_qty[i]);
  }
  for (int i = 0; i < pc_n; i++) {
    CHECKF(bag.pockets[GBB_POCKET_PC].entries[i].id == pc_ids[i],
          "%s: pc[%d] id 0x%02X != 0x%02X", file, i,
          bag.pockets[GBB_POCKET_PC].entries[i].id, pc_ids[i]);
    CHECKF(bag.pockets[GBB_POCKET_PC].entries[i].qty == pc_qty[i],
          "%s: pc[%d] qty %u != %u", file, i,
          bag.pockets[GBB_POCKET_PC].entries[i].qty, pc_qty[i]);
  }

  /* Gen 1: no Key items / Balls / TM-HM pockets. */
  CHECKF(!gbb_field_present(g, GBB_POCKET_KEY), "%s: Gen 1 must not have Key items", file);
  CHECKF(!gbb_field_present(g, GBB_POCKET_BALLS), "%s: Gen 1 must not have Balls", file);
  CHECKF(!gbb_field_present(g, GBB_POCKET_TMHM), "%s: Gen 1 must not have TM/HM", file);
}

static void expect_gen2(const char* file, uint8_t expect_gen,
                        int items_count, int key_count, int balls_count, int pc_count,
                        const uint8_t* key_ids, int key_n,
                        const uint8_t* ball_ids, const uint8_t* ball_qty, int ball_n) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  printf("  -- %s (%u bytes)\n", file, (unsigned)len);

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(s.gen == expect_gen, "%s: expected gen %d, got %d", file, expect_gen, s.gen);
  GbGame g = session_game(&s);

  GbBag bag;
  CHECKF(gbb_read(&s, &bag), "%s: gbb_read", file);

  CHECKF(bag.pockets[GBB_POCKET_ITEMS].count == items_count,
        "%s: items count %u != %d", file, bag.pockets[GBB_POCKET_ITEMS].count, items_count);
  CHECKF(bag.pockets[GBB_POCKET_KEY].count == key_count,
        "%s: key count %u != %d", file, bag.pockets[GBB_POCKET_KEY].count, key_count);
  CHECKF(bag.pockets[GBB_POCKET_BALLS].count == balls_count,
        "%s: balls count %u != %d", file, bag.pockets[GBB_POCKET_BALLS].count, balls_count);
  CHECKF(bag.pockets[GBB_POCKET_PC].count == pc_count,
        "%s: PC count %u != %d", file, bag.pockets[GBB_POCKET_PC].count, pc_count);

  plausible(file, g, &bag.pockets[GBB_POCKET_ITEMS], GBB_CAP_ITEMS);
  plausible(file, g, &bag.pockets[GBB_POCKET_BALLS], GBB_CAP_BALLS);
  plausible(file, g, &bag.pockets[GBB_POCKET_PC], GBB_CAP_PC);
  for (int i = 0; i < bag.pockets[GBB_POCKET_KEY].count; i++) {
    uint8_t id = bag.pockets[GBB_POCKET_KEY].entries[i].id;
    CHECKF(id >= 1 && id <= gbb_max_item_id(g), "%s: key[%d] id 0x%02X out of range",
          file, i, id);
    CHECKF(bag.pockets[GBB_POCKET_KEY].entries[i].qty == 1,
          "%s: key[%d] qty != 1 (no quantity byte exists)", file, i);
  }

  for (int i = 0; i < key_n; i++)
    CHECKF(bag.pockets[GBB_POCKET_KEY].entries[i].id == key_ids[i],
          "%s: key[%d] id 0x%02X != 0x%02X", file, i,
          bag.pockets[GBB_POCKET_KEY].entries[i].id, key_ids[i]);
  for (int i = 0; i < ball_n; i++) {
    CHECKF(bag.pockets[GBB_POCKET_BALLS].entries[i].id == ball_ids[i],
          "%s: ball[%d] id 0x%02X != 0x%02X", file, i,
          bag.pockets[GBB_POCKET_BALLS].entries[i].id, ball_ids[i]);
    CHECKF(bag.pockets[GBB_POCKET_BALLS].entries[i].qty == ball_qty[i],
          "%s: ball[%d] qty %u != %u", file, i,
          bag.pockets[GBB_POCKET_BALLS].entries[i].qty, ball_qty[i]);
  }

  /* TM/HM: 57-entry count array, not a list -- just bound each count. */
  CHECKF(gbb_field_present(g, GBB_POCKET_TMHM), "%s: Gen 2 must have TM/HM", file);
  int owned = 0;
  for (int i = 0; i < GBB_TMHM_COUNT; i++) {
    uint8_t c;
    CHECKF(gbb_tmhm_get(&bag, i, &c), "%s: gbb_tmhm_get(%d)", file, i);
    CHECKF(c <= 99, "%s: tmhm[%d] count %u over cap", file, i, c);
    if (c > 0) owned++;
  }
  printf("     TM/HM owned (nonzero count): %d/57\n", owned);
}

/* ------------------------------------------------- P1: gbb_pocket_of() pin
 *
 * BACKLOG #195: independently re-derived from the SAME decomp source the
 * brief cites (pokecrystal's data/items/attributes.asm pocket column) --
 * NOT copied from gb_bag.c's own kBalls/kKey tables, so a shifted range or a
 * dropped id in gb_bag.c actually shows up as a mismatch here rather than
 * both sides agreeing by construction. Every id 1..0xBE (Gen 2's real-item
 * range) and the TM/HM id block 0xBF..0xF9 are checked; Gen 1 gets its own
 * (much simpler) ITEMS-vs-TM/HM check over 1..0xFA. */
static const uint8_t kPinG2Balls[] = {
  0x01, 0x02, 0x04, 0x05, 0x9D, 0x9F, 0xA0, 0xA1, 0xA4, 0xA5, 0xA6, 0xB1,
};
/* Review D2: pokegold's own data/items/attributes.asm -- independently
 * parsed the same way pokecrystal's was -- differs at exactly these four
 * ids: KEY_ITEM (CLEAR_BELL/GS_BALL/BLUE_CARD/EGG_TICKET) in pokecrystal's
 * table, but an unused pocket-ITEM placeholder (ITEM_46/73/74/81) in
 * pokegold's. Split out of the shared Key list so the pin can want
 * GBB_POCKET_KEY on Crystal and GBB_POCKET_COUNT (invalid id) on Gold/
 * Silver for the SAME four ids. */
static const uint8_t kPinG2KeyBoth[] = {
  0x07, 0x36, 0x37, 0x3A, 0x3B, 0x3D, 0x42, 0x43, 0x44, 0x45, 0x47,
  0x7F, 0x80, 0x82, 0x85, 0x86, 0xAF, 0xB2,
};
static const uint8_t kPinG2KeyCrystalOnly[] = { 0x46, 0x73, 0x74, 0x81 };

static bool pin_in(uint8_t id, const uint8_t* set, size_t n) {
  for (size_t i = 0; i < n; i++) if (set[i] == id) return true;
  return false;
}

/* Independently re-derived TM/HM admission test (same four ranges + two
 * holes gb_bag.c's gbb_tmhm_index_of documents, re-checked here rather than
 * called -- a pin calling the function it is pinning could never catch a
 * bug IN that function). */
static bool pin_g2_is_tmhm(uint8_t id) {
  if (id >= 0xBFu && id <= 0xC2u) return true;   /* TM01-04 */
  if (id == 0xC3u) return false;                 /* hole */
  if (id >= 0xC4u && id <= 0xDBu) return true;   /* TM05-28 */
  if (id == 0xDCu) return false;                 /* hole */
  if (id >= 0xDDu && id <= 0xF2u) return true;   /* TM29-50 */
  if (id >= 0xF3u && id <= 0xF9u) return true;   /* HM01-07 */
  return false;
}

/* Review D2: gb_pocket_of()'s Gen-2 answer for one id, per the OTHER game's
 * own attributes.asm (pokegold's, checked separately from pokecrystal's) --
 * `game` must be GBF_G_GS or GBF_G_CRYSTAL. The four Crystal-only ids are
 * the ONE place the two games' own tables disagree; everything else is
 * identical between them. */
static GbBagPocket pin_g2_pocket_of(GbGame game, uint8_t id) {
  if (id > 0xBEu) return pin_g2_is_tmhm(id) ? GBB_POCKET_TMHM : GBB_POCKET_COUNT;
  if (pin_in(id, kPinG2KeyCrystalOnly, sizeof kPinG2KeyCrystalOnly))
    return (game == GBF_G_CRYSTAL) ? GBB_POCKET_KEY : GBB_POCKET_COUNT;
  if (pin_in(id, kPinG2Balls, sizeof kPinG2Balls))      return GBB_POCKET_BALLS;
  if (pin_in(id, kPinG2KeyBoth, sizeof kPinG2KeyBoth))  return GBB_POCKET_KEY;
  return GBB_POCKET_ITEMS;
}

static void pocket_of_pin(void) {
  g_ran++;
  int mismatches = 0;
  static const GbGame kG2Games[2] = { GBF_G_GS, GBF_G_CRYSTAL };
  static const char* const kG2Names[2] = { "GS", "CRYSTAL" };
  for (int g = 0; g < 2; g++) {
    GbGame game = kG2Games[g];
    for (unsigned i = 1; i <= 0xF9u; i++) {
      uint8_t id = (uint8_t)i;
      GbBagPocket got = gbb_pocket_of(game, id);
      GbBagPocket want = pin_g2_pocket_of(game, id);
      if (got != want) {
        mismatches++;
        printf("  !! FAIL: gbb_pocket_of(%s, 0x%02X) = %d, want %d\n", kG2Names[g], id, got, want);
      }
    }
    for (unsigned i = 0xFAu; i <= 0xFFu; i++) {   /* past HM07: never a valid Gen-2 id */
      GbBagPocket got = gbb_pocket_of(game, (uint8_t)i);
      if (got != GBB_POCKET_COUNT) {
        mismatches++;
        printf("  !! FAIL: gbb_pocket_of(%s, 0x%02X) = %d, want GBB_POCKET_COUNT (invalid)\n", kG2Names[g], i, got);
      }
    }
  }
  CHECKF(gbb_pocket_of(GBF_G_GS, 0x00u) == GBB_POCKET_COUNT, "gbb_pocket_of(GS, 0x00) must be COUNT");
  CHECKF(gbb_pocket_of(GBF_G_CRYSTAL, 0x73u) == GBB_POCKET_KEY, "GS_BALL (0x73) is a KEY item on Crystal");
  CHECKF(gbb_pocket_of(GBF_G_GS, 0x73u) == GBB_POCKET_COUNT,
        "Review D2: GS_BALL's id (0x73) is NOT a valid item at all on Gold/Silver "
        "(pokegold's own attributes.asm: an unused ITEM_73 placeholder)");
  CHECKF(gbb_pocket_of(GBF_G_GS, 0x46u) == GBB_POCKET_COUNT, "Review D2: CLEAR_BELL's id invalid on G/S");
  CHECKF(gbb_pocket_of(GBF_G_GS, 0x74u) == GBB_POCKET_COUNT, "Review D2: BLUE_CARD's id invalid on G/S");
  CHECKF(gbb_pocket_of(GBF_G_GS, 0x81u) == GBB_POCKET_COUNT, "Review D2: EGG_TICKET's id invalid on G/S");

  /* Gen 1: ITEMS vs TM/HM only (0xC4..0xFA, no holes -- gb_item_names.c's
   * own gb1_tmhm_label range, re-checked independently here). */
  for (unsigned i = 1; i <= 0xFAu; i++) {
    uint8_t id = (uint8_t)i;
    GbBagPocket got = gbb_pocket_of(GBF_G_RED, id);
    GbBagPocket want = (id >= 0xC4u && id <= 0xFAu) ? GBB_POCKET_TMHM : GBB_POCKET_ITEMS;
    if (got != want) {
      mismatches++;
      printf("  !! FAIL: gbb_pocket_of(RED, 0x%02X) = %d, want %d\n", id, got, want);
    }
  }
  CHECKF(gbb_pocket_of(GBF_G_RED, 0xFBu) == GBB_POCKET_COUNT, "gbb_pocket_of(RED, 0xFB) must be COUNT (past 0xFA)");

  /* gbb_tmhm_index_of(): the count-array index round-trips through the same
   * four ranges, 0-based TM01..HM07, and returns -1 outside them or on Gen 1. */
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xBFu) == 0,  "TM01 index");
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xC2u) == 3,  "TM04 index");
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xC3u) == -1, "hole 0xC3 index");
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xC4u) == 4,  "TM05 index");
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xDBu) == 27, "TM28 index");
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xDCu) == -1, "hole 0xDC index");
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xDDu) == 28, "TM29 index");
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xF2u) == 49, "TM50 index");
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xF3u) == 50, "HM01 index");
  CHECKF(gbb_tmhm_index_of(GBF_G_GS, 0xF9u) == 56, "HM07 index");
  CHECKF(gbb_tmhm_index_of(GBF_G_RED, 0xC9u) == -1, "Gen 1 has no TM/HM count array");

  CHECK(mismatches == 0, "gbb_pocket_of()/gbb_tmhm_index_of() table pin (see FAILs above)");
  printf("  P1: gbb_pocket_of() pinned over 1..0xFF (both gens), %d mismatch(es)\n", mismatches);
}

/* ---------------------------------------------------------------- B0: no-op */

static void noop_zero_diff(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbBag bag;
  CHECKF(gbb_read(&s, &bag), "%s: gbb_read", file);

  /* BACKLOG #195 F3: the pocket table's involvement in the no-op invariant --
   * every id the REAL save currently has stored in a given pocket must
   * classify into that SAME pocket via gbb_pocket_of(), cross-checked
   * against actual cartridge data (not just the pin's own synthetic id
   * sweep above). This runs BEFORE gbb_write() so it exercises gbb_read()'s
   * output directly and can never itself perturb the no-op-diff check
   * below. Gen 1 (Red/Yellow) has no separate Balls/Key pocket to cross-
   * check -- Items entries there may legitimately be a Poke Ball or a key
   * item (gbb_pocket_of()'s own Gen-1 ITEMS-vs-TM/HM-only contract), so
   * only the TM/HM split is checked for that generation. */
  GbGame g = session_game(&s);
  bool gen2 = (g == GBF_G_GS || g == GBF_G_CRYSTAL);
  static const GbBagPocket kRealPockets[3] = { GBB_POCKET_ITEMS, GBB_POCKET_BALLS, GBB_POCKET_KEY };
  int real_checked = 0;
  for (int p = 0; p < (gen2 ? 3 : 1); p++) {
    GbBagPocket pocket = kRealPockets[p];
    const GbBagList* l = &bag.pockets[pocket];
    for (int i = 0; i < l->count; i++) {
      uint8_t id = l->entries[i].id;
      GbBagPocket classified = gbb_pocket_of(g, id);
      if (gen2) {
        CHECKF(classified == pocket,
              "%s: id 0x%02X stored in pocket %d but gbb_pocket_of() says %d",
              file, id, pocket, classified);
      } else {
        /* Gen 1's ONLY real storage pocket is Items -- a TM id legitimately
         * lives there too (gb_bag.h's own "TMs are bag items" note), so
         * gbb_pocket_of() correctly answers TMHM (the filter CATEGORY) for
         * those, not ITEMS (the storage pocket) -- the invariant here is
         * just "classifiable at all", same posture item_build()'s Gen-1
         * category filter (BACKLOG #195 F1) already relies on. */
        CHECKF(classified == GBB_POCKET_ITEMS || classified == GBB_POCKET_TMHM,
              "%s: id 0x%02X stored in Gen-1 Items pocket but gbb_pocket_of() says %d (neither ITEMS nor TMHM)",
              file, id, classified);
      }
      real_checked++;
    }
  }
  for (int i = 0; i < GBB_TMHM_COUNT && gen2; i++) {
    uint8_t c;
    if (!gbb_tmhm_get(&bag, i, &c) || c == 0) continue;
    /* Round-trip: the count array's own index i must map back to a real
     * item id whose gbb_pocket_of() is GBB_POCKET_TMHM (there is no single
     * canonical id per index -- TM01 and HM01 etc. sit at different id
     * OFFSETS depending on which of the four ranges i falls in -- so this
     * checks the INVERSE, gbb_tmhm_index_of() on the id that WOULD produce
     * index i, via the same four-range arithmetic the pin above verified). */
    uint8_t id;
    if (i < 4)        id = (uint8_t)(0xBFu + i);
    else if (i < 28)   id = (uint8_t)(0xC4u + (i - 4));
    else if (i < 50)   id = (uint8_t)(0xDDu + (i - 28));
    else               id = (uint8_t)(0xF3u + (i - 50));
    CHECKF(gbb_tmhm_index_of(g, id) == i, "%s: tmhm[%d] (count %u) round-trip id 0x%02X", file, i, c, id);
    real_checked++;
  }
  printf("  %s: pocket-table cross-check over %d real stored id(s)\n", file, real_checked);

  GbsStatus st = gbb_write(&s, &bag);
  CHECKF(st == GBS_OK, "%s: no-op gbb_write status %s", file, gbs_status_text(st));

  uint32_t diff = 0, first = 0;
  for (uint32_t i = 0; i < len; i++)
    if (g_img[i] != g_orig[i]) { if (!diff) first = i; diff++; }
  CHECKF(diff == 0,
        "%s: no-op gbb_write changed %u byte(s), first at 0x%04X (0x%02X -> 0x%02X)",
        file, diff, first, g_orig[first], g_img[first]);
}

/* D3(c) mutation note (verified by hand, not shipped as code): forcing gbb_write's
 * final `return changed ? gbs_finish(s) : GBS_OK;` to unconditionally call
 * gbs_finish() is only caught by this test (and by B0's zero-byte-diff test) on
 * Gold.sav/Crystal.sav. It is NOT caught on Red.sav/Yellow.sav, because Gen 1's
 * gbs_finish() is a genuine no-op (source/gb_session.c: "Gen 1 has no backup
 * mirror and no deferred checksum ... there is nothing left to close out" ->
 * returns GBS_OK without touching the image at all); Gen 1's checksum is instead
 * fixed inline by gen1_write_range_ex on every actual field write, so calling
 * gbs_finish an extra time on an already-no-op path has no observable effect for
 * that generation. 2/4 saves catch this specific mutation; the other 2/4 mutations
 * prescribed by the review (memset-not-memcpy on new_body, and cnt_changed-only)
 * are caught on all applicable saves (see the fix-pass report). */
/* Generation-independent no-op detector (D3c): rather than reading Gold's checksum
 * byte specifically, ask each generation's own save module where ITS stored
 * checksum lives (GEN1_OFF_CHECKSUM for Gen 1; g2_checksum_primary_off(version)
 * for Gen 2), corrupt that one byte, then prove a true no-op gbb_write() never
 * calls gbs_finish() (which would have repaired it) by asserting the corrupted
 * byte is STILL corrupt afterwards. Works on all four saves. */
static void noop_checksum_untouched(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK,
        "%s: open (checksum no-op)", file);
  GbBag bag;
  CHECKF(gbb_read(&s, &bag), "%s: gbb_read (checksum no-op)", file);

  uint32_t off = (s.gen == GB_GEN1) ? GEN1_OFF_CHECKSUM
                                     : g2_checksum_primary_off(s.g2w.sv.version);
  uint8_t before = g_img[off];
  g_img[off] = (uint8_t)~before;   /* corrupt the stored checksum */

  GbsStatus st = gbb_write(&s, &bag);
  CHECKF(st == GBS_OK, "%s: no-op gbb_write status %s (checksum no-op)",
        file, gbs_status_text(st));
  CHECKF(g_img[off] == (uint8_t)~before,
        "%s: a true no-op must not repair the checksum byte at 0x%04X", file, off);

  g_img[off] = before;   /* restore */
}

/* ---------------------------------------------------------------- B: round trip */

static void protect(uint32_t off, uint32_t len) {
  for (uint32_t i = 0; i < len && off + i < sizeof g_protect; i++) g_protect[off + i] = 1;
}

static void protect_mirrored(G2Version ver, uint32_t off, uint32_t len) {
  const G2MirrorRegion* map;
  int n = g2_mirror_map(ver, &map);
  for (int r = 0; r < n; r++) {
    uint32_t from = map[r].from, to = map[r].to;
    uint32_t lo = off, hi = off + len;
    uint32_t clo = lo > from ? lo : from;
    uint32_t chi = hi < to + 1 ? hi : to + 1;
    if (clo < chi) protect(map[r].dest + (clo - from), chi - clo);
  }
}

static void protect_field(GbGame g, uint8_t gen, G2Version ver, GbField f) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len) return;
  protect(off, len);
  if (gen == GB_GEN2) protect_mirrored(ver, off, len);
}

/* One full round trip: insert into every pocket the game has, write, re-open, read
 * back, and prove the only bytes that moved are the touched pockets' own count/body
 * fields (plus their Gen-2 mirror and stored checksums). */
static void roundtrip(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  printf("  -- roundtrip %s\n", file);

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(s.gen == expect_gen, "%s: gen", file);
  GbGame g = session_game(&s);
  G2Version ver = (expect_gen == GB_GEN2) ? s.g2w.sv.version : G2_VER_NONE;

  GbBag bag;
  CHECKF(gbb_read(&s, &bag), "%s: gbb_read before edit", file);

  uint8_t items_want_qty, pc_want_qty;
  uint8_t items_id = qty_pocket_edit(g, &bag.pockets[GBB_POCKET_ITEMS], GBB_CAP_ITEMS,
                                     7u, &items_want_qty);
  uint8_t pc_id = qty_pocket_edit(g, &bag.pockets[GBB_POCKET_PC], GBB_CAP_PC,
                                  3u, &pc_want_qty);
  CHECKF(gbb_insert(g, &bag, GBB_POCKET_ITEMS, items_id, 7u) == GBB_OK,
        "%s: insert into Items", file);
  CHECKF(gbb_insert(g, &bag, GBB_POCKET_PC, pc_id, 3u) == GBB_OK,
        "%s: insert into PC store", file);
  bool has_key = gbb_field_present(g, GBB_POCKET_KEY);
  bool has_balls = gbb_field_present(g, GBB_POCKET_BALLS);
  bool has_tmhm = gbb_field_present(g, GBB_POCKET_TMHM);
  uint8_t key_id = 0, balls_id = 0, balls_want_qty = 0;
  if (has_key) {
    key_id = pick_unused_id(g, &bag.pockets[GBB_POCKET_KEY]);
    CHECKF(gbb_insert(g, &bag, GBB_POCKET_KEY, key_id, 1u) == GBB_OK,
          "%s: insert into Key items", file);
  }
  if (has_balls) {
    balls_id = qty_pocket_edit(g, &bag.pockets[GBB_POCKET_BALLS], GBB_CAP_BALLS,
                               5u, &balls_want_qty);
    CHECKF(gbb_insert(g, &bag, GBB_POCKET_BALLS, balls_id, 5u) == GBB_OK,
          "%s: insert into Balls", file);
  }
  if (has_tmhm)
    CHECKF(gbb_tmhm_set(g, &bag, 0, 42u) == GBB_OK, "%s: tmhm_set(0)", file);

  GbsStatus st = gbb_write(&s, &bag);
  CHECKF(st == GBS_OK, "%s: gbb_write status %s", file, gbs_status_text(st));

  GbSession s2;
  bool s2_ok = gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK;
  CHECKF(s2_ok, "%s: re-open after edit", file);
  GbBag bag2;
  bool bag2_ok = s2_ok && gbb_read(&s2, &bag2);
  CHECKF(bag2_ok, "%s: gbb_read after edit", file);

  if (bag2_ok) {
    bool found = false;
    for (int i = 0; i < bag2.pockets[GBB_POCKET_ITEMS].count; i++)
      if (bag2.pockets[GBB_POCKET_ITEMS].entries[i].id == items_id &&
          bag2.pockets[GBB_POCKET_ITEMS].entries[i].qty == items_want_qty) found = true;
    CHECKF(found, "%s: Items insert did not round-trip", file);

    found = false;
    for (int i = 0; i < bag2.pockets[GBB_POCKET_PC].count; i++)
      if (bag2.pockets[GBB_POCKET_PC].entries[i].id == pc_id &&
          bag2.pockets[GBB_POCKET_PC].entries[i].qty == pc_want_qty) found = true;
    CHECKF(found, "%s: PC store insert did not round-trip", file);

    if (has_key) {
      found = false;
      for (int i = 0; i < bag2.pockets[GBB_POCKET_KEY].count; i++)
        if (bag2.pockets[GBB_POCKET_KEY].entries[i].id == key_id) found = true;
      CHECKF(found, "%s: Key items insert did not round-trip", file);
    }
    if (has_balls) {
      found = false;
      for (int i = 0; i < bag2.pockets[GBB_POCKET_BALLS].count; i++)
        if (bag2.pockets[GBB_POCKET_BALLS].entries[i].id == balls_id &&
            bag2.pockets[GBB_POCKET_BALLS].entries[i].qty == balls_want_qty) found = true;
      CHECKF(found, "%s: Balls insert did not round-trip", file);
    }
    if (has_tmhm) {
      uint8_t c = 0;
      CHECKF(gbb_tmhm_get(&bag2, 0, &c) && c == 42u,
            "%s: tmhm[0] did not round-trip", file);
    }
  }

  memset(g_protect, 0, sizeof g_protect);
  protect_field(g, expect_gen, ver, GBF_BAG_COUNT);
  protect_field(g, expect_gen, ver, GBF_BAG_BODY);
  protect_field(g, expect_gen, ver, GBF_PC_COUNT);
  protect_field(g, expect_gen, ver, GBF_PC_BODY);
  if (has_key) {
    protect_field(g, expect_gen, ver, GBF_KEY_ITEMS_COUNT);
    protect_field(g, expect_gen, ver, GBF_KEY_ITEMS_BODY);
  }
  if (has_balls) {
    protect_field(g, expect_gen, ver, GBF_BALLS_COUNT);
    protect_field(g, expect_gen, ver, GBF_BALLS_BODY);
  }
  if (has_tmhm) protect_field(g, expect_gen, ver, GBF_TMHM_COUNTS);
  if (expect_gen == GB_GEN1) {
    protect(GEN1_OFF_CHECKSUM, 1);
  } else {
    protect(g2_checksum_primary_off(ver), 2);
    protect(g2_checksum_backup_off(ver), 2);
  }

  uint32_t unexplained = 0, first = 0;
  for (uint32_t i = 0; i < len; i++) {
    if (g_img[i] != g_orig[i] && !g_protect[i]) {
      if (!unexplained) first = i;
      unexplained++;
    }
  }
  CHECKF(unexplained == 0,
        "%s: %u byte(s) changed outside the touched pockets, first at 0x%04X (0x%02X -> 0x%02X)",
        file, unexplained, first, g_orig[first], g_img[first]);
}

/* ---------------------------------------------------------- B2: single-pocket write
 * Mirror of P1a's one_field_only: change exactly one pocket -> gbs_open() still
 * succeeds (a Gen-2 skipped gbs_finish() shows up as a stale-checksum reopen
 * failure) -> re-read shows it, and every OTHER pocket's bytes are untouched. */
static void one_pocket_only(const char* file, uint8_t expect_gen, GbBagPocket pocket,
                            const char* label) {
  uint32_t len = load(file);
  if (!len) return;
  g_ran++;

  GbSession s;
  bool open_ok = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK;
  CHECKF(!open_ok || s.gen == expect_gen, "%s: opened gen %u, expected %u", file, s.gen, expect_gen);
  CHECKF(open_ok, "%s/%s: open", file, label);
  if (!open_ok) return;
  GbGame g = session_game(&s);
  if (!gbb_field_present(g, pocket)) return;   /* this generation lacks it: not a failure */

  GbBag bag;
  CHECKF(gbb_read(&s, &bag), "%s/%s: gbb_read", file, label);

  uint8_t want_id = 0, want_qty = 0;
  if (pocket == GBB_POCKET_TMHM) {
    CHECKF(gbb_tmhm_set(g, &bag, 5, 17u) == GBB_OK, "%s/%s: tmhm_set", file, label);
  } else if (pocket == GBB_POCKET_KEY) {
    want_id = pick_unused_id(g, &bag.pockets[pocket]);
    want_qty = 1u;
    GbBagOpStatus ost = gbb_insert(g, &bag, pocket, want_id, 1u);
    CHECKF(ost == GBB_OK, "%s/%s: insert status %d", file, label, ost);
  } else {
    int cap = gbb_pocket_cap(g, pocket);
    want_id = qty_pocket_edit(g, &bag.pockets[pocket], cap, 9u, &want_qty);
    GbBagOpStatus ost = gbb_insert(g, &bag, pocket, want_id, 9u);
    CHECKF(ost == GBB_OK, "%s/%s: insert status %d", file, label, ost);
  }

  GbsStatus st = gbb_write(&s, &bag);
  CHECKF(st == GBS_OK, "%s/%s: gbb_write status %s", file, label, gbs_status_text(st));

  GbSession s2;
  bool reopen_ok = gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK;
  CHECKF(reopen_ok, "%s/%s: re-open after single-pocket write (stale checksum?)",
        file, label);
  if (!reopen_ok) return;

  GbBag bag2;
  bool reread_ok = gbb_read(&s2, &bag2);
  CHECKF(reread_ok, "%s/%s: gbb_read after single-pocket write", file, label);
  if (!reread_ok) return;

  if (pocket == GBB_POCKET_TMHM) {
    uint8_t c = 0;
    CHECKF(gbb_tmhm_get(&bag2, 5, &c) && c == 17u,
          "%s/%s: tmhm[5] did not read back changed", file, label);
  } else {
    bool found = false;
    for (int i = 0; i < bag2.pockets[pocket].count; i++)
      if (bag2.pockets[pocket].entries[i].id == want_id &&
          bag2.pockets[pocket].entries[i].qty == want_qty) found = true;
    CHECKF(found, "%s/%s: the changed pocket did not read back as changed", file, label);
  }
}

static void one_pocket_only_all(void) {
  static const char* const g1files[] = { "Red.sav", "Yellow.sav" };
  for (size_t i = 0; i < sizeof g1files / sizeof g1files[0]; i++) {
    one_pocket_only(g1files[i], GB_GEN1, GBB_POCKET_ITEMS, "Items");
    one_pocket_only(g1files[i], GB_GEN1, GBB_POCKET_PC, "PC");
  }
  static const char* const g2files[] = { "Gold.sav", "Crystal.sav" };
  for (size_t i = 0; i < sizeof g2files / sizeof g2files[0]; i++) {
    one_pocket_only(g2files[i], GB_GEN2, GBB_POCKET_ITEMS, "Items");
    one_pocket_only(g2files[i], GB_GEN2, GBB_POCKET_KEY, "Key items");
    one_pocket_only(g2files[i], GB_GEN2, GBB_POCKET_BALLS, "Balls");
    one_pocket_only(g2files[i], GB_GEN2, GBB_POCKET_PC, "PC");
    one_pocket_only(g2files[i], GB_GEN2, GBB_POCKET_TMHM, "TM/HM");
  }
}

/* ---------------------------------------------------------------- C: refusals */

static void refusals(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(s.gen == expect_gen, "%s: gen", file);
  GbGame g = session_game(&s);

  GbBag bag;
  CHECKF(gbb_read(&s, &bag), "%s: gbb_read", file);

  /* bad id: 0x00, 0xFF, and (Gen 2 only) a TM/HM-numbering id past NUM_ITEMS */
  CHECKF(gbb_insert(g, &bag, GBB_POCKET_ITEMS, 0x00u, 1u) == GBB_ERR_BADID,
        "%s: id 0x00 refused", file);
  CHECKF(gbb_insert(g, &bag, GBB_POCKET_ITEMS, 0xFFu, 1u) == GBB_ERR_BADID,
        "%s: id 0xFF (terminator) refused", file);
  if (expect_gen == GB_GEN2)
    CHECKF(gbb_insert(g, &bag, GBB_POCKET_ITEMS, 0xC4u, 1u) == GBB_ERR_BADID,
          "%s: Gen-2 TM/HM-numbering id 0xC4 refused as an Items id", file);
  else
    CHECKF(gbb_insert(g, &bag, GBB_POCKET_ITEMS, 0xC4u, 1u) == GBB_OK,
          "%s: Gen-1 TM01 item id 0xC4 accepted in the bag", file);

  /* bad qty */
  CHECKF(gbb_insert(g, &bag, GBB_POCKET_ITEMS, 0x03u, 0u) == GBB_ERR_QTY,
        "%s: qty 0 refused", file);
  CHECKF(gbb_insert(g, &bag, GBB_POCKET_ITEMS, 0x03u, 100u) == GBB_ERR_QTY,
        "%s: qty 100 (over 99) refused", file);

  /* full pocket: fill Items to its cap with distinct ids, then one more must refuse */
  GbBag full;
  memset(&full, 0, sizeof full);
  int cap = GBB_CAP_ITEMS;
  for (int i = 0; i < cap; i++) {
    uint8_t id = (uint8_t)(1 + i);   /* 0x01..0x14, all valid on every game */
    CHECKF(gbb_insert(g, &full, GBB_POCKET_ITEMS, id, 1u) == GBB_OK,
          "%s: fill Items slot %d", file, i);
  }
  CHECKF(gbb_insert(g, &full, GBB_POCKET_ITEMS, (uint8_t)(cap + 1), 1u) == GBB_ERR_FULL,
        "%s: 21st distinct id into a full Items pocket refused as GBB_ERR_FULL", file);
  /* a full pocket still MERGES into an id already present */
  CHECKF(gbb_insert(g, &full, GBB_POCKET_ITEMS, 0x01u, 5u) == GBB_OK,
        "%s: merging into an existing id in a full pocket still works", file);

  /* saturating merge (D1): inserting more than the remaining headroom sets the
   * stack to GBB_QTY_CAP and reports GBB_ERR_QTY -- the real games open a
   * second slot for the overflow; this core does not, so it must not
   * silently discard the remainder. Built in memory, no session needed. */
  GbBag sat;
  memset(&sat, 0, sizeof sat);
  CHECKF(gbb_insert(g, &sat, GBB_POCKET_ITEMS, 0x14u, 99u) == GBB_OK,
        "%s: fill a stack to 99", file);
  CHECKF(gbb_insert(g, &sat, GBB_POCKET_ITEMS, 0x14u, 10u) == GBB_ERR_QTY,
        "%s: merge into a stack already at 99 saturates and refuses", file);
  CHECKF(sat.pockets[GBB_POCKET_ITEMS].entries[0].qty == 99u,
        "%s: saturated stack qty still 99", file);
  CHECKF(sat.pockets[GBB_POCKET_ITEMS].count == 1,
        "%s: saturated merge did not add a second entry", file);

  GbBag sat2;
  memset(&sat2, 0, sizeof sat2);
  CHECKF(gbb_insert(g, &sat2, GBB_POCKET_ITEMS, 0x14u, 60u) == GBB_OK,
        "%s: seed a stack of 60", file);
  CHECKF(gbb_insert(g, &sat2, GBB_POCKET_ITEMS, 0x14u, 50u) == GBB_ERR_QTY,
        "%s: merge 50 into 60 (over cap) saturates and refuses", file);
  CHECKF(sat2.pockets[GBB_POCKET_ITEMS].entries[0].qty == 99u,
        "%s: 60+50 saturated to 99", file);

  GbBag sat3;
  memset(&sat3, 0, sizeof sat3);
  CHECKF(gbb_insert(g, &sat3, GBB_POCKET_ITEMS, 0x14u, 60u) == GBB_OK,
        "%s: seed a stack of 60 (ok merge)", file);
  CHECKF(gbb_insert(g, &sat3, GBB_POCKET_ITEMS, 0x14u, 30u) == GBB_OK,
        "%s: merge 30 into 60 (under cap) succeeds", file);
  CHECKF(sat3.pockets[GBB_POCKET_ITEMS].entries[0].qty == 90u,
        "%s: 60+30 == 90", file);

  /* pocket the game lacks: Gen 1 asking for Key items/Balls/TM-HM */
  if (expect_gen == GB_GEN1) {
    CHECKF(gbb_insert(g, &bag, GBB_POCKET_KEY, 0x01u, 1u) == GBB_ERR_NOT_PRESENT,
          "%s: Key items refused as GBB_ERR_NOT_PRESENT on Gen 1", file);
    CHECKF(gbb_insert(g, &bag, GBB_POCKET_BALLS, 0x01u, 1u) == GBB_ERR_NOT_PRESENT,
          "%s: Balls refused as GBB_ERR_NOT_PRESENT on Gen 1", file);
    CHECKF(gbb_tmhm_set(g, &bag, 0, 5u) == GBB_ERR_NOT_PRESENT,
          "%s: TM/HM refused as GBB_ERR_NOT_PRESENT on Gen 1", file);
  } else {
    /* Key items: no quantity -- qty != 1 refused, and a duplicate id is refused */
    CHECKF(gbb_insert(g, &bag, GBB_POCKET_KEY, 0x07u, 2u) == GBB_ERR_QTY,
          "%s: Key item qty != 1 refused", file);
    GbBag k = bag;
    GbBagOpStatus first = gbb_insert(g, &k, GBB_POCKET_KEY, 0x40u, 1u);
    CHECKF(first == GBB_OK || first == GBB_ERR_FULL,
          "%s: first Key insert is OK or the pocket was already full", file);
    if (first == GBB_OK)
      CHECKF(gbb_insert(g, &k, GBB_POCKET_KEY, 0x40u, 1u) == GBB_ERR_ARG,
            "%s: duplicate Key item id refused", file);

    /* TM/HM count over cap */
    CHECKF(gbb_tmhm_set(g, &bag, 0, 100u) == GBB_ERR_QTY,
          "%s: TM/HM count 100 (over 99) refused", file);
    CHECKF(gbb_tmhm_set(g, &bag, 0, 99u) == GBB_OK,
          "%s: TM/HM count 99 (at cap) accepted", file);
    CHECKF(gbb_tmhm_set(g, &bag, -1, 1u) == GBB_ERR_ARG,
          "%s: TM/HM index -1 refused", file);
    CHECKF(gbb_tmhm_set(g, &bag, 57, 1u) == GBB_ERR_ARG,
          "%s: TM/HM index 57 (out of range) refused", file);
  }

  /* remove/set_qty out-of-range slot */
  CHECKF(gbb_remove(g, &bag, GBB_POCKET_ITEMS, -1) == GBB_ERR_ARG,
        "%s: remove slot -1 refused", file);
  CHECKF(gbb_remove(g, &bag, GBB_POCKET_ITEMS, bag.pockets[GBB_POCKET_ITEMS].count)
        == GBB_ERR_ARG, "%s: remove slot == count refused", file);
  CHECKF(gbb_set_qty(g, &bag, GBB_POCKET_ITEMS, 0, 0u) == GBB_ERR_QTY,
        "%s: set_qty 0 refused", file);

  /* D3(a): slot == count is one past the last occupied slot, out of range */
  CHECKF(gbb_set_qty(g, &bag, GBB_POCKET_ITEMS, bag.pockets[GBB_POCKET_ITEMS].count, 5u)
        == GBB_ERR_ARG, "%s: set_qty slot == count refused", file);
  /* D3(b): gbb_tmhm_get at exactly GBB_TMHM_COUNT is out of range */
  {
    uint8_t c;
    CHECKF(gbb_tmhm_get(&bag, GBB_TMHM_COUNT, &c) == false,
          "%s: gbb_tmhm_get(GBB_TMHM_COUNT) refused", file);
  }
}

/* D6: assert the pocket cap/entry-shape literals in gb_bag.c's static k_pocket[]
 * table against what gb_fields.c actually generated, for every game x every list
 * pocket the game has. Uses the pure accessors gbb_body_field/gbb_count_field
 * rather than duplicating k_pocket[] here. */
static void check_pocket_caps(void) {
  static const GbGame games[] = { GBF_G_RED, GBF_G_YELLOW, GBF_G_GS, GBF_G_CRYSTAL };
  static const GbBagPocket pockets[] = {
    GBB_POCKET_ITEMS, GBB_POCKET_KEY, GBB_POCKET_BALLS, GBB_POCKET_PC
  };
  static const char* gname[] = { "RED", "YELLOW", "GS", "CRYSTAL" };
  static const char* pname[] = { "ITEMS", "KEY", "BALLS", "PC" };

  for (size_t gi = 0; gi < sizeof games / sizeof games[0]; gi++) {
    GbGame g = games[gi];

    for (size_t pi = 0; pi < sizeof pockets / sizeof pockets[0]; pi++) {
      GbBagPocket p = pockets[pi];
      if (!gbb_field_present(g, p)) continue;   /* game lacks the pocket */
      g_ran++;

      int cap = gbb_pocket_cap(g, p);
      int esz = (p == GBB_POCKET_KEY) ? 1 : 2;   /* KEY carries no quantity */
      GbField body = gbb_body_field(p);
      uint16_t want = (uint16_t)(cap * esz + 1);
      uint16_t have = gbf_len(g, body);
      CHECKF(want == have,
            "%s/%s: cap %d * entry %d + 1 == %u, but gbf_len == %u",
            gname[gi], pname[pi], cap, esz, want, have);
    }

    if (gbb_field_present(g, GBB_POCKET_TMHM)) {
      g_ran++;
      uint16_t tlen = gbf_len(g, GBF_TMHM_COUNTS);
      CHECKF(tlen == GBB_TMHM_COUNT,
            "%s/TMHM: gbf_len == %u, expected GBB_TMHM_COUNT (%d)",
            gname[gi], tlen, GBB_TMHM_COUNT);
    }
  }
}

/* ---------------------------------------------------------------- P0: key-item
 * composition (BACKLOG #99, b99 review P0). No corpus needed -- gbb_g1_key_item_
 * compose() is pure C over a synthetic bitmap, not a real ROM read. The bitmap
 * mirrors the real cartridge's own shape (review N2/this review): badges 21..28
 * set, ids 4/20/29 clear (neighbours of the badge run), ids 89/104/111 set (the
 * "unswept 81..120 block" the located table settled), id 97 clear. */
static void set_ki_bit(uint8_t bits[15], unsigned id) {
  unsigned i = id - 1u;
  bits[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

static void key_item_compose(void) {
  g_ran++;
  uint8_t bits[15];
  memset(bits, 0, sizeof bits);
  for (unsigned id = 21; id <= 28; id++) set_ki_bit(bits, id);   /* 8 badges */
  set_ki_bit(bits, 89);
  set_ki_bit(bits, 104);
  set_ki_bit(bits, 111);

  /* Explicit spot checks (the ids the review named). */
  CHECK(gbb_g1_key_item_compose(true, bits, 21) == true, "table: badge 21 is key");
  CHECK(gbb_g1_key_item_compose(true, bits, 28) == true, "table: badge 28 is key");
  CHECK(gbb_g1_key_item_compose(true, bits, 4)  == false, "table: id 4 is not key");
  CHECK(gbb_g1_key_item_compose(true, bits, 20) == false, "table: id 20 is not key");
  CHECK(gbb_g1_key_item_compose(true, bits, 29) == false, "table: id 29 is not key");
  CHECK(gbb_g1_key_item_compose(true, bits, 89)  == true, "table: id 89 is key (located)");
  CHECK(gbb_g1_key_item_compose(true, bits, 104) == true, "table: id 104 is key (located)");
  CHECK(gbb_g1_key_item_compose(true, bits, 111) == true, "table: id 111 is key (located)");
  CHECK(gbb_g1_key_item_compose(true, bits, 97)  == false, "table: id 97 is not key");

  for (unsigned tbl = 0; tbl <= 1; tbl++) {
    bool have_table = tbl != 0;
    for (unsigned id = 0x00; id <= 0xFF; id++) {
      bool got = gbb_g1_key_item_compose(have_table, bits, (uint8_t)id);
      if (id >= 0xC4 && id <= 0xC8) {
        /* HM01..HM05: key on BOTH paths, the game's own path never reaches
         * the table either way. */
        CHECKF(got == true, "id 0x%02X (HM): expected key on BOTH paths (have_table=%d)",
              id, (int)have_table);
      } else if (id >= 0xC9) {
        /* TM01..TM50 (0xC9..0xFA) plus the rest of the byte range up to
         * 0xFF: all >= 0xC4 and not an HM id, so compose()'s own first
         * branch returns false unconditionally, table or not. */
        CHECKF(got == false, "id 0x%02X (>= HM range, not an HM): expected NOT key (have_table=%d)",
              id, (int)have_table);
      } else if (id == 0x00) {
        CHECKF(got == false, "id 0x00: expected NOT key (have_table=%d)", (int)have_table);
      } else if (have_table && id >= 121 && id <= 195) {
        CHECKF(got == false, "id 0x%02X (>120, table path): expected NOT key", id);
      } else if (!have_table && id >= 121 && id <= 195) {
        bool want = gbb_is_g1_key_item((uint8_t)id);
        CHECKF(got == want, "id 0x%02X (list path): expected to match gbb_is_g1_key_item (%d)",
              id, (int)want);
      } else if (have_table) {
        /* 1..120, outside 0xC4..0xFA: the synthetic table bit, computed the
         * same way set_ki_bit wrote it. */
        unsigned i = id - 1u;
        bool want = (bits[i >> 3] >> (i & 7u)) & 1u;
        CHECKF(got == want, "id 0x%02X (table path, 1..120): expected the table bit", id);
      } else {
        bool want = gbb_is_g1_key_item((uint8_t)id);
        CHECKF(got == want, "id 0x%02X (list path, 1..120): expected to match gbb_is_g1_key_item",
              id);
      }
    }
  }
}

/* ---------------------------------------------------------------- main */

int main(void) {
  printf("== A0: pocket caps vs generated field lengths (D6) ==\n");
  check_pocket_caps();

  printf("== A: reads against docs/GEN12-PARITY-DESIGN.md Appendix A ==\n");
  {
    static const uint8_t red_bag_ids[] = { 0xCD, 0xCE, 0xE3, 0xE5, 0xEB, 0xEC, 0xC5, 0xC6, 0x05, 0x06 };
    static const uint8_t red_bag_qty[] = {    3,   19,   69,   40,   61,    1,    1,    1,    1,    1 };
    static const uint8_t red_pc_ids[]  = { 0x14, 0x13, 0x12, 0x11, 0x10, 0x36, 0x34, 0x0E, 0x50, 0x52 };
    static const uint8_t red_pc_qty[]  = {    3,    1,    1,    2,    2,   88,    1,    1,   55,   55 };
    expect_red_or_yellow("Red.sav", GB_GEN1, 19, 50,
                         red_bag_ids, red_bag_qty, 10, red_pc_ids, red_pc_qty, 10);
    expect_red_or_yellow("Yellow.sav", GB_GEN1, 16, 32, NULL, NULL, 0, NULL, NULL, 0);
  }
  {
    static const uint8_t gold_key_ids[] = {
      0x07, 0x36, 0x37, 0x3A, 0x3B, 0x3D, 0x42, 0x44, 0x47, 0x7F, 0x85, 0x86, 0xAF, 0xB2
    };
    static const uint8_t gold_ball_ids[] = { 0x01, 0x02, 0x04, 0x05 };
    static const uint8_t gold_ball_qty[] = {   23,    7,    4,    6 };
    expect_gen2("Gold.sav", GB_GEN2, 17, 14, 4, 50,
               gold_key_ids, 14, gold_ball_ids, gold_ball_qty, 4);
    expect_gen2("Crystal.sav", GB_GEN2, 11, 16, 11, 32, NULL, 0, NULL, NULL, 0);
  }

  printf("== P0: Gen-1 key-item composition (BACKLOG #99) ==\n");
  key_item_compose();

  printf("== P1: gbb_pocket_of()/gbb_tmhm_index_of() table pin (BACKLOG #195) ==\n");
  pocket_of_pin();

  printf("== B0: no-op read->write is a zero-byte diff ==\n");
  noop_zero_diff("Red.sav");
  noop_zero_diff("Yellow.sav");
  noop_zero_diff("Gold.sav");
  noop_zero_diff("Crystal.sav");

  printf("== B0b: no-op leaves a corrupted stored checksum untouched (all 4 saves) ==\n");
  noop_checksum_untouched("Red.sav");
  noop_checksum_untouched("Yellow.sav");
  noop_checksum_untouched("Gold.sav");
  noop_checksum_untouched("Crystal.sav");

  printf("== B: round trips ==\n");
  roundtrip("Red.sav", GB_GEN1);
  roundtrip("Yellow.sav", GB_GEN1);
  roundtrip("Gold.sav", GB_GEN2);
  roundtrip("Crystal.sav", GB_GEN2);

  printf("== B2: single-pocket write ==\n");
  one_pocket_only_all();

  printf("== C: refusals ==\n");
  refusals("Red.sav", GB_GEN1);
  refusals("Gold.sav", GB_GEN2);
  refusals("Crystal.sav", GB_GEN2);

  if (!g_ran) printf("  (no corpus present -- structural checks only)\n");
  printf("%s: %d/%d checks passed over %d save use(s)\n",
        g_fail ? "FAIL" : "ok", g_check - g_fail, g_check, g_ran);
  return g_fail ? 1 : 0;
}
