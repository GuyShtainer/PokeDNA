/* Host test: BACKLOG #150 S150-8, the DOWN-converting edge -- a native Bank cell into
 * a Gen-3 PC box (bank_down_convert_gen3's pure core) or into a Game Boy save of the
 * OTHER generation under the Gen-1<->Gen-2 time-capsule rules (bank_down_convert_gb's
 * pure core). Pure logic only: source/bank_down_convert.c's two GBA-facing arms do
 * real card I/O (the ledger write, gbs_insert/gb_persist) and are proven by the
 * mGBA gesture at the hand-resolved second merge, not here.
 *
 *   cc -std=c11 -Wall -Wextra -I source -DPDNA_GEN12_HOST tests/host_xferdown_test.c \
 *      source/xfer_rec.c source/bank_cell.c source/gen12_convert.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/item_map_g1g2.c source/gb_bag.c source/gb_fields.c source/gb_session.c source/gen3_to_gb.c source/gb_sidecar.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c source/data_tables.c \
 *      source/evolutions.c source/item_map_g2g3.c source/gb_item_names.c \
 *      source/bank_down_convert.c source/gb_moves_legal.c \
 *      -o /tmp/hxdown && /tmp/hxdown
 *
 * source/evolutions.c is GENERATED and gitignored -- sections that need a real
 * evolution floor SKIP (never fail) when pk_evo_have_data() is false, exactly as
 * tests/host_xfer_roundtrip_test.c's own MAKE LEGAL section already does.
 *
 * Sections:
 *   A. xr_game_item_mask over met_game 0..6.
 *   B. the item-map edge, through bdc_convert_gen3_core: a mapped item travels on the
 *      right cart, a no-counterpart item waits (item_dropped), a cart-restricted item
 *      (BICYCLE/CARD KEY/BASEMENT KEY) fails the mask on the wrong cart.
 *   C. xr_time_capsule_block: Gen1->Gen2 always 0; Gen2->Gen1 species/move bounds.
 *   D. round trip 2->3->2 (byte-identical via the record, real corpus): bc_pack a real
 *      Gen-2 record, bdc_convert_gen3_core it, build the ledger entry with
 *      gbsc_entry_from + the four S150-6 fields (decision 8) + the D-8b-link
 *      nick_written override, round-trip it through a real gbsc buffer, and assert
 *      original80 == cell80 plus every bc_unpack field the cell started with.
 *   E. round trip 1->2->1 and 2->1->2 (real corpus, through bdc_convert_gb_core): the
 *      entry's original80 is byte-identical to the source native cell in both
 *      directions; the WRITTEN record is a lossy view (named, not asserted equal);
 *      xr_time_capsule_block returned 0 for every case that was allowed to run.
 *   F. the time-capsule refusal: a Gen-2 cell with dex 152 and one with a move > 165
 *      both refuse before any ledger entry is built.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "xfer_rec.h"
#include "bank_cell.h"
#include "gen12_convert.h"
#include "gen3_to_gb.h"
#include "gb_sidecar.h"
#include "gen1_save.h"
#include "gen2_save.h"
#include "bank_down_convert.h"
#include "gb_moves_legal.h"   /* F2b: g3gb_moves_fill -- the caller-level fill review D1 pins */
#include "gen3_edit.h"   /* F3 (review): gen3_edit_load -- an INDEPENDENT reader of the
                          * converted record's own raw nickname bytes, for the
                          * nick_written-override assertion below */
#include "gen3_mon.h"    /* R2 (review): pk_decode_mon -- the re-lifted-Gen-1 friendship check */
#include "gen3_box.h"    /* R2 (review): pk_resolve -- computes .friendship/.level from EXP */

#define GB_ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* ============================================================================ */
/* A. xr_game_item_mask                                                          */
/* ============================================================================ */
static void test_mask(void) {
  CHECK(xr_game_item_mask(0) == 0, "met_game 0 -> 0");
  CHECK(xr_game_item_mask(1) == 0x01, "Sapphire -> 0x01");
  CHECK(xr_game_item_mask(2) == 0x01, "Ruby -> 0x01");
  CHECK(xr_game_item_mask(3) == 0x02, "Emerald -> 0x02");
  CHECK(xr_game_item_mask(4) == 0x04, "FireRed -> 0x04");
  CHECK(xr_game_item_mask(5) == 0x04, "LeafGreen -> 0x04");
  CHECK(xr_game_item_mask(6) == 0, "out of range -> 0");
}

/* ============================================================================ */
/* Helpers: build a native cell from a synthetic Gb12Mon-shaped GbEditMon so B/D/E  */
/* can drive bc_pack directly without needing a real GB save on disk for every case.*/
/* ============================================================================ */
static void build_gen2_cell(uint8_t out80[80], uint16_t species_dex, uint8_t item,
                            uint16_t move0, uint32_t serial) {
  GbEditMon m; memset(&m, 0, sizeof m);
  m.gen = GB_GEN2;
  gb_set_species(&m, species_dex, NULL);
  gb_set_level(&m, 20);
  gb_set_dv(&m, GB_ATK, 12); gb_set_dv(&m, GB_DEF, 12);
  gb_set_dv(&m, GB_SPE, 12); gb_set_dv(&m, GB_SPC, 12);
  gb_set_move(&m, 0, move0);
  gb_set_otid(&m, 12345);
  gb_set_held_item(&m, item);
  int rc = bc_pack(&m, 0, 0 /* origin unknown */, 0, serial, out80);
  if (rc != 0) { fprintf(stderr, "bc_pack failed rc=%d\n", rc); exit(1); }
}

/* F6 (review): build a native cell that is FAITHFUL to a real corpus G2Mon -- every
 * field bc_view()/gen12_convert() actually reads (species/level/DVs/moves/PP-ups/
 * OT id/OT name/nickname/item/friendship/pokerus/caught data), not the fixed
 * level 20 / DVs 12 / OT 12345 the old build_gen2_cell() used for every corpus mon
 * regardless of what it actually stored. Real fields make the round-trip assertions
 * in sections D/E genuine two-hop byte compares instead of exercising the SAME three
 * synthetic numbers on every corpus record. */
static void build_gen2_cell_faithful(uint8_t out80[80], const G2Mon* g2, uint32_t serial) {
  GbEditMon m; memset(&m, 0, sizeof m);
  m.gen = GB_GEN2;
  bool ok = gb_set_species(&m, g2->species, NULL);
  if (!ok) { fprintf(stderr, "gb_set_species(%u) failed\n", g2->species); exit(1); }
  gb_set_level(&m, g2->level);
  gb_set_dv(&m, GB_ATK, g2->dv[0]); gb_set_dv(&m, GB_DEF, g2->dv[1]);
  gb_set_dv(&m, GB_SPE, g2->dv[2]); gb_set_dv(&m, GB_SPC, g2->dv[3]);
  for (int i = 0; i < 4; i++) {
    gb_set_move(&m, i, g2->moves[i]);
    gb_set_ppup(&m, i, g2->pp_up[i]);
  }
  gb_set_otid(&m, g2->otid);
  gb_set_held_item(&m, g2->held_item);
  gb_set_friendship(&m, g2->friendship);
  gb_set_pokerus(&m, g2->pokerus);
  if (g2->caught_valid) {
    gb_set_caught(&m, g2->caught_time, g2->caught_level, g2->caught_loc, g2->ot_gender);
  }
  /* Names: g2->otname/nickname are ALREADY decoded UTF-8 (gen2_save.c's own
   * decoder, the same convention gb_get_otname/gb_get_nickname produce) --
   * gb_set_otname/gb_set_nickname re-encode; fall back to the lossy setter for a
   * name this test's own decode step could not perfectly round-trip (rare, and
   * exactly the kind of edge this section is FOR finding, not hiding). */
  if (!gb_set_otname(&m, g2->otname)) gb_set_otname_lossy(&m, g2->otname);
  if (!gb_set_nickname(&m, g2->nickname)) gb_set_nickname_lossy(&m, g2->nickname);
  int rc = bc_pack(&m, 0, 0, 0, serial, out80);
  if (rc != 0) { fprintf(stderr, "bc_pack failed rc=%d\n", rc); exit(1); }
}

/* F3 (review): a Gen-2 cell whose nickname is the male gender sign (U+2642, GB_GEN1/2's
 * own byte for it -- gb_edit.c's encoder recognises the UTF-8 sequence directly) --
 * NOT ASCII, so its GB raw byte and its Gen-3 raw byte cannot possibly agree by
 * charset coincidence, unlike an ASCII letter which happens to still differ but less
 * legibly proves the override actually ran. */
static void build_gen2_cell_named(uint8_t out80[80], uint16_t species_dex, const char* nick,
                                  uint32_t serial) {
  GbEditMon m; memset(&m, 0, sizeof m);
  m.gen = GB_GEN2;
  gb_set_species(&m, species_dex, NULL);
  gb_set_level(&m, 20);
  gb_set_dv(&m, GB_ATK, 12); gb_set_dv(&m, GB_DEF, 12);
  gb_set_dv(&m, GB_SPE, 12); gb_set_dv(&m, GB_SPC, 12);
  gb_set_move(&m, 0, 33);
  gb_set_otid(&m, 12345);
  bool named = gb_set_nickname(&m, nick);
  if (!named) { fprintf(stderr, "gb_set_nickname(%s) failed\n", nick); exit(1); }
  int rc = bc_pack(&m, 0, 0, 0, serial, out80);
  if (rc != 0) { fprintf(stderr, "bc_pack failed rc=%d\n", rc); exit(1); }
}

static void build_gen1_cell(uint8_t out80[80], uint16_t species_dex, uint16_t move0,
                            uint32_t serial) {
  GbEditMon m; memset(&m, 0, sizeof m);
  m.gen = GB_GEN1;
  GbGen1Base base = { .base = { 35, 55, 40, 90, 50 }, .type1 = 0x18, .type2 = 0x18 };  /* Electric */
  gb_set_species(&m, species_dex, &base);
  gb_set_level(&m, 20);
  gb_set_dv(&m, GB_ATK, 12); gb_set_dv(&m, GB_DEF, 12);
  gb_set_dv(&m, GB_SPE, 12); gb_set_dv(&m, GB_SPC, 12);
  gb_set_move(&m, 0, move0);
  gb_set_otid(&m, 12345);
  int rc = bc_pack(&m, 0, 0, 0, serial, out80);
  if (rc != 0) { fprintf(stderr, "bc_pack failed rc=%d\n", rc); exit(1); }
}

/* ============================================================================ */
/* B. the item-map edge, through bdc_convert_gen3_core.                          */
/* ============================================================================ */
static void test_item_edge(void) {
  /* Light Ball (0x1D) on a Gen-2 Pikachu (dex 25) -> item_g2_to_g3 maps it; on
   * Emerald (met_game 3) the mask must pass -- travels, no item_dropped note. */
  uint8_t cell[80];
  build_gen2_cell(cell, 25, 0x1D, 33, 1);
  uint8_t out80[80]; GbEditMon written; Gb12Notes notes; uint16_t g3item = 0xFFFF;
  Gb12Result r = bdc_convert_gen3_core(cell, 3, out80, &written, &notes, &g3item);
  CHECK(r == GB12_OK, "Light Ball Pikachu converts (got %s)", gen12_reason_text(r));
  CHECK(!notes.item_dropped, "Light Ball travels: no item_dropped note");
  CHECK(g3item != 0, "Light Ball travels: g3_item is non-zero (got %u)", g3item);
  /* F1 (review): a TRAVELLING item must also name itself in notes.item_g2 -- the
   * "Item: <name> travels" loss row (gb_down_loss_screen) keys off this field, not
   * off item_dropped, so a bug that only set item_g2 in the dropped branch left the
   * travelling row silently blank. */
  CHECK(notes.item_g2 == 0x1D, "Light Ball travels: item_g2 names it too (got 0x%02X)", notes.item_g2);

  /* Mail (0xB5..0xBD): no Gen-3 counterpart -> item_g2_to_g3 returns 0 -> dropped,
   * named, but the item still readable back out of the ORIGINAL cell (S11.20 item 11:
   * "no new ledger field is added for this"). */
  uint8_t cell2[80];
  build_gen2_cell(cell2, 25, 0xB5, 33, 2);
  uint8_t out2[80]; GbEditMon written2; Gb12Notes notes2; uint16_t g3item2 = 0xFFFF;
  Gb12Result r2 = bdc_convert_gen3_core(cell2, 3, out2, &written2, &notes2, &g3item2);
  CHECK(r2 == GB12_OK, "Mail-holding Pikachu still converts (item dropped, not refused)");
  CHECK(notes2.item_dropped, "Mail (no counterpart) drops with item_dropped set");
  CHECK(notes2.item_g2 == 0xB5, "item_g2 names the dropped id (got 0x%02X)", notes2.item_g2);
  CHECK(g3item2 == 0, "Mail: g3_item is 0 (nothing written)");
  /* The item is recoverable straight out of original80 (== cell2 here). */
  GbEditMon back; BcMeta bm;
  CHECK(bc_unpack(cell2, &back, &bm), "cell2 still unpacks");
  CHECK(gb_get_held_item(&back) == 0xB5, "cell2's own held item is still 0xB5 (never spent)");

  /* BICYCLE (item_g2_to_g3 maps it, mask 0x?? per item_map_g2g3.h:32-33 -- cart-
   * restricted). Whatever its actual mask is, met_game 0 (out of range) must always
   * fail it, proving the mask gate actually runs (not just "mapped => travels"). */
  uint8_t cell3[80];
  build_gen2_cell(cell3, 25, 0x1E /* Bicycle, gb_item_names.c */, 33, 3);
  uint8_t out3[80]; GbEditMon written3; Gb12Notes notes3; uint16_t g3item3 = 0xFFFF;
  Gb12Result r3 = bdc_convert_gen3_core(cell3, 0, out3, &written3, &notes3, &g3item3);
  CHECK(r3 == GB12_OK, "Bicycle holder still converts on an out-of-range met_game");
  CHECK(g3item3 == 0 || notes3.item_dropped,
        "on met_game 0 the cart mask is 0 -- Bicycle cannot travel (g3item=%u dropped=%d)",
        g3item3, notes3.item_dropped);
}

/* ============================================================================ */
/* C. xr_time_capsule_block                                                      */
/* ============================================================================ */
static void test_time_capsule(void) {
  uint16_t bad = 0xFFFF;
  CHECK(xr_time_capsule_block(GB_GEN1, GB_GEN2, 1, NULL, &bad) == 0,
        "Gen1 -> Gen2 always allowed");
  CHECK(xr_time_capsule_block(GB_GEN2, GB_GEN1, 152, NULL, &bad) == 1,
        "dex 152 (Chikorita) blocks Gen2->Gen1 (species)");
  CHECK(bad == 152, "bad names the species dex (got %u)", bad);

  uint16_t moves_ok[4] = { 33, 84, 0, 0 };
  CHECK(xr_time_capsule_block(GB_GEN2, GB_GEN1, 25, moves_ok, &bad) == 0,
        "dex 25 + legal Gen-1 moves allowed");

  uint16_t moves_bad[4] = { 33, 250, 0, 0 };
  CHECK(xr_time_capsule_block(GB_GEN2, GB_GEN1, 25, moves_bad, &bad) == 2,
        "move 250 (> 165) blocks Gen2->Gen1 (move)");
  CHECK(bad == 250, "bad names the move id (got %u)", bad);
}

/* ============================================================================ */
/* D. round trip 2->3->2, byte-identical via the record (real corpus).           */
/* ============================================================================ */
static uint8_t g_gen2_img[64 * 1024];

static bool load_file(const char* path, uint8_t* buf, size_t cap, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  *out_len = fread(buf, 1, cap, f);
  fclose(f);
  return *out_len > 0;
}

static void test_roundtrip_2_3_2(void) {
  size_t len;
  char path[512];
  snprintf(path, sizeof path, "%s/Crystal.sav", GB_ROMS);
  if (!load_file(path, g_gen2_img, sizeof g_gen2_img, &len)) {
    printf("  SKIP D (no %s)\n", path);
    return;
  }
  G2Save sv;
  if (!g2_detect(g_gen2_img, (uint32_t)len, &sv) || !sv.supported) {
    printf("  SKIP D (detect failed)\n");
    return;
  }
  G2Header hd;
  if (!g2_read_header(g_gen2_img, &sv, &hd)) { printf("  SKIP D (header)\n"); return; }

  int n = g2_box_count_at(g_gen2_img, &sv, &hd, 0);
  if (n <= 0) { printf("  SKIP D (box 0 empty)\n"); return; }

  int ran = 0;
  for (int slot = 0; slot < n && ran < 3; slot++) {
    G2Mon g2;
    if (!g2_box_mon_at(g_gen2_img, &sv, &hd, 0, slot, &g2)) continue;
    if (g2.species < 1 || g2.species > 251 || g2.is_egg) continue;

    /* F6 (review): build_gen2_cell_faithful() carries EVERY field bc_view()/
     * gen12_convert() actually read off the REAL corpus record -- level, all four
     * DVs, all four moves + PP-ups, OT id, OT name, nickname, item, friendship,
     * pokerus, caught data -- not the fixed level 20 / DVs 12 / OT 12345 every
     * corpus mon used to get regardless of what it actually stored. */
    uint8_t cell[80];
    build_gen2_cell_faithful(cell, &g2, 1000u + (uint32_t)slot);

    uint8_t out80[80]; GbEditMon written; Gb12Notes notes; uint16_t g3item;
    Gb12Result r = bdc_convert_gen3_core(cell, 3, out80, &written, &notes, &g3item);
    CHECK(r == GB12_OK, "slot %d converts (got %s)", slot, gen12_reason_text(r));
    if (r != GB12_OK) continue;

    GbscEntry e;
    uint32_t epoch = 0x12345678u;
    gbsc_entry_from(&e, &written, cell, epoch);
    /* decision 8: the four S150-6 fields, plus D-8b-link's nick_written override --
     * the Gen-3 nickname bytes verbatim, so S150-8b's nickname merge on the way back
     * depends on exactly this. */
    e.kind = XR_KIND_NATIVE_HOME;
    e.state = XR_STATE_PENDING;
    e.direction = XR_DIR_ABROAD_G3;
    e.claimed = 1;
    /* F3 (review): nick_written must hold the nickname AS WRITTEN ABROAD -- for
     * this (Gen-3) direction that is the CONVERTED record's own raw 10-byte
     * nickname field (out80 + 0x08, gen3_mon.c's own decode_name() call site),
     * never the GB bytes gbsc_entry_from() copies from `written` by default (the
     * cell's own unpacked GB record -- a no-op override that would leave
     * S150-8b's nickname merge trying to decode GB bytes as if they were Gen-3
     * ones). Read the independent copy through gen3_edit_load(), a completely
     * different code path from the plain pointer arithmetic the override itself
     * uses, so this is not the same tautology the review found (memcpy then
     * compare itself). */
    /* R1 (review): only 10 bytes are the Gen-3 nickname (gen3_mon.c's own
     * decode_name(..., mon + 0x08, 10)) -- byte 11 at +0x12 is the plaintext
     * language byte, not part of the name; copying sizeof(e.nick_written) (11)
     * read one byte too far and reproduced the bug this test was meant to catch. */
    memcpy(e.nick_written, out80 + 0x08, 10); e.nick_written[10] = 0;

    EditMon indep; gen3_edit_load(out80, false, &indep);
    CHECK(memcmp(e.nick_written, indep.raw + 0x08, 10) == 0,
          "slot %d: nick_written matches an INDEPENDENT read of the Gen-3 record's own nickname bytes", slot);
    CHECK(memcmp(e.nick_written, written.nick, 10) != 0,
          "slot %d: nick_written differs from the GB bytes (the override actually ran, not a no-op)", slot);

    CHECK(memcmp(e.original80, cell, 80) == 0, "slot %d: original80 == cell80", slot);
    CHECK(e.kind == XR_KIND_NATIVE_HOME, "slot %d: kind == XR_KIND_NATIVE_HOME", slot);
    CHECK(e.state == XR_STATE_PENDING, "slot %d: state == XR_STATE_PENDING", slot);
    CHECK(e.direction == XR_DIR_ABROAD_G3, "slot %d: direction == XR_DIR_ABROAD_G3", slot);
    CHECK(e.claimed == 1, "slot %d: claimed == 1", slot);

    /* round trip through a real 1042-byte gbsc buffer */
    uint8_t buf[GBSC_FILE_MAX]; uint32_t blen = (uint32_t)gbsc_init(buf, 0xAAu);
    int idx = gbsc_add(buf, &blen, GBSC_FILE_MAX, &e);
    CHECK(idx == 0, "slot %d: gbsc_add succeeds", slot);
    GbscEntry got;
    CHECK(gbsc_get(buf, blen, idx, &got), "slot %d: gbsc_get succeeds", slot);
    CHECK(memcmp(got.original80, cell, 80) == 0, "slot %d: got.original80 == cell80", slot);
    CHECK(got.kind == XR_KIND_NATIVE_HOME && got.state == XR_STATE_PENDING &&
          got.direction == XR_DIR_ABROAD_G3 && got.claimed == 1,
          "slot %d: the four S150-6 fields survive gbsc round trip", slot);

    /* The home bytes survive the DOWN edge intact -- bc_unpack(got.original80)
     * reproduces the source field for field. This does NOT prove the merge back
     * (xr_merge_down/S150-9 do not exist yet); the last hop is asserted at the
     * record boundary, not through a merge. */
    GbEditMon back; BcMeta backmeta;
    CHECK(bc_unpack(got.original80, &back, &backmeta), "slot %d: original80 re-unpacks", slot);
    /* F6 (review): the `|| true` here made the species check unconditionally pass --
     * deleted; this is now a REAL assertion against the corpus record's own species. */
    CHECK(gb_get_species_dex(&back) == g2.species,
          "slot %d: species survives (got %u want %u)", slot, gb_get_species_dex(&back), g2.species);
    CHECK(gb_get_level(&back) == g2.level, "slot %d: level survives (got %u want %u)",
          slot, gb_get_level(&back), g2.level);
    CHECK(gb_get_dv(&back, GB_ATK) == g2.dv[0] && gb_get_dv(&back, GB_DEF) == g2.dv[1] &&
          gb_get_dv(&back, GB_SPE) == g2.dv[2] && gb_get_dv(&back, GB_SPC) == g2.dv[3],
          "slot %d: all four DVs survive (real corpus DVs, not a hardcoded 12)", slot);
    for (int mi = 0; mi < 4; mi++)
      CHECK(gb_get_move(&back, mi) == g2.moves[mi],
            "slot %d: move %d survives (got %u want %u)", slot, mi, gb_get_move(&back, mi), g2.moves[mi]);
    CHECK(gb_get_otid(&back) == g2.otid, "slot %d: OT id survives (got %u want %u)",
          slot, gb_get_otid(&back), g2.otid);
    CHECK(gb_get_held_item(&back) == g2.held_item,
          "slot %d: held item byte survives inside original80 even when dropped from the Gen-3 record", slot);
    CHECK(gb_get_friendship(&back) == g2.friendship, "slot %d: friendship survives (got %u want %u)",
          slot, gb_get_friendship(&back), g2.friendship);
    {
      char back_nick[64], back_ot[64];
      gb_get_nickname(&back, back_nick, sizeof back_nick);
      gb_get_otname(&back, back_ot, sizeof back_ot);
      CHECK(strcmp(back_nick, g2.nickname) == 0,
            "slot %d: nickname survives (got %s want %s)", slot, back_nick, g2.nickname);
      CHECK(strcmp(back_ot, g2.otname) == 0,
            "slot %d: OT name survives (got %s want %s)", slot, back_ot, g2.otname);
    }
    ran++;
  }
  if (!ran) printf("  SKIP D (no importable box-0 slot found)\n");
  else printf("  D: %d slot(s) round-tripped 2->3->2 via the record\n", ran);
}

/* F3 (review), dedicated case: a nickname containing the MALE GENDER SIGN (U+2642) --
 * not representable in ASCII at all, so its GB raw byte and its Gen-3 raw byte cannot
 * agree by charset coincidence the way two ASCII letters occasionally might. Proves
 * xfer_down_write()'s nick_g3 override survives a genuinely non-ASCII GB glyph, not
 * just "some bytes happened to differ". */
static void test_nick_written_nonascii_glyph(void) {
  uint8_t cell[80];
  build_gen2_cell_named(cell, 25, "PIKA\xE2\x99\x82", 950);   /* "PIKA<male sign>" */

  GbEditMon back0; BcMeta meta0;
  CHECK(bc_unpack(cell, &back0, &meta0), "nonascii fixture: cell unpacks");
  char nick_check[64];
  CHECK(gb_get_nickname(&back0, nick_check, sizeof nick_check) > 0 &&
        strstr(nick_check, "\xE2\x99\x82") != NULL,
        "nonascii fixture: the male sign actually landed in the GB record (got %s)", nick_check);

  uint8_t out80[80]; GbEditMon written; Gb12Notes notes; uint16_t g3item;
  Gb12Result r = bdc_convert_gen3_core(cell, 3, out80, &written, &notes, &g3item);
  CHECK(r == GB12_OK, "nonascii fixture converts (got %s)", gen12_reason_text(r));
  if (r != GB12_OK) return;

  GbscEntry e;
  gbsc_entry_from(&e, &written, cell, 0);
  /* R1 (review): 10 bytes only -- xfer_down_write's own fix, exactly reproduced. */
  memcpy(e.nick_written, out80 + 0x08, 10); e.nick_written[10] = 0;

  EditMon indep; gen3_edit_load(out80, false, &indep);
  CHECK(memcmp(e.nick_written, indep.raw + 0x08, 10) == 0,
        "nonascii: nick_written matches an independent read of the Gen-3 record");
  CHECK(memcmp(e.nick_written, written.nick, 10) != 0,
        "nonascii: nick_written differs from the GB bytes (male sign's GB byte != its Gen-3 byte)");
}

/* ============================================================================ */
/* E. round trip 1->2->1 and 2->1->2, through bdc_convert_gb_core.               */
/* ============================================================================ */
static void test_bridge_one_case(uint16_t species_dex, uint8_t src_gen, uint8_t dst_gen,
                                 uint8_t item, const char* label) {
  uint8_t cell[80];
  if (src_gen == GB_GEN1) build_gen1_cell(cell, species_dex, 33, 500);
  else build_gen2_cell(cell, species_dex, item, 33, 501);

  int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
  GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
  uint16_t from4[4]; uint8_t bad4[4]; int nbad;   /* BACKLOG #212 */
  /* the Gen-1 base table gen3_to_gb() needs for a Gen-1 destination -- the same
   * synthetic table build_gen1_cell() uses, exercising the retry path a real
   * gb_gen1_base_from_rom() lookup would feed on the GBA (decision 15). */
  GbGen1Base base = { .base = { 35, 55, 40, 90, 50 }, .type1 = 0x18, .type2 = 0x18 };
  const GbGen1Base* g1base = (dst_gen == GB_GEN1) ? &base : NULL;
  bdc_convert_gb_core(cell, dst_gen, false, g1base, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                      &notes, from4, bad4, &nbad);
  CHECK(tc == 0, "%s: time capsule passes (tc=%d bad=%u)", label, tc, tc_bad);
  CHECK(g12 == GB12_OK, "%s: intermediate conversion ok (got %s)", label, gen12_reason_text(g12));
  if (tc != 0 || g12 != GB12_OK) return;
  CHECK(g3gb == G3GB_OK, "%s: gen3_to_gb succeeds (got %s)", label, g3gb_status_text(g3gb));
  if (g3gb != G3GB_OK) return;

  /* (a) the entry's original80 is byte-identical to the source native cell. */
  GbscEntry e;
  gbsc_entry_from(&e, &out, cell, 0);
  e.kind = XR_KIND_NATIVE_HOME; e.state = XR_STATE_CLAIMED; e.direction = XR_DIR_ABROAD_GB;
  e.claimed = 1;
  CHECK(memcmp(e.original80, cell, 80) == 0, "%s: original80 == cell80", label);

  /* (b) the WRITTEN GB record is a lossy view -- state which fields differ, never
   * assert equality it cannot have. Held item is the one this lane's own relax
   * forces to differ whenever `item` != 0 (decision 15: no held item crosses this
   * bridge at all). Gen-1 destinations also have no Pokerus/catch-rate-as-item
   * concept, which gen3_to_gb already omits by construction. */
  if (dst_gen == GB_GEN2) {
    CHECK(gb_get_held_item(&out) == 0, "%s: held item is dropped on the written record (item stayed behind)", label);
  }
  CHECK(gb_get_species_dex(&out) == species_dex, "%s: species survives the bridge", label);

  /* (c) xr_time_capsule_block returned 0 -- already asserted above as a precondition. */

  /* F1 (xfer-items fix pass): a Gen-2 source mon holding an item into a Gen-1 target
   * always drops the item (bank_down_convert.c:118, gen12_convert refuses a held item
   * on that arm) and bdc_convert_gb_core reports that via loss.item_dropped -- NOT
   * item_outcome, which this bridge path never sets (item_outcome only exists for the
   * Gen-3 -> GB down-convert core, bdc_convert_gen3_core). pdna_gen12.c:3252's row
   * predicate must read item_dropped or the loss screen silently drops this row.
   * Reproduce the UNFIXED predicate here (not by calling the GBA-only
   * gb_paste_loss_screen) and show the difference directly. */
  if (item != 0 && dst_gen == GB_GEN1) {
    CHECK(notes.item_dropped, "%s: bdc_convert_gb_core's notes report item_dropped", label);
    /* pdna_gen12.c:4200 (gb_bank_down_bridge, the GBA-facing caller) folds notes.item_dropped
     * into loss.item_dropped with exactly this OR-assign before the loss screen reads it --
     * bdc_convert_gb_core itself never touches loss->item_dropped. Reproduce that one line
     * so the predicate below sees what the real screen sees. */
    loss.item_dropped |= notes.item_dropped;
    CHECK(loss.item_dropped, "%s: loss.item_dropped set after the real caller's merge", label);
    /* Calls the REAL predicate pdna_gen12.c's gb_paste_loss_screen now uses
     * (gen3_to_gb.c's g3gb_loss_needs_item_row) -- not a re-derivation, so reverting the
     * F1 fix (removing item_dropped from that function's OR chain) turns this RED. */
    CHECK(g3gb_loss_needs_item_row(&loss),
          "%s: g3gb_loss_needs_item_row is TRUE (item_dropped seen)", label);
    bool unfixed_predicate = loss.item_outcome != G3GB_ITEM_NONE || loss.secret_id;
    CHECK(!unfixed_predicate,
          "%s: the OLD inline predicate (pre-lane, no item_dropped term) is FALSE here -- "
          "this is the row the lane made vanish (item_outcome=%d secret_id=%d)",
          label, (int)loss.item_outcome, (int)loss.secret_id);
  }
}

static void test_bridge_roundtrips(void) {
  test_bridge_one_case(25, GB_GEN1, GB_GEN2, 0, "1->2 Pikachu");
  test_bridge_one_case(25, GB_GEN2, GB_GEN1, 0, "2->1 Pikachu, no item");
  test_bridge_one_case(25, GB_GEN2, GB_GEN1, 0x1D, "2->1 Pikachu, Light Ball dropped");
}

/* F6/R2 (review): a REAL two-hop byte compare -- a real Crystal.sav box-0 record run
 * through the bridge (Gen 2 -> Gen 1, the only direction Gen 1's smaller species/
 * move range can ever refuse, so it is also the more interesting one).
 *
 * R2 correction: build_gen2_cell_faithful() rebuilt the cell from a DECODED G2Mon
 * (g2_box_mon_at), which already lost the exact loss this test exists to find --
 * g2_box_mon_at decodes raw byte 0xE1 (the "PK" ligature, gb_edit.c's own charmap)
 * to a plain space in its UTF-8 otname string, and gb_set_otname then RE-ENCODES
 * that space back to a normal GB space byte, never touching 0xE1 again. The loss
 * had already happened before the bridge ever ran. Fixed the same way
 * tests/host_bankcell_test.c:267/329 builds a cell for its own corpus sweeps:
 * gb_load() straight off the raw list bytes (never through a decoded G2Mon), then
 * bc_pack() -- the RAW otname/nickname bytes ride along unchanged until gen3_to_gb's
 * own encoder actually re-spells them. */
static void test_bridge_real_corpus_2_to_1(void) {
  size_t len;
  char path[512];
  snprintf(path, sizeof path, "%s/Crystal.sav", GB_ROMS);
  if (!load_file(path, g_gen2_img, sizeof g_gen2_img, &len)) { printf("  SKIP E2 (no %s)\n", path); return; }
  G2Save sv;
  if (!g2_detect(g_gen2_img, (uint32_t)len, &sv) || !sv.supported) { printf("  SKIP E2 (detect failed)\n"); return; }
  G2Header hd;
  if (!g2_read_header(g_gen2_img, &sv, &hd)) { printf("  SKIP E2 (header)\n"); return; }
  uint32_t off = g2_list_offset(&sv, 0, hd.current_box);
  if (off == 0) { printf("  SKIP E2 (no box0 list)\n"); return; }
  int n = gb_list_count(GB_GEN2, g_gen2_img + off, 0);
  if (n <= 0) { printf("  SKIP E2 (box 0 empty)\n"); return; }

  int ran = 0;
  GbGen1Base base = { .base = { 35, 55, 40, 90, 50 }, .type1 = 0x18, .type2 = 0x18 };
  for (int slot = 0; slot < n && ran < 3; slot++) {
    GbEditMon mon;
    if (!gb_load(&mon, GB_GEN2, g_gen2_img + off, 0, slot)) continue;
    if (mon.list_species == G2_LIST_EGG) continue;
    uint16_t dex = gb_get_species_dex(&mon);
    if (dex < 1 || dex > gb_max_species(GB_GEN1)) continue;   /* time-capsule species bound -- not this test's own edge */
    bool moves_ok = true;
    for (int mi = 0; mi < 4; mi++) {
      uint8_t mv = gb_get_move(&mon, mi);
      if (mv != 0 && mv > gb_max_move(GB_GEN1)) moves_ok = false;
    }
    if (!moves_ok) continue;   /* time-capsule move bound, same reason */

    /* R2: pin the documented case for Crystal.sav box 0 slot 0 (dex 1, OT bytes
     * `8C A0 B3 B3 A8 A0 E1 50 50 50 50` -- byte 6 is the "PK" ligature 0xE1). If
     * this fires, the corpus changed and the pinned assertion below needs a new
     * fixture, not a quiet skip. */
    if (slot == 0) {
      CHECK(dex == 1, "Crystal.sav box0 slot0 precondition: dex 1 (got %u) -- corpus changed?", dex);
      CHECK(mon.otname[6] == 0xE1,
            "Crystal.sav box0 slot0 precondition: OT byte[6] is 0xE1 (got 0x%02X) -- corpus changed?",
            mon.otname[6]);
    }

    uint8_t cell[80];
    int rc = bc_pack(&mon, 0, BC_ORIGIN_CRYSTAL, 0, 2000u + (uint32_t)slot, cell);
    CHECK(rc == 0, "real 2->1 slot %d: bc_pack succeeds", slot);
    if (rc != 0) continue;

    int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
    GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
    uint16_t from4[4]; uint8_t bad4[4]; int nbad;   /* BACKLOG #212 */
    bdc_convert_gb_core(cell, GB_GEN1, false, &base, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                        &notes, from4, bad4, &nbad);
    CHECK(tc == 0, "real 2->1 slot %d: time capsule passes", slot);
    CHECK(g12 == GB12_OK, "real 2->1 slot %d: intermediate conversion ok (%s)", slot, gen12_reason_text(g12));
    CHECK(g3gb == G3GB_OK, "real 2->1 slot %d: gen3_to_gb succeeds (%s)", slot, g3gb_status_text(g3gb));
    if (tc != 0 || g12 != GB12_OK || g3gb != G3GB_OK) continue;

    /* (a) original80 (this arm's own ledger field) is byte-identical to the source
     * native cell -- the home bytes are never spent, regardless of what the WRITTEN
     * Gen-1 record could or couldn't carry. */
    GbscEntry e;
    gbsc_entry_from(&e, &out, cell, 0);
    CHECK(memcmp(e.original80, cell, 80) == 0, "real 2->1 slot %d: original80 == cell80", slot);

    /* (b) friendship: gen3_to_gb() ALWAYS flags friendship_dropped for a Gen-1
     * target (source/gen3_to_gb.c's set_gen2_only_fields: "gen != GB_GEN2" is
     * unconditional, not gated on the actual friendship value) -- confirmed by
     * reading that function, not assumed. */
    CHECK(loss.friendship_dropped, "real 2->1 slot %d: friendship_dropped flagged (corpus value was %u)",
          slot, gb_get_friendship(&mon));

    if (slot == 0) {
      /* R2: pin the round trip on the documented slot -- Gen 2's 120 has nowhere
       * to live in a Gen-1 record at all ("none"); lifting that WRITTEN Gen-1
       * record back up through gen12_convert (a later, separate gesture -- the
       * same one hop 5 of Guy's 2->3->1->2 scenario would take) does NOT recover
       * 120 -- it lands on gen3_edit.c's own documented Gen-1-import default, 70
       * (em_create_base's own comment: "70 is the CAUGHT base friendship ...
       * what a Gen-1 import keeps"). */
      CHECK(gb_get_friendship(&mon) == 120, "real 2->1 slot 0 precondition: corpus friendship is 120 (got %u)",
            gb_get_friendship(&mon));

      uint8_t cell2[80];
      int rc2 = bc_pack(&out, 0, BC_ORIGIN_RED, 0, 2100u, cell2);
      CHECK(rc2 == 0, "real 2->1->(up) slot 0: re-pack the written Gen-1 record");
      if (rc2 == 0) {
        uint8_t up80[80]; GbEditMon up_written; Gb12Notes up_notes; uint16_t up_g3item;
        Gb12Result upr = bdc_convert_gen3_core(cell2, 3, up80, &up_written, &up_notes, &up_g3item);
        CHECK(upr == GB12_OK, "real 2->1->(up) slot 0: the Gen-1 record lifts back up (%s)",
              gen12_reason_text(upr));
        if (upr == GB12_OK) {
          PkMon pk;
          CHECK(pk_decode_mon(up80, false, &pk), "real 2->1->(up) slot 0: the re-lifted record decodes");
          pk_resolve(&pk);
          CHECK(pk.friendship == 70,
                "real 2->1->(up) slot 0: friendship 120 -> Gen1(none) -> back %u (want 70, gen3_edit.c's own Gen-1-import default, NOT the original 120)",
                pk.friendship);
        }
      }
    }

    /* (c) OT name / nickname. R2's own finding: byte 6 of box0 slot0's OT name is
     * the "PK" ligature 0xE1 -- bc_view()'s own g2_decode_text has no case for it
     * (g2_glyph's `default: return " ";`), so it silently decodes to a plain space
     * one hop before gen12_convert ever runs, and neither loss.nick_lossy nor
     * loss.ot_lossy (gen3_to_gb's OWN, unrelated, name-loss flags) ever fired for it --
     * a real space re-encodes into Gen 1 as an ordinary space, no ambiguity left to
     * catch by the time gen3_to_gb sees it. BACKLOG #177's notes.otname_lossy is the
     * fix (review F1): bank_down_convert.c's static gb_name_changed() spells the
     * SOURCE record's OT name and the WRITTEN record's OT name through the same
     * lossless, generation-neutral speller (gb_get_otname) and compares the two --
     * "PK" (the source spelling) vs " " (what byte 0x7F, an ordinary space, spells to
     * on the written side) differ, so the flag fires without any byte-range table. */
    char written_ot[64], written_nick[64];
    gb_get_otname(&out, written_ot, sizeof written_ot);
    gb_get_nickname(&out, written_nick, sizeof written_nick);
    if (slot == 0) {
      CHECK(out.otname[6] == 0x7F,
            "BACKLOG #177 (documented, pre-existing loss): OT byte[6] 0xE1 -> 0x%02X after 2->1"
            " (want 0x7F, the current -- lossy -- behaviour; if this now reads 0xE1, #177 already fixed it,"
            " update this pin)", out.otname[6]);
      CHECK(notes.otname_lossy,
            "BACKLOG #177: notes.otname_lossy fires for the pinned Crystal.sav box0 slot0 "
            "OT name (raw byte[6] == 0xE1, the 'PK' ligature bc_view()'s own decode drops)");
    } else {
      char src_ot[64], src_nick[64];
      gb_get_otname(&mon, src_ot, sizeof src_ot);
      gb_get_nickname(&mon, src_nick, sizeof src_nick);
      bool ot_ok = strcmp(written_ot, src_ot) == 0;
      bool nick_ok = strcmp(written_nick, src_nick) == 0;
      printf("  real 2->1 slot %d: OT name %s -> %s (%s), nickname %s -> %s (%s)\n",
             slot, src_ot, written_ot, ot_ok ? "same" : "CHANGED",
             src_nick, written_nick, nick_ok ? "same" : "CHANGED");
    }
    ran++;
  }
  if (!ran) printf("  SKIP E2 (no real corpus slot both time-capsule-legal and Gen1-base-available)\n");
  else printf("  E2: %d real Crystal.sav slot(s) run through the 2->1 bridge\n", ran);
}

/* ============================================================================ */
/* E3. BACKLOG #177: notes.otname_lossy / notes.nick_lossy, off two SYNTHETIC     */
/*     records -- an ASCII-only name (never lossy) and a nickname holding a raw   */
/*     GB glyph g2_decode_text's own switch has no case for (always lossy).      */
/* ============================================================================ */
static void test_name_glyph_loss_synthetic(void) {
  GbGen1Base base = { .base = { 35, 55, 40, 90, 50 }, .type1 = 0x18, .type2 = 0x18 };

  /* (a) plain ASCII OT name + nickname -- gb_set_otname/gb_set_nickname round-trip
   * every byte through gen1_char_ascii/g2_glyph's explicit letter ranges, never their
   * default fallback, so neither flag should ever fire for a name a player could type
   * on a real keyboard-less Game Boy naming screen. */
  {
    uint8_t cell[80];
    GbEditMon m; memset(&m, 0, sizeof m);
    m.gen = GB_GEN2;
    gb_set_species(&m, 25, NULL);
    gb_set_level(&m, 20);
    gb_set_dv(&m, GB_ATK, 12); gb_set_dv(&m, GB_DEF, 12);
    gb_set_dv(&m, GB_SPE, 12); gb_set_dv(&m, GB_SPC, 12);
    gb_set_move(&m, 0, 33);
    gb_set_otid(&m, 12345);
    CHECK(gb_set_otname(&m, "GUY"), "ascii fixture: OT name sets");
    CHECK(gb_set_nickname(&m, "SPARKY"), "ascii fixture: nickname sets");
    int rc = bc_pack(&m, 0, 0, 0, 970u, cell);
    CHECK(rc == 0, "ascii fixture: bc_pack succeeds");
    if (rc != 0) return;

    int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
    GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
    uint16_t from4[4]; uint8_t bad4[4]; int nbad;   /* BACKLOG #212 */
    bdc_convert_gb_core(cell, GB_GEN1, false, &base, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                        &notes, from4, bad4, &nbad);
    CHECK(tc == 0 && g12 == GB12_OK && g3gb == G3GB_OK,
          "ascii fixture: 2->1 bridge converts (tc=%d g12=%s g3gb=%s)",
          tc, gen12_reason_text(g12), g3gb_status_text(g3gb));
    CHECK(!notes.otname_lossy, "BACKLOG #177: an ASCII-only OT name never sets otname_lossy");
    CHECK(!notes.nick_lossy,   "BACKLOG #177: an ASCII-only nickname never sets nick_lossy");
  }

  /* (b) nickname case: plant the "MN" ligature (0xE2) directly via gb_set_nickname_raw
   * -- the byte the standard ASCII setter could never produce (typing "MN" encodes as
   * two ordinary letters, 0x82/0x8D, never the ligature) but a real GS/Crystal save can
   * genuinely hold (the games' own "PKMN" abbreviation glyphs). g2_glyph has no case
   * for 0xE2 either (the same gap as 0xE1's "PK" pair), so this is the nickname twin of
   * the pinned Crystal.sav OT case above. */
  {
    uint8_t cell[80];
    GbEditMon m; memset(&m, 0, sizeof m);
    m.gen = GB_GEN2;
    gb_set_species(&m, 25, NULL);
    gb_set_level(&m, 20);
    gb_set_dv(&m, GB_ATK, 12); gb_set_dv(&m, GB_DEF, 12);
    gb_set_dv(&m, GB_SPE, 12); gb_set_dv(&m, GB_SPC, 12);
    gb_set_move(&m, 0, 33);
    gb_set_otid(&m, 12345);
    CHECK(gb_set_otname(&m, "GUY"), "nick fixture: OT name sets");
    uint8_t nick_raw[GB_NAME_BYTES] = { 0xE2, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50 };
    gb_set_nickname_raw(&m, nick_raw);
    int rc = bc_pack(&m, 0, 0, 0, 971u, cell);
    CHECK(rc == 0, "nick fixture: bc_pack succeeds");
    if (rc != 0) return;

    GbEditMon back; BcMeta bmeta;
    CHECK(bc_unpack(cell, &back, &bmeta) && back.nick[0] == 0xE2,
          "nick fixture precondition: the raw 0xE2 byte actually landed in the cell");

    int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
    GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
    uint16_t from4[4]; uint8_t bad4[4]; int nbad;   /* BACKLOG #212 */
    bdc_convert_gb_core(cell, GB_GEN1, false, &base, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                        &notes, from4, bad4, &nbad);
    CHECK(tc == 0 && g12 == GB12_OK && g3gb == G3GB_OK,
          "nick fixture: 2->1 bridge converts (tc=%d g12=%s g3gb=%s)",
          tc, gen12_reason_text(g12), g3gb_status_text(g3gb));
    CHECK(notes.nick_lossy,
          "BACKLOG #177: notes.nick_lossy fires for a nickname whose raw byte 0xE2 "
          "('MN' ligature) g2_decode_text has no case for");
    CHECK(!notes.otname_lossy, "BACKLOG #177: the OT name (plain ASCII 'GUY') stays clean");
  }

  /* (c) review F1's own example: a nickname holding the male gender sign (raw byte
   * 0xEF, gb_char_decode's shared case spells it U+2642 in EITHER generation).
   *
   * BACKLOG #183 (source/gen3_mon.c's decode_name) changed this case's expected
   * outcome from lossy to CLEAN: before #183, the Gen-3 intermediate's own PkMon
   * decode had no case for 0xB5/0xB6 (gen3_decode_char's single-`char` return can't
   * carry a 3-byte UTF-8 glyph) and fell to '?', so gen3_to_gb wrote a literal '?'
   * into the Gen-1 record -- a real, silent spelling change that gb_name_changed
   * correctly caught as lossy. #183 gave decode_name its own gender-sign cases (the
   * same UTF-8 spelling gb_edit.c/gen1_save.c/gen2_save.c already use for 0xEF/0xF5),
   * so the Gen-3 intermediate now carries a real U+2642, gen3_to_gb's gb_set_nickname
   * writes Gen 1's own 0xEF byte for it (gb_edit.c:676), and gb_get_nickname reads it
   * back as U+2642 again -- source and written spelling now genuinely match, because
   * Gen 1 always could store this glyph; only the Gen-3 hop was silently corrupting
   * it. Confirmed by mutating decode_name back to the pre-#183 cases (dropping the
   * 0xB5/0xB6 special-case so it falls through to gen3_decode_char's '?'): this CHECK
   * flips (nick_lossy becomes false -> true), proving the assertion below is
   * exercising the real fixed path, not a vacuous one. */
  {
    uint8_t cell[80];
    GbEditMon m; memset(&m, 0, sizeof m);
    m.gen = GB_GEN2;
    gb_set_species(&m, 25, NULL);
    gb_set_level(&m, 20);
    gb_set_dv(&m, GB_ATK, 12); gb_set_dv(&m, GB_DEF, 12);
    gb_set_dv(&m, GB_SPE, 12); gb_set_dv(&m, GB_SPC, 12);
    gb_set_move(&m, 0, 33);
    gb_set_otid(&m, 12345);
    CHECK(gb_set_otname(&m, "GUY"), "gender fixture: OT name sets");
    CHECK(gb_set_nickname(&m, "PIKA\xE2\x99\x82"), "gender fixture: nickname sets (PIKA-male sign)");

    int rc = bc_pack(&m, 0, 0, 0, 972u, cell);
    CHECK(rc == 0, "gender fixture: bc_pack succeeds");
    if (rc != 0) return;

    int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
    GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
    uint16_t from4[4]; uint8_t bad4[4]; int nbad;   /* BACKLOG #212 */
    bdc_convert_gb_core(cell, GB_GEN1, false, &base, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                        &notes, from4, bad4, &nbad);
    CHECK(tc == 0 && g12 == GB12_OK && g3gb == G3GB_OK,
          "gender fixture: 2->1 bridge converts (tc=%d g12=%s g3gb=%s)",
          tc, gen12_reason_text(g12), g3gb_status_text(g3gb));
    CHECK(!notes.nick_lossy,
          "BACKLOG #183: notes.nick_lossy stays clean for a nickname carrying the male "
          "gender sign crossing the 2->1 bridge -- Gen 1 genuinely stores 0xEF, so once "
          "the Gen-3 intermediate decodes 0xB5 correctly the round trip is lossless");
  }

  /* (d) BACKLOG #216b: a Gen-2 nickname carrying an umlaut (Ü, U+00DC) bridged DOWN to
   * Gen 1 -- Gen 1 has no umlaut code point at all (pokered's charmap.asm has none),
   * so this must raise nick_lossy, the same shape as (b)'s "MN" ligature case above,
   * not silently transliterate or drop the glyph. */
  {
    uint8_t cell[80];
    GbEditMon m; memset(&m, 0, sizeof m);
    m.gen = GB_GEN2;
    gb_set_species(&m, 25, NULL);
    gb_set_level(&m, 20);
    gb_set_dv(&m, GB_ATK, 12); gb_set_dv(&m, GB_DEF, 12);
    gb_set_dv(&m, GB_SPE, 12); gb_set_dv(&m, GB_SPC, 12);
    gb_set_move(&m, 0, 33);
    gb_set_otid(&m, 12345);
    CHECK(gb_set_otname(&m, "GUY"), "umlaut fixture: OT name sets");
    CHECK(gb_set_nickname(&m, "M\xC3\x9CLLER"), "umlaut fixture: nickname (M\xC3\x9CLLER) sets");
    int rc = bc_pack(&m, 0, 0, 0, 972u, cell);
    CHECK(rc == 0, "umlaut fixture: bc_pack succeeds");
    if (rc != 0) return;

    int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
    GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
    uint16_t from4[4]; uint8_t bad4[4]; int nbad;
    bdc_convert_gb_core(cell, GB_GEN1, false, &base, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                        &notes, from4, bad4, &nbad);
    CHECK(tc == 0 && g12 == GB12_OK && g3gb == G3GB_OK,
          "umlaut fixture: 2->1 bridge converts (tc=%d g12=%s g3gb=%s)",
          tc, gen12_reason_text(g12), g3gb_status_text(g3gb));
    CHECK(notes.nick_lossy,
          "BACKLOG #216b: notes.nick_lossy fires for a nickname carrying U-umlaut "
          "crossing the 2->1 bridge -- Gen 1 has no umlaut code point at all");
    CHECK(!notes.otname_lossy, "BACKLOG #216b: the OT name ('GUY', plain ASCII) stays clean");
  }
}

/* ============================================================================ */
/* E2. BACKLOG #216b: a Gen-2 "CAF\xC3\xA9" / "M\xC3\x9CLLER" bridged UP to Gen 3   */
/* (through bc_view()/gen12_convert(), the SAME hop test_roundtrip_2_3_2 exercises) */
/* and back DOWN to a fresh Gen-2 record (through gb_set_nickname/gb_set_otname,   */
/* gb_edit.c's own canonical, unchanged encoder) must be byte-identical, both ways. */
/* ============================================================================ */
static void test_cafe_umlaut_bridge_2_3_2(void) {
  uint8_t cell[80];
  GbEditMon m; memset(&m, 0, sizeof m);
  m.gen = GB_GEN2;
  gb_set_species(&m, 25, NULL);
  gb_set_level(&m, 20);
  gb_set_dv(&m, GB_ATK, 12); gb_set_dv(&m, GB_DEF, 12);
  gb_set_dv(&m, GB_SPE, 12); gb_set_dv(&m, GB_SPC, 12);
  gb_set_move(&m, 0, 33);
  gb_set_otid(&m, 12345);
  CHECK(gb_set_otname(&m, "M\xC3\x9CLLER"), "cafe/muller fixture: OT name (M\xC3\x9CLLER) sets");
  CHECK(gb_set_nickname(&m, "CAF\xC3\xA9"), "cafe/muller fixture: nickname (CAF\xC3\xA9) sets");
  int rc = bc_pack(&m, 0, 0, 0, 973u, cell);
  CHECK(rc == 0, "cafe/muller fixture: bc_pack succeeds");
  if (rc != 0) return;

  GbEditMon orig; BcMeta origmeta;
  CHECK(bc_unpack(cell, &orig, &origmeta), "cafe/muller fixture: cell unpacks");

  /* UP: bc_view() (gen2_save.c's g2_decode_text -- BACKLOG #216b's decoder fix) then
   * gen12_convert() (gen3_edit.c's encode_name -- BACKLOG #216b's encoder fix). */
  uint8_t out80[80]; GbEditMon written; Gb12Notes notes; uint16_t g3item;
  Gb12Result r = bdc_convert_gen3_core(cell, 3, out80, &written, &notes, &g3item);
  CHECK(r == GB12_OK, "cafe/muller fixture: 2->3 bridge converts (got %s)", gen12_reason_text(r));
  if (r != GB12_OK) return;
  CHECK(!notes.nick_lossy, "BACKLOG #216b: notes.nick_lossy is false -- CAF\xC3\xA9 round-trips into Gen 3");
  CHECK(!notes.otname_lossy, "BACKLOG #216b: notes.otname_lossy is false -- M\xC3\x9CLLER round-trips into Gen 3");

  /* Decode the Gen-3 record's own bytes (gen3_mon.c's decode_name -- BACKLOG #216b's
   * third fix) and require the EXACT same UTF-8 spelling the source cell held. */
  char g3_nick[32], g3_ot[32];
  gen3_decode_name(g3_nick, sizeof g3_nick, out80 + 0x08, 10);
  gen3_decode_name(g3_ot, sizeof g3_ot, out80 + 0x14, 7);
  CHECK(strcmp(g3_nick, "CAF\xC3\xA9") == 0,
        "BACKLOG #216b: the Gen-3 nickname decodes to CAF\xC3\xA9 exactly (got %s)", g3_nick);
  CHECK(strcmp(g3_ot, "M\xC3\x9CLLER") == 0,
        "BACKLOG #216b: the Gen-3 OT name decodes to M\xC3\x9CLLER exactly (got %s)", g3_ot);

  /* DOWN: re-encode that SAME decoded text into a fresh Gen-2 record through
   * gb_edit.c's gb_set_nickname/gb_set_otname (the canonical, unchanged encoder this
   * lane did not touch) and require the raw bytes to equal the ORIGINAL cell's raw
   * bytes -- "bridged to Gen 3 and back must be byte-identical". */
  GbEditMon back; memset(&back, 0, sizeof back);
  back.gen = GB_GEN2;
  CHECK(gb_set_nickname(&back, g3_nick), "cafe/muller fixture: the decoded nickname re-encodes into Gen 2");
  CHECK(gb_set_otname(&back, g3_ot), "cafe/muller fixture: the decoded OT name re-encodes into Gen 2");
  CHECK(memcmp(back.nick, orig.nick, GB_NAME_BYTES) == 0,
        "BACKLOG #216b: the nickname's raw GB bytes are byte-identical after the 2->3->2 round trip");
  CHECK(memcmp(back.otname, orig.otname, GB_NAME_BYTES) == 0,
        "BACKLOG #216b: the OT name's raw GB bytes are byte-identical after the 2->3->2 round trip");
}

/* ============================================================================ */
/* F. the time-capsule refusal -- species still refuses whole; moves no longer   */
/* do (BACKLOG #212) -- no ledger entry is ever built either way.                */
/* ============================================================================ */
static void test_time_capsule_refusal(void) {
  uint8_t cell[80];
  build_gen2_cell(cell, 152 /* Chikorita, > Gen-1's 151 */, 0, 33, 900);
  int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
  GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
  uint16_t from4[4]; uint8_t bad4[4]; int nbad;
  bdc_convert_gb_core(cell, GB_GEN1, false, NULL, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                      &notes, from4, bad4, &nbad);
  /* bdc_convert_gb_core returns *tc != 0 (checked FIRST, before gen12_convert or
   * gen3_to_gb_fixed ever run -- both g12/g3gb are left at their initial "not
   * attempted" sentinel) -- the FIRST line that could write anything is the caller's
   * ledger write in bank_down_convert.c's GBA-facing arm, never reached here at all.
   * The species-floor check (BACKLOG #150 S150-8 decision 14) is UNCHANGED by
   * BACKLOG #212 -- a species Gen 1 cannot represent at all has no per-slot fix. */
  CHECK(tc == 1, "dex 152 refuses before conversion (tc=%d)", tc);
  CHECK(tc_bad == 152, "tc_bad names the species");
  CHECK(g12 == GB12_ERR_EMPTY, "g12 stays at its initial sentinel (never attempted)");

  /* BACKLOG #212: a lone bad move no longer refuses via `tc` at all -- g3gb_moves_ok()
   * flags it (bad4[0]=1, nbad=1) and xr_time_capsule_block is called with moves4==NULL
   * (species-floor only), so `tc` stays 0 and conversion proceeds. With g1base == NULL
   * (this test's own precondition, matching the ORIGINAL "before conversion" case
   * above), the Gen-1 base-stats gate still fires (screen()'s own check ORDER: move
   * screening happens BEFORE the g1base check, so a caller-flagged bad move never
   * masks the separate base-stats requirement) -- G3GB_ERR_NEEDS_BASE, exactly the
   * same "needs a located ROM" outcome gb_bank_down_bridge's own retry handles. */
  uint8_t cell2[80];
  build_gen2_cell(cell2, 25, 0, 250 /* > gb_max_move(GB_GEN1)=165 */, 901);
  bdc_convert_gb_core(cell2, GB_GEN1, false, NULL, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                      &notes, from4, bad4, &nbad);
  CHECK(tc == 0, "move 250 no longer refuses via tc (tc=%d)", tc);
  CHECK(g12 == GB12_OK, "move 250: intermediate conversion still ok (%s)", gen12_reason_text(g12));
  CHECK(nbad == 1 && bad4[0] == 1 && bad4[1] == 0 && bad4[2] == 0 && bad4[3] == 0,
        "move 250: bad4 flags exactly slot 0 (nbad=%d bad4=%u,%u,%u,%u)",
        nbad, bad4[0], bad4[1], bad4[2], bad4[3]);
  CHECK(from4[0] == 250, "move 250: from4[0] carries the raw source move id (got %u)", from4[0]);
  CHECK(g3gb == G3GB_ERR_NEEDS_BASE,
        "move 250, no g1base: the base-stats gate still fires (got %s)", g3gb_status_text(g3gb));

  /* Supplying a base (the retry gb_bank_down_bridge itself performs) now SUCCEEDS --
   * the whole point of BACKLOG #212: `out`'s slot 0 is EMPTY (this pure core only
   * clips, per its own header comment; the caller fills it), the other three slots
   * (all 0 in build_gen2_cell's fixture -- see its own single gb_set_move(&m, 0, ...)
   * call) stay untouched. */
  GbGen1Base base = { .base = { 35, 55, 40, 90, 50 }, .type1 = 0x18, .type2 = 0x18 };
  bdc_convert_gb_core(cell2, GB_GEN1, false, &base, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                      &notes, from4, bad4, &nbad);
  CHECK(tc == 0 && g12 == GB12_OK, "move 250 with a base: still converts to the intermediate");
  CHECK(g3gb == G3GB_OK, "move 250 with a base: gen3_to_gb_fixed accepts (got %s)",
        g3gb_status_text(g3gb));
  CHECK(gb_get_move(&out, 0) == 0,
        "move 250 with a base: bdc_convert_gb_core's own `out` has slot 0 CLIPPED "
        "(empty) rather than the whole record refused -- G-H8, this pure core does "
        "not fill (got %u)", gb_get_move(&out, 0));
}

/* ============================================================================ */
/* F2. BACKLOG #212 (review D4: ALL boxes, both corpus saves -- was box 0 of      */
/* Crystal.sav only): every box's record with a species Gen 1 can represent,     */
/* bridged with a real base -- "one bucket" proof: every record either lands     */
/* with >= 1 move (empty or filled -- this pure core only clips, so a bad slot   */
/* is 0 here) or the base-stats/species/glitch/egg refusal that predates this    */
/* fix; G3GB_ERR_MOVE is NEVER reachable through this call site again (the whole */
/* point of the fix -- confirmed over real data, not just the two synthetic      */
/* fixtures above). g2_list_offset()'s own `box` argument (looped 0..           */
/* G2_NUM_BOXES-1 here) is the box actually being read; its separate            */
/* `current_box` argument (hd.current_box, constant across the loop) only tells  */
/* it which ONE of those boxes is stored at the save's "current box" SRAM        */
/* location instead of the uniform per-box table -- it is not itself a box       */
/* index, so the old single-box call's "box-0" framing named the wrong one of    */
/* its two int arguments. Also review D1/D4: every bad slot filled with NO ROM   */
/* (an empty learn table, same as --op paste80 with no --rom) must never leave a */
/* record with zero moves total -- if the caller-level `nleft == 0` predicate    */
/* would ever fire on real corpus data, that is this fix's own bug reappearing.  */
/* ============================================================================ */
static void test_bridge_corpus_no_move_refusal(void) {
  static const char* k_files[] = { "Crystal.sav", "Gold.sav" };
  GbGen1Base base = { .base = { 35, 55, 40, 90, 50 }, .type1 = 0x18, .type2 = 0x18 };
  int checked = 0, with_bad = 0, zero_move = 0, files_loaded = 0;

  for (size_t fi = 0; fi < sizeof k_files / sizeof k_files[0]; fi++) {
    size_t len;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", GB_ROMS, k_files[fi]);
    if (!load_file(path, g_gen2_img, sizeof g_gen2_img, &len)) { printf("  SKIP F2 (no %s)\n", path); continue; }
    G2Save sv;
    if (!g2_detect(g_gen2_img, (uint32_t)len, &sv) || !sv.supported) { printf("  SKIP F2 (%s detect failed)\n", k_files[fi]); continue; }
    G2Header hd;
    if (!g2_read_header(g_gen2_img, &sv, &hd)) { printf("  SKIP F2 (%s header)\n", k_files[fi]); continue; }
    files_loaded++;

    for (int box = 0; box < G2_NUM_BOXES; box++) {
      uint32_t off = g2_list_offset(&sv, box, hd.current_box);
      if (off == 0) continue;
      int n = gb_list_count(GB_GEN2, g_gen2_img + off, 0);
      if (n <= 0) continue;

      for (int slot = 0; slot < n; slot++) {
        GbEditMon mon;
        if (!gb_load(&mon, GB_GEN2, g_gen2_img + off, 0, slot)) continue;
        if (mon.list_species == G2_LIST_EGG) continue;
        uint16_t dex = gb_get_species_dex(&mon);
        if (dex < 1 || dex > gb_max_species(GB_GEN1)) continue;   /* the species-floor bucket -- tested separately above */

        uint8_t cell[80];
        uint32_t serial = 3000u + (uint32_t)fi * 10000u + (uint32_t)box * 100u + (uint32_t)slot;
        if (bc_pack(&mon, 0, BC_ORIGIN_CRYSTAL, 0, serial, cell) != 0) continue;

        int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
        GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
        uint16_t from4[4]; uint8_t bad4[4]; int nbad;
        bdc_convert_gb_core(cell, GB_GEN1, false, &base, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                            &notes, from4, bad4, &nbad);
        checked++;
        if (tc != 0 || g12 != GB12_OK) continue;   /* a different bucket (species/glitch), unrelated to this fix */

        CHECK(g3gb != G3GB_ERR_MOVE,
              "%s box %d slot %d: G3GB_ERR_MOVE must be UNREACHABLE through this call "
              "site (BACKLOG #212's whole point) -- got it anyway", k_files[fi], box, slot);
        if (g3gb != G3GB_OK) continue;   /* GLITCH/EGG/ARG -- not this fix's edge either */

        if (nbad > 0) {
          with_bad++;
          for (int i = 0; i < 4; i++) {
            if (bad4[i]) {
              CHECK(gb_get_move(&out, i) == 0,
                    "%s box %d slot %d: bad4[%d] flagged but out's move[%d] is %u, not "
                    "clipped to empty", k_files[fi], box, slot, i, i, gb_get_move(&out, i));
            } else if (from4[i] != 0) {
              CHECK(gb_get_move(&out, i) == (uint8_t)from4[i],
                    "%s box %d slot %d: a KEPT slot's move changed under conversion "
                    "(from4[%d]=%u out=%u)", k_files[fi], box, slot, i, from4[i], gb_get_move(&out, i));
            }
          }

          /* review D1/D4: no ROM (an empty learn table) -- the caller-level fill
           * (mirroring gb_bank_down_bridge/gb_paste_hook/do_paste80) must never
           * silently leave a real corpus record with zero moves total. */
          uint8_t learn4[4] = { 0, 0, 0, 0 };
          uint8_t fill4[4] = { 0, 0, 0, 0 };
          (void)g3gb_moves_fill(&out, bad4, learn4, fill4);
          int nleft = 0;
          for (int i = 0; i < 4; i++) if (gb_get_move(&out, i)) nleft++;
          if (nleft == 0) zero_move++;
        }
      }
    }
  }
  CHECK(files_loaded > 0, "F2: at least one of Crystal.sav/Gold.sav was found under %s", GB_ROMS);
  CHECK(checked > 0, "F2: at least one real corpus box record was checked (got %d)", checked);
  CHECK(zero_move == 0, "F2: %d real corpus record(s) would land with zero moves under "
        "the no-ROM fill -- the caller-level refusal exists precisely to catch this "
        "(got %d, want 0)", zero_move, zero_move);
  printf("  F2: %d record(s) checked across %d file(s) x %d boxes, %d had >= 1 bad slot "
         "(per-slot clip proven, no whole-record move refusal, %d zero-move)\n",
         checked, files_loaded, G2_NUM_BOXES, with_bad, zero_move);
}

/* ============================================================================ */
/* F2b. review D1 pin: a cell whose only non-empty moves are BOTH out of range for  */
/* the destination generation (no learn table) -- 2 of 4 slots bad, not 4 of 4. The */
/* OLD predicate `nbad == 4 && nfill == 0` is FALSE here (nbad == 2) even though the */
/* record would land with zero moves left -- the exact case review D1 found reachable*/
/* through --op paste80 in production. Proves the caller-level `nleft == 0` predicate*/
/* (source/pdna_gen12.c's gb_paste_hook / gb_bank_down_bridge, tests/host_gbsurgery_  */
/* tool.c's do_paste80) is the one that must fire, not the old nbad==4 shortcut. */
/* ============================================================================ */
static void test_caller_zero_move_refusal_two_bad(void) {
  GbEditMon m; memset(&m, 0, sizeof m);
  m.gen = GB_GEN2;
  gb_set_species(&m, 25, NULL);            /* Pikachu -- Gen 1 can represent it */
  gb_set_level(&m, 20);
  gb_set_dv(&m, GB_ATK, 12); gb_set_dv(&m, GB_DEF, 12);
  gb_set_dv(&m, GB_SPE, 12); gb_set_dv(&m, GB_SPC, 12);
  gb_set_move(&m, 0, 200);                 /* > 165, out of Gen-1 range */
  gb_set_move(&m, 1, 230);                 /* > 165, out of Gen-1 range */
  gb_set_otid(&m, 12345);
  uint8_t cell[80];
  int rc = bc_pack(&m, 0, 0, 0, 9001u, cell);
  CHECK(rc == 0, "F2b: bc_pack builds the two-bad-move fixture (rc=%d)", rc);

  GbGen1Base base = { .base = { 35, 55, 40, 90, 50 }, .type1 = 0x18, .type2 = 0x18 };
  int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
  GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
  uint16_t from4[4]; uint8_t bad4[4]; int nbad;
  bdc_convert_gb_core(cell, GB_GEN1, false, &base, &tc, &tc_bad, &g12, &g3gb, &out, &loss,
                      &notes, from4, bad4, &nbad);
  CHECK(tc == 0 && g12 == GB12_OK, "F2b: converts to the intermediate");
  CHECK(g3gb == G3GB_OK, "F2b: gen3_to_gb_fixed accepts (got %s)", g3gb_status_text(g3gb));
  CHECK(nbad == 2, "F2b: exactly 2 of 4 slots flagged bad (got %d)", nbad);

  /* No ROM (the "--rom absent" equivalent) -- the caller-level fill (mirroring
   * pdna_gen12.c's gb_bank_down_bridge/gb_paste_hook) gets an all-empty learn
   * table, so nfill stays 0. */
  uint8_t learn4[4] = { 0, 0, 0, 0 };
  uint8_t fill4[4] = { 0, 0, 0, 0 };
  int nfill = g3gb_moves_fill(&out, bad4, learn4, fill4);
  CHECK(nfill == 0, "F2b: no learn table -> nothing filled (got %d)", nfill);

  int nleft = 0;
  for (int i = 0; i < 4; i++) if (gb_get_move(&out, i)) nleft++;
  CHECK(nleft == 0, "F2b: the record would be WRITTEN with zero moves left (got %d) -- "
        "the caller-level refusal (review D1's `nleft == 0`) must fire here", nleft);
  CHECK(!(nbad == 4 && nfill == 0), "F2b: demonstrates the review D1 bug -- the OLD "
        "predicate `nbad == 4 && nfill == 0` is FALSE here (nbad=%d) even though the "
        "record has zero moves left; only `nleft == 0` catches this case", nbad);
}

/* ============================================================================ */
/* G. F2 (review): app_xfer_pending_undo()/app_xfer_promote()'s identity re-check   */
/* -- gbsc_remove(idx) alone is a blind index into a file that may have changed    */
/* under us (a GB paste to the same key, gbsc_evict_oldest, a re-mount); this      */
/* proves the SAME predicate (kind==NATIVE_HOME && direction==ABROAD_G3 &&         */
/* state==PENDING) both pdna_main.c functions gate on tells a genuine match apart  */
/* from a stale index -- app_xfer_pending_undo itself is FatFs/GBA-only and not    */
/* host-testable, so this exercises its exact identity predicate against the same */
/* gb_sidecar.c primitives it calls. */
/* ============================================================================ */
static bool xfer_identity_ok(const uint8_t* buf, uint32_t len, int idx) {
  GbscEntry e;
  if (!gbsc_get(buf, len, idx, &e)) return false;
  return e.kind == XR_KIND_NATIVE_HOME && e.direction == XR_DIR_ABROAD_G3 &&
         e.state == XR_STATE_PENDING;
}

/* BACKLOG #174/#175 review D6 (2026-09-23): test_party_flavour_identity() and
 * test_defer_delete_identity() (BACKLOG #174 D10(1)/D10(3)) DELETED -- both were
 * tautologies, not evidence (STANDING-RULES self-audit point 1: "a proof that
 * cannot fail is decoration"). test_party_flavour_identity() called
 * bdc_convert_gen3_core() TWICE with IDENTICAL arguments and asserted the two
 * results matched -- that is memcmp(x, x), true by construction, and reverting
 * D3's src_id80 fix (BACKLOG #174 D3, the defer-delete identity bytes) still gave
 * 150 passed / 0 failed on this whole suite, proving it blind. test_defer_delete_
 * identity() asserted memcmp(a, copy_of_a) == 0 and memcmp(a, unrelated_const) !=
 * 0 against a local scratch buffer -- it never called pdna_bank.c's own flush code
 * at all (pdna_bank.c is not host-compilable, out of this lane's scope), so it was
 * testing memcmp's own behaviour, not this codebase's. tools/dgb_shots.py's
 * run_s150_8_party_vsd() (review D5's fix pass) now carries the real pin instead:
 * its post-save vsd_report(expect_changed=[... "/PokeDNA/bank/box00.box" ...])
 * mutation-fails (RED, exit 1) when src_id80 is reverted to NULL, because
 * pdna_bank_flush_deletions() then never finds a matching Bank slot to delete and
 * box00.box never appears in the diff -- an assertion against the REAL flush code,
 * not a restatement of memcmp. */

static void test_pending_identity_check(void) {
  uint8_t cell[80];
  build_gen2_cell(cell, 25, 0, 33, 700);
  uint8_t out80[80]; GbEditMon written; Gb12Notes notes; uint16_t g3item;
  Gb12Result r = bdc_convert_gen3_core(cell, 3, out80, &written, &notes, &g3item);
  CHECK(r == GB12_OK, "identity-check fixture converts");

  GbscEntry e;
  gbsc_entry_from(&e, &written, cell, 0);
  e.kind = XR_KIND_NATIVE_HOME; e.state = XR_STATE_PENDING; e.direction = XR_DIR_ABROAD_G3; e.claimed = 1;

  uint8_t buf[GBSC_FILE_MAX]; uint32_t len = (uint32_t)gbsc_init(buf, 0xBBu);
  int idx = gbsc_add(buf, &len, GBSC_FILE_MAX, &e);
  CHECK(idx == 0, "identity-check fixture: gbsc_add succeeds");

  CHECK(xfer_identity_ok(buf, len, idx), "matching PENDING/NATIVE_HOME/ABROAD_G3 entry: identity check passes");

  /* Simulate a promotion racing ahead of us (e.g. app_xfer_promote already ran, or
   * gb_bank_down_bridge's own xfer_down_claim_now touched this same index by
   * coincidence): flip state to CLAIMED in place -- the undo path must now refuse
   * to touch it rather than deleting a live, already-promoted (or otherwise
   * foreign) entry. */
  GbscEntry stale;
  CHECK(gbsc_get(buf, len, idx, &stale), "re-read for the CLAIMED mutation");
  stale.state = XR_STATE_CLAIMED;
  uint8_t buf2[GBSC_FILE_MAX]; uint32_t len2 = len;
  memcpy(buf2, buf, len);
  CHECK(gbsc_remove(buf2, &len2, idx) == 0, "mutation fixture: remove the PENDING copy");
  int idx2 = gbsc_add(buf2, &len2, GBSC_FILE_MAX, &stale);
  CHECK(idx2 == idx, "mutation fixture: the CLAIMED entry lands back at the same index");
  CHECK(!xfer_identity_ok(buf2, len2, idx2),
        "a since-CLAIMED entry at the SAME index fails the identity check (undo must leave it alone)");

  /* A wrong KIND (e.g. an ordinary Gen-3-home entry from an unrelated GB paste
   * landing at the same index after an evict/re-add) must also fail. */
  GbscEntry wrongkind = e;
  wrongkind.kind = XR_KIND_G3_HOME;
  uint8_t buf3[GBSC_FILE_MAX]; uint32_t len3 = (uint32_t)gbsc_init(buf3, 0xCCu);
  int idx3 = gbsc_add(buf3, &len3, GBSC_FILE_MAX, &wrongkind);
  CHECK(idx3 == 0, "wrong-kind fixture: gbsc_add succeeds");
  CHECK(!xfer_identity_ok(buf3, len3, idx3), "a wrong-KIND entry at the same index fails the identity check");

  /* An out-of-range index (the file shrank under us, e.g. gbsc_remove of an
   * unrelated earlier entry) must fail via gbsc_get() itself, not crash. */
  CHECK(!xfer_identity_ok(buf, len, idx + 5), "an out-of-range index fails the identity check (no crash)");
}

int main(void) {
  test_mask();               printf("  (A) xr_game_item_mask        ok\n");
  test_item_edge();          printf("  (B) item-map edge            ok\n");
  test_time_capsule();       printf("  (C) xr_time_capsule_block    ok\n");
  test_roundtrip_2_3_2();    printf("  (D) round trip 2->3->2       ok\n");
  test_nick_written_nonascii_glyph(); printf("  (D2) nick_written, non-ASCII glyph ok\n");
  test_bridge_roundtrips();  printf("  (E) round trip 1<->2 bridge  ok\n");
  test_bridge_real_corpus_2_to_1(); printf("  (E2) real 2->1 corpus bridge ok\n");
  test_name_glyph_loss_synthetic(); printf("  (E3) BACKLOG #177 name-glyph loss ok\n");
  test_cafe_umlaut_bridge_2_3_2(); printf("  (E2b) BACKLOG #216b cafe/muller 2->3->2 ok\n");
  test_time_capsule_refusal();printf("  (F) time-capsule refusal     ok\n");
  test_bridge_corpus_no_move_refusal(); printf("  (F2) BACKLOG #212 corpus, no whole-record move refusal ok\n");
  test_caller_zero_move_refusal_two_bad(); printf("  (F2b) review D1, 2-bad-move zero-move refusal ok\n");
  test_pending_identity_check(); printf("  (G) pending identity check   ok\n");
  /* review D6: test_party_flavour_identity/test_defer_delete_identity DELETED
   * (tautologies) -- see this file's own comment above (H)/(I)'s old spot. */

  printf("%d checks, %d failed\n", g_check, g_fail);
  if (g_fail) { printf("FAILED\n"); return 1; }
  printf("all host_xferdown_test checks passed\n");
  return 0;
}
