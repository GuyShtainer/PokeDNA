/*
 * Gen-2 box names -- see pdna_gbboxnames.h for the full design note (BACKLOG #94).
 * A scrolling list of the 14 box names; A opens the same osk_input "BOX NAME"
 * editor Gen 3's own box-banner rename uses; B asks to save.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbboxnames.h"
#include "gb_boxnames.h"
#include "gb_edit.h"       /* GB_TEXT_MAX                                           */
#include "pdna_gen12.h"    /* gb_persist                                            */
#include "ui.h"
#include "snd.h"
#include "rmbl.h"
#include "osk.h"
#include "pdna_app.h"      /* app_confirm                                           */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN)) snd_move();
  else if (k & KEY_A)      snd_ok();
  else if (k & KEY_B)      snd_back();
  return k;
}

static void s_msg(const char* title, u16 ink, const char* l1, const char* l2) {
  ui_panel(12, 50, 216, l2 ? 60 : 50, UI_PANEL, UI_BORDER);
  ui_text(20, 58, ink, title);
  ui_hline(16, 70, 208, UI_BORDER);
  if (l1) ui_text(20, 76, UI_TEXT, l1);
  if (l2) ui_text(20, 86, UI_TEXT, l2);
  ui_text(20, l2 ? 96 : 86, UI_DIM, "A ok");
  s_wait(KEY_A | KEY_B);
}

typedef struct { uint32_t gen; int top, sel; bool valid; } GbBnPaint;

static void bn_row_paint(const GbSession* s, int box, int y, bool sel) {
  char nm[GB_TEXT_MAX]; nm[0] = 0;
  gbbn_read(s, box, nm, sizeof nm);
  char l[32];
  /* "BOX 1: " (7) + up to 8 glyphs -- well inside the 232 px row width. */
  siprintf(l, "BOX %-2d: %s", box + 1, nm);
  ui_fill_rect(4, y, 232, UI_ROW_H, UI_BG);
  ui_text_sel(4, y, 232, sel, UI_TEXT, l);
}

void pdna_gb_boxnames(GbSession* s, bool can_edit) {
  if (!gbbn_supported(s)) {
    s_msg("BOX NAMES", UI_DIM, "Gen 1 boxes have no names", "of their own.");
    return;
  }
  rmbl_fire(RCUE_ROOM);

  int sel = 0, top = 0;
  bool dirty = false;
  bool renamed = false;
  const int vis = 12;
  GbBnPaint pv;
  memset(&pv, 0, sizeof pv);

  for (;;) {
    if (sel < top) top = sel;
    else if (sel >= top + vis) top = sel - vis + 1;
    if (top > G2_NUM_BOXES - vis) top = G2_NUM_BOXES - vis;
    if (top < 0) top = 0;

    bool full = !pv.valid || pv.gen != ui_clear_gen() || top != pv.top;

    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "BOX NAMES");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);

      for (int i = 0; i < vis && top + i < G2_NUM_BOXES; i++)
        bn_row_paint(s, top + i, 18 + i * 10, top + i == sel);

      ui_hline(0, 140, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, can_edit ? "A rename  B save+back" : "read-only (Omega)  B back");
    } else if (sel != pv.sel || renamed) {
      bn_row_paint(s, pv.sel, 18 + (pv.sel - top) * 10, false);
      bn_row_paint(s, sel,    18 + (sel    - top) * 10, true);
    }

    pv.top = top; pv.sel = sel; pv.gen = ui_clear_gen(); pv.valid = true;
    renamed = false;

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) break;
    if (k & KEY_UP)   sel = sel ? sel - 1 : G2_NUM_BOXES - 1;
    if (k & KEY_DOWN) sel = (sel + 1) % G2_NUM_BOXES;

    if (k & KEY_A) {
      if (!can_edit) {
        snd_deny();
        s_msg("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0);
        pv.valid = false;
        continue;
      }
      char cur[GB_TEXT_MAX]; cur[0] = 0;
      gbbn_read(s, sel, cur, sizeof cur);
      char buf[12];
      if (!osk_input("BOX NAME", cur[0] ? cur : "BOX", buf, 9)) continue;   /* B/cancel: no-op */
      GbsStatus st = gbbn_rename(s, sel, buf);
      if (st == GBS_OK) {
        dirty = true;
        renamed = true;
        rmbl_fire(RCUE_EDIT);
      } else {
        snd_error();
        s_msg("NAME REFUSED", UI_WARN, gbs_status_text(st), "That name was not used.");
        pv.valid = false;
      }
    }
  }

  if (dirty && app_confirm("Save box names?", "Writes the save file now."))
    gb_persist("boxname");
}
