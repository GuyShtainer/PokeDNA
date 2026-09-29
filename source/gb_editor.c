/* gb_editor.c — the field model of the Game Boy mon editor. See gb_editor.h. */

#include <string.h>

#include "gb_editor.h"
#include "data_tables.h"   /* pk_species_name, pk_move_name */

/* ---- tiny text helpers (no stdio: this file dual-compiles into the GBA image) --- */

static void put_ch(char* dst, int cap, int* pos, char c) {
  if (*pos < cap - 1) dst[(*pos)++] = c;
  dst[*pos] = 0;
}
static void put_str(char* dst, int cap, int* pos, const char* s) {
  while (*s) put_ch(dst, cap, pos, *s++);
}
static void put_uint(char* dst, int cap, int* pos, unsigned v) {
  char tmp[11]; int n = 0;
  do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
  while (n) put_ch(dst, cap, pos, tmp[--n]);
}

/* ---- rows --------------------------------------------------------------------- */

static const char* const LABEL[GBE_NUM] = {
  "Nickname", "OT Name", "OT ID", "Level",
  "Item", "Friendship", "Cur HP",
  "Move 1", "Move 2", "Move 3", "Move 4",
  "Max PP 1", "Max PP 2", "Max PP 3", "Max PP 4",
  "PP 1", "PP 2", "PP 3", "PP 4",
  "DV Atk", "DV Def", "DV Spe", "DV Spc",
  "DV HP",
  "Gender",
  "Shiny", "Egg",
  "Met Time", "Met Level", "Met Loc", "Met OT Gender",
  "StatExp HP", "StatExp Atk", "StatExp Def", "StatExp Spe", "StatExp Spc",
};

/* dex<=251 IS the Gen-3 internal species index for the whole Gen-1/2 range: Gen 3 kept
 * the original 251 in National-Dex order and only assigned NEW internal ids (252..411,
 * Hoenn-ordered) to the Hoenn-native species (data_tables.h's own header: "SPECIES are
 * keyed by the INTERNAL Gen-3 index (1..411, Hoenn-ordered)"). So pk_species_gender_ratio
 * -- which the comment there marks "internal" -- takes a bare Gen-2 dex number unchanged
 * here exactly as gb_dv_effects_of() above already does; there is no separate map to look
 * up. Gen-2 gender ratios are therefore IDENTICAL to Gen 3's for every one of the 251
 * species both generations share (same fact gb_edit.h's GbGen1Base comment relies on for
 * base stats). */
bool gbe_has_gender_row(const GbEditMon* e) {
  uint16_t dex;
  uint8_t ratio;
  if (!e || e->gen != GB_GEN2) return false;
  dex = gb_get_species_dex(e);
  if (dex == 0) return false;
  ratio = pk_species_gender_ratio(dex);
  return ratio != 0x00u && ratio != 0xFEu && ratio != 0xFFu;
}

int gbe_fields(const GbEditMon* e, uint8_t out[GBE_NUM]) {
  int n = 0;
  if (!e || !out) return 0;
  for (int f = 0; f < GBE_NUM; f++) {
    if ((f == GBE_ITEM || f == GBE_FRIEND) && e->gen != GB_GEN2) continue;
    if (f == GBE_CURHP && !e->is_party) continue;   /* box records: no HP row (#231) */
    if (f == GBE_GENDER && !gbe_has_gender_row(e)) continue;
    /* BACKLOG #95: SHINY/EGG/MET* are Gen 2 only, same as ITEM/FRIEND above --
     * none of the four MET rows has a Gen-1 equivalent (gb_set_caught refuses a
     * Gen-1 record outright: no such field exists there), EGG is the species-LIST
     * byte Gen 1 never has (gb_set_egg's own "Gen 1 has no eggs at all"), and
     * SHINY -- though structurally just a DV combination, same as Gen 1's DVs --
     * mirrors Gen 3's F_SHINY row, which the "no separate feature Gen 3 lacks"
     * half of the parity rule (#61) would refuse to invent for Gen 1 anyway
     * (Gen 1 itself has no shiny concept at all, pokered has no such derivation). */
    if ((f == GBE_SHINY || f == GBE_EGG || f == GBE_METTIME || f == GBE_METLEVEL
         || f == GBE_METLOC || f == GBE_METOTGENDER) && e->gen != GB_GEN2) continue;
    /* BACKLOG #95 review C1 (blocker): the four MET rows pack into record bytes
     * 0x1D/0x1E, which are a real capture record ONLY in Crystal -- in Gold/Silver
     * the same two bytes are Unused1/Unused2 (see GbEditMon.has_caught, gb_edit.h).
     * A Gen-2 record whose caller never proved Crystal-ness (has_caught defaults
     * false) simply does not get these rows, exactly like GBE_ITEM/FRIEND above
     * refuse a Gen-1 record it cannot tell apart from a Gen-2 one on gen alone. */
    if ((f == GBE_METTIME || f == GBE_METLEVEL || f == GBE_METLOC || f == GBE_METOTGENDER)
        && !e->has_caught) continue;
    out[n++] = (uint8_t)f;
  }
  return n;
}

const char* gbe_label(int f) { return (f >= 0 && f < GBE_NUM) ? LABEL[f] : "?"; }

const char* gbe_label_of(const GbEditMon* e, int f) {
  if (f == GBE_FRIEND && e && gb_is_egg(e)) return "Egg cycles";
  return gbe_label(f);
}

int gbe_kind(int f) {
  if (f == GBE_NICK || f == GBE_OT) return GBE_K_TEXT;
  if (f >= GBE_MV0 && f <= GBE_MV3) return GBE_K_MOVE;
  if (f == GBE_ITEM) return GBE_K_ITEM;
  if (f == GBE_DVH) return GBE_K_SHOW;
  return GBE_K_NUM;   /* SHINY/EGG/GENDER: LEFT/RIGHT/A all just toggle, same shape
                        * GENDER already uses (gbe_adjust's own dir-independent branches) */
}

/* ---- values ------------------------------------------------------------------- */

static int dv_stat(int f) {            /* row -> GB_* stat index for the four stored DVs */
  switch (f) {
    case GBE_DVA: return GB_ATK;
    case GBE_DVD: return GB_DEF;
    case GBE_DVS: return GB_SPE;
    case GBE_DVC: return GB_SPC;
    default:      return -1;
  }
}

void gbe_value(const GbEditMon* e, int f, char* out, int cap) {
  int pos = 0;
  if (!out || cap <= 0) return;
  out[0] = 0;
  if (!e) return;

  if (f == GBE_NICK) { gb_get_nickname(e, out, cap); return; }
  if (f == GBE_OT)   { gb_get_otname(e, out, cap);   return; }

  switch (f) {
    case GBE_OTID:   put_uint(out, cap, &pos, gb_get_otid(e)); return;
    case GBE_LEVEL:  put_uint(out, cap, &pos, gb_get_level(e)); return;
    case GBE_ITEM: {
      uint8_t it = gb_get_held_item(e);
      if (!it) { put_str(out, cap, &pos, "None"); return; }
      put_ch(out, cap, &pos, '#'); put_uint(out, cap, &pos, it); return;
    }
    case GBE_FRIEND: put_uint(out, cap, &pos, gb_get_friendship(e)); return;
    case GBE_DVH:    put_uint(out, cap, &pos, gb_get_dv(e, GB_HP)); return;
    case GBE_CURHP:
      put_uint(out, cap, &pos, gb_get_current_hp(e)); put_ch(out, cap, &pos, '/');
      put_uint(out, cap, &pos, gb_get_stat(e, GB_HP)); return;
    case GBE_GENDER: {
      GbDvEffects fx;
      gb_dv_effects_of(e, &fx);
      /* gbe_has_gender_row() already excludes genderless (fx.gender == 2) from ever
       * reaching this row; a defensive "M" if it somehow did beats printing nothing. */
      put_str(out, cap, &pos, fx.gender == 1 ? "F" : "M");
      return;
    }
    case GBE_SHINY: {
      GbDvEffects fx;
      gb_dv_effects_of(e, &fx);
      put_str(out, cap, &pos, fx.shiny ? "Yes" : "No");
      return;
    }
    case GBE_EGG: put_str(out, cap, &pos, gb_is_egg(e) ? "Yes" : "No"); return;
    case GBE_METTIME: {
      static const char* const T[4] = { "None", "Morning", "Day", "Night" };
      put_str(out, cap, &pos, T[gb_get_caught_time(e) & 3]);
      return;
    }
    case GBE_METLEVEL: put_uint(out, cap, &pos, gb_get_caught_level(e)); return;
    case GBE_METLOC: put_ch(out, cap, &pos, '#'); put_uint(out, cap, &pos, gb_get_caught_loc(e)); return;
    case GBE_METOTGENDER: put_str(out, cap, &pos, gb_get_caught_ot_gender(e) ? "F" : "M"); return;
    default: break;
  }

  if (f >= GBE_MV0 && f <= GBE_MV3) {
    uint8_t mv = gb_get_move(e, f - GBE_MV0);
    put_str(out, cap, &pos, mv ? pk_move_name(mv) : "-");
    return;
  }
  if (f >= GBE_PPU0 && f <= GBE_PPU3) {
    int i = f - GBE_PPU0;
    uint8_t mv = gb_get_move(e, i);
    if (!mv) { put_str(out, cap, &pos, "-"); return; }
    put_uint(out, cap, &pos, gb_max_pp(e->gen, mv, gb_get_ppup(e, i)));
    put_str(out, cap, &pos, "  Ups ");
    put_uint(out, cap, &pos, gb_get_ppup(e, i));
    return;
  }
  if (f >= GBE_PP0 && f <= GBE_PP3) {
    int i = f - GBE_PP0;
    uint8_t mv = gb_get_move(e, i);
    if (!mv) { put_str(out, cap, &pos, "-"); return; }
    put_uint(out, cap, &pos, gb_get_pp(e, i));
    put_ch(out, cap, &pos, '/');
    put_uint(out, cap, &pos, gb_max_pp(e->gen, mv, gb_get_ppup(e, i)));
    return;
  }
  if (dv_stat(f) >= 0) { put_uint(out, cap, &pos, gb_get_dv(e, dv_stat(f))); return; }
  if (f >= GBE_SE0 && f <= GBE_SE4) { put_uint(out, cap, &pos, gb_get_statexp(e, f - GBE_SE0)); return; }
}

/* ---- d-pad ---------------------------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Is `move` already in a slot other than `except`? */
static bool move_taken(const GbEditMon* e, int except, uint8_t move) {
  if (!move) return false;
  for (int i = 0; i < 4; i++)
    if (i != except && gb_get_move(e, i) == move) return true;
  return false;
}

/* Step a move id by `dir` to the next id that is not a duplicate; 0 (empty) is a
 * legal stop at the bottom. Clamps at gb_max_move(). */
static bool step_move(GbEditMon* e, int i, int dir) {
  int cur = gb_get_move(e, i), max = gb_max_move(e->gen);
  int v = cur;
  for (;;) {
    v += dir;
    if (v < 0 || v > max) return false;
    if (!move_taken(e, i, (uint8_t)v)) break;
  }
  return gb_set_move(e, i, (uint8_t)v);
}

/* GBE_GENDER has no "up"/"down": with only two states, LEFT, RIGHT and A all do the
 * SAME thing -- flip to the other gender -- by moving the Attack DV (the only DV
 * gender is derived from, gen2_save.c's g2_gender_from_dv) to the NEAREST value that
 * (a) yields the other gender and (b) leaves shininess exactly as it was.
 *
 * (b) is one equality test, not two special cases, because gb_dv_effects() is itself
 * the single source of truth for both properties: trying every candidate Atk DV 0..15
 * through it and keeping only the ones whose fx.shiny matches today's fx.shiny
 * automatically reproduces both halves of the brief this implements --
 *   - shiny stays shiny: a shiny mon's Def/Spe/Spc are already 10/10/10 (g2_dv_shiny),
 *     so a candidate keeps fx.shiny true only by ALSO having its Atk bit 1 set, i.e.
 *     landing back in {2,3,6,7,10,11,14,15};
 *   - a non-shiny mon never BECOMES shiny by accident: when Def/Spe/Spc are not all
 *     10 the filter is a no-op (fx.shiny is false for every candidate, same as today),
 *     and when they ARE all 10 -- the one configuration where the wrong Atk value
 *     would create a shiny -- the filter excludes exactly that set.
 * No separate "is it shiny-eligible" check is needed; the derivation function already
 * encodes it.
 *
 * Ties (two candidates equally far from the current DV) keep the SMALLER value: the
 * loop only replaces `best` on a STRICTLY shorter distance, and runs v ascending. */
static bool gbe_flip_gender(GbEditMon* e) {
  uint8_t cur[4];
  uint16_t dex;
  uint8_t ratio;
  GbDvEffects cur_fx;
  int target, best = -1, best_dist = 16;

  cur[0] = gb_get_dv(e, GB_ATK);
  cur[1] = gb_get_dv(e, GB_DEF);
  cur[2] = gb_get_dv(e, GB_SPE);
  cur[3] = gb_get_dv(e, GB_SPC);
  dex   = gb_get_species_dex(e);
  ratio = dex ? pk_species_gender_ratio(dex) : 0xFFu;

  gb_dv_effects(cur, dex, ratio, &cur_fx);
  if (cur_fx.gender != 0 && cur_fx.gender != 1) return false;  /* no real gender here */
  target = cur_fx.gender ^ 1;

  for (int v = 0; v <= 15; v++) {
    uint8_t cand[4]; GbDvEffects fx; int dist;
    cand[0] = (uint8_t)v; cand[1] = cur[1]; cand[2] = cur[2]; cand[3] = cur[3];
    gb_dv_effects(cand, dex, ratio, &fx);
    if (fx.gender != target || fx.shiny != cur_fx.shiny) continue;
    dist = v - (int)cur[0]; if (dist < 0) dist = -dist;
    if (dist < best_dist) { best_dist = dist; best = v; }
  }
  if (best < 0) return false;   /* no candidate: cannot happen for a real gender_ratio
                                  * byte (see gbe_has_gender_row), refuse rather than guess */
  return gb_set_dv(e, GB_ATK, (uint8_t)best);
}

/* GBE_SHINY has no "up"/"down" either -- LEFT, RIGHT and A all flip shininess, the
 * same dir-independent shape GBE_GENDER already uses. Turning shiny ON needs
 * Def=Spe=Spc=10 (g2_dv_shiny's own precondition) PLUS an Atk DV with bit 1 set;
 * Def/Spe/Spc feed NOTHING else (gbe_flip_gender's own header: gender is Atk-only),
 * so forcing them to 10 has no side effect on gender -- only Atk needs a search, run
 * exactly like gbe_flip_gender's own (prefer keeping today's gender, else nearest,
 * else smallest). Turning shiny OFF is simpler and never touches gender: the
 * precondition needs all three of Def/Spe/Spc at 10, so nudging just Def away is
 * enough to un-shiny it -- but the nudge has to be 10 -> 8, not 10 -> 9: the derived
 * HP DV is assembled from BIT 0 of each of the four stored DVs (g2_hp_dv, gen2_save.c
 * -- Def's bit 0 is worth 4 there), and 10 (0b1010) and 9 (0b1001) differ in bit 0 as
 * well as bit 1, so 9 silently moves the HP DV by +/-4 in every one of the 8 shiny Atk
 * combinations. 8 (0b1000) shares bit 0 with 10 -- only bit 1 differs -- so it breaks
 * the "Def == 10" precondition (un-shinies) while leaving the HP DV, and everything
 * else, untouched. */
static bool gbe_flip_shiny(GbEditMon* e) {
  uint8_t cur[4];
  uint16_t dex; uint8_t ratio;
  GbDvEffects cur_fx;
  bool any;

  cur[0] = gb_get_dv(e, GB_ATK); cur[1] = gb_get_dv(e, GB_DEF);
  cur[2] = gb_get_dv(e, GB_SPE); cur[3] = gb_get_dv(e, GB_SPC);
  dex = gb_get_species_dex(e);
  ratio = dex ? pk_species_gender_ratio(dex) : 0xFFu;
  gb_dv_effects(cur, dex, ratio, &cur_fx);

  if (cur_fx.shiny) {
    e->shiny_gender_forced = false;
    return gb_set_dv(e, GB_DEF, 8);   /* OFF -- see the header comment for why 8, not 9 */
  }

  /* ON: Def/Spe/Spc -> 10 first (free), then search Atk among the 8 bit-1-set values. */
  any = gb_set_dv(e, GB_DEF, 10);
  any = gb_set_dv(e, GB_SPE, 10) || any;
  any = gb_set_dv(e, GB_SPC, 10) || any;

  int target = cur_fx.gender, best = -1, best_dist = 16, found_pass = -1;
  for (int pass = 0; pass < 2 && best < 0; pass++) {   /* pass 0: keep gender; pass 1: any */
    for (int v = 0; v <= 15; v++) {
      if (!(v & 2)) continue;
      uint8_t cand[4] = { (uint8_t)v, 10, 10, 10 };
      GbDvEffects fx; gb_dv_effects(cand, dex, ratio, &fx);
      if (pass == 0 && fx.gender != target) continue;
      int dist = v - (int)cur[0]; if (dist < 0) dist = -dist;
      if (dist < best_dist) { best_dist = dist; best = v; found_pass = pass; }
    }
  }
  if (best >= 0) any = gb_set_dv(e, GB_ATK, (uint8_t)best) || any;
  /* found_pass == 1: no candidate kept the original gender, so this species has no
   * shiny combination for that gender at all -- gbe_shiny_note() reports it. */
  e->shiny_gender_forced = (found_pass == 1);
  return any;
}

bool gbe_adjust(GbEditMon* e, int f, int dir, bool big) {
  /* Only gbe_flip_shiny() ever sets this, and only for the toggle that just ran;
   * clear it on every entry so pdna_gbedit.c's post-edit check cannot re-show the
   * popup on every later row (gbmon re-verify C9). */
  if (e) e->shiny_gender_forced = false;
  if (!e || (dir != -1 && dir != 1)) return false;
  int step = big ? 10 : 1;

  switch (f) {
    case GBE_OTID: {
      int v = clampi((int)gb_get_otid(e) + dir * (big ? 100 : 1), 0, 65535);
      if (v == gb_get_otid(e)) return false;
      gb_set_otid(e, (uint16_t)v); return true;
    }
    case GBE_LEVEL: {
      int v = clampi((int)gb_get_level(e) + dir * step, 1, 100);
      if (v == gb_get_level(e)) return false;
      return gb_set_level(e, (uint8_t)v);
    }
    case GBE_ITEM: {
      int v = clampi((int)gb_get_held_item(e) + dir * step, 0, 255);
      if (v == gb_get_held_item(e)) return false;
      return gb_set_held_item(e, (uint8_t)v);
    }
    case GBE_FRIEND: {
      int v = clampi((int)gb_get_friendship(e) + dir * step, 0, 255);
      if (v == gb_get_friendship(e)) return false;
      return gb_set_friendship(e, (uint8_t)v);
    }
    case GBE_CURHP: {                       /* THE reviving edit; 0..stored max (#231) */
      (void)gbe_settle_stats(e);            /* edit against the FRESH max, never a stale one */
      int mx = gb_get_stat(e, GB_HP);
      int v = big ? (dir > 0 ? mx : 0) : clampi((int)gb_get_current_hp(e) + dir, 0, mx);
      if (v == gb_get_current_hp(e)) return false;
      return gb_set_current_hp(e, (uint16_t)v);
    }
    case GBE_GENDER: return gbe_flip_gender(e);
    case GBE_SHINY:  return gbe_flip_shiny(e);
    case GBE_EGG:    return gb_set_egg(e, !gb_is_egg(e));
    case GBE_METTIME: {
      int v = clampi((int)gb_get_caught_time(e) + dir, 0, 3);
      if (v == gb_get_caught_time(e)) return false;
      return gb_set_caught(e, (uint8_t)v, gb_get_caught_level(e),
                            gb_get_caught_loc(e), gb_get_caught_ot_gender(e));
    }
    case GBE_METLEVEL: {
      int v = clampi((int)gb_get_caught_level(e) + dir * step, 0, 63);
      if (v == gb_get_caught_level(e)) return false;
      return gb_set_caught(e, gb_get_caught_time(e), (uint8_t)v,
                            gb_get_caught_loc(e), gb_get_caught_ot_gender(e));
    }
    case GBE_METLOC: {
      int v = clampi((int)gb_get_caught_loc(e) + dir * step, 0, 127);
      if (v == gb_get_caught_loc(e)) return false;
      return gb_set_caught(e, gb_get_caught_time(e), gb_get_caught_level(e),
                            (uint8_t)v, gb_get_caught_ot_gender(e));
    }
    case GBE_METOTGENDER: {
      uint8_t v = gb_get_caught_ot_gender(e) ? 0 : 1;
      return gb_set_caught(e, gb_get_caught_time(e), gb_get_caught_level(e),
                            gb_get_caught_loc(e), v);
    }
    default: break;
  }

  if (f >= GBE_MV0 && f <= GBE_MV3) {
    bool any = false;
    for (int k = 0; k < step; k++) { if (!step_move(e, f - GBE_MV0, dir)) break; any = true; }
    return any;
  }
  if (f >= GBE_PPU0 && f <= GBE_PPU3) {
    int i = f - GBE_PPU0;
    int v = clampi((int)gb_get_ppup(e, i) + dir, 0, 3);
    if (v == gb_get_ppup(e, i)) return false;
    return gb_set_ppup(e, i, (uint8_t)v);
  }
  if (f >= GBE_PP0 && f <= GBE_PP3) {
    int i = f - GBE_PP0;
    uint8_t mv = gb_get_move(e, i);
    if (!mv) return false;
    int max = gb_max_pp(e->gen, mv, gb_get_ppup(e, i));
    int v = clampi((int)gb_get_pp(e, i) + dir * (big ? 5 : 1), 0, max);
    if (v == gb_get_pp(e, i)) return false;
    return gb_set_pp(e, i, (uint8_t)v);
  }
  if (dv_stat(f) >= 0) {
    int st = dv_stat(f);
    int v = clampi((int)gb_get_dv(e, st) + dir * (big ? 5 : 1), 0, 15);
    if (v == gb_get_dv(e, st)) return false;
    return gb_set_dv(e, st, (uint8_t)v);
  }
  if (f >= GBE_SE0 && f <= GBE_SE4) {
    int st = f - GBE_SE0;
    int v = clampi((int)gb_get_statexp(e, st) + dir * (big ? 5000 : 100), 0, 65535);
    if (v == gb_get_statexp(e, st)) return false;
    return gb_set_statexp(e, st, (uint16_t)v);
  }
  return false;
}

bool gbe_press(GbEditMon* e, int f) {
  /* Only gbe_flip_shiny() ever sets this, and only for the toggle that just ran;
   * clear it on every entry so pdna_gbedit.c's post-edit check cannot re-show the
   * popup on every later row (gbmon re-verify C9). */
  if (e) e->shiny_gender_forced = false;
  if (!e) return false;
  switch (f) {
    case GBE_LEVEL:  return gb_set_level(e, (uint8_t)(gb_get_level(e) < 100 ? 100 : 1));
    /* GBE_ITEM is GBE_K_ITEM now, not GBE_K_NUM (UX-parity audit, Guy 2026-09-
     * 07): pdna_gbedit.c's GBE_K_ITEM branch opens the restricted item picker
     * on A instead of ever reaching this case -- kept anyway as the pure-C
     * "clear it" fallback this switch already offers every other field
     * (a caller that presses without going through the kind dispatch still
     * gets a defined, useful answer, not a silent no-op). */
    case GBE_ITEM:   return gb_get_held_item(e) ? gb_set_held_item(e, 0) : false;
    case GBE_FRIEND: return gb_set_friendship(e, (uint8_t)(gb_get_friendship(e) == 255 ? 0 : 255));
    case GBE_CURHP: {                       /* A: full <-> fainted */
      (void)gbe_settle_stats(e);            /* edit against the FRESH max, never a stale one */
      uint16_t mx = gb_get_stat(e, GB_HP);
      return gb_set_current_hp(e, gb_get_current_hp(e) == mx ? 0 : mx);
    }
    case GBE_GENDER: return gbe_flip_gender(e);
    case GBE_SHINY:  return gbe_flip_shiny(e);
    case GBE_EGG:    return gb_set_egg(e, !gb_is_egg(e));
    case GBE_METTIME: return gb_set_caught(e, (uint8_t)(gb_get_caught_time(e) == 3 ? 0 : 3),
                                            gb_get_caught_level(e), gb_get_caught_loc(e),
                                            gb_get_caught_ot_gender(e));
    case GBE_METLEVEL: return gb_set_caught(e, gb_get_caught_time(e),
                                             (uint8_t)(gb_get_caught_level(e) >= 63 ? 0 : 63),
                                             gb_get_caught_loc(e), gb_get_caught_ot_gender(e));
    case GBE_METLOC: return gb_set_caught(e, gb_get_caught_time(e), gb_get_caught_level(e),
                                           (uint8_t)(gb_get_caught_loc(e) >= 127 ? 0 : 127),
                                           gb_get_caught_ot_gender(e));
    case GBE_METOTGENDER: return gb_set_caught(e, gb_get_caught_time(e), gb_get_caught_level(e),
                                                gb_get_caught_loc(e),
                                                (uint8_t)(gb_get_caught_ot_gender(e) ? 0 : 1));
    default: break;
  }
  if (f >= GBE_PPU0 && f <= GBE_PPU3) {
    int i = f - GBE_PPU0;
    return gb_set_ppup(e, i, (uint8_t)(gb_get_ppup(e, i) == 3 ? 0 : 3));
  }
  if (f >= GBE_PP0 && f <= GBE_PP3) {
    int i = f - GBE_PP0;
    uint8_t mv = gb_get_move(e, i);
    if (!mv) return false;
    uint8_t max = gb_max_pp(e->gen, mv, gb_get_ppup(e, i));
    if (gb_get_pp(e, i) == max) return false;
    return gb_set_pp(e, i, max);
  }
  if (dv_stat(f) >= 0) {
    int st = dv_stat(f);
    return gb_set_dv(e, st, (uint8_t)(gb_get_dv(e, st) == 15 ? 0 : 15));
  }
  if (f >= GBE_SE0 && f <= GBE_SE4) {
    int st = f - GBE_SE0;
    return gb_set_statexp(e, st, (uint16_t)(gb_get_statexp(e, st) == 65535 ? 0 : 65535));
  }
  return false;
}

/* ---- keyboard / picker results -------------------------------------------------- */

bool gbe_set_text(GbEditMon* e, int f, const char* s, char first_bad[GB_GLYPH_MAX]) {
  if (first_bad) first_bad[0] = 0;
  if (!e || !s) return false;
  if (f == GBE_NICK) {
    if (gb_text_lossy(e->gen, s, GB_NICK_GLYPHS, first_bad) > 0) return false;
    return gb_set_nickname(e, s);
  }
  if (f == GBE_OT) {
    if (gb_text_lossy(e->gen, s, GB_OT_GLYPHS, first_bad) > 0) return false;
    return gb_set_otname(e, s);
  }
  return false;
}

bool gbe_set_move(GbEditMon* e, int f, uint16_t move) {
  if (!e || f < GBE_MV0 || f > GBE_MV3) return false;
  if (move > gb_max_move(e->gen)) return false;
  if (move_taken(e, f - GBE_MV0, (uint8_t)move)) return false;
  return gb_set_move(e, f - GBE_MV0, (uint8_t)move);
}

/* ---- header / stats ------------------------------------------------------------- */

void gbe_header(const GbEditMon* e, char* out, int cap) {
  int pos = 0;
  if (!out || cap <= 0) return;
  out[0] = 0;
  if (!e) return;
  uint16_t dex = gb_get_species_dex(e);
  if (gb_is_egg(e)) put_str(out, cap, &pos, "EGG ");
  put_str(out, cap, &pos, (dex >= 1 && dex <= 251) ? pk_species_name(dex) : "MISSINGNO");
  put_str(out, cap, &pos, "  Lv");
  put_uint(out, cap, &pos, gb_get_level(e));
  GbDvEffects fx;
  gb_dv_effects_of(e, &fx);
  if (fx.gender == 0) put_str(out, cap, &pos, "  M");
  if (fx.gender == 1) put_str(out, cap, &pos, "  F");
  if (fx.shiny)       put_str(out, cap, &pos, "  SHINY");
  if (fx.unown) { put_str(out, cap, &pos, "  "); put_ch(out, cap, &pos, (char)('A' + fx.unown_letter)); }
}

const char* gbe_stale_note(const GbEditMon* e) {
  if (!e || !e->is_party || !e->stats_stale) return 0;
  if (e->gen == GB_GEN2) return "Party stats will be recalculated.";
  return "Gen 1: stats refresh on level-up or box withdrawal.";
}

/* BACKLOG #95 review C2: -1 == gbe_flip_shiny() did not just force a gender move; 0/1
 * == it did, and this is the gender (0 male, 1 female) the Pokemon ended up as -- read
 * off the record itself (gb_dv_effects_of), not remembered from the search, so it is
 * always the ACTUAL current gender. e->shiny_gender_forced is the flag itself
 * (host_gbeditor_test.c's test_shiny asserts that one directly); this is the
 * screen-facing form pdna_gbedit.c's msg_wait call needs to pick which of the two
 * PDNA_GBEDIT_SHINY_FORCED_MALE/FEMALE_L1/L2 pairs to show. */
int gbe_shiny_forced_gender(const GbEditMon* e) {
  GbDvEffects fx;
  if (!e || !e->shiny_gender_forced) return -1;
  gb_dv_effects_of(e, &fx);
  return fx.gender == 1 ? 1 : 0;
}

bool gbe_settle_stats(GbEditMon* e) {
  if (!e) return false;
  if (!e->is_party || !e->stats_stale) return true;
  return gb_recalc_stats(e);
}
