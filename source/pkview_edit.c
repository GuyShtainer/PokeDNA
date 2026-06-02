/*
 * Field-edit screen for gba-pokeviewer (PKHeX-style, in-RAM).
 * UP/DOWN pick a field; LEFT/RIGHT adjust by 1, L/R shoulders by a bigger step;
 * A opens a list picker (species/item/move/nature) or the on-screen keyboard
 * (nickname/OT) or toggles (ability/shiny/gender); B cancels; START -> commit
 * confirm. Personality-derived fields (nature/shiny/gender) re-roll the PID.
 * The actual SD write is done by the caller on the returned record.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "pkview_edit.h"
#include "ui.h"
#include "gen3_mon.h"
#include "gen3_edit.h"
#include "gen3_box.h"      /* pk_resolve */
#include "data_tables.h"
#include "osk.h"

enum {
  F_SPECIES, F_NICK, F_LEVEL, F_NATURE, F_ABILITY, F_SHINY, F_GENDER,
  F_ITEM, F_FRIEND,
  F_IV0, F_IV1, F_IV2, F_IV3, F_IV4, F_IV5,
  F_EV0, F_EV1, F_EV2, F_EV3, F_EV4, F_EV5,
  F_MV0, F_MV1, F_MV2, F_MV3,
  F_OT,
  F_NUM
};

static const char* const FLABEL[F_NUM] = {
  "Species", "Nickname", "Level", "Nature", "Ability", "Shiny", "Gender",
  "Item", "Friendship",
  "IV HP", "IV Atk", "IV Def", "IV Spe", "IV SpA", "IV SpD",
  "EV HP", "EV Atk", "EV Def", "EV Spe", "EV SpA", "EV SpD",
  "Move 1", "Move 2", "Move 3", "Move 4", "OT Name",
};

#define VIS_ROWS 16

static void s_vsync(void) { VBlankIntrWait(); key_poll(); }
static u16  s_wait(u16 mask) { u16 k; do { s_vsync(); k = key_hit(mask); } while (!k); return k; }
static int  clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static const char* nature_name16(uint16_t n) { return pk_nature_name((uint8_t)n); }
static const char* GEN[3] = { "Male", "Female", "-" };

static void refresh(EditMon* e, PkMon* cur) {
  em_preview(e, cur);
  pk_resolve(cur);                 /* fills gender (+ box level/stats) */
}

/* format a field's current value into buf */
static void field_value(int f, const PkMon* c, char* buf) {
  switch (f) {
    case F_SPECIES: siprintf(buf, "%s", pk_species_name(c->species)); break;
    case F_NICK:    siprintf(buf, "%s", c->nickname); break;
    case F_LEVEL:   siprintf(buf, "%u", (unsigned)c->level); break;
    case F_NATURE:  siprintf(buf, "%s", pk_nature_name(c->nature)); break;
    case F_ABILITY: siprintf(buf, "%s", pk_ability_name(pk_species_ability(c->species, c->abilityNum))); break;
    case F_SHINY:   siprintf(buf, "%s", c->isShiny ? "Yes" : "No"); break;
    case F_GENDER:  siprintf(buf, "%s", GEN[c->gender <= 2 ? c->gender : 2]); break;
    case F_ITEM:    siprintf(buf, "%s", c->heldItem ? pk_item_name(c->heldItem) : "-"); break;
    case F_FRIEND:  siprintf(buf, "%u", (unsigned)c->friendship); break;
    case F_IV0: case F_IV1: case F_IV2: case F_IV3: case F_IV4: case F_IV5:
      siprintf(buf, "%u", (unsigned)c->ivs[f - F_IV0]); break;
    case F_EV0: case F_EV1: case F_EV2: case F_EV3: case F_EV4: case F_EV5:
      siprintf(buf, "%u", (unsigned)c->evs[f - F_EV0]); break;
    case F_MV0: case F_MV1: case F_MV2: case F_MV3: {
      uint16_t mv = c->moves[f - F_MV0];
      siprintf(buf, "%s", mv ? pk_move_name(mv) : "-"); break;
    }
    case F_OT:      siprintf(buf, "%s", c->otName); break;
    default:        buf[0] = 0;
  }
}

static void render(const PkMon* c, int sel, int top) {
  ui_clear();
  char line[64];
  int evtot = c->evs[0] + c->evs[1] + c->evs[2] + c->evs[3] + c->evs[4] + c->evs[5];
  ui_text(4, 0, UI_TITLE, "EDIT POKEMON");
  siprintf(line, "EV %d/510", evtot);
  ui_text(168, 0, evtot > 510 ? UI_WARN : UI_DIM, line);
  siprintf(line, "%s  Lv%u  %s%s%s", pk_species_name(c->species), (unsigned)c->level,
           pk_nature_name(c->nature), c->isShiny ? "  SHINY" : "",
           c->gender == 1 ? "  F" : c->gender == 0 ? "  M" : "");
  ui_text(4, 10, UI_DIRCLR, line);
  ui_hline(0, 19, UI_SCR_W, UI_BORDER);

  char val[40];
  for (int i = 0; i < VIS_ROWS && top + i < F_NUM; i++) {
    int f = top + i, y = 21 + i * 8;
    bool s = (f == sel);
    if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
    ui_text(6, y, s ? UI_SELTEXT : UI_DIM, FLABEL[f]);
    field_value(f, c, val);
    char vt[40];
    ui_truncate(vt, val, 16);
    ui_text(118, y, s ? UI_SELTEXT : UI_TEXT, vt);
  }
  ui_hline(0, 151, UI_SCR_W, UI_BORDER);
  ui_text(4, 152, UI_DIM, "L/R+- A:pick B:exit START:save");
}

/* generic scrollable id picker; returns chosen id or -1 (B). */
static int pick_id(const char* title, int count, const char* (*name_fn)(uint16_t), int current) {
  int sel = clampi(current, 0, count - 1), top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + VIS_ROWS) top = sel - VIS_ROWS + 1;
    ui_clear();
    ui_text(4, 2, UI_TITLE, title);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    char row[40];
    for (int i = 0; i < VIS_ROWS && top + i < count; i++) {
      int id = top + i, y = 14 + i * 8;
      bool s = (id == sel);
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      siprintf(row, "%3d  %s", id, name_fn((uint16_t)id));
      char rt[40]; ui_truncate(rt, row, 28);
      ui_text(6, y, s ? UI_SELTEXT : UI_TEXT, rt);
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, "U/D move  L/R +-10  A pick  B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    else if (k & KEY_A) return sel;
    else if (k & KEY_UP)   sel = clampi(sel - 1, 0, count - 1);
    else if (k & KEY_DOWN) sel = clampi(sel + 1, 0, count - 1);
    else if (k & KEY_L)    sel = clampi(sel - 10, 0, count - 1);
    else if (k & KEY_R)    sel = clampi(sel + 10, 0, count - 1);
  }
}

/* re-roll PID for a (nature, shiny, gender) combo, relaxing gender then shiny. */
static void reroll_to(EditMon* e, const PkMon* c, int nat, int shiny, int gender) {
  uint8_t ratio = pk_species_gender_ratio(c->species);
  if (em_reroll(e, nat, shiny, gender, ratio)) return;
  if (em_reroll(e, nat, shiny, -1, ratio)) return;
  em_reroll(e, nat, -1, -1, ratio);
}

static void adjust(int f, int dir, bool big, EditMon* e, const PkMon* c) {
  int s = big ? 10 : 1;
  switch (f) {
    case F_SPECIES: em_set_species(e, clampi(c->species + dir * (big ? 10 : 1), 1, 411)); break;
    case F_LEVEL:   em_set_level(e, clampi(c->level + dir * s, 1, 100)); break;
    case F_FRIEND:  em_set_friendship(e, clampi(c->friendship + dir * s, 0, 255)); break;
    case F_ITEM:    em_set_item(e, clampi(c->heldItem + dir * (big ? 10 : 1), 0, 65535)); break;
    case F_ABILITY: em_set_ability(e, c->abilityNum ^ 1); break;
    case F_NATURE:  reroll_to(e, c, (c->nature + (dir > 0 ? 1 : 24)) % 25,
                              c->isShiny ? 1 : 0, c->gender < 2 ? c->gender : -1); break;
    case F_SHINY:   reroll_to(e, c, c->nature, c->isShiny ? 0 : 1, c->gender < 2 ? c->gender : -1); break;
    case F_GENDER:  if (c->gender < 2) reroll_to(e, c, c->nature, c->isShiny ? 1 : 0, c->gender ^ 1); break;
    case F_IV0: case F_IV1: case F_IV2: case F_IV3: case F_IV4: case F_IV5:
      em_set_iv(e, f - F_IV0, (uint8_t)(big ? (dir > 0 ? 31 : 0) : clampi(c->ivs[f - F_IV0] + dir, 0, 31))); break;
    case F_EV0: case F_EV1: case F_EV2: case F_EV3: case F_EV4: case F_EV5:
      em_set_ev(e, f - F_EV0, (uint8_t)clampi(c->evs[f - F_EV0] + dir * s, 0, 255)); break;
    case F_MV0: case F_MV1: case F_MV2: case F_MV3:
      em_set_move(e, f - F_MV0, clampi(c->moves[f - F_MV0] + dir * (big ? 10 : 1), 0, 65535)); break;
    default: break;   /* names: use A */
  }
}

static void press_a(int f, EditMon* e, const PkMon* c) {
  char buf[20];
  switch (f) {
    case F_SPECIES: { int id = pick_id("SPECIES", 412, pk_species_name, c->species); if (id > 0) em_set_species(e, (uint16_t)id); break; }
    case F_ITEM:    { int id = pick_id("ITEM", 400, pk_item_name, c->heldItem); if (id >= 0) em_set_item(e, (uint16_t)id); break; }
    case F_NATURE:  { int id = pick_id("NATURE", 25, nature_name16, c->nature); if (id >= 0) reroll_to(e, c, id, c->isShiny ? 1 : 0, c->gender < 2 ? c->gender : -1); break; }
    case F_MV0: case F_MV1: case F_MV2: case F_MV3:
      { int id = pick_id("MOVE", 355, pk_move_name, c->moves[f - F_MV0]); if (id >= 0) em_set_move(e, f - F_MV0, (uint16_t)id); break; }
    case F_NICK: if (osk_input("NICKNAME", c->nickname, buf, 11)) em_set_nickname(e, buf); break;
    case F_OT:   if (osk_input("OT NAME", c->otName, buf, 8))    em_set_otname(e, buf); break;
    case F_ABILITY: em_set_ability(e, c->abilityNum ^ 1); break;
    case F_SHINY:   reroll_to(e, c, c->nature, c->isShiny ? 0 : 1, c->gender < 2 ? c->gender : -1); break;
    case F_GENDER:  if (c->gender < 2) reroll_to(e, c, c->nature, c->isShiny ? 1 : 0, c->gender ^ 1); break;
    default: break;
  }
}

static bool confirm(void) {
  ui_clear();
  ui_text(20, 50, UI_TITLE, "Commit changes to the save?");
  ui_text(20, 66, UI_TEXT, "A = write (backup made first)");
  ui_text(20, 78, UI_WARN, "B = cancel");
  ui_text(20, 100, UI_DIM, "The original is backed up to .bak,");
  ui_text(20, 110, UI_DIM, "then the new save is verified on write.");
  u16 k = s_wait(KEY_A | KEY_B);
  return (k & KEY_A) != 0;
}

bool pkview_edit(const uint8_t* rec, bool is_party, uint8_t* out_rec) {
  EditMon e;
  gen3_edit_load(rec, is_party, &e);
  PkMon cur;
  refresh(&e, &cur);

  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
  int sel = 0, top = 0;
  bool committed = false;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + VIS_ROWS) top = sel - VIS_ROWS + 1;
    render(&cur, sel, top);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) break;
    else if (k & KEY_START) { if (confirm()) { gen3_edit_commit(&e, out_rec); committed = true; break; } }
    else if (k & KEY_UP)   sel = (sel == 0) ? F_NUM - 1 : sel - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % F_NUM;
    else if (k & KEY_A)    { press_a(sel, &e, &cur); refresh(&e, &cur); }
    else if (k & KEY_LEFT)  { adjust(sel, -1, false, &e, &cur); refresh(&e, &cur); }
    else if (k & KEY_RIGHT) { adjust(sel, +1, false, &e, &cur); refresh(&e, &cur); }
    else if (k & KEY_L)     { adjust(sel, -1, true,  &e, &cur); refresh(&e, &cur); }
    else if (k & KEY_R)     { adjust(sel, +1, true,  &e, &cur); refresh(&e, &cur); }
  }
  key_repeat_mask(KEY_UP | KEY_DOWN);    /* restore the global repeat set */
  return committed;
}
