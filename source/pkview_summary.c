/*
 * Game-faithful paged Pokémon summary (6 cards) for gba-pokeviewer — now an
 * inline VIEW + EDIT screen. Each editable field registers its on-screen box
 * during render; in edit mode a cursor highlights one and A / LEFT-RIGHT edit it
 * in place (reusing the editor's field dispatchers). No separate "edit" mode.
 *   cards: 0 INFO  1 SKILLS  2 IVs  3 EVs  4 BATTLE MOVES  5 CONTEST MOVES
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pkview_summary.h"
#include "ui.h"
#include "gen3_mon.h"
#include "gen3_edit.h"
#include "gen3_box.h"      /* pk_resolve */
#include "data_tables.h"
#include "pkview_edit.h"   /* F_*, em_field_press / em_field_adjust */
#include "mon_front.h"
#include "mon_icons.h"
#include "type_icons.h"
#include "snd.h"

#define NCARDS 6

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

static const char* gender_str(uint8_t g) { return g == 0 ? " M" : g == 1 ? " F" : ""; }

static void draw_left(const PkMon* p) {
  ui_panel(0, 11, 92, 139, UI_PANEL, UI_BORDER);
  const uint16_t* spr = mon_front_for(p->species, p->isShiny);
  if (spr) ui_sprite(14, 14, MON_FRONT_W, MON_FRONT_H, spr);
  else     ui_sprite(30, 30, MON_ICON_W, MON_ICON_H, mon_icon_for(p->species));

  char buf[40];
  siprintf(buf, "No.%u", (unsigned)pk_national_no(p->species));
  ui_text(6, 82, C_KEY, buf);
  if (p->isShiny) ui_text(64, 82, C_HOT, "*");
  char nm[24];
  ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 11);
  ui_text(6, 92, C_VAL, nm);
  siprintf(buf, "Lv%u%s", (unsigned)p->level, gender_str(p->gender));
  ui_text(6, 102, C_VAL, buf);
  char sp[24];
  ui_truncate(sp, pk_species_name(p->species), 11);
  ui_text(6, 112, C_KEY, sp);
  if (p->isBadEgg)   ui_text(6, 138, UI_WARN, "BAD EGG");
  else if (p->isEgg) ui_text(6, 138, C_HOT, "EGG");
}

static void card_info(const PkMon* p) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, "POKEMON INFO"); y += 12;

  ui_text(x, y, C_KEY, "Species"); reg(F_SPECIES, x + 48, y, 88);
  ui_text(x + 48, y, C_VAL, pk_species_name(p->species)); y += 9;

  ui_text(x, y, C_KEY, "Name"); reg(F_NICK, x + 48, y, 88);
  { char nm[24]; ui_truncate(nm, p->nickname[0] ? p->nickname : "-", 11); ui_text(x + 48, y, C_VAL, nm); } y += 9;

  ui_text(x, y, C_KEY, "OT"); reg(F_OT, x + 48, y, 52);
  ui_text(x + 48, y, C_VAL, p->otName);
  siprintf(b, "TID %05u", (unsigned)(p->otId & 0xFFFF)); ui_text(x + 104, y, UI_DIM, b); y += 9;

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
  ui_text(x + 108, y, C_VAL, p->gender == 0 ? "M" : p->gender == 1 ? "F" : "-"); y += 10;

  siprintf(b, "Met Lv%u %s", (unsigned)p->metLevel, pk_location_name(p->metLocation));
  { char mb[40]; ui_truncate(mb, b, 17); ui_text(x, y, UI_DIM, mb); }
}

static void card_skills(const PkMon* p) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, "SKILLS"); y += 12;
  ui_text(x, y, C_KEY, "Level"); reg(F_LEVEL, x + 60, y, 40);
  siprintf(b, "%u", (unsigned)p->level); ui_text(x + 60, y, C_VAL, b); y += 9;
  ui_text(x, y, C_KEY, "Item"); reg(F_ITEM, x + 60, y, 76);
  ui_text(x + 60, y, C_VAL, p->heldItem ? pk_item_name(p->heldItem) : "none"); y += 9;
  ui_text(x, y, C_KEY, "Friend"); reg(F_FRIEND, x + 60, y, 40);
  siprintf(b, "%u", (unsigned)p->friendship); ui_text(x + 60, y, C_VAL, b); y += 11;
  /* Each stat row edits that stat's EV — the only persistent, lossless stat lever
   * in Gen-3 (final stats are derived from base+IV+EV+level+nature). The number
   * updates live; full IV/EV grids remain on the IV/EV cards. */
  for (int i = 0; i < 6; i++) {
    int s = DISP[i], bo = pk_nature_boost(p->nature), h = pk_nature_hinder(p->nature);
    u16 col = (s == bo) ? UI_OK : (s == h) ? UI_WARN : C_VAL;
    reg(F_EV0 + s, x, y, 138);
    siprintf(b, "%-7s", DLAB[i]); ui_text(x, y, C_KEY, b);
    siprintf(b, "%4u", (unsigned)p->stats[s]); ui_text(x + 54, y, col, b);
    siprintf(b, "EV%u", (unsigned)p->evs[s]);  ui_text(x + 94, y, UI_DIM, b);
    y += 9;
  }
  ui_text(x, y + 1, UI_DIM, "<>: train EVs");
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

static void render_card(const PkMon* p, int card) {
  ui_clear();
  g_nslot = 0;
  if (g_edit) { ui_fill_rect(0, 0, 50, 9, UI_WARN); ui_text(8, 1, UI_PANEL, "EDIT"); } /* unmissable */
  else        ui_text(4, 2, UI_DIM, "VIEW");
  draw_dots(150, 2, NCARDS, card);
  ui_hline(0, 10, UI_SCR_W, UI_BORDER);
  draw_left(p);
  switch (card) {
    case 0: card_info(p);          break;
    case 1: card_skills(p);        break;
    case 2: card_spread(p, false); break;
    case 3: card_spread(p, true);  break;
    case 4: card_moves(p, false);  break;
    case 5: card_moves(p, true);   break;
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

bool pkview_inspect(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec) {
  EditMon e;
  gen3_edit_load(rec, is_party, &e);
  PkMon cur;
  em_preview(&e, &cur); pk_resolve(&cur);

  int card = 0, fsel = 0;
  bool dirty = false;
  if (can_edit) key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);

  for (;;) {
    g_edit = can_edit;
    render_card(&cur, card);
    if (can_edit && g_nslot) {
      if (fsel >= g_nslot) fsel = g_nslot - 1;
      int sx = g_slot[fsel].x, sy = g_slot[fsel].y, sw = g_slot[fsel].w;
      m3_frame(sx - 2, sy - 1, sx + sw, sy + UI_ROW_H, UI_SELTEXT);
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, can_edit ? "U/D field  A list  <> +/-  L/R card  B" : "L/R card  B back");

    u16 k, fresh;
    do { s_vsync(); fresh = key_hit(KEY_FULL);
         k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
    if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
    else if (fresh & (KEY_L | KEY_R))     snd_tab();
    else if (fresh & KEY_A)               snd_ok();
    else if (fresh & (KEY_LEFT | KEY_RIGHT)) { if (can_edit) snd_edit(); }
    else if (fresh & KEY_B)               snd_back();

    if (k & KEY_B) {
      bool ret = false;
      if (can_edit && dirty && confirm()) { gen3_edit_commit(&e, out_rec); ret = true; }
      if (can_edit) key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
      return ret;
    }
    else if (k & (KEY_L | KEY_R)) { card = (card + (k & KEY_R ? 1 : NCARDS - 1)) % NCARDS; fsel = 0; }
    else if (can_edit && g_nslot && (k & KEY_A)) {
      em_field_press(g_slot[fsel].field, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true;
    }
    else if (can_edit && g_nslot && (k & KEY_LEFT)) {
      em_field_adjust(g_slot[fsel].field, -1, false, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true;
    }
    else if (can_edit && g_nslot && (k & KEY_RIGHT)) {
      em_field_adjust(g_slot[fsel].field, +1, false, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true;
    }
    else if (k & KEY_UP)   { if (can_edit && g_nslot) fsel = (fsel > 0) ? fsel - 1 : g_nslot - 1; }
    else if (k & KEY_DOWN) { if (can_edit && g_nslot) fsel = (fsel + 1) % g_nslot; }
  }
}
