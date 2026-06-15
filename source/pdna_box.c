/*
 * Game-faithful PC box screen for PokeDNA (mimics the Gen-3 PC).
 * Left "PKMN DATA" panel (front sprite + name/No/Lv/gender/item) + a 6x5 grid of
 * 32x32 box-icon sprites on a colored wallpaper, with a ◄ box-name ► banner.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"            /* EWRAM_BSS (after tonc.h so u8 macro doesn't clash) */
#include "pdna_box.h"
#include "ui.h"
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "data_tables.h"
#include "mon_front.h"
#include "mon_icons.h"
#include "hand_cursor.h"
#include "pdna_summary.h"
#include "pdna_app.h"
#include "snd.h"
#include "osk.h"

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

/* The real Gen-3 PC-storage hand cursor (generated RGB15 blobs, git-ignored):
 * hand_cursor = open pointing hand, hand_reach = mid grab, hand_grab = closed fist. */
static void blit_frame(int x, int y, const uint16_t* f, int fw, int fh) {
  for (int j = 0; j < fh; j++)
    for (int i = 0; i < fw; i++) {
      uint16_t p = f[j * fw + i];
      if (p & 0x8000) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
}
static void draw_hand(int x, int y) { blit_frame(x, y, hand_cursor, HAND_W, HAND_H); }
static void draw_grab(int x, int y) { blit_frame(x, y, hand_grab,   HAND_GRAB_W, HAND_GRAB_H); }

static PkMon EWRAM_BSS g_box[30];
static int s_move_from = -1;          /* slot being repositioned in move-mode, or -1 */

/* Swap two 80-byte records inside a 30-record block (the current box's records). */
static void swap_records(uint8_t* recs, int a, int b) {
  if (a == b) return;
  uint8_t* ra = recs + (uint32_t)a * 80;
  uint8_t* rb = recs + (uint32_t)b * 80;
  uint8_t tmp[80];
  memcpy(tmp, ra, 80); memcpy(ra, rb, 80); memcpy(rb, tmp, 80);
}

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

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

static const char* const WP_NAME[G3_BOX_WALLPAPER_COUNT] = {
  "Forest", "City", "Desert", "Savanna", "Crag", "Volcano", "Snow", "Cave",
  "Beach", "Seafloor", "River", "Sky", "Polkadot", "Pokecenter", "Machine", "Plain",
};
/* Emerald "Walda"/secret wallpapers (chooser ids 16..31). */
static const char* const WALDA_NAME[G3_WALDA_COUNT] = {
  "Zigzagoon", "Screen", "Horizontal", "Diagonal", "Block", "Ribbon", "Pokecenter2", "Frame",
  "Blank", "Circles", "Azumarill", "Pikachu", "Legendary", "Dusclops", "Ludicolo", "Whiscash",
};
static const char* wp_name(int id) { return id < 16 ? WP_NAME[id] : WALDA_NAME[id - 16]; }

/* Draw box wallpaper `wp` into the region. Falls back to the procedural grass for
 * any wallpaper without a real generated bitmap (see wallpaper_bmp). */
static void draw_wallpaper(int wp, int x, int y, int w, int h);

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

  const uint16_t* spr = mon_front_for_form(p->species, p->isShiny, p->form);
  if (spr) ui_sprite(6, 16, MON_FRONT_W, MON_FRONT_H, spr);
  else     ui_sprite(22, 32, MON_ICON_W, MON_ICON_H, mon_icon_for_form(p->species, p->form));

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

/* Box wallpaper: blit the real wallpaper for `wp` (deduped 8x8 RGB15 tiles +
 * a 20x18 tilemap) if present, else fall back to the procedural grass. The strong
 * accessors come from the generated wallpapers.c (git-ignored); these weak
 * fallbacks return NULL so the build works before the wallpapers are generated. */
const uint16_t* wallpaper_tile_data(int wp, int* ntiles);
const uint16_t* wallpaper_tilemap(int wp);
__attribute__((weak)) const uint16_t* wallpaper_tile_data(int wp, int* n) { (void)wp; if (n) *n = 0; return 0; }
__attribute__((weak)) const uint16_t* wallpaper_tilemap(int wp) { (void)wp; return 0; }

static void draw_wallpaper(int wp, int x, int y, int w, int h) {
  int nt; const uint16_t* tiles = wallpaper_tile_data(wp, &nt);
  const uint16_t* map = wallpaper_tilemap(wp);
  if (!tiles || !map) { draw_grass(x, y, w, h); return; }
  for (int ty = 0; ty < 18; ty++)
    for (int tx = 0; tx < 20; tx++) {
      const uint16_t* t = tiles + (uint32_t)map[ty * 20 + tx] * 64;
      int bx = x + tx * 8, by = y + ty * 8;
      for (int j = 0; j < 8 && by + j < y + h; j++)
        for (int i = 0; i < 8 && bx + i < x + w; i++)
          m3_plot(bx + i, by + j, t[j * 8 + i] & 0x7FFF);
    }
}

/* Repaint just the wallpaper pixels inside [cx,cy,cw,ch] (clipped to the box
 * region) — used to erase the floating hand without a full redraw. */
static void wallpaper_patch(int wp, int cx, int cy, int cw, int ch) {
  int nt; const uint16_t* tiles = wallpaper_tile_data(wp, &nt);
  const uint16_t* map = wallpaper_tilemap(wp);
  int x0 = cx < WP_X ? WP_X : cx, y0 = cy < WP_Y ? WP_Y : cy;
  int x1 = cx + cw, y1 = cy + ch;
  if (x1 > WP_X + WP_W) x1 = WP_X + WP_W;
  if (y1 > WP_Y + WP_H) y1 = WP_Y + WP_H;
  for (int py = y0; py < y1; py++)
    for (int px = x0; px < x1; px++) {
      int lx = px - WP_X, ly = py - WP_Y;
      u16 c = 0;
      if (tiles && map && lx < 160 && ly < 144) {
        const uint16_t* t = tiles + (uint32_t)map[(ly / 8) * 20 + (lx / 8)] * 64;
        c = t[(ly % 8) * 8 + (lx % 8)] & 0x7FFF;
      } else c = RGB15(19, 25, 12);   /* grass-ish fallback tone */
      m3_plot(px, py, c);
    }
}

/* 32x32 box icon, blitted in the icons' natural facing (the front sprite, party
 * list and bank all draw un-mirrored, so the PC grid matches them). 0x8000 = opaque. */
static void blit_icon(int x, int y, const u16* icon) {
  if (!icon) return;
  for (int j = 0; j < MON_ICON_H; j++)
    for (int i = 0; i < MON_ICON_W; i++) {
      u16 p = icon[j * MON_ICON_W + i];
      if (p & 0x8000) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
}

static void hand_xy(int cur, int* hx, int* hy) {
  *hx = GRID_X + (cur % COLS) * CELL_W + 3;
  *hy = GRID_Y + (cur / COLS) * CELL_H - 16;
  if (*hy < 28) *hy = 28;
}

/* Draw all 30 box icons, optionally skipping one slot (the one held in-hand during
 * move-mode, which is drawn riding the cursor instead). */
static void grid_icons_skip(int skip) {
  for (int s = 0; s < 30; s++)
    if (s != skip && g_box[s].species)
      blit_icon(GRID_X + (s % COLS) * CELL_W, GRID_Y + (s / COLS) * CELL_H, mon_icon_for_form(g_box[s].species, g_box[s].form));
}
static void grid_icons(void) { grid_icons_skip(-1); }

/* Repaint a rect: wallpaper pixels + any box icons overlapping it (skipping the
 * held slot). Used to erase a moving hand without a full redraw. */
static void redraw_region(int wp, int x, int y, int w, int h, int skip) {
  wallpaper_patch(wp, x, y, w, h);
  for (int s = 0; s < 30; s++) {
    if (s == skip || !g_box[s].species) continue;
    int ix = GRID_X + (s % COLS) * CELL_W, iy = GRID_Y + (s / COLS) * CELL_H;
    if (ix < x + w && ix + MON_ICON_W > x && iy < y + h && iy + MON_ICON_H > y)
      blit_icon(ix, iy, mon_icon_for_form(g_box[s].species, g_box[s].form));
  }
}

static void draw_footer(bool is_bank, bool on_title, bool moving) {
  const char* f = moving ? "Move: D-pad place  A drop  B cancel"
                : on_title ? "A rename/wallpaper  DOWN grid  L/R box  B close"
                : is_bank  ? "A menu  UP title  L/R box  B close"
                           : "A menu  UP title  SEL party  L/R box  B close";
  /* clear the footer strip first (it changes between modes) */
  ui_fill_rect(WP_X, 152, WP_W, 8, UI_BG);
  ui_text(WP_X + 2, 152, RGB15(31, 31, 31), f);
}

static void draw_cursor_hand(int cur, bool on_title) {
  if (on_title) draw_hand(WP_X + WP_W / 2 - 4, 14);
  else { int hx, hy; hand_xy(cur, &hx, &hy); draw_hand(hx, hy); }
}

/* Carry positions while move-mode holds a mon: the held icon (lifted) and the
 * closed grab fist GRIPPING it (sat on the icon's top, not hovering above it). */
static void carry_xy(int cur, int* ix, int* iy, int* fx, int* fy) {
  *ix = GRID_X + (cur % COLS) * CELL_W;
  *iy = GRID_Y + (cur / COLS) * CELL_H - 4;          /* lifted = "picked up" */
  if (*iy < WP_Y) *iy = WP_Y;
  *fx = *ix + (MON_ICON_W - HAND_GRAB_W) / 2;         /* fist centered over the icon */
  *fy = *iy - 6;                                      /* and gripping its top */
  if (*fy < WP_Y) *fy = WP_Y;
}
/* Bounding box covering both the carried icon and the grab fist (for partial erase). */
static void carry_bbox(int ix, int iy, int fx, int fy, int* x, int* y, int* w, int* h) {
  int x0 = ix < fx ? ix : fx, y0 = iy < fy ? iy : fy;
  int x1 = ix + MON_ICON_W; if (fx + HAND_GRAB_W > x1) x1 = fx + HAND_GRAB_W;
  int y1 = iy + MON_ICON_H; if (fy + HAND_GRAB_H > y1) y1 = fy + HAND_GRAB_H;
  *x = x0 - 1; *y = y0 - 1; *w = x1 - x0 + 2; *h = y1 - y0 + 2;
}

/* Full repaint — on entry, box change, after a menu/edit, or a mode change. */
static void render_full(BoxSource* src, int box, int cur, bool on_title, bool moving) {
  ui_clear();
  draw_tab(0, PANEL_W + 1, "PKMN DATA", true);
  draw_tab(PANEL_W + 1, 92, src->is_bank ? "(BANK)" : "PARTY SEL", false);
  draw_tab(PANEL_W + 93, UI_SCR_W - (PANEL_W + 93), "CLOSE B", false);
  draw_left(on_title ? 0 : &g_box[cur]);

  draw_wallpaper(src->get_wp(box), WP_X, WP_Y, WP_W, WP_H);

  char bn[12], bnocc[24];
  src->get_name(box, bn);
  int occ = 0;
  for (int s = 0; s < 30; s++) if (g_box[s].species) occ++;
  siprintf(bnocc, "%s  %d/30", bn[0] ? bn : "BOX", occ);
  draw_banner(WP_X + 2, 13, WP_W - 4, bnocc);
  if (on_title) m3_frame(WP_X, 12, WP_X + WP_W - 1, 27, UI_SELTEXT);

  if (moving && s_move_from >= 0) {                 /* carry: held mon rides the cursor */
    grid_icons_skip(s_move_from);                   /* its source slot reads empty */
    int ix, iy, fx, fy; carry_xy(cur, &ix, &iy, &fx, &fy);
    blit_icon(ix, iy, mon_icon_for_form(g_box[s_move_from].species, g_box[s_move_from].form));
    draw_grab(fx, fy);                               /* closed fist gripping it */
  } else {
    grid_icons();
    draw_cursor_hand(cur, on_title);
  }
  draw_footer(src->is_bank, on_title, moving);
}

/* Pick-up grab animation: at the source cell the open hand descends, spreads
 * (reach) and closes (grab) onto the mon, then lifts. A one-shot played the moment
 * MOVE is chosen, before the carry state takes over. */
static void play_grab_anim(BoxSource* src, int box, int slot) {
  int wp = src->get_wp(box);
  int cx = GRID_X + (slot % COLS) * CELL_W;
  int cy = GRID_Y + (slot / COLS) * CELL_H;
  /* (frame, w, h, x-nudge, y from cell top) — descend, close, then lift */
  const struct { const uint16_t* f; int w, h, dx, dy; } seq[] = {
    { hand_cursor, HAND_W,      HAND_H,      3, -18 },
    { hand_reach,  HAND_REACH_W, HAND_REACH_H, 0, -12 },
    { hand_grab,   HAND_GRAB_W,  HAND_GRAB_H,  3,  -4 },
    { hand_grab,   HAND_GRAB_W,  HAND_GRAB_H,  3, -12 },
  };
  for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
    /* erase the whole descent column + the mon, then redraw the mon and this frame */
    redraw_region(wp, cx - 4, cy - 20, MON_ICON_W + 8, MON_ICON_H + 24, -1);
    int hy = cy + seq[i].dy; if (hy < WP_Y) hy = WP_Y;
    blit_frame(cx + seq[i].dx, hy, seq[i].f, seq[i].w, seq[i].h);
    for (int v = 0; v < 3; v++) s_vsync();
  }
}

/* Light update on cursor move — repaint only the old hand area + the new hand +
 * the left PKMN-DATA panel, so browsing the PC never re-renders the whole box. */
static void move_cursor(BoxSource* src, int box, int old_cur, bool old_title,
                        int cur, bool on_title) {
  int wp = src->get_wp(box);
  /* erase the old hand */
  if (old_title) {
    wallpaper_patch(wp, WP_X, WP_Y, WP_W, 16);          /* banner area top strip */
    char bn[12], bnocc[24]; src->get_name(box, bn);
    int occ = 0; for (int s = 0; s < 30; s++) if (g_box[s].species) occ++;
    siprintf(bnocc, "%s  %d/30", bn[0] ? bn : "BOX", occ);
    draw_banner(WP_X + 2, 13, WP_W - 4, bnocc);
  } else {
    int hx, hy; hand_xy(old_cur, &hx, &hy);
    wallpaper_patch(wp, hx - 1, hy - 1, HAND_W + 3, HAND_H + 3);
    /* any icons overlapping the erased rect */
    for (int s = 0; s < 30; s++) {
      if (!g_box[s].species) continue;
      int ix = GRID_X + (s % COLS) * CELL_W, iy = GRID_Y + (s / COLS) * CELL_H;
      if (ix < hx + HAND_W + 2 && ix + MON_ICON_W > hx - 1 &&
          iy < hy + HAND_H + 2 && iy + MON_ICON_H > hy - 1)
        blit_icon(ix, iy, mon_icon_for_form(g_box[s].species, g_box[s].form));
    }
  }
  if (on_title) m3_frame(WP_X, 12, WP_X + WP_W - 1, 27, UI_SELTEXT);
  draw_cursor_hand(cur, on_title);
  draw_left(on_title ? 0 : &g_box[cur]);              /* the selected mon changed */
  if (on_title != old_title) draw_footer(src->is_bank, on_title, false);
}

/* Partial update while CARRYING a mon (move-mode): erase the old icon+fist and
 * draw them at the new cell — no ui_clear / full repaint, so move-mode doesn't
 * flicker. Mirrors move_cursor for the carry state. */
static void carry_move(BoxSource* src, int box, int old_cur, int cur) {
  int wp = src->get_wp(box);
  int ix, iy, fx, fy, bx, by, bw, bh;
  carry_xy(old_cur, &ix, &iy, &fx, &fy);
  carry_bbox(ix, iy, fx, fy, &bx, &by, &bw, &bh);
  redraw_region(wp, bx, by, bw, bh, s_move_from);     /* restore wallpaper + other icons */
  if (by < 28) {                                      /* erase reached the box-name banner -> repaint it */
    char bn[12], bnocc[24]; src->get_name(box, bn);
    int occ = 0; for (int s = 0; s < 30; s++) if (g_box[s].species) occ++;
    siprintf(bnocc, "%s  %d/30", bn[0] ? bn : "BOX", occ);
    draw_banner(WP_X + 2, 13, WP_W - 4, bnocc);
  }
  carry_xy(cur, &ix, &iy, &fx, &fy);
  blit_icon(ix, iy, mon_icon_for_form(g_box[s_move_from].species, g_box[s_move_from].form));
  draw_grab(fx, fy);
  draw_left(&g_box[cur]);                             /* panel follows the destination cell */
}

/* Wallpaper chooser: live-previews each wallpaper behind the box's icons.
 * 16 standard ids, plus the 16 Emerald "Walda"/secret wallpapers (ids 16..31) when
 * the source allows (PC Emerald, wp_count==32). LEFT/RIGHT cycle, A confirms, B cancels. */
static int wallpaper_pick(BoxSource* src, int cur_wp) {
  int count = src->wp_count > 0 ? src->wp_count : G3_BOX_WALLPAPER_COUNT;
  int wp = (cur_wp >= 0 && cur_wp < count) ? cur_wp : 0;
  for (;;) {
    ui_clear();
    draw_wallpaper(wp, WP_X, WP_Y, WP_W, WP_H);
    grid_icons();                                  /* the box's icons, for context */
    char b[40]; siprintf(b, "%d/%d  %s%s", wp + 1, count, wp_name(wp), wp >= 16 ? " (secret)" : "");
    ui_panel(50, 0, 140, 13, UI_PANEL, UI_BORDER);
    ui_text(56, 2, UI_TITLE, b);
    ui_text(2, 152, RGB15(31, 31, 31), "L/R wallpaper   A set   B cancel");
    u16 k; do { s_vsync(); k = key_hit(KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B); } while (!k);
    if (k & KEY_B) { snd_back(); return -1; }
    if (k & KEY_A) { snd_ok(); return wp; }
    if (k & (KEY_LEFT | KEY_L))  { snd_move(); wp = (wp > 0) ? wp - 1 : count - 1; }
    if (k & (KEY_RIGHT | KEY_R)) { snd_move(); wp = (wp + 1) % count; }
  }
}

/* Overlay menu when the box TITLE is selected: rename / change wallpaper. Each
 * edit mutates the source and commits via its verified-write path. */
static void box_options_menu(BoxSource* src, int box) {
  static const char* const OPT[3] = { "Rename box", "Wallpaper", "Cancel" };
  int sel = 0;
  for (;;) {
    const int mx = 70, my = 54, mw = 100, mh = 18 + 3 * 14 + 11;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "BOX");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < 3; i++) {
      int y = my + 18 + i * 14; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, OPT[i]);
    }
    ui_text(mx + 6, my + mh - 9, UI_DIM, "A pick  B back");
    u16 k; do { s_vsync(); k = key_hit(KEY_UP | KEY_DOWN | KEY_A | KEY_B); } while (!k);
    if (k & KEY_B) { snd_back(); return; }
    else if (k & KEY_UP)   { snd_move(); sel = (sel > 0) ? sel - 1 : 2; }
    else if (k & KEY_DOWN) { snd_move(); sel = (sel + 1) % 3; }
    else if (k & KEY_A) {
      snd_ok();
      if (sel == 0) {                              /* rename */
        char cur[12]; src->get_name(box, cur);
        char buf[12];
        if (osk_input("BOX NAME", cur[0] ? cur : "BOX", buf, 9)) {
          src->set_name(box, buf);
          src->commit();
        }
        return;
      } else if (sel == 1) {                       /* wallpaper */
        int wp = wallpaper_pick(src, src->get_wp(box));
        if (wp >= 0) {
          if (src->is_bank || wp < G3_BOX_WALLPAPER_FRIENDS) {  /* standard wallpaper */
            src->set_wp(box, wp);
            src->commit();
          } else {                                        /* Emerald Walda secret wallpaper (PC) */
            src->set_wp(box, G3_BOX_WALLPAPER_FRIENDS);
            app_set_walda((uint8_t)(wp - G3_BOX_WALLPAPER_FRIENDS));
            if (src->commit()) app_commit_sb1();          /* box byte + the Walda config */
          }
        }
        return;
      } else return;                               /* cancel */
    }
  }
}

int pdna_box(BoxSource* src) {
  int nb = src->nboxes; if (nb < 1) nb = 1;
  int box = src->start_box; if (box < 0 || box >= nb) box = 0;
  int cur = 0;
  bool on_title = false;
  bool need_full = true;
  s_move_from = -1;
  uint8_t* recs = src->records(box);          /* current box's 30*80 records */
  pk_decode_box_raw(recs, g_box);
  /* switch to box `nbx` (wrapping), reload + redraw */
  #define SWITCH_BOX(nbx) do { box = (nbx); recs = src->records(box); \
                               pk_decode_box_raw(recs, g_box); cur = 0; need_full = true; } while (0)

  for (;;) {
    if (need_full) { render_full(src, box, cur, on_title, s_move_from >= 0); need_full = false; }
    u16 k, fresh;
    do { s_vsync(); fresh = key_hit(KEY_FULL);
         k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
    /* fresh-press earcons (held d-pad repeats stay silent) */
    if      (fresh & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
    else if (fresh & (KEY_L | KEY_R | KEY_SELECT))               snd_tab();
    else if (fresh & (KEY_A | KEY_START))                        snd_ok();
    else if (fresh & KEY_B)                                      snd_back();

    int old_cur = cur; bool old_title = on_title;

    /* ---- MOVE MODE: holding a mon; reposition it within the box ---- */
    if (s_move_from >= 0) {
      if (k & KEY_B) { snd_back(); s_move_from = -1; need_full = true; }   /* cancel -> full redraw */
      else if (k & KEY_A) {                          /* drop -> swap source <-> cursor */
        swap_records(recs, s_move_from, cur);
        pk_decode_box_raw(recs, g_box);
        src->mark_dirty();                           /* PC: deferred to exit; bank: quiet write now */
        s_move_from = -1; need_full = true;          /* settled -> full redraw */
      }
      else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
      else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
      else if (k & KEY_UP)    { if (cur >= COLS) cur -= COLS; }
      else if (k & KEY_DOWN)  { if (cur < COLS * (ROWS - 1)) cur += COLS; }
      /* cursor move while carrying -> partial redraw (no ui_clear), so it doesn't flicker */
      if (!need_full && cur != old_cur) carry_move(src, box, old_cur, cur);
      continue;                                      /* move-mode swallows all other keys */
    }

    if (k & KEY_B) return 0;
    else if ((k & KEY_START) && !src->is_bank) return 2;
    else if (k & KEY_L) { SWITCH_BOX((box + nb - 1) % nb); }
    else if (k & KEY_R) { SWITCH_BOX((box + 1) % nb); }
    else if (on_title) {                           /* TITLE row: limited controls */
      if (k & KEY_DOWN) on_title = false;
      /* LEFT/RIGHT on the box name flips boxes, like the real Gen-3 PC (fresh
       * presses only, so holding doesn't machine-gun through boxes). */
      else if (fresh & KEY_LEFT)  { SWITCH_BOX((box + nb - 1) % nb); on_title = true; }
      else if (fresh & KEY_RIGHT) { SWITCH_BOX((box + 1) % nb); on_title = true; }
      else if (k & KEY_A) {
        if (src->can_edit()) { box_options_menu(src, box); need_full = true; }
        else { snd_deny(); }
      }
    }
    else if ((k & KEY_SELECT) && !src->is_bank) return 1;
    else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
    else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
    else if (k & KEY_UP)    { if (cur < COLS) on_title = true; else cur -= COLS; }
    else if (k & KEY_DOWN)  cur = (cur >= COLS * (ROWS - 1)) ? cur - COLS * (ROWS - 1) : cur + COLS;
    else if (k & KEY_A) {
      /* open the action menu on an occupied slot, OR on an empty slot when the
       * clipboard holds a mon to PASTE here. */
      if (g_box[cur].species || (src->can_edit() && app_clip_occupied())) {
        uint8_t* rec = recs + (uint32_t)cur * 80;
        int mbox = src->is_bank ? 0 : box;                               /* box index within menu_block */
        app_mon_menu(rec, false, src->commit, src->menu_block, mbox, cur);
        recs = src->records(box);                                        /* menu may have edited it */
        pk_decode_box_raw(recs, g_box);                                  /* refresh after possible write */
        if (app_take_move_request()) {                                   /* picked MOVE -> hold this slot */
          s_move_from = cur;
          render_full(src, box, cur, false, false);                      /* one clear: wipe the menu */
          play_grab_anim(src, box, cur);                                 /* real-PC grab animation */
          carry_move(src, box, cur, cur);                                /* lift into carry (no 2nd clear) */
          draw_footer(src->is_bank, false, true);                        /* move-mode footer */
        } else {
          need_full = true;                                              /* menu may have edited -> redraw */
        }
      }
    }

    /* cursor-only change -> light partial repaint; everything else did a full one */
    if (!need_full && (cur != old_cur || on_title != old_title))
      move_cursor(src, box, old_cur, old_title, cur, on_title);
  }
  #undef SWITCH_BOX
}
