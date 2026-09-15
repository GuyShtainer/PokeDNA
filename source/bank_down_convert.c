#include <string.h>

#include "bank_down_convert.h"
#include "xfer_rec.h"         /* xr_game_item_mask, xr_time_capsule_block */
#include "item_map_g2g3.h"    /* item_g2_to_g3 */
#include "data_tables.h"      /* pk_item_games */
#include "pdna_gen12.h"       /* gb_bank_down_gen3/gb_bank_down_bridge -- GBA arms below */

/* ---- pure core, arm 2 (decision 3+5+6) ------------------------------------- */

Gb12Result bdc_convert_gen3_core(const uint8_t cell80[BC_CELL_BYTES], uint8_t met_game,
                                 uint8_t out80[80], GbEditMon* written, Gb12Notes* notes,
                                 uint16_t* g3_item) {
  BcMeta meta;
  if (!bc_unpack(cell80, written, &meta)) return GB12_ERR_EMPTY;

  Gb12Mon view;
  if (!bc_view(written, &meta, bc_ident32(cell80), &view)) return GB12_ERR_EMPTY;

  /* decision 5: the held item travels through the map, gated by the cart mask, else
   * it waits behind (zeroed here so gen12_can_convert never sees GB12_ERR_HELD_ITEM;
   * the item is not lost -- it stays inside cell80/original80). */
  uint8_t g2_item = view.held_item;
  uint16_t mapped = g2_item ? item_g2_to_g3(g2_item) : 0;
  bool travels = mapped != 0 && (pk_item_games(mapped) & xr_game_item_mask(met_game)) != 0;
  view.held_item = 0;   /* decision 6: relax BEFORE gen12_convert, always */

  Gb12Target tgt = { .met_game = met_game };
  Gb12Result r = gen12_convert(&view, &tgt, out80, notes);
  if (r != GB12_OK) return r;

  if (g2_item != 0) {
    if (travels) {
      if (g3_item) *g3_item = mapped;
    } else {
      notes->item_dropped = true;
      notes->item_g2 = g2_item;
      if (g3_item) *g3_item = 0;
    }
  } else if (g3_item) {
    *g3_item = 0;
  }
  return GB12_OK;
}

/* ---- pure core, arm 1 (decision 14+15) ------------------------------------- */

void bdc_convert_gb_core(const uint8_t cell80[BC_CELL_BYTES], uint8_t dst_gen,
                         bool caught_available, const GbGen1Base* g1base,
                         int* tc, uint16_t* tc_bad, Gb12Result* g12, G3GbStatus* g3gb,
                         GbEditMon* out, Gen3ToGbLoss* loss, Gb12Notes* notes) {
  *tc = 0; *tc_bad = 0; *g12 = GB12_ERR_EMPTY; *g3gb = G3GB_ERR_ARG;
  memset(notes, 0, sizeof *notes);

  GbEditMon src_mon; BcMeta meta;
  if (!bc_unpack(cell80, &src_mon, &meta)) return;

  Gb12Mon view;
  if (!bc_view(&src_mon, &meta, bc_ident32(cell80), &view)) return;

  uint16_t moves4[4] = { view.moves[0], view.moves[1], view.moves[2], view.moves[3] };
  int block = xr_time_capsule_block(meta.gen, dst_gen, view.species_dex, moves4, tc_bad);
  if (block != 0) { *tc = block; return; }

  /* decision 15: a Gen-2 item cannot reach Gen 1 at all -- always zeroed on this arm
   * (unlike the Gen-3 arm there is no cart mask to consult: Gen 1 has no item bag
   * shape gen3_to_gb could even try to fill). The record (cell80) keeps it. */
  uint8_t g2_item = view.held_item;
  view.held_item = 0;
  if (g2_item != 0) { notes->item_dropped = true; notes->item_g2 = g2_item; }

  uint8_t mid80[80];
  Gb12Target tgt0 = { .met_game = 0 };   /* "the intermediate never lands anywhere" */
  Gb12Result cr = gen12_convert(&view, &tgt0, mid80, notes);
  *g12 = cr;
  if (cr != GB12_OK) return;

  /* gen12_convert() zeroed *notes at entry (D-Q8, verified against gen12_convert.c) --
   * re-apply the item-drop note it cannot know about, since gen12_convert() has no
   * item awareness at all on this path (met_game 0 never writes an item either way). */
  if (g2_item != 0) { notes->item_dropped = true; notes->item_g2 = g2_item; }

  G3GbStatus st = gen3_to_gb(mid80, dst_gen, caught_available, g1base, out, loss);
  *g3gb = st;
}

/* ============================================================================
 * GBA-facing arms -- thin wrappers, excluded from the host build (bank_down_convert.c
 * is linked whole into tests/host_xferdown_test.c, so the FatFs/tonc-touching bodies
 * this lane's real card I/O needs cannot live in THIS file the way pdna_gen12.c's own
 * PDNA_GEN12_HOST guard hides its own GBA glue). gb_bank_down_gen3/gb_bank_down_bridge
 * (source/pdna_gen12.c, declared in pdna_gen12.h) do the actual work: they need g_ed,
 * the sidecar buffer and the loss/legal screens, which are that file's own statics --
 * see the S150-8 delivery report's dispatcher-reconciliation note for why the D-Q1
 * "arms in a NEW file" instruction is satisfied at this thin-wrapper layer rather than
 * by re-exposing pdna_gen12.c's internals wholesale.
 * ============================================================================ */
#ifndef PDNA_GEN12_HOST

BankDownResult bank_down_convert_gb(BoxSource* src, int dst_box, int dst_cell,
                                    const uint8_t cell80[80]) {
  (void)src; (void)dst_cell;
  return gb_bank_down_bridge(dst_box, cell80);
}

BankDownResult bank_down_convert_gen3(BoxSource* src, int dst_box, int dst_cell,
                                      const uint8_t cell80[80], const uint8_t dstrec[80],
                                      uint8_t out80[80]) {
  return gb_bank_down_gen3(src, dst_box, dst_cell, cell80, dstrec, out80);
}

#endif
