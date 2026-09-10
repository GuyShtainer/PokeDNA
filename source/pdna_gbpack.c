/*
 * Gen-2's OWN Pack (item bag) / PC item store -- U5, BACKLOG #67. See
 * pdna_gbpack.h for the full ground-truth comment (box geometry, tile ids,
 * pocket order, the CANCEL row, the description-box/item-names/per-item-
 * pocket-membership deviations). Sibling of pdna_gbbag.c (Gen 1's own ITEM
 * screen, U4) -- same shell, same commit contract, different real-cartridge
 * geometry and pocket set.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbpack.h"
#include "gb_bag.h"
#include "gb_edit.h"        /* GB_GEN1/GB_GEN2                                        */
#include "gen2_save.h"      /* G2_VER_CRYSTAL                                          */
#include "pdna_gen12.h"     /* gb_rollback / gb_persist / gb12_arena_tail(_release)   */
#include "pdna_gbscreen.h"  /* the shared GB-screen shell                             */
#include "pdna_origin_art.h" /* PDNA_GEN2                                             */
#include "pdna_layout.h"    /* PDNA_GBSCR_ACT_*, PDNA_GBTR_ACT_*, GBTR_HEADER2_MAXW   */
#include "pdna_trainer.h"   /* num_entry / num_entry_opt                              */
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

/* Which real GbGame to pass gbb_insert()/gbb_remove()/gbb_set_qty()/gbb_tmhm_set() --
 * SAME resolution gb_bag.c's own gbb_read()/gbb_write() already do internally for a
 * Gen-2 session (s->g2w.sv.version), duplicated here because those four editing
 * calls take an explicit GbGame, not a GbSession*. Identical for GS/Crystal on every
 * fact this screen touches (gbb_max_item_id, pocket presence) -- resolved properly
 * anyway, not hardcoded, matching gb_bag.c's own comment posture. */
static GbGame g2pack_game(const GbSession* s) {
  if (s->gen == GB_GEN1) return GBF_G_RED;   /* never reached (Gen-2 only screen) */
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

/* ============================================================================
 * ---- the plain row-list fallback (BACKLOG #67's own "no dead end" rule) --
 * same shape as pdna_gbbag.c's own pdna_gbbag_plain(), over the four real
 * pockets (cycled LEFT/RIGHT) plus the PC store.
 * ============================================================================ */

static const char* const kPocketNames[5] = { "ITEMS", "BALLS", "KEY ITEMS", "TM/HM", "PC ITEM STORE" };
static const GbBagPocket kUiPocket[4] = { GBB_POCKET_ITEMS, GBB_POCKET_BALLS,
                                          GBB_POCKET_KEY, GBB_POCKET_TMHM };

static int g2pack_row_total(const GbBag* bag, GbBagPocket pocket) {
  if (pocket == GBB_POCKET_TMHM) return GBB_TMHM_COUNT + 1;   /* +CANCEL */
  return bag->pockets[pocket].count + 1;                       /* +CANCEL */
}

static void g2pack_row_label(const GbBag* bag, char* buf, int bufsz, GbBagPocket pocket, int idx) {
  (void)bufsz;
  if (pocket == GBB_POCKET_TMHM) {
    int num = (idx < 50) ? idx + 1 : idx - 50 + 1;
    siprintf(buf, "%s%02u", (idx < 50) ? "TM" : "HM", (unsigned)num);
  } else {
    siprintf(buf, "ITEM-%u", (unsigned)bag->pockets[pocket].entries[idx].id);
  }
}
static void gbpack_row_paint(const GbBag* bag, GbBagPocket pocket, int row, int y, bool sel) {
  int total = g2pack_row_total(bag, pocket);
  char lbl[16], val[16];
  if (row >= total - 1) {
    if (row == total - 1) siprintf(lbl, "CANCEL"); else lbl[0] = 0;
    val[0] = 0;
  } else {
    g2pack_row_label(bag, lbl, sizeof lbl, pocket, row);
    if (pocket == GBB_POCKET_KEY) siprintf(val, "-");
    else if (pocket == GBB_POCKET_TMHM) {
      uint8_t c = 0; gbb_tmhm_get(bag, row, &c);
      siprintf(val, "x%u", (unsigned)c);
    } else siprintf(val, "x%u", (unsigned)bag->pockets[pocket].entries[row].qty);
  }
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y - 1, 236, 9, UI_BG);
  ui_text(6, y, sel ? UI_SELTEXT : UI_TEXT, lbl);
  char valp[24]; siprintf(valp, "%-8s", val);
  ui_text(6 + 12 * 8, y, sel ? UI_SELTEXT : UI_TEXT, valp);
}

__attribute__((noinline))
static bool pdna_gbpack_plain(GbBag* bag, bool can_edit, const char* header, const char* header2) {
  int pcyc = 0;         /* 0..3 = kUiPocket cycle position; PC is a 5th state */
  bool in_pc = false;
  int sel = 0, top = 0;
  const int vis = 12;

  for (;;) {
    GbBagPocket pocket = in_pc ? GBB_POCKET_PC : kUiPocket[pcyc];
    int total = g2pack_row_total(bag, pocket);
    if (sel >= total) sel = total > 0 ? total - 1 : 0;
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    if (top < 0) top = 0;

    ui_clear();
    int hline_y = header2 ? 20 : 11;
    int row_y0  = header2 ? 23 : 14;
    char title[24]; siprintf(title, "%s", header ? header : kPocketNames[in_pc ? 4 : pcyc]);
    ui_text(4, 2, UI_TITLE, title);
    if (header2) ui_ptext_fit(4, 11, GBTR_HEADER2_MAXW, UI_DIM, header2);
    ui_hline(0, hline_y, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < vis && top + i < total; i++)
      gbpack_row_paint(bag, pocket, top + i, row_y0 + i * 9, top + i == sel);
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    trainer_key_legend(can_edit ? "A edit  L/R pocket  B save" : "L/R pocket  B back");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) return true;
    if (k & KEY_START) { in_pc = !in_pc; sel = 0; top = 0; continue; }
    if (k & (KEY_LEFT | KEY_RIGHT)) {
      if (!in_pc) pcyc = (k & KEY_RIGHT) ? (pcyc + 1) % 4 : (pcyc + 3) % 4;
      sel = 0; top = 0; continue;
    }
    if (total > 0 && (k & KEY_UP))        sel = (sel > 0) ? sel - 1 : total - 1;
    else if (total > 0 && (k & KEY_DOWN)) sel = (sel + 1) % total;
    else if (can_edit && sel < total - 1 && (k & KEY_A)) {
      if (pocket == GBB_POCKET_KEY) { /* no qty to edit */ }
      else if (pocket == GBB_POCKET_TMHM) {
        uint8_t cur = 0; gbb_tmhm_get(bag, sel, &cur);
        uint32_t q = num_entry("COUNT", cur, GBB_TMHM_CAP);
        gbb_tmhm_set(GBF_G_CRYSTAL, bag, sel, (uint8_t)q);
      } else {
        uint32_t q = num_entry("QUANTITY", bag->pockets[pocket].entries[sel].qty, GBB_QTY_CAP);
        if (q < 1) q = 1;
        gbb_set_qty(GBF_G_CRYSTAL, bag, pocket, sel, (uint8_t)q);
      }
    }
  }
}

/* ============================================================================
 * ---- U5: Gold/Silver/Crystal's OWN Pack, on the shared shell (see
 * pdna_gbpack.h for the full ground-truth comment this layout is built from).
 * ============================================================================ */

#define ROWS_VISIBLE 5
#define CURSOR_COL   7
#define NAME_COL     8
#define QTY_COL      17     /* 'x' at 17, tens at 18, ones at 19 */
#define TMNUM_COL    5      /* tens at 5, ones at 6 (before the cursor) */

static int name_row(int slot) { return 2 + 2 * slot; }   /* 2,4,6,8,10 */
static int qty_row(int slot)  { return name_row(slot) + 1; }

/* ROM order KEY/ITEMS/TM-HM/BALLS (design sec 1.4's own citation), indexed by
 * UI cycle position (kUiPocket's own order ITEMS/BALLS/KEY/TM-HM). */
static const uint8_t kPackRomIdx[4] = { 1, 3, 0, 2 };
static const uint8_t kLabelIds[4][5] = {
  { 0x06, 0x07, 0x08, 0x09, 0x0a },   /* ITEMS */
  { 0x15, 0x16, 0x17, 0x18, 0x19 },   /* BALLS */
  { 0x0b, 0x0c, 0x0d, 0x0e, 0x0f },   /* KEY */
  { 0x10, 0x11, 0x12, 0x13, 0x14 },   /* TM/HM */
};

enum { G2I_UL = 25, G2I_H = 26, G2I_UR = 27, G2I_V = 28, G2I_DL = 29, G2I_DR = 30,
       G2I_BLANK = 31 };

/* D2-style blinking down-scroll marker, same idiom pdna_gbbag.c's own
 * g1_frame_ctr/g1bag_scroll_marker_on() use -- NOT independently re-verified
 * against a real scrolled Gen-2 capture this slice (every captured pocket fit
 * in 5 rows), a documented assumption pending a real 6+-entry capture. */
static uint16_t g2_pack_frame_ctr;
static bool g2pack_scroll_marker_on(void) { return ((g2_pack_frame_ctr >> 5) & 1u) != 0; }

static bool g2_pack_swap_active;
static int  g2_pack_swap_src;

/* The description box's own frame (rows 12-17), textbox-relative ids 25-31 --
 * SAME absolute VRAM ids (0x79-0x7E) as Gen 1's own EXIT box, confirmed live.
 * No text painted inside it (the description-pointer table was not located
 * this slice, per the brief's own permission -- a documented, honest blank). */
static void g2pack_desc_box(GbScreen* gs) {
  gbscr_cell(gs, 0, 12, GBSCR_SRC_TEXTBOX, G2I_UL);
  gbscr_cell(gs, 19, 12, GBSCR_SRC_TEXTBOX, G2I_UR);
  for (int x = 1; x < 19; x++) gbscr_cell(gs, x, 12, GBSCR_SRC_TEXTBOX, G2I_H);
  for (int y = 13; y <= 16; y++) {
    gbscr_cell(gs, 0, y, GBSCR_SRC_TEXTBOX, G2I_V);
    gbscr_cell(gs, 19, y, GBSCR_SRC_TEXTBOX, G2I_V);
    for (int x = 1; x < 19; x++) gbscr_cell(gs, x, y, GBSCR_SRC_TEXTBOX, G2I_BLANK);
  }
  gbscr_cell(gs, 0, 17, GBSCR_SRC_TEXTBOX, G2I_DL);
  gbscr_cell(gs, 19, 17, GBSCR_SRC_TEXTBOX, G2I_DR);
  for (int x = 1; x < 19; x++) gbscr_cell(gs, x, 17, GBSCR_SRC_TEXTBOX, G2I_H);
}

/* Row 0 (static header) + the pic/label column (cols 0-4, rows 1-11). `cyc`
 * is the UI cycle position (0..3); `in_pc` (PC store mode) reuses cyc's own
 * art (Items' column) since the PC store was not independently pixel-dumped
 * this slice -- pdna_gbpack.h's own header comment documents this. */
static void g2pack_pic_column(GbScreen* gs, int cyc, bool in_pc) {
  for (int c = 0; c < 20; c++) gbscr_cell(gs, c, 0, GBSCR_SRC_PACKMENU, (uint8_t)(0x28 + c));
  for (int c = 0; c < 5; c++) {
    gbscr_cell(gs, c, 1, GBSCR_SRC_PACKMENU, 0x24);
    gbscr_cell(gs, c, 2, GBSCR_SRC_PACKMENU, 0x24);
  }
  int rom_idx = kPackRomIdx[in_pc ? 0 : cyc];
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 5; c++)
      gbscr_cell(gs, c, 3 + r, GBSCR_SRC_PACK_M, (uint8_t)(rom_idx * 15 + r * 5 + c));
  for (int c = 0; c < 5; c++) gbscr_cell(gs, c, 6, GBSCR_SRC_PACKMENU, 0x24);
  static const uint8_t kTop[5] = { 0x00, 0x04, 0x04, 0x04, 0x01 };
  static const uint8_t kBot[5] = { 0x02, 0x05, 0x05, 0x05, 0x03 };
  for (int c = 0; c < 5; c++) gbscr_cell(gs, c, 7, GBSCR_SRC_PACKMENU, kTop[c]);
  const uint8_t* lbl = kLabelIds[in_pc ? 0 : cyc];
  for (int c = 0; c < 5; c++) gbscr_cell(gs, c, 8, GBSCR_SRC_PACKMENU, lbl[c]);
  for (int c = 0; c < 5; c++) gbscr_cell(gs, c, 9, GBSCR_SRC_PACKMENU, kBot[c]);
  for (int c = 0; c < 5; c++) {
    gbscr_cell(gs, c, 10, GBSCR_SRC_PACKMENU, 0x24);
    gbscr_cell(gs, c, 11, GBSCR_SRC_PACKMENU, 0x24);
  }
}

/* Repaints every list-content cell unconditionally (idempotent via
 * gbscr_cell's own dirty-tracking) -- deliberately NOT incremental, same
 * posture pdna_gbbag.c's own g1bag_paint_list() documents (the U3 review's
 * D1: a wholesale repaint is what catches a leaked cell on a pocket switch
 * or scroll, not a diff count). */
static void g2pack_paint_list(GbScreen* gs, const GbBag* bag, GbBagPocket pocket,
                              int top, int sel) {
  int total = g2pack_row_total(bag, pocket);
  bool tmhm = (pocket == GBB_POCKET_TMHM);
  bool key  = (pocket == GBB_POCKET_KEY);
  char buf[16];
  for (int slot = 0; slot < ROWS_VISIBLE; slot++) {
    int idx = top + slot;
    int ny = name_row(slot), qy = qty_row(slot);
    bool has = idx < total;
    bool is_cancel = has && idx == total - 1;
    bool is_sel = has && idx == sel;
    bool is_swap_src = g2_pack_swap_active && has && !is_cancel &&
                       idx == g2_pack_swap_src && !is_sel;

    gbscr_cell(gs, CURSOR_COL, ny, (is_sel || is_swap_src) ? GBSCR_SRC_FONT : GBSCR_SRC_BLANK,
              (is_sel || is_swap_src) ? 0xED : 0);

    if (tmhm && has && !is_cancel) {
      int num = (idx < 50) ? idx + 1 : idx - 50 + 1;
      siprintf(buf, "%02u", (unsigned)num);
      gbscr_text(gs, TMNUM_COL, ny, buf);
    } else {
      gbscr_cell(gs, TMNUM_COL, ny, GBSCR_SRC_BLANK, 0);
      gbscr_cell(gs, TMNUM_COL + 1, ny, GBSCR_SRC_BLANK, 0);
    }

    if (is_cancel) {
      siprintf(buf, "CANCEL");
    } else if (has) {
      g2pack_row_label(bag, buf, sizeof buf, pocket, idx);
    } else {
      buf[0] = 0;
    }
    gbscr_text(gs, NAME_COL, ny, buf);
    for (int cx = NAME_COL + (int)strlen(buf); cx < QTY_COL; cx++)
      gbscr_cell(gs, cx, ny, GBSCR_SRC_BLANK, 0);

    bool has_qty = has && !is_cancel && !key;
    if (has_qty) {
      gbscr_text(gs, QTY_COL, qy, "\xC3\x97");   /* U+00D7 -> 0xF1 */
      unsigned q = 0;
      if (tmhm) { uint8_t c = 0; gbb_tmhm_get(bag, idx, &c); q = c; }
      else      q = bag->pockets[pocket].entries[idx].qty;
      siprintf(buf, "%2u", q);
      gbscr_text(gs, QTY_COL + 1, qy, buf);
    } else {
      for (int cx = QTY_COL; cx <= QTY_COL + 2; cx++)
        gbscr_cell(gs, cx, qy, GBSCR_SRC_BLANK, 0);
    }
  }

  bool more_below = (top + ROWS_VISIBLE) < total;
  bool marker_on = more_below && g2pack_scroll_marker_on();
  gbscr_cell(gs, 18, 11, marker_on ? GBSCR_SRC_FONT : GBSCR_SRC_BLANK,
            marker_on ? 0xEE : 0);
}

static void gbpack_clamp_scroll(int total, int* sel, int* top) {
  if (total <= 0) { *sel = 0; *top = 0; return; }
  if (*sel >= total) *sel = total - 1;
  if (*sel < 0) *sel = 0;
  if (*sel < *top) *top = *sel;
  if (*sel >= *top + ROWS_VISIBLE - 1) *top = *sel - (ROWS_VISIBLE - 2);
  if (*top < 0) *top = 0;
}

/* START menu: ADD ITEM / REMOVE / SWAP / PC STORE toggle / CANCEL -- not
 * offered for the TM/HM pocket (a fixed 57-slot count array has no insert/
 * remove/swap notion, gb_bag.h's own contract); that pocket's START press
 * only offers the PC-store toggle. Same "wide OSK cap, clamp to 0xFF, let
 * gbb_insert's own refusal answer" posture as pdna_gbbag.c's own U4 fix (R1). */
__attribute__((noinline))
static int gbpack_start_menu(GbBag* bag, GbBagPocket pocket, GbGame game, int* sel, int* top,
                             int* swap_src_out, bool in_pc) {
  bool has_edit_ops = (pocket != GBB_POCKET_TMHM);
  const char* opts[5];
  int n = 0;
  if (has_edit_ops) { opts[n++] = "ADD ITEM"; opts[n++] = "REMOVE"; opts[n++] = "SWAP"; }
  opts[n++] = in_pc ? "PACK" : "PC STORE";
  opts[n++] = "CANCEL";
  int csel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "PACK MENU");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < n; i++) {
      if (i == csel) ui_panel(2, 14 + i * 9 - 1, 236, 9, UI_SEL, UI_TITLE);
      ui_text(6, 14 + i * 9, i == csel ? UI_SELTEXT : UI_TEXT, opts[i]);
    }
    trainer_key_legend("A choose  U/D  B cancel");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return 0;
    if (k & KEY_UP)   csel = (csel > 0) ? csel - 1 : n - 1;
    if (k & KEY_DOWN) csel = (csel + 1) % n;
    if (!(k & KEY_A)) continue;

    if (csel == n - 1) return 0;                 /* CANCEL */
    if (csel == n - 2) return 2;                  /* PC/PACK toggle -- caller's job */

    const GbBagList* l = &bag->pockets[pocket];
    if (csel == 0) {   /* ADD ITEM */
      /* Per-item pocket membership (which pocket a given item id legally
       * belongs to) was NOT located this slice (time-boxed, per the brief's
       * own permission) -- fall back to the brief's own sanctioned rule:
       * ADD ITEM only works from the Items pocket; Balls/Key items refuse
       * outright (a real id typed here might genuinely belong to a DIFFERENT
       * pocket than the one open, and this core has no table to tell that
       * apart -- refusing is honest, silently accepting into the wrong
       * pocket would not be). */
      if (pocket != GBB_POCKET_ITEMS) {
        msg_wait("WRONG POCKET", UI_WARN, "Add items from the Items pocket.", 0);
        continue;
      }
      uint32_t id, qty;
      if (!num_entry_opt("ITEM ID", 1, 999, &id)) continue;
      uint8_t id8 = (uint8_t)(id > 0xFFu ? 0xFFu : id);
      if (!num_entry_opt("QUANTITY", 1, 999, &qty)) continue;
      bool qty_in_range = qty >= 1u && qty <= GBB_QTY_CAP;
      uint8_t qty8 = (uint8_t)(qty > 0xFFu ? 0xFFu : qty);
      GbBagOpStatus st = gbb_insert(game, bag, pocket, id8, qty8);
      if (st == GBB_ERR_FULL)
        msg_wait("BAG FULL", UI_WARN, "This pocket has no free slot.", 0);
      else if (st == GBB_ERR_BADID)
        msg_wait("BAD ID", UI_WARN, "That item id does not exist.", 0);
      else if (st == GBB_ERR_QTY) {
        if (qty_in_range)
          msg_wait("SATURATED", UI_WARN, "Quantity clamped to the cap.", 0);
        else
          msg_wait("BAD QUANTITY", UI_WARN, "Quantity must be 1-99.", 0);
      }
      *sel = l->count > 0 ? l->count - 1 : 0;
    } else if (csel == 1) {   /* REMOVE */
      if (l->count > 0) gbb_remove(game, bag, pocket, *sel);
    } else if (csel == 2) {   /* SWAP -- arm, same pick-source-then-destination
                               * semantic as pdna_gbbag.c's own U4 D10. */
      if (l->count > 1 && *sel < l->count) {
        *swap_src_out = *sel;
        return 1;
      }
      return 0;
    }
    gbpack_clamp_scroll(g2pack_row_total(bag, pocket), sel, top);
    return 0;
  }
}

__attribute__((noinline))
static bool pdna_gbpack_gen2_screen(GbScreen* gs, GbBag* bag, GbGame game, bool can_edit) {
  static const char* const kLegendEdit[4] = {
    PDNA_GBTR_ACT_EDIT, PDNA_GBTR_ACT_SAVE, PDNA_GBSCR_ACT_SIZE, PDNA_GBTR_ACT_MORE
  };
  static const char* const kLegendView[4] = {
    0, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, 0
  };
  gbscr_set_legend(gs, can_edit ? kLegendEdit : kLegendView);

  int cyc = 0;
  bool in_pc = false;
  int sel = 0, top = 0;
  bool want_commit = false;
  g2_pack_frame_ctr = 0;
  g2_pack_swap_active = false;

  GbBagPocket pocket = in_pc ? GBB_POCKET_PC : kUiPocket[cyc];
  g2pack_pic_column(gs, cyc, in_pc);
  g2pack_desc_box(gs);
  g2pack_paint_list(gs, bag, pocket, top, sel);
  for (;;) {
    gbscr_flush(gs, 0);

    u16 k = 0;
    for (;;) {
      s_vsync();
      g2_pack_frame_ctr++;
      if ((g2_pack_frame_ctr & 31u) == 0) {
        g2pack_paint_list(gs, bag, pocket, top, sel);
        gbscr_flush(gs, 0);
      }
      k = key_hit(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B |
                  KEY_SELECT | KEY_START);
      if (k) break;
    }
    if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
    else if (k & KEY_A) snd_ok();
    else if (k & KEY_B) snd_back();

    if (k & KEY_SELECT) { gbscr_toggle_scale(gs); continue; }
    if (k & KEY_B) {
      if (g2_pack_swap_active) {
        g2_pack_swap_active = false;
        g2pack_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
        continue;
      }
      want_commit = true; break;
    }

    int total = g2pack_row_total(bag, pocket);
    if (k & (KEY_LEFT | KEY_RIGHT)) {
      if (!in_pc) {
        cyc = (k & KEY_RIGHT) ? (cyc + 1) % 4 : (cyc + 3) % 4;
        pocket = kUiPocket[cyc];
        sel = 0; top = 0;
        g2_pack_swap_active = false;
        g2pack_pic_column(gs, cyc, in_pc);
        g2pack_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
      }
      continue;
    }
    if (k & KEY_START) {
      if (can_edit) {
        int swap_src = 0;
        g2_pack_swap_active = false;
        int r = gbpack_start_menu(bag, pocket, game, &sel, &top, &swap_src, in_pc);
        if (r == 1) { g2_pack_swap_active = true; g2_pack_swap_src = swap_src; }
        else if (r == 2) {
          in_pc = !in_pc;
          pocket = in_pc ? GBB_POCKET_PC : kUiPocket[cyc];
          sel = 0; top = 0;
          g2pack_pic_column(gs, cyc, in_pc);
        }
        g2pack_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
      }
      continue;
    }
    if (k & KEY_UP) {
      if (sel > 0) sel--;
      gbpack_clamp_scroll(total, &sel, &top);
      g2pack_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
    } else if (k & KEY_DOWN) {
      if (sel < total - 1) sel++;
      gbpack_clamp_scroll(total, &sel, &top);
      g2pack_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
    } else if (k & KEY_A) {
      if (g2_pack_swap_active) {
        int cnt = bag->pockets[pocket].count;
        if (sel < cnt && g2_pack_swap_src < cnt && sel != g2_pack_swap_src) {
          GbBagEntry tmp = bag->pockets[pocket].entries[sel];
          bag->pockets[pocket].entries[sel] = bag->pockets[pocket].entries[g2_pack_swap_src];
          bag->pockets[pocket].entries[g2_pack_swap_src] = tmp;
        }
        g2_pack_swap_active = false;
        g2pack_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
        continue;
      }
      if (sel == total - 1) { want_commit = true; break; }   /* CANCEL row */
      if (can_edit) {
        if (pocket == GBB_POCKET_KEY) {
          /* no quantity field to edit -- matches the real cartridge's own
           * "key items have no ×N" posture. */
        } else if (pocket == GBB_POCKET_TMHM) {
          uint8_t cur = 0; gbb_tmhm_get(bag, sel, &cur);
          uint32_t q = num_entry("COUNT", cur, GBB_TMHM_CAP);
          gbb_tmhm_set(game, bag, sel, (uint8_t)q);
          g2pack_paint_list(gs, bag, pocket, top, sel);
          gbscr_mark_all_dirty(gs);
        } else {
          uint32_t q = num_entry("QUANTITY", bag->pockets[pocket].entries[sel].qty, GBB_QTY_CAP);
          if (q < 1) q = 1;
          gbb_set_qty(game, bag, pocket, sel, (uint8_t)q);
          g2pack_paint_list(gs, bag, pocket, top, sel);
          gbscr_mark_all_dirty(gs);
        }
      }
    }
  }

  return want_commit;
}

void pdna_gbpack(GbSession* s, bool can_edit) {
  if (!s || s->gen != GB_GEN2) {
    msg_wait("PACK", UI_WARN, "This screen is Gen-2 only.", 0);
    return;
  }

  uint32_t shell_need = gbscr_tail_need(PDNA_GEN2,
      GBSCR_NEED_TEXTBOX | GBSCR_NEED_PACKMENU | GBSCR_NEED_PACK);
  uint32_t need = shell_need + 2u * (uint32_t)sizeof(GbBag);
  uint8_t* tail = gb12_arena_tail(need);
  if (!tail) {
    msg_wait("PACK", UI_WARN, "Not enough memory right now.", 0);
    return;
  }
  GbBag* bag = (GbBag*)(tail + shell_need);
  GbBag* t0  = (GbBag*)(tail + shell_need + sizeof(GbBag));
  if (!gbb_read(s, bag)) {
    gb12_arena_tail_release();
    msg_wait("PACK", UI_WARN, "Could not read this save.", 0);
    return;
  }
  memcpy(t0, bag, sizeof *t0);

  GbGame game = g2pack_game(s);
  GbScreen gs;
  const char* reason = 0;
  bool ok = gbscr_open(PDNA_GEN2, &gs, tail, shell_need,
                       GBSCR_NEED_TEXTBOX | GBSCR_NEED_PACKMENU | GBSCR_NEED_PACK, &reason);
  bool want_commit;
  if (ok) {
    want_commit = pdna_gbpack_gen2_screen(&gs, bag, game, can_edit);
    gbscr_close(&gs);
  } else {
    want_commit = pdna_gbpack_plain(bag, can_edit, PDNA_GBTR_FALLBACK_TITLE,
                                    reason ? reason : PDNA_GBSCR_REASON_UNAVAILABLE);
  }

  bool commit_ok = true;
  if (want_commit && memcmp(bag, t0, sizeof *t0) != 0) {
    if (app_confirm("Save pack changes?", "Writes the item edits now.")) {
      GbsStatus st = gbb_write(s, bag);
      if (st != GBS_OK) {
        gb_rollback();
        snd_error();
        msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
        commit_ok = false;
      }
    } else {
      commit_ok = false;
    }
  } else if (want_commit) {
    snd_back();
    commit_ok = false;
  }
  gb12_arena_tail_release();

  if (want_commit && commit_ok) gb_persist("bag");
}
