#include "xfer_rec.h"
#include <string.h>
#include "gb_edit.h"      /* gb_max_species/gb_max_move, GB_GEN1 -- already pure C */
#include "bank_cell.h"    /* bc_is_native, bc_unpack, BcMeta                         */
#include "gen3_mon.h"     /* PkMon, pk_decode_mon                                    */
#include "gen3_box.h"     /* pk_resolve                                              */
#include "data_tables.h"  /* pk_national_no                                          */

/* Same constants gb_sidecar.c's gbsc_key() uses (source/gb_sidecar.h:68-70). */
uint64_t xr_key_g3(const uint8_t rec80[80]) {
  uint64_t h = 14695981039346656037ULL;   /* FNV-1a-64 offset basis */
  for (int i = 0; i < 8; i++) {
    h ^= (uint64_t)rec80[i];
    h *= 1099511628211ULL;                /* FNV-1a-64 prime */
  }
  return h;
}

/* BACKLOG #150 S150-8 decision 5. met_game: 1 Sapphire, 2 Ruby, 3 Emerald, 4 FireRed,
 * 5 LeafGreen (pdna_main.c's own app_met_game() spelling). */
uint8_t xr_game_item_mask(uint8_t met_game) {
  switch (met_game) {
    case 1: case 2: return 0x01u;   /* RS   */
    case 3:         return 0x02u;   /* Emerald */
    case 4: case 5: return 0x04u;   /* FRLG */
    default:        return 0u;
  }
}

/* BACKLOG #150 S150-8 decision 14/D-Q7. */
int xr_time_capsule_block(uint8_t src_gen, uint8_t dst_gen, uint16_t species_dex,
                          const uint16_t moves[4], uint16_t* bad) {
  if (dst_gen != GB_GEN1) return 0;    /* Gen 1 -> Gen 2 (or anything else): always allowed */
  (void)src_gen;                       /* only the DESTINATION's floor matters here */
  if (species_dex == 0 || species_dex > gb_max_species(GB_GEN1)) {
    if (bad) *bad = species_dex;
    return 1;
  }
  if (moves) {
    for (int i = 0; i < 4; i++) {
      if (moves[i] != 0 && moves[i] > gb_max_move(GB_GEN1)) {
        if (bad) *bad = moves[i];
        return 2;
      }
    }
  }
  return 0;
}

/* Moves + PP-Ups, decision 11 (per-slot legality, G-H8). Split out of xr_merge_down
 * per the ten golden rules' function-length rule -- mirrors gb_sidecar.c's own
 * merge_moves() in shape. */
static void xr_merge_moves(GbEditMon* out, const GbEditMon* home, const GbscEntry* e,
                           const PkMon* m, XrMergeReport* rep) {
  for (int i = 0; i < 4; i++) {
    uint8_t base_mv, base_up;
    if (e->has_written_moves) {
      base_mv = e->moves_written[i];
      base_up = (uint8_t)((e->ppup_written >> (i * 2)) & 0x3u);
    } else {
      base_mv = gb_get_move(home, i);
      base_up = gb_get_ppup(home, i);
    }
    uint16_t cur_mv16 = m->moves[i];
    uint8_t cur_mv = (cur_mv16 > 255u) ? 0 : (uint8_t)cur_mv16;
    uint8_t cur_up = (uint8_t)((m->ppBonuses >> (i * 2)) & 0x3u);
    if (cur_mv == base_mv && cur_up == base_up) continue;   /* this slot unchanged */

    if (cur_mv != 0 && cur_mv > gb_max_move(home->gen)) {
      rep->move_refused[i] = true;
      continue;                                             /* keep the home's move */
    }
    /* Move first (gb_set_move resets PP/PP-Ups for the slot); PP-Ups next; current
     * PP clamped last -- the exact order gen3_to_gb.c:124-141's set_moves follows.
     * Neither of the last two for an empty slot. */
    if (!gb_set_move(out, i, cur_mv)) { rep->move_refused[i] = true; continue; }
    if (cur_mv != 0) {
      gb_set_ppup(out, i, cur_up);
      uint8_t cap = gb_max_pp(home->gen, cur_mv, cur_up);
      uint8_t pp = m->pp[i];
      if (pp > cap) pp = cap;
      gb_set_pp(out, i, pp);
    }
    rep->moves_changed = true;
  }
}

/* Nickname, decision 11 (direction-gated, safe degrade). Split out of xr_merge_down
 * for the same reason as xr_merge_moves(); mirrors gb_sidecar.c's merge_nickname(). */
static void xr_merge_nickname(GbEditMon* out, const GbEditMon* home, const GbscEntry* e,
                              const uint8_t g3_rec80[80], const PkMon* m,
                              XrMergeReport* rep) {
  if (e->direction != XR_DIR_ABROAD_G3) {
    rep->nick_baseline_missing = true;   /* pre-#150 or mis-stamped entry -- skip, don't refuse */
    return;
  }
  if (memcmp(g3_rec80 + 0x08, e->nick_written, 10) == 0) return;   /* unchanged */

  char bad[GB_GLYPH_MAX];
  if (gb_text_lossy(home->gen, m->nickname, GB_NICK_GLYPHS, bad) == 0) {
    if (gb_set_nickname(out, m->nickname)) rep->renamed = true;
  } else {
    rep->rename_refused = true;   /* home nickname kept, byte for byte */
  }
}

/* BACKLOG #150 S150-8b, decisions 10 + 11. Mirrors gb_sidecar.c's gbsc_merge_up() in
 * shape, opposite in direction -- see xfer_rec.h's contract comment for the full
 * reasoning; only the mechanics are repeated here. */
bool xr_merge_down(const GbscEntry* e, const uint8_t g3_rec80[80], GbEditMon* out,
                   XrMergeReport* rep) {
  XrMergeReport local;
  if (!rep) rep = &local;
  memset(rep, 0, sizeof *rep);
  if (!e || !g3_rec80 || !out) return false;
  /* G-F4/G-H6, mirrored: this direction's ORIGINAL must BE native; gbsc_merge_up's
   * refusal is the opposite polarity (its original80 must NOT be native). */
  if (!bc_is_native(e->original80)) return false;

  GbEditMon home;
  BcMeta meta;
  if (!bc_unpack(e->original80, &home, &meta)) return false;

  PkMon m;
  if (!pk_decode_mon(g3_rec80, false, &m)) return false;
  pk_resolve(&m);

  *out = home;
  rep->species_kept = true;   /* decision 10: xr_merge_down never applies an evolution */

  /* species/evolution: reported, never applied (decision 10) */
  uint16_t nat_now = pk_national_no(m.species);
  if (nat_now != e->species_written) rep->evolved = true;

  /* level (and EXP), decision 11 */
  uint8_t base_level = e->written_level ? e->written_level : gb_get_level(&home);
  if (m.level != 0 && m.level != base_level) {
    if (gb_set_level(out, m.level)) rep->level_changed = true;
  }

  xr_merge_moves(out, &home, e, &m, rep);
  xr_merge_nickname(out, &home, e, g3_rec80, &m, rep);

  /* The Gen-2 held item (a GB-only concept) always stays in the cell -- it was never
   * part of the Gen-3 record to begin with, so there is nothing to fold in here; the
   * confirm screen's shipped string just needs to say so (G-H7). */
  rep->gb_item_ignored = (home.gen == GB_GEN2);

  return true;
}
