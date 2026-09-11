/*
 * Gen-1/2 Pokedex + (Gen 2) Unown-forms screens (BACKLOG #87 item 3). See
 * pdna_gbdex.h for the caller contract and the UX-parity rule (reuses pdna_pick.c's
 * shared pdna_dex_screen() UNCHANGED -- this file is only the GB wiring).
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbdex.h"
#include "gb_dex.h"
#include "gb_edit.h"       /* gb_max_species */
#include "pdna_pick.h"     /* pdna_dex_screen, pdna_dex_set_max */
#include "pdna_gen12.h"    /* gb_persist -- the verified-write commit path            */
#include "pdna_trainer.h"  /* trainer_flag_row_paint / trainer_key_legend             */
#include "pdna_app.h"      /* app_confirm                                            */
#include "ui.h"
#include "snd.h"

static u16 s_wait(u16 mask) {
  u16 k, fresh;
  do {
    VBlankIntrWait(); snd_vblank(); key_poll();
    fresh = key_hit(mask);
    k = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT));
  } while (!k);
  if      (fresh & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  else if (fresh & KEY_A) snd_ok();
  else if (fresh & KEY_B) snd_back();
  return k;
}

/* ---- the get/set shims pdna_dex_screen() calls through -----------------------
 * File-static current session, same shape as pdna_main.c's own g_sb1/g_sb2/g_game
 * for the Gen-3 dex shims (dex_state/dex_set_state, pdna_main.c:5188-5193) -- the
 * DexGetState/DexSetState function pointer types carry no closure, so the session
 * has to live somewhere pdna_dex_screen's callback can reach without a parameter. */
static GbSession* s_gbdex_session;

static int gbdex_shim_get(int nat) {
  if (!s_gbdex_session) return 0;
  if (gbdex_get(s_gbdex_session, (uint16_t)nat, true)) return 2;    /* caught */
  return gbdex_get(s_gbdex_session, (uint16_t)nat, false) ? 1 : 0;  /* seen / none */
}

/* Does NOT call gbs_finish() -- gb_dex.h's own batching contract: pdna_dex_screen's
 * bulk Catch/See/Wipe-ALL op can call this up to gb_max_species() times in one user
 * gesture (dex_bulk's ALL loops); pdna_gbdex() below finishes ONCE after the whole
 * screen session, on confirm, matching the Gen-3 caller's own one-commit-at-exit
 * shape (pdna_main.c's pdna_dex_edit -> app_commit_dex, a single call after the
 * screen returns, never per A-press). */
static void gbdex_shim_set(int nat, int state) {
  if (!s_gbdex_session) return;
  gbdex_set(s_gbdex_session, (uint16_t)nat, true,  state >= 2);
  gbdex_set(s_gbdex_session, (uint16_t)nat, false, state >= 1);
}

/* ---- Unown forms (Gen 2 only): a 26-row A..Z toggle list, the plain PokeDNA list
 * idiom -- pdna_pick.c's own DV_LIST view geometry (x0=4, y0=24, row height 9, 13
 * visible rows) reproduced here as PLAIN NUMBERS, not shared code: the brief's own
 * acceptance gate restricts pdna_pick.c's diff to the item-1 cap only, so this row
 * list is entirely this file's own code, just drawn to match. trainer_flag_row_paint
 * (pdna_trainer.h, already public, already the "label ON/off" row every badge/flag
 * screen in this codebase uses) supplies the row painter itself. */
#define UNOWN_ROWS_VISIBLE 13
#define UNOWN_ROW_Y0       24
#define UNOWN_ROW_STEP     9

static void unown_clamp_scroll(int* sel, int* top) {
  if (*sel < 0) *sel = 0;
  if (*sel > 25) *sel = 25;
  if (*sel < *top) *top = *sel;
  if (*sel >= *top + UNOWN_ROWS_VISIBLE) *top = *sel - (UNOWN_ROWS_VISIBLE - 1);
  if (*top < 0) *top = 0;
  if (*top > 26 - UNOWN_ROWS_VISIBLE) *top = 26 - UNOWN_ROWS_VISIBLE;
}

static void unown_letter_label(int letter, char out[16]) {
  siprintf(out, "Letter %c", (char)('A' + letter));
}

static void unown_render(GbSession* s, int sel, int top, bool can_edit) {
  ui_clear();
  ui_text(4, 2, UI_TITLE, "UNOWN FORMS");
  ui_hline(0, 11, UI_SCR_W, UI_BORDER);
  for (int i = 0; i < UNOWN_ROWS_VISIBLE && top + i < 26; i++) {
    int letter = top + i;
    char lbl[16]; unown_letter_label(letter, lbl);
    trainer_flag_row_paint(lbl, gbdex_unown_seen(s, letter),
                           UNOWN_ROW_Y0 + i * UNOWN_ROW_STEP, letter == sel);
  }
  /* D3 (b87 fix pass, DO-NOT-SHIP review): a read-only cart must not advertise an
   * "A toggle" it will refuse -- the legend itself is the tell. */
  trainer_key_legend(can_edit ? "U/D select  A toggle  B back" : "U/D select  B back");
}

/* Returns true iff at least one letter's seen state actually changed. Does NOT call
 * gbs_finish() -- same batching contract as the dex shims above; pdna_gbdex() finishes
 * once after BOTH this screen and the Pokedex screen have had their turn.
 * D3 (b87 fix pass, DO-NOT-SHIP review): this screen used to ignore can_edit entirely
 * -- a read-only cart (Everdrive, or any cart pdna_app.h's app_can_edit() refuses)
 * could still flip Unown letters. Gated the same way pdna_dex_screen (the sibling
 * screen this file also drives) already gates its own edits. */
static bool unown_forms_screen(GbSession* s, bool can_edit) {
  int sel = 0, top = 0;
  bool dirty = false;
  unown_clamp_scroll(&sel, &top);
  unown_render(s, sel, top, can_edit);
  for (;;) {
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return dirty;
    if (k & KEY_UP)   sel--;
    if (k & KEY_DOWN) sel++;
    if (k & KEY_A) {
      if (!can_edit) {
        snd_deny();
      } else {
        bool now = gbdex_unown_seen(s, sel);
        GbsStatus st = gbdex_unown_set(s, sel, !now);
        if (st == GBS_OK && gbdex_unown_seen(s, sel) != now) dirty = true;
      }
    }
    unown_clamp_scroll(&sel, &top);
    unown_render(s, sel, top, can_edit);
  }
}

/* ---- the entry chooser (Gen 2 only): "Pokedex" / "Unown forms" ---------------
 * Gen 1 has neither Unown nor a choice to make -- pdna_gbdex() skips this chooser
 * entirely and opens pdna_dex_screen() directly for a Gen-1 session. Loops so a
 * visit can touch BOTH sub-screens (e.g. mark a species caught, then flip to Unown
 * forms) before the ONE confirm+commit at the very end -- never a confirm per
 * sub-screen, matching the Gen-3 caller's single end-of-visit commit. */
static bool gbdex_chooser(GbSession* s, bool can_edit) {
  enum { ROW_DEX = 0, ROW_UNOWN, ROW_N };
  static const char* const kLbl[ROW_N] = { "Pokedex", "Unown forms" };
  int sel = 0;
  bool dirty = false;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "POKEDEX (GEN 2)");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < ROW_N; i++) {
      int y = 24 + i * 9;
      if (i == sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      ui_text(6, y, i == sel ? UI_SELTEXT : UI_TEXT, kLbl[i]);
    }
    trainer_key_legend("U/D select  A choose  B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return dirty;
    if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : ROW_N - 1;
    if (k & KEY_DOWN) sel = (sel + 1) % ROW_N;
    if (k & KEY_A) {
      if (sel == ROW_DEX) {
        key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
        if (pdna_dex_screen(gbdex_shim_get, gbdex_shim_set, NULL, NULL, can_edit))
          dirty = true;
      } else {
        if (unown_forms_screen(s, can_edit)) dirty = true;
      }
    }
  }
}

bool pdna_gbdex(GbSession* s, bool can_edit) {
  if (!s || !s->open) return false;
  s_gbdex_session = s;

  pdna_dex_set_max((int)gb_max_species(s->gen));   /* Gen 1 -> 151, Gen 2 -> 251 */

  bool dirty;
  if (s->gen == GB_GEN2) {
    dirty = gbdex_chooser(s, can_edit);
  } else {
    key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    dirty = pdna_dex_screen(gbdex_shim_get, gbdex_shim_set, NULL, NULL, can_edit);
  }

  s_gbdex_session = NULL;

  if (!dirty) return false;
  /* D2 (b87 fix pass, DO-NOT-SHIP review): gbdex_shim_set/gbdex_unown_set write
   * straight into the session's image (g_ed->img via gbdex_set/gbdex_unown_set,
   * not a staging buffer) -- a decline here must DISCARD those bytes or a later,
   * unrelated persist (e.g. a different screen's own confirm-and-commit later in
   * the same session) would silently carry this screen's declined dex edits out
   * to the .sav too. gb_rollback() restores g_ed->img from g_ed->pristine, same
   * pattern every other decline path in pdna_gen12.c already uses. */
  if (!app_confirm("Save Pokedex changes?", "Writes the dex now.")) { gb_rollback(); return false; }

  GbsStatus st = gbs_finish(s);
  if (st != GBS_OK) { gb_rollback(); return false; }
  return gb_persist("dex");
}
