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
#include "pdna_pick.h"     /* pdna_dex_screen, pdna_dex_set_max, pdna_dex_set_cell_art */
#include "pdna_gen12.h"    /* gb_persist -- the verified-write commit path            */
#include "pdna_trainer.h"  /* trainer_flag_row_paint / trainer_key_legend             */
#include "pdna_app.h"      /* app_confirm                                            */
#include "pdna_origin_art.h" /* BACKLOG #124: pdna_origin_art_icon, pdna_origin_cell_render */
#include "dex_cell_art_rule.h" /* BACKLOG #124: the pure-C selection rule this callback gates on */
#include "ui.h"
#include "snd.h"

/* ---- BACKLOG #124: the dex screen's cell-art override, GB ROM sprites -------------
 * The dex grid's cell painter (dex_cell_grid(), pdna_pick.c, shared verbatim with the
 * Gen-3 dex) draws through mon_icon_for()/the icon store by default. That is a
 * PokeDNA-internal icon set, not the species' own Gen-1/2 ROM art -- the box grid two
 * screens away already shows the REAL Gen-2 party-menu icon for a GB-era cell
 * (pdna_origin_art.c's pdna_origin_box_art(), ERA_GEN2 branch, called from
 * pdna_box.c:~1343's era_cell_draw()). This installs the SAME fetch, reused (not
 * copied): pdna_origin_art_icon() is a new public entry point in pdna_origin_art.c
 * that factors that branch's own have()/stack-room/fetch/pack sequence
 * (pdna_origin_art_have(PDNA_GEN2) -> pdna_origin_art_stack_room(PDNA_GB_ICON_NEED) ->
 * fetch_pic_ex(...,icon=1,...) -> s_gb.icon() -> gb_art_icon_cb() ->
 * gb_art_fetch_icon()) into ONE function pdna_origin_box_art() now also calls, so the
 * dex becomes that chain's SECOND caller, never a second implementation.
 *
 * Reached through pdna_pick.h's pdna_dex_set_cell_art() process-wide override (its own
 * comment there has the full contract) rather than a new pdna_dex_screen() parameter,
 * because a parameter would touch the Gen-3 caller's (pdna_main.c's) call site for a
 * feature it never uses. GEN 1 ONLY REFUSES (Gen 1 has no menu icons,
 * gb_art_source.c's own rule) via the ctx-carried session gen check below, not by
 * skipping installation for a Gen-1 visit: installing at BOTH call sites
 * unconditionally and self-gating inside the callback means a Gen-1 session can never
 * accidentally borrow a SEPARATELY-registered Gen-2 ROM's icon for a same-numbered
 * Kanto species (both generations' ROMs may be registered at once, exactly like the
 * box grid tolerates for imports of both eras in one save) -- the callback's own `s->
 * gen != GB_GEN2` check is the actual safety boundary, not caller discipline. */

/* Split exactly like pdna_box.c's era_cell_draw()/era_cell_blit(): the scale+blit
 * buffer (2,048 B for a 32x32 RGB15 cell) must never coexist on the stack with the
 * fetch chain's own frame (gb_art_fetch_icon's ~5.6 KB tail, gated on
 * PDNA_GB_ICON_NEED=6,144) -- noinline so an inlined copy cannot silently merge the
 * two frames back together. Called only AFTER pdna_origin_art_icon()'s fetch has
 * already returned and popped. */
static bool __attribute__((noinline))
gbdex_cell_blit(const PdnaArt* a, int x, int y, int w, int h) {
  u16 cell[32 * 32];             /* 2,048 B of STACK -- never a static, never EWRAM */
  if (w <= 0 || h <= 0 || w > 32 || h > 32) return false;   /* validate: the only
                                                             * caller passes 32x32
                                                             * today, but this buffer
                                                             * cannot cover more */
  if (!pdna_origin_cell_render(a, cell, w, h)) return false;
  ui_sprite(x, y, w, h, cell);
  return true;
}

/* The PdnaDexCellArtFn itself: `ctx` is the GbSession* the caller is visiting (never
 * NULL -- pdna_gbdex() below only installs this while `s` is in scope). No fetch is
 * attempted for a Gen-1 session (see the file header comment above for why this check
 * lives here and not at the install site) or when the species is out of the Gen-1/2
 * range (`dex` is pdna_pick.c's own pk_national_no() result, so this is defensive, not
 * load-bearing -- pdna_dex_set_max() already caps the visible list at 151/251).
 *
 * dex_cell_art_source() (BACKLOG #124, source/dex_cell_art_rule.h) is the pure-C gate:
 * this function computes the three real-world inputs and asks the RULE what to do,
 * rather than encoding the decision inline -- tests/host_dexcellart_test.c exercises
 * the exact same function against the whole truth table, so a review can check the
 * gate's LOGIC on the host without a GBA build. `store_ok` is always true here (the
 * caller, pdna_pick.c's dex_cell_grid(), always has its own unchanged fallback ready
 * for a `false` return) -- passed explicitly so the rule states its whole contract. */
static bool gbdex_cell_art(uint16_t dex, int x, int y, int w, int h, void* ctx) {
  const GbSession* s = (const GbSession*)ctx;
  bool gb_session = s && s->gen == GB_GEN2;
  bool gb_have = gb_session && pdna_origin_art_have(PDNA_GEN2);
  if (dex_cell_art_source(gb_session, gb_have, /*store_ok=*/true) != DEX_CELL_ART_GB)
    return false;
  if (dex < 1 || dex > 251) return false;
  PdnaArt a;
  if (!pdna_origin_art_icon(dex, &a) || !a.px) return false;
  return gbdex_cell_blit(&a, x, y, w, h);
}

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

/* Returns true iff the raw wUnownDex bytes at exit differ from the bytes at entry.
 * Does NOT call gbs_finish() -- same batching contract as the dex shims above;
 * pdna_gbdex() finishes once after BOTH this screen and the Pokedex screen have had
 * their turn.
 * D3 (b87 fix pass, DO-NOT-SHIP review): this screen used to ignore can_edit entirely
 * -- a read-only cart (Everdrive, or any cart pdna_app.h's app_can_edit() refuses)
 * could still flip Unown letters. Gated the same way pdna_dex_screen (the sibling
 * screen this file also drives) already gates its own edits.
 * N2 (b87 fix pass, DO-NOT-SHIP review): dirty used to be an OR of every individual
 * toggle's own before/after change -- cycling a letter on then off again in the same
 * visit left dirty stuck true even though the net state matched what the session
 * started with, forcing an unnecessary "Save Pokedex changes?" prompt and write.
 * R2 (b87 fix pass 2, DO-NOT-SHIP review, MUTATION-PROVEN): N2's own fix compared
 * gbdex_unown_seen()'s per-letter MEMBERSHIP (26 bools), but wUnownDex is an ORDERED
 * list -- toggling a letter off then back on REMOVES then RE-APPENDS it (it moves
 * from its old slot to whatever the first empty slot now is), which changes the raw
 * bytes and the in-game Unown-page order without changing which 26 letters read
 * "seen". That left a genuine edit reporting clean -- neither committed (no confirm
 * prompt fired) nor rolled back (the reordered bytes stayed staged in the session
 * image for whatever LATER, unrelated persist came next). Fixed: gbdex_unown_list()
 * snapshots/compares the raw 26 bytes (same memcmp-no-op shape pdna_gbtrainer.c's
 * own card-commit path uses, `if (!memcmp(&t, &t0, sizeof t)) ... return`), not
 * membership. */
static bool unown_forms_screen(GbSession* s, bool can_edit) {
  uint8_t snap[26];
  bool have_snap = gbdex_unown_list(s, snap);

  int sel = 0, top = 0;
  unown_clamp_scroll(&sel, &top);
  unown_render(s, sel, top, can_edit);
  for (;;) {
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) {
      uint8_t now[26];
      if (!have_snap || !gbdex_unown_list(s, now)) return false;
      return memcmp(now, snap, sizeof now) != 0;
    }
    if (k & KEY_UP)   sel--;
    if (k & KEY_DOWN) sel++;
    if (k & KEY_A) {
      if (!can_edit) {
        snd_deny();
      } else {
        bool now = gbdex_unown_seen(s, sel);
        (void)gbdex_unown_set(s, sel, !now);
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
        /* BACKLOG #124: installed tightly around this one call, not the whole chooser
         * loop -- unown_forms_screen() (the ROW_UNOWN branch below) never needs it, and
         * a bracket that outlived this call would still be live (with `s`, a stack
         * pointer this function received, as ctx) after gbdex_chooser() itself
         * returns. */
        pdna_dex_set_cell_art(gbdex_cell_art, s);
        bool dex_dirty = pdna_dex_screen(gbdex_shim_get, gbdex_shim_set, NULL, NULL, can_edit);
        pdna_dex_set_cell_art(NULL, NULL);
        if (dex_dirty) dirty = true;
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
    /* BACKLOG #124: installed here too (a Gen-1 visit) so gbdex_cell_art's own
     * `s->gen != GB_GEN2` check is the ONE place that decides Gen 1 gets no GB-ROM
     * icons -- see this file's header comment for why that must not be "just don't
     * install it here" (a separately-registered Gen-2 ROM must not leak into a Gen-1
     * dex's Kanto-range cells). */
    pdna_dex_set_cell_art(gbdex_cell_art, s);
    dirty = pdna_dex_screen(gbdex_shim_get, gbdex_shim_set, NULL, NULL, can_edit);
    pdna_dex_set_cell_art(NULL, NULL);
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
