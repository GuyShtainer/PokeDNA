/*
 * xfer_plant -- BACKLOG #150 S150-15 decision 13, finding (e): PDNA_DELTA-only
 * seam. See xfer_plant.h for the contract. No symbol from this file exists in
 * either shipped gate build (PDNA_DELTA is never defined there).
 */
#include "xfer_plant.h"

#ifdef PDNA_DELTA
#include <string.h>
#include "bank_plant.h"        /* bank_plant_cell0 -- the SAME slot-0 cell the Bank shows */
#include "bank_down_convert.h" /* bdc_convert_gen3_core                                   */
#include "gen3_clip.h"         /* pk3_validate                                            */
#include "xfer_rec.h"          /* xr_key_g3                                               */

/* 2026-09-16 00:00:00, gb_sidecar.h's own bit layout: year-2000 << 26 | month << 22 |
 * day << 17. The date this lane's shot chain was cut on. */
#define PLANT_EPOCH ((uint32_t)((26u << 26) | (9u << 22) | (16u << 17)))

/* BACKLOG #150 S150-15 review (fixture fix 1): a serial OUTSIDE every other
 * PDNA_DELTA fixture's own planted range -- bank_plant_box0() uses 1..7,
 * bank_plant_box_full() uses 6..30, S150-9's bank_plant_xfer_seed_all() uses
 * {1, 2, 26, 27} -- so this seam's own converted record's xr_key_g3() cannot
 * collide with any of theirs (reviewer-verified: serial 150 -> key
 * 7dba5240d2bd8fa2, pk3_validate 1, distinct from all four S150-9 slots). Differs
 * from bank_plant_cell0()'s own bytes only at cell offsets 4..7/73..76 (the
 * ident32/bank_serial span), which no summary card draws -- the 00-vs-05 shot-
 * chain parity (bank_plant_cell0's own slot-0 VIEW vs this seam's GB ORIGINAL)
 * still holds byte-for-byte on every DRAWN field. */
#define XFER_PLANT_SERIAL 150u /* outside box0 1..7, box_full 6..30, S150-9 {1,2,26,27} */

bool xfer_plant_converted(uint8_t met_game, uint8_t out80[80]) {
  if (!out80) return false;
  uint8_t cell[80];
  bank_plant_cell0_serial(cell, XFER_PLANT_SERIAL);
  GbEditMon written; Gb12Notes notes; uint16_t g3_item = 0;
  Gb12Result r = bdc_convert_gen3_core(cell, met_game, out80, &written, &notes, &g3_item);
  if (r != GB12_OK) return false;
  return pk3_validate(out80);
}

bool xfer_plant_entry(uint8_t met_game, const uint8_t rec80[80], GbscEntry* e) {
  if (!rec80 || !e) return false;
  uint8_t cell[80], converted[80];
  bank_plant_cell0_serial(cell, XFER_PLANT_SERIAL);
  GbEditMon written; Gb12Notes notes; uint16_t g3_item = 0;
  Gb12Result r = bdc_convert_gen3_core(cell, met_game, converted, &written, &notes, &g3_item);
  if (r != GB12_OK) return false;
  if (xr_key_g3(rec80) != xr_key_g3(converted)) return false;

  memset(e, 0, sizeof *e);
  gbsc_entry_from(e, &written, cell, PLANT_EPOCH);
  e->kind      = XR_KIND_NATIVE_HOME;
  e->state     = XR_STATE_CLAIMED;
  e->direction = XR_DIR_ABROAD_G3;
  e->claimed   = 1;
  return true;
}

#endif /* PDNA_DELTA */
