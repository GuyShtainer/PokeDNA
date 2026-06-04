/*
 * Game-faithful PC box screen for gba-pokeviewer (mimics the Gen-3 PC).
 * Left "PKMN DATA" panel (front sprite + name/No/Lv/gender/item) + a 6x5 grid of
 * 32x32 box-icon sprites on a colored wallpaper, with a ◄ box-name ► banner.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"            /* EWRAM_BSS (after tonc.h so u8 macro doesn't clash) */
#include "pkview_box.h"
#include "ui.h"
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "data_tables.h"
#include "mon_front.h"
#include "mon_icons.h"
#include "hand_cursor.h"
#include "pkview_summary.h"
#include "pkview_app.h"

#define COLS 6
#define ROWS 5
#define CELL_W 24
#define CELL_H 22
#define GRID_X 82
#define GRID_Y 30
#define PANEL_W 76
#define WP_X 78           /* wallpaper / grid region */
#define WP_Y 12
#define WP_W 162
#define WP_H 141          /* 12..153 */

/* The real Gen-3 PC-storage hand cursor (generated RGB15 blob, git-ignored). */
static void draw_hand(int x, int y) {
  for (int j = 0; j < HAND_H; j++)
    for (int i = 0; i < HAND_W; i++) {
      uint16_t p = hand_cursor[j * HAND_W + i];
      if (p & 0x8000) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
}

static PkMon EWRAM_BSS g_box[30];

static void s_vsync(void) { VBlankIntrWait(); key_poll(); }

/* ---- procedural Emerald-PC chrome (colors decoded from the real wallpapers) ---- */

/* a tiny 2-tone leaf sprig for the grass wallpaper */
static void draw_leaf(int x, int y, u16 dk, u16 lt) {
  m3_plot(x + 1, y,     dk);
  m3_plot(x + 2, y,     lt);
  m3_plot(x,     y + 1, dk);
  m3_plot(x + 1, y + 1, lt);
  m3_plot(x + 2, y + 1, dk);
  m3_plot(x + 1, y + 2, dk);
}

/* the forest/grass wallpaper: flat green field + an offset scatter of leaf sprigs */
static void draw_grass(int x, int y, int w, int h) {
  const u16 base = RGB15(19, 25, 12), dk = RGB15(13, 19, 6), lt = RGB15(22, 28, 15);
  ui_fill_rect(x, y, w, h, base);
  for (int j = 0; j + 6 < h; j += 12) {
    int off = ((j / 12) & 1) ? 8 : 0;
    for (int i = off; i + 4 < w; i += 16) draw_leaf(x + i + 2, y + j + 3, dk, lt);
  }
}

/* light checkerboard behind the front sprite (the PKMN DATA "monitor") */
static void draw_checker(int x, int y, int w, int h, u16 a, u16 b) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      m3_plot(x + i, y + j, (((i >> 3) ^ (j >> 3)) & 1) ? a : b);
}

/* solid 4x7 triangle arrows (banner ends) */
static void tri_left(int x, int y, u16 c) {
  for (int i = 0; i < 4; i++) for (int j = 3 - i; j <= 3 + i; j++) m3_plot(x + i, y + j, c);
}
static void tri_right(int x, int y, u16 c) {
  for (int i = 0; i < 4; i++) for (int j = 3 - i; j <= 3 + i; j++) m3_plot(x + (3 - i), y + j, c);
}

/* tan rounded box-name banner with arrows */
static void draw_banner(int x, int y, int w, const char* name) {
  const u16 fill = RGB15(30, 27, 14), hi = RGB15(31, 31, 24), sh = RGB15(24, 15, 2),
            bd = RGB15(14, 9, 0), ink = RGB15(8, 5, 0);
  ui_fill_rect(x, y, w, 14, fill);
  ui_hline(x, y, w, hi);
  ui_hline(x, y + 13, w, sh);
  m3_frame(x, y, x + w - 1, y + 13, bd);
  tri_left(x + 5, y + 4, ink);
  tri_right(x + w - 9, y + 4, ink);
  int tw = (int)strlen(name) * 8;
  int tx = x + (w - tw) / 2; if (tx < x + 12) tx = x + 12;
  ui_text(tx, y + 3, ink, name);
}

/* a top-bar tab (PKMN DATA / PARTY / CLOSE); active reads bright, inactive dim */
static void draw_tab(int x, int w, const char* label, bool active) {
  const u16 fill = active ? RGB15(7, 22, 27) : RGB15(3, 10, 14);
  const u16 ink  = active ? RGB15(29, 31, 31) : RGB15(12, 20, 24);
  ui_fill_rect(x, 0, w, 12, fill);
  ui_hline(x, 0, w, active ? RGB15(14, 28, 31) : RGB15(6, 16, 20));
  m3_frame(x, 0, x + w - 1, 11, RGB15(2, 8, 11));
  int tw = (int)strlen(label) * 8;
  int tx = x + (w - tw) / 2; if (tx < x + 2) tx = x + 2;
  ui_text(tx, 2, ink, label);
}

static const char* gender_str(uint8_t g) { return g == 0 ? " M" : g == 1 ? " F" : ""; }

static void draw_left(const PkMon* p) {
  ui_panel(0, 0, PANEL_W, 160, UI_PANEL, UI_BORDER);
  /* checkered "monitor" backdrop behind the front sprite, like the real PKMN DATA window */
  draw_checker(5, 15, 66, 66, RGB15(23, 23, 25), RGB15(28, 28, 30));
  m3_frame(4, 14, 71, 81, UI_BORDER);
  if (!p || p->species == 0) { ui_text(16, 92, UI_DIM, "(empty)"); return; }

  const uint16_t* spr = mon_front_for(p->species, p->isShiny);
  if (spr) ui_sprite(6, 16, MON_FRONT_W, MON_FRONT_H, spr);
  else     ui_sprite(22, 32, MON_ICON_W, MON_ICON_H, mon_icon_for(p->species));

  char buf[40];
  siprintf(buf, "No.%u", (unsigned)pk_national_no(p->species));
  ui_text(4, 86, UI_DIRCLR, buf);
  if (p->isShiny) ui_text(56, 86, UI_WARN, "*");
  char nm[24];
  ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 9);
  ui_text(4, 96, UI_TEXT, nm);
  siprintf(buf, "Lv%u%s", (unsigned)p->level, gender_str(p->gender));
  ui_text(4, 106, UI_TEXT, buf);
  char sp[24];
  ui_truncate(sp, pk_species_name(p->species), 9);
  ui_text(4, 116, UI_DIM, sp);
  ui_text(4, 130, UI_DIRCLR, "Item");
  char it[24];
  ui_truncate(it, p->heldItem ? pk_item_name(p->heldItem) : "-", 9);
  ui_text(4, 139, UI_TEXT, it);
}

static void render(const uint8_t* pc, int box, int cur) {
  ui_clear();

  /* top tab bar: PKMN DATA (active) | PARTY | CLOSE */
  draw_tab(0, PANEL_W + 1, "PKMN DATA", true);
  draw_tab(PANEL_W + 1, 92, "PARTY", false);
  draw_tab(PANEL_W + 93, UI_SCR_W - (PANEL_W + 93), "CLOSE", false);

  draw_left(&g_box[cur]);

  /* grass wallpaper behind the banner + grid */
  draw_grass(WP_X, WP_Y, WP_W, WP_H);

  char bn[12];
  pk_box_name(pc, box, bn);
  int occ = 0;
  for (int s = 0; s < 30; s++) if (g_box[s].species) occ++;
  draw_banner(WP_X + 2, 13, WP_W - 4, bn[0] ? bn : "BOX");

  /* 32x32 icon grid (tightly packed, like the game) */
  for (int r = 0; r < ROWS; r++) {
    for (int cc = 0; cc < COLS; cc++) {
      int s = r * COLS + cc;
      int x = GRID_X + cc * CELL_W, y = GRID_Y + r * CELL_H;
      if (g_box[s].species) ui_sprite(x, y, MON_ICON_W, MON_ICON_H, mon_icon_for(g_box[s].species));
    }
  }

  /* white Gen-3 hand floats just above the selected mon; clamp so the top row
   * doesn't poke into the banner. */
  int hx = GRID_X + (cur % COLS) * CELL_W + 3;
  int hy = GRID_Y + (cur / COLS) * CELL_H - 16;
  if (hy < 28) hy = 28;
  draw_hand(hx, hy);

  char hl[40]; siprintf(hl, "A:menu L/R:box ST:card  %d/30", occ);
  ui_text(WP_X + 2, 152, RGB15(31, 31, 31), hl);
}

int pkview_box(uint8_t* pc) {
  int box = pk_current_box(pc);
  if (box < 0 || box >= G3_TOTAL_BOXES) box = 0;
  int cur = 0;
  pk_read_box(pc, box, g_box);

  for (;;) {
    render(pc, box, cur);
    u16 k;
    do { s_vsync(); k = key_hit(KEY_FULL) | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);

    if (k & KEY_B) return 0;
    else if (k & KEY_SELECT) return 1;
    else if (k & KEY_START) return 2;
    else if (k & KEY_L) { box = (box + G3_TOTAL_BOXES - 1) % G3_TOTAL_BOXES; pk_read_box(pc, box, g_box); cur = 0; }
    else if (k & KEY_R) { box = (box + 1) % G3_TOTAL_BOXES; pk_read_box(pc, box, g_box); cur = 0; }
    else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
    else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
    else if (k & KEY_UP)    cur = (cur < COLS) ? cur + COLS * (ROWS - 1) : cur - COLS;
    else if (k & KEY_DOWN)  cur = (cur >= COLS * (ROWS - 1)) ? cur - COLS * (ROWS - 1) : cur + COLS;
    else if (k & KEY_A) {
      if (g_box[cur].species) {
        uint8_t* rec = pc + 0x0004 + ((uint32_t)box * 30 + cur) * 80;     /* PokemonStorage.boxes */
        if (app_mon_menu(rec, false, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc))
          pk_read_box(pc, box, g_box);                                    /* refresh after write */
      }
    }
  }
}
