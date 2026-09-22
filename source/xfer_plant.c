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

bool xfer_plant_converted(uint8_t met_game, uint8_t out80[80]) {
  if (!out80) return false;
  uint8_t cell[80];
  bank_plant_cell0(cell);
  GbEditMon written; Gb12Notes notes; uint16_t g3_item = 0;
  Gb12Result r = bdc_convert_gen3_core(cell, met_game, out80, &written, &notes, &g3_item);
  if (r != GB12_OK) return false;
  return pk3_validate(out80);
}

bool xfer_plant_entry(uint8_t met_game, const uint8_t rec80[80], GbscEntry* e) {
  if (!rec80 || !e) return false;
  uint8_t cell[80], converted[80];
  bank_plant_cell0(cell);
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
