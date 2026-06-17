/*
 * Hardware-OAM layer for the PC/Bank box screen — see box_oam.h for the design.
 *
 * The grid of 32x32 box icons + the overlays that sit on top of them are GBA OBJ
 * sprites. The wallpaper / banner / panel / tabs / footer stay software on the BG2
 * Mode-3 bitmap (sprites composite above them, for free, every frame).
 *
 * OBJ-VRAM tile budget (bitmap mode -> only ids 512..1023 usable):
 *   ids 512..991  (480) : the 30 grid icons (slot s -> 512 + s*16, 16 tiles each)
 *   ids 992..1007 ( 16) : cursor hand (32x32)            [region A, permanent]
 *   ids 1008..1023( 16) : grab fist (move) OR item glyphs (ITEM)  [region B, shared]
 * Exactly 512 tiles.
 *
 * OBJ palette banks (16 total): 0..12 = the 13 shared icon palettes (uploaded once);
 *   13 = hand (normal) + grab + item glyph; 14 = hand (orange, MOVE); 15 = carried
 *   item icon.  ITEM "translucent" hand = OBJ alpha-blend (bank 13 + BLDCNT).
 */
#include <tonc.h>
#include "box_oam.h"
#include "mon_icons_oam.h"
#include "hand_oam.h"
#include "item_icons.h"

/* grid geometry — MUST match pdna_box.c */
#define COLS    6
#define ROWS    5
#define CELL_W  24
#define CELL_H  22
#define GRID_X  82
#define GRID_Y  30
#define WP_X    78
#define WP_W    162
#define WP_Y    12

/* OBJ tile ids (charblock-4-relative 4bpp indices; only 512..1023 valid in bitmap modes) */
#define TID_ICON0   512                 /* slot s -> TID_ICON0 + s*16            */
#define TID_HAND    992                 /* region A: cursor hand (16 tiles)      */
#define TID_REGB    1008                /* region B: grab fist / item glyphs     */
#define TID_GRAB    TID_REGB            /* 32x32 grab fist (16 tiles)            */
#define TID_CITEM   TID_REGB            /* 16x16 carried item (4 tiles)          */
#define TID_IGLYPH  (TID_REGB + 4)      /* 8x8 generic item marker (1 tile)      */

/* OBJ palette banks */
#define PB_ICON_MAX 12                  /* icon banks 0..12                       */
#define PB_HAND     13                  /* hand(normal)+grab+item glyph           */
#define PB_HANDORG  14                  /* hand orange (MOVE)                     */
#define PB_CITEM    15                  /* carried item icon                      */

/* OAM entry assignment */
#define OE_ICON0    0                   /* 0..29 grid icons                       */
#define OE_HAND     30                  /* cursor hand                            */
#define OE_GRAB     31                  /* grab fist (move carry)                 */
#define OE_CARRY    32                  /* carried icon (move carry)              */
#define OE_CITEM    33                  /* carried item (ITEM carry)              */
#define OE_MARK0    34                  /* 34..63 ITEM-mode held-item markers     */
#define OE_COUNT    64

static OBJ_ATTR s_shadow[128];          /* OAM shadow; flushed in vblank          */
static uint8_t  s_iconbank[30];         /* palette bank per grid slot (0=empty)   */
static uint8_t  s_occupied[30];         /* 1 if slot has an icon                  */
static int      s_bob = 0;              /* current unison Y-bob offset (0/1)      */
static int      s_regb = -1;            /* what region B holds: 0=grab 1=item -1=none */
static int      s_carry_from = -1;      /* grid slot lifted out during move-carry, or -1 */

static inline OBJ_ATTR* oe(int i) { return &s_shadow[i]; }

/* hide one shadow entry */
static void hide(int i) { obj_hide(oe(i)); }

/* DMA `bytes` from src into OBJ tile id `tid` (charblock-4-relative). */
static void upload_tiles(int tid, const void* src, int bytes) {
  /* OBJ tile memory base = tile_mem_obj[0] (0x06010000); each 4bpp tile = 32 B. */
  uint16_t* dst = (uint16_t*)((uint8_t*)tile_mem_obj[0] + (uint32_t)tid * 32);
  dma3_cpy(dst, src, bytes);
}

/* ------- region B (time-shared between move grab-fist and ITEM glyphs) ------- */
static void load_regb_grab(void) {
  if (s_regb == 0) return;
  upload_tiles(TID_GRAB, hand_oam_grab_tiles, HAND_OAM_TILES * HAND_OAM_TILE_BYTES);
  s_regb = 0;
}

/* Upload the generic 8x8 "holds item" marker glyph into region B (it never changes,
 * uploaded once when region B first switches to ITEM use). Pixel index 4 = fill,
 * 5 = border; bank 13 (PB_HAND) slots 4/5 carry those colours (set in enter()).
 * Hand art only uses bank-13 slots 1..3, so 4/5 don't collide. */
static void load_iglyph(void) {
  uint8_t glyph[32];
  for (int j = 0; j < 8; j++)
    for (int i = 0; i < 4; i++) {            /* 2 px/byte */
      int x0 = i * 2, x1 = i * 2 + 1;
      int e0 = (j == 0 || j == 7 || x0 == 0 || x0 == 7) ? 5 : 4;
      int e1 = (j == 0 || j == 7 || x1 == 0 || x1 == 7) ? 5 : 4;
      glyph[j * 4 + i] = (uint8_t)((e0 & 0xF) | ((e1 & 0xF) << 4));
    }
  upload_tiles(TID_IGLYPH, glyph, sizeof glyph);
}

/* Rescale the 24x24 RGB15 icon for `item` into a 4-tile (16x16) 4bpp sprite at
 * TID_CITEM with a private palette in bank 15. item 0 / no icon -> a filled glyph
 * (the generic marker recoloured into bank 15). Switches region B to ITEM use. */
static void load_regb_item(uint16_t carried_item) {
  load_iglyph();
  uint16_t cpal[16]; for (int i = 0; i < 16; i++) cpal[i] = 0;
  cpal[4] = RGB15(28, 24, 6); cpal[5] = RGB15(8, 6, 1);   /* glyph fallback colours */
  uint8_t ctiles[4 * 32];
  for (unsigned b = 0; b < sizeof ctiles; b++) ctiles[b] = 0;
  const uint16_t* ic = carried_item ? item_icon_for(carried_item) : 0;
  if (ic) {
    int ncol = 6;                                  /* keep 4/5 as the glyph spares */
    uint8_t idx16[16 * 16];
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++) {
        uint16_t p = ic[(y * 24 / 16) * ITEM_ICON_W + (x * 24 / 16)];
        if (!(p & 0x8000)) { idx16[y * 16 + x] = 0; continue; }
        uint16_t c = p & 0x7FFF; int found = 0;
        for (int k = 1; k < ncol; k++) if (cpal[k] == c) { idx16[y * 16 + x] = k; found = 1; break; }
        if (!found) {
          if (ncol < 16) { cpal[ncol] = c; idx16[y * 16 + x] = ncol; ncol++; }
          else {                                   /* palette full: nearest match */
            int best = 1, bd = 0x7fffffff;
            int pr = c & 31, pg = (c >> 5) & 31, pb = (c >> 10) & 31;
            for (int k = 1; k < 16; k++) {
              int dr = pr - (cpal[k] & 31), dg = pg - ((cpal[k] >> 5) & 31), db = pb - ((cpal[k] >> 10) & 31);
              int dd = dr * dr + dg * dg + db * db;
              if (dd < bd) { bd = dd; best = k; }
            }
            idx16[y * 16 + x] = best;
          }
        }
      }
    int bi = 0;                                    /* pack 4 tonc tiles (2x2) */
    for (int ty = 0; ty < 2; ty++)
      for (int tx = 0; tx < 2; tx++)
        for (int ry = 0; ry < 8; ry++)
          for (int rx = 0; rx < 8; rx += 2) {
            int px = tx * 8 + rx, py = ty * 8 + ry;
            uint8_t lo = idx16[py * 16 + px], hi = idx16[py * 16 + px + 1];
            ctiles[bi++] = (uint8_t)((lo & 0xF) | ((hi & 0xF) << 4));
          }
  } else {
    /* no item icon: draw the generic filled glyph (indices 4/5) into the 16x16 */
    uint8_t idx16[16 * 16];
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++)
        idx16[y * 16 + x] = (y == 0 || y == 15 || x == 0 || x == 15) ? 5 : 4;
    int bi = 0;
    for (int ty = 0; ty < 2; ty++)
      for (int tx = 0; tx < 2; tx++)
        for (int ry = 0; ry < 8; ry++)
          for (int rx = 0; rx < 8; rx += 2) {
            int px = tx * 8 + rx, py = ty * 8 + ry;
            uint8_t lo = idx16[py * 16 + px], hi = idx16[py * 16 + px + 1];
            ctiles[bi++] = (uint8_t)((lo & 0xF) | ((hi & 0xF) << 4));
          }
  }
  for (int i = 0; i < 16; i++) pal_obj_mem[PB_CITEM * 16 + i] = cpal[i];
  upload_tiles(TID_CITEM, ctiles, sizeof ctiles);
  s_regb = 1;
}

void boxoam_enter(void) {
  oam_init(s_shadow, 128);                         /* clears shadow to hidden     */
  for (int i = 0; i < 30; i++) { s_occupied[i] = 0; s_iconbank[i] = 0; }
  s_bob = 0; s_regb = -1; s_carry_from = -1;

  /* shared icon palettes -> banks 0..12 (416 bytes) */
  for (int i = 0; i < MON_ICON_OAM_BANKS * MON_ICON_OAM_PALLEN; i++)
    pal_obj_mem[i] = mon_icon_oam_pal[i];

  /* hand palette -> bank 13 (normal) and an orange-tinted copy -> bank 14 (MOVE) */
  for (int i = 0; i < 16; i++) {
    uint16_t c = hand_oam_pal[i];
    pal_obj_mem[PB_HAND * 16 + i] = c;
    /* push toward orange: keep luma, bias R up / B down */
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    uint32_t lum = (r * 2 + g * 5 + b) >> 3;
    uint32_t nr = lum + 6; if (nr > 31) nr = 31;
    uint16_t o = (i == 0) ? 0 : (uint16_t)(nr | (((lum * 5) >> 3) << 5) | ((lum >> 2) << 10));
    pal_obj_mem[PB_HANDORG * 16 + i] = o;
  }
  /* item-marker glyph colours: bank 13 slots 4/5 (the hand art only uses 1..3, so
   * these don't collide). slot 4 = tan fill, slot 5 = dark border. */
  pal_obj_mem[PB_HAND * 16 + 4] = RGB15(28, 24, 6);
  pal_obj_mem[PB_HAND * 16 + 5] = RGB15(8, 6, 1);

  /* hand tiles -> region A (id 992) */
  upload_tiles(TID_HAND, hand_oam_cursor_tiles, HAND_OAM_TILES * HAND_OAM_TILE_BYTES);
  load_regb_grab();                                /* default region B = grab fist */

  /* hide every overlay entry up front */
  for (int i = OE_HAND; i < OE_COUNT; i++) hide(i);

  REG_DISPCNT |= DCNT_OBJ | DCNT_OBJ_1D;           /* enable OBJ, 1D tile mapping  */
}

void boxoam_exit(void) {
  REG_DISPCNT &= ~DCNT_OBJ;                         /* OBJ off for every other screen */
  REG_BLDCNT = 0;                                   /* drop any ITEM-mode blend     */
  oam_init(s_shadow, 128);
  oam_copy(oam_mem, s_shadow, 128);                 /* clear hardware OAM           */
}

void boxoam_suspend(void) { REG_DISPCNT &= ~DCNT_OBJ; REG_BLDCNT = 0; }
void boxoam_resume(void)  { REG_DISPCNT |= DCNT_OBJ | DCNT_OBJ_1D; }

/* (re)place grid slot s's icon sprite at its cell (incl. the current bob offset). */
static void place_grid_slot(int s) {
  int x = GRID_X + (s % COLS) * CELL_W;
  int y = GRID_Y + (s / COLS) * CELL_H + s_bob;
  obj_set_attr(oe(OE_ICON0 + s),
               ATTR0_SQUARE | ATTR0_4BPP | (y & ATTR0_Y_MASK),
               ATTR1_SIZE_32 | (x & ATTR1_X_MASK),
               ATTR2_ID(TID_ICON0 + s * MON_ICON_OAM_TILES) | ATTR2_PRIO(2) |
               ATTR2_PALBANK(s_iconbank[s]));
}

void boxoam_load_box(const PkMon box[30]) {
  for (int s = 0; s < 30; s++) {
    const uint8_t* tiles; int bank;
    if (box[s].species &&
        mon_icon_oam_for_form(box[s].species, box[s].form, &tiles, &bank)) {
      upload_tiles(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles,
                   MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
      s_occupied[s] = 1; s_iconbank[s] = (uint8_t)bank;
      place_grid_slot(s);
    } else {
      s_occupied[s] = 0;
      hide(OE_ICON0 + s);
    }
  }
}

void boxoam_set_bob(int dy) {
  if (dy == s_bob) return;
  s_bob = dy;
  for (int s = 0; s < 30; s++) {
    if (!s_occupied[s]) continue;
    int y = GRID_Y + (s / COLS) * CELL_H + s_bob;
    obj_set_pos(oe(OE_ICON0 + s), GRID_X + (s % COLS) * CELL_W, y);
  }
}

static void hand_xy(int cur, int* hx, int* hy) {
  *hx = GRID_X + (cur % COLS) * CELL_W + 3 - 7;     /* 32px sprite vs 18px art: nudge left */
  *hy = GRID_Y + (cur / COLS) * CELL_H - 16;
  if (*hy < 28) *hy = 28;
}

void boxoam_cursor(int cur, bool on_title, int mode) {
  int hx, hy;
  if (on_title) { hx = WP_X + WP_W / 2 - 4 - 7; hy = 14; }
  else hand_xy(cur, &hx, &hy);

  int bank = (mode == BOXOAM_HAND_MOVE) ? PB_HANDORG : PB_HAND;
  u16 a0 = ATTR0_SQUARE | ATTR0_4BPP | (hy & ATTR0_Y_MASK);
  if (mode == BOXOAM_HAND_ITEM) {
    a0 |= ATTR0_BLEND;                               /* translucent (BLDCNT set below) */
    REG_BLDCNT = BLD_OBJ | BLD_BG2 | BLD_STD;        /* obj over BG2 bitmap, alpha    */
    REG_BLDALPHA = (10) | (8 << 8);                  /* ~10/16 obj + ~8/16 bg          */
  } else {
    REG_BLDCNT = 0;
  }
  obj_set_attr(oe(OE_HAND), a0,
               ATTR1_SIZE_32 | (hx & ATTR1_X_MASK),
               ATTR2_ID(TID_HAND) | ATTR2_PRIO(0) | ATTR2_PALBANK(bank));
  /* showing the hand means we're not move-carrying: hide carry sprites */
  hide(OE_GRAB); hide(OE_CARRY);
}

void boxoam_carry(int cur, int from) {
  if (from < 0) {                                    /* end carry: restore the lifted slot */
    hide(OE_GRAB); hide(OE_CARRY);
    if (s_carry_from >= 0 && s_occupied[s_carry_from]) place_grid_slot(s_carry_from);
    s_carry_from = -1;
    return;
  }
  load_regb_grab();                                  /* region B back to the fist     */
  REG_BLDCNT = 0;                                    /* carry is opaque               */
  s_carry_from = from;
  hide(OE_ICON0 + from);                             /* source cell reads empty while lifted */

  /* carried icon rides the cursor cell, lifted 4px (matches the software carry_xy) */
  int ix = GRID_X + (cur % COLS) * CELL_W;
  int iy = GRID_Y + (cur / COLS) * CELL_H - 4; if (iy < WP_Y) iy = WP_Y;
  if (s_occupied[from]) {
    obj_set_attr(oe(OE_CARRY),
                 ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
                 ATTR1_SIZE_32 | (ix & ATTR1_X_MASK),
                 ATTR2_ID(TID_ICON0 + from * MON_ICON_OAM_TILES) | ATTR2_PRIO(1) |
                 ATTR2_PALBANK(s_iconbank[from]));
  } else hide(OE_CARRY);

  /* grab fist centered over the icon top (matches carry_xy fist position) */
  int fx = ix + (32 - 32) / 2;                       /* fist sprite is 32 wide        */
  int fy = iy - 6; if (fy < WP_Y) fy = WP_Y;
  obj_set_attr(oe(OE_GRAB),
               ATTR0_SQUARE | ATTR0_4BPP | (fy & ATTR0_Y_MASK),
               ATTR1_SIZE_32 | ((fx - 7) & ATTR1_X_MASK),  /* 18px art in 32px box: nudge */
               ATTR2_ID(TID_GRAB) | ATTR2_PRIO(0) | ATTR2_PALBANK(PB_HAND));
  hide(OE_HAND);                                     /* hand hidden while carrying    */
}

void boxoam_item_markers(const PkMon box[30], bool show) {
  if (!show) { for (int m = 0; m < 30; m++) hide(OE_MARK0 + m); return; }
  load_iglyph();                                     /* ensure the marker tile exists */
  s_regb = 1;                                        /* region B is now in ITEM use    */
  int m = 0;
  for (int s = 0; s < 30 && m < 30; s++) {
    if (!box[s].species || !box[s].heldItem) continue;
    int ix = GRID_X + (s % COLS) * CELL_W + CELL_W - 8;   /* fixed-cell badge (no bob) */
    int iy = GRID_Y + (s / COLS) * CELL_H + CELL_H - 9;
    obj_set_attr(oe(OE_MARK0 + m),
                 ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
                 ATTR1_SIZE_8 | (ix & ATTR1_X_MASK),
                 ATTR2_ID(TID_IGLYPH) | ATTR2_PRIO(0) | ATTR2_PALBANK(PB_HAND));
    m++;
  }
  for (; m < 30; m++) hide(OE_MARK0 + m);
}

void boxoam_carry_item(int cur, uint16_t item) {
  if (!item) { hide(OE_CITEM); return; }
  load_regb_item(item);                              /* upload this item's 16x16 icon */
  int ix = GRID_X + (cur % COLS) * CELL_W + 10;
  int iy = GRID_Y + (cur / COLS) * CELL_H - 14; if (iy < WP_Y) iy = WP_Y;
  obj_set_attr(oe(OE_CITEM),
               ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
               ATTR1_SIZE_16 | (ix & ATTR1_X_MASK),
               ATTR2_ID(TID_CITEM) | ATTR2_PRIO(0) | ATTR2_PALBANK(PB_CITEM));
}

void boxoam_commit(void) {
  oam_copy(oam_mem, s_shadow, OE_COUNT);
}
