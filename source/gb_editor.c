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
  "Item", "Friendship",
  "Move 1", "Move 2", "Move 3", "Move 4",
  "Max PP 1", "Max PP 2", "Max PP 3", "Max PP 4",
  "PP 1", "PP 2", "PP 3", "PP 4",
  "DV Atk", "DV Def", "DV Spe", "DV Spc",
  "DV HP",
  "StatExp HP", "StatExp Atk", "StatExp Def", "StatExp Spe", "StatExp Spc",
};

int gbe_fields(const GbEditMon* e, uint8_t out[GBE_NUM]) {
  int n = 0;
  if (!e || !out) return 0;
  for (int f = 0; f < GBE_NUM; f++) {
    if ((f == GBE_ITEM || f == GBE_FRIEND) && e->gen != GB_GEN2) continue;
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
  if (f == GBE_DVH) return GBE_K_SHOW;
  return GBE_K_NUM;
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

bool gbe_adjust(GbEditMon* e, int f, int dir, bool big) {
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
  if (!e) return false;
  switch (f) {
    case GBE_LEVEL:  return gb_set_level(e, (uint8_t)(gb_get_level(e) < 100 ? 100 : 1));
    case GBE_ITEM:   return gb_get_held_item(e) ? gb_set_held_item(e, 0) : false;
    case GBE_FRIEND: return gb_set_friendship(e, (uint8_t)(gb_get_friendship(e) == 255 ? 0 : 255));
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

bool gbe_settle_stats(GbEditMon* e) {
  if (!e) return false;
  if (!e->is_party || !e->stats_stale) return true;
  return gb_recalc_stats(e);
}
