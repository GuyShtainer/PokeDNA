/* SPDX-License-Identifier: GPL-3.0-or-later
 * pdna_hist.c -- the HISTORY screen (BACKLOG #234 slice 3, design D7): the current branch of the undo journal, newest
 * first, with the step the image sits on, the floors (crossed steps undo/redo cannot pass) and recorded-vs-SAVED per
 * step. A = jump (a chained undo/redo through the engine's cursor rule, stopping at floors), START = redo everything
 * reachable (the T2 re-apply surface), B = back.
 *
 * Built ONLY on jrn_app.h (rows) and pdna_app.h (the jump, which re-derives the decoded copies), so it carries no
 * Gen-3 layout: slice 4 hosts the same screen for a Game Boy session by giving the journal a GB image. The rows live
 * in the shared EWRAM borrow (app_box_swap_acquire): no static of its own. */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "jrn_app.h"
#include "pdna_app.h"
#include "snd.h"
#include "ui.h"

#define HH_MAX   48      /* rows in the window (the newest 48 steps of the branch)            */
#define HH_Y0    17      /* first row's y                                                      */
#define HH_RH    11      /* row pitch                                                          */
#define HH_VIS   12      /* rows that fit above the footer                                     */

static void h_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16 h_wait(u16 mask) {
  u16 k, fresh;
  do { h_vsync(); fresh = key_hit(mask); k = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN)); } while (!k);
  if (fresh & KEY_A) snd_ok(); else if (fresh & KEY_B) snd_back(); else snd_move();
  return k;
}

/* One row. `n` real rows, then (when the window reaches the very first step) a pseudo row "before any step". */
static void h_row(const JaHist* rows, int n, int i, int top, bool sel, bool has_root) {
  int y = HH_Y0 + (i - top) * HH_RH;
  u16 ink;
  char buf[32];
  if (i - top < 0 || i - top >= HH_VIS) return;
  ui_fill_rect(0, y - 1, UI_SCR_W, HH_RH, UI_BG);
  if (sel) ui_panel(1, y - 1, UI_SCR_W - 2, HH_RH, UI_SEL, UI_TITLE);
  if (i < n) {
    const JaHist* r = &rows[i];
    ink = sel ? UI_SELTEXT : (r->ahead ? UI_DIM : UI_TEXT);
    if (r->at_cursor) ui_ptext(4, y, sel ? UI_SELTEXT : UI_TITLE, ">");
    ui_ptext_fit(13, y, 118, ink, r->name[0] ? r->name : "(step)");
    if (r->crossed) ui_ptext(134, y, UI_WARN, "FLOOR");
    siprintf(buf, "%s", r->saved ? "SAVED" : (r->ahead ? "undone" : "recorded"));
    ui_ptext_right(UI_SCR_W - 6, y, r->saved ? UI_OK : (sel ? UI_SELTEXT : UI_WARN), buf);
  } else if (has_root) {
    ink = sel ? UI_SELTEXT : UI_DIM;
    ui_ptext_fit(13, y, 200, ink, "-- before the first recorded step --");
  }
}

static void h_paint_all(const JaHist* rows, int n, int total, int sel, int top, bool has_root, bool retired) {
  char hd[32];
  ui_clear();
  ui_text(4, 2, UI_TITLE, "HISTORY");
  siprintf(hd, "%d step%s%s", n, n == 1 ? "" : "s", (retired || total > n) ? "+" : "");
  ui_ptext_right(UI_SCR_W - 6, 3, UI_DIM, hd);
  ui_hline(0, 13, UI_SCR_W, UI_BORDER);
  for (int i = top; i < top + HH_VIS && i < total; i++) h_row(rows, n, i, top, i == sel, has_root);
  ui_text(4, 152, UI_DIM, "A jump  START newest  B back");
}

/* The result of a jump/redo-all, in words. AUR_DONE = arrived (nothing to say). */
static void h_say(int rc, int moved, const char* stop) {
  char l1[40];
  if (rc == AUR_DONE) return;
  if (rc == AUR_FLOOR) {
    siprintf(l1, "Stopped at %.24s.", stop[0] ? stop : "a floor");
    msg_wait("STOPPED AT A FLOOR", UI_WARN, l1, moved ? "It crossed files: do it by hand." : "Nothing moved (it crossed files).");
  } else if (rc == AUR_DIVERGED) {
    msg_wait("HISTORY DIVERGED", UI_WARN, "History diverged here: the save", "no longer matches this step.");
  } else if (rc == AUR_ARENA) {
    msg_wait("NOT NOW", UI_WARN, "The box data is on loan to", "another screen. Leave and retry.");
  } else if (rc == AUR_NOTHING) {
    msg_wait("HISTORY", UI_OK, "Already there.", 0);
  } else {
    msg_wait("HISTORY", UI_WARN, "Could not move there.", "Nothing further was changed.");
  }
}

void pdna_history_screen(void) {
  JaHist* rows = (JaHist*)app_box_swap_acquire(sizeof(JaHist) * HH_MAX);
  int sel = 0, top = 0, n = 0, more = 0, floor_hit = 0;
  bool refetch = true, has_root;
  if (!rows) { msg_wait("HISTORY", UI_WARN, "Not enough memory right now.", 0); return; }
  for (;;) {
    int total;
    if (refetch) {
      n = jrnapp_history(rows, HH_MAX, &more, &floor_hit);
      refetch = false;
      if (sel >= n + 1) sel = n;
    }
    if (n == 0) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "HISTORY");
      ui_hline(0, 13, UI_SCR_W, UI_BORDER);
      ui_ptext(8, 40, UI_TEXT, app_undo_live() ? "Nothing recorded in this save yet." : "History is off for this save.");
      ui_ptext(8, 54, UI_DIM, app_undo_live() ? "Edits you make are recorded here." : "(read-only card, or no journal.)");
      ui_text(4, 152, UI_DIM, "B back");
      (void)h_wait(KEY_B);
      break;
    }
    has_root = !more && !floor_hit;                          /* the window reaches the first step: "before it" is a valid target */
    total = n + (has_root ? 1 : 0);
    if (sel >= total) sel = total - 1;
    if (sel < top) top = sel;
    if (sel >= top + HH_VIS) top = sel - HH_VIS + 1;
    h_paint_all(rows, n, total, sel, top, has_root, more || floor_hit);
    for (;;) {
      u16 k = h_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START);
      if (k & KEY_B) { app_box_swap_release(); return; }
      if (k & KEY_UP)   { if (sel > 0) sel--; else sel = total - 1; }
      else if (k & KEY_DOWN) { sel = (sel + 1) % total; }
      else if (k & (KEY_A | KEY_START)) {
        char stop[25];
        int moved = 0, rc;
        uint32_t target = (k & KEY_START) ? rows[0].seq : (sel < n ? rows[sel].seq : 0u);
        rc = app_history_jump(target, stop, &moved);
        refetch = true;
        h_say(rc, moved, stop);
        break;
      }
      if (sel < top) top = sel;
      if (sel >= top + HH_VIS) top = sel - HH_VIS + 1;
      h_paint_all(rows, n, total, sel, top, has_root, more || floor_hit);
    }
  }
  app_box_swap_release();
}
