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
#include "pkview_summary.h"
#include "pkview_app.h"

#define COLS 6
#define ROWS 5
#define CELL_W 26
#define CELL_H 27
#define GRID_X 80
#define GRID_Y 18
#define PANEL_W 76

static PkMon EWRAM_BSS g_box[30];
static PkMon EWRAM_BSS g_occ[30];

static void s_vsync(void) { VBlankIntrWait(); key_poll(); }

static u16 wallpaper_color(uint8_t wp) {
  const u16 pal[8] = {
    RGB15(12, 20, 10), RGB15(10, 16, 24), RGB15(22, 14, 10), RGB15(20, 10, 20),
    RGB15(10, 22, 22), RGB15(22, 20, 10), RGB15(16, 12, 22), RGB15(14, 18, 14),
  };
  return pal[wp & 7];
}

static const char* gender_str(uint8_t g) { return g == 0 ? " M" : g == 1 ? " F" : ""; }

static void draw_left(const PkMon* p) {
  ui_panel(0, 0, PANEL_W, 160, UI_PANEL, UI_BORDER);
  if (!p || p->species == 0) { ui_text(16, 72, UI_DIM, "(empty)"); return; }

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
  draw_left(&g_box[cur]);

  char bn[12];
  pk_box_name(pc, box, bn);
  int occ = 0;
  for (int s = 0; s < 30; s++) if (g_box[s].species) occ++;

  /* box-name banner with arrows + count */
  ui_panel(78, 2, 160, 14, UI_PANEL, UI_TITLE);
  ui_text(82, 5, UI_TEXT, "<");
  char b[24];
  siprintf(b, "BOX %d", box + 1);
  ui_text(106, 5, UI_TITLE, b);
  siprintf(b, "%d/30", occ);
  ui_text(180, 5, UI_DIM, b);
  ui_text(230, 5, UI_TEXT, ">");

  /* wallpaper + 32x32 icon grid (tightly packed, like the game) */
  ui_fill_rect(78, 18, 160, 134, wallpaper_color(pk_box_wallpaper(pc, box)));
  for (int r = 0; r < ROWS; r++) {
    for (int cc = 0; cc < COLS; cc++) {
      int s = r * COLS + cc;
      int x = GRID_X + cc * CELL_W, y = GRID_Y + r * CELL_H;
      if (g_box[s].species) ui_sprite(x, y, MON_ICON_W, MON_ICON_H, mon_icon_for(g_box[s].species));
      if (s == cur) m3_frame(x - 1, y - 1, x + CELL_W - 1, y + CELL_H - 2, UI_SELTEXT);
    }
  }

  ui_text(78, 153, UI_DIM, "A L/R:box SEL:party ST:card B");
}

int pkview_box(uint8_t* pc) {
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
    else if (k & KEY_START) return 2;
    else if (k & KEY_L) { box = (box + G3_TOTAL_BOXES - 1) % G3_TOTAL_BOXES; pk_read_box(pc, box, g_box); cur = 0; }
    else if (k & KEY_R) { box = (box + 1) % G3_TOTAL_BOXES; pk_read_box(pc, box, g_box); cur = 0; }
    else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
    else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
    else if (k & KEY_UP)    cur = (cur < COLS) ? cur + COLS * (ROWS - 1) : cur - COLS;
    else if (k & KEY_DOWN)  cur = (cur >= COLS * (ROWS - 1)) ? cur - COLS * (ROWS - 1) : cur + COLS;
    else if (k & KEY_A) {
      if (g_box[cur].species) {
        int act = app_action_menu(app_can_edit());
        if (act == 0) {
          int no = 0, ci = 0;
          for (int s = 0; s < 30; s++)
            if (g_box[s].species) { if (s == cur) ci = no; g_occ[no++] = g_box[s]; }
          pkview_summary(g_occ, no, ci);
        } else if (act == 1) {
          uint8_t* rec = pc + 0x0004 + ((uint32_t)box * 30 + cur) * 80;  /* PokemonStorage.boxes */
          if (app_edit_commit(rec, false, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc))
            pk_read_box(pc, box, g_box);                                  /* refresh after write */
        }
      }
    }
  }
}
