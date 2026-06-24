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
#include "gen3_edit.h"      /* em_set_item: held-item moves in ITEM cursor mode */
#include "data_tables.h"
#include "mon_front.h"
#include "mon_icons.h"
#include "item_icons.h"     /* item_icon_for: held-item markers in ITEM mode */
#include "hand_cursor.h"
#include "box_oam.h"        /* hardware-OAM icon/cursor/carry/marker rendering */
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

/* SELECT cursor modes: 0 normal hand, 1 MOVE (orange hand, A grabs a mon directly),
 * 2 ITEM (translucent hand, mons show held items, A picks one up / drops / swaps).
 * The hand's normal/orange/translucent looks are produced by box_oam.c (a palette
 * swap + OBJ alpha-blend), not a per-pixel software blit. */
#define CM_NORMAL 0
#define CM_MOVE   1
#define CM_ITEM   2
static int s_cur_mode = CM_NORMAL;
static int s_item_held = 0;           /* item id carried in ITEM mode (0 = none) */
static int s_tab_focus = -1;          /* top-tab cursor: -1 none, 0 PKMN DATA, 1 PARTY SEL, 2 SAVE */
static int s_item_from = -1;          /* slot the carried item was taken from */
static int s_item_from_box = -1;      /* box the carried item came from (for put-back across boxes) */

static PkMon EWRAM_BSS g_box[30];
/* Mon-in-hand carry (move mode). The carried mon lives in s_held (a copy); its ORIGIN
 * cell keeps the real record (lift-don't-clear) and is only cleared on a successful drop,
 * so an interrupted carry never loses the mon. s_orig_slot<0 means "no origin": either a
 * fresh DUPLICATE (s_held_dup -> cancel discards it) or a real mon displaced by a swap
 * (s_held_dup=false -> cancel must place it in a free slot; can't cross save scopes).
 * Statics persist across pdna_box runs so a carry survives the PC<->Bank screen hand-off;
 * pdna_box_clear_carry() resets it per save. */
static bool s_holding = false;
static uint8_t s_held[80];
static int  s_orig_box = -1, s_orig_slot = -1;
static bool s_orig_bank = false;
static bool s_held_dup = false;   /* the held mon is a fresh, discardable duplicate */
void pdna_box_clear_carry(void) { s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_bank = false; s_held_dup = false; }

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
  /* shorter name when an item icon is shown so the 24x24 icon (right) doesn't clip it */
  ui_truncate(it, p->heldItem ? pk_item_name(p->heldItem) : "-", p->heldItem ? 5 : 9);
  ui_text(4, 139, UI_TEXT, it);
  if (p->heldItem) {                                  /* the real item icon (already in ROM) */
    const uint16_t* iic = item_icon_for(p->heldItem);
    if (iic) ui_sprite(PANEL_W - 26, 126, ITEM_ICON_W, ITEM_ICON_H, iic);
  }
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

/* --- Icons, cursor, carry, and item markers are HARDWARE OBJ sprites (box_oam.c).
 * The GPU composites all 30 icons + the overlays above the BG bitmap every frame,
 * so the bob is a free in-vblank OAM nudge (all icons together, no flicker) and the
 * cursor never stalls. Only the wallpaper / banner / panel / tabs / footer below are
 * software Mode-3. Helpers below push OAM state from the box loop's existing render
 * points; boxoam_commit() flushes the OAM shadow in the vblank tick (s_vsync). --- */

/* one-shot flag: re-upload the box's icon tiles into OBJ VRAM on the next paint */
static bool s_oam_reload = true;

/* the cursor look matching the active cursor mode */
static int cursor_look(void) {
  return s_cur_mode == CM_MOVE ? BOXOAM_HAND_MOVE
       : s_cur_mode == CM_ITEM ? BOXOAM_HAND_ITEM
                               : BOXOAM_HAND_NORMAL;
}

/* ---- mon-in-hand carry helpers (data-safety notes on the s_held declaration) ---- */
static void start_carry(BoxSource* src, const uint8_t* recs, int box, int slot) {
  memcpy(s_held, recs + (uint32_t)slot * 80, 80);            /* lift-don't-clear: copy, origin stays */
  s_holding = true; s_orig_box = box; s_orig_slot = slot; s_orig_bank = src->is_bank;
  s_held_dup = false;                                         /* a real mon (origin keeps it) */
}

/* Clear the origin cell after a within-scope drop; handles bank paging and returns the
 * CURRENT box reloaded into recs. Call AFTER placing the held mon at the dest (dest-first
 * ordering -> a mid-op power loss duplicates, never loses). */
static uint8_t* clear_origin(BoxSource* src, int box) {
  if (s_orig_slot < 0 || s_orig_bank != src->is_bank) { s_orig_slot = -1; return src->records(box); }
  uint8_t* o = src->records(s_orig_box);                     /* bank: flushes the current (dest) box first */
  memset(o + (uint32_t)s_orig_slot * 80, 0, 80);
  src->mark_dirty(); s_orig_slot = -1;
  return src->records(box);                                  /* reload the current box */
}

/* Drop the held mon onto cursor cell `cur`. Within the origin's scope: true move (place +
 * clear origin; swap if occupied). Across the PC<->Bank boundary: COPY onto an empty cell
 * only (origin kept) so a mon can't be lost between two save scopes. *done=true when the
 * hand is empty afterwards. Returns the (maybe reloaded) recs. */
static uint8_t* drop_held(BoxSource* src, int box, int cur, uint8_t* recs, bool* done) {
  *done = false;
  if (s_orig_slot >= 0 && s_orig_bank == src->is_bank && s_orig_box == box && cur == s_orig_slot) {
    s_holding = false; *done = true; return recs;            /* dropped back on its own cell */
  }
  bool occupied = g_box[cur].species != 0;
  if (s_orig_bank != src->is_bank) {                         /* cross-scope -> COPY (never lose) */
    if (occupied) { snd_deny(); return recs; }
    /* Across the PC<->Bank boundary a true move can't be made loss-proof (two save
     * scopes, two prompts), so this is a COPY — confirm it so the user isn't surprised
     * by a duplicate (they delete the original to finish a move). */
    boxoam_suspend();
    bool ok = app_confirm(src->is_bank ? "Copy to Bank?" : "Copy to PC?",
                          src->is_bank ? "The PC keeps the original." : "The Bank keeps the original.");
    boxoam_resume();
    if (!ok) return recs;                                    /* keep holding */
    memcpy(recs + (uint32_t)cur * 80, s_held, 80); src->mark_dirty();
    s_holding = false; *done = true; return recs;
  }
  if (!occupied) {                                           /* empty -> place, clear origin */
    memcpy(recs + (uint32_t)cur * 80, s_held, 80); src->mark_dirty();
    recs = clear_origin(src, box);
    s_holding = false; *done = true; return recs;
  }
  /* occupied within scope -> SWAP, then KEEP HOLDING the displaced occupant (place it
   * yourself next; we don't auto-throw it into the held mon's old cell). */
  if (s_orig_slot < 0 && s_held_dup) { snd_deny(); return recs; }          /* a fresh dup can't swap */
  if (s_orig_slot >= 0 && s_orig_box != box && src->is_bank) { snd_deny(); return recs; }  /* bank cross-box swap unsafe */
  uint8_t occ[80]; memcpy(occ, recs + (uint32_t)cur * 80, 80);             /* save the occupant */
  memcpy(recs + (uint32_t)cur * 80, s_held, 80);                           /* place the held mon at the cursor */
  src->mark_dirty();
  recs = clear_origin(src, box);                                           /* free the held mon's old cell */
  memcpy(s_held, occ, 80);                                                  /* now carry the displaced occupant */
  s_orig_slot = -1; s_held_dup = false;            /* RAM-only real mon: place it; never crosses save scopes */
  return recs;                                      /* *done stays false: still holding */
}

/* Push the full sprite state for the current frame: icons (reloaded if needed),
 * cursor / carry, and ITEM markers + carried item. */
static void oam_sync(int cur, bool on_title, int box, bool is_bank) {
  if (s_oam_reload) { boxoam_load_box(g_box); s_oam_reload = false; }
  if (s_holding) {
    PkMon hm; pk_decode_mon(s_held, false, &hm);
    boxoam_carry_held(cur, hm.species, hm.form);             /* held mon front-most + orange fist */
    if (s_orig_slot >= 0 && s_orig_bank == is_bank && s_orig_box == box)
      boxoam_hide_slot(s_orig_slot);                         /* lift-hide the origin cell */
    boxoam_item_markers(g_box, false);
    boxoam_carry_item(cur, 0, false);
  } else if (s_cur_mode == CM_ITEM && s_item_held) {
    boxoam_carry_end();                                      /* ITEM GRAB: full item + orange fist */
    boxoam_item_markers(g_box, true);
    boxoam_carry_item(cur, (uint16_t)s_item_held, true);
  } else {
    boxoam_carry_end();
    boxoam_item_markers(g_box, s_cur_mode == CM_ITEM);
    if (s_cur_mode == CM_ITEM && g_box[cur].heldItem)
      boxoam_carry_item(cur, g_box[cur].heldItem, false);    /* HOVER: small item bottom-left */
    else
      boxoam_carry_item(cur, 0, false);
    boxoam_cursor(cur, on_title, cursor_look());             /* cursor hand last; restores region A */
  }
}

static void draw_footer(bool is_bank, bool on_title, bool moving) {
  const char* f;
  if (s_tab_focus >= 0)    f = "L/R tab  A pick  DOWN back";
  else if (moving)         f = "Move  A drop  B cancel";
  else if (s_item_held)    f = "Item  A give  B putback";
  else if (on_title)       f = "A edit  UP tabs  DOWN grid  L/R box";
  else if (s_cur_mode == CM_MOVE) f = "MOVE  A grab  SEL mode  B";
  else if (s_cur_mode == CM_ITEM) f = "ITEM  A take  SEL mode  B";
  else                     f = is_bank ? "A menu  SEL mode  L/R box  B"
                                       : "A menu  UP title  SEL mode  B";
  /* clear the footer strip first (it changes between modes) */
  ui_fill_rect(WP_X, 152, WP_W, 8, UI_BG);
  ui_text(WP_X + 2, 152, RGB15(31, 31, 31), f);
}

/* Set a box mon's held item (decrypt -> set -> re-encode + checksum, in place). */
static void box_set_held(uint8_t* recs, int slot, uint16_t item) {
  uint8_t* rec = recs + (uint32_t)slot * 80;
  EditMon e; gen3_edit_load(rec, false, &e);
  em_set_item(&e, item);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, 80);
}

/* A safe home slot to deposit the carried item: its source slot if still empty-
 * handed, else the first occupied mon with no item; -1 if nowhere (never lose it). */
static int item_home(void) {
  if (s_item_from >= 0 && g_box[s_item_from].species && !g_box[s_item_from].heldItem) return s_item_from;
  for (int s = 0; s < 30; s++) if (g_box[s].species && !g_box[s].heldItem) return s;
  return -1;
}

/* Repaint the box-name banner + occupancy + the on-title selection frame (BG, software). */
static void draw_box_banner(BoxSource* src, int box, bool on_title) {
  char bn[12], bnocc[24];
  src->get_name(box, bn);
  int occ = 0;
  for (int s = 0; s < 30; s++) if (g_box[s].species) occ++;
  siprintf(bnocc, "%s  %d/30", bn[0] ? bn : "BOX", occ);
  draw_banner(WP_X + 2, 13, WP_W - 4, bnocc);
  if (on_title) m3_frame(WP_X, 12, WP_X + WP_W - 1, 27, UI_SELTEXT);
}

/* Full BG repaint (tabs + left panel + wallpaper + banner + footer). The icons,
 * cursor, carry and item markers are SPRITES (oam_sync), composited above this BG
 * by the GPU — so `moving` only affects the footer/sprite state, not the BG. With
 * clear=false it repaints OVER the current screen (no black flash) to wipe a modal. */
static void render_full(BoxSource* src, int box, int cur, bool on_title, bool moving, bool clear) {
  if (clear) ui_clear();
  draw_tab(0, PANEL_W + 1, "PKMN DATA", s_tab_focus < 0 || s_tab_focus == 0);
  draw_tab(PANEL_W + 1, 92, src->is_bank ? "(BANK)" : "PARTY SEL", s_tab_focus == 1);
  draw_tab(PANEL_W + 93, UI_SCR_W - (PANEL_W + 93), "SAVE", s_tab_focus == 2);
  draw_left(on_title ? 0 : &g_box[cur]);

  draw_wallpaper(src->get_wp(box), WP_X, WP_Y, WP_W, WP_H);
  draw_box_banner(src, box, on_title);
  draw_footer(src->is_bank, on_title, moving);

  oam_sync(cur, on_title, box, src->is_bank);        /* icons + cursor + carry + markers */
}

/* Pick-up grab cue when MOVE is chosen: the carried icon + grab fist lift over a few
 * frames. Pure OAM (the carry sprites already exist via oam_sync) — no software blit,
 * no flicker. The cursor hand is already hidden (carry state). */
static void play_grab_anim(BoxSource* src, int box, int slot) {
  (void)src; (void)box;
  /* boxoam_carry already lifts the icon 4px; nudge a touch more for a "grab" beat */
  for (int v = 0; v < 6; v++) { boxoam_commit(); s_vsync(); }
  (void)slot;
}

/* Light update on cursor move: with hardware sprites the icons composite themselves,
 * so this only repositions the cursor sprite (in oam_sync), refreshes the left PKMN-
 * DATA panel (the selected mon changed) and the on-title banner/footer/frame. No
 * software icon repaint, no erase — the GPU handles overlap. */
static void move_cursor(BoxSource* src, int box, int old_cur, bool old_title,
                        int cur, bool on_title) {
  (void)old_cur;
  if (on_title != old_title) {                        /* entering/leaving the title row */
    draw_wallpaper(src->get_wp(box), WP_X, WP_Y, WP_W, WP_H);  /* clear stale title frame */
    draw_box_banner(src, box, on_title);
    draw_footer(src->is_bank, on_title, false);
  }
  draw_left(on_title ? 0 : &g_box[cur]);              /* the selected mon changed */
  oam_sync(cur, on_title, box, src->is_bank);         /* reposition cursor sprite */
}

/* Update while CARRYING a mon (move-mode): just reposition the carry sprites and the
 * left panel. The GPU composites; nothing to erase. */
static void carry_move(BoxSource* src, int box, int old_cur, int cur) {
  (void)old_cur;
  draw_left(&g_box[cur]);                             /* panel follows the destination cell */
  oam_sync(cur, false, box, src->is_bank);           /* moves OE_CARRY + OE_GRAB sprites */
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
    /* the box's icons stay composited as OBJ sprites above this preview BG */
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

/* Idle icon bob: every ANIM_PERIOD frames toggle a 1px unison Y-offset on ALL icon
 * sprites in a single in-vblank OAM write (boxoam_set_bob). Every icon moves together,
 * the GPU composites the rest, ~0 CPU, and the cursor never blocks — so this is free and
 * flicker-free (the hardware-OBJ path, now that the BG2-priority bug is fixed). Gated on
 * app_anim_enabled(); suspended while move-carrying and in ITEM mode (markers stay put). */
#define ANIM_PERIOD 30                    /* vblanks per bob toggle (~0.5s, Gen-3 cadence) */

int pdna_box(BoxSource* src) {
  int nb = src->nboxes; if (nb < 1) nb = 1;
  int box = src->start_box; if (box < 0 || box >= nb) box = 0;
  int cur = 0;
  bool on_title = false;
  bool need_full = true;
  int anim_ctr = 0, bob = 0;                   /* current unison Y-bob offset (0/1) */
  /* s_holding persists across pdna_box runs so a carried mon survives the PC<->Bank
   * hand-off (the receiving screen just keeps drawing it). It's reset per-save by
   * pdna_box_clear_carry() — do NOT reset it here. */
  s_cur_mode = CM_NORMAL; s_item_held = 0; s_item_from = -1; s_item_from_box = -1;   /* fresh cursor mode each open */
  s_tab_focus = -1;
  /* A Day-Care withdraw-to-PC parked a mon in a free slot and asked us to carry it:
   * open that box and lift the parked mon into the glove so the user places it. */
  int pickup_ps = -1;
  if (!s_holding) { int pb, ps;
    if (app_take_pickup(&pb, &ps) && pb >= 0 && pb < nb && ps >= 0 && ps < 30) { box = pb; cur = ps; pickup_ps = ps; }
  }
  boxoam_enter();                             /* enable OBJ; upload hand/grab + palettes */
  s_oam_reload = true;                        /* first paint uploads the box's icon tiles */
  uint8_t* recs = src->records(box);          /* current box's 30*80 records */
  pk_decode_box_raw(recs, g_box);
  if (pickup_ps >= 0) start_carry(src, recs, box, pickup_ps);   /* lift the parked mon */
  /* Cursor-arrival hint when crossing the PC<->Bank edge: bottom row (carrying up into the
   * bank) or the top tabs (only when NOT carrying — you can't rest a held mon on a tab). */
  { int st = app_box_start_take();
    if (st == 1 && !s_holding) s_tab_focus = src->is_bank ? 2 : 1;
    else if (st == 2) cur = COLS * (ROWS - 1);
  }
  /* switch to box `nbx` (wrapping), reload + redraw */
  #define SWITCH_BOX(nbx) do { box = (nbx); recs = src->records(box); \
                               pk_decode_box_raw(recs, g_box); cur = 0; \
                               bob = 0; anim_ctr = 0; \
                               if (!src->is_bank) app_note_pc_box(box); \
                               s_oam_reload = true; need_full = true; } while (0)

  for (;;) {
    if (need_full) { render_full(src, box, cur, on_title, s_holding, true); need_full = false; }
    u16 k, fresh;
    do { s_vsync();
         /* real 2-frame pose bob: DMA-swap all icons' tiles between frame 0/1 in vblank;
          * paused while carrying or in ITEM mode (icons + markers stay static). */
         if (app_anim_enabled(ANIM_BOX) && !s_holding && s_cur_mode != CM_ITEM) {
           if (++anim_ctr >= ANIM_PERIOD) { anim_ctr = 0; bob ^= 1; boxoam_set_frame(bob); }
         } else if (bob) { bob = 0; boxoam_set_frame(0); }
         boxoam_commit();                       /* flush the OAM shadow in the vblank window */
         fresh = key_hit(KEY_FULL);
         k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
    /* fresh-press earcons (held d-pad repeats stay silent) */
    if      (fresh & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
    else if (fresh & (KEY_L | KEY_R | KEY_SELECT))               snd_tab();
    else if (fresh & (KEY_A | KEY_START))                        snd_ok();
    else if (fresh & KEY_B)                                      snd_back();

    int old_cur = cur; bool old_title = on_title;

    /* ---- MOVE MODE (mon-in-hand): the carried mon floats; place it anywhere ---- */
    if (s_holding) {
      bool homeless = (s_orig_slot < 0 && !s_held_dup);   /* a real mon displaced by a swap (RAM-only) */
      if (k & KEY_B) {                               /* cancel */
        if (homeless) {                              /* must place it somewhere -> first free in this box */
          int fs = -1; for (int s = 0; s < COLS * ROWS; s++) if (!g_box[s].species) { fs = s; break; }
          if (fs < 0) { snd_deny(); }                /* box full: keep holding */
          else { memcpy(recs + (uint32_t)fs * 80, s_held, 80); src->mark_dirty();
                 snd_back(); s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_bank = false; s_held_dup = false;
                 pk_decode_box_raw(recs, g_box); s_oam_reload = true; need_full = true; }
        } else {                                     /* origin keeps it / a dup is discarded */
          snd_back(); s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_bank = false; s_held_dup = false;
          s_oam_reload = true; need_full = true;
        }
      }
      else if (k & KEY_A) {                          /* drop / swap onto the cursor cell */
        bool done; recs = drop_held(src, box, cur, recs, &done);
        (void)done;                                  /* a cross-scope copy may have shown a confirm dialog */
        pk_decode_box_raw(recs, g_box); s_oam_reload = true; need_full = true;
      }
      else if ((k & (KEY_L | KEY_R)) && nb > 1) {    /* carry to the next/prev box (even a FULL one) */
        int nbx = (k & KEY_R) ? (box + 1) % nb : (box + nb - 1) % nb;
        SWITCH_BOX(nbx);                             /* held mon floats along; no slot needed */
      }
      else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
      else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
      else if (k & KEY_UP)    {
        if (cur >= COLS) cur -= COLS;
        else if (homeless) snd_deny();                          /* place the swapped mon before leaving */
        else if (!src->is_bank) { boxoam_exit(); return 4; }    /* off PC top -> Bank, still holding */
      }
      else if (k & KEY_DOWN)  {
        if (cur < COLS * (ROWS - 1)) cur += COLS;
        else if (homeless) snd_deny();                          /* place the swapped mon before leaving */
        else if (src->is_bank) { boxoam_exit(); return 5; }     /* off Bank bottom -> PC, still holding */
      }

      /* cursor move while carrying -> partial redraw (no ui_clear), so it doesn't flicker */
      if (!need_full && cur != old_cur) carry_move(src, box, old_cur, cur);
      continue;                                      /* move-mode swallows all other keys */
    }

    /* ---- ITEM CARRY: holding a held item; place it / swap onto another mon ---- */
    if (s_item_held > 0) {
      if (k & KEY_B) {                               /* put it back (never lose it) */
        if (s_item_from_box >= 0 && s_item_from_box != box) SWITCH_BOX(s_item_from_box);  /* back to its box */
        int home = item_home();
        if (home >= 0) { box_set_held(recs, home, (uint16_t)s_item_held); pk_decode_box_raw(recs, g_box);
                         src->mark_dirty(); s_item_held = 0; s_item_from = -1; s_item_from_box = -1; need_full = true; }
        else snd_deny();
      }
      else if (k & KEY_A) {                          /* give / swap onto the cursor mon */
        if (g_box[cur].species) {
          uint16_t old = g_box[cur].heldItem;        /* swap: take this mon's old item */
          box_set_held(recs, cur, (uint16_t)s_item_held);
          pk_decode_box_raw(recs, g_box);
          src->mark_dirty();
          s_item_held = old; s_item_from = old ? cur : -1;   /* keep holding the swapped-out item */
          s_item_from_box = old ? box : -1;
          need_full = true;
        } else snd_deny();
      }
      else if ((k & KEY_L) && nb > 1) { SWITCH_BOX((box + nb - 1) % nb); }   /* flip boxes while carrying */
      else if ((k & KEY_R) && nb > 1) { SWITCH_BOX((box + 1) % nb); }
      else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
      else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
      else if (k & KEY_UP)    { if (cur >= COLS) cur -= COLS; }
      else if (k & KEY_DOWN)  { if (cur < COLS * (ROWS - 1)) cur += COLS; }
      /* carried item + cursor are sprites: just reposition them (no BG repaint) */
      if (!need_full && cur != old_cur) { draw_left(&g_box[cur]); oam_sync(cur, false, box, src->is_bank); }
      continue;                                      /* item-carry swallows all other keys */
    }

    /* ---- TOP-TAB cursor (reached by pressing UP on the box name): pick a tab ---- */
    if (s_tab_focus >= 0) {
      if      (k & KEY_UP) { if (!src->is_bank) { s_tab_focus = -1; boxoam_exit(); return 4; } }   /* up past the PC tabs -> Bank */
      else if (k & (KEY_B | KEY_DOWN)) { s_tab_focus = -1; need_full = true; }              /* back to box name */
      else if (k & KEY_LEFT)  { s_tab_focus = (s_tab_focus > 0) ? s_tab_focus - 1 : 2; need_full = true; }
      else if (k & KEY_RIGHT) { s_tab_focus = (s_tab_focus + 1) % 3; need_full = true; }
      else if (k & KEY_A) {
        if (s_tab_focus == 0) { s_tab_focus = -1; on_title = false; need_full = true; }      /* PKMN DATA -> grid */
        else if (s_tab_focus == 1) { if (src->is_bank) snd_deny();                            /* PARTY SEL */
                                     else { s_tab_focus = -1; boxoam_exit(); return 3; } }
        else { s_tab_focus = -1; boxoam_exit(); return 0; }                                   /* SAVE -> exit (save prompt) */
      }
      continue;
    }

    if (k & KEY_B) { if (s_cur_mode != CM_NORMAL && !on_title) { s_cur_mode = CM_NORMAL; need_full = true; } else { boxoam_exit(); return 0; } }
    else if ((k & KEY_START) && !src->is_bank) { boxoam_exit(); return 2; }
    else if (k & KEY_L) { SWITCH_BOX((box + nb - 1) % nb); }
    else if (k & KEY_R) { SWITCH_BOX((box + 1) % nb); }
    else if (k & KEY_SELECT) {                       /* cycle cursor mode (Omega-only edit modes) */
      if (!on_title && src->can_edit()) { s_cur_mode = (s_cur_mode + 1) % 3; need_full = true; }
      else snd_deny();
    }
    else if (on_title) {                           /* TITLE row: limited controls */
      if (k & KEY_DOWN) on_title = false;
      else if (k & KEY_UP) { s_tab_focus = src->is_bank ? 2 : 1; need_full = true; }   /* up into the top tabs */
      /* LEFT/RIGHT on the box name flips boxes, like the real Gen-3 PC (fresh
       * presses only, so holding doesn't machine-gun through boxes). */
      else if (fresh & KEY_LEFT)  { SWITCH_BOX((box + nb - 1) % nb); on_title = true; }
      else if (fresh & KEY_RIGHT) { SWITCH_BOX((box + 1) % nb); on_title = true; }
      else if (k & KEY_A) {
        if (src->can_edit()) { boxoam_suspend(); box_options_menu(src, box); boxoam_resume(); need_full = true; }
        else { snd_deny(); }
      }
    }
    else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
    else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
    else if (k & KEY_UP)    { if (cur < COLS) on_title = true; else cur -= COLS; }
    else if (k & KEY_DOWN)  { if (src->is_bank && cur >= COLS * (ROWS - 1)) { boxoam_exit(); return 5; }   /* off the bank bottom -> PC tabs */
                              else cur = (cur >= COLS * (ROWS - 1)) ? cur - COLS * (ROWS - 1) : cur + COLS; }
    else if ((k & KEY_A) && s_cur_mode == CM_MOVE) {     /* orange hand: grab the mon directly */
      if (!src->can_edit() || !g_box[cur].species) snd_deny();
      else {
        start_carry(src, recs, box, cur);
        render_full(src, box, cur, false, false, false);
        play_grab_anim(src, box, cur);
        carry_move(src, box, cur, cur);
        draw_footer(src->is_bank, false, true);
      }
    }
    else if ((k & KEY_A) && s_cur_mode == CM_ITEM) {     /* transparent hand: pick up the held item */
      if (!src->can_edit()) snd_deny();
      else if (g_box[cur].species && g_box[cur].heldItem) {
        s_item_held = g_box[cur].heldItem; s_item_from = cur; s_item_from_box = box;
        box_set_held(recs, cur, 0);
        pk_decode_box_raw(recs, g_box);
        src->mark_dirty();
        need_full = true;
      } else snd_deny();                                 /* empty slot or no item */
    }
    else if (k & KEY_A) {
      /* NORMAL: open the action menu on an occupied slot, or on an empty slot when
       * editable (CREATE a mon, or PASTE if the clipboard holds one). */
      if (g_box[cur].species || src->can_edit()) {
        uint8_t* rec = recs + (uint32_t)cur * 80;
        int mbox = src->is_bank ? 0 : box;                               /* box index within menu_block */
        boxoam_suspend();                                                /* sprites off while the menu/summary is up */
        app_mon_menu(rec, false, src->is_bank, src->commit, src->menu_block, mbox, cur);
        boxoam_resume();
        recs = src->records(box);                                        /* menu may have edited it */
        pk_decode_box_raw(recs, g_box);                                  /* refresh after possible write */
        s_oam_reload = true;                                             /* contents may have changed */
        if (app_take_move_request()) {                                   /* picked MOVE -> into the glove */
          start_carry(src, recs, box, cur);
          render_full(src, box, cur, false, false, false);               /* repaint OVER the menu, no black flash */
          play_grab_anim(src, box, cur);                                 /* grab cue (OAM lift) */
          carry_move(src, box, cur, cur);                                /* lift into carry */
          draw_footer(src->is_bank, false, true);                        /* move-mode footer */
        } else if (app_take_dup_request()) {                            /* picked DUPLICATE -> a fresh COPY in the glove */
          memcpy(s_held, recs + (uint32_t)cur * 80, 80);                /* copy floats in-hand; no origin (cancel discards it) */
          s_holding = true; s_orig_box = box; s_orig_slot = -1; s_orig_bank = src->is_bank; s_held_dup = true;
          render_full(src, box, cur, false, false, false);
          play_grab_anim(src, box, cur);
          carry_move(src, box, cur, cur);
          draw_footer(src->is_bank, false, true);
        } else {
          int pb, ps;
          if (app_take_pickup(&pb, &ps) && pb >= 0 && pb < nb && ps >= 0 && ps < 30) {
            box = pb; recs = src->records(box); pk_decode_box_raw(recs, g_box);   /* TO DAY-CARE->PC: carry the parked mon */
            cur = ps; start_carry(src, recs, box, ps); s_oam_reload = true;
            render_full(src, box, cur, false, false, false);
            play_grab_anim(src, box, cur);
            carry_move(src, box, cur, cur);
            draw_footer(src->is_bank, false, true);
          } else {
            need_full = true;                                            /* menu may have edited -> redraw */
          }
        }
      }
    }

    /* cursor-only change -> light update: reposition the cursor sprite + refresh the
     * left panel (and banner/footer if the title row changed). Icons/markers are
     * sprites the GPU composites, so there is nothing to erase or repaint. */
    if (!need_full && (cur != old_cur || on_title != old_title))
      move_cursor(src, box, old_cur, old_title, cur, on_title);
  }
  #undef SWITCH_BOX
}
