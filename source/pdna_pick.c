/*
 * Rich pickers for the editor: an HGSS-style species icon grid with live search
 * + filter (Gen/type/legendary) + sort, and a move picker with type chips,
 * power/accuracy/PP and the in-game description. Item/nature use searchable lists.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "perf.h"        /* dex screen span + page/bob rollups (telemetry) */
#include "pdna_pick.h"
#include "pdna_app.h"      /* app_confirm, app_anim_enabled (Pokedex screen) */
#include "ui.h"
#include "pdna_layout.h"   /* list/popup geometry, shared with tests/host_textfit_test.c */
#include "data_tables.h"
#include "gen3_items.h"    /* pk_item_pocket / PkPocket (item-picker category filter) */
#include "gb_item_names.h" /* gb_item_label / GBIN_GEN1 / GBIN_GEN2 (BACKLOG #195)     */
#include "gb_art_source.h" /* gb_art_item_desc (BACKLOG #340b)                         */
#include "gb_bag.h"        /* gbb_pocket_of (BACKLOG #195 restricted category filter) */
#include "gen3_places.h"   /* met-location region / per-game scoping / list build     */
#include "mon_icons.h"
#include "mon_icons_gate.h"  /* PDNA_MON_ICONS_ART_COMPILED -- is this a full-art build */
#include "art_icons_cache.h" /* art_icons_row_for -- species -> icon-store row          */
#include "icon_store.h"      /* the dex page declares its 21 rows before it paints them */
#include "pdna_summary.h"    /* BACKLOG #203: pdna_summary_portrait_screen -- the detail view's backdrop */
#include "rumble.h"          /* BACKLOG #203: rumble_io_suspend/resume around the ROM portrait fetch */
#include "dex_detail_rule.h" /* BACKLOG #203: L/R wrap + status word + cycle (host-tested) */
#include "pdna_origin_art.h" /* BACKLOG #124: pdna_origin_art_have -- is a GB ROM actually
                              * registered, for dex_declare_page()'s double-fetch guard  */
#include "type_icons.h"
#include "item_icons.h"
#include "osk.h"
#include "snd.h"

#define CANCEL 0xFFFF
#define NSPECIES 412
#define NMOVE    355
#define NITEM    400

static u16 EWRAM_BSS g_list[NSPECIES];   /* internal species ids in display order */
static int g_n;

/* CREATE restriction (BACKLOG #50 UX-parity, Guy 2026-09-07): pick_species()
 * is reused UNCHANGED for the Gen-1/2 create flow's species picker (which used
 * to be a separate, plainer dex-list screen, pdna_gen12.c's now-deleted
 * gb_create_pick_species) -- but a Gen-1 session must never offer a Gen-2+
 * species, and a Gen-2 session must never offer a Gen-3+ one: gb_new_mon() has
 * no base-stat/learnset source for anything past its own generation's ROM
 * table, and a species this list should never have produced reaching it would
 * be a silent wrong-generation record, not merely a slow one.
 *
 * A file-static ceiling, not a second entry point: build_species() already
 * combines several independent constraints (the Gen/type/legendary `filter`,
 * and `search`) with a plain AND, and every one of pick_species()'s OWN
 * existing callers (the Gen-3 create/edit flows) must see EXACTLY today's
 * unrestricted list -- a new parameter would touch every call site. 0 (the
 * default, set by nothing) means "unrestricted", so nothing changes for them.
 * Set it right before calling pick_species() and clear it right after -- it
 * must never leak into the NEXT, unrelated call.
 *
 * Verified empirically (a one-off host-side sweep of pk_national_no() over
 * every internal id 1..411, this feature's own commit message) that National
 * Dex 1..251 maps from EXACTLY ONE internal index each, zero gaps or
 * duplicates -- so a plain national-dex-number ceiling is sufficient on its
 * own; no Gen-3-only "form" hides inside that range under a low dex number
 * needing separate exclusion (Unown, dex 201, IS a real Gen-2 species and is
 * correctly kept for a Gen-2-restricted list by the ceiling alone -- its
 * LETTER is decided later, by gb_new_mon's own DVs, the same way pick_species'
 * OWN Gen-3 caller decides it via a separate pick_unown_form() call AFTER the
 * species is picked here, not by a distinct list entry). */
static uint16_t g_species_max_dex = 0;   /* 0 = unrestricted: every existing caller */
void pick_species_set_max_dex(uint16_t max_dex) { g_species_max_dex = max_dex; }

/* ONE index buffer for every flat list on this screen family — the generic list_pick,
 * the item picker, the ball picker, the met-location picker and the MOVE picker. They
 * are all leaf screens (none opens another), so sharing is safe, and it is what pays
 * for the newer pickers: this used to be two separate static u16[NITEM] arrays, so
 * folding them frees 800 bytes of EWRAM rather than spending any. G3_PLACE_MAX (217)
 * fits well inside it. _Static_assert keeps that true if any count ever grows.
 *
 * The move picker joined the fold on 2026-08-23 (it had its own static u16[NMOVE],
 * g_mv, 710 B) to help pay for source/icon_store.c's row pool. The exclusivity proof is
 * the same one the others rest on and it was re-checked by grep, not assumed:
 * pick_move()'s body calls no other picker and no list_pick, its ONLY caller in the
 * whole tree is pdna_edit.c's F_MV0 case arm -- a sibling of the very case arm that
 * calls pick_item() -- and a switch case blocks on its picker's return value before the
 * next arm can run. So no two users of this buffer are ever live at once. */
static u16 EWRAM_BSS g_idx[NITEM];
_Static_assert(NITEM >= G3_PLACE_MAX, "g_idx must hold the whole met-location list");
_Static_assert(NITEM >= NMOVE, "g_idx must hold the whole move list (folded g_mv)");

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
/* fresh presses for all keys + auto-repeat for the held d-pad (tonc key_repeat,
 * configured globally in init_system) so holding a direction keeps scrolling.
 * Also the per-file UI-sound chokepoint (fresh presses only). */
static u16  s_wait(u16 m) {
  const u16 dpad = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT;
  u16 k, fresh;
  do { s_vsync(); fresh = key_hit(m); k = fresh | key_repeat(m & dpad); } while (!k);
  if      (fresh & dpad)              snd_move();
  else if (fresh & (KEY_L | KEY_R))   snd_tab();
  else if (fresh & KEY_A)             snd_ok();
  else if (fresh & KEY_B)             snd_back();
  return k;
}
static int  clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static char up1(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static bool ci_contains(const char* hay, const char* ndl) {
  if (!ndl[0]) return true;
  for (; *hay; hay++) {
    const char* a = hay; const char* b = ndl;
    while (*b && up1(*a) == up1(*b)) { a++; b++; }
    if (!*b) return true;
  }
  return false;
}

static bool all_digits(const char* s) {
  if (!*s) return false;
  for (; *s; s++) if (*s < '0' || *s > '9') return false;
  return true;
}
/* true if the decimal of `val` starts with the digit string `q` (incremental no. match) */
static bool num_prefix(unsigned val, const char* q) {
  char b[8]; siprintf(b, "%u", val);
  return strncmp(b, q, strlen(q)) == 0;
}

static bool is_legendary(uint16_t n) {
  switch (n) {
    case 144: case 145: case 146: case 150: case 151:
    case 243: case 244: case 245: case 249: case 250: case 251:
    case 377: case 378: case 379: case 380: case 381:
    case 382: case 383: case 384: case 385: case 386: return true;
  }
  return false;
}

/* ---- type chips ---- */
static const char* const TYPE_ABBR[18] = {
  "NOR","FIG","FLY","PSN","GRD","RCK","BUG","GHO","STL",
  "@",  "FIR","WAT","GRS","ELE","PSY","ICE","DRG","DRK",
};
static u16 type_color(uint8_t t) {
  const u16 C[18] = {
    RGB15(19,19,15), RGB15(24, 9, 7), RGB15(20,18,28), RGB15(20, 9,20), RGB15(26,21,12),
    RGB15(22,20,11), RGB15(20,24, 7), RGB15(14,10,20), RGB15(18,18,22), RGB15(16,16,16),
    RGB15(30,14, 8), RGB15( 8,16,30), RGB15(12,24,10), RGB15(30,28, 8), RGB15(30,12,18),
    RGB15(16,28,30), RGB15(12,10,28), RGB15(11, 9,11),
  };
  return C[t < 18 ? t : 0];
}
static void type_chip(int x, int y, uint8_t t) {
  if (t >= 18) return;
  /* PDNA_FILT_CHIP_W, not a bare 26: the filter lists draw the type NAME a fixed
   * PDNA_FILT_CHIP_DX along from the chip, and the host test asserts the chip stops
   * before it. Two literals could not be checked against each other.
   *
   * The VERTICAL pair used to be a bare `9` at `y` — one pixel LOWER than the y-1 row
   * bar every caller (fm_row, dxm_row, mv_row) backs its rows with, so the chip's last
   * scanline lived in the NEXT row's rect. Harmless while unselected rows painted no
   * background; the moment partial repaint made every row wipe its own rect, the row
   * below ate that scanline and chips came out 8 px tall (all but the last in the
   * window, which has no row below it to eat it) and flickered as the cursor passed.
   * The chip now IS the bar's rect, drawn narrow — invariant (1) of the ROW REPAINT
   * RULE below. Its label stays on the text baseline `y`, level with the row's name. */
  ui_fill_rect(x, y + PDNA_FILT_CHIP_DY, PDNA_FILT_CHIP_W, PDNA_FILT_CHIP_H, type_color(t));
  ui_text(x + 2, y, RGB15(31, 31, 31), TYPE_ABBR[t]);
}
/* The real Gen-3 type badge (32x14, generated). Use where a row is tall enough;
 * the compact text type_chip() stays for 8-9px list rows. */
static void type_icon(int x, int y, uint8_t t) {
  if (t >= 18) return;
  /* app_type_badge adds the ROM rung on top of the compiled type_icon_for() (Phase 1,
   * docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 4.7); a ROM badge is not the same
   * crop as the compiled one (16 px RSE / 12 px FRLG vs the compiled 14), so draw the
   * height it actually reports rather than assuming TYPE_ICON_H. */
  uint8_t h = TYPE_ICON_H;
  const uint16_t* ic = app_type_badge(t, &h);
  if (ic) ui_sprite(x, y, TYPE_ICON_W, h, ic);
  else    ui_type_chip(x, y, TYPE_ICON_W, TYPE_ICON_H, t);   /* art-free: original chip */
}

/* ===================== species grid ==================================== */

static const char* filter_name(int f) {
  switch (f) {
    case 0: return "All";
    case 1: return "Gen 1";
    case 2: return "Gen 2";
    case 3: return "Gen 3";
    case 4: return "Legendary";
    default: return pk_type_name((uint8_t)(f - 5));
  }
}
#define NFILTER (5 + 18)

static void build_species(int filter, int sort, const char* search) {
  g_n = 0;
  bool by_num = all_digits(search);                      /* digits -> match dex number, else name */
  for (uint16_t in = 1; in <= 411; in++) {
    uint16_t nat = pk_national_no(in);
    if (!nat) continue;                                  /* skip non-species slots */
    if (g_species_max_dex && nat > g_species_max_dex) continue;   /* CREATE restriction */
    if (filter == 1 && nat > 151) continue;
    if (filter == 2 && (nat < 152 || nat > 251)) continue;
    if (filter == 3 && nat < 252) continue;
    if (filter == 4 && !is_legendary(nat)) continue;
    if (filter >= 5) {
      uint8_t t = (uint8_t)(filter - 5);
      if (pk_species_type1(in) != t && pk_species_type2(in) != t) continue;
    }
    if (search[0]) {
      if (by_num) { if (!num_prefix(nat, search)) continue; }
      else        { if (!ci_contains(pk_species_name(in), search)) continue; }
    }
    g_list[g_n++] = in;
  }
  /* insertion sort: by national no. (0) or name (1) */
  for (int i = 1; i < g_n; i++) {
    uint16_t v = g_list[i];
    int j = i - 1;
    while (j >= 0) {
      bool gt = (sort == 1) ? (strcmp(pk_species_name(g_list[j]), pk_species_name(v)) > 0)
                            : (pk_national_no(g_list[j]) > pk_national_no(v));
      if (!gt) break;
      g_list[j + 1] = g_list[j];
      j--;
    }
    g_list[j + 1] = v;
  }
}

#define GCOLS 7           /* full-size 32x32 icons in a 7-wide grid */
#define GVROWS 3
#define GCELLX 33
#define GCELLY 34
#define GX 8
#define GY 22
#define GICON 32

/* True if filter id `f` can select at least one species under the active
 * create-mode ceiling (pick_species_set_max_dex(), 0 = no ceiling = always
 * true) -- shared by filter_ids()'s own START-menu list AND the plain L/R
 * quick-cycle below, so the two can never drift apart: before this, the
 * ceiling was ONLY consulted by the menu list, so L/R could still cycle onto
 * a "Gen 2"/"Gen 3" filter guaranteed to page to nothing inside a Gen-1-
 * restricted create. Type filters (5..22, MYSTERY 14 already excluded
 * unconditionally) are not similarly audited -- whether a given type has zero
 * Gen-1 representatives (Steel and Dark do not exist as types before Gen 2)
 * is a data question this file has no cheap way to answer, so a type filter
 * can still page to an empty list under a ceiling; that residual is left as
 * a known, minor rough edge rather than in scope here. */
/* DEX_NAT_MAX/s_dex_max moved up here from their original spot below (D5, b87 fix
 * pass, DO-NOT-SHIP review) so filter_usable() can see s_dex_max too -- see its own
 * header comment just below the -300-line gap for the full species-cap story
 * (BACKLOG #87 item 1: Gen 1 (151) / Gen 2 (251) GB sessions reuse the shared dex
 * screen under this cap; 386 is the unrestricted Gen-3 default). */
#define DEX_NAT_MAX 386
static int s_dex_max = DEX_NAT_MAX;

/* D5 (b87 fix pass, DO-NOT-SHIP review): the GB dex screen's own species cap
 * (s_dex_max, BACKLOG #87 item 1) was invisible to filter_usable() -- only
 * g_species_max_dex (the CREATE picker's OWN, separate ceiling) was consulted, so a
 * Gen-1 GB session (s_dex_max==151) still offered "Gen 2"/"Gen 3+" filter rows that
 * paged to an empty list once dex_build()'s own post-filter (source line ~575) threw
 * every species past the cap away. Both ceilings gate both filters now -- whichever
 * one is active (a session is never under both at once, but the OR costs nothing and
 * stays correct if that ever changes). */
static bool filter_usable(int f) {
  if (f == 5 + 9) return false;                                              /* MYSTERY: unused, always */
  if ((g_species_max_dex && g_species_max_dex <= 151u) || s_dex_max <= 151) { if (f == 2) return false; } /* Gen 2: empty under a Gen-1 ceiling */
  if ((g_species_max_dex && g_species_max_dex <= 251u) || s_dex_max <= 251) { if (f == 3) return false; } /* Gen 3+: empty under either ceiling */
  return true;
}

/* the filter ids selectable in the menu / by L-R (All, Gen1-3, Legendary, types
 * except the unused MYSTERY type 9, and whatever filter_usable() has ruled
 * out under the active create-mode ceiling). */
static int filter_ids(int* out) {
  int n = 0;
  for (int f = 0; f <= 4; f++) if (filter_usable(f)) out[n++] = f;
  for (int t = 0; t < 18; t++) if (t != 9) out[n++] = 5 + t;
  return n;
}

/* Row geometry shared by the two FILTER / SORT lists.
 *
 * They used to run at an 8 px pitch with the selection drawn as ui_panel(2, y-1, .., 9):
 * ui_panel's frame puts its bottom line at y+7, which is INSIDE the 8-row glyph box, so
 * the highlight rule ran straight through the feet of "Sort: No. (dex)" and its fill
 * ended one pixel above the next row's ascenders — "Status: All" / "Game: All" looked
 * clipped by the box above them. A 9 px pitch with a BORDERLESS bar (sp_row's idiom,
 * below) leaves a clear pixel row between the bar and the next row, and nothing crosses
 * the text. One row of the window pays for it: 15 rows now end at y=147, above the
 * y=152 footer.
 *
 * The numbers themselves live in pdna_layout.h so tests/host_textfit_test.c checks THESE
 * ones (bar-contains-glyph-box, bar-clear-of-the-next-row, last row above the footer)
 * rather than a copy that never changes when this does. */
#define FILT_Y0     PDNA_FILT_Y0
#define FILT_ROW_H  PDNA_FILT_ROW_H
#define FILT_VIS    PDNA_FILT_VIS
/* BORDERLESS on purpose — see above. This is the one thing the host test cannot read
 * from a header: that this is ui_fill_rect and not ui_panel. */
static void filt_bar(int y) {
  ui_fill_rect(PDNA_FILT_BAR_X, y + PDNA_FILT_BAR_DY, PDNA_FILT_BAR_W, PDNA_FILT_BAR_H, UI_SEL);
}

/* ============================ THE ROW REPAINT RULE ==========================
 * Read this before touching any *_row / *_row_bg helper or any full-paint loop in this
 * file. Partial repaint means a row's background is no longer supplied by a screen-wide
 * ui_clear(): every row helper wipes its OWN rect and draws on top. Two invariants make
 * that safe, and BOTH have to hold or the screen paints differently depending on which
 * path last touched it:
 *
 * (1) A ROW PAINTS NOTHING OUTSIDE THE RECT ITS OWN WIPE COVERS.
 *     Otherwise the stray pixels belong to a neighbour's rect and the neighbour's wipe
 *     deletes them, so a row's appearance depends on whether the row next to it happened
 *     to be repainted. type_chip() broke this (9 px at y against a bar at y-1) until it
 *     was tied to PDNA_FILT_CHIP_DY/_H; nothing else in this file does.
 *
 * (2) WHERE TWO ROWS' RECTS OVERLAP BY CONSTRUCTION, THE SELECTED ROW WINS THE SHARED
 *     SCANLINE -- SO EVERY PAINT PATH PAINTS THE SELECTED ROW LAST.
 *     Two geometries here are one pixel taller than their own pitch and cannot be fixed
 *     without a layout change: IFILT's 12 px box on an 11 px pitch (shared scanline
 *     y+9) and the met-location / generic-list 9 px panel on an 8 px pitch (shared
 *     scanline y+7). The shared scanline is simply whichever of the two rows painted
 *     second. The row-pair diff paints old-then-new, i.e. selected LAST; a full paint
 *     walking rows ascending would instead let the row BELOW the cursor write it, and
 *     the selection bar would be 8 rows tall right after the screen opens and 9 rows
 *     tall after any cursor move. So the full paints below skip the selected row in the
 *     loop and draw it afterwards, which is also exactly what the pre-partial-repaint
 *     code (ui_clear + only the selected row drawing a bar) put on screen.
 *
 * RESIDUAL, stated rather than hidden: on the 9-on-8 geometry the shared scanline y+7
 * is also glyph row 7 of the 8 px cell, which libtonc's sys8 inks for , ; g j p q y. An
 * unselected row's wipe therefore clips the DESCENDERS of the row above it. Every name
 * these two screens can show today is upper-case (s_location[], s_nature[]), so nothing
 * is clipped in this build; closing it for a future mixed-case name_fn needs the
 * highlight to fit the pitch, which on an 8 px pitch with an 8 px glyph box means either
 * dropping the border (the sp_row / filt_bar idiom) or widening the pitch and losing a
 * visible row -- a layout decision, not a repaint fix, so it is not forced here.
 * ============================================================================ */

/* Row background for the FILT_* geometry (filter_menu + dex_menu share it): the bar is
 * exactly as tall as the row pitch (9 on 9), so this is the one list geometry where the
 * rects are disjoint and invariant (2) has nothing to arbitrate. It still needed
 * invariant (1): its type chip used to hang one scanline below this rect. */
static void filt_row_bg(int y, bool sel) {
  if (sel) filt_bar(y);
  else     ui_fill_rect(PDNA_FILT_BAR_X, y + PDNA_FILT_BAR_DY, PDNA_FILT_BAR_W, PDNA_FILT_BAR_H, UI_BG);
}

/* One row of the FILTER/SORT list: row 0 is the sort toggle, the rest are the
 * gen/type/legendary filter list (a type row draws a chip). */
static void fm_row(const int* fids, int r, int y, bool sel, int sort) {
  filt_row_bg(y, sel);
  if (r == 0) {
    char b[32];
    siprintf(b, PDNA_FILT_SORT_FMT, sort ? PDNA_FILT_SORT_NAME : PDNA_FILT_SORT_DEX);
    ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_DIRCLR, b);
  } else {
    int fid = fids[r - 1];
    if (fid >= 5) { type_chip(PDNA_FILT_TEXT_X, y, (uint8_t)(fid - 5));
                    ui_text(PDNA_FILT_TEXT_X + PDNA_FILT_CHIP_DX, y, sel ? UI_SELTEXT : UI_TEXT, filter_name(fid)); }
    else ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_TEXT, filter_name(fid));
  }
}

/* a nice list to jump to any filter (type rows show a colored chip) + sort toggle.
 *
 * D2: full = first paint, an overlay wiped us (never happens today -- this screen opens
 * nothing of its own -- but tracked anyway, the same uniform idiom every other picker in
 * this file uses so a call added here later needs no new bookkeeping), or the window
 * scrolled. A cursor move repaints the row pair; the A-toggle on row 0 (sel does not
 * move) repaints just that row. */
static void filter_menu(int* filter, int* sort) {
  int fids[24];
  int nf = filter_ids(fids);
  int rows = 1 + nf, sel = 0, top = 0;
  int prev_sel = -1, prev_top = -1, prev_sort = -1;
  uint32_t gen = 0; bool valid = false;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + FILT_VIS) top = sel - FILT_VIS + 1;

    bool full = !valid || gen != ui_clear_gen() || top != prev_top;

    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "FILTER / SORT");
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < FILT_VIS && top + i < rows; i++)   /* selected row LAST: rule (2) */
        if (top + i != sel) fm_row(fids, top + i, FILT_Y0 + i * FILT_ROW_H, false, *sort);
      if (sel >= top && sel < top + FILT_VIS && sel < rows)
        fm_row(fids, sel, FILT_Y0 + (sel - top) * FILT_ROW_H, true, *sort);
      ui_text(4, PDNA_FILT_FOOTER_Y, UI_DIM, PDNA_FILT_FOOT);
    } else if (sel != prev_sel) {
      int oi = prev_sel - top, ni = sel - top;
      if (oi >= 0 && oi < FILT_VIS) fm_row(fids, prev_sel, FILT_Y0 + oi * FILT_ROW_H, false, *sort);
      if (ni >= 0 && ni < FILT_VIS) fm_row(fids, sel,      FILT_Y0 + ni * FILT_ROW_H, true,  *sort);
    } else if (*sort != prev_sort) {
      int i = sel - top;
      if (i >= 0 && i < FILT_VIS) fm_row(fids, sel, FILT_Y0 + i * FILT_ROW_H, true, *sort);
    }

    prev_sel = sel; prev_top = top; prev_sort = *sort; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_A) { if (sel == 0) *sort ^= 1; else { *filter = fids[sel - 1]; return; } }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : rows - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % rows;
    else if (k & KEY_L)    sel = clampi(sel - 8, 0, rows - 1);
    else if (k & KEY_R)    sel = clampi(sel + 8, 0, rows - 1);
  }
}

/* One art-free species-picker row (9 px pitch, list mode): selection = a filled bar. */
static void sp_row(int y, uint16_t in, bool sel) {
  ui_fill_rect(2, y - 1, 236, 9, sel ? UI_SEL : UI_BG);
  char row[40];
  siprintf(row, "No.%03u", (unsigned)pk_national_no(in));
  ui_text(6, y, sel ? UI_SELTEXT : UI_DIM, row);
  ui_ptext_fit(66, y, 130, sel ? UI_SELTEXT : UI_TEXT, pk_species_name(in));
}

/* BACKLOG #330: declare the species grid's page to icon_store before the paint loop.
 * The picker is an overlay on the box screen, whose 25..30-row plan stays live (and its
 * rows PINNED) underneath. Undeclared, every picker cell was a non-plan miss that needs a
 * free victim slot -- but on the ROM rung the pool is only 4 slots, 3 of them pinned by the
 * box's plan and 1 hot, so slot_victim() answered -1 and icon_store_row() returned NULL:
 * a blank cell. (Emerald's cache rung has 6 slots, which is why only R/S went blank.)
 * Declaring replaces the box's plan with this page's, so the sweep refills the pool in
 * bulk groups exactly like the Pokedex. Artless only, same gate as dex_declare_page. */
static void species_declare_page(int top_idx, int vis) {
#if PDNA_MON_ICONS_ART_COMPILED
  (void)top_idx; (void)vis;
#else
  uint16_t rows[ICON_STORE_PLAN_MAX];
  int n = 0;
  for (int i = 0; i < vis && top_idx + i < g_n && n < (int)ICON_STORE_PLAN_MAX; i++)
    rows[n++] = art_icons_row_for(g_list[top_idx + i], 0);
  icon_store_plan(rows, n);
#endif
}

uint16_t pick_species(uint16_t current) {
  int filter = 0, sort = 0;
  char search[16] = "";
  build_species(filter, sort, search);
  int sel = 0;
  for (int i = 0; i < g_n; i++) if (g_list[i] == current) { sel = i; break; }

  char hdr[48];
  int prev_sel = -1, prev_top = -1, toprow = 0;
  bool relist = true;                 /* force a full redraw initially + after any list change */
  /* header-badge memo (D1): type_icon() -> app_type_badge() hits the SD in a ROM-registered
   * build (rom_type_sheet_load, ~12 sectors, no cache -- see type_icon()'s own comment) and
   * the header strip below used to call it on EVERY keypress, cursor-only moves included.
   * 0xFF is a safe sentinel: valid type ids are 0..17. */
  uint8_t prev_t1 = 0xFF, prev_t2 = 0xFF;
  /* gen/valid shadow (the same idiom the other three D1 sites use): a cancelled OSK
   * (SELECT into the search, SELECT out) wipes the whole screen via its own ui_clear()
   * and returns with relist false and top unchanged -- without this term neither the
   * badge nor the LIST was repainted after that, which had been a live blank-screen
   * bug on this exact path since before D1 attached the badge memo to it. */
  uint32_t gen = 0; bool valid = false;
  /* Art-free build: one text row per species instead of the icon grid (Guy's call).
   * Same machinery — the grid just collapses to 1 column of 9 px rows. */
#if !PDNA_MON_ICONS_ART_COMPILED
  /* BACKLOG #344: retire any plan the screen underneath left live BEFORE the probe. A box page
   * can rest with 3 of the ROM rung's 4 slots pinned (+1 hot = no victim), and the probe's
   * off-plan row 1 then answers NULL -- the picker opened as the TEXT LIST on Ruby (CREATE from
   * box 6). icon_store_plan(0, 0) drops plan + pins; the grid below declares its own page, and
   * the box re-declares its plan on return (oam_sync -> boxoam_declare_box). */
  icon_store_plan(0, 0);
#endif
  const bool lst = (mon_icon_for(1) == 0);
  const int cols = lst ? 1 : GCOLS, vrows = lst ? 13 : GVROWS;

  for (;;) {
    if (sel >= g_n) sel = g_n ? g_n - 1 : 0;
    int srow = sel / cols;            /* edge scroll: cursor roams the page, list moves only at the edges */
    if (srow < toprow) toprow = srow;
    if (srow >= toprow + vrows) toprow = srow - vrows + 1;
    if (toprow < 0) toprow = 0;
    int top_idx = toprow * cols;

    /* Only a page scroll or a list change needs a full repaint; moving the cursor
     * within the page just swaps the selection frame + repaints the header strip
     * (the slow per-pixel grid blit no longer runs on every keypress). */
    bool full = relist || !valid || gen != ui_clear_gen() || top_idx != prev_top;
    relist = false;

    if (full) {
      ui_clear();
      ui_hline(0, 21, UI_SCR_W, UI_BORDER);
      ui_hline(0, 147, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A pick  L/R filter  SEL find");
      if (!lst) species_declare_page(top_idx, cols * vrows);
      for (int i = 0; i < cols * vrows; i++) {
        int idx = top_idx + i;
        if (idx >= g_n) break;
        if (lst) sp_row(24 + i * 9, g_list[idx], idx == sel);
        else {
          int x = GX + (i % GCOLS) * GCELLX, y = GY + (i / GCOLS) * GCELLY;
          ui_icon_scaled(x, y, GICON, GICON, mon_icon_for(g_list[idx]));
        }
      }
    } else if (prev_sel >= top_idx && prev_sel < top_idx + cols * vrows) {
      int pi = prev_sel - top_idx;                 /* erase the old selection */
      if (lst) sp_row(24 + pi * 9, g_list[prev_sel], false);
      else {
        int px = GX + (pi % GCOLS) * GCELLX, py = GY + (pi / GCOLS) * GCELLY;
        m3_frame(px - 1, py - 1, px + GICON, py + GICON, UI_BG);
        if (pi < GCOLS) ui_hline(px - 1, 21, GICON + 2, UI_BORDER);   /* the frame's top edge IS the rule row: restore it (#338) */
      }
    }

    if (g_n) {                                      /* draw the current selection */
      int si = sel - top_idx;
      if (lst) { if (!full) sp_row(24 + si * 9, g_list[sel], true); }
      else {
        int sx = GX + (si % GCOLS) * GCELLX, sy = GY + (si / GCOLS) * GCELLY;
        m3_frame(sx - 1, sy - 1, sx + GICON, sy + GICON, UI_SELTEXT);
      }
    }

    /* header strip (No./name + type badges + filter line) — repaint just this band.
     * The badge rect (x=172..240, y=4..20 -- wide enough for the tallest ROM badge,
     * ROM_TYPE_BADGE_H_RSE=16) is carved OUT of this wipe: it is wiped and redrawn only
     * below, when (t1,t2) actually changed. The title text and filter line still repaint
     * unconditionally every press exactly as before (cheap, no SD) — this memo touches
     * only the badge call. */
    ui_fill_rect(0, 0, 172, 21, UI_BG);
    ui_fill_rect(172, 0, UI_SCR_W - 172, 4, UI_BG);
    ui_fill_rect(172, 20, UI_SCR_W - 172, 1, UI_BG);
    uint16_t cs = g_n ? g_list[sel] : 0;
    if (cs) {
      siprintf(hdr, "No.%u  %s", (unsigned)pk_national_no(cs), pk_species_name(cs));
      ui_text(4, 2, UI_TITLE, hdr);
      uint8_t t1 = pk_species_type1(cs), t2 = pk_species_type2(cs);
      if (full || t1 != prev_t1 || t2 != prev_t2) {
        ui_fill_rect(172, 4, UI_SCR_W - 172, 16, UI_BG);   /* clip wipe: only the badge rect */
        if (t1 == t2) type_icon(204, 4, t1);
        else { type_icon(172, 4, t1); type_icon(205, 4, t2); }
        prev_t1 = t1; prev_t2 = t2;
      }
    } else { prev_t1 = 0xFF; prev_t2 = 0xFF; }
    siprintf(hdr, "[%s] sort:%s  %d", filter_name(filter), sort ? "A-Z" : "No.", g_n);
    ui_text(4, 11, UI_DIM, hdr);

    prev_sel = sel; prev_top = top_idx; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT | KEY_START);
    if (k & (KEY_B | KEY_A)) {
      icon_store_plan(0, 0);              /* retire this page's plan (the box repaint re-declares its own) */
      return ((k & KEY_B) || !g_n) ? CANCEL : g_list[sel];
    }
    else if (k & KEY_LEFT)  sel = (sel > 0) ? sel - 1 : 0;
    else if (k & KEY_RIGHT) sel = (sel < g_n - 1) ? sel + 1 : sel;
    else if (k & KEY_UP)    { if (sel >= cols) sel -= cols; }
    else if (k & KEY_DOWN)  { if (sel + cols < g_n) sel += cols; }
    else if (k & KEY_L) { do { filter = (filter + NFILTER - 1) % NFILTER; } while (!filter_usable(filter)); build_species(filter, sort, search); sel = 0; relist = true; }
    else if (k & KEY_R) { do { filter = (filter + 1) % NFILTER; } while (!filter_usable(filter)); build_species(filter, sort, search); sel = 0; relist = true; }
    else if (k & KEY_START) { filter_menu(&filter, &sort); build_species(filter, sort, search); sel = 0; relist = true; }
    else if (k & KEY_SELECT) {
      char q[16];
      if (osk_search("SEARCH", search, q, sizeof(q))) { strcpy(search, q); build_species(filter, sort, search); sel = 0; relist = true; }
    }
  }
}

/* ===================== Pokedex viewer / editor ========================= */
/* HGSS-style Pokedex on top of the same species grid + filters. Per-state render:
 * UNSEEN greyscale, SEEN full-colour static, CAUGHT full-colour + a 2-frame bob
 * (all caught cells share one frame counter so they bob in unison) and a Poke-Ball
 * corner marker. A cycles a species unseen->seen->caught (Omega-only edit). Three
 * views (L/R), the gen/type/legendary filter + caught-status filter + name search
 * (START / SELECT). Reuses g_list + build_species: no new EWRAM. */

#define DEX_ANIM_PERIOD 30          /* vblanks per bob frame (~0.5s, the Gen-3 cadence) */
/* BACKLOG #296: one bob flip used to repaint every caught cell in ONE uninterrupted run (21
 * cells through the artless ladder = ~3.3 frames of mGBA CPU, measured by frame-end PC
 * sampling + an instruction-step trace). A 3-frame key press that started and ended inside
 * that run was never polled -- a dropped DOWN. The flip is now sliced: DEX_BOB_SLICE cells per
 * idle loop pass (~0.6 frame), so s_vsync()'s key_poll runs at least once per frame between
 * slices. DEX_BOB_DONE = no flip pass in progress (any value >= the page's cell count). */
#define DEX_BOB_SLICE 4
#define DEX_BOB_DONE  0x4000

#define DV_GRID 0
#define DV_LIST 1
#define DV_TYPE 2
#define DV_N    3
static const char* const DV_NAME[DV_N] = { "Grid", "List", "Type" };

#define DS_ALL    0
#define DS_CAUGHT 1
#define DS_SEEN   2
#define DS_UNSEEN 3
#define DS_N      4
static const char* const DS_NAME[DS_N] = { "All", "Caught", "Seen", "Unseen" };

/* BACKLOG #204: packed into one struct (was four separate file-static function
 * pointers) so the stack walker's struct-field classifier has ONE stable section
 * anchor for this dispatch family -- every new static ahead of these four used to
 * shift their individual section-anchor offsets and need another "coincidental
 * spilled section-anchor" line in tools/stack_edges.txt (b195 added @32/@36; see
 * that file's git history). A plain file-static struct is STILL not enough on its
 * own: -fsection-anchors (default at -O2 for this target) groups every small
 * .bss/.data static in a TRANSLATION UNIT under ONE shared base register and
 * reaches each by a #offset from it, so s_dex's four fields would still ride
 * whatever unrelated statics the compiler happens to place immediately before it
 * (g_species_max_dex/g_n measured on this build) -- the exact same class of
 * collision, just now contiguous instead of scattered. The section attribute below
 * gives s_dex its OWN single-member input section, so GCC's per-section anchor
 * pass can never fold it in with a neighbor: s_dex.get/set/getnat/setnat are then
 * ALWAYS at offsets 0/4/8/12 from s_dex's own anchor, and no other static's
 * addition or removal in this file can ever move them again. Confirmed empirically
 * (see the walker's own --dump-sites report before/after this attribute). */
static struct {
  DexGetState get;
  DexSetState set;
  DexGetNat   getnat;   /* national-dex live? (may be NULL) */
  DexSetNat   setnat;   /* enable/disable national dex (may be NULL) */
} s_dex __attribute__((section(".bss.pdna_pick_s_dex")));
/* BACKLOG #124: optional cell-art override, installed by pdna_gbdex.c around a
 * GB-session dex visit only -- see pdna_pick.h's own comment on pdna_dex_set_cell_art
 * for the full contract (NULL by default, so an ordinary Gen-3 dex visit is
 * byte-for-byte unchanged: dex_cell_art_call() below is simply never true).
 *
 * EWRAM_BSS (review ruling, BACKLOG #124): a plain file-static pair here would cost 8
 * B of IWRAM .bss, which is stack headroom on this build (the arena tail this brief's
 * "no new statics" line means) -- pushing the stack margin from 2,080 to 2,072,
 * a STOP-worthy shrink caught by tools/stack_budget.py. EWRAM has 1,172 B free and
 * does not feed the stack ceiling at all, so the SAME 8 bytes cost nothing there. */
typedef struct { PdnaDexCellArtFn fn; void* ctx; bool serves_page;
                 uint8_t detail_gen;   /* BACKLOG #203: 0 = Gen-3 detail art, PDNA_GEN1/2 = GB (lives in the struct's padding: no new static) */
               } DexCellArtOverride;
static EWRAM_BSS DexCellArtOverride s_cell_art;

void pdna_dex_set_detail_gen(uint8_t gen) { s_cell_art.detail_gen = gen; }

void pdna_dex_set_cell_art(PdnaDexCellArtFn fn, void* ctx, bool serves_page) {
  s_cell_art.fn = fn; s_cell_art.ctx = ctx; s_cell_art.serves_page = serves_page;
}

/* BACKLOG #208: the page-begin hook, PDNA_DELTA-only end to end (see pdna_pick.h's
 * own comment on PdnaDexPageFn for why) -- same EWRAM_BSS posture as s_cell_art
 * above (a plain file-static pointer here costs IWRAM .bss the stack ceiling counts
 * against; EWRAM does not). NULL by default -- an ordinary Gen-3 dex visit never
 * installs this, so dex_page_begin_call() below is simply never true for it. */
#ifdef PDNA_DELTA
static EWRAM_BSS PdnaDexPageFn s_page_begin_fn;

void pdna_dex_set_page_begin(PdnaDexPageFn fn) { s_page_begin_fn = fn; }

/* Same discipline as dex_cell_art_call() below (BACKLOG #124's own review ruling):
 * the ONE place that dispatches through the stored pointer, out of line, so the
 * compiled indirect-call-site count for this dispatch cannot silently disagree
 * between the artless and normal build variants. Declared in tools/stack_edges.txt
 * (the walker's whole-graph sweep cannot see through a value loaded from a file
 * static). */
static __attribute__((noinline)) void dex_page_begin_call(void) {
  if (s_page_begin_fn) s_page_begin_fn();
}
#endif

/* Review A5 cross-review finding: "does the override serve THIS PAGE at all" (used to
 * skip the icon-store plan and the bob-animation refresh) must NOT be re-derived here
 * from pdna_origin_art_have(PDNA_GEN2) -- that global is boot-sticky and independent
 * per era (Settings can register a Gen-1 AND a Gen-2 ROM at once), so a Gen-1 (Red)
 * dex visit with a Gen-2 ROM also registered would answer "yes" from have(GEN2) alone
 * even though gbdex_cell_art() refuses every cell of a Gen-1 session (its own gate
 * also checks the session's gen). The installer (pdna_gbdex.c) already computes the
 * exact same expression gbdex_cell_art() gates on, ONCE, at install time -- this just
 * reads the answer it stored, so the two questions can never re-diverge. noinline for
 * the same reason dex_cell_art_call() below is: an out-of-line query stays cheaper to
 * reason about than an inline field read if this function's callers ever change
 * again (a previous version of this function DID measurably perturb -O2's register
 * allocation elsewhere in this TU when read inline -- see this file's own git
 * history for the churn that cost). */
static bool __attribute__((noinline)) dex_cell_art_page_served(void) {
  return s_cell_art.fn && s_cell_art.serves_page;
}

/* Review ruling (BACKLOG #124, STOP 2): the ONLY place in this file that actually
 * dispatches through s_cell_art.fn -- every caller (dex_cell_grid() below) goes
 * through this one noinline function instead of calling s_cell_art.fn directly, so
 * the compiled indirect-call-site COUNT for this dispatch is exactly 1 in every build
 * variant (artless and normal used to disagree -- 2 vs 1 -- when dex_cell_grid called
 * s_cell_art.fn inline, because the two builds' surrounding code shapes differ enough
 * at -O2 to change how many call sites that one C call site compiles to; a single
 * noinline wrapper removes the surrounding shape from the question entirely).
 * tools/stack_edges.txt declares this one site, verified by address on both ELFs. */
static __attribute__((noinline)) bool
dex_cell_art_call(uint16_t dex, int x, int y, int w, int h) {
  if (!s_cell_art.fn) return false;
  return s_cell_art.fn(dex, x, y, w, h, s_cell_art.ctx);
}

/* Species cap for the shared screen (BACKLOG #87): the Gen-3 dex is a fixed 386,
 * but Gen 1 (151) / Gen 2 (251) sessions reuse this same screen and must not see or
 * bulk-touch species past their generation's dex. Reset to 386 by every caller on
 * entry (pdna_dex_edit in pdna_main.c) so a prior GB visit can never leak into the
 * next Gen-3 one — this file never assumes the previous session cleaned up after
 * itself. 0 is never a valid value (see the clamp below); default is the full 386.
 * (D5, b87 fix pass: DEX_NAT_MAX/s_dex_max themselves now live up near
 * filter_usable() -- this comment stays here, where the reader first meets the
 * cap's own story.) */
void pdna_dex_set_max(int max_dex) {
  s_dex_max = (max_dex > 0 && max_dex <= DEX_NAT_MAX) ? max_dex : DEX_NAT_MAX;
}

/* Snapshot of every species' dex state before the last bulk op, so a mistaken
 * "Catch/See/Wipe ALL" can be undone in one step (the changes aren't written to the
 * SD until the user confirms the dex save, so this RAM revert fully restores it). */
static int8_t s_dex_snap[DEX_NAT_MAX];
static bool   s_dex_snap_valid = false;
static bool   s_dex_snap_natl = false;   /* National-Dex state at snapshot time (Catch ALL flips it) */

/* BACKLOG #204: every internal caller dispatches s_dex's four fields through these
 * four noinline wrappers instead of touching s_dex.FIELD directly -- same technique
 * as dex_cell_art_call() above for s_cell_art.fn (BACKLOG #124), for the same
 * reason: the artless and full-art builds inline dex_bulk()/dex_counts() etc. into
 * pdna_dex_screen differently enough that the RAW field-dispatch site count/split
 * (bare vs struct-field-classified) measurably disagreed between the two variants
 * on this exact struct (3 bare + 12 field in artless, 1 bare + 14 field in normal,
 * both summing to 15 -- confirmed via --dump-sites) even though the section
 * attribute above already pins s_dex's OWN offsets stable. tools/stack_budget.py's
 * `caller argsites=N` declaration is a single GLOBAL count with no per-variant
 * knob, so two different bare-argsite counts for the same symbol name is a hard
 * conflict, not a matter of picking the "right" number. Routing every dispatch
 * through one noinline call site per field removes the variance by construction
 * (matches this file's OWN prior fix for the exact same class of problem): every
 * caller sees a plain direct `bl dex_dget`/etc., and each wrapper's own single
 * field-classified access is unaffected by how any OTHER function inlines around
 * it. dex_getnat_live()/dex_setnat_call() also fold in the NULL-checks every call
 * site used to repeat by hand (getnat/setnat may be NULL -- see the struct's own
 * field comments), so behaviour is unchanged at every existing site — except the one
 * NULL-presence test at the Undo branch, a value read, not a dispatch. */
static __attribute__((noinline)) int  dex_dget(int nat) { return s_dex.get(nat); }
static __attribute__((noinline)) void dex_dset(int nat, int st) { s_dex.set(nat, st); }
static __attribute__((noinline)) bool dex_getnat_live(void) { return s_dex.getnat && s_dex.getnat(); }
static __attribute__((noinline)) void dex_setnat_call(bool on) { if (s_dex.setnat) s_dex.setnat(on); }

static int dstate(uint16_t internal) { return dex_dget((int)pk_national_no(internal)); }

/* Build g_list for the dex: species filter (build_species) -> caught-status filter
 * -> (Type view only) a stable sort by primary type. */
static void dex_build(int filter, int sort, const char* search, int status, int view) {
  build_species(filter, sort, search);                 /* g_list/g_n by No. or A-Z */
  if (s_dex_max < DEX_NAT_MAX) {                        /* GB session: hide species past the cap */
    int w = 0;
    for (int i = 0; i < g_n; i++)
      if (pk_national_no(g_list[i]) <= (uint16_t)s_dex_max) g_list[w++] = g_list[i];
    g_n = w;
  }
  if (status != DS_ALL) {                              /* keep only the matching states */
    int w = 0;
    for (int i = 0; i < g_n; i++) {
      int st = dstate(g_list[i]);
      bool keep = (status == DS_CAUGHT) ? (st == 2)
                : (status == DS_SEEN)   ? (st == 1)
                                        : (st == 0);   /* DS_UNSEEN */
      if (keep) g_list[w++] = g_list[i];
    }
    g_n = w;
  }
  if (view == DV_TYPE) {                                /* stable insertion sort by type1 */
    for (int i = 1; i < g_n; i++) {
      uint16_t v = g_list[i]; uint8_t tv = pk_species_type1(v); int j = i - 1;
      while (j >= 0 && pk_species_type1(g_list[j]) > tv) { g_list[j + 1] = g_list[j]; j--; }
      g_list[j + 1] = v;
    }
  }
}

static void dex_counts(int* seen, int* caught) {
  int s = 0, c = 0;
  for (int nat = 1; nat <= s_dex_max; nat++) { int st = dex_dget(nat); if (st >= 1) s++; if (st >= 2) c++; }
  *seen = s; *caught = c;
}

static void dex_geom(int view, int* cols, int* cw, int* ch, int* x0, int* y0, int* vrows) {
  if (view == DV_LIST) { *cols = 1; *cw = 232; *ch = 9;  *x0 = 4; *y0 = 24; *vrows = 13; }
  else                 { *cols = 7; *cw = 33;  *ch = 34; *x0 = 8; *y0 = 24; *vrows = 3;  }
}

/* Tell icon_store the page's 21 icon rows one call BEFORE the paint loop asks for them.
 *
 * This screen is the reason the store has a plan at all. It re-requests every visible
 * cell on a full repaint, on every one-row scroll step (all 21, including the 14 that
 * did not move), and on every bob flip -- and it does so in list order, which is a
 * CYCLIC SWEEP through a set far larger than the pool. Against a cache smaller than the
 * page that has a hit rate of exactly zero: every access evicts the entry needed 21
 * accesses later. Measured on the host FatFs harness before this call existed, a
 * 21-cell page cost 21 separate transfers on each of those events.
 *
 * Declared, the store sorts the misses by source offset, merges consecutive rows and
 * sweeps forward in pool-sized bulk groups instead. Under the default filter + No.
 * sort, national 1-251 map 1:1 and monotonically onto icons.bin rows 1-251, so most
 * full pages are one contiguous 21-row span and each group collapses to ONE f_read.
 * A-Z sort and the type view destroy that locality; the sweep then degenerates to one
 * read per row IN ASCENDING OFFSET ORDER, which is still strictly better than the
 * arbitrary order the paint loop would otherwise seek in.
 *
 * ONLY IN THE ARTLESS BUILD. With mon_icons.c linked, mon_icon_for* is compiled
 * .rodata and never reaches the store -- but icon_store is still live underneath
 * box_oam.c, so declaring here would make a full-art build read 21 KiB off the card for
 * a plan nothing consumes. That is a regression, not a no-op, hence the gate.
 *
 * List view declares nothing: dex_cell_list is text only, zero rows, zero I/O. */
static void dex_declare_page(bool grid, int top, int vis) {
  /* BACKLOG #208: PDNA_DELTA-only (see pdna_pick.h's own comment on PdnaDexPageFn),
   * independent of PDNA_MON_ICONS_ART_COMPILED (icon_store below is a Gen-3 concern;
   * the GB ROM art path runs the same in every build). List view draws no art at all
   * (this function's own rule, both branches below), so the hook only fires for
   * grid. */
#ifdef PDNA_DELTA
  if (grid) dex_page_begin_call();
#endif
#if PDNA_MON_ICONS_ART_COMPILED
  (void)grid; (void)top; (void)vis;
#else
  uint16_t rows[ICON_STORE_PLAN_MAX];
  int n = 0;
  if (!grid) { icon_store_plan(0, 0); return; }
  if (dex_cell_art_page_served()) { icon_store_plan(0, 0); return; }  /* BACKLOG #124 review A5: a page the GB override serves must not also plan+read icon-store rows */
  for (int i = 0; i < vis && top + i < g_n && n < (int)ICON_STORE_PLAN_MAX; i++)
    rows[n++] = art_icons_row_for(g_list[top + i], 0);
  icon_store_plan(rows, n);
#endif
}

/* one grid/type cell at the icon origin: greyscale unseen, colour seen,
 * colour-at-frame-`bob` + Poke-Ball caught. */
static void dex_cell_grid(int x, int y, uint16_t in, int bob) {
  int st = dstate(in);
  /* BACKLOG #124: the GB-session override, seen/caught cells only (st==0 keeps the
   * existing grey/`?` treatment below -- the override reveals nothing the current
   * build does not already reveal for a SEEN species, but must not reveal MORE for
   * an unseen one). A `false` return (ROM not registered right now, stack too
   * shallow, species not servable) falls straight through to the unchanged path. */
  if (st != 0 && dex_cell_art_call(pk_national_no(in), x, y, 32, 32)) {
    if (st == 2) ui_pokeball(x + 21, y + 21);
    return;
  }
  if (!mon_icon_for(in)) {                                   /* art-free build */
    ui_name_chip(x, y + 9, 32, 13, UI_PANEL, st == 0 ? UI_DIM : UI_TEXT,
                 st == 0 ? "?" : pk_species_name(in));
    if (st == 2) ui_pokeball(x + 21, y + 21);
    return;
  }
  if (st == 0)      ui_icon_scaled_grey(x, y, 32, 32, mon_icon_for(in));
  else if (st == 1) ui_icon_scaled(x, y, 32, 32, mon_icon_for(in));
  else { ui_icon_scaled(x, y, 32, 32, mon_icon_for_frame(in, (uint8_t)bob)); ui_pokeball(x + 21, y + 21); }
}

/* one list row text, coloured by state (or the selection colour) */
static void dex_cell_list(int x, int y, uint16_t in, bool sel) {
  int nat = pk_national_no(in), st = dex_dget(nat);
  char row[44], rt[44];
  siprintf(row, "%03d %-11s %s", nat, pk_species_name(in), st == 2 ? "CAUGHT" : st == 1 ? "seen" : "-");
  ui_truncate(rt, row, 28);
  ui_text(x + 2, y, sel ? UI_SELTEXT : st == 2 ? UI_OK : st == 1 ? UI_TEXT : UI_DIM, rt);
}

static void dex_header(int view, int filter, int status, uint16_t sel_in, int seen, int caught) {
  ui_fill_rect(0, 0, UI_SCR_W, 22, UI_BG);
  char h[64], ht[40];
  if (sel_in) siprintf(h, "No.%u %s", (unsigned)pk_national_no(sel_in), pk_species_name(sel_in));
  else        strcpy(h, "POKEDEX");
  ui_truncate(ht, h, 19);
  ui_text(4, 2, UI_TITLE, ht);
  siprintf(h, "S%d C%d", seen, caught);
  ui_text(168, 2, UI_DIM, h);
  if (sel_in) {
    uint8_t t1 = pk_species_type1(sel_in), t2 = pk_species_type2(sel_in);
    if (t1 == t2) siprintf(h, "%s  %s  %s   %s", DV_NAME[view], filter_name(filter), DS_NAME[status], pk_type_name(t1));
    else          siprintf(h, "%s  %s  %s   %s/%s", DV_NAME[view], filter_name(filter), DS_NAME[status], pk_type_name(t1), pk_type_name(t2));
  } else siprintf(h, "%s  %s  %s", DV_NAME[view], filter_name(filter), DS_NAME[status]);
  ui_truncate(ht, h, 29);
  ui_text(4, 12, UI_DIM, ht);
  ui_hline(0, 22, UI_SCR_W, UI_BORDER);
}

/* SELECT-all overlay: catch / see / wipe every species, plus the National Dex
 * toggle (the flag/var/magic trio that actually reveals #152..386 in-game).
 * Returns true if anything changed. */
static bool dex_bulk(void) {
  int sel = 0;
  for (;;) {
    /* options: Catch/See/Wipe ALL (state 2/1/0), National-Dex toggle (3), Undo (-1), Cancel (-2) */
    const char* L[PDNA_DEXBULK_MAX]; int act[PDNA_DEXBULK_MAX], n = 0;
    bool natl = dex_getnat_live();
    L[n] = "Catch ALL"; act[n++] = 2;
    L[n] = "See ALL";   act[n++] = 1;
    L[n] = "Wipe ALL";  act[n++] = 0;
    if (s_dex.setnat) { L[n] = natl ? "Natl Dex: ON" : "Natl Dex: OFF"; act[n++] = 3; }
    if (s_dex_snap_valid) { L[n] = "Undo last"; act[n++] = -1; }
    L[n] = "Cancel"; act[n++] = -2;
    if (sel >= n) sel = n - 1;
    /* Six options put the old fixed my=42 panel at 42..154 — its bottom border and hint
     * row landed on the dex screen's own footer. Laid out above it instead. */
    int my, mh;
    ui_popup_vfit(n, PDNA_DEXBULK_ROW_H, PDNA_DEXBULK_HEAD, PDNA_DEXBULK_FOOT, &my, &mh);
    const int mx = 56, mw = 128;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "DEX: ALL");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < n; i++) { int y = my + PDNA_DEXBULK_HEAD + i * PDNA_DEXBULK_ROW_H; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      uint16_t col = s ? UI_SELTEXT : (act[i] == -1 ? UI_OK : act[i] == 3 ? (natl ? UI_OK : UI_WARN) : UI_TEXT);
      ui_text(mx + 10, y, col, L[i]); }
    ui_text(mx + 6, my + mh + PDNA_POPUP_HINT_DY, UI_DIM, "A pick  B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      int a = act[sel];
      if (a == -2) return false;                                  /* Cancel */
      if (a == 3) {                                               /* toggle National Dex */
        bool now = !natl;
        if (!app_confirm(now ? "Enable National Dex?" : "Disable National Dex?",
                         now ? "Reveals #152-386 in-game." : "Hides #152-386 in-game."))
          return false;
        dex_setnat_call(now);
        return true;
      }
      if (a == -1) {                                              /* Undo the last bulk op */
        for (int nat = 1; nat <= s_dex_max; nat++) dex_dset(nat, s_dex_snap[nat - 1]);
        /* This is equivalent to the old setnat && getnat guard because s_dex_snap_natl
         * is seeded from dex_getnat_live() in the same session and s_dex_snap_valid
         * resets on entry. */
        if (dex_getnat_live() != s_dex_snap_natl)
          dex_setnat_call(s_dex_snap_natl);                              /* Catch ALL auto-unlocked natl -> revert too */
        s_dex_snap_valid = false;
        return true;
      }
      { char amsg[28]; siprintf(amsg, "All %d. (Undo available.)", s_dex_max);
        if (!app_confirm(a == 2 ? "Catch every species?" : a == 1 ? "See every species?" : "Wipe the whole dex?",
                         amsg)) return false; }
      for (int nat = 1; nat <= s_dex_max; nat++) s_dex_snap[nat - 1] = (int8_t)dex_dget(nat);   /* snapshot first */
      s_dex_snap_natl = dex_getnat_live();                 /* incl. the National-Dex state */
      s_dex_snap_valid = true;
      for (int nat = 1; nat <= s_dex_max; nat++) dex_dset(nat, a);
      /* Catching every species is meaningless without National mode (the dex caps at the
       * regional list otherwise), so unlock it too — matches the user's expectation. */
      if (a == 2) dex_setnat_call(true);
      return true;
    }
  }
}

/* One row of dex_menu's list: rows 0/1 are the sort/status toggles, row 2 (can_edit
 * only) is the "Mark all..." bulk-edit entry, the rest are the gen/type/legendary
 * filter list -- same shape as fm_row, with the two extra fixed rows spliced in. */
static void dxm_row(const int* fids, int base, bool can_edit, int r, int y, bool sel,
                    int sort, int status) {
  filt_row_bg(y, sel);
  if (r == 0) { char b[32];
                siprintf(b, PDNA_FILT_SORT_FMT, sort ? PDNA_FILT_SORT_NAME : PDNA_FILT_SORT_DEX);
                ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_DIRCLR, b); }
  else if (r == 1) { char b[32]; siprintf(b, "Status: %s", DS_NAME[status]);
                     ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_DIRCLR, b); }
  else if (can_edit && r == 2) ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_WARN, PDNA_FILT_MARKALL);
  else { int fid = fids[r - base];
         if (fid >= 5) { type_chip(PDNA_FILT_TEXT_X, y, (uint8_t)(fid - 5));
                         ui_text(PDNA_FILT_TEXT_X + PDNA_FILT_CHIP_DX, y, sel ? UI_SELTEXT : UI_TEXT, filter_name(fid)); }
         else ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_TEXT, filter_name(fid)); }
}

/* START menu: sort toggle, status cycle, optional "Mark all", then the
 * gen/type/legendary filter list. Returns 0 nothing, 1 rebuild needed, 2 the dex
 * was bulk-changed (rebuild + mark dirty).
 *
 * THE TRAP (per the brief): dex_bulk() is a sub-overlay, but unlike every OSK/picker in
 * this codebase it does NOT open with a ui_clear() -- it draws its confirm-list panel
 * directly on top of whatever is already on screen (its own "stay open" comment). So
 * `gen != ui_clear_gen()` -- the mechanism every other trap in this file relies on --
 * genuinely does not see it: dex_bulk's Cancel and Undo-last paths can return having
 * called ui_clear() zero times, leaving its popup's pixels sitting over this screen's
 * rows with nothing in the paint state to say so. (The "counts on other rows" shape the
 * brief describes does not actually apply here -- this menu shows no seen/caught
 * numbers, only the sort/status/filter rows below; pdna_dex_screen's dex_header owns
 * those, one call frame up.) Simpler-correct fix taken: force a full repaint the
 * iteration after ANY call to dex_bulk(), whether or not it changed anything -- cheaper
 * to reason about than reconstructing which of dex_bulk's several return paths did or
 * did not paint, and it is what `relist` already means everywhere else in this file. */
static int dex_menu(int* filter, int* sort, int* status, bool can_edit) {
  int fids[24]; int nf = filter_ids(fids);
  int base = can_edit ? 3 : 2;                          /* rows before the filter list */
  int rows = base + nf, sel = 0, top = 0;
  bool changed = false, bulked = false;
  int prev_sel = -1, prev_top = -1, prev_sort = -1, prev_status = -1;
  bool relist = false;
  uint32_t gen = 0; bool valid = false;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + FILT_VIS) top = sel - FILT_VIS + 1;

    bool full = relist || !valid || gen != ui_clear_gen() || top != prev_top;
    relist = false;

    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "FILTER / SORT / FIND");
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < FILT_VIS && top + i < rows; i++)   /* selected row LAST: rule (2) */
        if (top + i != sel)
          dxm_row(fids, base, can_edit, top + i, FILT_Y0 + i * FILT_ROW_H, false, *sort, *status);
      if (sel >= top && sel < top + FILT_VIS && sel < rows)
        dxm_row(fids, base, can_edit, sel, FILT_Y0 + (sel - top) * FILT_ROW_H, true, *sort, *status);
      ui_text(4, PDNA_FILT_FOOTER_Y, UI_DIM, PDNA_FILT_FOOT);
    } else if (sel != prev_sel) {
      int oi = prev_sel - top, ni = sel - top;
      if (oi >= 0 && oi < FILT_VIS) dxm_row(fids, base, can_edit, prev_sel, FILT_Y0 + oi * FILT_ROW_H, false, *sort, *status);
      if (ni >= 0 && ni < FILT_VIS) dxm_row(fids, base, can_edit, sel,      FILT_Y0 + ni * FILT_ROW_H, true,  *sort, *status);
    } else if (*sort != prev_sort || *status != prev_status) {
      int i = sel - top;
      if (i >= 0 && i < FILT_VIS) dxm_row(fids, base, can_edit, sel, FILT_Y0 + i * FILT_ROW_H, true, *sort, *status);
    }

    prev_sel = sel; prev_top = top; prev_sort = *sort; prev_status = *status; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B);
    if (k & KEY_B) return bulked ? 2 : (changed ? 1 : 0);
    else if (k & KEY_A) {
      if (sel == 0)       { *sort ^= 1; changed = true; }
      else if (sel == 1)  { *status = (*status + 1) % DS_N; changed = true; }
      else if (can_edit && sel == 2) { if (dex_bulk()) bulked = true; relist = true; }   /* stay open */
      else { *filter = fids[sel - base]; return bulked ? 2 : 1; }         /* pick -> close */
    }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : rows - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % rows;
    else if (k & KEY_L)    sel = clampi(sel - 8, 0, rows - 1);
    else if (k & KEY_R)    sel = clampi(sel + 8, 0, rows - 1);
  }
}

/* ===================== BACKLOG #203: Pokedex DETAIL view ======================
 * One screen for every generation, opened with A from a dex cell/row (Gen-3 dex AND the
 * Gen-1/2 dex -- pdna_gbdex.c reuses pdna_dex_screen, so this IS the shared code). Walks
 * g_list/g_n in place (the grid's filtered order; never rebuilt here). Portrait panel in
 * the summary's left-column shape; art is species-keyed (no owned PkMon exists for a dex
 * entry): Gen 3 -> pdna_origin_art_front_by_species(INTERNAL id), GB -> by_dex_gen(gen,
 * NATIONAL no.). Unseen entries show no art (the grid reveals nothing for state 0 either).
 * Keys: B back; L/R previous/next entry (wraps); A cycles seen/caught when can_edit, deny
 * tone otherwise. *sel_io follows the viewed entry. Returns true iff a state changed. */
static void dex_detail_art(uint16_t in, int st) {
  ui_panel(0, 11, 92, 139, RGB15(4, 7, 16), UI_BORDER);
  m3_frame(11, 13, 80, 78, UI_BORDER);
  pdna_summary_portrait_screen();
  PdnaArt art; memset(&art, 0, sizeof art);
  if (st != 0) {
    rumble_io_suspend();   /* the portrait fetch decompresses from ROM */
    if (s_cell_art.detail_gen)
      (void)pdna_origin_art_portrait_by_dex_gen(s_cell_art.detail_gen, (uint16_t)pk_national_no(in), &art);
    else
      (void)pdna_origin_art_front_by_species(in, &art);
    rumble_io_resume();
  }
  if (art.px) {
    int ax, ay;
    pdna_origin_art_place(&art, 12, 14, 68, 64, &ax, &ay);
    if (art.gen != PDNA_GEN3) ui_fill_rect(ax, ay, art.w, art.h, 0x7FFF);   /* #275: the page behind a GB picture */
    ui_sprite(ax, ay, art.w, art.h, art.px);
  } else if (st != 0 && mon_icon_for_form(in, 0)) {
    ui_sprite(30, 30, MON_ICON_W, MON_ICON_W, mon_icon_for_form(in, 0));   /* trainer-card fallback icon */
  } else {
    ui_text(38, 42, UI_DIM, st == 0 ? "?" : "no art");
  }
  char buf[24];
  ui_hline(4, 81, 84, UI_BORDER);
  siprintf(buf, "#%03u", (unsigned)pk_national_no(in));
  ui_text(6, 84, UI_DIRCLR, buf);
  ui_ptext_fit(6, 94, 86, UI_TEXT, st ? pk_species_name(in) : "?????");
}

/* The right-hand text block; repainted alone after an A-cycle. */
static void dex_detail_text(uint16_t in, int st, int idx) {
  ui_fill_rect(94, 12, UI_SCR_W - 94, 134, UI_BG);
  char b[40];
  ui_ptext_fit(100, 20, 136, UI_TITLE, pk_species_name(in));
  siprintf(b, "No. %03u", (unsigned)pk_national_no(in));
  ui_text(100, 38, UI_DIRCLR, b);
  siprintf(b, "%s", dex_detail_status_word(st));
  ui_text(100, 52, st == 2 ? UI_OK : st == 1 ? UI_TEXT : UI_DIM, b);
  if (st != 0) {
    uint8_t t1 = pk_species_type1(in), t2 = pk_species_type2(in);
    if (t1 == t2) siprintf(b, "%s", pk_type_name(t1));
    else          siprintf(b, "%s/%s", pk_type_name(t1), pk_type_name(t2));
    ui_text(100, 66, UI_DIM, b);
  }
  siprintf(b, "%d/%d", idx + 1, g_n);
  ui_text(100, 132, UI_DIM, b);
}

static bool dex_detail(int* sel_io, bool can_edit) {
  bool dirty = false;
  int idx = *sel_io;
  bool full = true;
  if (g_n <= 0 || idx < 0 || idx >= g_n) return false;
  for (;;) {
    uint16_t in = g_list[idx];
    int st = dstate(in);
    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "POKEDEX");
      ui_hline(0, 147, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, can_edit ? "L/R entry  A cyc  B back" : "L/R entry  B back");
      dex_detail_art(in, st);
    }
    dex_detail_text(in, st, idx);
    full = false;
    u16 k = s_wait(KEY_A | KEY_B | KEY_L | KEY_R);
    if (k & KEY_B) break;
    else if (k & KEY_L) { idx = dex_detail_step(idx, g_n, -1); full = true; }
    else if (k & KEY_R) { idx = dex_detail_step(idx, g_n, +1); full = true; }
    else if (k & KEY_A) {
      if (can_edit) {
        dex_dset((int)pk_national_no(in), dex_detail_cycle(st));
        dirty = true;
        full = true;    /* art appears/disappears with state 0 */
      } else snd_deny();
    }
  }
  *sel_io = idx;
  return dirty;
}

bool pdna_dex_screen(DexGetState get, DexSetState set,
                     DexGetNat getnat, DexSetNat setnat, bool can_edit) {
  s_dex.get = get; s_dex.set = set;
  s_dex.getnat = getnat; s_dex.setnat = can_edit ? setnat : NULL;   /* read-only carts can't toggle it */
  s_dex_snap_valid = false;        /* fresh session: no bulk op to undo yet */
  /* Rent 32 more icon rows for the WHOLE life of this screen -- the one screen in the
   * app that can hold the borrow that long, and the reason it matters is the SCROLL.
   * A 21-cell page fits Tier B, so a one-step scroll keeps 20 of its 21 rows resident
   * and re-reads one instead of the whole page (host-measured: 1 transfer / 2 sectors,
   * against 21 / 42). Releasing per keypress the way the party screens must would throw
   * that away on every d-pad press.
   *
   * WHY THIS SCREEN MAY HOLD AND THEY MAY NOT: the borrow is g_pc, and the rule is that
   * a holder must not reach anything that touches the PC. Everything this screen opens
   * -- dex_menu, dex_bulk, osk_search, app_confirm -- reads and writes only the dex
   * flags in g_sb1/g_sb2, and its caller (pdna_dex_edit, pdna_main.c) commits through
   * app_commit_sb12, which by its own comment does NOT touch PC storage. The commit
   * also happens AFTER this function returns, i.e. after the release below.
   *
   * A refusal (unsaved box moves) is not an error: the pool stays Tier A, a page is not
   * resident, mon_icon_anim_cheap() answers no and the cells keep a static frame. The
   * store logs the reason.
   *
   * ARTLESS ONLY, for dex_declare_page's reason: with mon_icons.c linked the store is
   * never consulted here at all, so renting 33 KB of the user's PC storage would buy
   * nothing and risk something. */
#if !PDNA_MON_ICONS_ART_COMPILED
  icon_store_borrow(true);
#endif
  perf_span_begin("dex");            /* enter cost: up to 21 cells through the ladder */
  bool perf_first_paint = true;
  /* Art-free build: no icons -> the LIST is the primary view (Guy's call: grids of
   * icons become lists until the art exists; L/R still reaches the chip grid). */
  int filter = 0, sort = 0, status = DS_ALL, view = mon_icon_for(1) ? DV_GRID : DV_LIST;
  char search[16] = "";
  dex_build(filter, sort, search, status, view);
  int seen, caught; dex_counts(&seen, &caught);
  bool dirty = false;

  int sel = 0, toprow = 0;
  int prev_sel = -1, prev_top = -1, prev_view = -1;
  bool relist = true;
  /* dex_menu's own comment (above) explains why IT needs gen/valid: dex_bulk's
   * Cancel/Undo-last paths can return without a ui_clear(), which `relist` alone
   * would miss. That leaves a DIFFERENT trap for the caller here: dex_menu itself
   * ALWAYS ui_clear()s on its own first frame (its own `!valid` forces that), so
   * ui_clear_gen() DOES advance on every dex_menu() call -- but when it returns 0
   * (cancelled, r<1 below), `relist` is left false and view/top are unchanged, so
   * the old `full` predicate never saw it: dex_menu's own filter-panel pixels were
   * left sitting over the dex grid after Cancel. Same fix as dex_menu uses on
   * itself for the same reason -- gen/valid alongside relist. */
  uint32_t gen = 0; bool valid = false;
  int bob = 0, anim_ctr = 0, bob_next = DEX_BOB_DONE;

  for (;;) {
    int cols, cw, ch, x0, y0, vrows;
    dex_geom(view, &cols, &cw, &ch, &x0, &y0, &vrows);
    int vis = cols * vrows;
    if (sel >= g_n) sel = g_n ? g_n - 1 : 0;
    int srow = sel / cols;                              /* edge scroll */
    if (srow < toprow) toprow = srow;
    if (srow >= toprow + vrows) toprow = srow - vrows + 1;
    if (toprow < 0) toprow = 0;
    int top = toprow * cols;
    bool grid = (view != DV_LIST);

    bool full = relist || !valid || gen != ui_clear_gen() || view != prev_view || top != prev_top;
    relist = false;

    if (full) {
      bob_next = DEX_BOB_DONE;   /* the page draw below paints every cell at the current `bob`: an unfinished flip pass is moot */
      /* One page draw: a fresh entry, a view/filter change, a one-row scroll step
       * (which repaints all `vis` cells, including the ones that did not move), or an
       * overlay that painted over us -- dex_menu() always ui_clear()s on its own first
       * frame regardless of what the user does inside it, including a plain B-cancel
       * (r==0 below), which sets neither `relist` nor moves view/top; only the
       * gen != ui_clear_gen() term catches that path. Rolled up rather than logged per
       * occurrence -- a held D-pad produces one of these every few frames. */
      perf_rep_begin(PERF_REP_PAGE, "dex.page");
      dex_declare_page(grid, top, vis);
      ui_clear();
      ui_hline(0, 22, UI_SCR_W, UI_BORDER);
      ui_hline(0, 147, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A open  L/R view  ST  SEL  B");
      for (int i = 0; i < vis && top + i < g_n; i++) {
        int x = x0 + (i % cols) * cw, y = y0 + (i / cols) * ch;
        if (grid) dex_cell_grid(x, y, g_list[top + i], bob);
        else dex_cell_list(x, y, g_list[top + i], (top + i == sel));
      }
      perf_rep_end(PERF_REP_PAGE);
    } else if (prev_sel >= top && prev_sel < top + vis) {   /* erase old selection chrome */
      int pi = prev_sel - top, px = x0 + (pi % cols) * cw, py = y0 + (pi / cols) * ch;
      if (grid) m3_frame(px - 1, py - 1, px + 32, py + 32, UI_BG);
      else { ui_fill_rect(px, py - 1, cw, ch, UI_BG); dex_cell_list(px, py, g_list[prev_sel], false); }
    }

    if (g_n) {                                          /* draw current selection chrome */
      int si = sel - top, sx = x0 + (si % cols) * cw, sy = y0 + (si / cols) * ch;
      if (grid) m3_frame(sx - 1, sy - 1, sx + 32, sy + 32, UI_SELTEXT);
      else { ui_fill_rect(sx, sy - 1, cw, ch, UI_SEL); dex_cell_list(sx, sy, g_list[sel], true); }
    }

    dex_header(view, filter, status, g_n ? g_list[sel] : 0, seen, caught);

    prev_sel = sel; prev_top = top; prev_view = view;
    valid = true; gen = ui_clear_gen();          /* read AFTER the ui_clear() above, not before */
    if (perf_first_paint) { perf_first_paint = false; perf_span_end(); }

    /* wait for input; meanwhile bob the caught cells (grid/type, anim enabled) */
    u16 k, fresh;
    const u16 dpad = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT;
    do {
      s_vsync();
      fresh = key_hit(KEY_FULL);
      k = fresh | key_repeat(dpad);
      /* Animate ONLY on idle frames (no key pending) so a press/repeat is never delayed
       * by the multi-cell repaint — this kills the occasional cursor "stick". Recompose
       * EVERY visible caught cell at once (shared `bob`) via compose-then-CPU-copy
       * (ui_blit_over, memcpy32 — see that function's own comment for why: a per-vblank
       * animation tick, not DMA), no erase,
       * so there's never a frame where a sprite is blanked. */
      /* mon_icon_anim_cheap() is now "is every row of the page this screen DECLARED
       * already in RAM" (art_fallbacks.c), not "which rung is serving" -- the old
       * question gated off the CHEAPER rung and waved the more expensive one through,
       * and is why Guy's Pokedex sat still. dex_declare_page() above declares the 21
       * rows and pdna_dex_screen rents the space to hold them (icon_store_borrow), so
       * on a normal page this answers YES on both rungs and the flip is provably zero
       * SD transactions -- against the 252-420 disk_read calls it used to cost every
       * 30 frames. When it answers no (an unsaved PC refuses the borrow, so 21 rows do
       * not fit) caught cells keep their first frame: a static Pokemon beats a hole and
       * costs nothing per tick, and the log says which case it was.
       *
       * BACKLOG #124: the per-cell dex_cell_art_page_served() guard below --
       * deliberately called at its ONE use site inside this loop, not hoisted to a
       * named local at function entry: a persistent local here (even read-only, even
       * a plain bool) measurably perturbed -O2's register allocation across an
       * unrelated pre-existing dispatch elsewhere in this function on the artless
       * build only (this file's own git history has the churn that cost). The
       * function itself is a single EWRAM struct read (no SD/no I/O), called at most
       * `vis` times (<=21) once per 30-frame tick -- negligible either way. Not
       * added to THIS outer condition either, for the same register-pressure reason.
       *
       * A page the override serves has no icon-store rows declared for it at all
       * (dex_declare_page() skips the plan for the SAME `serves_page` answer, see its
       * own comment), so mon_icon_for_frame() below would either serve the WRONG
       * rung's picture or nothing for a GB-served cell; worse, re-driving the
       * override here would put a GB ROM SD fetch on this per-tick path, which
       * pdna_box.c's era_cell_draw() comment names as the one thing this class of
       * fetch must never do. The per-cell `continue` below leaves those cells exactly
       * as the full repaint drew them -- "static beats a hole" -- while every OTHER
       * (non-GB-served) caught cell on the SAME page still bobs normally, INCLUDING
       * every caught cell on a page `serves_page` is false for (a Gen-1 session, or
       * no ROM registered right now) even if some unrelated era's ROM happens to be
       * registered -- review A5's whole point. `anim_ctr`/`bob` themselves keep
       * ticking either way (harmless -- no I/O, just a counter and a page with zero
       * animating cells doing one extra no-op pass). */
      if (!k && grid && app_anim_enabled(ANIM_DEX) && mon_icon_anim_cheap() &&
          (bob_next < vis || ++anim_ctr >= DEX_ANIM_PERIOD)) {
        if (bob_next >= vis) { anim_ctr = 0; bob ^= 1; bob_next = 0; }   /* start a flip pass */
        int bob_end = bob_next + DEX_BOB_SLICE;      /* BACKLOG #296: one SLICE of the pass per idle iteration */
        if (bob_end > vis) bob_end = vis;
        /* The tick the user feels most: every visible CAUGHT cell is re-fetched through
         * the artless ladder on one 30-frame timer, so the cost scales with how many
         * Pokemon are moving. Rolled up; emitted when the screen is left. */
        perf_rep_begin(PERF_REP_BOB, "bob.dex");
        for (int i = bob_next; i < bob_end && top + i < g_n; i++) {
          uint16_t in = g_list[top + i];
          if (dstate(in) != 2) continue;
          if (dex_cell_art_page_served()) continue;  /* BACKLOG #124: only a page the override can actually serve skips bob */
          int x = x0 + (i % cols) * cw, y = y0 + (i / cols) * ch;
          { const uint16_t* ic = mon_icon_for_frame(in, (uint8_t)bob);
            if (ic) ui_blit_over(x, y, 32, 32, ic, UI_BG); }   /* art-free: static cell stays */
          ui_pokeball(x + 21, y + 21);
          if (top + i == sel) m3_frame(x - 1, y - 1, x + 32, y + 32, UI_SELTEXT);
        }
        perf_rep_end(PERF_REP_BOB);
        bob_next = (bob_end >= vis || top + bob_end >= g_n) ? DEX_BOB_DONE : bob_end;
      }
    } while (!k);
    if      (fresh & dpad)                          snd_move();
    else if (fresh & (KEY_L | KEY_R | KEY_SELECT))  snd_tab();
    else if (fresh & (KEY_A | KEY_START))           snd_ok();
    else if (fresh & KEY_B)                         snd_back();

    if (k & KEY_B) break;
    else if (k & KEY_A) {
      if (g_n) {                                       /* BACKLOG #203: A opens the detail view for everyone; the seen/caught cycle lives inside it */
        if (dex_detail(&sel, can_edit)) {
          dirty = true; dex_counts(&seen, &caught);
          if (status != DS_ALL) {                      /* a changed entry may drop out of the filtered list */
            uint16_t cur = g_list[sel];                /* the entry the user ended on (list not rebuilt yet) */
            dex_build(filter, sort, search, status, view);
            for (int i = 0; i < g_n; i++) if (g_list[i] == cur) { sel = i; break; }   /* dropped out: sel stays, clamped at the loop head */
          }
        }
        relist = true;                                 /* the detail view cleared the whole screen */
      }
    }
    else if (k & KEY_UP)    { if (cols == 1) sel = (sel > 0) ? sel - 1 : (g_n ? g_n - 1 : 0);          /* top -> wrap to last */
                              else if (sel >= cols) sel -= cols; else sel = g_n ? g_n - 1 : 0; }
    else if (k & KEY_DOWN)  { if (cols == 1) sel = (g_n && sel < g_n - 1) ? sel + 1 : 0;               /* bottom -> wrap to first */
                              else if (sel + cols < g_n) sel += cols;
                              else if (g_n && sel < ((g_n - 1) / cols) * cols) sel = g_n - 1;          /* partial last row: land on its last cell */
                              else sel = 0; }                                                          /* last row -> wrap to first */
    else if (k & KEY_LEFT)  { if (cols == 1) sel = clampi(sel - vrows, 0, g_n ? g_n - 1 : 0); else if (sel > 0) sel--; }
    else if (k & KEY_RIGHT) { if (cols == 1) sel = clampi(sel + vrows, 0, g_n ? g_n - 1 : 0); else if (sel < g_n - 1) sel++; }
    else if (k & KEY_L) { view = (view + DV_N - 1) % DV_N; dex_build(filter, sort, search, status, view); sel = 0; toprow = 0; relist = true; }
    else if (k & KEY_R) { view = (view + 1) % DV_N;       dex_build(filter, sort, search, status, view); sel = 0; toprow = 0; relist = true; }
    else if (k & KEY_START) {
      int r = dex_menu(&filter, &sort, &status, can_edit);
      if (r >= 1) { dex_build(filter, sort, search, status, view); sel = 0; toprow = 0; relist = true;
                    if (r == 2) { dirty = true; dex_counts(&seen, &caught); } }
    }
    else if (k & KEY_SELECT) {
      char q[16];
      if (osk_search("SEARCH", search, q, sizeof(q))) { strcpy(search, q); dex_build(filter, sort, search, status, view); sel = 0; toprow = 0; relist = true; }
    }
  }
  /* Leaving the screen: emit both rollups now, while the numbers still belong to a
   * screen the user can name, instead of waiting for whatever reuses the slots next. */
  perf_rep_flush(PERF_REP_PAGE);
  perf_rep_flush(PERF_REP_BOB);
  perf_span_end();          /* no-op if the first paint already closed it */
  /* Retire the declaration, never the CONTENTS. A plan is a claim about what the screen
   * on the glass is about to draw, and this screen is gone -- leaving it live would let
   * the next screen's unrelated fetch answer icon_store_plan_resident() with a stale
   * yes, and that answer is the animation gate. The rows themselves stay resident, so
   * coming straight back here costs nothing. */
  icon_store_plan(0, 0);
  /* Give g_pc back on the ONE exit this screen has. icon_store_borrow(false) retires the
   * plan as well, so the line above is belt-and-braces -- kept because the plan must be
   * retired even in a build where the borrow was refused. */
  icon_store_borrow(false);
  return dirty;
}

/* ===================== move picker ===================================== */

/* The move list shares g_idx with the item/ball/met-location/generic pickers -- see
 * g_idx's own comment for the exclusivity proof. `g_mv` is a name for that storage,
 * not storage of its own. */
#define g_mv g_idx
static int g_mvn;

/* BACKLOG #204: the five GB-picker scalars (this move ceiling plus the four item
 * ones declared further down -- g_item_max_id/g_item_gen/g_item_game/
 * g_item_open_pocket, pick_item_set_gen1_2()'s own header comment) packed into one
 * file-static struct instead of five separate file-scope statics, so the stack
 * walker's struct-field classifier has one stable section anchor for this whole
 * family (each new bare static used to shift the others' individual anchors --
 * b195 added two "coincidental spilled section-anchor" lines for exactly this).
 * uint16_t for the two ceilings (max_id values run past 255 -- items go to ~0xFF
 * but moves/species do not), uint8_t for the rest (gen/game/pocket_plus_one -- see
 * the _Static_assert pinned at their point of use for the range proof). g_item_gen
 * stays a plain small int (0 / GBIN_GEN1 / GBIN_GEN2, gb_item_names.h #defines),
 * g_item_game/g_item_open_pocket keep their enum semantics through the packed
 * uint8_t -- C's implicit int<->enum conversion makes this transparent at every
 * existing call site. Field order matches declaration order below for a stable,
 * predictable layout. */
static struct {
  uint16_t move_max_id;    /* BACKLOG #189: 0 = unrestricted, every existing caller's default */
  uint16_t item_max_id;    /* BACKLOG #189/#195: 0 = unrestricted (raw "#n" held-item mode) */
  uint8_t  item_gen;       /* 0 / GBIN_GEN1 / GBIN_GEN2 */
  uint8_t  item_game;      /* GbGame: GBF_G_RED/YELLOW/GS/CRYSTAL */
  uint8_t  item_open_pocket; /* GbBagPocket, one-shot; sentinel GBB_POCKET_COUNT */
  uint8_t  item_allow_none;  /* #340a: held-item mode keeps a "NO ITEM" row (id 0) so the picker can still clear */
} s_gb_pick = { 0, 0, 0, GBF_G_RED, GBB_POCKET_COUNT, 0 };
/* Range proof for the two enum-into-uint8_t fields above (BACKLOG #204). */
_Static_assert(GBB_POCKET_COUNT < 256, "GbBagPocket must fit item_open_pocket's uint8_t");
_Static_assert(GBF_G_COUNT < 256, "GbGame must fit item_game's uint8_t");
#define g_move_max_id       s_gb_pick.move_max_id
void pick_move_set_gen_max(uint16_t max_id) { g_move_max_id = max_id; }

/* sort modes for the move list */
#define NMVSORT 6
static const char* const MV_SORT[NMVSORT] = { "No.", "Name", "Power", "Acc", "PP", "Type" };

/* true if move a should be ordered AFTER move b under the given sort. Numeric keys
 * sort high-first (descending) since "strongest move first" is what you want; Name
 * and Type sort ascending. Insertion sort is stable, so ties keep ascending id. */
static bool move_gt(uint16_t a, uint16_t b, int sort) {
  switch (sort) {
    case 1: return strcmp(pk_move_name(a), pk_move_name(b)) > 0;   /* A-Z */
    case 2: return pk_move_power(a)    < pk_move_power(b);          /* power, high first */
    case 3: return pk_move_accuracy(a) < pk_move_accuracy(b);      /* acc,   high first */
    case 4: return pk_move_pp(a)       < pk_move_pp(b);            /* PP,    high first */
    case 5: return pk_move_type(a)     > pk_move_type(b);          /* group by type */
    default: return a > b;                                         /* by No. (id) */
  }
}

static void build_moves(int type_filter, int sort, const char* search) {
  g_mvn = 0;
  for (uint16_t m = 1; m < NMOVE; m++) {
    if (g_move_max_id && m > g_move_max_id) continue;   /* BACKLOG #189: the gen ceiling */
    const char* nm = pk_move_name(m);
    if (nm[0] == '-' || nm[0] == '?') continue;
    if (type_filter >= 0 && pk_move_type(m) != type_filter) continue;
    if (search[0] && !ci_contains(nm, search)) continue;
    g_mv[g_mvn++] = m;
  }
  for (int i = 1; i < g_mvn; i++) {
    uint16_t v = g_mv[i];
    int j = i - 1;
    while (j >= 0 && move_gt(g_mv[j], v, sort)) { g_mv[j + 1] = g_mv[j]; j--; }
    g_mv[j + 1] = v;
  }
}

static void text_wrap(int x, int y, int cols, u16 ink, const char* s) {
  char line[40];
  int n = (int)strlen(s), i = 0;
  while (i < n) {
    if (y > 144) return;                 /* never write past the panel into the footer */
    int take = (n - i > cols) ? cols : (n - i);
    if (n - i > cols) { int b = take; while (b > 0 && s[i + b] != ' ') b--; if (b > 0) take = b; }
    int j = 0; for (; j < take; j++) line[j] = s[i + j];
    line[j] = 0;
    ui_text(x, y, ink, line);
    y += 9; i += take; while (i < n && s[i] == ' ') i++;
  }
}

/* One list row at screen slot `i` (list index `idx`). type_chip() is the plain
 * text-abbreviation chip (local TYPE_ABBR/type_color tables) -- no SD here, unlike the
 * detail panel's type_icon(). The row's own rect is [y-1, y+8): 9 px, exactly the height
 * of its own selection panel one pixel above the 8 px glyph line, so wiping it (whether
 * selected or not) clears any leftover panel from a row that WAS selected without
 * touching a neighbor -- the next row's own rect starts at y+8, contiguous, not
 * overlapping. */
static void mv_row(int idx, int i, bool sel) {
  uint16_t m = g_mv[idx];
  /* BACKLOG #36 item 7: y/wipe geometry used to be a hardcoded `14 + i*9` / `(2, y-1,
   * 236, 9)` literal pair that happened to equal FILT_Y0/FILT_ROW_H/PDNA_FILT_BAR_* --
   * type_chip()'s own header comment already names mv_row as one of the three callers
   * backing its rows with that bar rect (fm_row, dxm_row, mv_row), so a future
   * PDNA_FILT_BAR_H/DY edit silently drifting this one out of step would reopen exactly
   * the bug type_chip's comment describes (chip painting outside the wipe rect,
   * invariant (1) of the ROW REPAINT RULE above) -- caught only by eye, not the build.
   * Pinned to the same macros fm_row/dxm_row use (see pdna_layout.h's PDNA_MV_VIS/
   * PDNA_MV_DETAIL_Y for the window-height half of this same tie). The selection paint
   * stays ui_panel (bordered), not filt_bar (borderless) -- that's mv_row's own visual
   * choice, unchanged here. */
  int y = FILT_Y0 + i * FILT_ROW_H;
  ui_fill_rect(PDNA_FILT_BAR_X, y + PDNA_FILT_BAR_DY, PDNA_FILT_BAR_W, PDNA_FILT_BAR_H, UI_BG);
  if (sel) ui_panel(PDNA_FILT_BAR_X, y + PDNA_FILT_BAR_DY, PDNA_FILT_BAR_W, PDNA_FILT_BAR_H, UI_SEL, UI_TITLE);
  char nm[20]; ui_truncate(nm, pk_move_name(m), 14);   /* fixed-width column: PP/type align */
  ui_text(6, y, sel ? UI_SELTEXT : UI_TEXT, nm);
  type_chip(120, y, pk_move_type(m));
}

uint16_t pick_move(uint16_t current) {
  int tf = -1, sort = 0;
  char search[16] = "";
  build_moves(tf, sort, search);
  int sel = 0;
  for (int i = 0; i < g_mvn; i++) if (g_mv[i] == current) { sel = i; break; }

  int top = 0;
  int prev_top = -1, prev_sel = -1;
  uint16_t prev_mid = 0xFFFF;          /* sentinel: NMOVE=355, no real id reaches it */
  uint32_t gen = 0;
  bool relist = false, valid = false;
  for (;;) {
    if (sel >= g_mvn) sel = g_mvn ? g_mvn - 1 : 0;
    if (sel < top) top = sel;
    if (sel >= top + PDNA_MV_VIS) top = sel - (PDNA_MV_VIS - 1);

    /* full: first paint, a picker/OSK overlay wiped us, the filter/sort/search list was
     * rebuilt, or the visible window scrolled -- every row's text is then genuinely new.
     * Otherwise a pure cursor move only touches the two affected rows (the row pair). */
    bool full = relist || !valid || gen != ui_clear_gen() || top != prev_top;
    relist = false;

    if (full) {
      ui_clear();
      char h[48];
      /* BACKLOG #189: when a ceiling is set, replace the type-filter chip with the
       * ceiling itself ("MOVES 1-165") so the filtering that made a later-gen move
       * disappear is visible, not silent -- Guy's own stated preference ("at least
       * flag which minimum gen each attk is available", read as "just filter" per
       * the brief). g_move_max_id==0 (every Gen-3 caller) keeps the old header,
       * byte for byte -- pinned by a host test. */
      if (g_move_max_id)
        siprintf(h, "MOVES 1-%u %s %d", (unsigned)g_move_max_id, MV_SORT[sort], g_mvn);
      else
        siprintf(h, "MOVES [%.3s] %s %d", tf < 0 ? "All" : pk_type_name((uint8_t)tf),
                 MV_SORT[sort], g_mvn);
      ui_text(4, 1, UI_TITLE, h);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A pick  L/R type  SEL find");
      for (int i = 0; i < PDNA_MV_VIS && top + i < g_mvn; i++) mv_row(top + i, i, top + i == sel);
    } else if (sel != prev_sel) {
      if (prev_sel >= top && prev_sel < top + PDNA_MV_VIS) mv_row(prev_sel, prev_sel - top, false);
      if (sel      >= top && sel      < top + PDNA_MV_VIS) mv_row(sel,      sel - top,      true);
    }

    /* detail panel for the selected move: fixed-width stat columns so the real type
     * badge gets its own column top-right and never sits on the numbers. Both
     * type_icon() (app_type_badge -> rom_type_sheet_load) and app_move_desc() hit the SD
     * in a ROM-registered build with no cache of their own, so this repaints only when
     * the selected move's id actually changed -- an unmoved cursor or a filter/sort
     * change that leaves sel pointing at the same id costs neither. */
    uint16_t mid = g_mvn ? g_mv[sel] : (uint16_t)0xFFFF;
    if (full || mid != prev_mid) {
      ui_fill_rect(0, PDNA_MV_DETAIL_Y, UI_SCR_W, 58, UI_BG);
      if (g_mvn) {
        ui_panel(0, PDNA_MV_DETAIL_Y, UI_SCR_W, 58, UI_PANEL, UI_BORDER);
        type_icon(202, 95, pk_move_type(mid));
        char num[48];
        siprintf(num, "Pow %3u  Acc %3u  PP %2u",
                 (unsigned)pk_move_power(mid), (unsigned)pk_move_accuracy(mid), (unsigned)pk_move_pp(mid));
        ui_text(6, 96, UI_DIRCLR, num);
        text_wrap(6, 110, 28, UI_TEXT, app_move_desc(mid));
      }
      prev_mid = mid;
    }

    prev_sel = sel; prev_top = top; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT | KEY_START);
    if (k & KEY_B) return CANCEL;
    else if (k & KEY_A) return g_mvn ? g_mv[sel] : CANCEL;
    else if (k & KEY_UP)   sel = clampi(sel - 1, 0, g_mvn ? g_mvn - 1 : 0);
    else if (k & KEY_DOWN) sel = clampi(sel + 1, 0, g_mvn ? g_mvn - 1 : 0);
    else if (k & KEY_L) { do { tf = (tf <= -1) ? 17 : tf - 1; } while (tf == 9); build_moves(tf, sort, search); sel = 0; top = 0; relist = true; }
    else if (k & KEY_R) { do { tf = (tf >= 17) ? -1 : tf + 1; } while (tf == 9); build_moves(tf, sort, search); sel = 0; top = 0; relist = true; }
    else if (k & KEY_START) { sort = (sort + 1) % NMVSORT; build_moves(tf, sort, search); sel = 0; top = 0; relist = true; }
    else if (k & KEY_SELECT) { char q[16]; if (osk_search("SEARCH", search, q, sizeof(q))) { strcpy(search, q); build_moves(tf, sort, search); sel = 0; top = 0; relist = true; } }
  }
}

/* ===================== generic searchable list (item / nature) ========= */
/* BACKLOG #107: list_build's own filter+sort loop is now pr_build() (below, inside
 * pick_rows' own section) -- list_pick calls through pick_rows now, so nothing here
 * builds its own idx[] any more. */

/* One row of the generic list picker. NOTE ON THE HEIGHT: it is a literal 9 (not
 * `rowh - 1` = 7) on an 8 px pitch (rowh=8) -- one pixel taller than its own pitch,
 * so neighbouring rows' rects share the boundary scanline y+7 and invariant (2) of the
 * ROW REPAINT RULE applies: the caller paints the selected row LAST in BOTH the pair
 * diff and the full paint, so the selected row's whole 9-row panel survives either way.
 *
 * This geometry is NOT as clean as item_filter_menu's 12-on-11, and the difference is
 * worth knowing before reusing it: there the shared scanline is two rows below the glyph
 * box, here it IS glyph row 7 -- the row libtonc's sys8 inks for , ; g j p q y. So an
 * unselected row's wipe clips the descenders of the row above. Every name list_pick can
 * be handed today is upper-case, but this helper is generic on name_fn; a mixed-case one
 * wants the pitch widened to 9 (as the FILT_* lists did) or the panel dropped for a
 * borderless bar (sp_row's idiom) first.
 *
 * BACKLOG #107: the icon-column branch this used to have (rowh=26, an ITEM_ICON_W/H
 * sprite via a second function-pointer parameter) is REMOVED, not preserved as dead
 * capability -- it had exactly zero real callers anywhere in the tree (list_pick's
 * only caller, pick_nature, always passed icon_fn=0; grepped, not assumed) and,
 * post-extraction, keeping it would have meant a genuinely-unresolvable indirect
 * call site: lp_row_cb would dereference LpCtx.icon_fn through pick_rows' opaque
 * `ctx`, and the static stack-budget walker cannot prove a struct-field load is
 * always NULL the way GCC's own constant-propagation could when icon_fn was a
 * literal-0 argument at list_pick's one call site (pre-extraction, the whole `if
 * (icon_fn)` branch was compiled away; post-extraction it would have been a real,
 * never-taken `bl` the walker has no way to rule out). The contest donor picker's
 * OWN icon column (pdna_contest.c, mon_row) exercises pick_rows' PR_ROWH26 preset
 * for real instead, with a real, live species-icon call -- so the preset itself
 * stays fully justified; only list_pick's dead second consumer of it is gone. */
static void lp_row(const char* (*name_fn)(uint16_t), int id, int y, bool sel) {
  char row[40], rt[40];
  siprintf(row, "%3d %s", id, name_fn((uint16_t)id));
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y - 1, 236, 9, UI_BG);
  ui_truncate(rt, row, 28);
  ui_text(6, y, sel ? UI_SELTEXT : UI_TEXT, rt);
}

/* ===================== pick_rows: the generic searchable/sortable engine ===========
 * BACKLOG #107: list_pick's own loop, extracted so a second caller (pdna_contest.c's
 * donor picker) gets the SAME chrome -- dirty-row repaint via ui_clear_gen(), L/R
 * paging, SELECT -> osk_search when search_key != NULL, START -> A-Z resort when
 * PR_SORTABLE is set (search_key doubles as the sort key, exactly list_build's own
 * idiom above) -- without a second copy of the loop. list_pick (below) becomes a
 * thin caller over this; its rendering (lp_row) and geometry (rowh 26 with icons / 8
 * without) are UNCHANGED, so its one live caller's (pick_nature) screen is pixel-
 * identical to before the extraction.
 *
 * `row(i, y, sel, ctx)` draws underlying item `i` (0..n-1, the CALLER's own
 * numbering -- a filtered/sorted display position is translated back to this before
 * the callback is invoked, so `row` never has to know filtering happened) at screen
 * y; it owns its whole rect start to finish (the ROW REPAINT RULE above) and must
 * draw identically whether invoked from the full-paint branch or the row-pair diff.
 * `ctx` is opaque to this engine -- handed back to `row`/`search_key` unchanged, and
 * it exists precisely so callers hold their per-call state (which name/icon
 * functions, which save buffer, ...) WITHOUT a new file-static (BACKLOG #107's own
 * "no new statics" bar): pass the address of a local struct built for that one call.
 *
 * `search_key(i, ctx)` returns the string osk_search's live filter matches
 * (ci_contains) against and, when PR_SORTABLE is set, the A-Z sort key too -- NULL
 * disables BOTH search and sort (a caller wanting sort with no search has no use
 * case in this file today, so the two are not split further).
 *
 * Geometry is one of three fixed presets chosen by `opts`, not a 5th/6th parameter:
 * every screen in this file before this extraction already used one of exactly
 * three shapes. Default (neither bit set): rowh 8, vis 16, page 10 -- list_pick's
 * own non-icon numbers. PR_ROWH26: rowh 26, vis 5, page 5 -- list_pick's own icon
 * numbers (a 24x24 icon column fits the 25 px panel). PR_ROWH9: rowh
 * PDNA_FILT_ROW_H, vis PDNA_FILT_VIS, page = vis (a full-page jump) -- the FILT
 * geometry pdna_contest.c's donor picker uses for its plain (no-icon) rows.
 *
 * Returns the chosen underlying id (0..n-1), or -1 on B. (Not list_pick's own
 * CANCEL/0xFFFF: a plain `int` return has no natural sentinel shared by every
 * future caller's id space, so every caller here already narrows its own result --
 * list_pick casts back to uint16_t/CANCEL right below.)
 *
 * `idx` scratch is g_idx (shared -- see its header comment above for the
 * exclusivity proof: every user is a leaf screen that calls no OTHER g_idx user
 * while its own list is open. pdna_contest.c's donor picker joins that same set --
 * it opens no other g_idx-backed picker while pick_rows() is live).
 *
 * PR_SORTABLE/PR_ROWH9/PR_ROWH26 (the `opts` bits) are declared in pdna_pick.h --
 * this is a public entry point now, not a file-local helper. */

static int pr_build(u16* idx, int n, const char* (*search_key)(int, void*), void* ctx,
                    const char* search, int sort) {
  int m = 0;
  for (int i = 0; i < n; i++)
    if (!search[0] || !search_key || ci_contains(search_key(i, ctx), search)) idx[m++] = (u16)i;
  if (sort && search_key) {                          /* insertion sort by search_key() */
    for (int i = 1; i < m; i++) {
      u16 v = idx[i]; int j = i - 1;
      /* Copy v's key OUT to a stable local buffer before comparing -- search_key()
       * is allowed to return a pointer into a SHARED mutable buffer (pdna_contest.c's
       * mon_key does exactly this, via its ctx's keybuf), so calling it twice inside
       * one strcmp() -- once for idx[j], once for v -- would have both calls return
       * THE SAME address, and by the time strcmp reads them both operands hold
       * whichever call ran last: a buffer compared against itself, always 0. Caught
       * live in the b107-contest search+sort shot: the sort toggle visibly did
       * nothing (BACKLOG #107's own review attack list named "sort stability" --
       * this is the sharper failure a stability check alone would have missed). */
      char vkey[32];
      strncpy(vkey, search_key(v, ctx), sizeof vkey - 1); vkey[sizeof vkey - 1] = 0;
      while (j >= 0 && strcmp(search_key(idx[j], ctx), vkey) > 0) { idx[j + 1] = idx[j]; j--; }
      idx[j + 1] = v;
    }
  }
  return m;
}

int pick_rows(const char* title, int n, int current,
             void (*row)(int i, int y, bool sel, void* ctx), void* ctx,
             const char* (*search_key)(int i, void* ctx), int opts) {
  u16* idx = g_idx;
  bool searchable = search_key != 0;
  bool sortable   = ((opts & PR_SORTABLE) != 0) && searchable;
  char search[16] = "";
  int sort = 0;
  int m = pr_build(idx, n, search_key, ctx, search, sort);
  int sel = 0;
  for (int i = 0; i < m; i++) if (idx[i] == current) { sel = i; break; }

  int rowh = 8, vis = 16, page = 10;
  if (opts & PR_ROWH26)      { rowh = 26; vis = 5; page = 5; }
  else if (opts & PR_ROWH9)  { rowh = PDNA_FILT_ROW_H; vis = PDNA_FILT_VIS; page = PDNA_FILT_VIS; }

  int top = 0;
  int prev_top = -1, prev_sel = -1;
  bool relist = false;
  uint32_t gen = 0; bool valid = false;

  for (;;) {
    if (sel >= m) sel = m ? m - 1 : 0;
    if (sel < top) top = sel;                       /* edge scroll: cursor roams, list moves only at edges */
    if (sel >= top + vis) top = sel - vis + 1;
    if (top < 0) top = 0;

    /* full: first paint, an overlay (the OSK, if searchable) wiped us, the search/sort
     * list was rebuilt, or the visible window scrolled. Otherwise a cursor move touches
     * just the two affected rows -- row()/search_key() are pure id -> content lookups,
     * so an unmoved id's row never goes stale. */
    bool full = relist || !valid || gen != ui_clear_gen() || top != prev_top;
    relist = false;

    if (full) {
      ui_clear();
      char h[40]; siprintf(h, "%s  %s  %d", title, sortable ? (sort ? "A-Z" : "No.") : "", m);
      ui_text(4, 2, UI_TITLE, h);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < vis && top + i < m; i++)          /* selected row LAST: rule (2) */
        if (top + i != sel) row(idx[top + i], 14 + i * rowh, false, ctx);
      if (m && sel >= top && sel < top + vis)
        row(idx[sel], 14 + (sel - top) * rowh, true, ctx);
      char foot[48];
      siprintf(foot, "A pick  L/R +-%d  %s%sB", page,
               searchable ? "SEL find  " : "", sortable ? "ST sort  " : "");
      ui_text(4, 152, UI_DIM, foot);
    } else if (sel != prev_sel) {
      if (prev_sel >= top && prev_sel < top + vis)
        row(idx[prev_sel], 14 + (prev_sel - top) * rowh, false, ctx);
      if (sel >= top && sel < top + vis)
        row(idx[sel], 14 + (sel - top) * rowh, true, ctx);
    }

    prev_sel = sel; prev_top = top; valid = true; gen = ui_clear_gen();

    u16 mask = KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B;
    if (searchable) mask |= KEY_SELECT;
    if (sortable)   mask |= KEY_START;
    u16 k = s_wait(mask);
    if (k & KEY_B) return -1;
    else if (k & KEY_A) return m ? idx[sel] : -1;
    else if (k & KEY_UP)   sel = clampi(sel - 1, 0, m ? m - 1 : 0);
    else if (k & KEY_DOWN) sel = clampi(sel + 1, 0, m ? m - 1 : 0);
    else if (k & KEY_L)    sel = clampi(sel - page, 0, m ? m - 1 : 0);
    else if (k & KEY_R)    sel = clampi(sel + page, 0, m ? m - 1 : 0);
    else if (sortable && (k & KEY_START)) { sort ^= 1; m = pr_build(idx, n, search_key, ctx, search, sort); sel = 0; relist = true; }
    else if (searchable && (k & KEY_SELECT)) {
      char q[16];
      if (osk_search("SEARCH", search, q, sizeof(q))) { strcpy(search, q); m = pr_build(idx, n, search_key, ctx, search, sort); sel = 0; relist = true; }
    }
  }
}

/* ---- list_pick: a thin caller over pick_rows (BACKLOG #107) ----------------------
 * `LpCtx` carries name_fn through pick_rows' opaque `ctx` instead of a file-static,
 * so this extraction adds zero new statics. Geometry and lp_row's own rendering are
 * byte-for-byte what they were before the extraction (plain 8 px rows; the icon
 * branch is gone -- see lp_row's own header comment on why). */
typedef struct { const char* (*name_fn)(uint16_t); } LpCtx;

static void lp_row_cb(int i, int y, bool sel, void* vctx) {
  LpCtx* c = (LpCtx*)vctx;
  lp_row(c->name_fn, i, y, sel);
}
static const char* lp_key_cb(int i, void* vctx) {
  LpCtx* c = (LpCtx*)vctx;
  return c->name_fn((uint16_t)i);
}

static uint16_t list_pick(const char* title, int count, const char* (*name_fn)(uint16_t),
                          int current, bool searchable, bool sortable) {
  LpCtx c = { name_fn };
  int opts = sortable ? PR_SORTABLE : 0;
  /* searchable/sortable are coupled through pick_rows' single search_key slot (its
   * own documented contract: search_key != NULL enables SELECT, unconditionally) --
   * a caller wanting sortable=true, searchable=false would ALSO get SELECT offered.
   * No such caller exists (grep: list_pick's only live caller, pick_nature, passes
   * both false), so this is not further split. */
  int r = pick_rows(title, count, current, lp_row_cb, &c,
                    (searchable || sortable) ? lp_key_cb : 0, opts);
  return r < 0 ? CANCEL : (uint16_t)r;
}

/* Gen-1/2 restriction (UX-parity audit, Guy 2026-09-07: "the item row -- Gen 3
 * uses pick_item with names; if the GB editor adjusts the item id numerically,
 * switch it to pick_item restricted to the gen's item ids, names '#n' where
 * the app has none"). The GB editor's held-item field (gb_editor.c's GBE_ITEM)
 * used to LEFT/RIGHT-step a raw byte 0..255 instead of opening a real picker.
 *
 * Unlike pick_species_set_max_dex()'s ceiling, this is not merely "hide ids
 * above a cutoff": Gen-1/2 items have NO numbering in common with Gen 3 AT
 * ALL (gen2_save.h's own held_item comment: "Gen-2 item id, no Gen-3
 * equivalent for many"), so a Gen-3 name/description/icon at the SAME raw
 * number would be actively WRONG, not merely irrelevant. Restricted mode
 * therefore also suppresses every Gen-3-specific thing that has no meaning
 * here: real names (item_label_for below draws "#n"), icons (gb_item_icon_or_none
 * returns NULL), descriptions (item_desc_for returns a placeholder), and the
 * category/per-game filters (Gen-3 pocket/availability metadata that says
 * nothing about a Gen-2 id -- the START filter menu and the L/R view-cycle,
 * which only ever shows an icon-bearing view, are both no-ops while
 * restricted; search still works, but by NUMBER, the species picker's own
 * digit-search idiom, since there is no name to search by).
 *
 * "#n" stands in until a future phase reads the real Gen-1/2 item names live
 * off the user's own cartridge (Guy's own words: "P2 will bring ROM names"),
 * the same way rom_gblearn.c now reads level-up learnsets -- this SCREEN does
 * not need to change again when that lands, only where item_label_for's text
 * comes from. 0 = unrestricted, every existing Gen-3 caller's default.
 *
 * BACKLOG #195: `g_item_gen` (0 / GBIN_GEN1 / GBIN_GEN2, gb_item_names.h)
 * upgrades restricted mode from raw "#n" rows to REAL names (gb_item_label)
 * plus a pocket CATEGORY filter (gbb_pocket_of) -- the ROM-read icon/
 * description upgrade the comment above describes is still a later phase;
 * this one is the NAME phase Guy's own ask ("there should be a way to add
 * items") needed. g_item_gen == 0 (the held-item fields' own use, unchanged)
 * keeps the exact old "#n"-only, numeric-search-only behaviour.
 *
 * Review D2: `g_item_game` is the ACTUAL GbGame (GBF_G_RED/YELLOW/GS/
 * CRYSTAL), not merely the generation -- gbb_pocket_of()'s answer for four
 * ids (0x46/0x73/0x74/0x81) depends on Gold/Silver vs Crystal specifically
 * (pokegold's own attributes.asm differs from pokecrystal's there, gb_bag.h's
 * own header comment on gbb_pocket_of has the derivation), so a "pick any
 * representative Gen-2 game" shortcut silently mis-filters Gold. Every
 * caller threads its own real game through now.
 *
 * BACKLOG #204: these four scalars are packed into s_gb_pick (declared near
 * g_move_max_id above, this file) -- see that struct's own comment. */
_Static_assert(GBIN_GEN2 < 256, "item_gen fits uint8_t");
#define g_item_max_id       s_gb_pick.item_max_id
#define g_item_gen          s_gb_pick.item_gen
#define g_item_game         s_gb_pick.item_game
#define g_item_open_pocket  s_gb_pick.item_open_pocket
#define g_item_allow_none   s_gb_pick.item_allow_none
void pick_item_set_gen1_2_max(uint16_t max_id) { pick_item_set_gen1_2(0, GBF_G_RED, max_id); }
void pick_item_set_gen1_2(int gen, GbGame game, uint16_t max_id) {
  g_item_gen = gen; g_item_game = game; g_item_max_id = max_id;
  g_item_allow_none = 0;                      /* every plain setter (and its (0,RED,0) clear) drops the held-item row */
}
/* #340a: real names AND the "NO ITEM" (id 0) row the old raw "#n" held-item picker had. */
void pick_item_set_gen1_2_held(int gen, GbGame game, uint16_t max_id) {
  pick_item_set_gen1_2(gen, game, max_id);
  g_item_allow_none = 1;
}
void pick_item_set_gen1_2_cat(GbBagPocket pocket0) { g_item_open_pocket = pocket0; }

static void item_label_for(uint16_t id, char* out, int cap) {
  if (g_item_max_id) {
    if (id == 0 && g_item_allow_none && g_item_gen) { siprintf(out, "NO ITEM"); return; }
    if (g_item_gen && gb_item_label(g_item_gen, (uint8_t)id, out, cap)) return;
    siprintf(out, "#%u", (unsigned)id);
    return;
  }
  pk_item_label(id, out, cap);
}
/* Named gb_item_icon_or_none, not item_icon_for -- item_icons.h already
 * declares a real, unrelated `item_icon_for()` (a different, lower-level
 * compiled-art lookup); this name was one collision away from shadowing it. */
static const uint16_t* gb_item_icon_or_none(uint16_t id) {
  return g_item_max_id ? NULL : app_item_icon(id);
}
static const char* item_desc_for(uint16_t id) {
  if (!g_item_max_id) return app_item_desc(id);
  /* #340b: Gen 2 reads the description off the user's own ROM; Gen 1 has none, and a
   * missing/unreadable ROM keeps the honest string. */
  if (g_item_gen == GBIN_GEN2 && id >= 1u && id <= 255u) {
    const char* d = gb_art_item_desc((uint8_t)id);
    if (d && !(d[0] == '?' && d[1] == 0)) return d;            /* an unused id's "?" text: honest string (review-zr D3) */
  }
  return PDNA_ITEM_NO_DESC_YET;
}

/* ---- item picker filters (BACKLOG #13, "like the pokedex"): two combinable
 * axes — CATEGORY (via pk_item_pocket) and GAME availability (via the
 * generated pk_item_games mask: bit0 RS, bit1 Emerald, bit2 FRLG). ---- */
#define NICAT PDNA_IFILT_NCAT   /* count lives in pdna_layout.h: the host test needs the row count */
static const char* const ICAT_NAME[NICAT] = { "All", "Items", "Key items", "Poke Balls", "TMs-HMs", "Berries" };
static const int8_t ICAT_POCKET[NICAT]    = { -1, POCKET_ITEMS, POCKET_KEY, POCKET_BALLS, POCKET_TMHM, POCKET_BERRIES };
#define NIGAME 4
static const char* const IGAME_NAME[NIGAME] = { "All", "RS", "Emerald", "FRLG" };  /* mask bit = 1<<(g-1) */

/* BACKLOG #195: restricted-mode (Gen 1/2) category filter, through
 * gbb_pocket_of() -- separate from ICAT_NAME/ICAT_POCKET above (Gen-3's own
 * pocket enum, gen3_items.h's PkPocket, means nothing for a Gen-1/2 id).
 * Gen 1 offers only ITEMS/TM-HM (its cartridge has no separate Key/Balls
 * pocket -- gb_bag.h's own table); Gen 2 offers all four real pockets.
 * Index 0 is always "All" (no gbb_pocket_of() call at all, not merely a
 * pocket value nothing matches -- GBB_POCKET_COUNT is itself a valid enum
 * value name, but it is never what an id maps to, so using it as the All
 * sentinel here is safe). */
#define NRICAT_G1 3
static const char* const RICAT_NAME_G1[NRICAT_G1] = { "All", "Items", "TM-HM" };
static const GbBagPocket RICAT_POCKET_G1[NRICAT_G1] = {
  GBB_POCKET_COUNT, GBB_POCKET_ITEMS, GBB_POCKET_TMHM,
};
#define NRICAT_G2 5
static const char* const RICAT_NAME_G2[NRICAT_G2] = { "All", "Items", "Poke Balls", "Key items", "TM-HM" };
static const GbBagPocket RICAT_POCKET_G2[NRICAT_G2] = {
  GBB_POCKET_COUNT, GBB_POCKET_ITEMS, GBB_POCKET_BALLS, GBB_POCKET_KEY, GBB_POCKET_TMHM,
};

/* filter by category + game + search, then optionally sort A-Z (same insertion
 * sort as list_build); returns the count. */
static int item_build(u16* idx, const char* search, int sort, int cat, int gamef) {
  int n = 0;
  for (int i = 0; i < 377; i++) {
    if (g_item_max_id) {
      /* Restricted (Gen 1/2): the ceiling is gbb_max_item_id()'s real-ITEM
       * bound (Items/Key/Balls/PC id space) -- Gen 2's TM/HM block (0xBF-
       * 0xF9) lives ABOVE that ceiling (its own separate count-array
       * numbering, gb_bag.h's own comment), so it needs an explicit second
       * admission test here or ADD ITEM could never reach it at all
       * (BACKLOG #195 F2's own "TM/HM pocket: ADD sets gbb_tmhm_set"
       * requirement -- gbb_tmhm_index_of() is the same admission test
       * gbb_pocket_of() itself uses for the TM/HM bucket, re-run instead of
       * re-derived so the two can never silently drift apart). */
      bool in_ceiling = ((uint16_t)i <= g_item_max_id);
      bool g2_tmhm = (!in_ceiling && g_item_gen == GBIN_GEN2 && i <= 0xF9 &&
                      gbb_tmhm_index_of(g_item_game, (uint8_t)i) >= 0);
      if (!in_ceiling && !g2_tmhm) continue;
      /* Review D2: an id that is not a real item AT ALL for this specific
       * game (id 0/0xFF always; on Gold/Silver, also the four ids
       * pokecrystal's own table has as Key items but pokegold's has as
       * unused ITEM placeholders -- gbb_pocket_of()'s own header comment)
       * never belongs in ANY category, "All" included -- this is also what
       * makes the stray "#0" (NO_ITEM) row disappear from the unfiltered
       * list. TM/HM ids are checked separately (g2_tmhm above already
       * proved admission; gbb_pocket_of() agrees for those, so this is not
       * a second, possibly-diverging test, just skipped to avoid a
       * redundant call). */
      if (g_item_gen && !g2_tmhm && !(i == 0 && g_item_allow_none) &&
          gbb_pocket_of(g_item_game, (uint8_t)i) == GBB_POCKET_COUNT) continue;
      /* #357a: the 25 unused ids inside the item range have a pocket but no real name
       * (gb_item_label fails): never offer them -- held AND bag/pack ADD pickers. */
      if (g_item_gen && !(i == 0 && g_item_allow_none)) {
        char nm0[GB_ITEM_NAME_MAXLEN + 8];
        if (!gb_item_label(g_item_gen, (uint8_t)i, nm0, sizeof nm0)) continue;
      }
      if (g_item_gen && cat) {
        GbBagPocket want = (g_item_gen == GBIN_GEN2) ? RICAT_POCKET_G2[cat] : RICAT_POCKET_G1[cat];
        if (gbb_pocket_of(g_item_game, (uint8_t)i) != want) continue;
      }
      if (search[0]) {
        /* Names exist only when g_item_gen is set -- search those too, on
         * top of the species-picker's own digit-search idiom (a plain
         * numeric search still works either way, and is the ONLY option
         * left when g_item_gen == 0, the held-item fields' raw "#n" mode). */
        bool hit = num_prefix((unsigned)i, search);
        if (!hit && g_item_gen) {
          char nm[GB_ITEM_NAME_MAXLEN + 8];
          item_label_for((uint16_t)i, nm, sizeof nm);
          hit = ci_contains(nm, search);
        }
        if (!hit) continue;
      }
    } else {
      if (cat && pk_item_pocket((uint16_t)i) != ICAT_POCKET[cat]) continue;
      if (gamef && !(pk_item_games((uint16_t)i) & (1 << (gamef - 1)))) continue;
      if (search[0] && !ci_contains(pk_item_name((uint16_t)i), search)) continue;
    }
    idx[n++] = (u16)i;
  }
  if (sort && !g_item_max_id) {          /* no names to sort by, either */
    for (int i = 1; i < n; i++) {
      u16 v = idx[i]; int j = i - 1;
      while (j >= 0 && strcmp(pk_item_name(idx[j]), pk_item_name(v)) > 0) { idx[j + 1] = idx[j]; j--; }
      idx[j + 1] = v;
    }
  }
  return n;
}

/* Row background for the IFILT_* geometry (item_filter_menu + loc_filter_menu share
 * it): 8 rows, so there is room for a BORDERED highlight here -- but it has to enclose
 * the glyph box rather than cut it: ui_panel's bottom rule sits at y+h-2, so height 12
 * from y-2 puts it at y+8, one pixel clear of the 8-row text. Pitch 11 keeps the next
 * row's ascenders out of the bar. (Both numbers in pdna_layout.h, asserted by
 * tests/host_textfit_test.c.)
 *
 * THE TRAP: that 12-px box on an 11-px pitch means two neighboring rows' rects share
 * one boundary scanline (row i's box spans y-2..y+9, row i+1's spans y+9..y+20). This
 * helper always wipes its own FULL nominal rect, never a clipped one, so the shared
 * scanline is just whichever of the two rows painted second -- which makes it invariant
 * (2) of the ROW REPAINT RULE, not a property any single call site can guarantee on its
 * own. "Old row before new row" in the pair diff is only half of it: the FULL paint has
 * to leave the selected row for last too, or the box is 11 rows tall the instant the
 * screen opens and 12 rows tall after one cursor move. Both call sites below do both.
 * The shared scanline sits at y+9, two rows clear of the y..y+7 glyph box, so unlike
 * the 9-on-8 geometry this one costs no text: the rule alone makes it exact. */
static void ifilt_row_bg(int y, bool sel) {
  if (sel) ui_panel(PDNA_FILT_BAR_X, y + PDNA_IFILT_BOX_DY, PDNA_FILT_BAR_W,
                    PDNA_IFILT_BOX_H, UI_SEL, UI_TITLE);
  else     ui_fill_rect(PDNA_FILT_BAR_X, y + PDNA_IFILT_BOX_DY, PDNA_FILT_BAR_W,
                        PDNA_IFILT_BOX_H, UI_BG);
}

/* One row of item_filter_menu: row 0 sort, row 1 game, rows 2.. category (pick+close). */
static void ifm_row(int r, int y, bool sel, int sort, int gamef, int cat) {
  ifilt_row_bg(y, sel);
  char b[36];
  if (r == 0)      { siprintf(b, PDNA_FILT_SORT_FMT, sort ? PDNA_FILT_SORT_NAME : PDNA_FILT_SORT_ID);
                     ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_DIRCLR, b); }
  else if (r == 1) { siprintf(b, "Game: %s", IGAME_NAME[gamef]);
                     ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_DIRCLR, b); }
  else             { siprintf(b, "%s%s", ICAT_NAME[r - 2], (cat == r - 2) ? "  <" : "");
                     ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_TEXT, b); }
}

/* START filter menu, the species picker's filter_menu idiom: Sort and Game
 * rows toggle in place (A / d-pad LEFT-RIGHT), a CATEGORY row picks + closes.
 *
 * No scrolling (8 rows, all always visible), so `full` needs only the first-paint /
 * overlay-wiped-us terms. A cursor move repaints the row pair; a sort/game toggle (sel
 * does not move for any of A/LEFT/RIGHT on rows 0-1) repaints just that row. */
static void item_filter_menu(int* cat, int* gamef, int* sort) {
  const int rows = PDNA_IFILT_ROWS;              /* Sort + Game + one per category */
  int sel = 0;
  int prev_sel = -1, prev_sort = -1, prev_gamef = -1;
  uint32_t gen = 0; bool valid = false;
  for (;;) {
    bool full = !valid || gen != ui_clear_gen();

    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "FILTER / SORT");
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      for (int r = 0; r < rows; r++)                        /* selected row LAST: rule (2) */
        if (r != sel) ifm_row(r, PDNA_IFILT_Y0 + r * PDNA_IFILT_ROW_H, false, *sort, *gamef, *cat);
      if (sel >= 0 && sel < rows)
        ifm_row(sel, PDNA_IFILT_Y0 + sel * PDNA_IFILT_ROW_H, true, *sort, *gamef, *cat);
      ui_text(4, PDNA_FILT_FOOTER_Y, UI_DIM, PDNA_IFILT_FOOT);
    } else if (sel != prev_sel) {
      if (prev_sel >= 0 && prev_sel < rows)                 /* old row first: rule (2) */
        ifm_row(prev_sel, PDNA_IFILT_Y0 + prev_sel * PDNA_IFILT_ROW_H, false, *sort, *gamef, *cat);
      ifm_row(sel,      PDNA_IFILT_Y0 + sel      * PDNA_IFILT_ROW_H, true,  *sort, *gamef, *cat);
    } else if (*sort != prev_sort || *gamef != prev_gamef) {
      ifm_row(sel, PDNA_IFILT_Y0 + sel * PDNA_IFILT_ROW_H, true, *sort, *gamef, *cat);
    }

    prev_sel = sel; prev_sort = *sort; prev_gamef = *gamef; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_A) {
      if (sel == 0)      *sort ^= 1;
      else if (sel == 1) *gamef = (*gamef + 1) % NIGAME;
      else { *cat = sel - 2; return; }             /* category picks + closes, like the species list */
    }
    else if (k & KEY_UP)    sel = (sel > 0) ? sel - 1 : rows - 1;
    else if (k & KEY_DOWN)  sel = (sel + 1) % rows;
    else if ((k & KEY_LEFT)  && sel == 1) *gamef = (*gamef + NIGAME - 1) % NIGAME;
    else if ((k & KEY_RIGHT) && sel == 1) *gamef = (*gamef + 1) % NIGAME;
  }
}

static void rcm_row(const char* const* names, int active, int r, int y, bool sel) {
  ifilt_row_bg(y, sel);
  char b[24];
  siprintf(b, "%s%s", names[r], (active == r) ? "  <" : "");
  ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_TEXT, b);
}

/* BACKLOG #195: the restricted (Gen 1/2) picker's OWN, much smaller filter
 * menu -- category only (no Sort/Game rows: neither has any meaning for a
 * Gen-1/2 id, item_build()'s own restricted branch never reads `sort`/
 * `gamef`). A dedicated function rather than teaching item_filter_menu() a
 * restricted mode, so the unrestricted (Gen-3, ceiling 0) picker's own
 * behaviour and this function's row geometry can never entangle -- the
 * brief's own pin ("The Gen-3 picker's behaviour with ceiling 0 stays
 * byte-identical") is trivially true here because this code path is
 * unreachable unless g_item_max_id is set. Reuses ifilt_row_bg (geometry-
 * only, no category-count assumption) and the same PDNA_IFILT_Y0/ROW_H/
 * FOOTER_Y layout constants item_filter_menu() already uses. */
static void ritem_cat_menu(int gen, int* cat) {
  int n = (gen == GBIN_GEN2) ? NRICAT_G2 : NRICAT_G1;
  const char* const* names = (gen == GBIN_GEN2) ? RICAT_NAME_G2 : RICAT_NAME_G1;
  int sel = (*cat >= 0 && *cat < n) ? *cat : 0;
  int prev_sel = -1;
  uint32_t gen_g = 0; bool valid = false;

  for (;;) {
    bool full = !valid || gen_g != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "CATEGORY");
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      for (int r = 0; r < n; r++) if (r != sel) rcm_row(names, *cat, r, PDNA_IFILT_Y0 + r * PDNA_IFILT_ROW_H, false);
      rcm_row(names, *cat, sel, PDNA_IFILT_Y0 + sel * PDNA_IFILT_ROW_H, true);   /* selected row LAST: rule (2) */
      ui_text(4, PDNA_FILT_FOOTER_Y, UI_DIM, "A pick  U/D  B cancel");
    } else if (sel != prev_sel) {
      rcm_row(names, *cat, prev_sel, PDNA_IFILT_Y0 + prev_sel * PDNA_IFILT_ROW_H, false);  /* old row first */
      rcm_row(names, *cat, sel,      PDNA_IFILT_Y0 + sel      * PDNA_IFILT_ROW_H, true);
    }
    prev_sel = sel; valid = true; gen_g = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_A) { *cat = sel; return; }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
  }
}

/* ---- item picker with view modes ---- */
#define IV_LIST 0
#define IV_ICONS 1
#define IV_GRID 2
#define IV_SPLIT 3
#define IV_N 4
static const char* const IV_NAME[IV_N] = { "list", "icons", "grid", "split" };
#define IITEM 24                                  /* item icon size */

/* geometry per view: columns, cell w/h, origin, visible rows */
static void iv_geom(int v, int* cols, int* cw, int* ch, int* x0, int* y0, int* vrows) {
  switch (v) {
    case IV_ICONS: *cols = 8; *cw = 29;  *ch = 27; *x0 = 6; *y0 = 22; *vrows = 4;  break;
    case IV_GRID:  *cols = 3; *cw = 78;  *ch = 40; *x0 = 6; *y0 = 22; *vrows = 3;  break;
    case IV_SPLIT: *cols = 1; *cw = 116; *ch = 26; *x0 = 2; *y0 = 22; *vrows = 4;  break;
    default:       *cols = 1; *cw = 232; *ch = 9;  *x0 = 4; *y0 = 14; *vrows = 13; break;
  }
}

/* draw one cell's content (icon at the cell origin, no selection chrome). */
static void iv_cell(int v, int x, int y, int id) {
  const uint16_t* ic = gb_item_icon_or_none((uint16_t)id);  /* compiled art -> the registered ROM; NULL if restricted */
  char lbl[48];
  item_label_for((uint16_t)id, lbl, sizeof lbl);     /* "No26 EARTHQUAKE" for a TM, or "#n" if restricted */
  if (v == IV_LIST) {
    char row[64];
    if (g_item_max_id) siprintf(row, "%s", lbl);     /* lbl is already "#n" -- no separate number column */
    else                siprintf(row, "%3d %s", id, lbl);
    ui_ptext_fit(x + 2, y, UI_SCR_W - (x + 4), UI_TEXT, row);
  } else if (v == IV_ICONS) {
    if (ic) ui_sprite(x, y, IITEM, IITEM, ic);
  } else if (v == IV_GRID) {
    if (ic) ui_sprite(x, y, IITEM, IITEM, ic);
    ui_ptext_fit(x, y + 25, IITEM + 6, UI_DIM, lbl);
  } else {                                          /* IV_SPLIT left row: icon + name */
    if (ic) ui_sprite(x, y, IITEM, IITEM, ic);
    ui_ptext_fit(x + 28, y + 8, 92, UI_TEXT, lbl);
  }
}

uint16_t pick_item(uint16_t current) {
  u16* idx = g_idx;
  char search[16] = "";
  /* Restricted (Gen 1/2): always list view -- every OTHER view shows an icon
   * that would be wrong (gb_item_icon_or_none()'s own comment), and there is no
   * point offering a mode this session can never leave (L/R is disabled
   * below while restricted). */
  int sort = 0, view = g_item_max_id ? IV_LIST : (app_item_icon(13) ? IV_SPLIT : IV_LIST), cat = 0, gamef = 0;
  /* BACKLOG #195: consume the one-shot opening-category primer (ADD ITEM's
   * own pre-filter to the pocket it was opened from) -- a caller that never
   * calls pick_item_set_gen1_2_cat() sees g_item_open_pocket still at its
   * GBB_POCKET_COUNT default, which maps to no row below (cat stays 0/All),
   * matching every OTHER pick_item() caller's behaviour exactly. */
  if (g_item_gen) {
    const GbBagPocket* map = (g_item_gen == GBIN_GEN2) ? RICAT_POCKET_G2 : RICAT_POCKET_G1;
    int ncat = (g_item_gen == GBIN_GEN2) ? NRICAT_G2 : NRICAT_G1;
    for (int i = 1; i < ncat; i++) if (map[i] == g_item_open_pocket) { cat = i; break; }
  }
  g_item_open_pocket = GBB_POCKET_COUNT;
  int n = item_build(idx, search, sort, cat, gamef);
  int sel = 0;
  for (int i = 0; i < n; i++) if (idx[i] == current) { sel = i; break; }
  /* #357c: held mode only -- a non-zero byte the list does not carry (an unused id) gets ONE
   * preselected row of its own, so A keeps the byte and B still cancels; without it the cursor
   * sat on NO ITEM and an accidental A cleared the item. */
  if (g_item_gen && g_item_allow_none && current != 0 && current <= 0xFFu && n < NITEM) {
    int at = 0;
    while (at < n && idx[at] < current) at++;
    if (at >= n || idx[at] != current) {
      for (int k = n; k > at; k--) idx[k] = idx[k - 1];
      idx[at] = current; n++; sel = at;
    }
  }

  int prev_sel = -1, prev_top = -1, prev_view = -1, toprow = 0;
  unsigned gen = 0; bool valid = false;   /* gen term: osk_search paints its whole
                                           * keyboard even on a cancelled SELECT, and a
                                           * cancel changes no other shadowed value --
                                           * same fix pdna_dex_screen just got */
  bool relist = true;

  for (;;) {
    int cols, cw, ch, x0, y0, vrows;
    iv_geom(view, &cols, &cw, &ch, &x0, &y0, &vrows);
    int vis = cols * vrows;
    if (sel >= n) sel = n ? n - 1 : 0;
    int srow = sel / cols;            /* edge scroll: cursor roams the page, list moves only at the edges */
    if (srow < toprow) toprow = srow;
    if (srow >= toprow + vrows) toprow = srow - vrows + 1;
    if (toprow < 0) toprow = 0;
    int top = toprow * cols;
    bool grid = (view == IV_ICONS || view == IV_GRID);

    bool full = relist || !valid || gen != ui_clear_gen() ||
                view != prev_view || top != prev_top;
    relist = false;

    if (full) {
      ui_clear();
      char h[64], ht[40];                          /* active filters live in the header */
      /* Restricted: no game filter or A-Z sort exists (item_build's own
       * comment). Gen-3 raw mode (g_item_gen == 0, the held-item fields'
       * own use) has no category either -- the bracket that would otherwise
       * show it ("[All] No.") is dropped rather than printed as dead-
       * looking noise, same as before BACKLOG #195. Gen 1/2 DOES have a
       * category now (gbb_pocket_of()), so its header shows it. */
      if (g_item_max_id) {
        if (g_item_gen) {
          const char* const* names = (g_item_gen == GBIN_GEN2) ? RICAT_NAME_G2 : RICAT_NAME_G1;
          siprintf(h, "ITEM [%s] %d", names[cat], n);
        } else siprintf(h, "ITEM %d", n);
      }
      else siprintf(h, "ITEM %s [%s|%s] %s %d", IV_NAME[view], ICAT_NAME[cat], IGAME_NAME[gamef],
               sort ? "A-Z" : "No.", n);
      ui_truncate(ht, h, 29);
      ui_text(4, 2, UI_TITLE, ht);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      ui_hline(0, 147, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, g_item_max_id ? (g_item_gen ? PDNA_ITEM_GB2_FOOT : PDNA_ITEM_GB_FOOT)
                                             : "A pick  L/R view  ST  SEL  B");
      if (view == IV_SPLIT) ui_panel(122, 20, 116, 124, UI_PANEL, UI_BORDER);
      for (int i = 0; i < vis && top + i < n; i++)
        iv_cell(view, x0 + (i % cols) * cw, y0 + (i / cols) * ch, idx[top + i]);
    } else if (prev_sel >= top && prev_sel < top + vis) {
      int pi = prev_sel - top, px = x0 + (pi % cols) * cw, py = y0 + (pi / cols) * ch;
      if (grid) m3_frame(px - 1, py - 1, px + IITEM, py + IITEM, UI_BG);   /* erase old frame */
      else { ui_fill_rect(px, py - 1, cw, ch, UI_BG); iv_cell(view, px, py, idx[prev_sel]); }
    }

    if (n) {                                        /* draw current selection chrome */
      int si = sel - top, sx = x0 + (si % cols) * cw, sy = y0 + (si / cols) * ch;
      if (grid) m3_frame(sx - 1, sy - 1, sx + IITEM, sy + IITEM, UI_SELTEXT);
      else { ui_fill_rect(sx, sy - 1, cw, ch, UI_SEL); iv_cell(view, sx, sy, idx[sel]); }
    }

    /* description: split -> right panel; other modes -> one line above the footer */
    uint16_t cur = n ? idx[sel] : 0;
    if (view == IV_SPLIT) {
      ui_fill_rect(124, 22, 112, 120, UI_PANEL);
      { char inm[48]; item_label_for(cur, inm, sizeof inm);
        ui_ptext_fit(126, 24, 108, UI_TITLE, inm); }
      ui_ptext_wrap(126, 36, 108, UI_ROW_H, 12, UI_TEXT, item_desc_for(cur));
    } else {
      ui_fill_rect(0, 138, UI_SCR_W, 8, UI_BG);
      char d[96];
      if (g_item_max_id) siprintf(d, "%s", item_desc_for(cur));    /* #340b: restricted (Game Boy) mode shows the text alone -- the highlighted row already names it, and "NAME  text" overran 232 px */
      else { char nm[48]; item_label_for(cur, nm, sizeof nm); siprintf(d, "%s  %s", nm, item_desc_for(cur)); }
      ui_ptext_fit(4, 139, UI_SCR_W - 8, UI_DIM, d);
    }

    prev_sel = sel; prev_top = top; prev_view = view;
    valid = true; gen = ui_clear_gen();   /* after any ui_clear this pass issued */

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT | KEY_START);
    if (k & KEY_B) return CANCEL;
    else if (k & KEY_A) return n ? idx[sel] : CANCEL;
    else if (k & KEY_UP)    sel = clampi(sel - cols, 0, n ? n - 1 : 0);
    else if (k & KEY_DOWN)  sel = clampi(sel + cols, 0, n ? n - 1 : 0);
    else if (k & KEY_LEFT)  { if (cols > 1) sel = clampi(sel - 1, 0, n ? n - 1 : 0); }
    else if (k & KEY_RIGHT) { if (cols > 1) sel = clampi(sel + 1, 0, n ? n - 1 : 0); }
    else if (k & KEY_L) { if (!g_item_max_id) { view = (view + IV_N - 1) % IV_N; relist = true; } }
    else if (k & KEY_R) { if (!g_item_max_id) { view = (view + 1) % IV_N; relist = true; } }
    else if (k & KEY_START) {                     /* filter menu, like the species picker.
                                                     * BACKLOG #195: restricted mode now has
                                                     * its OWN filter (category only, via
                                                     * ritem_cat_menu) when names exist
                                                     * (g_item_gen != 0); the raw "#n" mode
                                                     * (held-item fields) still has no filter
                                                     * at all -- no category to filter BY. */
      if (!g_item_max_id) {
        item_filter_menu(&cat, &gamef, &sort);
        n = item_build(idx, search, sort, cat, gamef); sel = 0; toprow = 0; relist = true;
      } else if (g_item_gen) {
        ritem_cat_menu(g_item_gen, &cat);
        n = item_build(idx, search, sort, cat, gamef); sel = 0; toprow = 0; relist = true;
      }
    }
    else if (k & KEY_SELECT) {
      char q[16];
      if (osk_search("SEARCH", search, q, sizeof(q))) { strcpy(search, q); n = item_build(idx, search, sort, cat, gamef); sel = 0; toprow = 0; relist = true; }
    }
  }
}
static const char* nature16(uint16_t n) { return pk_nature_name((uint8_t)n); }
uint8_t  pick_nature(uint8_t current)  { uint16_t r = list_pick("NATURE", 25, nature16, current, false, false); return r == CANCEL ? current : (uint8_t)r; }

/* One of the (at most 2) ability panels. `desc` is the CACHED string (see pick_ability's
 * own comment) -- never a fresh app_ability_desc() call, so this never touches the SD. */
static void ab_panel(int i, uint16_t aid, const char* desc, bool sel) {
  int y = 18 + i * 56;
  ui_panel(2, y - 2, 236, 52, sel ? UI_SEL : UI_PANEL, sel ? UI_TITLE : UI_BORDER);
  char h[24]; siprintf(h, "%d. %s", i + 1, pk_ability_name(aid));
  ui_text(8, y + 2, sel ? UI_SELTEXT : UI_TEXT, h);
  text_wrap(8, y + 14, 28, UI_DIM, desc);
}

uint8_t pick_ability(uint16_t species, uint8_t cur) {
  uint16_t a0 = pk_species_ability(species, 0), a1 = pk_species_ability(species, 1);
  int n = (a1 && a1 != a0) ? 2 : 1;            /* most species have 2 distinct abilities */
  int sel = (cur && n == 2) ? 1 : 0;
  /* a0/a1 are fixed for this whole screen (species/cur are call params, not state this
   * loop mutates), so the SD-backed descriptions are worth fetching exactly ONCE, not
   * once per keypress. app_ability_desc's own contract (pdna_app.h) says its returned
   * pointer is only valid until the NEXT call to that same function (one shared 128 B
   * static buffer) -- so both are copied out here before either call can clobber the
   * other. da1 is zero-initialized so an n==1 species (never read) is never
   * "uninitialized", not because it is ever drawn. */
  char da0[128], da1[128] = "";
  strcpy(da0, app_ability_desc(a0));
  if (n == 2) strcpy(da1, app_ability_desc(a1));

  int prev_sel = -1;
  uint32_t gen = 0;
  bool valid = false;
  for (;;) {
    bool full = !valid || gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "ABILITY");
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      ui_text(4, 140, UI_DIM, PDNA_HINT_ABILITY);   /* #348 */
      ui_text(4, 152, UI_DIM, "A pick  U/D move  B cancel");
      for (int i = 0; i < n; i++) ab_panel(i, i ? a1 : a0, i ? da1 : da0, i == sel);
    } else if (sel != prev_sel) {
      /* n is 1 or 2, so "the two affected rows" is simply every panel there is. */
      for (int i = 0; i < n; i++) ab_panel(i, i ? a1 : a0, i ? da1 : da0, i == sel);
    }
    prev_sel = sel; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return cur;
    else if (k & KEY_A) return (uint8_t)sel;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
  }
}

/* Choose an Unown letter (0..27 = A..Z ! ?) with THE SAME layout/chrome as the
 * dex-style species picker (7x3 grid of 32x32 icons, edge scroll, header strip) —
 * picking the species then its letter reads as one continuous flow (Guy). Returns
 * the chosen form, or -1 on cancel. */
int pick_unown_form(int cur) {
  static const char* const LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!?";
  const int N = 28;
  int sel = (cur >= 0 && cur < N) ? cur : 0;
  char hdr[48];
  int prev_sel = -1, prev_top = -1, toprow = 0;
  bool relist = true;
  for (;;) {
    int srow = sel / GCOLS;           /* edge scroll, exactly like pick_species */
    if (srow < toprow) toprow = srow;
    if (srow >= toprow + GVROWS) toprow = srow - GVROWS + 1;
    if (toprow < 0) toprow = 0;
    int top_idx = toprow * GCOLS;

    bool full = relist || top_idx != prev_top;
    relist = false;
    if (full) {
      ui_clear();
      ui_hline(0, 21, UI_SCR_W, UI_BORDER);
      ui_hline(0, 147, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A pick  B cancel");
      for (int i = 0; i < GCOLS * GVROWS; i++) {
        int idx = top_idx + i;
        if (idx >= N) break;
        int x = GX + (i % GCOLS) * GCELLX, y = GY + (i / GCOLS) * GCELLY;
        /* #344 audit: SAFE -- all three callers run right after pick_species(), whose entry and exit retire the plan and its pins. */
        { const uint16_t* ic = mon_icon_for_form(201, (uint8_t)idx);
          if (ic) ui_icon_scaled(x, y, GICON, GICON, ic);
          else { char l[2] = { "ABCDEFGHIJKLMNOPQRSTUVWXYZ!?"[idx], 0 };   /* art-free: the letter IS the icon */
                 ui_name_chip(x, y + 6, GICON, 18, UI_PANEL, UI_TEXT, l); } }
      }
    } else if (prev_sel >= top_idx && prev_sel < top_idx + GCOLS * GVROWS) {
      int pi = prev_sel - top_idx;                 /* erase the old selection frame */
      int px = GX + (pi % GCOLS) * GCELLX, py = GY + (pi / GCOLS) * GCELLY;
      m3_frame(px - 1, py - 1, px + GICON, py + GICON, UI_BG);
      if (pi < GCOLS) ui_hline(px - 1, 21, GICON + 2, UI_BORDER);   /* restore the rule the erase crossed (#338) */
    }
    { int si = sel - top_idx;                      /* current selection frame */
      int sx = GX + (si % GCOLS) * GCELLX, sy = GY + (si / GCOLS) * GCELLY;
      m3_frame(sx - 1, sy - 1, sx + GICON, sy + GICON, UI_SELTEXT); }

    /* header strip: mirror the species picker's band (Unown is mono-Psychic) */
    ui_fill_rect(0, 0, UI_SCR_W, 21, UI_BG);
    siprintf(hdr, "No.201  UNOWN %c", LET[sel]);
    ui_text(4, 2, UI_TITLE, hdr);
    type_icon(204, 4, pk_species_type1(201));
    siprintf(hdr, "[Unown letter]  %d", N);
    ui_text(4, 11, UI_DIM, hdr);

    prev_sel = sel; prev_top = top_idx;
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    else if (k & KEY_A) return sel;
    else if (k & KEY_LEFT)  sel = (sel > 0) ? sel - 1 : 0;
    else if (k & KEY_RIGHT) sel = (sel < N - 1) ? sel + 1 : sel;
    else if (k & KEY_UP)    { if (sel >= GCOLS) sel -= GCOLS; }
    else if (k & KEY_DOWN)  { if (sel + GCOLS < N) sel += GCOLS; }
  }
}

/* ===================== Poke Ball picker (was a 12-press toggle) =============
 * Gen 3 keeps the ball in FOUR BITS of the Misc origins word (bits 11-14), and the twelve
 * balls happen to be item ids 1..12 (pk_item_pocket: "1..12 Master..Premier Ball"), so
 * this list doubles as an item list: same pk_item_name, same item_icon_for icons, same
 * app_item_desc blurb the item picker shows. That identity is the reason the ball field
 * gets its own tiny picker rather than a call into pick_item — pick_item can return any
 * of 377 ids, and anything above 12 would not survive the 4-bit field.
 *
 * Art-free build: item_icon_for returns NULL for every id, so the icon column is simply
 * empty and the names carry the screen. */
#define NBALL 12

/* One ball row at screen slot `i`, showing item id `id`. app_item_icon()/app_item_desc()
 * both hit the SD (a ROM-registered build with no cache of its own -- same shape as
 * app_type_badge, pdna_app.h), so this is called only for rows whose content is actually
 * about to be redrawn -- either every visible row (the window just scrolled to a new
 * page, so every id shown really is new) or just the one or two rows a cursor move
 * actually touches (same id, only the selection tint changes). The wipe-then-draw order
 * is the same "clip wipe" idiom as every other row helper in this file: a shorter name
 * or description must not leave the tail of the previous (longer) one behind. */
static void ball_row(int id, int i, bool sel) {
  int y = PDNA_BALL_Y0 + i * PDNA_BALL_ROW_H;
  int tw = UI_SCR_W - PDNA_BALL_TEXT_X - 4;
  if (sel) ui_panel(2, y - 1, 236, PDNA_BALL_ROW_H - 1, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y - 1, 236, PDNA_BALL_ROW_H - 1, UI_BG);
  const uint16_t* ic = app_item_icon((uint16_t)id);
  if (ic) ui_sprite(PDNA_BALL_ICON_X, y, ITEM_ICON_W, ITEM_ICON_H, ic);
  ui_ptext_fit(PDNA_BALL_TEXT_X, y + PDNA_BALL_NAME_DY, tw,
               sel ? UI_SELTEXT : UI_TEXT, pk_item_name((uint16_t)id));
  ui_ptext_fit(PDNA_BALL_TEXT_X, y + PDNA_BALL_DESC_DY, tw,
               UI_DIM, app_item_desc((uint16_t)id));
}

uint8_t pick_ball(uint8_t current) {
  int sel = (current >= 1 && current <= NBALL) ? current - 1 : 3;   /* default: Poke Ball */
  int top = 0;
  int prev_top = -1, prev_sel = -1;
  uint32_t gen = 0;
  bool valid = false;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + PDNA_BALL_VIS) top = sel - PDNA_BALL_VIS + 1;

    /* full: first paint, an overlay wiped us, or the page scrolled -- every visible row
     * then shows a genuinely different ball. Otherwise a cursor move only touches the
     * row it left and the row it landed on (same page, same ids, only the tint moves). */
    bool full = !valid || gen != ui_clear_gen() || top != prev_top;

    if (full) {
      ui_clear();
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      ui_ptext(4, PDNA_BALL_NOTE_Y, UI_DIM, PDNA_BALL_NOTE);
      ui_hline(0, PDNA_BALL_RULE_Y, UI_SCR_W, UI_BORDER);
      ui_text(4, PDNA_FILT_FOOTER_Y, UI_DIM, PDNA_BALL_FOOT);
      for (int i = 0; i < PDNA_BALL_VIS && top + i < NBALL; i++)
        ball_row(top + i + 1, i, top + i == sel);
    } else if (sel != prev_sel) {
      int oi = prev_sel - top, ni = sel - top;
      if (oi >= 0 && oi < PDNA_BALL_VIS) ball_row(top + oi + 1, oi, false);
      if (ni >= 0 && ni < PDNA_BALL_VIS) ball_row(top + ni + 1, ni, true);
    }

    /* title (".. sel+1/NBALL") is the one thing above the hline that changes with the
     * cursor alone; cheap (no SD), so it just repaints on any sel move, full or not. */
    if (full || sel != prev_sel) {
      ui_fill_rect(0, 0, UI_SCR_W, 11, UI_BG);
      char h[40];
      siprintf(h, PDNA_BALL_TITLE_FMT, PDNA_BALL_TITLE, sel + 1, NBALL);
      ui_text(4, 2, UI_TITLE, h);
    }

    prev_sel = sel; prev_top = top; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B);
    if (k & KEY_B) return current;
    else if (k & KEY_A) return (uint8_t)(sel + 1);
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : NBALL - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % NBALL;
    else if (k & KEY_L)    sel = 0;
    else if (k & KEY_R)    sel = NBALL - 1;
  }
}

/* ===================== met-location + region pickers =======================
 * ~217 usable place ids is far too many for a ±1 field, so both are lists over
 * gen3_places.c: it owns which ids exist, which region each belongs to, and which origin
 * games can stamp one. The picker only draws.
 *
 * FILTER + SORT (Guy's ask):
 *   - region scope   (All / Hoenn / Kanto / Sevii Isles / Special) — and, because region
 *     and place are the same axis at two zooms, choosing a region in pick_region() drops
 *     straight into this list already scoped to it.
 *   - game scope, which is the LEGALITY guard: it defaults to the record's own metGame,
 *     so a FireRed save is never offered Hoenn routes and an RS save is never offered
 *     Emerald's Marine Cave. Widening it is a deliberate press, not the default.
 *   - sort by id / name / region.
 *   - SELECT opens the on-screen keyboard for an incremental substring filter. That is
 *     the convention every other list here already uses (species, move, item), and it
 *     beats a first-letter jump for this data set: the useful queries are "ROUTE 1",
 *     "UNDERWATER" and "CAVE", i.e. shared INFIXES of dozens of names, which a
 *     first-letter jump cannot express at all. Typing digits filters by id instead.
 */
/* One row of loc_filter_menu: row 0 sort, row 1 game, row 2 = All, rows 3.. = regions
 * (pick+close). Same IFILT_* box geometry (and the same shared-scanline ordering
 * requirement -- see ifilt_row_bg's comment) as item_filter_menu. */
static void lfm_row(int r, int y, bool sel, int sort, int gamef, int region) {
  ifilt_row_bg(y, sel);
  char b[40];
  if (r == 0)      { siprintf(b, PDNA_FILT_SORT_FMT, g3_place_sort_name(sort));
                     ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_DIRCLR, b); }
  else if (r == 1) { siprintf(b, PDNA_LFILT_GAME_FMT, g3_place_game_name(gamef));
                     ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_DIRCLR, b); }
  else {
    int rgn = r - 3;                          /* row 2 = All, rows 3.. = the regions */
    const char* nm = (r == 2) ? PDNA_LFILT_ALL : g3_region_name(rgn);
    siprintf(b, "%s%s", nm, (region == rgn) ? "  <" : "");
    ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_TEXT, b);
  }
}

/* No scrolling (PDNA_LFILT_ROWS is small and fixed), so `full` is just first-paint /
 * overlay-wiped-us. Same row-pair / toggle-row split as item_filter_menu. */
static void loc_filter_menu(int* region, int* gamef, int* sort) {
  const int rows = PDNA_LFILT_ROWS;
  int sel = 0;
  int prev_sel = -1, prev_sort = -1, prev_gamef = -1;
  uint32_t gen = 0; bool valid = false;
  for (;;) {
    bool full = !valid || gen != ui_clear_gen();

    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, PDNA_LFILT_TITLE);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      for (int r = 0; r < rows; r++)                        /* selected row LAST: rule (2) */
        if (r != sel) lfm_row(r, PDNA_IFILT_Y0 + r * PDNA_IFILT_ROW_H, false, *sort, *gamef, *region);
      if (sel >= 0 && sel < rows)
        lfm_row(sel, PDNA_IFILT_Y0 + sel * PDNA_IFILT_ROW_H, true, *sort, *gamef, *region);
      ui_text(4, PDNA_FILT_FOOTER_Y, UI_DIM, PDNA_IFILT_FOOT);
    } else if (sel != prev_sel) {
      if (prev_sel >= 0 && prev_sel < rows)                 /* old row first: rule (2) */
        lfm_row(prev_sel, PDNA_IFILT_Y0 + prev_sel * PDNA_IFILT_ROW_H, false, *sort, *gamef, *region);
      lfm_row(sel,      PDNA_IFILT_Y0 + sel      * PDNA_IFILT_ROW_H, true,  *sort, *gamef, *region);
    } else if (*sort != prev_sort || *gamef != prev_gamef) {
      lfm_row(sel, PDNA_IFILT_Y0 + sel * PDNA_IFILT_ROW_H, true, *sort, *gamef, *region);
    }

    prev_sel = sel; prev_sort = *sort; prev_gamef = *gamef; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_A) {
      if (sel == 0)      *sort  = (*sort + 1) % G3_PSORT_COUNT;
      else if (sel == 1) *gamef = (*gamef + 1) % G3_PGAME_COUNT;
      else { *region = sel - 3; return; }         /* a region picks + closes */
    }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : rows - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % rows;
    else if (k & KEY_LEFT) { if (sel == 0) *sort  = (*sort + G3_PSORT_COUNT - 1) % G3_PSORT_COUNT;
                             if (sel == 1) *gamef = (*gamef + G3_PGAME_COUNT - 1) % G3_PGAME_COUNT; }
    else if (k & KEY_RIGHT){ if (sel == 0) *sort  = (*sort + 1) % G3_PSORT_COUNT;
                             if (sel == 1) *gamef = (*gamef + 1) % G3_PGAME_COUNT; }
  }
}

/* Short header tags; the long names stay in the filter menu. [0] of the region table is
 * "all regions", so a G3_RGN_* indexes it at r + 1. */
#define PDNA_TAG_ONE(s) s,
static const char* const LOC_RGN_TAG[]  = { PDNA_LOC_RGN_TAGS(PDNA_TAG_ONE) };
static const char* const LOC_GAME_TAG[] = { PDNA_LOC_GAME_TAGS(PDNA_TAG_ONE) };
static const char* const LOC_SORT_TAG[] = { PDNA_LOC_SORT_TAGS(PDNA_TAG_ONE) };
_Static_assert(sizeof LOC_RGN_TAG  / sizeof LOC_RGN_TAG[0]  == G3_RGN_COUNT + 1, "region tags");
_Static_assert(sizeof LOC_GAME_TAG / sizeof LOC_GAME_TAG[0] == G3_PGAME_COUNT,   "game tags");
_Static_assert(sizeof LOC_SORT_TAG / sizeof LOC_SORT_TAG[0] == G3_PSORT_COUNT,   "sort tags");

/* One row of the met-location list: 9 px selection height on an 8 px row pitch -- the
 * same overlapping shape as list_pick's non-icon row, and governed by the same invariant
 * (2) of the ROW REPAINT RULE (selected row painted last by BOTH paint paths). See
 * lp_row's comment for the descender caveat this geometry carries; s_location[] is
 * upper-case throughout, so nothing on this screen is clipped by it. */
static void ml_row(uint16_t id, int y, bool sel) {
  if (sel) ui_panel(2, y + PDNA_LOC_SEL_DY, 236, PDNA_LOC_SEL_H, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y + PDNA_LOC_SEL_DY, 236, PDNA_LOC_SEL_H, UI_BG);
  char row[64], rt[72];
  siprintf(row, "%3d %s", id, pk_location_name(id));
  ui_truncate(rt, row, PDNA_LOC_ROW_COLS);
  ui_text(PDNA_LOC_TEXT_X, y, sel ? UI_SELTEXT : UI_TEXT, rt);
}

uint16_t pick_metloc(uint16_t current, uint8_t metgame, int region0) {
  u16* idx = g_idx;
  char search[16] = "";
  int sort = G3_PSORT_ID;
  int gamef = g3_game_filter_for(metgame);
  int region = region0;                            /* <0 = all regions */
  int n = g3_place_list(idx, G3_PLACE_MAX, region, gamef, search, sort);
  int sel = 0;
  for (int i = 0; i < n; i++) if (idx[i] == current) { sel = i; break; }
  int top = 0;
  int prev_top = -1, prev_sel = -1;
  bool relist = false;
  uint32_t gen = 0; bool valid = false;

  for (;;) {
    if (sel >= n) sel = n ? n - 1 : 0;
    if (sel < top) top = sel;
    if (sel >= top + PDNA_LOC_VIS) top = sel - PDNA_LOC_VIS + 1;
    if (top < 0) top = 0;

    /* full: first paint, the list was rebuilt under us, an overlay wiped us, or the
     * window scrolled. Otherwise a cursor move touches just the row pair.
     *
     * `relist` is not redundant with `gen`. Both overlays this screen opens DO happen to
     * ui_clear() at least once even on an immediate cancel (osk_core renders before its
     * first key read; loc_filter_menu's first iteration is always `full`) -- but that is
     * a property of THEIR bodies, and loc_filter_menu's body is one this very slice
     * rewrote from "clear every frame" to "clear once per visit". Leaning on it would
     * make this screen silently under-repaint the day someone adds an early return above
     * that first paint. A rebuild is something WE do, so we say so ourselves, exactly
     * like pick_item does around item_filter_menu / item_build. */
    bool full = relist || !valid || gen != ui_clear_gen() || top != prev_top;
    relist = false;

    if (full) {
      ui_clear();
      char h[64], ht[48];
      siprintf(h, PDNA_LOC_HDR_FMT, LOC_RGN_TAG[region < 0 ? 0 : region + 1],
               LOC_GAME_TAG[gamef], LOC_SORT_TAG[sort], n);
      ui_truncate(ht, h, PDNA_LOC_HDR_COLS);
      ui_text(4, 2, UI_TITLE, ht);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      if (!n) ui_ptext(PDNA_LOC_TEXT_X, PDNA_LOC_Y0 + 8, UI_WARN, PDNA_LOC_EMPTY);
      for (int i = 0; i < PDNA_LOC_VIS && top + i < n; i++) /* selected row LAST: rule (2) */
        if (top + i != sel) ml_row(idx[top + i], PDNA_LOC_Y0 + i * PDNA_LOC_ROW_H, false);
      if (n && sel >= top && sel < top + PDNA_LOC_VIS)
        ml_row(idx[sel], PDNA_LOC_Y0 + (sel - top) * PDNA_LOC_ROW_H, true);
      ui_hline(0, PDNA_LOC_RULE_Y, UI_SCR_W, UI_BORDER);
      ui_text(4, PDNA_FILT_FOOTER_Y, UI_DIM, PDNA_LOC_FOOT);
    } else if (sel != prev_sel) {
      int oi = prev_sel - top, ni = sel - top;
      if (oi >= 0 && oi < PDNA_LOC_VIS) ml_row(idx[prev_sel], PDNA_LOC_Y0 + oi * PDNA_LOC_ROW_H, false);
      if (ni >= 0 && ni < PDNA_LOC_VIS) ml_row(idx[sel],      PDNA_LOC_Y0 + ni * PDNA_LOC_ROW_H, true);
    }

    prev_sel = sel; prev_top = top; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B | KEY_SELECT | KEY_START);
    if (k & KEY_B) return CANCEL;
    else if (k & KEY_A) return n ? idx[sel] : CANCEL;
    else if (k & KEY_UP)   sel = clampi(sel - 1, 0, n ? n - 1 : 0);
    else if (k & KEY_DOWN) sel = clampi(sel + 1, 0, n ? n - 1 : 0);
    else if (k & KEY_L)    sel = clampi(sel - PDNA_LOC_PAGE, 0, n ? n - 1 : 0);
    else if (k & KEY_R)    sel = clampi(sel + PDNA_LOC_PAGE, 0, n ? n - 1 : 0);
    else if (k & KEY_START) {
      uint16_t keep = n ? idx[sel] : current;
      loc_filter_menu(&region, &gamef, &sort);
      n = g3_place_list(idx, G3_PLACE_MAX, region, gamef, search, sort);
      sel = 0; top = 0; relist = true;
      for (int i = 0; i < n; i++) if (idx[i] == keep) { sel = i; break; }
    }
    else if (k & KEY_SELECT) {
      char q[16];
      if (osk_search("SEARCH", search, q, sizeof(q))) {
        strcpy(search, q);
        n = g3_place_list(idx, G3_PLACE_MAX, region, gamef, search, sort);
        sel = 0; top = 0; relist = true;
      }
    }
  }
}

/* One row of pick_region: a 14 px selection box on a 16 px pitch (2 px clear on both
 * sides -- no neighbor overlap, unlike ml_row/lp_row's 9-on-8 shape, so no ordering
 * care needed here). `cnt` -- how many places this region offers THIS game, 0 being
 * the honest way to show that a FireRed record has no Hoenn to be met in -- is
 * recomputed here via g3_place_list (a pure in-memory scan over the places table, no
 * SD) rather than cached, so a row-pair cursor move now costs 2 scans instead of the
 * full paint's G3_RGN_COUNT: a free byproduct of only redrawing the rows that changed,
 * not a new cache (gamef is fixed for the screen's life, so the count per row never
 * goes stale). */
static void rgn_row(int r, int y, bool sel, int gamef) {
  if (sel) ui_panel(2, y - 2, 236, 14, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y - 2, 236, 14, UI_BG);
  int cnt = g3_place_list(g_idx, G3_PLACE_MAX, r, gamef, "", G3_PSORT_ID);
  char b[40];
  siprintf(b, PDNA_RGN_ROW_FMT, g3_region_name(r), cnt);
  ui_text(PDNA_RGN_TEXT_X, y, sel ? UI_SELTEXT : (cnt ? UI_TEXT : UI_DIM), b);
}

int pick_region(int current, uint8_t metgame) {
  int gamef = g3_game_filter_for(metgame);
  int sel = (current >= 0 && current < G3_RGN_COUNT) ? current : 0;
  int prev_sel = -1;
  uint32_t gen = 0; bool valid = false;
  for (;;) {
    bool full = !valid || gen != ui_clear_gen();

    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, PDNA_RGN_TITLE);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      for (int r = 0; r < G3_RGN_COUNT; r++)                /* selected row LAST: rule (2) */
        if (r != sel) rgn_row(r, PDNA_RGN_Y0 + r * PDNA_RGN_ROW_H, false, gamef);
      if (sel >= 0 && sel < G3_RGN_COUNT)
        rgn_row(sel, PDNA_RGN_Y0 + sel * PDNA_RGN_ROW_H, true, gamef);
      ui_ptext(4, PDNA_RGN_NOTE_Y, UI_DIM, PDNA_RGN_NOTE);
      ui_hline(0, PDNA_RGN_RULE_Y, UI_SCR_W, UI_BORDER);
      ui_text(4, PDNA_FILT_FOOTER_Y, UI_DIM, PDNA_RGN_FOOT);
    } else if (sel != prev_sel) {
      if (prev_sel >= 0 && prev_sel < G3_RGN_COUNT)         /* old row first: rule (2) */
        rgn_row(prev_sel, PDNA_RGN_Y0 + prev_sel * PDNA_RGN_ROW_H, false, gamef);
      rgn_row(sel,      PDNA_RGN_Y0 + sel      * PDNA_RGN_ROW_H, true,  gamef);
    }

    prev_sel = sel; valid = true; gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    else if (k & KEY_A) return sel;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : G3_RGN_COUNT - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % G3_RGN_COUNT;
  }
}
