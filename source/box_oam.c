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
#include "rumble.h"         /* rumble_io_suspend: freeze GPIO during verified CPU ROM reads */
#include "log.h"            /* icon-upload self-verify diagnostics */
#include "pdna_app.h"       /* app_log_flush: anomaly evidence must survive a power-off */

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
#define TID_REGB    1008                /* region B: grab fist OR full-size item  */
#define TID_GRAB    TID_REGB            /* 32x32 grab fist (16 tiles)            */
#define TID_CITEM   TID_REGB            /* 32x32 full-size item, real icon (16 tiles) */

/* OBJ palette banks */
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
static uint16_t s_species[30];          /* species per slot (for the frame swap)  */
static uint8_t  s_form[30];             /* form per slot                          */
static uint8_t  s_isegg[30];            /* 1 = this slot shows the Egg icon (no bob)  */
static uint8_t  s_icon_blend[30];       /* 1 = draw this icon semi-transparent (ITEM mode, non-holders) */
static int      s_frame = 0;            /* current bob frame (0/1) in OBJ VRAM     */
static int      s_bob = 0;              /* current unison Y-bob offset (0/1)      */
static int      s_regb = -1;            /* what region B holds: 0=grab fist 1=item -1=none */
static int      s_rega = -1;            /* what region A holds: 0=hand 1=full item 2=held mon -1 */
static uint8_t  s_selmark[30];          /* 1 = whiten this slot (rubber-band selection)   */
static uint8_t  s_select_on = 0;        /* selection brightness-blend registers active    */
static uint8_t  s_covered[30];          /* 1 = slot's tile region borrowed by the chunk   */
static uint8_t  s_chunk_on = 0;         /* chunk-carry visuals active                     */
static uint8_t  s_chunk_valid = 0;      /* borrowed uploads valid (load_box invalidates)  */
static int      s_chunk_tr = 0, s_chunk_tc = 0;  /* last-applied anchor (skip re-DMA when unmoved) */

static inline OBJ_ATTR* oe(int i) { return &s_shadow[i]; }

/* hide one shadow entry */
static void hide(int i) { obj_hide(oe(i)); }

/* DMA `bytes` from src into OBJ tile id `tid` (charblock-4-relative). */
static void upload_tiles(int tid, const void* src, int bytes) {
  /* OBJ tile memory base = tile_mem_obj[0] (0x06010000); each 4bpp tile = 32 B. */
  uint16_t* dst = (uint16_t*)((uint8_t*)tile_mem_obj[0] + (uint32_t)tid * 32);
  dma3_cpy(dst, src, bytes);
}

/* Verified copy for the ONE-SHOT icon uploads. The 30 grid icons composite OVER the
 * wallpaper region every frame, and their ROM reads were the last unverified path that
 * could paint a "jumble" there — garbled icons are visually indistinguishable from a
 * garbled wallpaper. Same contract as pdna_box's wp_copy_verified: IWRAM_CODE so the
 * cart bus carries pure data reads, volatile source so the compare is a real 2nd read. */
IWRAM_CODE __attribute__((noinline))
static int icopy_verified(uint16_t* dst, const uint16_t* src, int n) {
  volatile const uint16_t* vsrc = src;
  for (int a = 0; a < 4; a++) {
    for (int i = 0; i < n; i++) dst[i] = vsrc[i];
    int ok = 1;
    for (int i = 0; i < n; i++) if (dst[i] != vsrc[i]) { ok = 0; break; }
    if (ok) return a;
  }
  return -1;
}

static uint16_t s_stage[256];        /* one 512 B verify chunk (IWRAM .bss — EWRAM is full) */

/* upload_tiles with staged verify: ROM -> RAM (verified, retried) -> DMA to VRAM
 * (RAM->VRAM cannot glitch). On give-up, the raw upload still runs (a maybe-garbled
 * icon beats a hole) and the anomaly is logged + flushed so it survives a power-off.
 * NOT used by boxoam_set_frame: a verified 15 KiB re-stage cannot fit the vblank
 * window, and any set_frame glitch self-heals at the next swap (<= 0.5 s). */
static void upload_tiles_verified(int tid, const void* src, int bytes) {
  const uint16_t* s = (const uint16_t*)src;
  uint16_t* dst = (uint16_t*)((uint8_t*)tile_mem_obj[0] + (uint32_t)tid * 32);
  int total = bytes / 2, done = 0, rr = 0, bad = 0;
  rumble_io_suspend();               /* same GPIO freeze the wallpaper's CPU reads get */
  while (done < total) {
    int n = total - done; if (n > 256) n = 256;
    int r = icopy_verified(s_stage, s + done, n);
    if (r < 0) { bad = 1; break; }
    rr += r;
    dma3_cpy(dst + done, s_stage, (uint32_t)n * 2);
    done += n;
  }
  rumble_io_resume();
  if (bad) { dma3_cpy(dst, src, bytes); log_line("icons: tid %d unstable rom reads", tid); }
  else if (rr) log_line("icons: tid %d %d re-reads", tid, rr);
  static int s_icon_flushes = 0;
  if ((bad || rr) && s_icon_flushes < 2) { s_icon_flushes++; app_log_flush(); }
}

/* ------- region B (time-shared between move/item grab-fist and the small item) ------- */
static void load_regb_grab(void) {
  if (s_regb == 0) return;
  upload_tiles_verified(TID_GRAB, hand_oam_grab_tiles, HAND_OAM_TILES * HAND_OAM_TILE_BYTES);
  s_regb = 0;
}

/* ------- region A (time-shared: cursor hand / held-mon icon / full grab item) -------
 * the cursor hand is hidden whenever we carry a mon or grab an item, so its 16 tiles
 * are free for the held mon's icon or the full-size item; restore the hand on the way back. */
static void load_rega_hand(void) {
  if (s_rega == 0) return;
  upload_tiles_verified(TID_HAND, hand_oam_cursor_tiles, HAND_OAM_TILES * HAND_OAM_TILE_BYTES);
  s_rega = 0;
}

/* Render the FULL 24x24 RGB15 icon for `item` at native size, centred in a 16-tile
 * (32x32) 4bpp sprite at TID_CITEM with a private 16-colour palette in bank 15.
 * Used both for the carried item (grab) and the hovered holder's item preview, so it
 * reads as the real, full-size sprite. Switches region B to ITEM use. item 0 / unknown
 * icon -> a simple filled box. The 24x24 art sits at offset (+4,+4) inside the 32x32. */
#define CITEM_OFF 4   /* centre the 24x24 art in the 32x32 sprite */

/* map a pixel of the sprite to a palette index (0 = transparent).
 * full -> 24x24 art centred (+4) in a 32x32 sprite; !full -> 24x24 down-scaled to 16x16. */
static uint8_t citem_index(const uint16_t* ic, const uint16_t* cpal, int ncol, int px, int py, bool full) {
  int sx, sy;
  if (full) { sx = px - CITEM_OFF; sy = py - CITEM_OFF;
              if (sx < 0 || sx >= ITEM_ICON_W || sy < 0 || sy >= ITEM_ICON_H) return 0; }
  else      { sx = px * ITEM_ICON_W / 16; sy = py * ITEM_ICON_H / 16; }   /* down-scale 24->16 */
  uint16_t p = ic[sy * ITEM_ICON_W + sx];
  if (!(p & 0x8000)) return 0;                       /* transparent pixel */
  uint16_t c = p & 0x7FFF;
  for (int k = 1; k < ncol; k++) if (cpal[k] == c) return (uint8_t)k;
  int best = 1, bd = 0x7fffffff, pr = c & 31, pg = (c >> 5) & 31, pb = (c >> 10) & 31;
  for (int k = 1; k < ncol; k++) {                   /* palette full: nearest match */
    int dr = pr - (cpal[k] & 31), dg = pg - ((cpal[k] >> 5) & 31), db = pb - ((cpal[k] >> 10) & 31);
    int dd = dr * dr + dg * dg + db * db;
    if (dd < bd) { bd = dd; best = k; }
  }
  return (uint8_t)best;
}

static void load_regb_item(uint16_t carried_item, bool full, int tid) {
  /* full = 32x32 (16 tiles) carried/grab item; !full = 16x16 (4 tiles) hover preview.
   * tid = where the tiles go (TID_CITEM in region B for hover, TID_HAND in region A for grab). */
  uint16_t cpal[16]; for (int i = 0; i < 16; i++) cpal[i] = 0;
  uint8_t ctiles[16 * 32];                           /* up to 16 tonc tiles, 4bpp */
  for (unsigned b = 0; b < sizeof ctiles; b++) ctiles[b] = 0;
  int across = full ? 4 : 2;                          /* tiles per row -> 32x32 or 16x16 */
  const uint16_t* ic = carried_item ? item_icon_for(carried_item) : 0;
  if (ic) {
    int ncol = 1;                                    /* build the 16-colour palette */
    for (int y = 0; y < ITEM_ICON_H; y++)
      for (int x = 0; x < ITEM_ICON_W; x++) {
        uint16_t p = ic[y * ITEM_ICON_W + x];
        if (!(p & 0x8000)) continue;
        uint16_t c = p & 0x7FFF; int found = 0;
        for (int k = 1; k < ncol; k++) if (cpal[k] == c) { found = 1; break; }
        if (!found && ncol < 16) cpal[ncol++] = c;
      }
    int bi = 0;                                      /* pack across*across tonc tiles */
    for (int ty = 0; ty < across; ty++)
      for (int tx = 0; tx < across; tx++)
        for (int ry = 0; ry < 8; ry++)
          for (int rx = 0; rx < 8; rx += 2) {
            int px = tx * 8 + rx, py = ty * 8 + ry;
            uint8_t lo = citem_index(ic, cpal, ncol, px, py, full);
            uint8_t hi = citem_index(ic, cpal, ncol, px + 1, py, full);
            ctiles[bi++] = (uint8_t)((lo & 0xF) | ((hi & 0xF) << 4));
          }
  } else {
    /* unknown item: a simple filled box (index 2 fill, 1 border) over the whole sprite */
    cpal[1] = RGB15(8, 6, 1); cpal[2] = RGB15(28, 24, 6);
    int dim = across * 8, bi = 0;
    for (int ty = 0; ty < across; ty++)
      for (int tx = 0; tx < across; tx++)
        for (int ry = 0; ry < 8; ry++)
          for (int rx = 0; rx < 8; rx += 2) {
            int px = tx * 8 + rx, py = ty * 8 + ry;
            uint8_t v0 = (px == 0 || px == dim - 1 || py == 0 || py == dim - 1) ? 1 : 2;
            uint8_t v1 = (px + 1 == dim - 1 || py == 0 || py == dim - 1) ? 1 : 2;
            ctiles[bi++] = (uint8_t)((v0 & 0xF) | ((v1 & 0xF) << 4));
          }
  }
  for (int i = 0; i < 16; i++) pal_obj_mem[PB_CITEM * 16 + i] = cpal[i];
  upload_tiles(tid, ctiles, across * across * 32);
}

void boxoam_enter(void) {
  oam_init(s_shadow, 128);                         /* clears shadow to hidden     */
  for (int i = 0; i < 30; i++) { s_occupied[i] = 0; s_iconbank[i] = 0;
                                 s_selmark[i] = 0; s_covered[i] = 0; }
  s_bob = 0; s_regb = -1; s_rega = -1;
  s_select_on = 0; s_chunk_on = 0; s_chunk_valid = 0;

  /* shared icon palettes -> banks 0..12 (416 bytes), staged + verified like the tiles
   * (a corrupted palette garbles every icon at once — the full-region jumble look) */
  { int n = MON_ICON_OAM_BANKS * MON_ICON_OAM_PALLEN;
    if (icopy_verified(s_stage, mon_icon_oam_pal, n) < 0)
      log_line("icons: palette unstable rom reads");        /* best-effort: last attempt still lands */
    for (int i = 0; i < n; i++) pal_obj_mem[i] = s_stage[i]; }

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
  load_rega_hand();                               /* region A = cursor hand        */
  load_regb_grab();                               /* default region B = grab fist  */

  /* hide every overlay entry up front */
  for (int i = OE_HAND; i < OE_COUNT; i++) hide(i);
  oam_copy(oam_mem, s_shadow, 128);                /* clear ALL hw OAM (64..127 unused -> no garbage) */

  /* Mode-3 BG2 (the wallpaper) defaults to priority 0, which would sit IN FRONT of
   * the prio-1/2 icon/carry sprites and hide the whole grid. Drop it to priority 3
   * so every sprite (hand 0 > carry 1 > icons 2 > BG2 3) composites above it. */
  REG_BG2CNT = (REG_BG2CNT & ~3) | 3;
  REG_DISPCNT |= DCNT_OBJ | DCNT_OBJ_1D;           /* enable OBJ, 1D tile mapping  */
}

void boxoam_exit(void) {
  REG_DISPCNT &= ~DCNT_OBJ;                         /* OBJ off for every other screen */
  REG_BLDCNT = 0;                                   /* drop any ITEM-mode blend     */
  REG_BG2CNT &= ~3;                                 /* restore BG2 priority 0       */
  oam_init(s_shadow, 128);
  oam_copy(oam_mem, s_shadow, 128);                 /* clear hardware OAM           */
}

void boxoam_suspend(void) { REG_DISPCNT &= ~DCNT_OBJ; REG_BLDCNT = 0; }
void boxoam_resume(void)  { REG_DISPCNT |= DCNT_OBJ | DCNT_OBJ_1D; }

/* (re)place grid slot s's icon sprite at its cell (incl. the current bob offset). */
static void place_grid_slot(int s) {
  int x = GRID_X + (s % COLS) * CELL_W;
  int y = GRID_Y + (s / COLS) * CELL_H + s_bob;
  u16 a0 = ATTR0_SQUARE | ATTR0_4BPP | (y & ATTR0_Y_MASK);
  /* ITEM mode fades non-holders; rubber-band selection whitens marks (never both) */
  if (s_icon_blend[s] || s_selmark[s]) a0 |= ATTR0_BLEND;
  obj_set_attr(oe(OE_ICON0 + s), a0,
               ATTR1_SIZE_32 | (x & ATTR1_X_MASK),
               ATTR2_ID(TID_ICON0 + s * MON_ICON_OAM_TILES) | ATTR2_PRIO(2) |
               ATTR2_PALBANK(s_iconbank[s]));
}

void boxoam_load_box(const PkMon box[30]) {
  s_frame = 0;                                       /* a fresh box always shows frame 0 */
  /* a reload rewrites all 30 tile regions and re-shows every entry, so any borrowed
   * chunk regions / selection marks are void — the caller re-applies the chunk after */
  s_chunk_valid = 0;
  for (int s = 0; s < 30; s++) { s_covered[s] = 0; s_selmark[s] = 0; }
  for (int s = 0; s < 30; s++) {
    const uint8_t* tiles; int bank;
    bool egg = box[s].isEgg && !box[s].isBadEgg;
    if (egg && mon_icon_oam_egg(&tiles, &bank)) {          /* an Egg reads as the real Egg icon */
      upload_tiles_verified(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles,
                   MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
      s_occupied[s] = 1; s_isegg[s] = 1; s_iconbank[s] = (uint8_t)bank;
      s_species[s] = box[s].species; s_form[s] = box[s].form;
      place_grid_slot(s);
    } else if (box[s].species &&
        mon_icon_oam_for_form_frame(box[s].species, box[s].form, 0, &tiles, &bank)) {
      upload_tiles_verified(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles,
                   MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
      s_occupied[s] = 1; s_isegg[s] = 0; s_iconbank[s] = (uint8_t)bank;
      s_species[s] = box[s].species; s_form[s] = box[s].form;
      place_grid_slot(s);
    } else {
      s_occupied[s] = 0; s_isegg[s] = 0;
      hide(OE_ICON0 + s);
    }
  }
}

/* The real Gen-3 box "bob" — a 2-frame pose swap. DMA the chosen frame's tiles for every
 * occupied icon into the SAME OBJ VRAM window (the two frames can't both fit, so we swap).
 * ~15 KiB for a full box; call in the vblank window. */
void boxoam_set_frame(int frame) {
  frame &= 1;
  if (frame == s_frame) return;
  s_frame = frame;
  for (int s = 0; s < 30; s++) {
    /* eggs keep their single Egg frame; covered regions hold the carried block's art
     * (the pose anim is paused for the whole carry — this guards the forced frame-0
     * reset at carry start; if the bob ever runs DURING a carry, this must instead
     * upload the carried mon's frame for covered slots) */
    if (!s_occupied[s] || s_isegg[s] || s_covered[s]) continue;
    const uint8_t* tiles; int bank;
    if (mon_icon_oam_for_form_frame(s_species[s], s_form[s], (uint8_t)frame, &tiles, &bank))
      upload_tiles(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles,
                   MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
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
  /* The glove art's pointing fingertip sits at sprite-local (13,0). Put it on the
   * icon's top-centre (cell_x+16, cell_y) so the cursor rests OVER the mon, not to
   * its left: hx+13 = cell_x+16 -> hx = cell_x+3; tip a touch above the icon top. */
  *hx = GRID_X + (cur % COLS) * CELL_W + 3;
  *hy = GRID_Y + (cur / COLS) * CELL_H - 2;
  if (*hy < WP_Y) *hy = WP_Y;
}

void boxoam_cursor(int cur, bool on_title, int mode) {
  load_rega_hand();                                  /* region A back to the hand (a grab/carry may have borrowed it) */
  int hx, hy;
  /* On the box name the hand used to park mid-banner — dead centre of "NAME  n/30", so it
   * covered the count on the Bank and the tail of the name on a PC box. The banner's own
   * left arrow is decoration (the right one says the same thing), so the hand goes there
   * instead: it still points at the banner, and no data is ever underneath it. */
  if (on_title) { hx = WP_X + 1; hy = 13; }
  else hand_xy(cur, &hx, &hy);

  int bank = (mode == BOXOAM_HAND_MOVE) ? PB_HANDORG : PB_HAND;
  int prio = 0;
  u16 a0 = ATTR0_SQUARE | ATTR0_4BPP | (hy & ATTR0_Y_MASK);
  if (mode == BOXOAM_HAND_ITEM) {
    /* ITEM mode: the hand stays OPAQUE. It used to carry ATTR0_BLEND (semi-transparent),
     * which dimmed the mon it hovered — but a mon that HOLDS an item must read normally
     * (Guy). Only the NON-holder icons fade (their own ATTR0_BLEND set in place_grid_slot);
     * keep BLDCNT configured for THEM. And drop the hand to priority 1 so the prio-0
     * held-item preview (boxoam_carry_item hover) pops IN FRONT of the hand, not behind it.
     * (BLD_OBJ stays in the 2nd/bottom target only, so overlapping faded icons blend
     * uniformly; never put it in the 1st-target mask or every object would fade.) */
    prio = 1;
    REG_BLDCNT = ((BLD_BG2 | BLD_OBJ) << 8) | BLD_STD;
    REG_BLDALPHA = (10) | (8 << 8);                  /* ~10/16 obj + ~8/16 below       */
  } else if (!s_select_on && !s_chunk_on) {
    /* don't kill the selection/chunk brightness blend — the select loop repositions
     * the cursor every step and would otherwise un-whiten the marked icons */
    REG_BLDCNT = 0;
  }
  obj_set_attr(oe(OE_HAND), a0,
               ATTR1_SIZE_32 | (hx & ATTR1_X_MASK),
               ATTR2_ID(TID_HAND) | ATTR2_PRIO(prio) | ATTR2_PALBANK(bank));
  /* showing the hand means we're not move-carrying: hide carry sprites */
  hide(OE_GRAB); hide(OE_CARRY);
}

/* Carry a HELD mon (move mode), mon-in-hand model. The held mon's icon is decoded into
 * region A (the cursor hand's 16 tiles, free while carrying) so it survives box reloads,
 * and rides FRONT-MOST (PRIO 0) above every box icon (PRIO 2). An orange, semi-transparent
 * grab fist sits BEHIND it (region B, PRIO 1). The cursor hand is hidden. species 0 -> just
 * the fist (empty hand). The caller hides the origin slot via boxoam_hide_slot(). */
void boxoam_carry_held(int cur, uint16_t species, uint8_t form, bool egg) {
  int cx = GRID_X + (cur % COLS) * CELL_W, cy = GRID_Y + (cur / COLS) * CELL_H;
  int ix = cx, iy = cy - 8; if (iy < WP_Y) iy = WP_Y;   /* Emerald's carry floats 8px up */
  load_regb_grab();                                  /* fist tiles -> region B */
  REG_BLDCNT = 0;                                    /* carried mon is opaque  */
  const uint8_t* tiles; int bank = 0;
  if (egg ? mon_icon_oam_egg(&tiles, &bank)                     /* an Egg rides the glove AS an Egg */
          : (species && mon_icon_oam_for_form_frame(species, form, 0, &tiles, &bank))) {
    upload_tiles_verified(TID_HAND, tiles, MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
    s_rega = 2;                                      /* region A now holds the held mon */
    obj_set_attr(oe(OE_CARRY),                       /* front-most */
                 ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
                 ATTR1_SIZE_32 | (ix & ATTR1_X_MASK),
                 ATTR2_ID(TID_HAND) | ATTR2_PRIO(0) | ATTR2_PALBANK(bank));
  } else hide(OE_CARRY);
  int fx = cx + 3, fy = cy - 10; if (fy < WP_Y) fy = WP_Y;
  obj_set_attr(oe(OE_GRAB),                          /* orange grab fist, behind the mon */
               ATTR0_SQUARE | ATTR0_4BPP | (fy & ATTR0_Y_MASK),
               ATTR1_SIZE_32 | (fx & ATTR1_X_MASK),
               ATTR2_ID(TID_GRAB) | ATTR2_PRIO(1) | ATTR2_PALBANK(PB_HANDORG));
  hide(OE_HAND);                                     /* hand hidden while carrying */
}

void boxoam_carry_end(void) { hide(OE_CARRY); hide(OE_GRAB); }   /* stop carrying */
void boxoam_hide_slot(int s) { if (s >= 0 && s < 30) hide(OE_ICON0 + s); }  /* lift-hide the origin */

/* Re-upload grid slot s's own icon (current bob frame) from the per-slot bookkeeping
 * and re-show/hide it — undoes a chunk borrow of that slot's tile region. */
static void restore_slot(int s) {
  if (!s_occupied[s]) { hide(OE_ICON0 + s); return; }
  const uint8_t* tiles; int bank;
  if (s_isegg[s] ? mon_icon_oam_egg(&tiles, &bank)
                 : mon_icon_oam_for_form_frame(s_species[s], s_form[s], (uint8_t)s_frame,
                                               &tiles, &bank)) {
    upload_tiles_verified(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles,
                 MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
    s_iconbank[s] = (uint8_t)bank;
    place_grid_slot(s);
  } else hide(OE_ICON0 + s);
}

void boxoam_select_mark(const uint8_t sel[30]) {
  for (int s = 0; s < 30; s++) {
    uint8_t m = sel[s] ? 1 : 0;
    if (m != s_selmark[s]) { s_selmark[s] = m; if (s_occupied[s]) place_grid_slot(s); }
  }
  /* HW-PROVEN cue (Guy, 2026-07-18): the semi-transparent-OBJ -> brightness-mode
   * whiten (GBATEK-documented, emulator-clean) renders NO effect at all on the real
   * Omega DE + GBA SP display. Use the one per-sprite effect this hardware has
   * demonstrably honored (ITEM mode, same registers): alpha-blend the CHOSEN icons
   * against the wallpaper below — they go ghost-translucent while the rest stay
   * solid. (Emerald whitens; translucent is the nearest look the HW gives us.) */
  REG_BLDCNT = ((BLD_BG2 | BLD_OBJ) << 8) | BLD_STD;
  REG_BLDALPHA = (10) | (8 << 8);                    /* ~10/16 icon + ~8/16 below */
  s_select_on = 1;
}

/* During the rubber-band drag the glove is HIDDEN: sprites cannot alpha-blend with
 * other sprites on this hardware, so the glove sitting on the corner cell made that
 * one chosen mon read as opaque (Guy's HW round 2026-07-27) — with the glove away,
 * every chosen mon ghosts uniformly and the ghost-block IS the cursor. */
void boxoam_select_cursor(void) { hide(OE_HAND); hide(OE_GRAB); hide(OE_CARRY); }

void boxoam_select_clear(void) {
  for (int s = 0; s < 30; s++)
    if (s_selmark[s]) { s_selmark[s] = 0; if (s_occupied[s]) place_grid_slot(s); }
  s_select_on = 0;
  REG_BLDCNT = 0;
}

/* §12b bitmap-understudy hooks: weak no-ops so box_oam links standalone; the box
 * screen (pdna_box.c) provides the real bitmap blit/restore. See box_oam.h. */
__attribute__((weak)) void boxoam_under_show(int slot) { (void)slot; }
__attribute__((weak)) void boxoam_under_hide(int slot) { (void)slot; }

void boxoam_chunk_carry(int tr, int tc, int fist_r, int fist_c,
                        const BoxOamChunkMon* mons, int n, bool fit, int lift) {
  uint8_t newcov[30];
  for (int s = 0; s < 30; s++) newcov[s] = 0;
  for (int i = 0; i < n && i < 30; i++)
    newcov[(tr + mons[i].rr) * COLS + (tc + mons[i].cc)] = 1;
  /* tiles need re-DMA only when the cell->mon mapping moved (or a reload voided it) */
  bool full = !s_chunk_valid || tr != s_chunk_tr || tc != s_chunk_tc;

  for (int s = 0; s < 30; s++)                       /* uncovered cells get their mon back */
    if (s_covered[s] && !newcov[s]) { s_covered[s] = 0; restore_slot(s); boxoam_under_hide(s); }

  for (int i = 0; i < n && i < 30; i++) {
    int r = tr + mons[i].rr, c = tc + mons[i].cc, s = r * COLS + c;
    const uint8_t* tiles; int bank = 0;
    bool have = mons[i].egg
              ? mon_icon_oam_egg(&tiles, &bank)
              : (mons[i].species &&
                 mon_icon_oam_for_form_frame(mons[i].species, mons[i].form, 0, &tiles, &bank));
    if (!s_covered[s]) {                             /* cover TRANSITION: put the occupant into
                                                      * the bitmap so the ghost blends over it */
      s_covered[s] = 1;
      if (s_occupied[s]) boxoam_under_show(s);
    }
    hide(OE_ICON0 + s);                              /* the block occludes this cell anyway */
    if (!have) { hide(OE_MARK0 + i); continue; }     /* no icon data -> same degrade as load_box */
    if (full) upload_tiles_verified(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles,
                           MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
    int x = GRID_X + c * CELL_W, y = GRID_Y + r * CELL_H - lift;   /* Emerald carries the block 8px up */
    if (y < WP_Y) y = WP_Y;
    u16 a0 = ATTR0_SQUARE | ATTR0_4BPP | ATTR0_BLEND | (y & ATTR0_Y_MASK);
    /* the block stays ghosted for the WHOLE carry (Emerald keeps the lifted block
     * whitened until placement — Guy: "keep the transparent effect until I place
     * them"); a blocked drop just ghosts HARDER (register weights below) */
    obj_set_attr(oe(OE_MARK0 + i), a0,
                 ATTR1_SIZE_32 | (x & ATTR1_X_MASK),
                 ATTR2_ID(TID_ICON0 + s * MON_ICON_OAM_TILES) | ATTR2_PRIO(1) |
                 ATTR2_PALBANK(bank));
  }
  for (int i = n; i < 30; i++) hide(OE_MARK0 + i);

  load_regb_grab();                                  /* fist rides the release cell, above the block */
  int fx = GRID_X + fist_c * CELL_W + 3, fy = GRID_Y + fist_r * CELL_H - 2 - lift;
  if (fy < WP_Y) fy = WP_Y;
  obj_set_attr(oe(OE_GRAB),
               ATTR0_SQUARE | ATTR0_4BPP | (fy & ATTR0_Y_MASK),
               ATTR1_SIZE_32 | (fx & ATTR1_X_MASK),
               ATTR2_ID(TID_GRAB) | ATTR2_PRIO(1) | ATTR2_PALBANK(PB_HANDORG));
  hide(OE_HAND); hide(OE_CARRY);
  /* block cue: LIGHT ghost = carrying (fits here); HEAVY ghost = blocked. The
   * carried block never goes solid until it is placed (HW round 2026-07-27). */
  REG_BLDCNT = ((BLD_BG2 | BLD_OBJ) << 8) | BLD_STD;
  REG_BLDALPHA = fit ? ((10) | (8 << 8))             /* light: "in hand"          */
                     : ((6)  | (12 << 8));           /* heavy: "can't drop here"  */
  s_chunk_tr = tr; s_chunk_tc = tc;
  s_chunk_on = 1; s_chunk_valid = 1;
}

void boxoam_chunk_end(void) {
  for (int s = 0; s < 30; s++)
    if (s_covered[s]) { s_covered[s] = 0; restore_slot(s); boxoam_under_hide(s); }
  for (int i = 0; i < 30; i++) hide(OE_MARK0 + i);
  hide(OE_GRAB);
  s_chunk_on = 0; s_chunk_valid = 0;
  REG_BLDCNT = 0;
}

void boxoam_item_markers(const PkMon box[30], bool show) {
  /* ITEM mode fades the icons so the cursor + item badges read clearly — but a mon
   * that HOLDS an item stays opaque (and its item badge/the carried item are opaque),
   * so you can see who has what. Update the per-icon blend bit + re-place any changed. */
  for (int s = 0; s < 30; s++) {
    uint8_t b = (show && box[s].species && !box[s].heldItem) ? 1 : 0;
    if (b != s_icon_blend[s]) { s_icon_blend[s] = b; if (s_occupied[s]) place_grid_slot(s); }
  }
  /* No per-holder glyph badges: region B now holds the FULL-SIZE item icon, and a mon
   * that holds an item is shown opaque (above) while the rest fade — so the cursor's
   * holder reveals its real item via the full-size preview (boxoam_carry_item). */
  for (int m = 0; m < 30; m++) hide(OE_MARK0 + m);
}

void boxoam_carry_item(int cur, uint16_t item, bool full) {
  int cx = GRID_X + (cur % COLS) * CELL_W, cy = GRID_Y + (cur / COLS) * CELL_H;
  if (!item) {                                       /* nothing held/hovered -> clear item sprites */
    hide(OE_CITEM); if (full) hide(OE_GRAB);
    return;
  }
  if (full) {
    /* GRAB: the FULL 32x32 item rides in front of EVERYTHING (region A, PRIO 0), held by an
     * orange, semi-transparent grab fist behind it (region B, PRIO 1). Hand hidden. */
    load_regb_item(item, true, TID_HAND); s_rega = 1;
    load_regb_grab();                                /* fist tiles -> region B */
    int ix = cx - (CITEM_OFF + 1), iy = cy - (CITEM_OFF - 1); if (iy < WP_Y) iy = WP_Y;
    int fx = cx + 3, fy = cy - 6; if (fy < WP_Y) fy = WP_Y;
    REG_BLDCNT = ((BLD_BG2 | BLD_OBJ) << 8) | BLD_STD;   /* blend the fist over what's below */
    REG_BLDALPHA = (10) | (8 << 8);
    obj_set_attr(oe(OE_GRAB),                        /* orange transparent grab fist, behind the item */
                 ATTR0_SQUARE | ATTR0_4BPP | ATTR0_BLEND | (fy & ATTR0_Y_MASK),
                 ATTR1_SIZE_32 | (fx & ATTR1_X_MASK),
                 ATTR2_ID(TID_GRAB) | ATTR2_PRIO(1) | ATTR2_PALBANK(PB_HANDORG));
    obj_set_attr(oe(OE_CITEM),                       /* the full item, opaque, front-most */
                 ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
                 ATTR1_SIZE_32 | (ix & ATTR1_X_MASK),
                 ATTR2_ID(TID_HAND) | ATTR2_PRIO(0) | ATTR2_PALBANK(PB_CITEM));
    hide(OE_HAND);
  } else {
    /* HOVER: small 16x16 item at the cell's bottom-CENTRE, ON the mon (PRIO 0). Was at the
     * bottom-LEFT (cx-3), which spilled into the left-neighbour cell and read as "behind the
     * left neighbour" — centre it on the holder so it clearly belongs to THIS mon. */
    load_regb_item(item, false, TID_CITEM); s_regb = 1;
    int ix = cx + 6, iy = cy + CELL_H - 6; if (iy < WP_Y) iy = WP_Y;
    obj_set_attr(oe(OE_CITEM),
                 ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
                 ATTR1_SIZE_16 | (ix & ATTR1_X_MASK),
                 ATTR2_ID(TID_CITEM) | ATTR2_PRIO(0) | ATTR2_PALBANK(PB_CITEM));
  }
}

void boxoam_commit(void) {
  oam_copy(oam_mem, s_shadow, OE_COUNT);
}
