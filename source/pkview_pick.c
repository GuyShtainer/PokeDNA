/*
 * Rich pickers for the editor: an HGSS-style species icon grid with live search
 * + filter (Gen/type/legendary) + sort, and a move picker with type chips,
 * power/accuracy/PP and the in-game description. Item/nature use searchable lists.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "pkview_pick.h"
#include "ui.h"
#include "data_tables.h"
#include "mon_icons.h"
#include "type_icons.h"
#include "item_icons.h"
#include "osk.h"

#define CANCEL 0xFFFF
#define NSPECIES 412
#define NMOVE    355
#define NITEM    400

static u16 EWRAM_BSS g_list[NSPECIES];   /* internal species ids in display order */
static int g_n;

static void s_vsync(void) { VBlankIntrWait(); key_poll(); }
static u16  s_wait(u16 m) { u16 k; do { s_vsync(); k = key_hit(m); } while (!k); return k; }
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

#define GCOLS 8           /* bigger icons (24x24) -> fewer, larger cells */
#define GVROWS 4
#define GCELLX 27
#define GCELLY 28
#define GX 12
#define GY 26
#define GICON 24

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

  for (;;) {
    if (sel >= g_n) sel = g_n ? g_n - 1 : 0;
    int top = (sel / GCOLS) - (GVROWS - 1);
    if (top < 0) top = 0;
    int top_idx = top * GCOLS;

    ui_clear();
    char hdr[48];
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
    ui_hline(0, 21, UI_SCR_W, UI_BORDER);

    for (int i = 0; i < GCOLS * GVROWS; i++) {
      int idx = top_idx + i;
      if (idx >= g_n) break;
      int x = GX + (i % GCOLS) * GCELLX, y = GY + (i / GCOLS) * GCELLY;
      ui_icon_scaled(x, y, GICON, GICON, mon_icon_for(g_list[idx]));
      if (idx == sel) m3_frame(x - 1, y - 1, x + GICON, y + GICON, UI_SELTEXT);
    }

    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, "A pick  L/R filter  ST menu  SEL find  B");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT | KEY_START);
    if (k & KEY_B) return CANCEL;
    else if (k & KEY_A) return g_n ? g_list[sel] : CANCEL;
    else if (k & KEY_LEFT)  sel = (sel > 0) ? sel - 1 : 0;
    else if (k & KEY_RIGHT) sel = (sel < g_n - 1) ? sel + 1 : sel;
    else if (k & KEY_UP)    { if (sel >= GCOLS) sel -= GCOLS; }
    else if (k & KEY_DOWN)  { if (sel + GCOLS < g_n) sel += GCOLS; }
    else if (k & KEY_L) { do { filter = (filter + NFILTER - 1) % NFILTER; } while (filter == 5 + 9); build_species(filter, sort, search); sel = 0; }
    else if (k & KEY_R) { do { filter = (filter + 1) % NFILTER; } while (filter == 5 + 9); build_species(filter, sort, search); sel = 0; }
    else if (k & KEY_START) { filter_menu(&filter, &sort); build_species(filter, sort, search); sel = 0; }
    else if (k & KEY_SELECT) {
      char q[16];
      if (osk_search("SEARCH", search, q, sizeof(q))) { strcpy(search, q); build_species(filter, sort, search); sel = 0; }
    }
  }
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

  for (;;) {
    if (sel >= n) sel = n ? n - 1 : 0;
    int top = sel - vis / 2;
    if (top > n - vis) top = n - vis;
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

uint16_t pick_item(uint16_t current) { return list_pick("ITEM", 377, pk_item_name, item_icon_for, current, true, true); }
static const char* nature16(uint16_t n) { return pk_nature_name((uint8_t)n); }
uint8_t  pick_nature(uint8_t current)  { uint16_t r = list_pick("NATURE", 25, nature16, 0, current, false, false); return r == CANCEL ? current : (uint8_t)r; }
