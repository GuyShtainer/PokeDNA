/*
 * Gen-1's OWN Item bag / PC item store -- U4, BACKLOG #67. See pdna_gbbag.h for
 * the full ground-truth comment (box geometry, tile ids, scroll behaviour, the
 * nested EXIT box, the "no item-name table" deviation). This file only ever
 * builds the Gen-1 real screen; a Gen-2 caller (not reached by this slice) or
 * a shell refusal both fall through to the plain row-list page below.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbbag.h"
#include "gb_bag.h"
#include "pdna_gen12.h"     /* gb_rollback / gb_persist / gb12_arena_tail(_release)   */
#include "pdna_gbscreen.h"  /* the shared GB-screen shell                             */
#include "pdna_origin_art.h" /* PDNA_GEN1                                             */
#include "pdna_layout.h"    /* PDNA_GBSCR_ACT_*, PDNA_GBTR_ACT_*, GBTR_HEADER2_MAXW   */
#include "pdna_trainer.h"   /* num_entry                                              */
#include "ui.h"
#include "snd.h"
#include "pdna_app.h"       /* msg_wait / app_confirm                                 */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* ============================================================================
 * ---- the plain row-list fallback (BACKLOG #67's own "no dead end" rule) --
 * a simple scrolling rows page over ONE pocket at a time, ITEM #n names (no
 * ROM/no names table needed), reached when the shell itself refuses.
 * ============================================================================ */

static void gbbag_row_paint(const GbBag* bag, GbBagPocket pocket, int row, int y, bool sel) {
  const GbBagList* l = &bag->pockets[pocket];
  char lbl[16], val[16];
  if (row >= l->count) { lbl[0] = 0; val[0] = 0; }
  else {
    const GbBagEntry* e = &l->entries[row];
    siprintf(lbl, "ITEM #%u", (unsigned)e->id);
    if (pocket == GBB_POCKET_KEY) siprintf(val, "-");
    else                          siprintf(val, "x%u", (unsigned)e->qty);
  }
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y - 1, 236, 9, UI_BG);
  ui_text(6, y, sel ? UI_SELTEXT : UI_TEXT, lbl);
  char valp[24]; siprintf(valp, "%-8s", val);
  ui_text(6 + 12 * 8, y, sel ? UI_SELTEXT : UI_TEXT, valp);
}

/* Returns true only when the caller should offer a commit prompt (B, matching
 * every other GB screen's own "always ask, the caller's memcmp is the no-op
 * check" contract). `header`/`header2`: the honest shell-refusal banner
 * (design sec 3.5), same posture as pdna_gbtrainer.c's own plain fallback. */
__attribute__((noinline))
static bool pdna_gbbag_plain(GbBag* bag, bool can_edit, const char* header, const char* header2) {
  GbBagPocket pocket = GBB_POCKET_ITEMS;
  int sel = 0, top = 0;
  const int vis = 12;

  for (;;) {
    const GbBagList* l = &bag->pockets[pocket];
    int n = l->count;
    if (sel >= n) sel = n > 0 ? n - 1 : 0;
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    if (top < 0) top = 0;

    ui_clear();
    int hline_y = header2 ? 20 : 11;
    int row_y0  = header2 ? 23 : 14;
    ui_text(4, 2, UI_TITLE, header ? header : (pocket == GBB_POCKET_PC ? "PC ITEM STORE" : "ITEM"));
    if (header2) ui_ptext_fit(4, 11, GBTR_HEADER2_MAXW, UI_DIM, header2);
    ui_hline(0, hline_y, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < vis && top + i < n; i++)
      gbbag_row_paint(bag, pocket, top + i, row_y0 + i * 9, top + i == sel);
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    trainer_key_legend(can_edit ? "A edit  L/R store  B save" : "L/R store  B back");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return true;
    if (k & (KEY_LEFT | KEY_RIGHT)) {
      pocket = (pocket == GBB_POCKET_ITEMS) ? GBB_POCKET_PC : GBB_POCKET_ITEMS;
      sel = 0; top = 0; continue;
    }
    if (n > 0 && (k & KEY_UP))        sel = (sel > 0) ? sel - 1 : n - 1;
    else if (n > 0 && (k & KEY_DOWN)) sel = (sel + 1) % n;
    else if (can_edit && n > 0 && (k & KEY_A)) {
      uint32_t q = num_entry("QUANTITY", l->entries[sel].qty, GBB_QTY_CAP);
      if (q < 1) q = 1;
      gbb_set_qty(GBF_G_RED, bag, pocket, sel, (uint8_t)q);
    }
  }
}

/* ============================================================================
 * ---- U4: Red/Yellow's OWN Item bag, on the shared shell (see pdna_gbbag.h
 * for the full ground-truth comment this layout is built from).
 * ============================================================================ */

enum {
  G1I_UL = 25, G1I_H = 26, G1I_UR = 27, G1I_V = 28, G1I_DL = 29, G1I_DR = 30,
  G1I_BLANK = 31
};

#define BOX_X0 4
#define BOX_Y0 2
#define BOX_X1 19
#define BOX_Y1 12
#define EXIT_X0 10
#define EXIT_Y0 12
#define EXIT_X1 19
#define EXIT_Y1 15
#define ROWS_VISIBLE 4
#define CURSOR_COL 5
#define NAME_COL   6
#define NAME_W     8    /* cols 6..13 -- measured against Yellow's "ESCAPE ROPE"/
                          * "MASTER BALL" captures, which run to col 16; this
                          * shell's own "ITEM #n" text never needs more than 8. */
#define QTY_COL    14

static int name_row(int slot) { return BOX_Y0 + 2 + slot * 2; }   /* 4,6,8,10 */
static int qty_row(int slot)  { return name_row(slot) + 1; }       /* 5,7,9,11 */

static void g1bag_border(GbScreen* gs) {
  gbscr_cell(gs, BOX_X0, BOX_Y0, GBSCR_SRC_TEXTBOX, G1I_UL);
  gbscr_cell(gs, BOX_X1, BOX_Y0, GBSCR_SRC_TEXTBOX, G1I_UR);
  for (int x = BOX_X0 + 1; x < BOX_X1; x++)
    gbscr_cell(gs, x, BOX_Y0, GBSCR_SRC_TEXTBOX, G1I_H);
  /* row BOX_Y0+1 (3): the blank pad row under the top border. */
  gbscr_cell(gs, BOX_X0, BOX_Y0 + 1, GBSCR_SRC_TEXTBOX, G1I_V);
  gbscr_cell(gs, BOX_X1, BOX_Y0 + 1, GBSCR_SRC_TEXTBOX, G1I_V);
  for (int x = BOX_X0 + 1; x < BOX_X1; x++)
    gbscr_cell(gs, x, BOX_Y0 + 1, GBSCR_SRC_TEXTBOX, G1I_BLANK);
  /* item rows' own left/right sides -- content cells are painted separately. */
  for (int slot = 0; slot < ROWS_VISIBLE; slot++) {
    int ny = name_row(slot), qy = qty_row(slot);
    gbscr_cell(gs, BOX_X0, ny, GBSCR_SRC_TEXTBOX, G1I_V);
    gbscr_cell(gs, BOX_X1, ny, GBSCR_SRC_TEXTBOX, G1I_V);
    gbscr_cell(gs, BOX_X0, qy, GBSCR_SRC_TEXTBOX, G1I_V);
    gbscr_cell(gs, BOX_X1, qy, GBSCR_SRC_TEXTBOX, G1I_V);
  }
  /* divider / bottom border (also the EXIT box's own top edge, cols 10-19). */
  gbscr_cell(gs, BOX_X0, BOX_Y1, GBSCR_SRC_TEXTBOX, G1I_DL);
  gbscr_cell(gs, BOX_X1, BOX_Y1, GBSCR_SRC_TEXTBOX, G1I_DR);
  for (int x = BOX_X0 + 1; x < BOX_X1; x++)
    gbscr_cell(gs, x, BOX_Y1, GBSCR_SRC_TEXTBOX, G1I_H);

  /* the nested EXIT box. */
  gbscr_cell(gs, EXIT_X0, EXIT_Y0 + 1, GBSCR_SRC_TEXTBOX, G1I_V);
  gbscr_cell(gs, EXIT_X1, EXIT_Y0 + 1, GBSCR_SRC_TEXTBOX, G1I_V);
  for (int x = EXIT_X0 + 1; x < EXIT_X1; x++)
    gbscr_cell(gs, x, EXIT_Y0 + 1, GBSCR_SRC_TEXTBOX, G1I_BLANK);
  gbscr_cell(gs, EXIT_X0, EXIT_Y0 + 2, GBSCR_SRC_TEXTBOX, G1I_V);
  gbscr_cell(gs, EXIT_X1, EXIT_Y0 + 2, GBSCR_SRC_TEXTBOX, G1I_V);
  gbscr_text(gs, EXIT_X0 + 2, EXIT_Y0 + 2, "EXIT");
  for (int x = EXIT_X0 + 1; x < EXIT_X0 + 2; x++)
    gbscr_cell(gs, x, EXIT_Y0 + 2, GBSCR_SRC_TEXTBOX, G1I_BLANK);
  for (int x = EXIT_X0 + 6; x < EXIT_X1; x++)
    gbscr_cell(gs, x, EXIT_Y0 + 2, GBSCR_SRC_TEXTBOX, G1I_BLANK);
  gbscr_cell(gs, EXIT_X0, EXIT_Y1, GBSCR_SRC_TEXTBOX, G1I_DL);
  gbscr_cell(gs, EXIT_X1, EXIT_Y1, GBSCR_SRC_TEXTBOX, G1I_DR);
  for (int x = EXIT_X0 + 1; x < EXIT_X1; x++)
    gbscr_cell(gs, x, EXIT_Y1, GBSCR_SRC_TEXTBOX, G1I_H);
}

/* Repaints every list-content cell unconditionally (idempotent via gbscr_cell's
 * own dirty-tracking) -- deliberately NOT incremental, so a scroll or a cursor
 * move can never leak a stale glyph from the previous top/sel (the exact
 * defect class the U3 review caught, D1). Item names are "ITEM #n" (no ROM
 * names table, see pdna_gbbag.h); quantity is always shown (this core has no
 * per-item key-item classification without a names/kind table -- a real key
 * item's row will show its stored qty, always 1, rather than the real game's
 * blank field; a documented, read-only-display deviation). */
static void g1bag_paint_list(GbScreen* gs, const GbBag* bag, GbBagPocket pocket, int top, int sel) {
  const GbBagList* l = &bag->pockets[pocket];
  char buf[16];
  for (int slot = 0; slot < ROWS_VISIBLE; slot++) {
    int idx = top + slot;
    int ny = name_row(slot), qy = qty_row(slot);
    bool has = idx < l->count;
    bool is_sel = has && idx == sel;

    gbscr_cell(gs, CURSOR_COL, ny, is_sel ? GBSCR_SRC_FONT : GBSCR_SRC_TEXTBOX,
              is_sel ? 0xED : G1I_BLANK);

    if (has) {
      siprintf(buf, "ITEM #%u", (unsigned)l->entries[idx].id);
    } else {
      buf[0] = 0;
    }
    gbscr_text(gs, NAME_COL, ny, buf);
    /* D1 (self-review, live shot): "ITEM #n" runs up to 9 glyphs wide for a
     * 3-digit id ("ITEM #250"), reaching column NAME_COL+9-1 == QTY_COL (14)
     * -- QTY_COL is also a legitimate NAME-row content column (it is only the
     * QTY ROW below that reserves it for the "x" glyph), so the blank sweep
     * must cover THAT column too, not stop one short of it. The old `cx <
     * QTY_COL` bound left column 14 unrepainted whenever a shorter id
     * followed a 3-digit one on the same row slot (e.g. Items' "ITEM #205"
     * scrolling into a 2-digit PC entry) -- a stale glyph leak, caught by
     * looking at the PC-store shot, not by any diff-count check (leak/scroll
     * correctness must be shown from the compiled binary, per review). */
    for (int cx = NAME_COL + (int)strlen(buf); cx <= QTY_COL; cx++)
      gbscr_cell(gs, cx, ny, GBSCR_SRC_TEXTBOX, G1I_BLANK);

    if (has && pocket != GBB_POCKET_KEY) {
      gbscr_text(gs, QTY_COL, qy, "\xC3\x97");   /* U+00D7, gb_char_encode -> 0xF1 */
      siprintf(buf, "%2u", (unsigned)l->entries[idx].qty);
      gbscr_text(gs, QTY_COL + 1, qy, buf);
    } else {
      gbscr_cell(gs, QTY_COL, qy, GBSCR_SRC_TEXTBOX, G1I_BLANK);
      gbscr_cell(gs, QTY_COL + 1, qy, GBSCR_SRC_TEXTBOX, G1I_BLANK);
      gbscr_cell(gs, QTY_COL + 2, qy, GBSCR_SRC_TEXTBOX, G1I_BLANK);
    }
  }
}

static void gbbag_clamp_scroll(int count, int* sel, int* top) {
  if (count <= 0) { *sel = 0; *top = 0; return; }
  if (*sel >= count) *sel = count - 1;
  if (*sel < 0) *sel = 0;
  if (*sel < *top) *top = *sel;
  if (*sel >= *top + ROWS_VISIBLE) *top = *sel - ROWS_VISIBLE + 1;
  int maxtop = count > ROWS_VISIBLE ? count - ROWS_VISIBLE : 0;
  if (*top > maxtop) *top = maxtop;
  if (*top < 0) *top = 0;
}

/* START menu: ADD ITEM / REMOVE / SWAP / TOSS (TOSS == REMOVE, brief's own
 * wording). No item-name table (see pdna_gbbag.h), so ADD ITEM asks for a raw
 * id 1..gbb_max_item_id() via num_entry -- the same honest "ITEM #n" posture
 * the list itself uses, not a fabricated picker over data this slice does not
 * have. */
__attribute__((noinline))
static void gbbag_start_menu(GbBag* bag, GbBagPocket pocket, int* sel, int* top) {
  static const char* const kOpts[4] = { "ADD ITEM", "REMOVE", "SWAP", "CANCEL" };
  int csel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "ITEM MENU");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < 4; i++) {
      if (i == csel) ui_panel(2, 14 + i * 9 - 1, 236, 9, UI_SEL, UI_TITLE);
      ui_text(6, 14 + i * 9, i == csel ? UI_SELTEXT : UI_TEXT, kOpts[i]);
    }
    trainer_key_legend("A choose  U/D  B cancel");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;
    if (k & KEY_UP)   csel = (csel > 0) ? csel - 1 : 3;
    if (k & KEY_DOWN) csel = (csel + 1) % 4;
    if (!(k & KEY_A)) continue;

    const GbBagList* l = &bag->pockets[pocket];
    if (csel == 3) return;
    if (csel == 0) {
      uint32_t maxid = gbb_max_item_id(GBF_G_RED);
      uint32_t id = num_entry("ITEM ID", 1, maxid);
      uint32_t qty = num_entry("QUANTITY", 1, GBB_QTY_CAP);
      GbBagOpStatus st = gbb_insert(GBF_G_RED, bag, pocket, (uint8_t)id, (uint8_t)qty);
      if (st == GBB_ERR_FULL)
        msg_wait("BAG FULL", UI_WARN, "This pocket has no free slot.", 0);
      else if (st == GBB_ERR_QTY)
        msg_wait("SATURATED", UI_WARN, "Quantity clamped to the cap.", 0);
      *sel = l->count > 0 ? l->count - 1 : 0;
    } else if (csel == 1) {
      if (l->count > 0) gbb_remove(GBF_G_RED, bag, pocket, *sel);
    } else if (csel == 2) {
      if (l->count > 1) {
        int other = (*sel + 1) % l->count;
        GbBagEntry tmp = bag->pockets[pocket].entries[*sel];
        bag->pockets[pocket].entries[*sel] = bag->pockets[pocket].entries[other];
        bag->pockets[pocket].entries[other] = tmp;
      }
    }
    gbbag_clamp_scroll(bag->pockets[pocket].count, sel, top);
    return;
  }
}

__attribute__((noinline))
static bool pdna_gbbag_gen1_screen(GbScreen* gs, GbBag* bag, bool can_edit) {
  static const char* const kLegendEdit[4] = {
    PDNA_GBTR_ACT_EDIT, PDNA_GBTR_ACT_SAVE, PDNA_GBSCR_ACT_SIZE, PDNA_GBTR_ACT_MORE
  };
  static const char* const kLegendView[4] = {
    0, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, PDNA_GBTR_ACT_MORE
  };
  gbscr_set_legend(gs, can_edit ? kLegendEdit : kLegendView);

  GbBagPocket pocket = GBB_POCKET_ITEMS;
  int sel = 0, top = 0;
  bool want_commit = false;

  g1bag_border(gs);
  g1bag_paint_list(gs, bag, pocket, top, sel);
  for (;;) {
    gbscr_flush(gs, 0);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B |
                   KEY_SELECT | KEY_START);
    if (k & KEY_SELECT) { gbscr_toggle_scale(gs); continue; }
    if (k & KEY_B) { want_commit = true; break; }

    const GbBagList* l = &bag->pockets[pocket];
    if (k & (KEY_LEFT | KEY_RIGHT)) {
      pocket = (pocket == GBB_POCKET_ITEMS) ? GBB_POCKET_PC : GBB_POCKET_ITEMS;
      sel = 0; top = 0;
      g1bag_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
      continue;
    }
    if (k & KEY_START) {
      if (can_edit) {
        gbbag_start_menu(bag, pocket, &sel, &top);
        g1bag_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
      }
      continue;
    }
    if (l->count > 0 && (k & KEY_UP)) {
      sel = (sel > 0) ? sel - 1 : l->count - 1;
      gbbag_clamp_scroll(l->count, &sel, &top);
      g1bag_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
    } else if (l->count > 0 && (k & KEY_DOWN)) {
      sel = (sel + 1) % l->count;
      gbbag_clamp_scroll(l->count, &sel, &top);
      g1bag_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
    } else if (can_edit && l->count > 0 && (k & KEY_A)) {
      uint32_t q = num_entry("QUANTITY", l->entries[sel].qty, GBB_QTY_CAP);
      if (q < 1) q = 1;
      gbb_set_qty(GBF_G_RED, bag, pocket, sel, (uint8_t)q);
      g1bag_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
    }
  }

  gbscr_close(gs);
  gb12_arena_tail_release();
  return want_commit;
}

void pdna_gbbag(GbSession* s, bool can_edit) {
  if (!s || s->gen != GB_GEN1) {
    msg_wait("ITEM", UI_WARN, "This screen is Gen-1 only.", 0);
    return;
  }

  /* GbBag lives in the arena tail slice, never a stack local (gb_bag.h's own
   * rule, 562 B) -- ONE slice sized for the shell's own tile bank PLUS this
   * bag, carved the same way U2c's own pic buffer is (gb12_arena_tail's "one
   * slice at a time, carve your own sub-regions" contract). shell_need is a
   * pure size computation (gbscr_tail_need never opens anything), so it is
   * safe to take even on the fallback path -- the plain page needs the SAME
   * `bag` pointer, just not the shell part of the slice. */
  uint32_t shell_need = gbscr_tail_need(PDNA_GEN1, GBSCR_NEED_TEXTBOX);
  uint32_t need = shell_need + (uint32_t)sizeof(GbBag);
  uint8_t* tail = gb12_arena_tail(need);
  if (!tail) {
    msg_wait("ITEM", UI_WARN, "Not enough memory right now.", 0);
    return;
  }
  GbBag* bag = (GbBag*)(tail + shell_need);
  if (!gbb_read(s, bag)) {
    gb12_arena_tail_release();
    msg_wait("ITEM", UI_WARN, "Could not read this save.", 0);
    return;
  }
  GbBag t0;
  memcpy(&t0, bag, sizeof t0);   /* the one-time no-op snapshot, same idiom
                                  * pdna_gbtrainer()'s own t/t0 pair uses. */

  GbScreen gs;
  const char* reason = 0;
  bool ok = gbscr_open(PDNA_GEN1, &gs, tail, shell_need, GBSCR_NEED_TEXTBOX, &reason);
  bool want_commit;
  if (ok) {
    want_commit = pdna_gbbag_gen1_screen(&gs, bag, can_edit);
    gbscr_close(&gs);
  } else {
    want_commit = pdna_gbbag_plain(bag, can_edit, PDNA_GBTR_FALLBACK_TITLE,
                                   reason ? reason : PDNA_GBSCR_REASON_UNAVAILABLE);
  }

  /* `bag` points INTO `tail` -- do every read of it (the memcmp, gbb_write)
   * BEFORE releasing the slice; gb12_arena_tail_release() only clears the
   * "lent" flag so a later caller may reuse these bytes, it does not zero
   * them, but nothing else in this function touches the arena in between, so
   * releasing early here would be a live stale-pointer trap for the NEXT
   * change to this file, not a bug today -- keep the release last on purpose. */
  bool commit_ok = true;
  if (want_commit && memcmp(bag, &t0, sizeof t0) != 0) {
    if (app_confirm("Save bag changes?", "Writes the item edits now.")) {
      GbsStatus st = gbb_write(s, bag);
      if (st != GBS_OK) {
        /* gbb_write may already have landed SOME pocket writes (gb_bag.h's
         * own contract) -- roll the whole image back rather than leave a
         * partial edit. */
        gb_rollback();
        snd_error();
        msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
        commit_ok = false;
      }
    } else {
      commit_ok = false;   /* user declined; nothing to persist */
    }
  } else if (want_commit) {
    snd_back();             /* no-op commit: bag == t0 */
    commit_ok = false;
  }
  gb12_arena_tail_release();

  /* gb_persist() plays its own snd_save()/snd_error() and, on any failure
   * past this point, has ALREADY called gb_rollback() and told the user why.
   * Runs AFTER the release: it touches the session's own image, not `bag`. */
  if (want_commit && commit_ok) gb_persist("bag");
}
