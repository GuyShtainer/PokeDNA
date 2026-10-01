/* SPDX-License-Identifier: GPL-3.0-or-later
 * pdna_hist.c -- the HISTORY screen (BACKLOG #234 slice 3, design D7): the current branch of the undo journal, newest
 * first, with the step the image sits on, the floors (crossed steps undo/redo cannot pass) and recorded-vs-SAVED per
 * step. A = jump (a chained undo/redo through the engine's cursor rule, stopping at floors), START = redo everything
 * reachable (the T2 re-apply surface), B = back.
 *
 * THE TREE (#304, display only): a step whose parent is a fork point (other steps leave it too -- the ones an undo-then-new-step orphaned)
 * is followed by ONE collapsed row "+ N other branches"; A on it opens it (the other branches' first steps, set off with a "|" and dim) and A
 * again closes it. A on one of those sibling rows does NOTHING but a deny tone and a one-line footer note: only the current branch is a
 * jump target (jumping onto another branch is new apply semantics and waits for Guy). A on a plain step is the jump, exactly as before.
 *
 * Built ONLY on jrn_app.h (rows) and pdna_app.h (the jump, which re-derives the decoded copies), so it carries no
 * Gen-3 layout: slice 4 hosts the same screen for a Game Boy session (pdna_history_screen_rows over the GB arena tail).
 * The Gen-3 rows live in the shared EWRAM borrow (app_box_swap_acquire): no static of its own. */
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
static u16 h_wait(u16 mask, bool deny_a) {                  /* deny_a: A is refused here (a sibling-branch row): the deny buzz, never the confirm blip */
  u16 k, fresh;
  do { h_vsync(); fresh = key_hit(mask); k = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN)); } while (!k);
  if (fresh & KEY_A) { if (deny_a) snd_deny(); else snd_ok(); } else if (fresh & KEY_B) snd_back(); else snd_move();
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
    if (r->kind == JH_FORK) {                                  /* the collapsed / opened summary of the other branches at this fork point */
      ink = sel ? UI_SELTEXT : UI_DIM;
      siprintf(buf, "%c %u other branch%s", r->open ? '-' : '+', (unsigned)r->nsib, r->nsib == 1 ? "" : "es");
      ui_ptext_fit(13, y, 150, ink, buf);
      if (r->open && r->nsib > JA_SIB_SHOW) ui_ptext_right(UI_SCR_W - 6, y, UI_DIM, "first 8");
      return;
    }
    if (r->kind == JH_SIB) {                                   /* another branch's first step: set off (indent + bar + dim), never a jump target */
      ink = sel ? UI_SELTEXT : UI_DIM;
      ui_ptext(15, y, ink, "|");
      ui_ptext_fit(22, y, 108, ink, r->name[0] ? r->name : "(step)");
      if (r->crossed) ui_ptext(134, y, UI_WARN, "FLOOR");
      ui_ptext_right(UI_SCR_W - 6, y, r->saved ? UI_OK : ink, r->saved ? "SAVED" : "other branch");
      return;
    }
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

/* The footer's one line: a denial `note` (until the next key), else the hint for what A does on the selected row. */
static const char* h_hint(const JaHist* rows, int n, int sel) {
  if (sel < n && rows[sel].kind == JH_FORK) return rows[sel].open ? "A hide  START newest  B back" : "A show  START newest  B back";
  if (sel < n && rows[sel].kind == JH_SIB) return "A -  START newest  B back";
  return "A jump  START newest  B back";
}

static void h_paint_all(const JaHist* rows, int n, int total, int sel, int top, bool has_root, bool retired, const char* note) {   /* retired: older steps exist past the window (or were retired) */
  char hd[32];
  int steps = 0;
  for (int i = 0; i < n; i++) if (rows[i].kind == JH_STEP) steps++;
  ui_clear();
  ui_text(4, 2, UI_TITLE, "HISTORY");
  siprintf(hd, "%d step%s%s", steps, steps == 1 ? "" : "s", retired ? "+" : "");
  ui_ptext_right(UI_SCR_W - 6, 3, UI_DIM, hd);
  ui_hline(0, 13, UI_SCR_W, UI_BORDER);
  for (int i = top; i < top + HH_VIS && i < total; i++) h_row(rows, n, i, top, i == sel, has_root);
  if (note) ui_text(4, 152, UI_WARN, note); else ui_text(4, 152, UI_DIM, h_hint(rows, n, sel));
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
  } else if (rc == AUR_PARTIAL) {
    msg_wait("PARTIAL STEP", UI_WARN, "A step was only partly applied:", "exit without saving it.");
  } else if (rc == AUR_NOTHING) {
    msg_wait("HISTORY", UI_OK, "Already there.", 0);
  } else {
    msg_wait("HISTORY", UI_WARN, "Could not move there.", "Nothing further was changed.");
  }
}

/* The screen over caller-owned `rows` (`max` rows: the newest `max` steps of the branch). Never touches the row buffer's
 * owner: the Gen-3 wrapper below borrows it from the file browser's entries, the Game Boy session from its arena tail. */
void pdna_history_screen_rows(JaHist* rows, int max) {
  int sel = 0, top = 0, n = 0, more = 0, floor_hit = 0, nopen = 0;
  uint32_t open_forks[JA_OPEN_MAX];                          /* the fork points whose other branches are shown (display state only) */
  bool refetch = true, has_root;
  const char* note = 0;
  if (!rows || max < 1) return;
  for (;;) {
    int total;
    if (refetch) {
      n = jrnapp_history_tree(rows, max, &more, &floor_hit, open_forks, nopen);
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
      (void)h_wait(KEY_B, false);
      return;
    }
    has_root = !more && !floor_hit;                          /* the window reaches the first step: "before it" is a valid target */
    total = n + (has_root ? 1 : 0);
    if (sel >= total) sel = total - 1;
    if (sel < top) top = sel;
    if (sel >= top + HH_VIS) top = sel - HH_VIS + 1;
    h_paint_all(rows, n, total, sel, top, has_root, more || floor_hit, note);
    note = 0;
    for (;;) {
      bool on_sib = sel < n && rows[sel].kind == JH_SIB;
      u16 k = h_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START, on_sib);
      if (k & KEY_B) return;
      if (k & KEY_UP)   { if (sel > 0) sel--; else sel = total - 1; }
      else if (k & KEY_DOWN) { sel = (sel + 1) % total; }
      else if ((k & KEY_A) && sel < n && rows[sel].kind == JH_FORK) {   /* open / close this fork: DISPLAY state only, nothing is applied */
        uint32_t fp = rows[sel].parent;
        int at = -1;
        for (int q = 0; q < nopen; q++) if (open_forks[q] == fp) at = q;
        if (at >= 0) { for (int q = at; q + 1 < nopen; q++) open_forks[q] = open_forks[q + 1]; nopen--; }
        else {
          if (nopen >= (int)JA_OPEN_MAX) { for (int q = 0; q + 1 < nopen; q++) open_forks[q] = open_forks[q + 1]; nopen--; }   /* the oldest-opened fork closes */
          open_forks[nopen++] = fp;
        }
        refetch = true;
        break;
      }
      else if ((k & KEY_A) && on_sib) { note = "Other branch - not jumpable."; break; }   /* the deny tone already played (h_wait); no jump onto a sibling */
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
      h_paint_all(rows, n, total, sel, top, has_root, more || floor_hit, 0);
    }
  }
}

void pdna_history_screen(void) {
  JaHist* rows = (JaHist*)app_box_swap_acquire(sizeof(JaHist) * HH_MAX);
  if (!rows) { msg_wait("HISTORY", UI_WARN, "Not enough memory right now.", 0); return; }
  pdna_history_screen_rows(rows, HH_MAX);
  app_box_swap_release();
}
