/*
 * Game-faithful paged Pokémon summary (6 cards) for PokeDNA — now an
 * inline VIEW + EDIT screen. Each editable field registers its on-screen box
 * during render; in edit mode a cursor highlights one and A / LEFT-RIGHT edit it
 * in place (reusing the editor's field dispatchers). No separate "edit" mode.
 *   cards: 0 INFO  1 SKILLS  2 IVs  3 EVs  4 BATTLE MOVES  5 CONTEST MOVES
 *          6 ORIGIN  7 CONDITION (cool/beauty/cute/smart/tough/sheen, editable)
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_summary.h"
#include "pdna_app.h"     /* app_anim_enabled (portrait animation) */
#include "ui.h"
#include "gen3_mon.h"
#include "gen3_edit.h"
#include "gen3_box.h"      /* pk_resolve */
#include "data_tables.h"
#include "pdna_edit.h"   /* F_*, em_field_press / em_field_adjust */
#include "mon_front.h"
#include "mon_back.h"
#include "mon_icons.h"
#include "mon_anim.h"     /* per-species Emerald front-animation family */
#include "type_icons.h"
#include "snd.h"

#define NCARDS 8

/* Summary portrait animation (the Emerald entrance/idle wiggle). Compiled in, but
 * gated at runtime on the ANIM_SUMMARY toggle (Settings > Animations > Summary),
 * which defaults OFF — the static portrait from draw_left stands in until the user
 * opts in. Set to 0 to compile it out entirely. */
#define SUMMARY_ANIM 1

/* real Gen-3 type badge (32x14); ui_sprite honours the 0x8000 opacity bit. */
static void type_badge(int x, int y, uint8_t t) {
  if (t < 18) ui_sprite(x, y, TYPE_ICON_W, TYPE_ICON_H, type_icon_for(t));
}

static const int   DISP[6]   = { PK_HP, PK_ATK, PK_DEF, PK_SPA, PK_SPD, PK_SPE };
static const char* DLAB[6]   = { "HP", "Attack", "Defense", "Sp.Atk", "Sp.Def", "Speed" };
static const char* DSHORT[6] = { "HP", "Atk", "Def", "SpA", "SpD", "Spd" };

#define C_HDR  UI_TITLE
#define C_KEY  UI_DIRCLR
#define C_VAL  UI_TEXT
#define C_HOT  UI_WARN

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

/* SELECT flips the portrait between front and back sprite (issue #12); persists
 * across mon-scroll within a summary session. */
static bool g_back = false;

/* ---- editable-field slot registry (filled during render in edit mode) ---- */
static int g_edit = 0, g_nslot = 0;
static struct { int field, x, y, w; } g_slot[24];
static void reg(int field, int x, int y, int w) {
  if (g_edit && g_nslot < 24) { g_slot[g_nslot].field = field; g_slot[g_nslot].x = x;
                                g_slot[g_nslot].y = y; g_slot[g_nslot].w = w; g_nslot++; }
}

static void draw_dots(int x, int y, int n, int active) {
  for (int i = 0; i < n; i++) {
    int cx = x + i * 7;
    if (i == active) ui_fill_rect(cx, y, 5, 5, C_HDR);
    else { ui_fill_rect(cx, y, 5, 5, UI_PANEL); m3_frame(cx, y, cx + 4, y + 4, UI_BORDER); }
  }
}

static int text_wrap(int x, int y, int cols, u16 ink, const char* s) {
  char line[64];
  int li = 0;
  while (*s) {
    const char* w = s; int wl = 0;
    while (s[wl] && s[wl] != ' ') wl++;
    int need = (li ? li + 1 : 0) + wl;
    if (need > cols && li) { line[li] = 0; ui_text(x, y, ink, line); y += UI_ROW_H; li = 0; }
    if (li) line[li++] = ' ';
    for (int i = 0; i < wl && li < (int)sizeof(line) - 1; i++) line[li++] = w[i];
    s += wl;
    while (*s == ' ') s++;
  }
  if (li) { line[li] = 0; ui_text(x, y, ink, line); y += UI_ROW_H; }
  return y;
}

static u16  gender_col(uint8_t g) { return g == 0 ? RGB15(12, 18, 31) : g == 1 ? RGB15(31, 13, 19) : UI_DIM; }
static const char* gender_sym(uint8_t g) { return g == 0 ? "M" : g == 1 ? "F" : "-"; }

/* ---- Emerald-style blue gradient backdrops ---- */
/* The sprite "screen" behind the portrait (yy in 14..77): a light-blue vertical
 * gradient, like the real summary. Used by draw_left AND portrait_redraw so the
 * static and animated sprite sit on the same backdrop. */
static u16 portrait_bg(int yy) {
  int t = yy - 14; if (t < 0) t = 0; if (t > 63) t = 63;
  return RGB15(16 - t * 6 / 63, 22 - t * 6 / 63, 30 - t * 4 / 63);
}
/* Whole-screen blue gradient (medium-dark so the light card text stays readable). */
static void summary_bg(void) {
  for (int b = 0; b < 16; b++)
    ui_fill_rect(0, b * 10, UI_SCR_W, 10, RGB15(6 - b / 5, 11 - b / 3, 22 - b / 2));
}

/* Shared portrait column (all 7 cards): framed sprite, dex no, name, Lv + colored
 * sex, species, type badges, and an egg/shiny tag. No editable fields here. */
static void draw_left(const PkMon* p) {
  ui_panel(0, 11, 92, 139, RGB15(4, 7, 16), UI_BORDER);    /* dark-blue info column */
  m3_frame(11, 13, 80, 78, UI_BORDER);                     /* sprite sub-frame */
  for (int yy = 14; yy <= 77; yy++) ui_fill_rect(12, yy, 68, 1, portrait_bg(yy));   /* blue "screen" */
  const uint16_t* spr = g_back ? mon_back_for_form(p->species, p->isShiny, p->form) : 0;
  if (!spr) spr = mon_front_for_form(p->species, p->isShiny, p->form);   /* fall back to front */
  if (spr) ui_sprite(14, 14, MON_FRONT_W, MON_FRONT_H, spr);
  else     ui_sprite(30, 30, MON_ICON_W, MON_ICON_H, mon_icon_for_form(p->species, p->form));
  if (p->isShiny) ui_text(70, 16, C_HOT, "*");             /* gold shiny mark on the portrait */

  char buf[40];
  ui_hline(4, 81, 84, UI_BORDER);
  siprintf(buf, "#%03u", (unsigned)pk_national_no(p->species));
  ui_text(6, 84, C_KEY, buf);
  if (g_back) ui_text(44, 84, UI_DIM, "back");             /* current portrait side */

  char nm[24];
  ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 11);
  ui_text(6, 94, C_VAL, nm);

  siprintf(buf, "Lv%u", (unsigned)p->level);
  ui_text(6, 104, C_VAL, buf);
  ui_text(6 + (int)strlen(buf) * 8 + 4, 104, gender_col(p->gender), gender_sym(p->gender));

  char sp[24];
  ui_truncate(sp, pk_species_name(p->species), 11);
  ui_text(6, 114, C_KEY, sp);

  uint8_t t1 = pk_species_type1(p->species), t2 = pk_species_type2(p->species);
  type_badge(6, 126, t1);
  if (t2 != t1) type_badge(42, 126, t2);

  if (p->isBadEgg)   ui_text(6, 142, UI_WARN, "BAD EGG");
  else if (p->isEgg) ui_text(6, 142, C_HOT, "EGG");
  else if (p->pokerus) ui_text(6, 142, UI_WARN, "Pokerus");
}

static void card_info(const PkMon* p) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, "POKEMON INFO"); y += 12;

  if (p->isEgg && !p->isBadEgg) {        /* an egg stores its hatch counter in friendship */
    siprintf(b, "Hatch ~%u steps", (unsigned)(p->friendship * 256));
    ui_text(x, y, C_HOT, b); y += 10;
    siprintf(b, "%u egg cycle%s left", (unsigned)p->friendship, p->friendship == 1 ? "" : "s");
    ui_text(x, y, UI_DIM, b); y += 11;
  }

  ui_text(x, y, C_KEY, "Species"); reg(F_SPECIES, x + 48, y, 88);
  ui_text(x + 48, y, C_VAL, pk_species_name(p->species)); y += 9;

  ui_text(x, y, C_KEY, "Name"); reg(F_NICK, x + 48, y, 88);
  { char nm[24]; ui_truncate(nm, p->nickname[0] ? p->nickname : "-", 11); ui_text(x + 48, y, C_VAL, nm); } y += 9;

  ui_text(x, y, C_KEY, "OT"); reg(F_OT, x + 48, y, 88);
  ui_text(x + 48, y, C_VAL, p->otName); y += 9;               /* TID on its own row (was off-screen) */
  siprintf(b, "TID %05u", (unsigned)(p->otId & 0xFFFF)); ui_text(x, y, UI_DIM, b); y += 9;

  uint8_t t1 = pk_species_type1(p->species), t2 = pk_species_type2(p->species);
  ui_text(x, y, C_KEY, "Type");
  if (t1 == t2) type_badge(x + 46, y - 2, t1);
  else { type_badge(x + 46, y - 2, t1); type_badge(x + 80, y - 2, t2); }
  y += 16;

  uint16_t ab = pk_species_ability(p->species, p->abilityNum);
  ui_text(x, y, C_KEY, "Ability"); reg(F_ABILITY, x + 48, y, 88);
  ui_text(x + 48, y, C_VAL, pk_ability_name(ab)); y += 9;
  y = text_wrap(x + 4, y, 17, UI_DIM, pk_ability_desc(ab)); y += 1;

  ui_text(x, y, C_KEY, "Nature"); reg(F_NATURE, x + 48, y, 60);
  ui_text(x + 48, y, C_HOT, pk_nature_name(p->nature)); y += 9;

  ui_text(x, y, C_KEY, "Shiny"); reg(F_SHINY, x + 48, y, 26);
  ui_text(x + 48, y, p->isShiny ? UI_OK : C_VAL, p->isShiny ? "Yes" : "No");
  ui_text(x + 80, y, C_KEY, "Sex"); reg(F_GENDER, x + 108, y, 22);
  ui_text(x + 108, y, gender_col(p->gender), p->gender == 0 ? "M" : p->gender == 1 ? "F" : "-"); y += 10;

  siprintf(b, "Met Lv%u %s", (unsigned)p->metLevel, pk_location_name(p->metLocation));
  { char mb[40]; ui_truncate(mb, b, 17); ui_text(x, y, UI_DIM, mb); }
}

static void card_skills(const PkMon* p) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, "SKILLS"); y += 11;
  ui_text(x, y, C_KEY, "Level"); reg(F_LEVEL, x + 60, y, 40);
  siprintf(b, "%u", (unsigned)p->level); ui_text(x + 60, y, C_VAL, b); y += 9;
  ui_text(x, y, C_KEY, "Item"); reg(F_ITEM, x + 60, y, 76);
  ui_text(x + 60, y, C_VAL, p->heldItem ? pk_item_name(p->heldItem) : "none"); y += 9;
  ui_text(x, y, C_KEY, "Friend"); reg(F_FRIEND, x + 60, y, 40);
  siprintf(b, "%u", (unsigned)p->friendship); ui_text(x + 60, y, C_VAL, b); y += 10;
  /* Each stat row edits that stat's EV — the only persistent, lossless stat lever
   * in Gen-3 (final stats are derived from base+IV+EV+level+nature). The number
   * updates live; full IV/EV grids remain on the IV/EV cards. */
  for (int i = 0; i < 6; i++) {
    int s = DISP[i], bo = pk_nature_boost(p->nature), h = pk_nature_hinder(p->nature);
    u16 col = (s == bo) ? UI_OK : (s == h) ? UI_WARN : C_VAL;
    reg(F_EV0 + s, x, y, 138);
    siprintf(b, "%-3s", DSHORT[i]); ui_text(x, y, C_KEY, b);   /* 3-char label clears the value column */
    siprintf(b, "%4u", (unsigned)p->stats[s]); ui_text(x + 30, y, col, b);
    siprintf(b, "EV%u", (unsigned)p->evs[s]);  ui_text(x + 72, y, UI_DIM, b);
    y += 10;                                                   /* 10px stride: edit-frames don't share a scanline */
  }
  ui_text(x, y, UI_DIM, "<>: train EVs"); y += 9;

  /* EXP: current total + how much remains for the next level and for Lv100.
   * Box mons store EXP (Growth +4) even though level/stats are computed; the
   * curve comes from the species' growth-rate table (pk_exp_for_level). */
  ui_hline(x, y - 2, 138, UI_BORDER);
  uint8_t gr = pk_species_growth(p->species);
  uint32_t exp = p->experience;
  siprintf(b, "EXP %lu", (unsigned long)exp); ui_text(x, y, C_KEY, b); y += 9;
  if (p->level >= 100) {
    ui_text(x, y, C_VAL, "At Lv100 (max)");
  } else {
    uint32_t nxt = pk_exp_for_level(gr, (uint8_t)(p->level + 1));
    uint32_t t100 = pk_exp_for_level(gr, 100);
    siprintf(b, "Lv%u +%lu", (unsigned)(p->level + 1), (unsigned long)(nxt > exp ? nxt - exp : 0));
    ui_text(x, y, C_VAL, b); y += 9;
    siprintf(b, "Lv100 +%lu", (unsigned long)(t100 > exp ? t100 - exp : 0));
    ui_text(x, y, UI_DIM, b);
  }
}

static void card_spread(const PkMon* p, bool ev) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, ev ? "EVs" : "IVs"); y += 12;
  int total = 0, maxv = ev ? 255 : 31;
  for (int i = 0; i < 6; i++) {
    int s = DISP[i], v = ev ? p->evs[s] : p->ivs[s];
    total += v;
    reg(ev ? F_EV0 + s : F_IV0 + s, x, y, 138);
    ui_text(x, y, C_KEY, DSHORT[i]);
    siprintf(b, "%3d", v); ui_text(x + 26, y, C_VAL, b);
    int fill = v * 84 / maxv;
    u16 col = (!ev && v == 31) ? UI_OK : (ev && v == 252) ? UI_OK : C_HDR;
    ui_progress(x + 54, y + 1, 84, 5, fill, col, UI_PANEL, UI_BORDER);
    y += 12;
  }
  y += 2;
  ui_text(x, y, C_KEY, "TOTAL");
  siprintf(b, "%d / %d", total, ev ? 510 : 186);
  ui_text(x + 44, y, total > (ev ? 510 : 186) ? UI_WARN : C_HOT, b);
}

static void card_moves(const PkMon* p, bool contest) {
  int x = 98, y = 14; char b[48];
  int step = contest ? 18 : 22;          /* battle rows are taller to fit the type badge */
  ui_text(x, y, C_HDR, contest ? "CONTEST MOVES" : "BATTLE MOVES"); y += 12;
  for (int i = 0; i < 4; i++) {
    uint16_t mv = p->moves[i];
    reg(F_MV0 + i, x, y, 132);
    if (mv == 0) { ui_text(x, y, UI_DIM, "-"); y += step; continue; }
    char nm[24];
    ui_truncate(nm, pk_move_name(mv), contest ? 16 : 9);
    ui_text(x, y, C_VAL, nm);
    if (!contest) {
      uint8_t base = pk_move_pp(mv);
      uint8_t maxpp = (uint8_t)(base + base / 5 * ((p->ppBonuses >> (i * 2)) & 3));
      siprintf(b, "PP%u/%u", (unsigned)p->pp[i], (unsigned)maxpp);
      ui_text(x + 78, y, UI_DIM, b);
      type_badge(x + 4, y + 8, pk_move_type(mv));            /* real type badge under the name */
    } else {
      ui_text(x + 8, y + UI_ROW_H, C_HOT, pk_contest_name(pk_move_contest(mv)));
    }
    y += step;
  }
}

/* ORIGIN / MET card — the caught info: Poké Ball, met level + location, origin game
 * (all editable), plus read-only OT / TID / SID and a Pokérus tag. */
static void card_origin(const PkMon* p) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, "ORIGIN / MET"); y += 13;

  ui_text(x, y, C_KEY, "Ball"); reg(F_BALL, x + 60, y, 76);
  { char bl[24]; ui_truncate(bl, pk_item_name(p->pokeball), 10); ui_text(x + 60, y, C_VAL, bl); } y += 11;   /* 10 cols fit x158..238 */

  ui_text(x, y, C_KEY, "Met Lv"); reg(F_METLEVEL, x + 60, y, 30);            /* shortened: was "Met at Lv" (overlapped value) */
  siprintf(b, "%u", (unsigned)p->metLevel); ui_text(x + 60, y, C_VAL, b); y += 11;

  ui_text(x, y, C_KEY, "Loc"); reg(F_METLOC, x + 60, y, 76);                 /* shortened: was "Location" (overlapped value) */
  { char lb[24]; ui_truncate(lb, pk_location_name(p->metLocation), 10); ui_text(x + 60, y, C_VAL, lb); } y += 11;

  ui_text(x, y, C_KEY, "Origin"); reg(F_METGAME, x + 60, y, 76);
  ui_text(x + 60, y, C_HOT, pk_metgame_name(p->metGame)); y += 14;

  siprintf(b, "OT %s", p->otName); { char ob[40]; ui_truncate(ob, b, 17); ui_text(x, y, UI_DIM, ob); } y += 9;
  siprintf(b, "TID %05u SID %05u", (unsigned)(p->otId & 0xFFFF), (unsigned)(p->otId >> 16));
  { char tb[40]; ui_truncate(tb, b, 17); ui_text(x, y, UI_DIM, tb); } y += 9;
  if (p->pokerus) ui_text(x, y, UI_WARN, "Pokerus");
}

/* CONTEST CONDITION card — cool/beauty/cute/smart/tough (raised by Pokéblocks) + sheen
 * (how "full" the mon is = how many Pokéblocks it's been fed). All editable (0..255). */
static void card_condition(const PkMon* p) {
  static const char* const CTL[6] = { "Cool", "Beauty", "Cute", "Smart", "Tough", "Sheen" };
  int x = 98, y = 14; char b[16];
  ui_text(x, y, C_HDR, "CONDITION"); y += 12;
  for (int i = 0; i < 6; i++) {
    int v = p->contest[i];
    reg(F_CT0 + i, x, y, 138);
    ui_text(x, y, C_KEY, CTL[i]);
    siprintf(b, "%3d", v); ui_text(x + 44, y, C_VAL, b);
    int fill = v * 64 / 255;
    ui_progress(x + 72, y + 1, 64, 5, fill, i == 5 ? C_HOT : C_HDR, UI_PANEL, UI_BORDER);
    y += 12;
  }
  y += 2;
  ui_text(x, y, UI_DIM, "Sheen = Pokeblocks fed");
}

static void render_card(const PkMon* p, int card) {
  summary_bg();                             /* Emerald-style blue gradient backdrop */
  g_nslot = 0;
  if (g_edit) { ui_fill_rect(0, 0, 50, 9, UI_WARN); ui_text(8, 1, UI_PANEL, "EDIT"); } /* unmissable */
  else        ui_text(4, 2, UI_DIM, "VIEW");
  draw_dots(150, 2, NCARDS, card);
  ui_hline(0, 10, UI_SCR_W, UI_BORDER);
  draw_left(p);
  ui_hline(98, 24, 100, UI_TITLE);          /* header accent rule under each card title */
  switch (card) {
    case 0: card_info(p);          break;
    case 1: card_skills(p);        break;
    case 2: card_spread(p, false); break;
    case 3: card_spread(p, true);  break;
    case 4: card_moves(p, false);  break;
    case 5: card_moves(p, true);   break;
    case 6: card_origin(p);        break;
    case 7: card_condition(p);     break;
  }
}

static bool confirm(void) {
  ui_clear();
  ui_panel(16, 44, 208, 60, UI_PANEL, UI_WARN);
  ui_text(28, 52, UI_TITLE, "Save changes?");
  ui_text(28, 72, UI_TEXT, "A = write  (backup made first)");
  ui_text(28, 86, UI_DIM,  "B = discard");
  u16 k; do { s_vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  bool yes = (k & KEY_A) != 0;
  if (yes) snd_ok(); else snd_back();
  return yes;
}

/* ---- portrait animation (Emerald-style entrance bounce + gentle idle bob) ----
 * A single 64x64 frame re-blitted at a vertical offset — no second asset, no extra
 * EWRAM. Plays a one-shot rise+overshoot on open / mon-change, then a slow idle
 * bob. Gated on app_anim_enabled() by the caller. */

/* the portrait sprite draw_left would pick (front/back, else the icon fallback) */
static const uint16_t* portrait_sprite(const PkMon* p, bool* is_icon) {
  const uint16_t* spr = g_back ? mon_back_for_form(p->species, p->isShiny, p->form) : 0;
  if (!spr) spr = mon_front_for_form(p->species, p->isShiny, p->form);
  if (spr) { *is_icon = false; return spr; }
  *is_icon = true; return mon_icon_for_form(p->species, p->form);
}

/* Emerald-style intro: a squish-and-bounce of the single frame (vertical squash +
 * horizontal widen, anchored at the feet) that plays once, THEN a gentle idle
 * float. wx/sy are the drawn width/height in px (64 = natural); dy floats the rest
 * pose. This is the procedural affine intro the real Gen-3 summary uses (the front
 * "frames" are transforms of one sprite, not a flipbook). */
/* sine LUT scaled to +/-64 over a 16-step period */
static const signed char SIN16[16] = { 0, 24, 45, 59, 64, 59, 45, 24, 0, -24, -45, -59, -64, -59, -45, -24 };
static int isin(int i) { return SIN16[i & 15]; }

#define ENT 24                          /* entrance length (frames); after it, a gentle float */
/* The species' Emerald front-animation family as a procedural pose over frame t:
 * wx/sy = drawn width/height (64 = natural), dx/dy = pixel offset. Plays once, then
 * settles to a uniform idle float — matching the real game's intro-then-static. */
static void portrait_params(int fam, int t, int* wx, int* sy, int* dx, int* dy) {
  *wx = 64; *sy = 64; *dx = 0; *dy = 0;
  if (t >= ENT) { *dy = isin((t - ENT) >> 1) / 32; return; }      /* idle float ~ +/-2 px */
  int dk = (ENT - t) * 64 / ENT;                                   /* decay 64..~2 */
  switch (fam) {
    case 0: { int s = (isin(t * 2 + 12) * 12 * dk) >> 12; *sy = 64 + s; *wx = 64 - s / 2; } break;   /* squish & bounce */
    case 1: { int s = (isin(t * 2 + 12) * 14 * dk) >> 12; *sy = 64 - s; *wx = 64 + s / 2; } break;   /* stretch (tall first) */
    case 2: { *dy = (isin(t * 3) * 5 * dk) >> 12; } break;                                            /* v-shake */
    case 3: { *dx = (isin(t * 3) * 6 * dk) >> 12; } break;                                            /* h-shake */
    case 4: { int g = (dk * 18) >> 6; *wx = 64 - g; *sy = 64 - g; *dx = (isin(t * 3) * 3) >> 6; } break; /* grow + vibrate */
    case 5: { int s = (isin(t * 2) * 14 * dk) >> 12; *wx = 64 - s; *sy = 64 - s; } break;             /* shrink-grow pulse */
    case 6: { *dy = -((dk * 14) >> 6); } break;                                                       /* v-slide (down into place) */
    case 7: { *dx = -((dk * 16) >> 6); } break;                                                       /* h-slide in */
    case 8: { int h = (isin(t * 4) * 10 * dk) >> 12; *dy = -(h < 0 ? -h : h); } break;                /* jumps (hops up) */
    default:{ *dx = (isin(t * 2) * 8 * dk) >> 12; int s = (isin(t * 2 + 4) * 5 * dk) >> 12; *sy = 64 + s; } break; /* wobble (rotate approx) */
  }
}

/* Redraw the portrait at pose (wx,sy,dy) iff it changed. Composes each scanline of
 * the sprite sub-frame interior (x 12..79, y 14..77) — panel background plus the
 * 64x64 frame squashed to `sy` / widened to `wx`, anchored at the feet (y 78),
 * centred at x 46 — into a line buffer and DMAs it to VRAM in one pass. No separate
 * erase, so the animation never flickers (call site runs it in vblank). */
static u16 s_pline[68];
static void portrait_redraw(const PkMon* p, int wx, int sy, int dx, int dy, int* lastkey) {
  int key = (wx & 0xFF) | ((sy & 0xFF) << 8) | (((dx + 64) & 0xFF) << 16) | (((dy + 64) & 0xFF) << 24);
  if (key == *lastkey) return;
  *lastkey = key;
  bool icon; const uint16_t* spr = portrait_sprite(p, &icon);
  int x0, baseline, top, iw, ih;
  if (!icon) { x0 = 46 - wx / 2 + dx; baseline = 78 + dy; top = baseline - sy; iw = wx; ih = sy; }
  else       { x0 = 30 + dx; top = 30 + dy; if (top < 14) top = 14; if (top > 45) top = 45;
               baseline = top + 32; iw = 32; ih = 32; }
  for (int yy = 14; yy <= 77; yy++) {
    u16 bg = portrait_bg(yy);
    for (int dx = 0; dx < 68; dx++) {
      int x = 12 + dx; u16 c = bg;
      if (yy >= top && yy < baseline && x >= x0 && x < x0 + iw) {
        int sj = icon ? (yy - top) : ((yy - top) * 64 / ih);
        int si = icon ? (x - x0)   : ((x - x0)   * 64 / iw);
        u16 px = icon ? spr[sj * 32 + si] : spr[sj * 64 + si];
        if (px & 0x8000) c = (u16)(px & 0x7FFF);
      }
      s_pline[dx] = c;
    }
    dma3_cpy(&vid_mem[yy * 240 + 12], s_pline, 68 * 2);
  }
  if (p->isShiny) ui_text(70, 16, C_HOT, "*");
}

/* Inline summary with two sub-modes:
 *   VIEW  (default): A enters EDIT; U/D scroll to the prev/next mon (real-PC style);
 *                    L/R flip card; B leaves. The save prompt appears HERE — only
 *                    when you try to leave or change mon with unsaved edits.
 *   EDIT  (after A): U/D pick a field, A opens its picker, <> nudge, L/R flip card,
 *                    B returns to VIEW (edits stay pending). The EDIT banner shows.
 * Returns 0 to exit, +1 for "next mon", -1 for "prev mon" (the caller loads it and
 * calls again). *saved is set true (and out_rec filled) if the user kept the edits. */
int pdna_inspect(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec,
                   bool* saved, int* card_io) {
  if (saved) *saved = false;
  EditMon e;
  gen3_edit_load(rec, is_party, &e);
  PkMon cur;
  em_preview(&e, &cur); pk_resolve(&cur);

  int card = (card_io && *card_io >= 0 && *card_io < NCARDS) ? *card_io : 0;
  int fsel = 0;
  bool dirty = false, editing = false;
  int anim_t = 0, lastkey = -1;                /* portrait pose key (entrance anim + idle float) */
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);

  for (;;) {
    g_edit = editing;
    render_card(&cur, card);
    if (editing && g_nslot) {
      if (fsel >= g_nslot) fsel = g_nslot - 1;
      int sx = g_slot[fsel].x, sy = g_slot[fsel].y, sw = g_slot[fsel].w;
      m3_frame(sx - 2, sy - 1, sx + sw, sy + UI_ROW_H, UI_SELTEXT);
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, editing ? "A list  <> +/-  U/D field  L/R card  B view"
                          : can_edit ? "A edit  U/D mon  <>/LR card  SEL flip  B"
                                     : "U/D mon  <>/LR card  SEL flip  B back");
    lastkey = 64 | (64 << 8) | (64 << 16) | (64 << 24);   /* render_card drew the rest pose (64,64,0,0) */
    if (SUMMARY_ANIM && app_anim_enabled(ANIM_SUMMARY)) { int fam = mon_anim_family(cur.species), wx, sy, dx, dy;
                              portrait_params(fam, anim_t, &wx, &sy, &dx, &dy); portrait_redraw(&cur, wx, sy, dx, dy, &lastkey); }

    u16 k, fresh;
    do { s_vsync();
         if (SUMMARY_ANIM && app_anim_enabled(ANIM_SUMMARY)) { int fam = mon_anim_family(cur.species), wx, sy, dx, dy;
                                   portrait_params(fam, ++anim_t, &wx, &sy, &dx, &dy); portrait_redraw(&cur, wx, sy, dx, dy, &lastkey); }
         fresh = key_hit(KEY_FULL);
         k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
    if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
    else if (fresh & (KEY_L | KEY_R | KEY_SELECT)) snd_tab();
    else if (fresh & KEY_A)               snd_ok();
    else if (fresh & (KEY_LEFT | KEY_RIGHT)) { if (editing) snd_edit(); else snd_tab(); }
    else if (fresh & KEY_B)               snd_back();

    if (fresh & KEY_SELECT) { g_back = !g_back; continue; }   /* flip front/back portrait */

    if (editing) {
      /* ---- EDIT MODE ---- */
      if (k & KEY_B) editing = false;              /* back to VIEW, keep pending edits */
      else if (k & (KEY_L | KEY_R)) { card = (card + (k & KEY_R ? 1 : NCARDS - 1)) % NCARDS; fsel = 0; }
      else if (g_nslot && (k & KEY_A))    { em_field_press(g_slot[fsel].field, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true; }
      else if (g_nslot && (k & KEY_LEFT)) { em_field_adjust(g_slot[fsel].field, -1, false, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true; }
      else if (g_nslot && (k & KEY_RIGHT)){ em_field_adjust(g_slot[fsel].field, +1, false, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true; }
      else if (k & KEY_UP)   { if (g_nslot) fsel = (fsel > 0) ? fsel - 1 : g_nslot - 1; }
      else if (k & KEY_DOWN) { if (g_nslot) fsel = (fsel + 1) % g_nslot; }
    } else {
      /* ---- VIEW MODE ---- */
      if (k & KEY_A) { if (can_edit) editing = true; }
      else if ((k & (KEY_L | KEY_R)) || (fresh & (KEY_LEFT | KEY_RIGHT))) {   /* L/R shoulder OR d-pad LEFT/RIGHT flip cards */
        int fwd = (k & KEY_R) || (fresh & KEY_RIGHT);
        card = (card + (fwd ? 1 : NCARDS - 1)) % NCARDS; fsel = 0;
      }
      else if (k & (KEY_UP | KEY_DOWN | KEY_B)) {    /* leaving this mon: prompt-save if dirty */
        if (dirty && confirm()) { gen3_edit_commit(&e, out_rec); if (saved) *saved = true; }
        key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
        if (card_io) *card_io = card;                /* keep the card sticky across mon-scroll */
        if (k & KEY_B) return 0;
        return (k & KEY_DOWN) ? +1 : -1;
      }
    }
  }
}
