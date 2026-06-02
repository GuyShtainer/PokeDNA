/*
 * Game-faithful paged Pokémon summary (6 cards) for gba-pokeviewer.
 *
 * Mimics the Gen-3 summary screens: a persistent left panel (front sprite +
 * No./nickname/level/gender) and a per-card right panel. Native order is
 * INFO, SKILLS, BATTLE MOVES, CONTEST MOVES; per the user's request an IV card
 * and an EV card (each with a total) are inserted after SKILLS:
 *   0 INFO   1 SKILLS   2 IVs   3 EVs   4 BATTLE MOVES   5 CONTEST MOVES
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pkview_summary.h"
#include "ui.h"
#include "gen3_mon.h"
#include "data_tables.h"
#include "mon_front.h"
#include "mon_icons.h"

#define NCARDS 6

/* Display order (game shows HP, Atk, Def, SpA, SpD, Spe) vs save-native order
 * (HP, Atk, Def, Spe, SpA, SpD). */
static const int   DISP[6]  = { PK_HP, PK_ATK, PK_DEF, PK_SPA, PK_SPD, PK_SPE };
static const char* DLAB[6]  = { "HP", "Attack", "Defense", "Sp.Atk", "Sp.Def", "Speed" };
static const char* DSHORT[6] = { "HP", "Atk", "Def", "SpA", "SpD", "Spe" };

#define C_HDR  UI_TITLE
#define C_KEY  UI_DIRCLR   /* field labels (cyan) */
#define C_VAL  UI_TEXT     /* values (white) */
#define C_HOT  UI_WARN     /* nature / highlights (orange) */

static void s_vsync(void) { VBlankIntrWait(); key_poll(); }

/* page dots at (x,y); `active` filled, others hollow */
static void draw_dots(int x, int y, int n, int active) {
  for (int i = 0; i < n; i++) {
    int cx = x + i * 7;
    if (i == active) ui_fill_rect(cx, y, 5, 5, C_HDR);
    else { ui_fill_rect(cx, y, 5, 5, UI_PANEL); m3_frame(cx, y, cx + 4, y + 4, UI_BORDER); }
  }
}

/* word-wrap `s` into lines of <= cols columns starting at (x,y); returns next y. */
static int text_wrap(int x, int y, int cols, u16 ink, const char* s) {
  char line[64];
  int li = 0;
  while (*s) {
    /* take a word */
    const char* w = s;
    int wl = 0;
    while (s[wl] && s[wl] != ' ') wl++;
    int need = (li ? li + 1 : 0) + wl;
    if (need > cols && li) { line[li] = 0; ui_text(x, y, ink, line); y += UI_ROW_H; li = 0; need = wl; }
    if (li) line[li++] = ' ';
    for (int i = 0; i < wl && li < (int)sizeof(line) - 1; i++) line[li++] = w[i];
    s += wl;
    while (*s == ' ') s++;
  }
  if (li) { line[li] = 0; ui_text(x, y, ink, line); y += UI_ROW_H; }
  return y;
}

static const char* gender_str(uint8_t g) { return g == 0 ? " M" : g == 1 ? " F" : ""; }

/* ---------- left panel (common to every card) ---------- */
static void draw_left(const PkMon* p) {
  ui_panel(0, 11, 92, 139, UI_PANEL, UI_BORDER);

  const uint16_t* spr = mon_front_for(p->species, p->isShiny);
  if (spr) ui_sprite(14, 14, MON_FRONT_W, MON_FRONT_H, spr);
  else     ui_icon16(38, 38, mon_icon_for(p->species));   /* fallback */

  char buf[40];
  uint8_t gr = pk_species_gender_ratio(p->species);
  uint8_t gender = pk_gender_from(p->personality, gr);

  siprintf(buf, "No.%u", (unsigned)pk_national_no(p->species));
  ui_text(6, 82, C_KEY, buf);
  if (p->isShiny) ui_text(64, 82, C_HOT, "*");

  char nm[24];
  ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 11);
  ui_text(6, 92, C_VAL, nm);

  siprintf(buf, "Lv%u%s", (unsigned)p->level, gender_str(gender));
  ui_text(6, 102, C_VAL, buf);

  char sp[24];
  ui_truncate(sp, pk_species_name(p->species), 11);
  ui_text(6, 112, C_KEY, sp);

  /* types */
  ui_text(6, 124, C_VAL, pk_type_name(pk_species_type1(p->species)));
  uint8_t t2 = pk_species_type2(p->species);
  if (t2 != pk_species_type1(p->species))
    ui_text(48, 124, C_VAL, pk_type_name(t2));

  if (p->isEgg)    ui_text(6, 136, C_HOT, "EGG");
  if (p->isBadEgg) ui_text(6, 136, UI_WARN, "BAD EGG");
}

/* ---------- card 0: INFO ---------- */
static void card_info(const PkMon* p) {
  int x = 98, y = 14;
  char buf[48];
  ui_text(x, y, C_HDR, "POKEMON INFO"); y += 11;

  siprintf(buf, "OT/%s", p->otName); ui_text(x, y, C_VAL, buf);
  siprintf(buf, "ID %05u", (unsigned)(p->otId & 0xFFFF)); ui_text(x + 78, y, C_KEY, buf); y += 11;

  ui_text(x, y, C_KEY, "Ability"); y += UI_ROW_H;
  uint16_t ab = pk_species_ability(p->species, p->abilityNum);
  ui_text(x + 4, y, C_VAL, pk_ability_name(ab)); y += UI_ROW_H;
  y = text_wrap(x + 4, y, 17, UI_DIM, pk_ability_desc(ab)); y += 3;

  ui_text(x, y, C_KEY, "Memo"); y += UI_ROW_H;
  ui_text(x + 4, y, C_HOT, pk_nature_name(p->nature));
  ui_text(x + 4 + (int)strlen(pk_nature_name(p->nature)) * 8 + 4, y, C_VAL, "nature"); y += UI_ROW_H;
  siprintf(buf, "met at Lv%u,", (unsigned)p->metLevel); ui_text(x + 4, y, C_VAL, buf); y += UI_ROW_H;
  ui_text(x + 4, y, C_HOT, pk_location_name(p->metLocation));
}

/* ---------- card 1: SKILLS ---------- */
static void card_skills(const PkMon* p) {
  int x = 98, y = 14;
  char buf[48];
  ui_text(x, y, C_HDR, "SKILLS"); y += 11;

  ui_text(x, y, C_KEY, "Item");
  ui_text(x + 36, y, C_VAL, p->heldItem ? pk_item_name(p->heldItem) : "none"); y += 11;

  for (int i = 0; i < 6; i++) {
    int s = DISP[i];
    int b = pk_nature_boost(p->nature), h = pk_nature_hinder(p->nature);
    u16 col = (s == b) ? UI_OK : (s == h) ? UI_WARN : C_VAL;
    siprintf(buf, "%-7s", DLAB[i]); ui_text(x, y, C_KEY, buf);
    siprintf(buf, "%5u", (unsigned)p->stats[s]); ui_text(x + 60, y, col, buf);
    y += UI_ROW_H;
  }
  y += 4;

  uint8_t gr = pk_species_growth(p->species);
  uint32_t cur = pk_exp_for_level(gr, p->level);
  uint32_t nxt = (p->level < 100) ? pk_exp_for_level(gr, p->level + 1) : p->experience;
  siprintf(buf, "EXP %u", (unsigned)p->experience); ui_text(x, y, C_KEY, buf); y += UI_ROW_H;
  if (p->level < 100 && nxt > cur) {
    uint32_t span = nxt - cur, into = (p->experience > cur) ? p->experience - cur : 0;
    int fill = (int)((uint64_t)into * 132 / span);
    ui_progress(x, y, 132, 5, fill, UI_OK, UI_PANEL, UI_BORDER); y += 8;
    siprintf(buf, "next: %u", (unsigned)(nxt > p->experience ? nxt - p->experience : 0));
    ui_text(x, y, UI_DIM, buf);
  } else {
    ui_text(x, y, UI_DIM, "max level");
  }
}

/* ---------- card 2 / 3: IVs / EVs ---------- */
static void card_spread(const PkMon* p, bool ev) {
  int x = 98, y = 14;
  char buf[48];
  ui_text(x, y, C_HDR, ev ? "EVs" : "IVs"); y += 12;

  int total = 0, maxv = ev ? 255 : 31;
  for (int i = 0; i < 6; i++) {
    int s = DISP[i];
    int v = ev ? p->evs[s] : p->ivs[s];
    total += v;
    ui_text(x, y, C_KEY, DSHORT[i]);
    siprintf(buf, "%3d", v); ui_text(x + 28, y, C_VAL, buf);
    int fill = v * 96 / maxv;
    u16 col = (!ev && v == 31) ? UI_OK : (ev && v == 252) ? UI_OK : C_HDR;
    ui_progress(x + 44, y + 1, 96, 5, fill, col, UI_PANEL, UI_BORDER);
    y += 12;
  }
  y += 2;
  ui_text(x, y, C_KEY, "TOTAL");
  siprintf(buf, "%d / %d", total, ev ? 510 : 186);
  ui_text(x + 44, y, C_HOT, buf);
}

/* ---------- card 4 / 5: BATTLE / CONTEST MOVES ---------- */
static void card_moves(const PkMon* p, bool contest) {
  int x = 98, y = 14;
  char buf[48];
  ui_text(x, y, C_HDR, contest ? "CONTEST MOVES" : "BATTLE MOVES"); y += 12;

  for (int i = 0; i < 4; i++) {
    uint16_t mv = p->moves[i];
    if (mv == 0) { ui_text(x, y, UI_DIM, "-"); y += 14; continue; }
    if (contest) ui_text(x, y, C_KEY, pk_contest_name(pk_move_contest(mv)));
    else         ui_text(x, y, C_KEY, pk_type_name(pk_move_type(mv)));
    ui_text(x + 44, y, C_VAL, pk_move_name(mv));
    if (!contest) {
      uint8_t ppup = (p->ppBonuses >> (i * 2)) & 3;
      uint8_t base = pk_move_pp(mv);
      uint8_t maxpp = (uint8_t)(base + base / 5 * ppup);
      siprintf(buf, "%u/%u", (unsigned)p->pp[i], (unsigned)maxpp);
      ui_text(x + 44, y + UI_ROW_H, UI_DIM, buf);
    }
    y += 16;
  }
}

static void render(const PkMon* p, int idx, int count, int card) {
  ui_clear();
  /* top bar */
  char hd[32];
  siprintf(hd, "%d/%d", idx + 1, count);
  ui_text(4, 2, UI_DIM, hd);
  draw_dots(150, 2, NCARDS, card);
  ui_hline(0, 10, UI_SCR_W, UI_BORDER);

  draw_left(p);
  switch (card) {
    case 0: card_info(p);        break;
    case 1: card_skills(p);      break;
    case 2: card_spread(p, false); break;
    case 3: card_spread(p, true);  break;
    case 4: card_moves(p, false);  break;
    case 5: card_moves(p, true);   break;
  }

  ui_hline(0, 151, UI_SCR_W, UI_BORDER);
  ui_text(4, 152, UI_DIM, "L/R card  U/D mon  B back");
}

int pkview_summary(const PkMon* party, int count, int idx) {
  int card = 0;
  if (count <= 0) return 0;
  if (idx < 0) idx = 0;
  if (idx >= count) idx = count - 1;

  for (;;) {
    render(&party[idx], idx, count, card);
    u16 k;
    do { s_vsync(); k = key_hit(KEY_FULL); } while (!k);

    if (k & KEY_B) return idx;
    else if (k & (KEY_LEFT | KEY_L))  card = (card + NCARDS - 1) % NCARDS;
    else if (k & (KEY_RIGHT | KEY_R)) card = (card + 1) % NCARDS;
    else if (k & KEY_UP)   idx = (idx + count - 1) % count;
    else if (k & KEY_DOWN) idx = (idx + 1) % count;
  }
}
