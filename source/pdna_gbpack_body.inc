/*
 * The pure-logic half of the Gen-2 Pack painter -- shared, byte-for-byte,
 * between source/pdna_gbpack.c (the shipped screen) and
 * tests/host_gbpack_test.c (the host suite), so the two can never drift
 * (BACKLOG #111 N5; the earlier state was a hand-maintained copy in the test
 * file, which happened to still match but had no mechanism keeping it that
 * way). Pure C: no tonc/GBA headers, no ui.h/snd.h -- only <string.h>/GbBag/
 * GbScreen (both already pure headers both callers already include), matching
 * hard rule 5. `siprintf` is provided by the includer (tonc's own fill-in on
 * the shipped side, `#define siprintf snprintf` on the host side, the same
 * convention every other GB-screen-adjacent host test already uses).
 */

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
static void g2pack_desc_box(GbScreen* gs, int frame) {
  int ul = G2I_UL + 6 * frame, ur = G2I_UR + 6 * frame, dl = G2I_DL + 6 * frame,
      dr = G2I_DR + 6 * frame, h = G2I_H + 6 * frame, v = G2I_V + 6 * frame;
  gbscr_cell(gs, 0, 12, GBSCR_SRC_TEXTBOX, ul);
  gbscr_cell(gs, 19, 12, GBSCR_SRC_TEXTBOX, ur);
  for (int x = 1; x < 19; x++) gbscr_cell(gs, x, 12, GBSCR_SRC_TEXTBOX, h);
  for (int y = 13; y <= 16; y++) {
    gbscr_cell(gs, 0, y, GBSCR_SRC_TEXTBOX, v);
    gbscr_cell(gs, 19, y, GBSCR_SRC_TEXTBOX, v);
    for (int x = 1; x < 19; x++) gbscr_cell(gs, x, y, GBSCR_SRC_BLANK, 0);
  }
  gbscr_cell(gs, 0, 17, GBSCR_SRC_TEXTBOX, dl);
  gbscr_cell(gs, 19, 17, GBSCR_SRC_TEXTBOX, dr);
  for (int x = 1; x < 19; x++) gbscr_cell(gs, x, 17, GBSCR_SRC_TEXTBOX, h);
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
      /* tmhm.asm: an HM row prints a literal 'H' then a left-aligned digit ('H3'),
       * a TM row its two-digit number (U5 re-verify N1). */
      if (g2pack_is_hm(real)) siprintf(buf, "H%u", (unsigned)num);
      else                    siprintf(buf, "%02u", (unsigned)num);
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
      if (q > 99u) siprintf(buf, "??");   /* '*' is not in the GB charmap (encodes to a blank); '?' is 0xE6 (U5 re-verify N2) */
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
