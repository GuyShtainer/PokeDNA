/*
 * Rich pickers for the editor: an HGSS-style species icon grid with live search
 * + filter (Gen/type/legendary) + sort, and a move picker with type chips,
 * power/accuracy/PP and the in-game description. Item/nature use searchable lists.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "pdna_pick.h"
#include "pdna_app.h"      /* app_confirm, app_anim_enabled (Pokedex screen) */
#include "ui.h"
#include "data_tables.h"
#include "mon_icons.h"
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
  ui_fill_rect(x, y, 26, 9, type_color(t));
  ui_text(x + 2, y, RGB15(31, 31, 31), TYPE_ABBR[t]);
}
/* The real Gen-3 type badge (32x14, generated). Use where a row is tall enough;
 * the compact text type_chip() stays for 8-9px list rows. */
static void type_icon(int x, int y, uint8_t t) {
  if (t >= 18) return;
  ui_sprite(x, y, TYPE_ICON_W, TYPE_ICON_H, type_icon_for(t));
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

/* the filter ids selectable in the menu / by L-R (All, Gen1-3, Legendary, types
 * except the unused MYSTERY type 9). */
static int filter_ids(int* out) {
  int n = 0;
  for (int f = 0; f <= 4; f++) out[n++] = f;
  for (int t = 0; t < 18; t++) if (t != 9) out[n++] = 5 + t;
  return n;
}

/* a nice list to jump to any filter (type rows show a colored chip) + sort toggle. */
static void filter_menu(int* filter, int* sort) {
  int fids[24];
  int nf = filter_ids(fids);
  int rows = 1 + nf, sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + 16) top = sel - 15;
    ui_clear();
    ui_text(4, 2, UI_TITLE, "FILTER / SORT");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < 16 && top + i < rows; i++) {
      int r = top + i, y = 14 + i * 8;
      bool s = (r == sel);
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      if (r == 0) {
        char b[32]; siprintf(b, "Sort: %s", *sort ? "A-Z (name)" : "No. (dex)");
        ui_text(8, y, s ? UI_SELTEXT : UI_DIRCLR, b);
      } else {
        int fid = fids[r - 1];
        if (fid >= 5) { type_chip(8, y, (uint8_t)(fid - 5)); ui_text(40, y, s ? UI_SELTEXT : UI_TEXT, filter_name(fid)); }
        else ui_text(8, y, s ? UI_SELTEXT : UI_TEXT, filter_name(fid));
      }
    }
    ui_text(4, 152, UI_DIM, "A select  U/D move  L/R page  B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_A) { if (sel == 0) *sort ^= 1; else { *filter = fids[sel - 1]; return; } }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : rows - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % rows;
    else if (k & KEY_L)    sel = clampi(sel - 8, 0, rows - 1);
    else if (k & KEY_R)    sel = clampi(sel + 8, 0, rows - 1);
  }
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

  for (;;) {
    if (sel >= g_n) sel = g_n ? g_n - 1 : 0;
    int srow = sel / GCOLS;           /* edge scroll: cursor roams the page, list moves only at the edges */
    if (srow < toprow) toprow = srow;
    if (srow >= toprow + GVROWS) toprow = srow - GVROWS + 1;
    if (toprow < 0) toprow = 0;
    int top_idx = toprow * GCOLS;

    /* Only a page scroll or a list change needs a full repaint; moving the cursor
     * within the page just swaps the selection frame + repaints the header strip
     * (the slow per-pixel grid blit no longer runs on every keypress). */
    bool full = relist || top_idx != prev_top;
    relist = false;

    if (full) {
      ui_clear();
      ui_hline(0, 21, UI_SCR_W, UI_BORDER);
      ui_hline(0, 147, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A pick  L/R filter  ST menu  SEL find  B");
      for (int i = 0; i < GCOLS * GVROWS; i++) {
        int idx = top_idx + i;
        if (idx >= g_n) break;
        int x = GX + (i % GCOLS) * GCELLX, y = GY + (i / GCOLS) * GCELLY;
        ui_icon_scaled(x, y, GICON, GICON, mon_icon_for(g_list[idx]));
      }
    } else if (prev_sel >= top_idx && prev_sel < top_idx + GCOLS * GVROWS) {
      int pi = prev_sel - top_idx;                 /* erase the old selection frame */
      int px = GX + (pi % GCOLS) * GCELLX, py = GY + (pi / GCOLS) * GCELLY;
      m3_frame(px - 1, py - 1, px + GICON, py + GICON, UI_BG);
    }

    if (g_n) {                                      /* draw the current selection frame */
      int si = sel - top_idx;
      int sx = GX + (si % GCOLS) * GCELLX, sy = GY + (si / GCOLS) * GCELLY;
      m3_frame(sx - 1, sy - 1, sx + GICON, sy + GICON, UI_SELTEXT);
    }

    /* header strip (No./name + type badges + filter line) — repaint just this band */
    ui_fill_rect(0, 0, UI_SCR_W, 21, UI_BG);
    uint16_t cs = g_n ? g_list[sel] : 0;
    if (cs) {
      siprintf(hdr, "No.%u  %s", (unsigned)pk_national_no(cs), pk_species_name(cs));
      ui_text(4, 2, UI_TITLE, hdr);
      uint8_t t1 = pk_species_type1(cs), t2 = pk_species_type2(cs);
      if (t1 == t2) type_icon(204, 4, t1);
      else { type_icon(172, 4, t1); type_icon(205, 4, t2); }
    }
    siprintf(hdr, "[%s] sort:%s  %d", filter_name(filter), sort ? "A-Z" : "No.", g_n);
    ui_text(4, 11, UI_DIM, hdr);

    prev_sel = sel; prev_top = top_idx;

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT | KEY_START);
    if (k & KEY_B) return CANCEL;
    else if (k & KEY_A) return g_n ? g_list[sel] : CANCEL;
    else if (k & KEY_LEFT)  sel = (sel > 0) ? sel - 1 : 0;
    else if (k & KEY_RIGHT) sel = (sel < g_n - 1) ? sel + 1 : sel;
    else if (k & KEY_UP)    { if (sel >= GCOLS) sel -= GCOLS; }
    else if (k & KEY_DOWN)  { if (sel + GCOLS < g_n) sel += GCOLS; }
    else if (k & KEY_L) { do { filter = (filter + NFILTER - 1) % NFILTER; } while (filter == 5 + 9); build_species(filter, sort, search); sel = 0; relist = true; }
    else if (k & KEY_R) { do { filter = (filter + 1) % NFILTER; } while (filter == 5 + 9); build_species(filter, sort, search); sel = 0; relist = true; }
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

#define DEX_NAT_MAX     386
#define DEX_ANIM_PERIOD 30          /* vblanks per bob frame (~0.5s, the Gen-3 cadence) */

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

static DexGetState s_dget;
static DexSetState s_dset;
static DexGetNat   s_getnat;   /* national-dex live? (may be NULL) */
static DexSetNat   s_setnat;   /* enable/disable national dex (may be NULL) */

/* Snapshot of every species' dex state before the last bulk op, so a mistaken
 * "Catch/See/Wipe ALL" can be undone in one step (the changes aren't written to the
 * SD until the user confirms the dex save, so this RAM revert fully restores it). */
static int8_t s_dex_snap[DEX_NAT_MAX];
static bool   s_dex_snap_valid = false;

static int dstate(uint16_t internal) { return s_dget((int)pk_national_no(internal)); }

/* Build g_list for the dex: species filter (build_species) -> caught-status filter
 * -> (Type view only) a stable sort by primary type. */
static void dex_build(int filter, int sort, const char* search, int status, int view) {
  build_species(filter, sort, search);                 /* g_list/g_n by No. or A-Z */
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
  for (int nat = 1; nat <= DEX_NAT_MAX; nat++) { int st = s_dget(nat); if (st >= 1) s++; if (st >= 2) c++; }
  *seen = s; *caught = c;
}

static void dex_geom(int view, int* cols, int* cw, int* ch, int* x0, int* y0, int* vrows) {
  if (view == DV_LIST) { *cols = 1; *cw = 232; *ch = 9;  *x0 = 4; *y0 = 24; *vrows = 13; }
  else                 { *cols = 7; *cw = 33;  *ch = 34; *x0 = 8; *y0 = 24; *vrows = 3;  }
}

/* one grid/type cell at the icon origin: greyscale unseen, colour seen,
 * colour-at-frame-`bob` + Poke-Ball caught. */
static void dex_cell_grid(int x, int y, uint16_t in, int bob) {
  int st = dstate(in);
  if (st == 0)      ui_icon_scaled_grey(x, y, 32, 32, mon_icon_for(in));
  else if (st == 1) ui_icon_scaled(x, y, 32, 32, mon_icon_for(in));
  else { ui_icon_scaled(x, y, 32, 32, mon_icon_for_frame(in, (uint8_t)bob)); ui_pokeball(x + 21, y + 21); }
}

/* one list row text, coloured by state (or the selection colour) */
static void dex_cell_list(int x, int y, uint16_t in, bool sel) {
  int nat = pk_national_no(in), st = s_dget(nat);
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
    const char* L[6]; int act[6], n = 0;
    bool natl = (s_getnat && s_getnat());
    L[n] = "Catch ALL"; act[n++] = 2;
    L[n] = "See ALL";   act[n++] = 1;
    L[n] = "Wipe ALL";  act[n++] = 0;
    if (s_setnat) { L[n] = natl ? "Natl Dex: ON" : "Natl Dex: OFF"; act[n++] = 3; }
    if (s_dex_snap_valid) { L[n] = "Undo last"; act[n++] = -1; }
    L[n] = "Cancel"; act[n++] = -2;
    if (sel >= n) sel = n - 1;
    const int mx = 56, my = 42, mw = 128, mh = 18 + n * 14 + 11;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "DEX: ALL");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < n; i++) { int y = my + 18 + i * 14; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      uint16_t col = s ? UI_SELTEXT : (act[i] == -1 ? UI_OK : act[i] == 3 ? (natl ? UI_OK : UI_WARN) : UI_TEXT);
      ui_text(mx + 10, y, col, L[i]); }
    ui_text(mx + 6, my + mh - 9, UI_DIM, "A pick  B back");
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
        s_setnat(now);
        return true;
      }
      if (a == -1) {                                              /* Undo the last bulk op */
        for (int nat = 1; nat <= DEX_NAT_MAX; nat++) s_dset(nat, s_dex_snap[nat - 1]);
        s_dex_snap_valid = false;
        return true;
      }
      if (!app_confirm(a == 2 ? "Catch every species?" : a == 1 ? "See every species?" : "Wipe the whole dex?",
                       "All 386. (Undo available.)")) return false;
      for (int nat = 1; nat <= DEX_NAT_MAX; nat++) s_dex_snap[nat - 1] = (int8_t)s_dget(nat);   /* snapshot first */
      s_dex_snap_valid = true;
      for (int nat = 1; nat <= DEX_NAT_MAX; nat++) s_dset(nat, a);
      /* Catching every species is meaningless without National mode (the dex caps at the
       * regional list otherwise), so unlock it too — matches the user's expectation. */
      if (a == 2 && s_setnat) s_setnat(true);
      return true;
    }
  }
}

/* START menu: sort toggle, status cycle, optional "Mark all", then the
 * gen/type/legendary filter list. Returns 0 nothing, 1 rebuild needed, 2 the dex
 * was bulk-changed (rebuild + mark dirty). */
static int dex_menu(int* filter, int* sort, int* status, bool can_edit) {
  int fids[24]; int nf = filter_ids(fids);
  int base = can_edit ? 3 : 2;                          /* rows before the filter list */
  int rows = base + nf, sel = 0, top = 0;
  bool changed = false, bulked = false;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + 16) top = sel - 15;
    ui_clear();
    ui_text(4, 2, UI_TITLE, "FILTER / SORT / FIND");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < 16 && top + i < rows; i++) {
      int r = top + i, y = 14 + i * 8; bool s = (r == sel);
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      if (r == 0) { char b[32]; siprintf(b, "Sort: %s", *sort ? "A-Z (name)" : "No. (dex)");
                    ui_text(8, y, s ? UI_SELTEXT : UI_DIRCLR, b); }
      else if (r == 1) { char b[32]; siprintf(b, "Status: %s", DS_NAME[*status]);
                         ui_text(8, y, s ? UI_SELTEXT : UI_DIRCLR, b); }
      else if (can_edit && r == 2) ui_text(8, y, s ? UI_SELTEXT : UI_WARN, "Mark all...");
      else { int fid = fids[r - base];
             if (fid >= 5) { type_chip(8, y, (uint8_t)(fid - 5)); ui_text(40, y, s ? UI_SELTEXT : UI_TEXT, filter_name(fid)); }
             else ui_text(8, y, s ? UI_SELTEXT : UI_TEXT, filter_name(fid)); }
    }
    ui_text(4, 152, UI_DIM, "A select  U/D move  L/R page  B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B);
    if (k & KEY_B) return bulked ? 2 : (changed ? 1 : 0);
    else if (k & KEY_A) {
      if (sel == 0)       { *sort ^= 1; changed = true; }
      else if (sel == 1)  { *status = (*status + 1) % DS_N; changed = true; }
      else if (can_edit && sel == 2) { if (dex_bulk()) bulked = true; }   /* stay open */
      else { *filter = fids[sel - base]; return bulked ? 2 : 1; }         /* pick -> close */
    }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : rows - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % rows;
    else if (k & KEY_L)    sel = clampi(sel - 8, 0, rows - 1);
    else if (k & KEY_R)    sel = clampi(sel + 8, 0, rows - 1);
  }
}

bool pdna_dex_screen(DexGetState get, DexSetState set,
                     DexGetNat getnat, DexSetNat setnat, bool can_edit) {
  s_dget = get; s_dset = set;
  s_getnat = getnat; s_setnat = can_edit ? setnat : NULL;   /* read-only carts can't toggle it */
  s_dex_snap_valid = false;        /* fresh session: no bulk op to undo yet */
  int filter = 0, sort = 0, status = DS_ALL, view = DV_GRID;
  char search[16] = "";
  dex_build(filter, sort, search, status, view);
  int seen, caught; dex_counts(&seen, &caught);
  bool dirty = false;

  int sel = 0, toprow = 0;
  int prev_sel = -1, prev_top = -1, prev_view = -1;
  bool relist = true;
  int bob = 0, anim_ctr = 0;

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

    bool full = relist || view != prev_view || top != prev_top;
    relist = false;

    if (full) {
      ui_clear();
      ui_hline(0, 22, UI_SCR_W, UI_BORDER);
      ui_hline(0, 147, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, can_edit ? "A cycle  L/R view  ST opts  SEL find  B"
                                        : "L/R view  ST opts  SEL find  B back");
      for (int i = 0; i < vis && top + i < g_n; i++) {
        int x = x0 + (i % cols) * cw, y = y0 + (i / cols) * ch;
        if (grid) dex_cell_grid(x, y, g_list[top + i], bob);
        else dex_cell_list(x, y, g_list[top + i], (top + i == sel));
      }
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

    /* wait for input; meanwhile bob the caught cells (grid/type, anim enabled) */
    u16 k, fresh;
    const u16 dpad = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT;
    do {
      s_vsync();
      fresh = key_hit(KEY_FULL);
      k = fresh | key_repeat(dpad);
      /* Animate ONLY on idle frames (no key pending) so a press/repeat is never delayed
       * by the multi-cell repaint — this kills the occasional cursor "stick". Recompose
       * EVERY visible caught cell at once (shared `bob`) via compose-over-DMA, no erase,
       * so there's never a frame where a sprite is blanked. */
      if (!k && grid && app_anim_enabled(ANIM_DEX) && ++anim_ctr >= DEX_ANIM_PERIOD) {
        anim_ctr = 0; bob ^= 1;
        for (int i = 0; i < vis && top + i < g_n; i++) {
          uint16_t in = g_list[top + i];
          if (dstate(in) != 2) continue;
          int x = x0 + (i % cols) * cw, y = y0 + (i / cols) * ch;
          ui_blit_over(x, y, 32, 32, mon_icon_for_frame(in, (uint8_t)bob), UI_BG);
          ui_pokeball(x + 21, y + 21);
          if (top + i == sel) m3_frame(x - 1, y - 1, x + 32, y + 32, UI_SELTEXT);
        }
      }
    } while (!k);
    if      (fresh & dpad)                          snd_move();
    else if (fresh & (KEY_L | KEY_R | KEY_SELECT))  snd_tab();
    else if (fresh & (KEY_A | KEY_START))           snd_ok();
    else if (fresh & KEY_B)                         snd_back();

    if (k & KEY_B) break;
    else if (k & KEY_A) {
      if (can_edit && g_n) {
        uint16_t in = g_list[sel]; int nat = pk_national_no(in);
        s_dset(nat, (s_dget(nat) + 1) % 3);
        dirty = true; dex_counts(&seen, &caught);
        if (status != DS_ALL) { dex_build(filter, sort, search, status, view); relist = true; }  /* may drop out */
        else if (grid) {                               /* repaint this cell's new state */
          int si = sel - top, sx = x0 + (si % cols) * cw, sy = y0 + (si / cols) * ch;
          ui_fill_rect(sx, sy, 32, 32, UI_BG);
          dex_cell_grid(sx, sy, in, bob);
          m3_frame(sx - 1, sy - 1, sx + 32, sy + 32, UI_SELTEXT);
        }
        /* list state is reflected by the selection-chrome redraw next iteration */
      } else if (!can_edit) snd_deny();
    }
    else if (k & KEY_UP)    { if (cols == 1) sel = (sel > 0) ? sel - 1 : (g_n ? g_n - 1 : 0);          /* top -> wrap to last */
                              else if (sel >= cols) sel -= cols; else sel = g_n ? g_n - 1 : 0; }
    else if (k & KEY_DOWN)  { if (cols == 1) sel = (g_n && sel < g_n - 1) ? sel + 1 : 0;               /* bottom -> wrap to first */
                              else if (sel + cols < g_n) sel += cols; else sel = 0; }
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
  return dirty;
}

/* ===================== move picker ===================================== */

static u16 EWRAM_BSS g_mv[NMOVE];
static int g_mvn;

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

uint16_t pick_move(uint16_t current) {
  int tf = -1, sort = 0;
  char search[16] = "";
  build_moves(tf, sort, search);
  int sel = 0;
  for (int i = 0; i < g_mvn; i++) if (g_mv[i] == current) { sel = i; break; }

  int top = 0;
  for (;;) {
    if (sel >= g_mvn) sel = g_mvn ? g_mvn - 1 : 0;
    if (sel < top) top = sel;
    if (sel >= top + 9) top = sel - 8;

    ui_clear();
    char h[48];
    siprintf(h, "MOVES [%s] sort:%s  %d", tf < 0 ? "All" : pk_type_name((uint8_t)tf),
             MV_SORT[sort], g_mvn);
    ui_text(4, 1, UI_TITLE, h);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);

    for (int i = 0; i < 9 && top + i < g_mvn; i++) {
      uint16_t m = g_mv[top + i];
      int y = 14 + i * 9;
      bool s = (top + i == sel);
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      char nm[20]; ui_truncate(nm, pk_move_name(m), 14);
      ui_text(6, y, s ? UI_SELTEXT : UI_TEXT, nm);
      type_chip(120, y, pk_move_type(m));
    }

    /* detail panel for the selected move: fixed-width stat columns so the real
     * type badge gets its own column top-right and never sits on the numbers. */
    if (g_mvn) {
      uint16_t m = g_mv[sel];
      ui_panel(0, 92, UI_SCR_W, 58, UI_PANEL, UI_BORDER);
      type_icon(202, 95, pk_move_type(m));
      char num[48];
      siprintf(num, "Pow %3u  Acc %3u  PP %2u",
               (unsigned)pk_move_power(m), (unsigned)pk_move_accuracy(m), (unsigned)pk_move_pp(m));
      ui_text(6, 96, UI_DIRCLR, num);
      text_wrap(6, 110, 28, UI_TEXT, pk_move_desc(m));
    }

    ui_text(4, 152, UI_DIM, "A pick  L/R type  ST sort  SEL find  B");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT | KEY_START);
    if (k & KEY_B) return CANCEL;
    else if (k & KEY_A) return g_mvn ? g_mv[sel] : CANCEL;
    else if (k & KEY_UP)   sel = clampi(sel - 1, 0, g_mvn ? g_mvn - 1 : 0);
    else if (k & KEY_DOWN) sel = clampi(sel + 1, 0, g_mvn ? g_mvn - 1 : 0);
    else if (k & KEY_L) { do { tf = (tf <= -1) ? 17 : tf - 1; } while (tf == 9); build_moves(tf, sort, search); sel = 0; top = 0; }
    else if (k & KEY_R) { do { tf = (tf >= 17) ? -1 : tf + 1; } while (tf == 9); build_moves(tf, sort, search); sel = 0; top = 0; }
    else if (k & KEY_START) { sort = (sort + 1) % NMVSORT; build_moves(tf, sort, search); sel = 0; top = 0; }
    else if (k & KEY_SELECT) { char q[16]; if (osk_search("SEARCH", search, q, sizeof(q))) { strcpy(search, q); build_moves(tf, sort, search); sel = 0; top = 0; } }
  }
}

/* ===================== generic searchable list (item / nature) ========= */

/* filter by `search` then optionally sort A-Z by name; returns the count. */
static int list_build(u16* idx, int count, const char* (*name_fn)(uint16_t),
                      const char* search, int sort) {
  int n = 0;
  for (int i = 0; i < count; i++)
    if (!search[0] || ci_contains(name_fn((uint16_t)i), search)) idx[n++] = (u16)i;
  if (sort) {                                       /* insertion sort by name (No. = id order) */
    for (int i = 1; i < n; i++) {
      u16 v = idx[i]; int j = i - 1;
      while (j >= 0 && strcmp(name_fn(idx[j]), name_fn(v)) > 0) { idx[j + 1] = idx[j]; j--; }
      idx[j + 1] = v;
    }
  }
  return n;
}

static uint16_t list_pick(const char* title, int count, const char* (*name_fn)(uint16_t),
                          const uint16_t* (*icon_fn)(uint16_t), int current,
                          bool searchable, bool sortable) {
  static u16 EWRAM_BSS idx[NITEM];
  char search[16] = "";
  int sort = 0;
  int n = list_build(idx, count, name_fn, search, sort);
  int sel = 0;
  for (int i = 0; i < n; i++) if (idx[i] == current) { sel = i; break; }

  const int rowh = icon_fn ? 26 : 8;                /* taller rows when showing 24x24 icons */
  const int vis  = icon_fn ? 5 : 16;
  const int page = icon_fn ? 5 : 10;
  int top = 0;

  for (;;) {
    if (sel >= n) sel = n ? n - 1 : 0;
    if (sel < top) top = sel;                       /* edge scroll: cursor roams, list moves only at edges */
    if (sel >= top + vis) top = sel - vis + 1;
    if (top < 0) top = 0;

    ui_clear();
    char h[40]; siprintf(h, "%s  %s  %d", title, sortable ? (sort ? "A-Z" : "No.") : "", n);
    ui_text(4, 2, UI_TITLE, h);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    char row[40], rt[40];
    for (int i = 0; i < vis && top + i < n; i++) {
      int id = idx[top + i], y = 14 + i * rowh;
      bool s = (top + i == sel);
      siprintf(row, "%3d %s", id, name_fn((uint16_t)id));
      if (icon_fn) {
        if (s) ui_panel(2, y - 1, 236, rowh - 1, UI_SEL, UI_TITLE);
        const uint16_t* ic = icon_fn((uint16_t)id);
        if (ic) ui_sprite(4, y, ITEM_ICON_W, ITEM_ICON_H, ic);
        ui_truncate(rt, row, 24);
        ui_text(32, y + 8, s ? UI_SELTEXT : UI_TEXT, rt);
      } else {
        if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
        ui_truncate(rt, row, 28);
        ui_text(6, y, s ? UI_SELTEXT : UI_TEXT, rt);
      }
    }
    char foot[48];
    siprintf(foot, "A pick  L/R +-%d  %s%sB", page,
             searchable ? "SEL find  " : "", sortable ? "ST sort  " : "");
    ui_text(4, 152, UI_DIM, foot);

    u16 mask = KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B;
    if (searchable) mask |= KEY_SELECT;
    if (sortable)   mask |= KEY_START;
    u16 k = s_wait(mask);
    if (k & KEY_B) return CANCEL;
    else if (k & KEY_A) return n ? idx[sel] : CANCEL;
    else if (k & KEY_UP)   sel = clampi(sel - 1, 0, n ? n - 1 : 0);
    else if (k & KEY_DOWN) sel = clampi(sel + 1, 0, n ? n - 1 : 0);
    else if (k & KEY_L)    sel = clampi(sel - page, 0, n ? n - 1 : 0);
    else if (k & KEY_R)    sel = clampi(sel + page, 0, n ? n - 1 : 0);
    else if (sortable && (k & KEY_START)) { sort ^= 1; n = list_build(idx, count, name_fn, search, sort); sel = 0; }
    else if (searchable && (k & KEY_SELECT)) {
      char q[16];
      if (osk_search("SEARCH", search, q, sizeof(q))) { strcpy(search, q); n = list_build(idx, count, name_fn, search, sort); sel = 0; }
    }
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
  const uint16_t* ic = item_icon_for((uint16_t)id);
  if (v == IV_LIST) {
    char row[40], rt[40];
    siprintf(row, "%3d %s", id, pk_item_name((uint16_t)id));
    ui_truncate(rt, row, 28);
    ui_text(x + 2, y, UI_TEXT, rt);
  } else if (v == IV_ICONS) {
    if (ic) ui_sprite(x, y, IITEM, IITEM, ic);
  } else if (v == IV_GRID) {
    if (ic) ui_sprite(x, y, IITEM, IITEM, ic);
    char nm[16]; ui_truncate(nm, pk_item_name((uint16_t)id), 9);
    ui_text(x, y + 25, UI_DIM, nm);
  } else {                                          /* IV_SPLIT left row: icon + name */
    if (ic) ui_sprite(x, y, IITEM, IITEM, ic);
    char nm[16]; ui_truncate(nm, pk_item_name((uint16_t)id), 11);
    ui_text(x + 28, y + 8, UI_TEXT, nm);
  }
}

uint16_t pick_item(uint16_t current) {
  static u16 EWRAM_BSS idx[NITEM];
  char search[16] = "";
  int sort = 0, view = IV_SPLIT;
  int n = list_build(idx, 377, pk_item_name, search, sort);
  int sel = 0;
  for (int i = 0; i < n; i++) if (idx[i] == current) { sel = i; break; }

  int prev_sel = -1, prev_top = -1, prev_view = -1, toprow = 0;
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

    bool full = relist || view != prev_view || top != prev_top;
    relist = false;

    if (full) {
      ui_clear();
      char h[40]; siprintf(h, "ITEM  %s  %s  %d", IV_NAME[view], sort ? "A-Z" : "No.", n);
      ui_text(4, 2, UI_TITLE, h);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      ui_hline(0, 147, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A pick  L/R view  ST sort  SEL find  B");
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
      ui_text(126, 24, UI_TITLE, pk_item_name(cur));
      text_wrap(126, 36, 13, UI_TEXT, pk_item_desc(cur));
    } else {
      ui_fill_rect(0, 138, UI_SCR_W, 8, UI_BG);
      char d[80], dt[40];
      siprintf(d, "%s  %s", pk_item_name(cur), pk_item_desc(cur));
      ui_truncate(dt, d, 29);
      ui_text(4, 139, UI_DIM, dt);
    }

    prev_sel = sel; prev_top = top; prev_view = view;

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT | KEY_START);
    if (k & KEY_B) return CANCEL;
    else if (k & KEY_A) return n ? idx[sel] : CANCEL;
    else if (k & KEY_UP)    sel = clampi(sel - cols, 0, n ? n - 1 : 0);
    else if (k & KEY_DOWN)  sel = clampi(sel + cols, 0, n ? n - 1 : 0);
    else if (k & KEY_LEFT)  { if (cols > 1) sel = clampi(sel - 1, 0, n ? n - 1 : 0); }
    else if (k & KEY_RIGHT) { if (cols > 1) sel = clampi(sel + 1, 0, n ? n - 1 : 0); }
    else if (k & KEY_L) { view = (view + IV_N - 1) % IV_N; relist = true; }
    else if (k & KEY_R) { view = (view + 1) % IV_N; relist = true; }
    else if (k & KEY_START) { sort ^= 1; n = list_build(idx, 377, pk_item_name, search, sort); sel = 0; relist = true; }
    else if (k & KEY_SELECT) {
      char q[16];
      if (osk_search("SEARCH", search, q, sizeof(q))) { strcpy(search, q); n = list_build(idx, 377, pk_item_name, search, sort); sel = 0; relist = true; }
    }
  }
}
static const char* nature16(uint16_t n) { return pk_nature_name((uint8_t)n); }
uint8_t  pick_nature(uint8_t current)  { uint16_t r = list_pick("NATURE", 25, nature16, 0, current, false, false); return r == CANCEL ? current : (uint8_t)r; }

uint8_t pick_ability(uint16_t species, uint8_t cur) {
  uint16_t a0 = pk_species_ability(species, 0), a1 = pk_species_ability(species, 1);
  int n = (a1 && a1 != a0) ? 2 : 1;            /* most species have 2 distinct abilities */
  int sel = (cur && n == 2) ? 1 : 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "ABILITY");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < n; i++) {
      uint16_t aid = i ? a1 : a0;
      int y = 18 + i * 56;
      bool s = (i == sel);
      ui_panel(2, y - 2, 236, 52, s ? UI_SEL : UI_PANEL, s ? UI_TITLE : UI_BORDER);
      char h[24]; siprintf(h, "%d. %s", i + 1, pk_ability_name(aid));
      ui_text(8, y + 2, s ? UI_SELTEXT : UI_TEXT, h);
      text_wrap(8, y + 14, 28, UI_DIM, pk_ability_desc(aid));
    }
    ui_text(4, 140, UI_DIM, "Gen-3 stores only the species' abilities");
    ui_text(4, 152, UI_DIM, "A pick  U/D move  B cancel");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return cur;
    else if (k & KEY_A) return (uint8_t)sel;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
  }
}

/* Choose an Unown letter (0..27 = A..Z ! ?). Shows all 28 forms as a grid of the
 * real letter icons. Returns the chosen form, or -1 on cancel. */
int pick_unown_form(int cur) {
  static const char* const LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!?";
  const int uc = 7, uw = 30, ux0 = 22, uy0 = 22;
  int sel = (cur >= 0 && cur < 28) ? cur : 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "CHOOSE UNOWN LETTER");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < 28; i++) {
      int x = ux0 + (i % uc) * uw, y = uy0 + (i / uc) * uw;
      if (i == sel) m3_frame(x - 2, y - 2, x + 25, y + 25, UI_SELTEXT);
      ui_icon_scaled(x, y, 24, 24, mon_icon_for_form(201, (uint8_t)i));
      char ch[2] = { LET[i], 0 };
      ui_text(x + 8, y + 25, i == sel ? UI_SELTEXT : UI_DIM, ch);
    }
    ui_text(4, 152, UI_DIM, "Move  A pick  B cancel");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    else if (k & KEY_A) return sel;
    else if (k & KEY_LEFT)  sel = (sel > 0) ? sel - 1 : 27;
    else if (k & KEY_RIGHT) sel = (sel + 1) % 28;
    else if (k & KEY_UP)    { if (sel >= uc) sel -= uc; }
    else if (k & KEY_DOWN)  { if (sel + uc < 28) sel += uc; }
  }
}
