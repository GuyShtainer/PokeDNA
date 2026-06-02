/*
 * Game-faithful PC box screen for gba-pokeviewer (mimics the Gen-3 PC).
 * Left "PKMN DATA" panel (front sprite + name/No/Lv/gender/item) + a 6x5 grid of
 * box-icon sprites on a colored wallpaper, with a ◄ box-name ► banner.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"            /* EWRAM_BSS (after tonc.h so u8 macro doesn't clash) */
#include "pkview_box.h"
#include "ui.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "data_tables.h"
#include "mon_front.h"
#include "mon_icons.h"
#include "pkview_summary.h"

#define COLS 6
#define ROWS 5
#define CELL_W 22
#define CELL_H 20
#define GRID_X 100
#define GRID_Y 44

static PkMon EWRAM_BSS g_box[30];
static PkMon EWRAM_BSS g_occ[30];

static void s_vsync(void) { VBlankIntrWait(); key_poll(); }

/* a few wallpaper tints keyed off the box's wallpaper id */
static u16 wallpaper_color(uint8_t wp) {
  const u16 pal[8] = {
    RGB15(12, 20, 10), RGB15(10, 16, 24), RGB15(22, 14, 10), RGB15(20, 10, 20),
    RGB15(10, 22, 22), RGB15(22, 20, 10), RGB15(16, 12, 22), RGB15(14, 18, 14),
  };
  return pal[wp & 7];
}

static const char* gender_str(uint8_t g) { return g == 0 ? " M" : g == 1 ? " F" : ""; }

static void draw_left(const PkMon* p) {
  ui_panel(0, 11, 92, 139, UI_PANEL, UI_BORDER);
  ui_text(6, 14, UI_DIRCLR, "PKMN DATA");
  if (!p || p->species == 0) {
    ui_text(20, 70, UI_DIM, "(empty)");
    return;
  }
  const uint16_t* spr = mon_front_for(p->species, p->isShiny);
  if (spr) ui_sprite(14, 24, MON_FRONT_W, MON_FRONT_H, spr);
  else     ui_icon16(38, 48, mon_icon_for(p->species));

  char buf[40];
  siprintf(buf, "No.%u", (unsigned)pk_national_no(p->species));
  ui_text(6, 92, UI_DIRCLR, buf);
  if (p->isShiny) ui_text(64, 92, UI_WARN, "*");
  char nm[24];
  ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 11);
  ui_text(6, 102, UI_TEXT, nm);
  siprintf(buf, "Lv%u%s", (unsigned)p->level, gender_str(p->gender));
  ui_text(6, 112, UI_TEXT, buf);
  char sp[24];
  ui_truncate(sp, pk_species_name(p->species), 11);
  ui_text(6, 122, UI_DIM, sp);
  ui_text(6, 134, UI_DIRCLR, "Item");
  ui_text(36, 134, UI_TEXT, p->heldItem ? pk_item_name(p->heldItem) : "-");
}

static void render(const uint8_t* pc, int box, int cur) {
  ui_clear();
  ui_text(4, 2, UI_TITLE, "PC BOXES");
  ui_hline(0, 10, UI_SCR_W, UI_BORDER);

  draw_left(&g_box[cur]);

  /* box-name banner with arrows + count */
  char bn[12];
  pk_box_name(pc, box, bn);
  int occ = 0;
  for (int s = 0; s < 30; s++) if (g_box[s].species) occ++;
  ui_panel(96, 14, 142, 22, UI_PANEL, UI_TITLE);
  ui_text(100, 21, UI_TEXT, "<");
  char banner[24];
  siprintf(banner, "BOX %d", box + 1);
  ui_text(120, 17, UI_TITLE, banner);
  ui_text(120, 26, UI_DIM, bn[0] ? bn : "");
  ui_text(230, 21, UI_TEXT, ">");
  siprintf(banner, "%d/30", occ);
  ui_text(196, 26, UI_DIM, banner);

  /* wallpaper + grid */
  ui_fill_rect(96, 40, 142, ROWS * CELL_H + 4, wallpaper_color(pk_box_wallpaper(pc, box)));
  for (int r = 0; r < ROWS; r++) {
    for (int cc = 0; cc < COLS; cc++) {
      int s = r * COLS + cc;
      int x = GRID_X + cc * CELL_W, y = GRID_Y + r * CELL_H;
      if (g_box[s].species) ui_icon16(x, y, mon_icon_for(g_box[s].species));
      if (s == cur) m3_frame(x - 1, y - 1, x + 16, y + 16, UI_SELTEXT);
    }
  }

  ui_hline(0, 151, UI_SCR_W, UI_BORDER);
  ui_text(4, 152, UI_DIM, "A view  L/R box  SEL party  B back");
}

int pkview_box(const uint8_t* pc) {
  int box = pk_current_box(pc);
  if (box < 0 || box >= G3_TOTAL_BOXES) box = 0;
  int cur = 0;
  pk_read_box(pc, box, g_box);

  for (;;) {
    render(pc, box, cur);
    u16 k;
    do { s_vsync(); k = key_hit(KEY_FULL); } while (!k);

    if (k & KEY_B) return 0;
    else if (k & KEY_SELECT) return 1;
    else if (k & KEY_L) { box = (box + G3_TOTAL_BOXES - 1) % G3_TOTAL_BOXES; pk_read_box(pc, box, g_box); cur = 0; }
    else if (k & KEY_R) { box = (box + 1) % G3_TOTAL_BOXES; pk_read_box(pc, box, g_box); cur = 0; }
    else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
    else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
    else if (k & KEY_UP)    cur = (cur < COLS) ? cur + COLS * (ROWS - 1) : cur - COLS;
    else if (k & KEY_DOWN)  cur = (cur >= COLS * (ROWS - 1)) ? cur - COLS * (ROWS - 1) : cur + COLS;
    else if (k & KEY_A) {
      if (g_box[cur].species) {
        int no = 0, ci = 0;
        for (int s = 0; s < 30; s++)
          if (g_box[s].species) { if (s == cur) ci = no; g_occ[no++] = g_box[s]; }
        pkview_summary(g_occ, no, ci);
      }
    }
  }
}
