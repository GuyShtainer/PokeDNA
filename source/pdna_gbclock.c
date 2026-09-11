/*
 * Gen-2's own "Clock fix" screen -- BACKLOG #86/#108. See pdna_gbclock.h for the full
 * design rationale (why this is NOT pdna_clock()'s Gen-3 absolute-sync/manual-set
 * shape, and the exact three rows this offers instead, straight from gb_clock.h's own
 * header). This file only ever draws plain rows (no GB-screen-shell art), same
 * posture as the Gen-3 screen it mirrors -- source/pdna_main.c's pdna_clock().
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbclock.h"
#include "gb_clock.h"
#include "pdna_gen12.h"    /* gb_persist -- the verified-write commit path            */
#include "pdna_layout.h"   /* PDNA_GBCLOCK_*                                          */
#include "pdna_trainer.h"  /* trainer_row_paint / trainer_key_legend                  */
#include "ui.h"
#include "snd.h"
#include "pdna_app.h"      /* msg_wait / app_confirm                                  */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* ---- the view-only readout at the top -------------------------------------------
 * Every value here is VIEW-ONLY (gb_clock.h's own contract): the offsets, the raw
 * day count + its mod-7 weekday, and the flag state. Nothing on this band is ever
 * written by this screen -- only the three rows below it are. */
static void gbclock_header(const GbClock* c) {
  ui_text(4, 4, UI_TITLE, PDNA_GBCLOCK_TITLE);
  ui_hline(0, 14, UI_SCR_W, UI_BORDER);

  char b[40];
  siprintf(b, "Offset  %ud %02u:%02u:%02u", (unsigned)c->rtc_offset_day,
          (unsigned)c->rtc_offset_hour, (unsigned)c->rtc_offset_minute,
          (unsigned)c->rtc_offset_second);
  ui_text(6, 22, UI_TEXT, b);

  siprintf(b, "Day count  %u at last save", (unsigned)c->day_count);   /* wCurDay is a snapshot: the game recomputes it from the RTC every UpdateTime, so no weekday here (b86 review D1) */
  ui_text(6, 32, UI_TEXT, b);

  if (c->status_flags_ok) {
    siprintf(b, "Clock flag  %s", c->status_flags ? "asks next load" : "clear");
    ui_text(6, 42, c->status_flags ? UI_WARN : UI_OK, b);
  } else {
    ui_text(6, 42, UI_DIM, "Clock flag  unreadable");
  }
}

enum { ROW_RESET = 0, ROW_SHIFT, ROW_CLEAR, ROW_N };
static const char* const kRowLbl[ROW_N] = {
  PDNA_GBCLOCK_ROW_RESET, PDNA_GBCLOCK_ROW_SHIFT, PDNA_GBCLOCK_ROW_CLEAR,
};

/* pdna_trainer.h's shared row painter ("%-6s %s") prints the WHOLE label untruncated
 * when it is longer than 6 chars (a plain %-6s pads a short string, never truncates a
 * long one) -- these three rows are action sentences, not "label value" pairs, so the
 * empty value column is intentional. */
static void gbclock_row_paint(int i, int y, bool sel) {
  trainer_row_paint(y, sel, kRowLbl[i], "", UI_TEXT);
}

typedef struct { int sel; bool valid; uint32_t gen; } ClockPaint;

static void gbclock_render(const GbClock* c, int sel, bool can_edit, ClockPaint* pv) {
  bool full = !pv->valid || pv->gen != ui_clear_gen();
  if (full) {
    ui_clear();
    gbclock_header(c);
    for (int i = 0; i < ROW_N; i++) gbclock_row_paint(i, 60 + i * 16, i == sel);
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    trainer_key_legend(can_edit ? "U/D select  A choose  B back" : "U/D select  B back");
  } else if (sel != pv->sel) {
    gbclock_row_paint(pv->sel, 60 + pv->sel * 16, false);
    gbclock_row_paint(sel,     60 + sel     * 16, true);
  }
  pv->sel = sel; pv->gen = ui_clear_gen(); pv->valid = true;
}

/* ---- Row 1: "Ask for the time at next load" -- gbc_request_time_reset() ---------
 * Reads before/after and skips gb_persist() when the flag did not actually move
 * (already 0x80 -- gbc_request_time_reset()'s own no-op contract) -- hard rule 3:
 * a no-op writes nothing, not even a redundant identical SD write. */
static void gbclock_do_reset(GbSession* s) {
  if (!app_confirm(PDNA_GBCLOCK_CONFIRM_RESET_TITLE, PDNA_GBCLOCK_CONFIRM_RESET_L1)) return;
  GbClock before; if (!gbc_read(s, &before)) return;
  GbsStatus st = gbc_request_time_reset(s);
  if (st != GBS_OK) { msg_wait("REFUSED", UI_WARN, gbs_status_text(st), 0); return; }
  GbClock after; if (!gbc_read(s, &after)) return;
  if (after.status_flags == before.status_flags) { snd_back(); return; }   /* already set */
  gb_persist("gbclock reset");
}

/* ---- Row 3: "Clear the clock-error flag" -- gbc_clear_status_flags() ------------
 * Same before/after no-op gate as the reset row above. */
static void gbclock_do_clear(GbSession* s) {
  if (!app_confirm(PDNA_GBCLOCK_CONFIRM_CLEAR_TITLE, PDNA_GBCLOCK_CONFIRM_CLEAR_L1)) return;
  GbClock before; if (!gbc_read(s, &before)) return;
  GbsStatus st = gbc_clear_status_flags(s);
  if (st != GBS_OK) { msg_wait("REFUSED", UI_WARN, gbs_status_text(st), 0); return; }
  GbClock after; if (!gbc_read(s, &after)) return;
  if (after.status_flags == before.status_flags) { snd_back(); return; }   /* already clear */
  gb_persist("gbclock clear");
}

/* ---- Row 2: "Shift the clock" -- gbc_shift() -------------------------------------
 * Signed +-days/hours/minutes (seconds always 0 -- not offered on this screen, the
 * same posture pdna_clock()'s own manual entry takes with sub-minute precision).
 * Ranges clamp rather than wrap: a delta is a one-shot nudge, not a clock face, so
 * there is nothing to gain from spinning past the cap. */
enum { SF_DAYS = 0, SF_HOURS, SF_MINUTES, SF_N };
static const char* const kShiftLbl[SF_N] = { "Days", "Hours", "Minutes" };
static const int kShiftCap[SF_N] = { 99, 23, 59 };

static void shift_field_paint(int i, int v, bool sel) {
  char r[24]; siprintf(r, "%-8s%+4d", kShiftLbl[i], v);
  int y = 30 + i * 16;
  ui_fill_rect(2, y - 2, 150, 13, UI_BG);
  if (sel) ui_panel(2, y - 2, 150, 13, UI_SEL, UI_TITLE);
  ui_text(10, y, sel ? UI_SELTEXT : UI_TEXT, r);
}

/* All-zero deltas never reach app_confirm or gbc_shift() -- nothing would change,
 * so both B and a zero-delta A exit this editor the same way (hard rule 3: a no-op
 * writes nothing, and here it does not even ask the question). */
__attribute__((noinline))
static void gbclock_shift_editor(GbSession* s) {
  int v[SF_N] = { 0, 0, 0 }, f = 0;
  int pv_v[SF_N] = { 0, 0, 0 }, pv_f = -1; bool valid = false; uint32_t gen = 0;
  key_repeat_mask(KEY_UP | KEY_DOWN);
  for (;;) {
    bool full = !valid || gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "SHIFT THE CLOCK");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < SF_N; i++) shift_field_paint(i, v[i], i == f);
      /* D1 (b86 shots review): the old 2-line wrap's second line ("offset -- never an
       * absolute time.", 33 chars) ran off the 240 px screen at x=6 -- ui_text is the
       * fixed 8 px/glyph sys8 face (29-char budget at this x, same as pdna_mirage()'s
       * own party rows, source/pdna_main.c) and clips SILENTLY on hardware, not with
       * an ellipsis. Three shorter lines instead, each measured to fit (28/23/14 chars). */
      ui_text(6, 118, UI_DIM, "Adds this to the clock's own");
      ui_text(6, 128, UI_DIM, "offset -- never sets an");
      ui_text(6, 138, UI_DIM, "absolute time.");
      ui_text(4, 152, UI_DIM, PDNA_GBCLOCK_SHIFT_KEYS);
    } else {
      for (int i = 0; i < SF_N; i++) {
        bool sel = (i == f), osel = (i == pv_f);
        if (v[i] != pv_v[i] || sel != osel) shift_field_paint(i, v[i], sel);
      }
    }
    for (int i = 0; i < SF_N; i++) pv_v[i] = v[i];
    pv_f = f; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) break;
    else if (k & KEY_LEFT)  f = (f > 0) ? f - 1 : SF_N - 1;
    else if (k & KEY_RIGHT) f = (f + 1) % SF_N;
    else if (k & (KEY_UP | KEY_DOWN)) {
      int dir = (k & KEY_UP) ? 1 : -1;
      int nv = v[f] + dir;
      if (nv > kShiftCap[f]) nv = kShiftCap[f];
      if (nv < -kShiftCap[f]) nv = -kShiftCap[f];
      v[f] = nv;
    } else if (k & KEY_A) {
      if (v[SF_DAYS] == 0 && v[SF_HOURS] == 0 && v[SF_MINUTES] == 0) { snd_back(); break; }
      char l1[48];
      siprintf(l1, "%+dd %+dh %+dm", v[SF_DAYS], v[SF_HOURS], v[SF_MINUTES]);
      if (app_confirm(PDNA_GBCLOCK_CONFIRM_SHIFT_TITLE, l1)) {
        GbsStatus st = gbc_shift(s, v[SF_DAYS], v[SF_HOURS], v[SF_MINUTES], 0);
        if (st != GBS_OK)
          msg_wait("SHIFT REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
        else
          gb_persist("gbclock shift");   /* nonzero delta always moves at least one byte */
      }
      break;
    }
  }
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);   /* restore the global set */
}

/* ------------------------------------------------------------------------------- */

void pdna_gbclock(GbSession* s, bool can_edit) {
  GbClock c;
  if (!s || !s->open || !gbc_read(s, &c)) {
    msg_wait("CLOCK", UI_WARN, "Could not read this save.", 0);
    return;
  }
  if (!c.present) {   /* Gen 1, or a GS/Crystal save missing the field -- honest, no dead end */
    ui_clear();
    ui_text(4, 4, UI_TITLE, PDNA_GBCLOCK_TITLE);
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    ui_text(6, 44, UI_DIM, PDNA_GBCLOCK_NOCLOCK_L1);
    ui_text(4, 152, UI_DIM, "B back");
    s_wait(KEY_B);
    return;
  }

  int sel = 0;
  ClockPaint pv; memset(&pv, 0, sizeof pv);
  for (;;) {
    gbc_read(s, &c);   /* re-read every entry: a row action below may have changed it */
    gbclock_render(&c, sel, can_edit, &pv);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : ROW_N - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % ROW_N;
    else if ((k & KEY_A) && can_edit) {
      switch (sel) {
        case ROW_RESET: gbclock_do_reset(s);    break;
        case ROW_SHIFT: gbclock_shift_editor(s); break;
        case ROW_CLEAR: gbclock_do_clear(s);    break;
      }
      pv.valid = false;   /* force a full repaint: the readout band may have changed */
    }
  }
}
