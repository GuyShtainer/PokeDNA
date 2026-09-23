#include <string.h>

#include "bank_down_convert.h"
#include "xfer_rec.h"         /* xr_game_item_mask, xr_time_capsule_block */
#include "item_map_g2g3.h"    /* item_g2_to_g3 */
#include "data_tables.h"      /* pk_item_games */
#include "gb_moves_legal.h"   /* BACKLOG #212: g3gb_moves_ok -- per-slot move predicate */
#include "pdna_gen12.h"       /* gb_bank_down_gen3/gb_bank_down_bridge -- GBA arms below */

/* ---- BACKLOG #177: the GB-side name-glyph-loss oracle -------------------------- */
/* review F1 (Fable pin): the earlier decoder-fallback rule (did a byte's own decode
 * land on the decoder's "unmapped" fallback glyph?) missed every lookalike loss --
 * accented letters folded to their plain ASCII form (C0-C5), the gender signs folded
 * to '?' (EF/F5), the Yen sign / times sign (F0/F1), '&' (E9), e-acute (EA), and the
 * eight bracket/punctuation glyphs (9A-9F) that decode cleanly but that gen3_encode_char
 * has NO case for and silently drops to a Gen-3 space on the way into the intermediate
 * record (gb_sidecar.c:440-461's own gb_nick_char_ok_for_gen3 lists that exact set) --
 * 19 real losses in a full 0x00-0xFF sweep. None of those trip a decoder default; all
 * of them are still a genuine "the name that comes out is not the name that went in".
 *
 * The fix compares SPELLING, not decode-path: gb_char_decode (gb_edit.c:800-870) is the
 * lossless, generation-neutral speller (it never guesses -- every GB byte spells to
 * its own distinct escape or glyph, byte-exact, both directions, swept 255/255 in a
 * scratch copy). Decode the SOURCE record's name and the WRITTEN record's name through
 * the SAME speller (gb_get_otname/gb_get_nickname, gb_edit.c's own public wrappers over
 * gb_char_decode) and compare the two spellings: any difference beyond what the target
 * generation's own more limited species-name default already explains is loss, full
 * stop -- no byte-range table to keep in sync with gen3_encode_char/g2_glyph ever again. */
static bool gb_name_changed(const GbEditMon* src, const GbEditMon* dst, bool ot) {
  char a[GB_NAME_BYTES * GB_GLYPH_MAX + 1], b[GB_NAME_BYTES * GB_GLYPH_MAX + 1];
  if (!src || !dst) return false;
  if (ot) { gb_get_otname(src, a, (int)sizeof a);   gb_get_otname(dst, b, (int)sizeof b); }
  else    { gb_get_nickname(src, a, (int)sizeof a); gb_get_nickname(dst, b, (int)sizeof b); }
  return strcmp(a, b) != 0;
}

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
      if (g3_item) *g3_item = mapped; notes->item_g2 = g2_item;   /* travels: name it on the loss row */
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
                         GbEditMon* out, Gen3ToGbLoss* loss, Gb12Notes* notes,
                         uint16_t from4[4], uint8_t bad4[4], int* nbad) {
  *tc = 0; *tc_bad = 0; *g12 = GB12_ERR_EMPTY; *g3gb = G3GB_ERR_ARG;
  *nbad = 0;
  memset(notes, 0, sizeof *notes);
  memset(bad4, 0, 4 * sizeof bad4[0]);
  memset(from4, 0, 4 * sizeof from4[0]);

  GbEditMon src_mon; BcMeta meta;
  if (!bc_unpack(cell80, &src_mon, &meta)) return;

  Gb12Mon view;
  if (!bc_view(&src_mon, &meta, bc_ident32(cell80), &view)) return;

  uint16_t moves4[4] = { view.moves[0], view.moves[1], view.moves[2], view.moves[3] };
  memcpy(from4, moves4, sizeof moves4);

  /* BACKLOG #212: the species-floor check stays exactly as before (a species this
   * generation cannot represent at all has no per-slot fix); the move-bound check is
   * pulled OUT of xr_time_capsule_block's own all-or-nothing refusal (moves4 == NULL
   * below means "species only, this arm handles moves itself") and replaced with
   * gb_moves_legal.h's g3gb_moves_ok() -- the SAME per-slot predicate BACKLOG #150
   * S150-10 gave gb_paste_hook (source/pdna_gen12.c's gb_clip_moves wraps the
   * identical call). `moves4` is `view.moves`, BEFORE gen12_convert runs below --
   * gen12_convert copies them straight through into the intermediate Gen-3 record
   * unchanged (gen12_convert.c: `em_set_move(&e, i, in->moves[i])`, no remap), so a
   * bad4 index computed here still lines up with mid80's own decoded moves when
   * gen3_to_gb_fixed reads them again further down. */
  int nb = g3gb_moves_ok(moves4, dst_gen, bad4);
  if (nb < 0) return;   /* bad dst_gen -- *g3gb stays G3GB_ERR_ARG (its init above) */
  *nbad = nb;

  int block = xr_time_capsule_block(meta.gen, dst_gen, view.species_dex, NULL, tc_bad);
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

  /* BACKLOG #212: gen3_to_gb_fixed(), not gen3_to_gb() -- nbad > 0 empties the bad
   * slots instead of refusing the whole record (S150-10 decision 1's own contract).
   * The caller (source/pdna_gen12.c's gb_bank_down_bridge) fills them from the
   * destination ROM's learnset the SAME way gb_paste_fill_moves() does for PASTE,
   * once it knows the converted species/level -- this function only clips. */
  G3GbStatus st = gen3_to_gb_fixed(mid80, dst_gen, caught_available, g1base,
                                   nb > 0 ? bad4 : NULL, out, loss);
  *g3gb = st;
  /* BACKLOG #177 (review F1): compare the SOURCE record's own name spelling against the
   * WRITTEN record's -- `out` is only meaningfully populated on G3GB_OK, so this runs
   * after `st` is known, not alongside the item-drop re-apply above (which does not
   * depend on gen3_to_gb having run at all). src_mon (bc_unpack's raw GbEditMon) is
   * still in scope here. */
  if (st == G3GB_OK) {   /* `out` is only written on G3GB_OK */
    notes->otname_lossy = gb_name_changed(&src_mon, out, true);
    notes->nick_lossy   = gb_name_changed(&src_mon, out, false);
  }
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

/* BACKLOG #174 (S150-8c): a zeroed dstrec makes gb_bank_down_gen3's own occupancy test
 * (`occ = (dstrec[0..3] != 0) || bc_is_native(dstrec)`) false by construction -- there is
 * no PC cell to be occupied, the caller has already proved the party ADD slot is free.
 * `.rodata`, 80 B of ROM, 0 B of RAM (the gate forbids new EWRAM statics, not new `const`).
 * dst_box = -1 makes the log line self-identifying ("box -1 slot N" = the party). */
static const uint8_t k_empty80[80] = {0};

BankDownResult bank_down_convert_gen3_party(BoxSource* src, int party_slot,
                                            const uint8_t cell80[80], uint8_t out80[80]) {
  return gb_bank_down_gen3(src, -1, party_slot, cell80, k_empty80, out80);
}

#endif
