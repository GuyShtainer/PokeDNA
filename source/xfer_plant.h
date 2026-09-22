#ifndef XFER_PLANT_H
#define XFER_PLANT_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_sidecar.h"   /* GbscEntry                                                */

/* BACKLOG #150 S150-15 decision 13, finding (e): the PDNA_DELTA-only seam that lets
 * the emulator/delta vehicle manufacture a converted mon WITH a ledger entry (the
 * plain `fuse_sav.py <Emerald.sav>` path has no way to create one otherwise, BACKLOG
 * #179 -- every DOWN drop stops at "SIDECAR FOLDER / Nothing transferred"). ZERO
 * callers in the shipped build (PDNA_DELTA is never defined there); precedent:
 * bank_plant.h's own #ifdef-the-whole-body idiom, `typedef int ..._no_empty_tu;` for
 * the non-PDNA_DELTA side. Consulted ONLY after the real ledger lookup (xfer_view.c)
 * finds nothing -- never masking a real read error. */

#ifdef PDNA_DELTA

/* bank_plant_cell0()'s cell (Gen-2 CHIKORITA @ L12, origin GOLD -- no item, level
 * above its evolution floor) converted DOWN with bdc_convert_gen3_core, met_game.
 * The CHIKORITA plant has no item and stands above its floor, so the core's output
 * IS exactly what gb_bank_down_gen3 would have placed -- no em_set_level/em_set_item
 * pass needed. `out80` (80 bytes) receives the converted Gen-3 record; pk3_validate()
 * is the caller's own proof it is legal. Returns false (out80 untouched) only if
 * bdc_convert_gen3_core itself refuses (should not happen for this fixed cell). */
bool xfer_plant_converted(uint8_t met_game, uint8_t out80[80]);

/* Recomputes the SAME converted record xfer_plant_converted would produce and
 * compares its key (xr_key_g3) against `rec80`'s -- true only when `rec80` IS that
 * exact converted mon (never a false positive for an unrelated record). On a match,
 * fills `*e` via gbsc_entry_from(written, bank_plant_cell0's own cell, PLANT_EPOCH)
 * with kind = XR_KIND_NATIVE_HOME, state = XR_STATE_CLAIMED, direction =
 * XR_DIR_ABROAD_G3, claimed = 1, so the note the GB ORIGINAL row shows reads
 * "GOLD 26-09-16" -- the same date this lane's shot chain was cut on. */
bool xfer_plant_entry(uint8_t met_game, const uint8_t rec80[80], GbscEntry* e);

#else
typedef int xfer_plant_no_empty_tu;
#endif /* PDNA_DELTA */

#endif
