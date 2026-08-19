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
#include "rom_mon.h"      /* phase-1 ROM-streamed icons (artless + the user's own ROM) */
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

/* Retail Emerald's PC transparency, taken from the decomp rather than eyeballed:
 * SetMonIconTransparency (pokemon_storage_system.c) writes BLDCNT = BLDCNT_TGT2_ALL
 * (0x3F00) and BLDALPHA = BLDALPHA_BLEND(7, 11) (0x0B07), and it fires ONLY in the
 * MOVE-ITEMS mode, where item-less icons get objMode = ST_OAM_OBJ_BLEND and the mons
 * that DO hold an item stay opaque.
 *
 * Two things this gets right that the old (10, 8) constants did not: the second-target
 * mask is ALL layers, not just BG2+OBJ, so a faded icon blends the same over the
 * wallpaper, the banner and another icon; and the effect bits are deliberately NONE,
 * because a semi-transparent OBJ blends against the 2nd-target mask regardless of
 * BLDCNT's effect field — retail never switches on a global effect, which is what made
 * ours look like a screen-wide dimmer instead of per-icon transparency. */
#define PSS_BLDCNT   ((u16)((BLD_BG0 | BLD_BG1 | BLD_BG2 | BLD_BG3 | BLD_OBJ | BLD_BACKDROP) << 8))
#define PSS_BLDALPHA ((u16)(7 | (11 << 8)))

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

/* ---- ROM-streamed icons (phase 1 of the ROM-gated build) --------------------
 * When the compiled icon art is absent (the artless build) and the app has an open
 * RomMon on the user's own ROM, the box streams the real icons from it: 512 B raw
 * 4bpp per slot, staged into s_stage and read TWICE (the EZ-Flash read path can
 * return success holding garbage, and a fused read shares the compiled-art risk
 * class the verified uploads already treat). The 2-frame pose swap stays OFF on
 * this path — re-streaming 15 KiB per bob tick is not vblank material — but the
 * 1 px OAM bob still runs, so the grid keeps its life. */
static const RomMon* s_rommon = 0;
static int           s_rom_icons = 0;            /* this box was loaded from the ROM */
/* Memoised icon LOCATION (see rom_mon.h RomMonLoc): the icon-table pointer and the
 * palette index for one species. Invalidated whenever the ROM source changes — the
 * RomMon POINTER can stay the same across a re-registration of a different file, so
 * the pointer is not a safe cache key. */
static RomMonLoc s_iconloc;
static uint16_t  s_iconloc_sp = 0xFFFF;          /* 0xFFFF = nothing memoised */
static uint8_t   s_iconloc_f;
void boxoam_rom_icons(const struct RomMon* rm) {
  s_rommon = (rm && rm->ok) ? rm : 0;
  s_iconloc.ok = 0; s_iconloc_sp = 0xFFFF;
}
int  boxoam_icons_available(void) {
  const uint8_t* t; int b;
  return mon_icon_oam_for(1, &t, &b) || s_rommon != 0;
}

static uint32_t stage_sum(void) {
  uint32_t v = 0;
  for (int i = 0; i < 256; i++) v += s_stage[i];
  return v;
}

/* Read one icon frame into s_stage, TWICE, and accept only when both passes sum the
 * same (with one buffer a full byte-compare needs a second read anyway; two whole
 * reads agreeing catches the transient-garbage failure the EZ driver can produce).
 * Retries like icopy_verified; on give-up the last read still stands (a maybe-garbled
 * icon beats a hole) and the anomaly is logged. */
static int rom_icon_read_verified(uint16_t species, uint8_t form, int egg, int* bank) {
  if (!s_rommon) return 0;
  uint16_t sp = egg ? 412 : species;
  uint8_t f  = egg ? 0 : form;
  rumble_io_suspend();
  /* LOCATE ONCE — VERIFIED — then read the same frame twice.
   *
   * Locating once per icon instead of once per verify pass is what removed four
   * RomCtx reads per icon (and all of them on a memo hit); what it must NOT remove is
   * the verify's cover over the lookup itself. The pointer and the palette id ARE the
   * answer to "which species is this": a garbled pointer that still lands inside the
   * image passes ptr_ok, and with the location cached BOTH frame passes then read that
   * same wrong offset, agree, and paint another species' icon with nothing logged.
   * rom_mon_locate_verified reads each field twice back to back and requires
   * agreement, which is exactly the coverage the pre-memo code got from re-locating
   * per pass — and it adds no FAR seek, because a same-offset re-read of 4 bytes stays
   * inside the current FatFs cluster (rom_mon.c read_small). The FRAME verify below is
   * untouched: still two whole reads that must agree, four attempts, same give-up log. */
  if (!(s_iconloc.ok && s_iconloc_sp == sp && s_iconloc_f == f)) {
    int unstable = 0;
    s_iconloc_sp = sp; s_iconloc_f = f;
    if (!rom_mon_locate_verified(s_rommon, sp, f, &s_iconloc, 4, &unstable)) {
      s_iconloc_sp = 0xFFFF;                     /* leaves ok = 0: the memo self-voids */
      rumble_io_resume();
      /* Fail CLOSED on an unstable lookup — the slot draws empty. Everywhere else here
       * "a maybe-garbled icon beats a hole", but that trade is about PIXELS; an address
       * we cannot agree on would confidently draw the wrong Pokemon, which reads as
       * the save being wrong. A bounds failure (a corrupt species id in the save) is
       * not logged, so it cannot spam the log on every box load. */
      if (unstable) { log_line("icons: rom locate unstable sp=%u", sp); app_log_flush(); }
      return 0;
    }
  }
  int ok = 0;
  for (int a = 0; a < 4 && !ok; a++) {
    if (!rom_mon_icon_at(s_rommon, &s_iconloc, 0, (uint8_t*)s_stage)) { rumble_io_resume(); return 0; }
    uint32_t s1 = stage_sum();
    if (!rom_mon_icon_at(s_rommon, &s_iconloc, 0, (uint8_t*)s_stage)) { rumble_io_resume(); return 0; }
    ok = (stage_sum() == s1);
  }
  rumble_io_resume();
  if (!ok) { log_line("icons: rom read unstable sp=%u", sp); app_log_flush(); }
  *bank = s_iconloc.pal;                          /* ROM pals live in OBJ banks 0..2 */
  return 1;
}

/* compiled art first, else the user's ROM (staged into s_stage). Returns the tile
 * source or NULL; *from_rom tells the uploader the bytes are already verified RAM. */
static const uint8_t* icon_tiles(uint16_t species, uint8_t form, uint8_t frame,
                                 int egg, int* bank, int* from_rom) {
  const uint8_t* t; int b;
  *from_rom = 0;
  /* EMPTY SLOT. The compiled-art branch below already short-circuits on species==0, but
   * the ROM-streamed branch did NOT: species 0 is a VALID row of the icon table (the
   * "??????" dummy), so rom_icon_read_verified happily performed two full verified
   * 512-byte reads off the microSD — ~22 sectors — for a result boxoam_load_box then
   * discards. MEASURED on the artless build: 60 rom_mon_icon calls / 180 ROM reads per
   * box load REGARDLESS of occupancy, so an empty box cost exactly as much to open as a
   * full one. All four icon_tiles call sites already NULL-guard. */
  if (!egg && !species) return 0;
  if (egg ? mon_icon_oam_egg(&t, &b)
          : (species && mon_icon_oam_for_form_frame(species, form, frame, &t, &b))) {
    *bank = b; return t;
  }
  if (frame == 0 && rom_icon_read_verified(species, form, egg, bank)) {
    *from_rom = 1; return (const uint8_t*)s_stage;
  }
  return 0;
}

/* forward: upload_tiles_verified is defined below */
static void upload_tiles_verified(int tid, const void* src, int bytes);

/* Upload one icon: staged ROM bytes are already verified RAM (plain DMA); compiled
 * art goes through the staged-verify path as always. */
static void upload_icon(int tid, const uint8_t* tiles, int from_rom) {
  if (from_rom) upload_tiles(tid, tiles, MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
  else upload_tiles_verified(tid, tiles, MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
}

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
 * are free for the held mon's icon or the full-size item; restore the hand on the way back.
 * The hand has POSES now (normal glove / wide-open reach, for the retail grab dip), so
 * region A's state ids are 10+pose — distinct from 1 (item) and 2 (held mon), and a pose
 * change simply invalidates the region so the next load uploads the right tiles. */
static int s_hand_pose = BOXOAM_POSE_NORMAL;
static int s_cur_dy = 0;                /* grab/place dip offset (0 = rest) */
static int s_cur_dx = 0;                /* cursor-slide X offset (retail fix #4) */
void boxoam_hand_pose(int pose) { s_hand_pose = (pose >= 0 && pose <= 2) ? pose : BOXOAM_POSE_NORMAL; }
void boxoam_cursor_dy(int dy)   { s_cur_dy = dy; }
void boxoam_cursor_dxy(int dx, int dy) { s_cur_dx = dx; s_cur_dy = dy; }
static void load_rega_hand(void) {
  int want = 10 + s_hand_pose;
  if (s_rega == want) return;
  const uint8_t* t = s_hand_pose == BOXOAM_POSE_REACH  ? hand_oam_reach_tiles
                   : s_hand_pose == BOXOAM_POSE_BOUNCE ? hand_oam_bounce_tiles
                                                       : hand_oam_cursor_tiles;
  upload_tiles_verified(TID_HAND, t, HAND_OAM_TILES * HAND_OAM_TILE_BYTES);
  s_rega = want;
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
  const uint16_t* ic = carried_item ? app_item_icon(carried_item) : 0;   /* + the ROM rung */
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
  s_hand_pose = BOXOAM_POSE_NORMAL; s_cur_dy = 0;   /* no mid-beat leakage across screens */
  s_select_on = 0; s_chunk_on = 0; s_chunk_valid = 0;

  /* shared icon palettes -> banks 0..12 (416 bytes), staged + verified like the tiles
   * (a corrupted palette garbles every icon at once — the full-region jumble look) */
  { int n = MON_ICON_OAM_BANKS * MON_ICON_OAM_PALLEN;
    if (icopy_verified(s_stage, mon_icon_oam_pal, n) < 0)
      log_line("icons: palette unstable rom reads");        /* best-effort: last attempt still lands */
    for (int i = 0; i < n; i++) pal_obj_mem[i] = s_stage[i]; }
  /* Artless + the user's ROM: the compiled palettes above were weak zeros, so pull
   * the game's own 3 shared icon palettes into banks 0..2 — the banks the streamed
   * icons' palette ids (0..2) address. Compiled art present -> compiled wins. */
  { const uint8_t* t; int b;
    s_rom_icons = 0;
    if (s_rommon && !mon_icon_oam_for(1, &t, &b)) {
      uint16_t pd[16];
      for (int pnum = 0; pnum < ROM_MON_PALS; pnum++)
        if (rom_mon_icon_pal(s_rommon, pnum, pd))
          for (int i = 0; i < 16; i++) pal_obj_mem[pnum * 16 + i] = pd[i];
    } }

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
  s_rom_icons = 0;
  for (int s = 0; s < 30; s++) {
    int bank, from_rom;
    bool egg = box[s].isEgg && !box[s].isBadEgg;
    const uint8_t* tiles = icon_tiles(box[s].species, box[s].form, 0, egg, &bank, &from_rom);
    if ((egg || box[s].species) && tiles) {
      upload_icon(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles, from_rom);
      s_occupied[s] = 1; s_isegg[s] = egg ? 1 : 0; s_iconbank[s] = (uint8_t)bank;
      s_species[s] = box[s].species; s_form[s] = box[s].form;
      if (from_rom) s_rom_icons = 1;
      place_grid_slot(s);
    } else {
      s_occupied[s] = 0; s_isegg[s] = 0;
      hide(OE_ICON0 + s);
    }
  }
}

/* The real Gen-3 box "bob" — a 2-frame pose swap. DMA the chosen frame's tiles for every
 * occupied icon into the SAME OBJ VRAM window (the two frames can't both fit, so we swap).
 *
 * WHY THE TWO FRAMES CANNOT BOTH BE RESIDENT, since it keeps getting re-asked: the box
 * screen is a BITMAP mode, so OBJ tile memory is only the upper half, tiles 512..1023 =
 * 16 KiB. This module already spends all of it — 30 icons x 16 tiles = 480 (TID_ICON0),
 * plus 16 for the hand (TID_HAND 992) and 16 for region B (TID_REGB 1008) = exactly 512
 * tiles. There is no second frame's worth of VRAM, at any price.
 *
 * The re-upload itself is cheap and DOES fit the vblank: upload_tiles is dma3_cpy, so a
 * full box is 15,360 B = 3,840 words, roughly 9 K cycles against vblank's 83,776 — about
 * 11%. (The "cannot fit the vblank window" note at the top of this file is about
 * upload_tiles_VERIFIED, which reads every word back a second time. This path uses the
 * plain copy: the icons were already verified when the box was staged, and a bob tick
 * re-sends bytes that are known good.)
 *
 * Returns 1 if it animated, 0 if it could not — the caller uses that to fall back to the
 * 1 px positional bob rather than leaving the grid dead. */
int boxoam_set_frame(int frame) {
  /* CAPABILITY FIRST, BEFORE the already-on-that-frame short-circuit. Getting this
   * order wrong is not cosmetic: on a box that cannot pose-swap, s_frame never advances
   * past 0, so an equality test placed above this line answers "1, already there" on
   * every even tick. The caller then skips its boxoam_set_bob(0), the grid nudges down
   * on odd ticks and never comes back up, and the icons park 1 px low forever — worse
   * than the bob this was meant to replace. That shipped in 010ec90; this is the fix.
   *
   * Streamed icons have no frame-1 source in RAM, and there is nowhere to cache one:
   * frame 1 for a full box is 15 KiB, EWRAM has ~1.5 KiB free, and the only large
   * borrowable block (app_arena_acquire) IS g_pc — the very buffer this screen is
   * displaying. So the artless/ROM-icon build keeps the positional bob. */
  if (s_rom_icons) return 0;
  frame &= 1;
  if (frame == s_frame) return 1;               /* already there == animating fine */
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
  return 1;
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
  /* The glove art's pointing fingertip sits at sprite-local (13,0): hx+13 =
   * cell_x+16 -> hx = cell_x+3. The REST HEIGHT is retail's (hand at cellY-12,
   * docs/retail-pickup-capture.md §1d): high enough that the grab dip (+8) puts the
   * fingers ON the mon, the fist closes at that exact spot, and the carried mon
   * rides 4 px under the fist -> floating at cellY-8, all with one anchor and no
   * pop at any pose swap. */
  *hx = GRID_X + (cur % COLS) * CELL_W + 3;
  *hy = GRID_Y + (cur / COLS) * CELL_H - 12;
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
  else { hand_xy(cur, &hx, &hy); hx += s_cur_dx; hy += s_cur_dy; }   /* dip + slide ride the hand */

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
    REG_BLDCNT = PSS_BLDCNT;
    REG_BLDALPHA = PSS_BLDALPHA;                     /* retail: 7/16 obj + 11/16 below */
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
  /* RETAIL GEOMETRY (measured + decomp §1d of docs/retail-pickup-capture.md): the
   * fist is the HAND at its rest anchor with swapped tiles — same position, so the
   * open->fist swap never pops — drawn IN FRONT, with the carried mon riding 4 px
   * BELOW it. +s_cur_dy is the grab/place dip driver as before. */
  int hx, hy; hand_xy(cur, &hx, &hy);
  int fx = hx + s_cur_dx, fy = hy + s_cur_dy; if (fy < WP_Y) fy = WP_Y;
  int ix = fx - 3 + 0, iy = fy + 4;              /* mon: centred under the fist */
  load_regb_grab();                                  /* fist tiles -> region B */
  REG_BLDCNT = 0;                                    /* carried mon is opaque  */
  int bank = 0, from_rom = 0;
  const uint8_t* tiles = icon_tiles(species, form, 0, egg, &bank, &from_rom);
  if (tiles) {                                       /* the held mon (or Egg) rides the glove */
    upload_icon(TID_HAND, tiles, from_rom);
    s_rega = 2;                                      /* region A now holds the held mon */
    obj_set_attr(oe(OE_CARRY),                       /* the mon, BEHIND the fist */
                 ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
                 ATTR1_SIZE_32 | (ix & ATTR1_X_MASK),
                 ATTR2_ID(TID_HAND) | ATTR2_PRIO(1) | ATTR2_PALBANK(bank));
  } else hide(OE_CARRY);
  /* the closed hand IN FRONT, white like retail's, at the hand's own anchor */
  obj_set_attr(oe(OE_GRAB),
               ATTR0_SQUARE | ATTR0_4BPP | (fy & ATTR0_Y_MASK),
               ATTR1_SIZE_32 | (fx & ATTR1_X_MASK),
               ATTR2_ID(TID_GRAB) | ATTR2_PRIO(0) | ATTR2_PALBANK(PB_HAND));
  hide(OE_HAND);                                     /* hand hidden while carrying */
}

void boxoam_carry_end(void) { hide(OE_CARRY); hide(OE_GRAB); }   /* stop carrying */
void boxoam_hide_slot(int s) { if (s >= 0 && s < 30) hide(OE_ICON0 + s); }  /* lift-hide the origin */
/* Undo a lift-hide WITHOUT re-uploading: a plain hide only touched the OAM entry, the
 * slot's tiles are still in VRAM, so re-placing the attrs is enough. The grab dip uses
 * this to keep the mon visibly in its cell while the open hand descends onto it. */
void boxoam_show_slot(int s) { if (s >= 0 && s < 30 && s_occupied[s]) place_grid_slot(s); }

/* Re-upload grid slot s's own icon (current bob frame) from the per-slot bookkeeping
 * and re-show/hide it — undoes a chunk borrow of that slot's tile region. */
static void restore_slot(int s) {
  if (!s_occupied[s]) { hide(OE_ICON0 + s); return; }
  int bank, from_rom;
  const uint8_t* tiles = icon_tiles(s_species[s], s_form[s], (uint8_t)s_frame,
                                    s_isegg[s], &bank, &from_rom);
  if (tiles) {
    upload_icon(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles, from_rom);
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
  REG_BLDCNT = PSS_BLDCNT;
  REG_BLDALPHA = PSS_BLDALPHA;                       /* retail: 7/16 icon + 11/16 below */
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
    int bank = 0, from_rom = 0;
    const uint8_t* tiles = icon_tiles(mons[i].species, mons[i].form, 0, mons[i].egg,
                                      &bank, &from_rom);
    bool have = (tiles != 0);
    if (!s_covered[s]) {                             /* cover TRANSITION: put the occupant into
                                                      * the bitmap so the ghost blends over it */
      s_covered[s] = 1;
      if (s_occupied[s]) boxoam_under_show(s);
    }
    hide(OE_ICON0 + s);                              /* the block occludes this cell anyway */
    if (!have) { hide(OE_MARK0 + i); continue; }     /* no icon data -> same degrade as load_box */
    if (full) upload_icon(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles, from_rom);
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
  /* ONE transparency, not two. This used to ghost the carried block HARDER when it
   * could not be dropped here; retail has no such state — an impossible multi-move
   * just plays a failure sound and leaves the look alone, and drop_chunk already
   * calls snd_deny. The carried block still never goes solid until it is placed. */
  (void)fit;
  REG_BLDCNT = PSS_BLDCNT;
  REG_BLDALPHA = PSS_BLDALPHA;
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
     * orange grab fist behind it (region B, PRIO 1). Hand hidden.
     *
     * The fist is OPAQUE. Retail's cursor and the thing in its hand are both plain
     * ST_OAM_OBJ_NORMAL — the storage system's only alpha is on the item-less GRID
     * icons in MOVE-ITEMS mode. A see-through hand was ours, and it read as the hand
     * being the ghost rather than the icons it was hovering over. This also stops
     * boxoam_carry_item owning the blend registers at all: boxoam_cursor's ITEM branch
     * already set them on the frame before (oam_sync runs it first). */
    load_regb_item(item, true, TID_HAND); s_rega = 1;
    load_regb_grab();                                /* fist tiles -> region B */
    int ix = cx - (CITEM_OFF + 1), iy = cy - (CITEM_OFF - 1); if (iy < WP_Y) iy = WP_Y;
    int fx = cx + 3, fy = cy - 6; if (fy < WP_Y) fy = WP_Y;
    obj_set_attr(oe(OE_GRAB),                        /* orange grab fist, opaque, behind the item */
                 ATTR0_SQUARE | ATTR0_4BPP | (fy & ATTR0_Y_MASK),
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
