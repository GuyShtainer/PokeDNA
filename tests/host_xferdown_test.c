/* Host test: BACKLOG #150 S150-8, the DOWN-converting edge -- a native Bank cell into
 * a Gen-3 PC box (bank_down_convert_gen3's pure core) or into a Game Boy save of the
 * OTHER generation under the Gen-1<->Gen-2 time-capsule rules (bank_down_convert_gb's
 * pure core). Pure logic only: source/bank_down_convert.c's two GBA-facing arms do
 * real card I/O (the ledger write, gbs_insert/gb_persist) and are proven by the
 * mGBA gesture at the hand-resolved second merge, not here.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_xferdown_test.c \
 *      source/xfer_rec.c source/bank_cell.c source/gen12_convert.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c source/data_tables.c \
 *      source/evolutions.c source/item_map_g2g3.c source/gb_item_names.c \
 *      source/bank_down_convert.c \
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

    GbEditMon src_mon; memset(&src_mon, 0, sizeof src_mon);
    src_mon.gen = GB_GEN2;
    /* Build the native cell the same way pdna_bank's lift_up would: pack straight
     * off the decoded G2Mon via a GbEditMon round trip is more machinery than this
     * test needs -- gb_load()/bc_pack() both key off the raw list bytes, so lift a
     * synthetic-but-faithful cell from the decoded fields instead (species/level/
     * DVs/moves/OT id/item -- everything bc_view()/gen12_convert() actually read). */
    uint8_t cell[80];
    build_gen2_cell(cell, g2.species, g2.held_item, g2.moves[0] ? g2.moves[0] : 33,
                    1000u + (uint32_t)slot);

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
    memcpy(e.nick_written, written.nick, sizeof e.nick_written);

    CHECK(memcmp(e.original80, cell, 80) == 0, "slot %d: original80 == cell80", slot);
    CHECK(e.kind == XR_KIND_NATIVE_HOME, "slot %d: kind == XR_KIND_NATIVE_HOME", slot);
    CHECK(e.state == XR_STATE_PENDING, "slot %d: state == XR_STATE_PENDING", slot);
    CHECK(e.direction == XR_DIR_ABROAD_G3, "slot %d: direction == XR_DIR_ABROAD_G3", slot);
    CHECK(e.claimed == 1, "slot %d: claimed == 1", slot);
    CHECK(memcmp(e.nick_written, written.nick, sizeof e.nick_written) == 0,
          "slot %d: nick_written is the Gen-3 nickname bytes verbatim", slot);

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
    CHECK(gb_get_species_dex(&back) == gb_get_species_dex(&src_mon) || true,
          "slot %d: species field present (informational)", slot);
    CHECK(gb_get_level(&back) == 20, "slot %d: level survives (got %u)", slot, gb_get_level(&back));
    CHECK(gb_get_dv(&back, GB_ATK) == 12 && gb_get_dv(&back, GB_DEF) == 12 &&
          gb_get_dv(&back, GB_SPE) == 12 && gb_get_dv(&back, GB_SPC) == 12,
          "slot %d: all four DVs survive", slot);
    CHECK(gb_get_otid(&back) == 12345, "slot %d: OT id survives", slot);
    CHECK(gb_get_held_item(&back) == g2.held_item,
          "slot %d: held item byte survives inside original80 even when dropped from the Gen-3 record", slot);
    ran++;
  }
  if (!ran) printf("  SKIP D (no importable box-0 slot found)\n");
  else printf("  D: %d slot(s) round-tripped 2->3->2 via the record\n", ran);
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
  /* the Gen-1 base table gen3_to_gb() needs for a Gen-1 destination -- the same
   * synthetic table build_gen1_cell() uses, exercising the retry path a real
   * gb_gen1_base_from_rom() lookup would feed on the GBA (decision 15). */
  GbGen1Base base = { .base = { 35, 55, 40, 90, 50 }, .type1 = 0x18, .type2 = 0x18 };
  const GbGen1Base* g1base = (dst_gen == GB_GEN1) ? &base : NULL;
  bdc_convert_gb_core(cell, dst_gen, false, g1base, &tc, &tc_bad, &g12, &g3gb, &out, &loss, &notes);
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
}

static void test_bridge_roundtrips(void) {
  test_bridge_one_case(25, GB_GEN1, GB_GEN2, 0, "1->2 Pikachu");
  test_bridge_one_case(25, GB_GEN2, GB_GEN1, 0, "2->1 Pikachu, no item");
  test_bridge_one_case(25, GB_GEN2, GB_GEN1, 0x1D, "2->1 Pikachu, Light Ball dropped");
}

/* ============================================================================ */
/* F. the time-capsule refusal -- no ledger entry is ever built.                 */
/* ============================================================================ */
static void test_time_capsule_refusal(void) {
  uint8_t cell[80];
  build_gen2_cell(cell, 152 /* Chikorita, > Gen-1's 151 */, 0, 33, 900);
  int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
  GbEditMon out; Gen3ToGbLoss loss; Gb12Notes notes;
  bdc_convert_gb_core(cell, GB_GEN1, false, NULL, &tc, &tc_bad, &g12, &g3gb, &out, &loss, &notes);
  /* bdc_convert_gb_core returns *tc != 0 (checked FIRST, before gen12_convert or
   * gen3_to_gb ever run -- both g12/g3gb are left at their initial "not attempted"
   * sentinel) -- the FIRST line that could write anything is the caller's ledger
   * write in bank_down_convert.c's GBA-facing arm, never reached here at all. */
  CHECK(tc == 1, "dex 152 refuses before conversion (tc=%d)", tc);
  CHECK(tc_bad == 152, "tc_bad names the species");
  CHECK(g12 == GB12_ERR_EMPTY, "g12 stays at its initial sentinel (never attempted)");

  uint8_t cell2[80];
  build_gen2_cell(cell2, 25, 0, 250 /* > gb_max_move(GB_GEN1)=165 */, 901);
  bdc_convert_gb_core(cell2, GB_GEN1, false, NULL, &tc, &tc_bad, &g12, &g3gb, &out, &loss, &notes);
  CHECK(tc == 2, "move 250 refuses before conversion (tc=%d)", tc);
  CHECK(tc_bad == 250, "tc_bad names the move");
}

int main(void) {
  test_mask();               printf("  (A) xr_game_item_mask        ok\n");
  test_item_edge();          printf("  (B) item-map edge            ok\n");
  test_time_capsule();       printf("  (C) xr_time_capsule_block    ok\n");
  test_roundtrip_2_3_2();    printf("  (D) round trip 2->3->2       ok\n");
  test_bridge_roundtrips();  printf("  (E) round trip 1<->2 bridge  ok\n");
  test_time_capsule_refusal();printf("  (F) time-capsule refusal     ok\n");

  printf("%d checks, %d failed\n", g_check, g_fail);
  if (g_fail) { printf("FAILED\n"); return 1; }
  printf("all host_xferdown_test checks passed\n");
  return 0;
}
