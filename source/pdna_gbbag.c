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
#include "gb_item_names.h" /* gb_item_label -- real Gen-1 item names (BACKLOG #111 superseded) */
#include "log.h"
#include "pdna_gen12.h"     /* gb_rollback / gb_persist / gb12_arena_tail(_release)   */
#include "pdna_gbscreen.h"  /* the shared GB-screen shell                             */
#include "pdna_origin_art.h" /* PDNA_GEN1                                             */
#include "pdna_layout.h"    /* PDNA_GBSCR_ACT_*, PDNA_GBTR_ACT_*, GBTR_HEADER2_MAXW   */
#include "pdna_trainer.h"   /* num_entry                                              */
#include "ui.h"
#include "snd.h"
#include "pdna_app.h"       /* msg_wait / app_confirm                                 */

/* BACKLOG #99: decide which of the two "is this a Gen-1 key item" answers is
 * live -- the ROM-LOCATED table (rom_gbui_g1_key_item(), exact for ids
 * 1..120 including the previously-unswept 81..120 block) when the shell has
 * the ROM open and located it, else gb_bag.c's documented factual id list
 * (gbb_is_g1_key_item(), the ONLY option available to the plain row-list
 * fallback page below -- it never has a GbScreen/RomGbUi at all). `gu` is
 * NULL from that fallback path on purpose. GBB_POCKET_KEY (Gen 2's own
 * pocket) is unconditionally key -- unaffected by either table. */
static bool s_ki_fallback_logged;   /* one log line per screen visit (b99 review P2) */
static bool bag_is_key_item(const RomGbUi* gu, GbBagPocket pocket, uint8_t id) {
  if (pocket == GBB_POCKET_KEY) return true;
  /* IsKeyItem_ (pokered engine/items/item_effects.asm:~2616) branches to
   * IsItemHM BEFORE it ever touches KeyItemFlags: an id >= HM01 never reaches
   * the table, and HM01..HM05 are key by THAT path (home/names.asm:~110). The
   * located table is silent there by construction -- gbb_g1_key_item_compose()
   * (source/gb_bag.c, pure C + host-tested) checks that range first, before
   * ever consulting the table (b99 review P0: routing HMs through the table
   * showed them a quantity). */
  bool have_table = gu && gu->ok && gu->gen == ROM_GBUI_GEN1 && gu->g1_keyitems != 0;
  if (!have_table && gu && gu->ok && gu->gen == ROM_GBUI_GEN1 && !s_ki_fallback_logged) {
    s_ki_fallback_logged = true;
    log_line("gbbag: key items via the id list (no located table)");
  }
  return gbb_g1_key_item_compose(have_table, have_table ? gu->g1_keyitems_bits : 0, id);
}

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
    /* D9 (review): '#' has no Gen-1 glyph (gb_edit.c's enc_one() has no case
     * for it, so it renders as a blank space on the GB screen) -- this plain
     * PokeDNA-font fallback page uses '-' instead too, purely for the SAME
     * wording everywhere this shell shows a raw item id. */
    if (!gb_item_label(GBIN_GEN1, e->id, lbl, sizeof lbl))
      siprintf(lbl, "ITEM-%u", (unsigned)e->id);
    /* R2 (re-verify 3, decided): the plain page agrees with the GB-shell page
     * -- a Gen-1 key item shows no quantity here either (its stored qty byte
     * is not user-meaningful; the cartridge never prints it). This page never
     * has a GbScreen/RomGbUi (it is what runs when the shell itself refused
     * to open) -- `gu=NULL` always resolves to the factual id list, see
     * bag_is_key_item()'s own comment and this page's own header2 note. */
    if (bag_is_key_item(0, pocket, e->id)) siprintf(val, "-");
    else                                   siprintf(val, "x%u", (unsigned)e->qty);
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
    ui_text(4, 152, UI_DIM, can_edit ? "A edit  L/R store  B save" : app_gb_readonly_footer());

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return true;
    if (k & (KEY_LEFT | KEY_RIGHT)) {
      pocket = (pocket == GBB_POCKET_ITEMS) ? GBB_POCKET_PC : GBB_POCKET_ITEMS;
      sel = 0; top = 0; continue;
    }
    if (n > 0 && (k & KEY_UP))        sel = (sel > 0) ? sel - 1 : n - 1;
    else if (n > 0 && (k & KEY_DOWN)) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      if (n > 0 && !can_edit) {
        snd_deny();
        msg_wait("READ-ONLY", UI_WARN, app_gb_readonly_why(), NULL);
        continue;
      }
      if (can_edit && n > 0) {
        uint32_t q = num_entry("QUANTITY", l->entries[sel].qty, GBB_QTY_CAP);
        if (q < 1) q = 1;
        gbb_set_qty(GBF_G_RED, bag, pocket, sel, (uint8_t)q);
      }
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
#define QTY_COL    14

static int name_row(int slot) { return BOX_Y0 + 2 + slot * 2; }   /* 4,6,8,10 */
static int qty_row(int slot)  { return name_row(slot) + 1; }       /* 5,7,9,11 */

/* D2 (review): the down-scroll marker at (18,11) BLINKS on the real
 * cartridge -- same frame-counter idiom pdna_gbtrainer.c's play-time colon
 * already uses (toggle every 32 VBlanks; not persisted, a fresh visit
 * always starts on the OFF phase, matching that file's own documented
 * choice). Driving the counter is the main loop's job (pdna_gbbag_gen1_screen
 * below); this file-static is read-only from g1bag_paint_list(). */
static uint16_t g1_frame_ctr;
static bool g1bag_scroll_marker_on(void) { return ((g1_frame_ctr >> 5) & 1u) != 0; }

/* D10 (review): SWAP's own pick-source-then-destination state -- read-only
 * from g1bag_paint_list() below (same out-of-band idiom as g1_frame_ctr
 * above, kept off g1bag_paint_list()'s own parameter list so its signature
 * stays the one every existing caller already uses). Not persisted; always
 * false on a fresh screen visit. */
static bool g1_swap_active;
static int  g1_swap_src;

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
/* D3 (review): the real cartridge's list is `count` real entries PLUS a
 * trailing, cursor-selectable CANCEL row (idx == l->count) -- A on it takes
 * the same "leave" path B already does. `has` used to stop at l->count
 * (real entries only); it now runs one further so CANCEL paints too. Every
 * caller now passes `sel`/`top` against a TOTAL of l->count+1 (see
 * gbbag_clamp_scroll below), so idx can legitimately equal l->count here. */
static void g1bag_paint_list(GbScreen* gs, const GbBag* bag, GbBagPocket pocket, int top, int sel) {
  const GbBagList* l = &bag->pockets[pocket];
  int total = l->count + 1;   /* + the CANCEL row */
  char buf[16];
  for (int slot = 0; slot < ROWS_VISIBLE; slot++) {
    int idx = top + slot;
    int ny = name_row(slot), qy = qty_row(slot);
    bool has = idx < total;
    bool is_cancel = has && idx == l->count;
    bool is_sel = has && idx == sel;
    /* D10: the SWAP source row gets its own marker when it is not also the
     * row the cursor is currently sitting on (once the cursor moves onto
     * it, `is_sel`'s own 0xED already says "this is the row A confirms").
     * No real-cartridge capture exists for this exact glyph in this slice
     * (SWAP was never pixel-dumped) -- 0xED is the game's own confirmed
     * cursor tile, so re-using it here (rather than inventing an unverified
     * tile id) is the documented, deliberate choice pending a real capture. */
    bool is_swap_src = g1_swap_active && has && !is_cancel && idx == g1_swap_src && !is_sel;

    gbscr_cell(gs, CURSOR_COL, ny, (is_sel || is_swap_src) ? GBSCR_SRC_FONT : GBSCR_SRC_TEXTBOX,
              (is_sel || is_swap_src) ? 0xED : G1I_BLANK);

    if (is_cancel) {
      siprintf(buf, "CANCEL");
    } else if (has) {
      /* gbnames: real name from the embedded Gen-1 identifier table
       * (gb_item_names.c, GREEN per licensing) when this id has one; the
       * "ITEM-n" fallback (D9, below) only fires for a documented hole.
       * D9 (review, still true for the fallback): '#' has no Gen-1 glyph --
       * gb_edit.c's enc_one() (the GB text encoder this screen's own
       * gbscr_text() calls) has no case for it, so it silently rendered as
       * a blank space; '-' (0xE3) is a real Gen-1 glyph and reads
       * unambiguously as "item id N". */
      if (!gb_item_label(GBIN_GEN1, l->entries[idx].id, buf, sizeof buf))
        siprintf(buf, "ITEM-%u", (unsigned)l->entries[idx].id);
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
     * correctness must be shown from the compiled binary, per review).
     * gbnames (review-sonnet ruling): a real name shares the NAME row with
     * the qty row below it (qty_row() == name_row()+1), so the name field's
     * true right edge is the box's own last interior column (BOX_X1-1 ==
     * 18), not QTY_COL -- a real name up to GB1_SHELL_NAME_CAP (13) chars
     * can run past the old QTY_COL=14 bound, and a SHORTER name after a
     * longer one must still blank every column the longer one could have
     * reached, all the way to 18, or it leaks a stale glyph the same way
     * the original D1 fix (above) describes.
     * gbnames review A3 (CONFIRMED, live on docs/shots/gb/
     * gbnames_crystal_02_balls_real_names.png -- "POKé BALLE"): strlen(buf)
     * counts UTF-8 BYTES, but gbscr_text() painted `buf` one COLUMN per
     * GLYPH -- "POKé BALL" is 9 glyphs (the e-acute's UTF-8 pair is ONE
     * glyph) but 10 bytes, so the sweep used to start one column short of
     * where the name actually ends, leaving the previous (longer) name's
     * last glyph on screen. gbscr_text_cols() walks the SAME gb_char_encode()
     * loop gbscr_text() does, so the two can never disagree again. */
    for (int cx = NAME_COL + gbscr_text_cols(gs->gen, buf); cx < BOX_X1; cx++)
      gbscr_cell(gs, cx, ny, GBSCR_SRC_TEXTBOX, G1I_BLANK);

    /* D1 (review): a Gen-1 key item prints NO quantity on the real cartridge,
     * exactly like GBB_POCKET_KEY already does not (Gen 2's separate
     * Key-items pocket) -- the old `pocket != GBB_POCKET_KEY` test alone
     * always passed for pocket==GBB_POCKET_ITEMS, so every Gen-1 key item
     * showed its stored qty (always 1) instead of a blank field. BACKLOG #99:
     * bag_is_key_item() prefers the ROM-located table (`gs->gu`, exact for
     * ids 1..120) and falls back to gbb_is_g1_key_item()'s factual id list
     * only when this shell's own ROM wasn't opened or didn't locate one. */
    bool is_key = has && !is_cancel &&
                  bag_is_key_item(&gs->gu, pocket, l->entries[idx].id);
    /* D4 (review): the qty field is a 2-wide "%2u" column (QTY_COL+1,
     * QTY_COL+2), but nothing ever blanked QTY_COL+3 -- a defensively out-
     * of-spec 3-digit qty (this core clamps every WRITE to GBB_QTY_CAP=99,
     * but a stored value can only be as trustworthy as the save it was
     * read from) would print into QTY_COL+3 leaking a stale digit glyph
     * there on the NEXT repaint once the value drops back to 1-2 digits.
     * Blank sweep now always covers up to QTY_COL+3, both branches. */
    if (has && !is_cancel && !is_key) {
      gbscr_text(gs, QTY_COL, qy, "\xC3\x97");   /* U+00D7, gb_char_encode -> 0xF1 */
      siprintf(buf, "%2u", (unsigned)l->entries[idx].qty);
      gbscr_text(gs, QTY_COL + 1, qy, buf);
      for (int cx = QTY_COL + 1 + (int)strlen(buf); cx <= QTY_COL + 3; cx++)
        gbscr_cell(gs, cx, qy, GBSCR_SRC_TEXTBOX, G1I_BLANK);
    } else {
      for (int cx = QTY_COL; cx <= QTY_COL + 3; cx++)
        gbscr_cell(gs, cx, qy, GBSCR_SRC_TEXTBOX, G1I_BLANK);
    }
  }

  /* D2 (review): a down-scroll marker (font tile 0xEE) blinks at (18,11)
   * exactly while there is more list below the 4 visible rows -- BLINKING
   * is g1bag_scroll_marker_on()'s job (driven off the same frame-counter
   * idiom pdna_gbtrainer.c's play-time colon uses, see the file comment on
   * g1_frame_ctr below); this function only decides WHETHER the marker slot
   * shows the glyph or the plain TEXTBOX blank for the current phase. */
  bool more_below = (top + ROWS_VISIBLE) < total;
  bool marker_on = more_below && g1bag_scroll_marker_on();
  gbscr_cell(gs, 18, 11, marker_on ? GBSCR_SRC_FONT : GBSCR_SRC_TEXTBOX,
            marker_on ? 0xEE : G1I_BLANK);
}

/* D3: `total` is l->count + 1 (the CANCEL row) -- every caller passes that,
 * never the raw pocket count, so sel can legitimately land on CANCEL.
 * D3: the real cartridge CLAMPS at both ends (no wrap) and, while scrolling,
 * PINS the cursor at visible slot 2 (screen row 8) -- `*top = *sel - 2` once
 * `sel` has scrolled two rows past `top`; there is deliberately no `maxtop`
 * clamp any more, so `top` keeps climbing right up to `total - 1`, which is
 * what leaves the real 4th (bottom) row BLANK once CANCEL itself is pinned
 * at slot 2 (verified against the review's real 26-press DOWN/UP log). */
static void gbbag_clamp_scroll(int total, int* sel, int* top) {
  if (total <= 0) { *sel = 0; *top = 0; return; }
  if (*sel >= total) *sel = total - 1;
  if (*sel < 0) *sel = 0;
  if (*sel < *top) *top = *sel;
  if (*sel >= *top + ROWS_VISIBLE - 1) *top = *sel - (ROWS_VISIBLE - 2);
  if (*top < 0) *top = 0;
}

/* START menu: ADD ITEM / REMOVE / SWAP / TOSS (TOSS == REMOVE, brief's own
 * wording). No item-name table (see pdna_gbbag.h), so ADD ITEM asks for a raw
 * id 1..gbb_max_item_id() via num_entry -- the same honest "ITEM #n" posture
 * the list itself uses, not a fabricated picker over data this slice does not
 * have.
 *
 * D10 (review): SWAP used to just swap *sel with the NEXT entry on the spot
 * -- the real game's own semantic is pick-source-then-destination (mark the
 * row SWAP was opened on, return to the list, move the cursor, A on the
 * destination swaps, B cancels the mark). This function only ARMS that:
 * `*swap_src_out` is set to the current `*sel` and true is returned so the
 * caller (pdna_gbbag_gen1_screen) enters swap-pick mode on the list itself;
 * the actual swap happens there. */
__attribute__((noinline))
static bool gbbag_start_menu(GbBag* bag, GbBagPocket pocket, int* sel, int* top,
                             int* swap_src_out) {
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
    if (k & KEY_B) return false;
    if (k & KEY_UP)   csel = (csel > 0) ? csel - 1 : 3;
    if (k & KEY_DOWN) csel = (csel + 1) % 4;
    if (!(k & KEY_A)) continue;

    const GbBagList* l = &bag->pockets[pocket];
    if (csel == 3) return false;
    if (csel == 0) {
      /* D5 (review): num_entry() returns `cur` on OSK cancel, indistinguishable
       * from "the user typed the same value" -- ADD ITEM used to insert id 1
       * x1 whenever B'd out of either prompt. num_entry_opt() (pdna_trainer.h)
       * tells cancel apart, and both prompts now abort the whole ADD on it.
       * QUANTITY's own maxv is deliberately NOT GBB_QTY_CAP: clamping the OSK
       * value to 99 up front would make a typed 100 silently become 99 and
       * never reach gbb_insert()'s own validation, so an out-of-range type-in
       * could never be told apart from a legal saturating merge below. */
      /* R1 (re-verify 3): the ID prompt used to pass gbb_max_item_id() as the
       * OSK cap, so a typed 251 was rewritten to 250 (TM50, a legal item) and
       * INSERTED -- the same silent-rewrite class as the quantity case above.
       * Same cure: a wide cap, then clamp to 0xFF (never a plain (uint8_t)
       * truncation -- 300 would become 44, a different legal item) so every
       * out-of-range id reaches gbb_insert()'s own BAD ID refusal. */
      uint32_t id, qty;
      if (!num_entry_opt("ITEM ID", 1, 999, &id)) continue;
      uint8_t id8 = (uint8_t)(id > 0xFFu ? 0xFFu : id);
      if (!num_entry_opt("QUANTITY", 1, 999, &qty)) continue;
      bool qty_in_range = qty >= 1u && qty <= GBB_QTY_CAP;
      uint8_t qty8 = (uint8_t)(qty > 0xFFu ? 0xFFu : qty);
      GbBagOpStatus st = gbb_insert(GBF_G_RED, bag, pocket, id8, qty8);
      if (st == GBB_ERR_FULL)
        msg_wait("BAG FULL", UI_WARN, "This pocket has no free slot.", 0);
      else if (st == GBB_ERR_BADID)
        msg_wait("BAD ID", UI_WARN, "That item id does not exist.", 0);
      else if (st == GBB_ERR_QTY) {
        /* Two distinct GBB_ERR_QTY causes (gb_bag.h's own gbb_insert()
         * contract): a typed quantity outside [1,99] is refused outright
         * (nothing changed); a typed quantity that was itself legal but
         * overflowed an EXISTING stack on merge gets that stack SATURATED
         * to the cap and written (gb_bag.c gbb_insert(), the "sum > cap"
         * branch) -- `qty_in_range` (the typed value, before gbb_insert
         * ever ran) is exactly what tells the two apart here. */
        if (qty_in_range)
          msg_wait("SATURATED", UI_WARN, "Quantity clamped to the cap.", 0);
        else
          msg_wait("BAD QUANTITY", UI_WARN, "Quantity must be 1-99.", 0);
      }
      *sel = l->count > 0 ? l->count - 1 : 0;
    } else if (csel == 1) {
      if (l->count > 0) gbb_remove(GBF_G_RED, bag, pocket, *sel);
    } else if (csel == 2) {
      /* Arm swap-pick mode on whatever row is currently selected -- a
       * CANCEL-row or empty-pocket selection has nothing to swap, so those
       * just close the menu with nothing armed (the caller only enters
       * swap-pick mode when this returns true). */
      if (l->count > 1 && *sel < l->count) {
        *swap_src_out = *sel;
        return true;
      }
      return false;
    }
    gbbag_clamp_scroll(bag->pockets[pocket].count + 1, sel, top);
    return false;
  }
}

__attribute__((noinline))
static bool pdna_gbbag_gen1_screen(GbScreen* gs, GbBag* bag, bool can_edit) {
  static const char* const kLegendEdit[4] = {
    PDNA_GBTR_ACT_EDIT, PDNA_GBTR_ACT_SAVE, PDNA_GBSCR_ACT_SIZE, PDNA_GBTR_ACT_MORE
  };
  /* D10 (review): the KEY_START handler below only calls gbbag_start_menu()
   * when can_edit -- in view mode START is a dead key (`continue`, no
   * action), so promising "START MORE" in the legend would be a lie the
   * player could act on for nothing; drop it the same way row 0 (A) is
   * already 0 here for "nothing to do read-only". */
  static const char* const kLegendView[4] = {
    0, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, 0
  };
  gbscr_set_legend(gs, can_edit ? kLegendEdit : kLegendView);

  GbBagPocket pocket = GBB_POCKET_ITEMS;
  int sel = 0, top = 0;
  bool want_commit = false;
  g1_frame_ctr = 0;
  g1_swap_active = false;

  g1bag_border(gs);
  g1bag_paint_list(gs, bag, pocket, top, sel);
  for (;;) {
    gbscr_flush(gs, 0);

    /* D2 (review): drive the scroll-marker blink the SAME way
     * pdna_gbtrainer.c's play-time colon does -- a per-frame counter ticked
     * while this screen blocks for a keypress, repainting (idempotently)
     * and reflushing every 32 VBlanks so the marker's phase actually
     * changes on screen instead of only at the next keypress. */
    u16 k = 0;
    for (;;) {
      s_vsync();
      g1_frame_ctr++;
      if ((g1_frame_ctr & 31u) == 0) {
        g1bag_paint_list(gs, bag, pocket, top, sel);
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
      /* D10: B cancels an armed SWAP mark without leaving the screen --
       * only when nothing is armed does B fall through to "leave". */
      if (g1_swap_active) {
        g1_swap_active = false;
        g1bag_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
        continue;
      }
      want_commit = true; break;
    }

    const GbBagList* l = &bag->pockets[pocket];
    int total = l->count + 1;   /* D3: + the CANCEL row */
    if (k & (KEY_LEFT | KEY_RIGHT)) {
      pocket = (pocket == GBB_POCKET_ITEMS) ? GBB_POCKET_PC : GBB_POCKET_ITEMS;
      sel = 0; top = 0;
      g1_swap_active = false;   /* a mark from the OTHER pocket makes no sense */
      g1bag_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
      continue;
    }
    if (k & KEY_START) {
      if (!can_edit) {
        snd_deny();
        msg_wait("READ-ONLY", UI_WARN, app_gb_readonly_why(), NULL);
        continue;
      }
      if (can_edit) {
        int swap_src = 0;
        g1_swap_active = false;   /* any START-menu visit disarms; only SWAP re-arms (N1) */
        if (gbbag_start_menu(bag, pocket, &sel, &top, &swap_src)) {
          g1_swap_active = true;
          g1_swap_src = swap_src;
        }
        g1bag_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
      }
      continue;
    }
    /* D3: CLAMP, not wrap -- UP at row 0 stays at row 0, DOWN at the last
     * row (CANCEL) stays on CANCEL. */
    if (k & KEY_UP) {
      if (sel > 0) sel--;
      gbbag_clamp_scroll(total, &sel, &top);
      g1bag_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
    } else if (k & KEY_DOWN) {
      if (sel < total - 1) sel++;
      gbbag_clamp_scroll(total, &sel, &top);
      g1bag_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
    } else if (k & KEY_A) {
      /* D10: A while a SWAP source is armed picks the DESTINATION -- the
       * current row, if it is a real (non-CANCEL) row other than the
       * source itself. Picking the source row again is a no-op cancel
       * (nothing to swap with itself), matching B's own cancel path. */
      if (g1_swap_active) {
        if (sel < l->count && g1_swap_src < l->count && sel != g1_swap_src) {
          GbBagEntry tmp = bag->pockets[pocket].entries[sel];
          bag->pockets[pocket].entries[sel] = bag->pockets[pocket].entries[g1_swap_src];
          bag->pockets[pocket].entries[g1_swap_src] = tmp;
        }
        g1_swap_active = false;
        g1bag_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
        continue;
      }
      /* D3: A on the CANCEL row is the same "leave" path as B. */
      if (sel == l->count) { want_commit = true; break; }
      if (!can_edit) {
        snd_deny();
        msg_wait("READ-ONLY", UI_WARN, app_gb_readonly_why(), NULL);
        continue;
      }
      if (can_edit && l->count > 0) {
        uint32_t q = num_entry("QUANTITY", l->entries[sel].qty, GBB_QTY_CAP);
        if (q < 1) q = 1;
        gbb_set_qty(GBF_G_RED, bag, pocket, sel, (uint8_t)q);
        g1bag_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
      }
    }
  }

  /* D8 (review): gbscr_close()/gb12_arena_tail_release() are the CALLER's
   * own (pdna_gbbag() below, ~gbscr_close(&gs) / ~gb12_arena_tail_release())
   * -- this function used to call BOTH again here, so gb12_arena_tail_release()
   * ran a second time before pdna_gbbag() ever read `bag` back for its own
   * memcmp()/gbb_write() (bag points INTO the tail slice this just marked
   * free for reuse). Nothing else claimed that slice in between on this
   * screen's own call path, so it has not bitten yet -- but it is exactly
   * the live stale-pointer trap the caller's own comment warns about. */
  return want_commit;
}

void pdna_gbbag(GbSession* s, bool can_edit) {
  s_ki_fallback_logged = false;   /* b99 review P2: one log line per visit */
  if (!s || s->gen != GB_GEN1) {
    msg_wait("ITEM", UI_WARN, "This screen is Gen-1 only.", 0);
    return;
  }

  /* GbBag lives in the arena tail slice, never a stack local (gb_bag.h's own
   * rule, 562 B) -- ONE slice sized for the shell's own tile bank PLUS TWO
   * GbBags (D10, review: the no-op snapshot `t0` used to be a 562 B stack
   * local -- pdna_gbbag_gen1_screen() below is __attribute__((noinline)) and
   * this function itself is not tiny either, so that stack local rode on
   * top of everything else this call chain already uses; it now lives in
   * the SAME arena slice as `bag`, right after it), carved the same way
   * U2c's own pic buffer is (gb12_arena_tail's "one slice at a time, carve
   * your own sub-regions" contract). shell_need is a pure size computation
   * (gbscr_tail_need never opens anything), so it is safe to take even on
   * the fallback path -- the plain page needs the SAME `bag` pointer, just
   * not the shell part of the slice. */
  uint32_t shell_need = gbscr_tail_need(PDNA_GEN1, GBSCR_NEED_TEXTBOX, 0);
  uint32_t need = shell_need + 2u * (uint32_t)sizeof(GbBag);
  uint8_t* tail = gb12_arena_tail(need);
  if (!tail) {
    msg_wait("ITEM", UI_WARN, "Not enough memory right now.", 0);
    return;
  }
  GbBag* bag = (GbBag*)(tail + shell_need);
  GbBag* t0  = (GbBag*)(tail + shell_need + sizeof(GbBag));
  if (!gbb_read(s, bag)) {
    gb12_arena_tail_release();
    msg_wait("ITEM", UI_WARN, "Could not read this save.", 0);
    return;
  }
  memcpy(t0, bag, sizeof *t0);   /* the one-time no-op snapshot, same idiom
                                  * pdna_gbtrainer()'s own t/t0 pair uses. */

  GbScreen gs;
  const char* reason = 0;
  bool ok = gbscr_open(PDNA_GEN1, &gs, tail, shell_need, GBSCR_NEED_TEXTBOX, 0, &reason);
  bool want_commit;
  if (ok) {
    want_commit = pdna_gbbag_gen1_screen(&gs, bag, can_edit);
    gbscr_close(&gs);
  } else {
    /* BACKLOG #99: this page never has a RomGbUi (the shell itself refused
     * to open, `reason` says why) -- it always shows key items via
     * gbb_is_g1_key_item()'s factual list (see gbbag_row_paint /
     * bag_is_key_item(0, ...) above), never the located table. Say so on the
     * same reason line the shell-refusal message already uses. */
    char reason2[80];   /* %.40s + the suffix + NUL: bounded (b99 review P1) */
    siprintf(reason2, "%.40s (key ids: list)",
            reason ? reason : PDNA_GBSCR_REASON_UNAVAILABLE);
    want_commit = pdna_gbbag_plain(bag, can_edit, PDNA_GBTR_FALLBACK_TITLE, reason2);
  }

  /* `bag` points INTO `tail` -- do every read of it (the memcmp, gbb_write)
   * BEFORE releasing the slice; gb12_arena_tail_release() only clears the
   * "lent" flag so a later caller may reuse these bytes, it does not zero
   * them, but nothing else in this function touches the arena in between, so
   * releasing early here would be a live stale-pointer trap for the NEXT
   * change to this file, not a bug today -- keep the release last on purpose. */
  bool commit_ok = true;
  if (want_commit && memcmp(bag, t0, sizeof *t0) != 0) {
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
