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

/* D-Kris fix (review-opus ac9ffc0): a female Crystal save (Kris) still showed the
 * boy's (Chris's) pack picture -- 13/15 pic tiles differ on the real cartridge.
 * `s->img` is the whole resident save image (gb_session.h: "CALLER-OWNED, must
 * outlive the session"), so the gender byte g2_offsets() locates (G/S: 0, not
 * stored, always false; Crystal: file offset 0x3E3D, bit 0) is read directly out
 * of it -- the SAME field the trainer card already reads via gbt_read()'s own
 * has_gender/gender pair, just fetched here without pulling in the whole
 * GbTrainer struct for one bit. */
static bool g2pack_is_female(const GbSession* s) {
  G2Offsets o;
  if (!g2_offsets(s->g2w.sv.version, &o) || !o.player_gender) return false;
  if (o.player_gender >= s->len) return false;
  return (s->img[o.player_gender] & 1u) != 0;
}

/* ============================================================================
 * ---- the plain row-list fallback (BACKLOG #67's own "no dead end" rule) --
 * same shape as pdna_gbbag.c's own pdna_gbbag_plain(), over the four real
 * pockets (cycled LEFT/RIGHT) plus the PC store.
 * ============================================================================ */

static const char* const kPocketNames[5] = { "ITEMS", "BALLS", "KEY ITEMS", "TM/HM", "PC ITEM STORE" };
static const GbBagPocket kUiPocket[4] = { GBB_POCKET_ITEMS, GBB_POCKET_BALLS,
                                          GBB_POCKET_KEY, GBB_POCKET_TMHM };

/* D4 fix (review-opus ac9ffc0): tmhm.asm's own TMHM_DisplayPocketItems skips any
 * TM/HM whose stored count is 0 -- the real TM/HM pocket lists ONLY OWNED
 * entries, not a fixed 57-row list (a save with TM01 zeroed opens the pocket
 * directly on "02 HEADBUTT", confirmed live). `g2_tm_owned[i]` is the raw
 * tmhm_index (0..GBB_TMHM_COUNT-1) of the i-th OWNED TM/HM, in ROM order;
 * `g2_tm_n` is how many are owned. Rebuilt UNCONDITIONALLY by every
 * g2pack_row_total() call for the TM/HM pocket (O(57) resident array reads, no
 * SD I/O) rather than tracked via separate invalidation at each pocket-switch/
 * edit call site -- always in sync with the live bag, by construction, at the
 * cost of one redundant rebuild for callers that already have a fresh one
 * (immaterial next to a single SD sector). */
static uint8_t g2_tm_owned[GBB_TMHM_COUNT];
static int     g2_tm_n;

static void g2_tm_rebuild(const GbBag* bag) {
  g2_tm_n = 0;
  for (int i = 0; i < GBB_TMHM_COUNT; i++) {
    uint8_t c = 0;
    gbb_tmhm_get(bag, i, &c);
    if (c > 0) g2_tm_owned[g2_tm_n++] = (uint8_t)i;
  }
}

static int g2pack_row_total(const GbBag* bag, GbBagPocket pocket) {
  if (pocket == GBB_POCKET_TMHM) { g2_tm_rebuild(bag); return g2_tm_n + 1; }  /* +CANCEL */
  return bag->pockets[pocket].count + 1;                       /* +CANCEL */
}

/* D5 fix (review-opus ac9ffc0): tmhm.asm skips the quantity column for an HM row
 * entirely (HMs are never consumed, so the game never prints a count next to
 * one) -- `tmhm_index` here is the RAW index (0..56), post g2_tm_owned mapping,
 * not the owned-list position. */
static bool g2pack_is_hm(int tmhm_index) { return tmhm_index >= 50; }

/* `idx` is the owned-list POSITION (0..g2_tm_n-1) for the TM/HM pocket, a real
 * entry index for every other pocket -- callers must have just called
 * g2pack_row_total() for the same pocket so g2_tm_owned/g2_tm_n are fresh. */
static void g2pack_row_label(const GbBag* bag, char* buf, int bufsz, GbBagPocket pocket, int idx) {
  (void)bufsz;
  if (pocket == GBB_POCKET_TMHM) {
    int real = (idx >= 0 && idx < g2_tm_n) ? g2_tm_owned[idx] : 0;
    int num = g2pack_is_hm(real) ? real - 50 + 1 : real + 1;
    siprintf(buf, "%s%02u", g2pack_is_hm(real) ? "HM" : "TM", (unsigned)num);
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
      int real = g2_tm_owned[row];
      if (g2pack_is_hm(real)) val[0] = 0;
      else { uint8_t c = 0; gbb_tmhm_get(bag, real, &c); siprintf(val, "x%u", (unsigned)c); }
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
        /* D4/D5: `sel` is an owned-list POSITION here (row_total() just above
         * rebuilt g2_tm_owned for this pocket) -- map to the real tmhm_index.
         * HMs (D5) have no count to edit at all -- matches the real cartridge's
         * own "no x column" posture; A on an HM row is a no-op here. */
        int real = g2_tm_owned[sel];
        if (!g2pack_is_hm(real)) {
          uint8_t cur = 0; gbb_tmhm_get(bag, real, &cur);
          uint32_t q = num_entry("COUNT", cur, GBB_TMHM_CAP);
          gbb_tmhm_set(GBF_G_CRYSTAL, bag, real, (uint8_t)q);
        }
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

/* D1 fix (adversarial review, review-opus ac9ffc0): GBSCR_SRC_TEXTBOX on Gen 2
 * resolves to RomGbUi.frames (9 frames x 6 tiles, 1bpp, pdna_gbscreen.c's own
 * gbscr_tile_pixels() GBSCR_SRC_TEXTBOX case: `rom_gbui_tile(local, local->frames,
 * v, 1, ...)`, v = the raw linear tile index) -- NOT Gen 1's textbox block, and
 * NOT the earlier draft's borrowed Gen-1 indices (25-31). Frame 0's own six tiles
 * (linear index 0..5) are the real box corners/edges -- confirmed by PIXELS
 * against RomGbUi.frames on both Gold and Crystal (VRAM 0x79-0x7E in the capture,
 * matched uniquely, not by the stale "textbox tile t -> 0x60+t" Gen-1 identity
 * tools/gb_oracle/README.md's harness convention assumes -- see that file's own
 * new note). The interior is GBSCR_SRC_BLANK (a flat fill, VRAM 0x7F) -- there is
 * no "blank frame tile" in the frames block at all, so G2I_BLANK never named a
 * real thing. */
enum { G2I_UL = 0, G2I_H = 1, G2I_UR = 2, G2I_V = 3, G2I_DL = 4, G2I_DR = 5 };

/* D2 fix (review-opus ac9ffc0): the earlier draft invented a blinking down-scroll
 * marker at (18,11) (a g1bag-style idiom borrowed without independent verification
 * against a real scrolled Gen-2 capture) -- the real cartridge sets no
 * SCROLLINGMENU_DISPLAY_ARROWS for this screen at all; the up/down triangle glyphs
 * visible on a real Pack screen are STATIC tiles already baked into row 0's own
 * header strip (g2pack_pic_column()'s own 0x28-0x3B run), never repainted. The
 * invented marker overwrote the bottom visible row's own tens-digit quantity cell
 * (u5_gold_04_tmhm.png showed "x▼1" where the real screen shows "x01") -- deleted
 * outright, no replacement painting. */

static bool g2_pack_swap_active;
static int  g2_pack_swap_src;

/* The description box's own frame (rows 12-17), Gen 2's own frames-block ids
 * (D1 fix above) -- SAME absolute VRAM ids (0x79-0x7E) as Gen 1's own EXIT box,
 * confirmed live. No item-description TEXT painted inside it (the description-
 * pointer table was not located this slice, per the brief's own permission --
 * a documented, honest blank) -- but see g2pack_desc_label() just below for the
 * ONE piece of prose this slice does own: the PC-store disambiguation line. */
static void g2pack_desc_box(GbScreen* gs) {
  gbscr_cell(gs, 0, 12, GBSCR_SRC_TEXTBOX, G2I_UL);
  gbscr_cell(gs, 19, 12, GBSCR_SRC_TEXTBOX, G2I_UR);
  for (int x = 1; x < 19; x++) gbscr_cell(gs, x, 12, GBSCR_SRC_TEXTBOX, G2I_H);
  for (int y = 13; y <= 16; y++) {
    gbscr_cell(gs, 0, y, GBSCR_SRC_TEXTBOX, G2I_V);
    gbscr_cell(gs, 19, y, GBSCR_SRC_TEXTBOX, G2I_V);
    for (int x = 1; x < 19; x++) gbscr_cell(gs, x, y, GBSCR_SRC_BLANK, 0);
  }
  gbscr_cell(gs, 0, 17, GBSCR_SRC_TEXTBOX, G2I_DL);
  gbscr_cell(gs, 19, 17, GBSCR_SRC_TEXTBOX, G2I_DR);
  for (int x = 1; x < 19; x++) gbscr_cell(gs, x, 17, GBSCR_SRC_TEXTBOX, G2I_H);
}

/* D6 fix (review-opus ac9ffc0): the PC item store paints the EXACT same picture
 * column + nameplate as the ITEMS pocket (g2pack_pic_column()'s own `in_pc ?
 * kPackRomIdx[0] : ...` -- both resolve to the Items picture), so nothing on
 * screen told the two apart. The real cartridge prints its own prose at hlcoord
 * (1,14) while in the PC store; this slice has no item-description text (D1's
 * own honest gap), so a short disambiguating label stands in at the SAME cell
 * origin instead of nothing. Cleared (blank row) the moment the screen leaves
 * the PC store, so a stale label never survives a toggle back to the Pack. */
static void g2pack_desc_label(GbScreen* gs, bool in_pc) {
  if (in_pc) gbscr_text(gs, 1, 14, "PC ITEM STORE");
  else       for (int x = 1; x < 19; x++) gbscr_cell(gs, x, 14, GBSCR_SRC_BLANK, 0);
}

/* Row 0 (static header) + the pic/label column (cols 0-4, rows 1-11). `cyc`
 * is the UI cycle position (0..3); `in_pc` (PC store mode) reuses cyc's own
 * art (Items' column) since the PC store was not independently pixel-dumped
 * this slice -- pdna_gbpack.h's own header comment documents this. `female`
 * (D-Kris fix, review-opus ac9ffc0) picks PACK_F over PACK_M for the 15-tile
 * picture only -- everything else in the column (header strip, filler,
 * nameplate border/label) is gender-invariant, confirmed by the reviewer's
 * own Gold-vs-Crystal-vs-Kris diff (13/15 pic tiles differ, none of the rest). */
static void g2pack_pic_column(GbScreen* gs, int cyc, bool in_pc, bool female) {
  for (int c = 0; c < 20; c++) gbscr_cell(gs, c, 0, GBSCR_SRC_PACKMENU, (uint8_t)(0x28 + c));
  for (int c = 0; c < 5; c++) {
    gbscr_cell(gs, c, 1, GBSCR_SRC_PACKMENU, 0x24);
    gbscr_cell(gs, c, 2, GBSCR_SRC_PACKMENU, 0x24);
  }
  int rom_idx = kPackRomIdx[in_pc ? 0 : cyc];
  GbScrSrc pic_src = female ? GBSCR_SRC_PACK_F : GBSCR_SRC_PACK_M;
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 5; c++)
      gbscr_cell(gs, c, 3 + r, pic_src, (uint8_t)(rom_idx * 15 + r * 5 + c));
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

    /* The cursor is ▶ FONT 0xED; an ARMED SWAP source is the cartridge's own
     * ▷ FONT 0xEC (a distinct, hollow-triangle glyph the real game uses for
     * exactly this "marked, waiting for its swap partner" state) -- NOT the
     * same tile as the cursor, which the earlier draft used for both. Gen 2
     * has this glyph (unlike Gen 1's own font, which may lack it -- this mark
     * is Gen-2-only code, so that gap never applies here). */
    GbScrSrc mark_src = is_sel ? GBSCR_SRC_FONT : (is_swap_src ? GBSCR_SRC_FONT : GBSCR_SRC_BLANK);
    uint8_t  mark_tile = is_sel ? 0xED : (is_swap_src ? 0xEC : 0);
    gbscr_cell(gs, CURSOR_COL, ny, mark_src, mark_tile);

    /* D4: `idx` is an owned-list POSITION for the TM/HM pocket (row_total()
     * above just rebuilt g2_tm_owned/g2_tm_n for this pocket) -- `real` is the
     * raw tmhm_index it maps to, only meaningful while tmhm && has && !is_cancel. */
    int real = (tmhm && has && !is_cancel) ? g2_tm_owned[idx] : 0;

    if (tmhm && has && !is_cancel) {
      int num = g2pack_is_hm(real) ? real - 50 + 1 : real + 1;
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

    /* D5: HM rows have no count column at all (tmhm.asm never prints one). */
    bool has_qty = has && !is_cancel && !key && !(tmhm && g2pack_is_hm(real));
    if (has_qty) {
      gbscr_text(gs, QTY_COL, qy, "\xC3\x97");   /* U+00D7 -> 0xF1 */
      unsigned q = 0;
      if (tmhm) { uint8_t c = 0; gbb_tmhm_get(bag, real, &c); q = c; }
      else      q = bag->pockets[pocket].entries[idx].qty;
      /* D8: a stored count/qty this core did not itself write (a foreign or
       * corrupt save) could exceed GBB_QTY_CAP/GBB_TMHM_CAP (both 99) -- "%2u"
       * on a 3-digit value would silently print its first two digits, a wrong
       * number rather than a visible "something is off" signal. */
      if (q > 99u) siprintf(buf, "**");
      else         siprintf(buf, "%2u", q);
      gbscr_text(gs, QTY_COL + 1, qy, buf);
    } else {
      for (int cx = QTY_COL; cx <= QTY_COL + 2; cx++)
        gbscr_cell(gs, cx, qy, GBSCR_SRC_BLANK, 0);
    }
  }
}

/* D3 fix (review-opus ac9ffc0): the earlier clamp pinned the cursor at slot
 * ROWS_VISIBLE-2 (row 4 of 5) as soon as it scrolled, matching Gen 1's own bag
 * (4 visible rows, pinned at slot 2) but NOT the real Gen-2 Pack's own ground
 * truth (gold_down4: four DOWNs from the top of a 26-entry list leave the
 * cursor on the FIFTH visible row, unscrolled -- the cursor walks all five rows
 * before the list starts moving, matching the "sel >= top+ROWS_VISIBLE" clamp
 * every other N-visible-row picker in this shell uses when the cursor is truly
 * free to reach the last row). */
static void gbpack_clamp_scroll(int total, int* sel, int* top) {
  if (total <= 0) { *sel = 0; *top = 0; return; }
  if (*sel >= total) *sel = total - 1;
  if (*sel < 0) *sel = 0;
  if (*sel < *top) *top = *sel;
  if (*sel >= *top + ROWS_VISIBLE) *top = *sel - (ROWS_VISIBLE - 1);
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
       * pocket would not be).
       *
       * D7 fix (review-opus ac9ffc0): the PC ITEM STORE is its own separate,
       * UNDIFFERENTIATED list, not one of the four real bag pockets with its
       * own membership rule -- the real cartridge lets you deposit ANY item
       * id into it (that is the store's whole job; gbb_insert()'s own id8
       * range check is what actually bounds a bad id, same as every other
       * pocket). Refusing ADD ITEM here was over-applying the Items-only
       * fallback to a pocket the fallback's own reasoning never covered. */
      if (pocket != GBB_POCKET_ITEMS && pocket != GBB_POCKET_PC) {
        /* D9 fix (review-opus ac9ffc0): this refusal is a known gap in THIS
         * core (no per-item pocket-membership table located yet), not a rule
         * of the real game -- say so, rather than let the message read as if
         * the cartridge itself refused. */
        msg_wait("WRONG POCKET", UI_WARN, "Add items from the Items pocket.",
                "No per-item pocket table yet.");
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
static bool pdna_gbpack_gen2_screen(GbScreen* gs, GbBag* bag, GbGame game, bool can_edit,
                                    bool female) {
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
  g2_pack_swap_active = false;

  GbBagPocket pocket = in_pc ? GBB_POCKET_PC : kUiPocket[cyc];
  g2pack_pic_column(gs, cyc, in_pc, female);
  g2pack_desc_box(gs);
  g2pack_desc_label(gs, in_pc);
  g2pack_paint_list(gs, bag, pocket, top, sel);
  for (;;) {
    gbscr_flush(gs, 0);

    u16 k = 0;
    for (;;) {
      s_vsync();
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
        g2pack_pic_column(gs, cyc, in_pc, female);
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
          g2pack_pic_column(gs, cyc, in_pc, female);
          g2pack_desc_label(gs, in_pc);
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
          /* D4/D5: `sel` is an owned-list POSITION (the `total` computed just
           * above this switch already rebuilt g2_tm_owned for this pocket).
           * HMs have no count to edit (D5) -- A is a no-op on an HM row. A
           * count set to 0 REMOVES the row (the real cartridge's own rule),
           * so the list can shrink under the cursor -- re-clamp afterward,
           * same as UP/DOWN already does, rather than leaving `sel`/`top`
           * pointing past the new (shorter) total. */
          int real = g2_tm_owned[sel];
          if (!g2pack_is_hm(real)) {
            uint8_t cur = 0; gbb_tmhm_get(bag, real, &cur);
            uint32_t q = num_entry("COUNT", cur, GBB_TMHM_CAP);
            gbb_tmhm_set(game, bag, real, (uint8_t)q);
            gbpack_clamp_scroll(g2pack_row_total(bag, pocket), &sel, &top);
          }
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
  bool female = g2pack_is_female(s);
  uint16_t pic_need = female ? GBSCR_NEED_PACK_F : GBSCR_NEED_PACK;
  uint16_t need_mask = GBSCR_NEED_TEXTBOX | GBSCR_NEED_PACKMENU | pic_need;
  GbScreen gs;
  const char* reason = 0;
  bool ok = gbscr_open(PDNA_GEN2, &gs, tail, shell_need, need_mask, &reason);
  if (!ok && female) {
    /* Same fail-safe pdna_gbtrainer.c's own gen2 card uses: a save claims
     * female on a ROM whose pack_f rom_gbui somehow failed to locate should
     * not happen (rom_gbui.c ties it to the same anchor as pack_m) -- retry
     * once as Chris rather than refuse the whole screen over one picture. */
    female = false;
    need_mask = (need_mask & ~(uint16_t)GBSCR_NEED_PACK_F) | GBSCR_NEED_PACK;
    ok = gbscr_open(PDNA_GEN2, &gs, tail, shell_need, need_mask, &reason);
  }
  bool want_commit;
  if (ok) {
    want_commit = pdna_gbpack_gen2_screen(&gs, bag, game, can_edit, female);
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
