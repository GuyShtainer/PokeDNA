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
#include "pdna_origin_art.h"   /* draw the mon in the art of the generation it came FROM */
#include "mon_anim.h"     /* per-species Emerald front-animation family */
#include "type_icons.h"
#include "snd.h"
#include "rumble.h"       /* rumble_io_suspend/resume around the ROM-read portrait LZ77 */
#include "gen3_ivroll.h"  /* the IV reroll: one shared roller, an undo/redo list */
#include "pdna_layout.h"  /* the reroll row's geometry + every string it draws */

#define NCARDS 8

/* Summary portrait animation (the Emerald entrance/idle wiggle). Compiled in, but
 * gated at runtime on the ANIM_SUMMARY toggle (Settings > Animations > Summary),
 * which defaults OFF — the static portrait from draw_left stands in until the user
 * opts in. Set to 0 to compile it out entirely. */
#define SUMMARY_ANIM 1

/* real Gen-3 type badge (32x14); ui_sprite honours the 0x8000 opacity bit.
 * Art-free build: the original coloured type chip stands in. */
static void type_badge(int x, int y, uint8_t t) {
  if (t >= 18) return;
  const uint16_t* ic = type_icon_for(t);
  if (ic) ui_sprite(x, y, TYPE_ICON_W, TYPE_ICON_H, ic);
  else    ui_type_chip(x, y, TYPE_ICON_W, TYPE_ICON_H, t);
}

static const int   DISP[6]   = { PK_HP, PK_ATK, PK_DEF, PK_SPA, PK_SPD, PK_SPE };
static const char* DSHORT[6] = { "HP", "Atk", "Def", "SpA", "SpD", "Spd" };

#define C_HDR  UI_TITLE
#define C_KEY  UI_DIRCLR
#define C_VAL  UI_TEXT
#define C_HOT  UI_WARN

/* The reroll's entropy: this counts vblanks, so the seed depends on WHEN A was pressed —
 * the only unpredictable thing a GBA has. gba_rtc_get is second-resolution and simply
 * absent with the RTC switched off, so it cannot be the source. */
static uint32_t s_ticks = 0;
static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); s_ticks++; }

/* SELECT flips the portrait between front and back sprite (issue #12); persists
 * across mon-scroll within a summary session. */
static bool g_back = false;

/* ---- editable-field slot registry (filled during render in edit mode) ---- */
static int g_edit = 0, g_nslot = 0;
static int g_create = 0;          /* CREATE mode: render the NEW chip, not VIEW/EDIT */
static struct { int field, x, y, w; } g_slot[24];
static void reg(int field, int x, int y, int w) {
  if (g_edit && g_nslot < 24) { g_slot[g_nslot].field = field; g_slot[g_nslot].x = x;
                                g_slot[g_nslot].y = y; g_slot[g_nslot].w = w; g_nslot++; }
}

/* F_SUM_REROLL is a SUMMARY-LOCAL pseudo-field, one past pdna_edit.h's F_NUM and
 * deliberately NOT an entry in that enum: the field-list editor walks 0..F_NUM-1 and indexes
 * FLABEL[] by the same id (pdna_edit.c:117-126), so a real field id would put a dead
 * "Reroll" row in a screen with no history to step and no confirm panel. It rides the slot
 * registry purely to inherit focus, the highlight frame and U/D navigation; summary_run
 * intercepts it before em_field_press/em_field_adjust ever see it.
 * LIFETIME = ONE POKEMON: ivh_reset runs at the top of summary_run, and every mon change
 * RETURNS from summary_run for the caller to call again (pdna_main.c:944, :968, :3238,
 * :3346), so a redo entry can never be applied to a different mon. ivh_sync, once per
 * repaint, is the other half of that promise. */
#define F_SUM_REROLL (F_NUM + 0)
_Static_assert(F_SUM_REROLL >= F_NUM, "summary pseudo-fields must sit past the real enum");
static IvHistory g_ivh;
static IvRollState g_roll_st;      /* seed survives across mons: never reset it */
static bool      g_last_ivonly = false;   /* the newest roll kept the PID */
static bool      g_have_roll   = false;

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
  rumble_io_suspend();   /* portrait fetch decompresses from ROM (Gen-3 LZ77 or GB pic) */
  /* One accessor decides the era: a mon converted from a Game Boy save wears its OWN
   * generation's art when that cartridge is registered, and everything else takes the
   * front/back ladder this used to open-code. With no GB ROM registered the result is
   * the same pointer and the same (14,14) 64x64 blit as before. */
  PdnaArt art; PdnaOrigin org;
  pdna_origin_art_portrait(p, g_back, &art, &org);
  rumble_io_resume();
  if (art.px) {
    int ax, ay;
    pdna_origin_art_place(&art, 12, 14, 68, 64, &ax, &ay);   /* 64x64 -> (14,14) exactly */
    ui_sprite(ax, ay, art.w, art.h, art.px);
  } else if (art.egg) {
    ui_sprite(30, 30, MON_ICON_W, MON_ICON_H, mon_icon_egg());
  } else {
    ui_sprite(30, 30, MON_ICON_W, MON_ICON_H, mon_icon_for_form(p->species, p->form));
  }
  if (p->isShiny) ui_text(70, 16, C_HOT, "*");             /* gold shiny mark on the portrait */
  /* Provenance stamp — drawn ONLY for a GB import, so a native Gen-3 mon's portrait is
   * pixel-identical to before. Opaque chip rather than bare text so it stays readable
   * over whatever the sprite puts behind it. "GB1?" = Gen 1 by elimination, i.e. the
   * record cannot prove it is not a Gen-2 Kanto mon (pdna_origin_art.h). */
  if (org.verdict == PDNA_ORIGIN_GB)
    ui_name_chip(13, 15, 30, 11, pdna_origin_color(&org), 0x7FFF, pdna_origin_tag(&org));

  char buf[40];
  ui_hline(4, 81, 84, UI_BORDER);
  siprintf(buf, "#%03u", (unsigned)pk_national_no(p->species));
  ui_text(6, 84, C_KEY, buf);
  if (p->species == 410) {                                 /* Deoxys (internal 410): show the current forme (SELECT cycles it) */
    static const char* const DF[4] = { "Normal", "Attack", "Defense", "Speed" };
    ui_text(40, 84, C_HOT, DF[p->form < 4 ? p->form : 0]);
  } else if (g_back) ui_text(44, 84, UI_DIM, "back");      /* current portrait side */

  ui_ptext_fit(6, 94, 86, C_VAL, p->nickname[0] ? p->nickname : pk_species_name(p->species));

  siprintf(buf, "Lv%u", (unsigned)p->level);
  ui_text(6, 104, C_VAL, buf);
  ui_text(6 + (int)strlen(buf) * 8 + 4, 104, gender_col(p->gender), gender_sym(p->gender));

  ui_ptext_fit(6, 114, 86, C_KEY, pk_species_name(p->species));

  uint8_t t1 = pk_species_type1(p->species), t2 = pk_species_type2(p->species);
  type_badge(6, 126, t1);
  if (t2 != t1) type_badge(42, 126, t2);

  if (p->isBadEgg)   ui_text(6, 142, UI_WARN, "BAD EGG");
  else if (p->isEgg) ui_text(6, 142, C_HOT, "EGG");
  else if (p->pokerus) ui_text(6, 142, UI_WARN, "Pokerus");
}

/* Right-hand card column: x=98, so 138 px to the screen edge with a 4 px margin.
 * Everything here is drawn with the proportional face — at 8 px/glyph the column was
 * 17 characters, which cut "Hatch ~10240 ste~" and "40 egg cycles le~" mid-word. */
#define INFO_W 138

static void card_info(const PkMon* p) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, "POKEMON INFO"); y += 12;

  if (p->isEgg && !p->isBadEgg) {        /* an egg stores its hatch counter in friendship */
    siprintf(b, "~%u steps to hatch", (unsigned)(p->friendship * 256));
    ui_ptext_fit(x, y, INFO_W, C_HOT, b); y += 10;
    siprintf(b, "%u egg cycle%s left", (unsigned)p->friendship, p->friendship == 1 ? "" : "s");
    ui_ptext_fit(x, y, INFO_W, UI_DIM, b); y += 11;
  }

  ui_text(x, y, C_KEY, "Spec."); reg(F_SPECIES, x + 48, y, 88);
  ui_text(x + 48, y, C_VAL, pk_species_name(p->species)); y += 9;

  ui_text(x, y, C_KEY, "Name"); reg(F_NICK, x + 48, y, 88);
  ui_ptext_fit(x + 48, y, INFO_W - 48, C_VAL, p->nickname[0] ? p->nickname : "-"); y += 9;

  ui_text(x, y, C_KEY, "OT"); reg(F_OT, x + 48, y, 88);
  ui_text(x + 48, y, C_VAL, p->otName); y += 9;               /* TID on its own row (was off-screen) */
  siprintf(b, "TID %05u", (unsigned)(p->otId & 0xFFFF)); ui_text(x, y, UI_DIM, b); y += 9;

  uint8_t t1 = pk_species_type1(p->species), t2 = pk_species_type2(p->species);
  ui_text(x, y, C_KEY, "Type");
  if (t1 == t2) type_badge(x + 46, y - 2, t1);
  else { type_badge(x + 46, y - 2, t1); type_badge(x + 80, y - 2, t2); }
  y += 16;

  uint16_t ab = pk_species_ability(p->species, p->abilityNum);
  ui_text(x, y, C_KEY, "Abil."); reg(F_ABILITY, x + 48, y, 88);
  ui_ptext_fit(x + 48, y, INFO_W - 48, C_VAL, pk_ability_name(ab)); y += 9;
  y += UI_ROW_H * ui_ptext_wrap(x + 4, y, INFO_W - 4, UI_ROW_H, 0, UI_DIM, app_ability_desc(ab));
  y += 1;

  ui_text(x, y, C_KEY, "Nat."); reg(F_NATURE, x + 48, y, 60);
  ui_text(x + 48, y, C_HOT, pk_nature_name(p->nature)); y += 9;

  ui_text(x, y, C_KEY, "Shiny"); reg(F_SHINY, x + 48, y, 26);
  ui_text(x + 48, y, p->isShiny ? UI_OK : C_VAL, p->isShiny ? "Yes" : "No");
  ui_text(x + 80, y, C_KEY, "Sex"); reg(F_GENDER, x + 108, y, 22);
  ui_text(x + 108, y, gender_col(p->gender), p->gender == 0 ? "M" : p->gender == 1 ? "F" : "-"); y += 10;

  /* The footer lives at y=152 and `y` is a running cursor: the egg rows above push it down
   * 21 px, which is how this row once landed ON TOP of the footer. It was then dropped
   * entirely when it collided — leaving an obviously empty line, which Guy asked to use.
   * So clamp the POSITION instead of skipping the row: at y=142 it ends at 150, two pixels
   * clear of the footer, and the proportional face fits the whole location name. */
  if (y > 142) y = 142;
  /* Met level 0 is not a missing value: it is how Gen 3 records "this came out of an egg",
   * and retail prints a hatch sentence rather than a level — the decomp's own comment at
   * src/egg_hatch.c:384-386 reads "A met level of 0 is interpreted on the summary screen as
   * 'hatched at'". Rendering the raw 0 is what made a created L32 Venusaur look broken to
   * Guy ("met at lvl 0"); the byte was right, the word was missing. An unhatched egg keeps
   * the numeric row — it has not been met yet at all.
   * "Hatched " is 44 px against INFO_W = 138; "Hatched at " is 58 and would push
   * LITTLEROOT TOWN to 143 and PETALBURG WOODS to 145, i.e. it would newly truncate names
   * that fit today. Drop the "at". */
  if (p->metLevel == 0 && !p->isEgg)
    siprintf(b, "Hatched %s", pk_location_name(p->metLocation));
  else
    siprintf(b, "Met Lv%u %s", (unsigned)p->metLevel, pk_location_name(p->metLocation));
  ui_ptext_fit(x, y, INFO_W, UI_DIM, b);
}

static void card_skills(const PkMon* p) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, "SKILLS"); y += 11;
  ui_text(x, y, C_KEY, "Level"); reg(F_LEVEL, x + 60, y, 40);
  siprintf(b, "%u", (unsigned)p->level); ui_text(x + 60, y, C_VAL, b); y += 9;
  ui_text(x, y, C_KEY, "Item"); reg(F_ITEM, x + 60, y, 76);
  { char it_[48];
    if (p->heldItem) pk_item_label(p->heldItem, it_, sizeof it_); else strcpy(it_, "none");
    ui_ptext_fit(x + 60, y, 238 - (x + 60), C_VAL, it_); } y += 9;
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

/* The reroll button, the two history arrows Guy asked for, and a note row saying where in
 * the history you are. Drawn dimmed in VIEW mode too, so the feature is discoverable. */
static void draw_reroll_row(const PkMon* p) {
  const int x = PDNA_SUM_CARD_X, y = PDNA_SUM_ROLL_Y, w = PDNA_SUM_CARD_W;
  bool armed = g_edit && !p->isBadEgg;
  if (armed) reg(F_SUM_REROLL, x, y, w);
  ui_panel(x - 2, y + PDNA_SUM_ROLL_BOX_DY, w + 2, PDNA_SUM_ROLL_BOX_H, UI_PANEL, UI_BORDER);
  int lw = (int)strlen(PDNA_SUM_ROLL_LBL) * UI_SYS8_W;
  ui_text(x + (w - lw) / 2, y, armed ? C_VAL : UI_DIM, PDNA_SUM_ROLL_LBL);
  /* Lit only when there is somewhere to step, so the arrows tell the truth about the ends
   * of the list rather than inviting a press that does nothing. */
  ui_text(x, y, (armed && g_ivh.cur > 0) ? C_HOT : UI_DIM, PDNA_SUM_ARROW_L);
  ui_text(x + w - UI_SYS8_W, y,
          (armed && g_ivh.cur + 1 < g_ivh.n) ? C_HOT : UI_DIM, PDNA_SUM_ARROW_R);
  char b[40];
  if (p->isBadEgg)       strcpy(b, PDNA_SUM_NOTE_BADEGG);
  else if (!g_edit)      strcpy(b, PDNA_SUM_NOTE_VIEW);
  else if (g_ivh.n <= 1) strcpy(b, PDNA_SUM_NOTE_IDLE);
  else {
    const char* tail = (g_ivh.cur == 0) ? PDNA_SUM_TAIL_ORIG
                     : (g_have_roll && g_ivh.cur + 1 == g_ivh.n && g_last_ivonly)
                       ? PDNA_SUM_TAIL_IVONLY : PDNA_SUM_TAIL_PIDIV;
    siprintf(b, PDNA_SUM_NOTE_FMT, g_ivh.cur + 1, g_ivh.n, tail);
  }
  ui_ptext_fit(x, PDNA_SUM_NOTE_Y, w, UI_DIM, b);
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
  if (!ev) draw_reroll_row(p);   /* the IV spread gets the reroll; the EV card has no RNG
                                  * story to tell */
}

static void card_moves(const PkMon* p, bool contest) {
  int x = 98, y = 14; char b[48];
  int step = contest ? 18 : 22;          /* battle rows are taller to fit the type badge */
  ui_text(x, y, C_HDR, contest ? "CONTEST MOVES" : "BATTLE MOVES"); y += 12;
  for (int i = 0; i < 4; i++) {
    uint16_t mv = p->moves[i];
    reg(F_MV0 + i, x, y, contest ? 132 : 74);
    if (mv == 0) { ui_text(x, y, UI_DIM, "-"); y += step; continue; }
    ui_ptext_fit(x, y, contest ? 132 : 74, C_VAL, pk_move_name(mv));
    if (!contest) {
      uint8_t base = pk_move_pp(mv);
      uint8_t maxpp = (uint8_t)(base + base / 5 * ((p->ppBonuses >> (i * 2)) & 3));
      reg(F_PP0 + i, x + 76, y, 60);                         /* editable current PP */
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
  ui_ptext_fit(x + 60, y, 238 - (x + 60), C_VAL, pk_item_name(p->pokeball)); y += 11;

  ui_text(x, y, C_KEY, "Met Lv"); reg(F_METLEVEL, x + 60, y, 30);            /* shortened: was "Met at Lv" (overlapped value) */
  siprintf(b, "%u", (unsigned)p->metLevel); ui_text(x + 60, y, C_VAL, b);
  /* The number stays (F_METLEVEL edits it), but 0 gets its meaning next to it. 41 px of
   * proportional text at x+92 ends at 231, inside the 238 margin, and 2 px clear of the
   * F_METLEVEL selection frame which ends at 188. */
  if (p->metLevel == 0 && !p->isEgg) ui_ptext(x + 92, y, UI_OK, "hatched");
  y += 11;

  ui_text(x, y, C_KEY, "Loc"); reg(F_METLOC, x + 60, y, 76);                 /* shortened: was "Location" (overlapped value) */
  ui_ptext_fit(x + 60, y, 238 - (x + 60), C_VAL, pk_location_name(p->metLocation)); y += 11;

  ui_text(x, y, C_KEY, "Origin"); reg(F_METGAME, x + 60, y, 76);
  ui_text(x + 60, y, C_HOT, pk_metgame_name(p->metGame)); y += 14;

  siprintf(b, "OT %s", p->otName); ui_ptext_fit(x, y, 238 - x, UI_DIM, b); y += 9;
  /* One row cannot hold both: "TID 38800 SID 15243" is 19 glyphs and the column is 17, so
   * truncating it dropped three of the five SID digits without saying so. */
  siprintf(b, "TID %05u", (unsigned)(p->otId & 0xFFFF));
  ui_text(x, y, UI_DIM, b); y += 9;
  siprintf(b, "SID %05u", (unsigned)(p->otId >> 16));
  ui_text(x, y, UI_DIM, b); y += 9;
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
    siprintf(b, "%3d", v); ui_text(x + 52, y, C_VAL, b);   /* +44 collided with "Beauty" */
    int fill = v * 64 / 255;
    ui_progress(x + 80, y + 1, 58, 5, fill, i == 5 ? C_HOT : C_HDR, UI_PANEL, UI_BORDER);
    y += 12;
  }
  y += 2;
  ui_text(x, y, UI_DIM, "Sheen: blocks fed");   /* 17 cols; the full phrase wrapped onto the portrait */
}

static void render_card(const PkMon* p, int card) {
  summary_bg();                             /* Emerald-style blue gradient backdrop */
  g_nslot = 0;
  if (g_edit)        { ui_fill_rect(0, 0, 50, 9, UI_WARN); ui_text(8, 1, UI_PANEL, "EDIT"); } /* unmissable */
  else if (g_create) { ui_fill_rect(0, 0, 50, 9, UI_OK);   ui_text(12, 1, UI_PANEL, "NEW"); } /* not saved yet */
  else               ui_text(4, 2, UI_DIM, "VIEW");
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

static bool confirm_q(const char* title, const char* a_line, const char* b_line) {
  ui_clear();
  ui_panel(16, 44, 208, 60, UI_PANEL, UI_WARN);
  ui_text(28, 52, UI_TITLE, title);
  ui_text(28, 72, UI_TEXT, a_line);
  ui_text(28, 86, UI_DIM,  b_line);
  u16 k; do { s_vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  bool yes = (k & KEY_A) != 0;
  if (yes) snd_ok(); else snd_back();
  return yes;
}

/* NOTE the A-line length. It used to read "A = write  (backup made first)": 30 glyphs
 * at sys8's fixed 8 px = 240 px drawn at x=28, i.e. 44 px past the panel's right edge
 * (16+208=224) and 28 px off the 240 px screen, so the tail wrapped to the far left as
 * loose "rst)". tests/host_textfit_test.c now pins every string on this panel. */
static bool confirm(void) {
  return confirm_q("Save changes?", "A = write (backup first)", "B = discard");
}

/* CREATE mode's keep/discard prompt. Deliberately worded so "discard" reads as
 * "the slot stays empty", not "your edits are lost". */
static bool confirm_keep(void) {
  return confirm_q("Keep this Pokemon?", "A = write (backup first)", "B = discard it");
}

static const char UNOWN_CH[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!?";

/* THE CONFIRM PANEL. For a caught Pokemon the IVs are not free: new IVs mean a new PID, and
 * the PID IS the nature, sex, ability slot, shininess and Unown letter. The search keeps
 * every one of them where it can (measured: never had to drop one over a 2,000-Pokemon
 * sweep); this panel is for the case it could not — a SHINY, where keeping the sparkle costs
 * ~8,192x more seeds than the budget can also spend on the nature — and for the one case
 * NOTHING can keep, an NPC trade, whose PID is a constant in the trade template rather than
 * RNG output. B cancels with NOTHING written and the history untouched. Never shown on the
 * PID-exempt path. Drawn at x=96 so the portrait at x=12..79 stays visible, which is what
 * its old comment claimed and its old geometry (x=16, w=208) did not do. */
static bool reroll_confirm(const IvRoll* r) {
  char ln[PDNA_SUM_RC_LINES][32];
  int n = 0;
  /* FIRST, because it is the only line that is about legality rather than looks — and the
   * only one no search could have avoided. */
  if (r->chg_trade && n < PDNA_SUM_RC_LINES)
    strcpy(ln[n++], PDNA_SUM_RC_TRADE);
  if (r->chg_shiny && n < PDNA_SUM_RC_LINES)
    strcpy(ln[n++], r->new_shiny ? PDNA_SUM_RC_SHINY_ON : PDNA_SUM_RC_SHINY_OFF);
  if (r->chg_nature && n < PDNA_SUM_RC_LINES)
    siprintf(ln[n++], PDNA_SUM_RC_NAT_FMT, pk_nature_name(r->old_nature),
                                           pk_nature_name(r->new_nature));
  if (r->chg_gender && n < PDNA_SUM_RC_LINES)
    siprintf(ln[n++], PDNA_SUM_RC_SEX_FMT, gender_sym(r->old_gender), gender_sym(r->new_gender));
  if (r->chg_ability && n < PDNA_SUM_RC_LINES)
    siprintf(ln[n++], PDNA_SUM_RC_ABI_FMT, (unsigned)(r->old_ability + 1),
                                           (unsigned)(r->new_ability + 1));
  if (r->chg_form && n < PDNA_SUM_RC_LINES)
    siprintf(ln[n++], PDNA_SUM_RC_UNO_FMT, UNOWN_CH[r->old_form % 28], UNOWN_CH[r->new_form % 28]);
  if (n == 0) return true;                        /* everything survived: no question */
  int h  = PDNA_SUM_RC_HEAD + n * PDNA_SUM_RC_ROW_H + PDNA_SUM_RC_FOOT;
  int tx = PDNA_SUM_RC_X + PDNA_SUM_RC_PAD;
  ui_panel(PDNA_SUM_RC_X, PDNA_SUM_RC_Y, PDNA_SUM_RC_W, h, UI_PANEL, UI_WARN);
  ui_text(tx, PDNA_SUM_RC_Y + 6, UI_TITLE, PDNA_SUM_RC_TITLE);
  /* PROPORTIONAL, not sys8: "Nat ADAMANT>ADAMANT" is 19 glyphs, and at sys8's fixed 8 px
   * cell that is 152 px against a 120 px text column — it would have lost the second
   * nature entirely, on the one line whose whole job is to name what changed.
   * tests/host_textfit_test.c pins every line on this panel at its worst case. */
  for (int i = 0; i < n; i++)
    ui_ptext_fit(tx, PDNA_SUM_RC_Y + PDNA_SUM_RC_HEAD + i * PDNA_SUM_RC_ROW_H,
                 PDNA_SUM_RC_TEXT_W, UI_WARN, ln[i]);
  ui_text(tx, PDNA_SUM_RC_Y + h - 14, UI_DIM, PDNA_SUM_RC_HINT);
  u16 k; do { s_vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  if (k & KEY_A) { snd_ok(); return true; }
  snd_back();
  return false;
}

/* A on the reroll row. CHUNKED: 8,000 candidates a frame with an s_vsync() between slices,
 * so sound, rumble and the frame counter keep running. A monolithic search would block
 * s_vsync for ~0.5-1.0 s on a shiny — and s_vsync is the sole caller of snd_vblank(), which
 * is the sole caller of rmbl_vblank(), which is the only thing that ENDS a rumble cue, so
 * the motor would stay energised for the whole search on Guy's Omega. */
static bool do_reroll(EditMon* e, PkMon* cur) {
  if (cur->isBadEgg) { snd_back(); return false; }
  if (!g_roll_st.seed)
    g_roll_st.seed = ((uint32_t)s_ticks << 11) ^ ((uint32_t)REG_VCOUNT << 3)
                   ^ e->personality ^ e->otId ^ 0x9E3779B9u;
  g_roll_st.rung = 0; g_roll_st.started = 0;
  IvRoll r; int rc;
  ui_name_chip(PDNA_SUM_CARD_X, PDNA_SUM_NOTE_Y - 1, 60, 10, UI_WARN, 0x7FFF, PDNA_SUM_ROLLING);
  while ((rc = iv_roll_step(e, cur, &g_roll_st, 8000u, &r)) == 0) s_vsync();
  if (rc < 0) { snd_back(); return false; }
  if (r.pid_locked && !reroll_confirm(&r)) return false;   /* cancelled: nothing applied */
  iv_roll_apply(e, &r);
  em_preview(e, cur); pk_resolve(cur);
  ivh_push(&g_ivh, e);
  g_last_ivonly = !r.pid_locked; g_have_roll = true;
  return true;                     /* the A-press earcon at line 602 already fired */
}

static bool do_history(EditMon* e, PkMon* cur, int dir) {
  if (!ivh_step(&g_ivh, e, dir)) { snd_back(); return false; }
  em_preview(e, cur); pk_resolve(cur);
  g_have_roll = false;             /* the note now describes a roll we have stepped off */
  snd_edit();
  return true;
}

/* ---- portrait animation (Emerald-style entrance bounce + gentle idle bob) ----
 * A single 64x64 frame re-blitted at a vertical offset — no second asset, no extra
 * EWRAM. Plays a one-shot rise+overshoot on open / mon-change, then a slow idle
 * bob. Gated on app_anim_enabled() by the caller. */

/* The portrait sprite draw_left would pick, through the SAME era router — so the
 * animated portrait and the static one can never disagree about which generation's
 * art a mon wears. Also reports the source pixel size (*sw x *sh): Gen-3 art is 64x64,
 * a Game Boy pic is not, and the pose math below scales relative to it. */
static const uint16_t* portrait_sprite(const PkMon* p, bool* is_icon, int* sw, int* sh) {
  rumble_io_suspend();   /* portrait fetch decompresses from ROM */
  PdnaArt art;
  pdna_origin_art_portrait(p, g_back, &art, 0);
  rumble_io_resume();
  if (art.px) { *is_icon = false; *sw = art.w; *sh = art.h; return art.px; }
  *is_icon = true; *sw = MON_ICON_W; *sh = MON_ICON_H;
  return art.egg ? mon_icon_egg() : mon_icon_for_form(p->species, p->form);
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
static void portrait_redraw(const PkMon* p, const uint16_t* spr, bool icon, int sw, int sh,
                            int wx, int sy, int dx, int dy, int* lastkey) {
  int key = (wx & 0xFF) | ((sy & 0xFF) << 8) | (((dx + 64) & 0xFF) << 16) | (((dy + 64) & 0xFF) << 24);
  if (key == *lastkey) return;
  *lastkey = key;
  /* spr/icon/sw/sh are supplied by the caller (fetched once per repaint) — see the
   * hoist comment in summary_run. This function must not fetch: it runs per frame. */
  /* Art-free build: no sprite exists at all. Without this check the loop below
   * sampled address 0 (open bus) and painted garbage stripes into the portrait —
   * paint the plain gradient instead (which also erases any previous pose). */
  int x0, baseline, top, iw, ih;
  if (!spr) { x0 = 0; baseline = -1; top = -1; iw = 0; ih = 1; }
  /* wx/sy come out of portrait_params in units where 64 == natural, so scaling them
   * by the source size makes a 56x56 Game Boy pic play the same pose at ITS natural
   * size instead of being stretched to 64. For 64x64 art this is the identity and the
   * arithmetic is literally what it was before. */
  else if (!icon) { iw = wx * sw / MON_FRONT_W; ih = sy * sh / MON_FRONT_H;
                    if (iw < 1) iw = 1;         /* never divide by zero at an extreme pose */
                    if (ih < 1) ih = 1;
                    x0 = 46 - iw / 2 + dx; baseline = 78 + dy; top = baseline - ih; }
  else       { x0 = 30 + dx; top = 30 + dy; if (top < 14) top = 14; if (top > 45) top = 45;
               baseline = top + 32; iw = 32; ih = 32; }
  /* The source column for a destination x depends only on x, x0, sw and iw — none of
   * which change down the frame — yet it used to be recomputed inside the innermost
   * loop. That is two software divisions per destination pixel (the ARM7TDMI has no
   * divide instruction, so each is a ~20-40 cycle __aeabi_idiv call) across 68x64
   * pixels: ~8,000 calls per pose change, and the pose changes nearly every frame
   * while the portrait is animating. Hoisting is enough — build the column table once
   * per redraw and lift the row term out of the x loop, and the count drops to 68 + 64.
   *
   * Deliberately NOT a fixed-point DDA: the expression below is character-for-character
   * the one that shipped, so the rendering cannot shift by a pixel at a rounding
   * boundary. Same pixels, ~60x fewer divisions. */
  int16_t sicol[68];
  if (spr && iw > 0) {
    for (int dx = 0; dx < 68; dx++) {
      int x = 12 + dx;
      int k = x - x0;
      sicol[dx] = (int16_t)(icon ? k : (k * sw / iw));
    }
  }
  for (int yy = 14; yy <= 77; yy++) {
    u16 bg = portrait_bg(yy);
    const uint16_t* srow = 0;
    if (spr && yy >= top && yy < baseline) {
      int sj = icon ? (yy - top) : ((yy - top) * sh / ih);
      srow = spr + sj * sw;
    }
    for (int dx = 0; dx < 68; dx++) {
      int x = 12 + dx; u16 c = bg;
      if (srow && x >= x0 && x < x0 + iw) {
        u16 px = srow[sicol[dx]];   /* icon: sw == 32, exactly the old expression */
        if (px & 0x8000) c = (u16)(px & 0x7FFF);
      }
      s_pline[dx] = c;
    }
    dma3_cpy(&vid_mem[yy * 240 + 12], s_pline, 68 * 2);
  }
  if (p->isShiny) ui_text(70, 16, C_HOT, "*");
  /* The loop above repaints the WHOLE frame interior, which erases anything draw_left
   * put there — the shiny star (restored just above) and the provenance stamp. Redraw
   * it here or the tag blinks out the moment the portrait animation starts. */
  PdnaOrigin org;
  pdna_origin_of(p, &org);
  if (org.verdict == PDNA_ORIGIN_GB)
    ui_name_chip(13, 15, 30, 11, pdna_origin_color(&org), 0x7FFF, pdna_origin_tag(&org));
}

/* Inline summary with two sub-modes:
 *   VIEW  (default): A enters EDIT; U/D scroll to the prev/next mon (real-PC style);
 *                    L/R flip card; B leaves. The save prompt appears HERE — only
 *                    when you try to leave or change mon with unsaved edits.
 *   EDIT  (after A): U/D pick a field, A opens its picker, <> nudge, L/R flip card,
 *                    B returns to VIEW (edits stay pending). The EDIT banner shows.
 * Returns 0 to exit, +1 for "next mon", -1 for "prev mon" (the caller loads it and
 * calls again). *saved is set true (and out_rec filled) if the user kept the edits. */
static int summary_run(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec,
                       bool* saved, int* card_io, bool create) {
  if (saved) *saved = false;
  EditMon e;
  gen3_edit_load(rec, is_party, &e);
  PkMon cur;
  em_preview(&e, &cur); pk_resolve(&cur);
  ivh_reset(&g_ivh, &e); g_have_roll = false;   /* the history belongs to THIS Pokemon */

  int card = (card_io && *card_io >= 0 && *card_io < NCARDS) ? *card_io : 0;
  int fsel = 0;
  bool dirty = false, editing = false;
  int anim_t = 0, lastkey = -1;                /* portrait pose key (entrance anim + idle float) */
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
  /* g_back is file-static and survives between calls: a create opened after someone
   * flipped a previous mon to its back sprite would otherwise open on the back. */
  if (create) g_back = false;

  for (;;) {
    g_edit = editing;
    g_create = create && !editing;
    /* Keep the history honest: anything that moved the PID or the IV word behind its back —
     * a hand-edited IV row, a nature change on the INFO card (which re-rolls the PID), a
     * species change — rebases it, so the arrows and the "Roll k/n" counter can never
     * describe a state the record is not in. */
    ivh_sync(&g_ivh, &e);
    render_card(&cur, card);
    if (editing && g_nslot) {
      if (fsel >= g_nslot) fsel = g_nslot - 1;
      int sx = g_slot[fsel].x, sy = g_slot[fsel].y, sw = g_slot[fsel].w;
      m3_frame(sx - 2, sy - 1, sx + sw, sy + UI_ROW_H, UI_SELTEXT);
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    const char* foot =
        (editing && g_nslot && g_slot[fsel].field == F_SUM_REROLL) ? PDNA_SUM_FOOT_REROLL
      : editing  ? (create ? PDNA_SUM_FOOT_CREATE_EDIT : PDNA_SUM_FOOT_EDIT)
      : create   ? PDNA_SUM_FOOT_CREATE
      : can_edit ? PDNA_SUM_FOOT_VIEW
                 : PDNA_SUM_FOOT_RO;
    ui_text(4, PDNA_SUM_FOOTER_Y, UI_DIM, foot);
    lastkey = 64 | (64 << 8) | (64 << 16) | (64 << 24);   /* render_card drew the rest pose (64,64,0,0) */
    /* Fetch the portrait ONCE per repaint, not once per animation frame.
     *
     * portrait_redraw used to call portrait_sprite() itself, and portrait_sprite goes
     * through the era router into mon_front_for_form -> LZ77UnCompWram of a 64x64 RGB15
     * sprite (8 KB) into mon_decomp. The pose changes on almost every frame while the
     * portrait animates, so that 8 KB BIOS decompress — plus, in the Game Boy case, a
     * cartridge read — was running at up to 60 Hz to redraw pixels that had not changed.
     *
     * Safe to hoist here specifically: the only thing that runs between this point and
     * the redraws is portrait_redraw, and the pointer is mon_decomp, a buffer shared
     * with mon_back. Anything decoding into mon_decomp between the fetch and the last
     * redraw would hand out the wrong mon's pixels — render_card/draw_left do exactly
     * that, which is why the fetch sits AFTER them and inside the same repaint. If a
     * future caller (rom_sprite.h:78, rom_gbsprite.h:134 and rom_itemart.h:145 all
     * nominate mon_decomp as their staging buffer) starts decoding inside this loop, it
     * must re-fetch here. */
    bool p_icon = false; int p_sw = MON_FRONT_W, p_sh = MON_FRONT_H;
    const uint16_t* p_spr = portrait_sprite(&cur, &p_icon, &p_sw, &p_sh);
    if (SUMMARY_ANIM && app_anim_enabled(ANIM_SUMMARY) && !cur.isEgg) { int fam = mon_anim_family(cur.species), wx, sy, dx, dy;
                              portrait_params(fam, anim_t, &wx, &sy, &dx, &dy); portrait_redraw(&cur, p_spr, p_icon, p_sw, p_sh, wx, sy, dx, dy, &lastkey); }

    u16 k, fresh;
    do { s_vsync();
         if (SUMMARY_ANIM && app_anim_enabled(ANIM_SUMMARY) && !cur.isEgg) { int fam = mon_anim_family(cur.species), wx, sy, dx, dy;
                                   portrait_params(fam, ++anim_t, &wx, &sy, &dx, &dy); portrait_redraw(&cur, p_spr, p_icon, p_sw, p_sh, wx, sy, dx, dy, &lastkey); }
         fresh = key_hit(KEY_FULL);
         k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
    if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
    else if (fresh & (KEY_L | KEY_R | KEY_SELECT)) snd_tab();
    else if (fresh & (KEY_A | KEY_START)) snd_ok();
    else if (fresh & (KEY_LEFT | KEY_RIGHT)) { if (editing) snd_edit(); else snd_tab(); }
    else if (fresh & KEY_B)               snd_back();

    if (fresh & KEY_SELECT) {                                 /* SELECT changes the portrait */
      if (cur.species == 410) {                               /* Deoxys (internal 410): cycle the forme (view-only; version-baked in Gen 3) */
        pk_set_deoxys_form((pk_get_deoxys_form() + 1) % 4);
        cur.form = (uint8_t)pk_get_deoxys_form();
      } else g_back = !g_back;                                /* every other mon: flip front/back */
      continue;
    }

    /* ---- CREATE: START keeps the new mon, from EITHER sub-mode. Never gated on
     * `dirty` — the record IS the new thing, so a create with zero edits must still
     * be writable (gen3_edit_commit is lossless, so it reproduces gen3_build_mon). */
    if (create && (fresh & KEY_START)) {
      if (confirm_keep()) {
        gen3_edit_commit(&e, out_rec); if (saved) *saved = true;
        key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
        if (card_io) *card_io = card;
        g_create = 0;
        return 0;
      }
      continue;                                    /* B in the prompt = carry on editing */
    }

    if (editing) {
      /* ---- EDIT MODE ---- */
      if (k & KEY_B) editing = false;              /* back to VIEW, keep pending edits */
      else if (k & (KEY_L | KEY_R)) { card = (card + (k & KEY_R ? 1 : NCARDS - 1)) % NCARDS; fsel = 0; }
      /* NOTE the LEFT/RIGHT branches test `fresh`, NOT `k`, on the reroll row: LEFT/RIGHT
       * are in key_repeat_mask above, so a held d-pad would otherwise scrub fifteen history
       * entries in a quarter of a second and machine-gun snd_back() at the end of them. */
      else if (g_nslot && (k & KEY_A)) {
        if (g_slot[fsel].field == F_SUM_REROLL) { if (do_reroll(&e, &cur)) dirty = true; }
        else { em_field_press(g_slot[fsel].field, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true; }
      }
      else if (g_nslot && (k & KEY_LEFT)) {
        if (g_slot[fsel].field == F_SUM_REROLL) {
          if ((fresh & KEY_LEFT) && do_history(&e, &cur, -1)) dirty = true;
        } else { em_field_adjust(g_slot[fsel].field, -1, false, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true; }
      }
      else if (g_nslot && (k & KEY_RIGHT)) {
        if (g_slot[fsel].field == F_SUM_REROLL) {
          if ((fresh & KEY_RIGHT) && do_history(&e, &cur, +1)) dirty = true;
        } else { em_field_adjust(g_slot[fsel].field, +1, false, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true; }
      }
      else if (k & KEY_UP)   { if (g_nslot) fsel = (fsel > 0) ? fsel - 1 : g_nslot - 1; }
      else if (k & KEY_DOWN) { if (g_nslot) fsel = (fsel + 1) % g_nslot; }
    } else {
      /* ---- VIEW MODE ---- */
      if (k & KEY_A) { if (can_edit) editing = true; }
      else if ((k & (KEY_L | KEY_R)) || (fresh & (KEY_LEFT | KEY_RIGHT))) {   /* L/R shoulder OR d-pad LEFT/RIGHT flip cards */
        int fwd = (k & KEY_R) || (fresh & KEY_RIGHT);
        card = (card + (fwd ? 1 : NCARDS - 1)) % NCARDS; fsel = 0;
      }
      else if (create) {
        /* No prev/next mon exists to scroll to, so U/D do nothing; B is the other way
         * out and asks the same keep/discard question START does. */
        if (k & KEY_B) {
          bool keep = confirm_keep();
          if (keep) { gen3_edit_commit(&e, out_rec); if (saved) *saved = true; }
          key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
          if (card_io) *card_io = card;
          g_create = 0;
          return 0;                                  /* discarded -> *saved stays false */
        }
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

/* ---- public entry points -------------------------------------------------
 * Two names over one body, so the five existing call sites keep their exact
 * signature and CREATE mode is a flag rather than a forked screen. */
int pdna_inspect(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec,
                 bool* saved, int* card_io) {
  return summary_run(rec, is_party, can_edit, out_rec, saved, card_io, false);
}

int pdna_inspect_create(uint8_t* rec, uint8_t* out_rec, bool* saved, int* card) {
  /* A brand-new record is never a party mon and editing is always on: the caller
   * (app_create_mon) is already Omega-gated. */
  return summary_run(rec, false, true, out_rec, saved, card, true);
}
