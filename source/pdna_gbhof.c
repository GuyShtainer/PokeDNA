/*
 * Gen 1/2's own Hall of Fame screen -- BACKLOG #89. See pdna_gbhof.h for the full
 * design rationale (no Gen-3 twin, why the "Records" nav row hosts it, the L/R
 * fast-jump / list-detail shape). This file only ever draws plain rows (no GB-shell
 * art), same posture as pdna_gbclock.c/pdna_battle_record().
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbhof.h"
#include "gb_hof.h"
#include "gb_edit.h"        /* GB_GEN1/GB_GEN2                                       */
#include "pdna_gen12.h"     /* gb_persist / gb_rollback -- the verified-write path    */
#include "pdna_trainer.h"   /* trainer_row_paint / trainer_key_legend                 */
#include "data_tables.h"    /* pk_species_name                                        */
#include "ui.h"
#include "snd.h"
#include "pdna_app.h"       /* msg_wait / app_confirm                                 */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

/* Same shape as every sibling GB screen's own s_wait (pdna_gbclock.c etc.): UP/DOWN
 * auto-repeat, L/R/A/B/START do not (matching pdna_battle_record()'s own L/R paging,
 * which is deliberately NOT in wait_keys' repeat mask either). */
static u16 s_wait(u16 mask) {
  u16 k, fresh;
  do {
    s_vsync();
    fresh = key_hit(mask);
    k = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN));
  } while (!k);
  if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
  else if (fresh & KEY_A) snd_ok();
  else if (fresh & KEY_B) snd_back();
  return k;
}

#define ROWS_PER_PAGE 8
#define ROW_Y0        34
#define ROW_DY        13

/* ---- LIST -------------------------------------------------------------------- */

static void hof_list_row_paint(const GbSession* s, int idx, int y, bool sel) {
  GbHofTeam t;
  char label[32];
  if (gbh_team(s, idx, &t) && t.n > 0) {
    int lo = 100, hi = 0;
    for (int m = 0; m < t.n; m++) {
      if (t.mon[m].level < lo) lo = t.mon[m].level;
      if (t.mon[m].level > hi) hi = t.mon[m].level;
    }
    siprintf(label, "#%-3d %d mon  Lv%d-%d", idx + 1, t.n, lo, hi);
  } else {
    siprintf(label, "#%-3d --", idx + 1);
  }
  trainer_row_paint(y, sel, label, "", UI_TEXT);
}

typedef struct { int sel; int top; bool valid; uint32_t gen; } HofListPaint;

static void hof_list_render(const GbSession* s, int present, int count, bool can_edit,
                            int sel, HofListPaint* pv) {
  int top = (sel / ROWS_PER_PAGE) * ROWS_PER_PAGE;
  bool full = !pv->valid || pv->gen != ui_clear_gen() || top != pv->top;

  if (full) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "HALL OF FAME");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char hdr[40];
    siprintf(hdr, "%d teams (lifetime count %d)", present, count);
    ui_text(6, 20, UI_DIM, hdr);

    if (present == 0) {
      ui_text(6, 60, UI_DIM, "No teams recorded yet.");
    } else {
      int shown = present - top;
      if (shown > ROWS_PER_PAGE) shown = ROWS_PER_PAGE;
      for (int i = 0; i < shown; i++)
        hof_list_row_paint(s, top + i, ROW_Y0 + i * ROW_DY, (top + i) == sel);
    }
    if (!can_edit) {
      /* Same two-line shape/column pdna_gbclock.c's own read-only branch uses. */
      ui_text(6, 112, UI_DIM, "Read-only cart - editing");
      ui_text(6, 122, UI_DIM, "needs an EZ-Flash Omega.");
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    trainer_key_legend(can_edit ? "U/D sel A view L/R pg ST menu"
                                : "U/D sel A view L/R pg B back");
  } else if (sel != pv->sel) {
    hof_list_row_paint(s, pv->sel, ROW_Y0 + (pv->sel - top) * ROW_DY, false);
    hof_list_row_paint(s, sel,     ROW_Y0 + (sel     - top) * ROW_DY, true);
  }
  pv->sel = sel; pv->top = top; pv->gen = ui_clear_gen(); pv->valid = true;
}

/* ---- DETAIL -------------------------------------------------------------------- */

static void hof_detail_render(const GbHofTeam* t, uint8_t gen, int team_no) {
  ui_clear();
  char title[24];
  siprintf(title, "TEAM #%d", team_no);
  ui_text(4, 4, UI_TITLE, title);
  ui_hline(0, 14, UI_SCR_W, UI_BORDER);

  int y0 = 22;
  if (gen == GB_GEN2) {
    char wc[24];
    siprintf(wc, "Wins so far: %d", (int)t->win_count);
    ui_text(6, 20, UI_DIM, wc);
    y0 = 34;
  }

  for (int m = 0; m < GBH_NUM_MONS; m++) {
    int y = y0 + m * 16;
    if (m >= t->n) continue;
    const GbHofMon* mn = &t->mon[m];
    const char* nm = (mn->dex >= 1 && mn->dex <= 251) ? pk_species_name(mn->dex) : "?";
    char line[32];
    if (gen == GB_GEN2)
      siprintf(line, "%-10s Lv%-3d%s", nm, mn->level, mn->shiny ? " *" : "");
    else
      siprintf(line, "%-10s Lv%-3d", nm, mn->level);
    ui_text(6, y, UI_TEXT, line);
    if (gen == GB_GEN2) {
      char ot[16];
      siprintf(ot, "OT %u", (unsigned)mn->otid);
      ui_text(168, y, UI_DIM, ot);
    }
  }
  ui_hline(0, 151, UI_SCR_W, UI_BORDER);
  trainer_key_legend("B back");
}

/* ---- START menu: CLEAR ALL / SET COUNT ------------------------------------------ */

/* CLEAR ALL: gbh_clear's own contract (gb_hof.h) is "on ANY non-GBS_OK, some teams may
 * already be zeroed in the image" -- gb_rollback() restores the WHOLE image from the
 * pristine copy every GB edit screen already keeps, discarding every chunk this call
 * wrote, good or bad. Same shape gbclock_do_reset/do_clear use for their own refusal. */
static void hof_do_clear(GbSession* s) {
  if (!app_confirm("CLEAR ALL",
                   "The PC's HALL OF FAME option disappears until you win again."))
    return;
  GbsStatus st = gbh_clear(s);
  if (st != GBS_OK) {
    gb_rollback();
    msg_wait("REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
    return;
  }
  gb_persist("hof clear");
}

/* SET COUNT: a plain stepper, same shape as pdna_gbclock.c's shift editor but a
 * single non-negative, clamped value (Gen 1 <=255, Gen 2 <=200 -- gbh_set_count's own
 * clamp; this editor mirrors it so the displayed cap never lies about what a press
 * past it will actually do). A no-op (value unchanged) skips app_confirm AND the
 * write entirely, hard rule 3. */
__attribute__((noinline))
static void hof_set_count_editor(GbSession* s, uint8_t gen) {
  int cap = (gen == GB_GEN1) ? 255 : 200;
  int start = gbh_count(s);
  int v = start;
  bool valid = false; int pv = -1; uint32_t g = 0;
  key_repeat_mask(KEY_UP | KEY_DOWN);
  for (;;) {
    bool full = !valid || g != ui_clear_gen();
    char b[28];
    siprintf(b, "Lifetime wins: %d / %d", v, cap);
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "SET COUNT");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      ui_text(10, 60, UI_TEXT, b);
      ui_text(4, 152, UI_DIM, "U/D value  A set  B cancel");
    } else if (v != pv) {
      ui_fill_rect(8, 52, 200, 16, UI_BG);
      ui_text(10, 60, UI_TEXT, b);
    }
    pv = v; valid = true; g = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) break;
    else if (k & (KEY_UP | KEY_DOWN)) {
      int dir = (k & KEY_UP) ? 1 : -1;
      v += dir;
      if (v < 0) v = 0;
      if (v > cap) v = cap;
    } else if (k & KEY_A) {
      if (v == start) { snd_back(); break; }   /* no-op: nothing to confirm or write */
      char l1[40];
      siprintf(l1, "Set lifetime count to %d?", v);
      if (app_confirm("SET COUNT", l1)) {
        GbsStatus st = gbh_set_count(s, v);
        if (st != GBS_OK) {
          gb_rollback();
          msg_wait("REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
        } else {
          gb_persist("hof setcount");
        }
      }
      break;
    }
  }
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);   /* restore the global set */
}

enum { HOFMENU_CLEAR = 0, HOFMENU_SETCOUNT, HOFMENU_N };
static const char* const kHofMenuLbl[HOFMENU_N] = { "CLEAR ALL", "SET COUNT" };

static void hof_start_menu(GbSession* s, uint8_t gen) {
  int sel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "HALL OF FAME MENU");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < HOFMENU_N; i++)
      trainer_row_paint(40 + i * 16, i == sel, kHofMenuLbl[i], "", UI_TEXT);
    ui_text(4, 152, UI_DIM, "U/D select  A choose  B back");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : HOFMENU_N - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % HOFMENU_N;
    else if (k & KEY_A) {
      if (sel == HOFMENU_CLEAR) hof_do_clear(s);
      else                      hof_set_count_editor(s, gen);
      return;   /* back to the list either way; it re-reads count/present fresh */
    }
  }
}

/* ---- entry ----------------------------------------------------------------------- */

void pdna_gbhof(GbSession* s, bool can_edit) {
  if (!s || !s->open) {
    msg_wait("HALL OF FAME", UI_WARN, "Could not read this save.", 0);
    return;
  }

  int sel = 0;
  bool in_detail = false;
  HofListPaint pv; memset(&pv, 0, sizeof pv);

  for (;;) {
    int present = gbh_team_count_present(s);
    int count = gbh_count(s);
    if (present == 0) sel = 0;
    else if (sel >= present) sel = present - 1;

    if (in_detail) {
      GbHofTeam t;
      if (gbh_team(s, sel, &t)) {
        hof_detail_render(&t, s->gen, sel + 1);
        s_wait(KEY_B);
      }
      in_detail = false;
      pv.valid = false;
      continue;
    }

    hof_list_render(s, present, count, can_edit, sel, &pv);
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   sel = (present > 0) ? ((sel > 0) ? sel - 1 : present - 1) : 0;
    else if (k & KEY_DOWN) sel = (present > 0) ? ((sel + 1) % present) : 0;
    else if (k & (KEY_LEFT | KEY_RIGHT)) {
      if (present > 0) {
        int dir = (k & KEY_LEFT) ? -1 : 1;
        sel += dir * ROWS_PER_PAGE;
        if (sel < 0) sel = 0;
        if (sel >= present) sel = present - 1;
      }
      pv.valid = false;
    } else if (k & KEY_A) {
      if (present > 0) { in_detail = true; pv.valid = false; }
      else { snd_deny(); }
    } else if (k & KEY_START) {
      if (!can_edit) { snd_deny(); continue; }
      hof_start_menu(s, s->gen);
      pv.valid = false;
    }
  }
}
