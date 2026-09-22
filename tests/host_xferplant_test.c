/* Host test: BACKLOG #150 S150-15 step 4, decision 13 -- the PDNA_DELTA-only seam
 * (source/xfer_plant.{c,h}). SEPARATE file from tests/host_xferview_test.c (see the
 * tail of this comment for why); picked up by run_host_tests.py's own glob.
 *
 *   cc -std=c11 -Wall -Wextra -I source -DPDNA_DELTA -DPDNA_GEN12_HOST \
 *      tests/host_xferplant_test.c \
 *      source/xfer_plant.c source/bank_plant.c source/gb_new_mon.c \
 *      source/gb_editor.c source/gb_session.c source/rom_gblearn.c \
 *      source/rom_gbbase.c source/rom_gbsprite.c source/gb_sprite_codec.c \
 *      source/ui_font.c \
 *      source/bank_down_convert.c source/gen12_convert.c source/gen3_to_gb.c \
 *      source/xfer_rec.c source/bank_cell.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/gb_sidecar.c source/gen3_save.c source/gen3_mon.c source/gen3_box.c \
 *      source/gen3_edit.c source/gen3_daycare.c source/data_tables.c \
 *      source/evolutions.c source/item_map_g2g3.c source/gb_item_names.c \
 *      source/gen3_clip.c \
 *      -o /tmp/hxplant && /tmp/hxplant
 *
 * Not a second `cc` line inside host_xferview_test.c: run_host_tests.py extracts
 * exactly ONE cc line (and only its first ~24 source lines) per host_*_test.c file,
 * and this test needs BOTH -DPDNA_DELTA (compile the seam's body at all) and
 * -DPDNA_GEN12_HOST (the DOWN-conversion core's own host posture,
 * host_xferdown_test.c's own precedent) together -- two preprocessor worlds sharing
 * one file's object list is exactly the #ifdef combinatorics golden rule 8 warns
 * against.
 *
 * gb_editor.c/gb_session.c/rom_gblearn.c/rom_gbbase.c/rom_gbsprite.c/
 * gb_sprite_codec.c/ui_font.c: gb_new_mon.c's own real deps (gbe_settle_stats,
 * rom_gblearn_moves_at_seeded), the exact object list tests/host_newmon_test.c's
 * own header comment documents for the same reason. source/evolutions.c is
 * GENERATED and gitignored -- this test needs no real evolution floor (the planted
 * CHIKORITA never evolves); it links only because bank_down_convert.c's TU needs
 * the symbol to exist somewhere.
 *
 * app_met_game() is stubbed to `3` (Emerald) below -- source/pdna_main.c, the real
 * definition, is not linked (needs the whole app); the shot chain's own fixture
 * (tools/fuse_sav.py <Emerald.sav>) makes the real app_met_game() return the same
 * value, which is WHY the seam's key recomputation matches on both sides
 * (decision 14). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "xfer_plant.h"
#include "bank_plant.h"
#include "bank_cell.h"
#include "gen3_clip.h"

uint8_t app_met_game(void) { return 3; }   /* Emerald -- see header comment */

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

int main(void) {
  printf("== xfer_plant_converted ==\n");
  uint8_t r[80];
  CHECK(xfer_plant_converted(3, r), "xfer_plant_converted(Emerald) succeeds");
  CHECK(pk3_validate(r), "the converted record passes pk3_validate");

  /* bank_plant_cell0() byte-equals slot 0 of bank_plant_box0() -- the pixel-parity
   * pair's own premise (decision 13: "the Bank's planted slot-0 VIEW and the pasted
   * mon's GB ORIGINAL the SAME bytes"). */
  printf("== bank_plant_cell0 == slot 0 of bank_plant_box0 ==\n");
  uint8_t cell0[80];
  bank_plant_cell0(cell0);
  static uint8_t recs[30 * 80];
  bank_plant_box0(recs);
  CHECK(memcmp(cell0, recs, 80) == 0, "bank_plant_cell0() byte-equals bank_plant_box0()'s slot 0");

  printf("== xfer_plant_entry ==\n");
  GbscEntry e;
  memset(&e, 0, sizeof e);
  CHECK(xfer_plant_entry(3, r, &e), "xfer_plant_entry(Emerald, the converted record) -> true");
  CHECK(bc_is_native(e.original80), "e.original80 passes bc_is_native()");
  CHECK(e.kind == XR_KIND_NATIVE_HOME, "e.kind == XR_KIND_NATIVE_HOME (got %u)", e.kind);
  CHECK(e.state == XR_STATE_CLAIMED, "e.state == XR_STATE_CLAIMED (got %u)", e.state);
  CHECK(e.direction == XR_DIR_ABROAD_G3, "e.direction == XR_DIR_ABROAD_G3 (got %u)", e.direction);
  CHECK(e.claimed == 1, "e.claimed == 1 (got %u)", e.claimed);
  CHECK(e.rtc_epoch == (uint32_t)((26u << 26) | (9u << 22) | (16u << 17)),
        "e.rtc_epoch == PLANT_EPOCH (2026-09-16, got 0x%08X)", e.rtc_epoch);
  CHECK(memcmp(e.original80, cell0, 80) == 0, "e.original80 byte-equals bank_plant_cell0()");

  /* a different record (an unrelated Gen-3 mon) does NOT match. */
  printf("== xfer_plant_entry: a different record refuses ==\n");
  uint8_t other[80]; memset(other, 0, sizeof other);
  other[0] = 0xAA; other[1] = 0xBB; other[2] = 0xCC; other[3] = 0xDD;
  GbscEntry e2; memset(&e2, 0, sizeof e2);
  CHECK(!xfer_plant_entry(3, other, &e2), "xfer_plant_entry on an unrelated record -> false");

  printf("\nxferplant: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
