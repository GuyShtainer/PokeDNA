#include "xfer_rec.h"
#include <string.h>
#include "gb_edit.h"      /* gb_max_species/gb_max_move, GB_GEN1 -- already pure C */
#include "bank_cell.h"    /* bc_is_native, bc_unpack, BcMeta                         */
#include "gen3_mon.h"     /* PkMon, pk_decode_mon                                    */
#include "gen3_box.h"     /* pk_resolve                                              */
#include "data_tables.h"  /* pk_national_no                                          */
#include "gen3_save.h"    /* gen3_decode_char -- review F4's unmappable-glyph guard  */
#include "item_map_g2g3.h" /* item_g2_to_g3 -- S150-9 decision 5's abroad_item_dropped */

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

/* Moves + PP-Ups, decision 11 (per-slot legality, G-H8), decision 1 (accept-gated
 * apply, mask-independent report). Split out of xr_merge_down_sel per the ten golden
 * rules' function-length rule -- mirrors gb_sidecar.c's own merge_moves() in shape. */
static void xr_merge_moves(GbEditMon* out, const GbEditMon* home, const GbscEntry* e,
                           const PkMon* m, uint8_t accept, XrMergeReport* rep) {
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
    uint8_t cur_up = (uint8_t)((m->ppBonuses >> (i * 2)) & 0x3u);
    if (cur_mv16 != 0 && cur_mv16 > gb_max_move(home->gen)) {   /* covers > 255 too */
      rep->move_refused[i] = true;
      continue;                                             /* keep the home's move */
    }
    uint8_t cur_mv = (uint8_t)cur_mv16;
    if (cur_mv == base_mv && cur_up == base_up) continue;
    rep->moves_changed = true;                         /* report is mask-independent */
    if (!(accept & XR_ACCEPT_MOVES)) continue;
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
  }
}

/* Nickname, decision 11 (direction-gated, safe degrade), decision 1 (accept-gated
 * apply). Split out of xr_merge_down_sel for the same reason as xr_merge_moves();
 * mirrors gb_sidecar.c's merge_nickname(). */
static void xr_merge_nickname(GbEditMon* out, const GbEditMon* home, const GbscEntry* e,
                              const uint8_t g3_rec80[80], const PkMon* m, uint8_t accept,
                              XrMergeReport* rep) {
  if (e->direction != XR_DIR_ABROAD_G3) {
    rep->nick_baseline_missing = true;   /* pre-#150 or mis-stamped entry -- skip, don't refuse */
    return;
  }
  if (memcmp(g3_rec80 + 0x08, e->nick_written, 10) == 0) return;   /* unchanged */

  /* Fable review F4: gen3_decode_char() folds every UNMAPPABLE Gen-3 glyph (0x01,
   * 0x1B, 0xB0, 0xF7, ...) to '?' by its own `default:` case -- the SAME character
   * 0xAC legitimately decodes to. Without this check, gb_text_lossy() sees a
   * perfectly GB-spellable '?' and never refuses, so an unmappable byte silently
   * becomes a literal question mark in the home's nickname instead of refusing the
   * rename. Scan the raw Gen-3 bytes directly: a decoded '?' whose raw byte is NOT
   * the genuine 0xAC is an unmappable glyph -- refuse, keep the home name. */
  for (int k = 0; k < 10 && g3_rec80[8 + k] != 0xFF; k++) {
    if (gen3_decode_char(g3_rec80[8 + k]) == '?' && g3_rec80[8 + k] != 0xAC) {
      rep->rename_refused = true;
      return;
    }
  }

  char bad[GB_GLYPH_MAX];
  if (gb_text_lossy(home->gen, m->nickname, GB_NICK_GLYPHS, bad) == 0) {
    rep->renamed = true;                               /* report is mask-independent */
    if (accept & XR_ACCEPT_NICK) gb_set_nickname(out, m->nickname);
  } else {
    rep->rename_refused = true;   /* home nickname kept, byte for byte */
  }
}

/* BACKLOG #150 S150-8b, decisions 10 + 11; S150-9 decision 1 adds the accept mask.
 * Mirrors gb_sidecar.c's gbsc_merge_up() in shape, opposite in direction -- see
 * xfer_rec.h's contract comment for the full reasoning; only the mechanics are
 * repeated here. */
bool xr_merge_down_sel(const GbscEntry* e, const uint8_t g3_rec80[80], uint8_t accept,
                       GbEditMon* out, XrMergeReport* rep) {
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
  rep->species_kept = true;   /* decision 10/4: this direction never applies an evolution */

  /* species/evolution: reported, never applied (decision 4/10) -- accept is ignored */
  uint16_t nat_now = pk_national_no(m.species);
  if (nat_now != e->species_written) rep->evolved = true;

  /* level (and EXP), decisions 1 + 5 + 11 */
  uint8_t base_level = e->written_level ? e->written_level : gb_get_level(&home);
  if (m.level != 0 && m.level != base_level) {
    rep->level_changed = true;                          /* report is mask-independent */
    rep->level_from = base_level;
    rep->level_to = (uint8_t)m.level;
    if (accept & XR_ACCEPT_LEVEL) gb_set_level(out, m.level);
  }

  xr_merge_moves(out, &home, e, &m, accept, rep);
  xr_merge_nickname(out, &home, e, g3_rec80, &m, accept, rep);

  /* The Gen-2 held item (a GB-only concept) always stays in the cell -- it was never
   * part of the Gen-3 record to begin with, so there is nothing to fold in here; the
   * confirm screen's shipped string just needs to say so (G-H7). Not gated by
   * `accept` -- there is nothing to apply, only to report. */
  rep->gb_item_ignored = (home.gen == GB_GEN2) && gb_get_held_item(&home) != 0;

  /* S150-9 decision 5: folds pdna_box.c's own out-of-band g3_item probe into the
   * report -- only a GEN-3-SIDE item is actually lost (an item holder restored
   * UNCHANGED, i.e. the Gen-3 item still equals what the native cell's own held
   * item maps to, must read as unchanged, never as a loss). Not gated by `accept`
   * -- a native cell has no item slot to receive it either way (G-H7). */
  uint16_t mapped_item = item_g2_to_g3(gb_get_held_item(&home));
  rep->abroad_item_dropped = (m.heldItem != 0 && m.heldItem != mapped_item);

  return true;
}

bool xr_merge_down(const GbscEntry* e, const uint8_t g3_rec80[80], GbEditMon* out,
                   XrMergeReport* rep) {
  return xr_merge_down_sel(e, g3_rec80, XR_ACCEPT_ALL, out, rep);
}

/* ---- site 2: xr_merge_down_gb, the Gen-1/2-side twin (BACKLOG #150 S150-9 decision 2) --
 * Same shape as xr_merge_down_sel, but the abroad side is a GB record (`now`), not a
 * Gen-3 one, so the comparisons run against gb_get_*(now) directly instead of a decoded
 * PkMon. See xfer_rec.h's contract comment for the refusal list. */
static void xr_merge_moves_gb(GbEditMon* out, const GbEditMon* home, const GbscEntry* e,
                              const GbEditMon* now, uint8_t accept, XrMergeReport* rep) {
  for (int i = 0; i < 4; i++) {
    uint8_t base_mv, base_up;
    if (e->has_written_moves) {
      base_mv = e->moves_written[i];
      base_up = (uint8_t)((e->ppup_written >> (i * 2)) & 0x3u);
    } else {
      base_mv = gb_get_move(home, i);
      base_up = gb_get_ppup(home, i);
    }
    uint8_t cur_mv = gb_get_move(now, i);
    uint8_t cur_up = gb_get_ppup(now, i);
    if (cur_mv != 0 && cur_mv > gb_max_move(home->gen)) {
      rep->move_refused[i] = true;
      continue;                                             /* keep the home's move */
    }
    if (cur_mv == base_mv && cur_up == base_up) continue;
    rep->moves_changed = true;                         /* report is mask-independent */
    if (!(accept & XR_ACCEPT_MOVES)) continue;
    if (!gb_set_move(out, i, cur_mv)) { rep->move_refused[i] = true; continue; }
    if (cur_mv != 0) {
      gb_set_ppup(out, i, cur_up);
      uint8_t cap = gb_max_pp(home->gen, cur_mv, cur_up);
      uint8_t pp = gb_get_pp(now, i);
      if (pp > cap) pp = cap;
      gb_set_pp(out, i, pp);
    }
  }
}

static void xr_merge_nickname_gb(GbEditMon* out, const GbEditMon* home, const GbscEntry* e,
                                 const GbEditMon* now, uint8_t accept, XrMergeReport* rep) {
  if (memcmp(now->nick, e->nick_written, GB_NAME_BYTES) == 0) return;   /* unchanged */
  char text[GB_TEXT_MAX];
  int n = gb_name_decode(now->gen, text, (int)sizeof text, now->nick, GB_NAME_BYTES);
  (void)n;
  char bad[GB_GLYPH_MAX];
  if (gb_text_lossy(home->gen, text, GB_NICK_GLYPHS, bad) == 0) {
    rep->renamed = true;                                /* report is mask-independent */
    if (accept & XR_ACCEPT_NICK) gb_set_nickname(out, text);
  } else {
    rep->rename_refused = true;   /* home nickname kept, byte for byte */
  }
}

bool xr_merge_down_gb_sel(const GbscEntry* e, const GbEditMon* now, uint8_t accept,
                          GbEditMon* out, XrMergeReport* rep) {
  XrMergeReport local;
  if (!rep) rep = &local;
  memset(rep, 0, sizeof *rep);
  if (!e || !now || !out) return false;
  if (!bc_is_native(e->original80)) return false;
  if (e->direction != XR_DIR_ABROAD_GB) return false;
  if (now->gen != e->gen) return false;

  GbEditMon home;
  BcMeta meta;
  if (!bc_unpack(e->original80, &home, &meta)) return false;

  *out = home;
  rep->species_kept = true;   /* decision 4: never applied on the native-home side */

  uint16_t dex_now = gb_get_species_dex(now);
  if (dex_now != 0 && dex_now != e->species_written) rep->evolved = true;

  uint8_t base_level = e->written_level ? e->written_level : gb_get_level(&home);
  uint8_t level_now = gb_get_level(now);
  if (level_now != base_level) {
    rep->level_changed = true;
    rep->level_from = base_level;
    rep->level_to = level_now;
    if (accept & XR_ACCEPT_LEVEL) gb_set_level(out, level_now);
  }

  xr_merge_moves_gb(out, &home, e, now, accept, rep);
  xr_merge_nickname_gb(out, &home, e, now, accept, rep);

  /* Item, decision 2's table: a Gen-2 RESIDENCE holding an item cannot ride onto a
   * Gen-1 home (abroad_item_dropped); a Gen-2 HOME with an item already in the cell
   * keeps it -- it comes back (gb_item_ignored, G-H7). Neither is ever applied. */
  if (now->gen == GB_GEN2 && gb_get_held_item(now) != 0) rep->abroad_item_dropped = true;
  rep->gb_item_ignored = (home.gen == GB_GEN2) && gb_get_held_item(&home) != 0;

  return true;
}

bool xr_merge_down_gb(const GbscEntry* e, const GbEditMon* now, GbEditMon* out,
                      XrMergeReport* rep) {
  return xr_merge_down_gb_sel(e, now, XR_ACCEPT_ALL, out, rep);
}

/* BACKLOG #206 review R1: pc_bank_restore_up's pick loop + RESTORED/PENDING
 * refusals, extracted verbatim (see xfer_rec.h's contract comment). Returns
 * XR_PICK_NONE/leaves `out` untouched when NULL/empty buf. */
XrRestorePick xr_restore_pick_basic(const uint8_t* buf, uint32_t len, int count,
                                    GbscEntry* out) {
  if (!buf || count <= 0) return XR_PICK_NONE;
  int best = -1;
  GbscEntry e;
  for (int i = 0; i < count; i++) {
    GbscEntry cand;
    if (!gbsc_get(buf, len, i, &cand)) continue;
    if (cand.kind != XR_KIND_NATIVE_HOME) continue;
    if (!bc_is_native(cand.original80)) continue;
    best = i;
    e = cand;
  }
  if (best < 0) return XR_PICK_NONE;
  if (e.state == XR_STATE_RESTORED) return XR_PICK_REFUSE_RESTORED;
  if (e.state == XR_STATE_PENDING) return XR_PICK_REFUSE_PENDING;
  if (out) *out = e;
  return XR_PICK_LIVE;
}

/* BACKLOG #150 S150-9 decision 11: moved verbatim out of pdna_gen12.c's
 * xfer_down_write() (the pre-#150-S150-9 shape) so the flagship host round-trip test
 * exercises the REAL entry the DOWN edge writes. Pure gbsc_entry_from() + five field
 * stores + one memcpy -- no I/O. */
void xr_entry_for_down(GbscEntry* e, const GbEditMon* written, const uint8_t cell80[80],
                       uint32_t epoch, uint8_t direction, const uint8_t nick_g3[10]) {
  gbsc_entry_from(e, written, cell80, epoch);
  e->kind = XR_KIND_NATIVE_HOME;
  e->state = XR_STATE_PENDING;
  e->direction = direction;
  e->claimed = 1;
  /* D-8b-link / F3 (review): nick_written holds the nickname bytes IN THE ABROAD
   * FORMAT -- Gen-3 bytes for ABROAD_G3 (nick_g3, the 10 raw bytes at the converted
   * record's own +0x08), GB bytes (written->nick) for ABROAD_GB. */
  if (nick_g3) { memcpy(e->nick_written, nick_g3, 10); e->nick_written[10] = 0; }
  else         memcpy(e->nick_written, written->nick, sizeof e->nick_written);
}
