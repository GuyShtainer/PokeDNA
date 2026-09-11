/* BACKLOG #91 M1 -- Gen-1 MAP screen (read-only, current map only). See
 * pdna_gbmap.h for the scope. */
#include <tonc.h>
#include <string.h>

#ifndef PDNA_DELTA
#include "ff.h"               /* FIL, f_open/f_lseek/f_read/f_close, FA_READ   */
#endif

#include "pdna_gbmap.h"
#include "rom_gbmap.h"
#include "gb_fields.h"
#include "pdna_gbscreen.h"
#include "pdna_gen12.h"       /* gb12_arena_tail/gb12_arena_tail_release      */
#include "pdna_origin_art.h"  /* PDNA_GEN1                                    */
#include "pdna_app.h"         /* msg_wait, app_gb_rom_path, app_current_save_*/
#include "gb_art_source.h"    /* GB_ROM_PATH_MAX, gb_rom_path_beside          */
#include "pdna_layout.h"      /* PDNA_GBSCR_ACT_*/
#include "ui.h"
#include "snd.h"

#ifdef PDNA_DELTA
#include "fused_gb.h"
#endif

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16 s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
  else if (k & KEY_B) snd_back();
  return k;
}
static void s_msg(const char* title, u16 ink, const char* l1, const char* l2) {
  msg_wait(title, ink, l1, l2);
}

/* ---------------------------------------------------------------- ROM I/O */
#ifndef PDNA_DELTA
static bool gbmap_sd_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  FIL* f = (FIL*)ctx;
  UINT br = 0;
  if (!f || !buf) return false;
  if (f_lseek(f, (FSIZE_t)off) != FR_OK) return false;
  if (f_read(f, buf, (UINT)len, &br) != FR_OK) return false;
  return br == len;
}

/* Same order gbscr_resolve_path() (pdna_gbscreen.c, static to that TU) uses
 * -- duplicated here via the PUBLIC accessors it itself is built on, since
 * that helper is not exported. */
static bool gbmap_resolve_path(char* out, int cap) {
  const char* reg = app_gb_rom_path(PDNA_GEN1);
  if (reg && reg[0]) {
    int i = 0; for (; reg[i] && i < cap - 1; i++) out[i] = reg[i]; out[i] = 0;
    return true;
  }
  if (app_current_save_is_gb()) return gb_rom_path_beside(app_current_save_path(), PDNA_GEN1, out, cap);
  return false;
}
#endif

/* ------------------------------------------------------------ view state */
#define VBW 5   /* viewport width, in BLOCKS (5*4 = 20 tiles == GBSCR_COLS)  */
#define VBH 5   /* viewport height, in BLOCKS (5*4 = 20 tiles, clipped to
                 * GBSCR_ROWS=18 -- the bottom 2 tile rows of the 5th block
                 * row are simply never iterated)                            */
#define MAPTILE_CACHE_TILES 256
#define MAPTILE_CACHE_BYTES (MAPTILE_CACHE_TILES * 16u)   /* 4,096 -- design's own budget */

typedef struct {
  RomGbMap1     g;
  GbMap1Header  hdr;
  GbMap1Tileset ts;
  int           vbx, vby;      /* viewport origin, in BLOCKS               */
  uint8_t       block_ids[VBW * VBH];   /* this viewport's own block ids    */
  uint8_t       tile_seen[MAPTILE_CACHE_TILES];   /* decoded-this-visit set */
} GbMapState;

/* Read the VBW*VBH block ids for the CURRENT viewport into st->block_ids
 * (out-of-map positions get block id 0xFF, which never decodes -- the
 * render loop treats it as BLANK). One 1-byte read per in-bounds cell,
 * bounded at VBW*VBH == 25 reads, never the whole map. */
static void gbmap_load_viewport_blocks(GbMapState* st) {
  memset(st->block_ids, 0xFF, sizeof st->block_ids);
  for (int by = 0; by < VBH; by++) {
    int mby = st->vby + by;
    if (mby < 0 || mby >= st->hdr.height) continue;
    for (int bx = 0; bx < VBW; bx++) {
      int mbx = st->vbx + bx;
      if (mbx < 0 || mbx >= st->hdr.width) continue;
      uint8_t id = 0xFF;
      st->g.read(st->g.ctx, st->hdr.blocks_off + (uint32_t)mby * st->hdr.width + (uint32_t)mbx, &id, 1);
      st->block_ids[by * VBW + bx] = id;
    }
  }
}

/* Ensure `tile_id`'s 16 raw 2bpp bytes are decoded into `maptiles` (a no-op
 * if already decoded this visit -- SD reads happen at most once per unique
 * tile id per screen visit, never per repaint/per-cell). */
static void gbmap_ensure_tile(GbMapState* st, uint8_t* maptiles, uint8_t tile_id) {
  if (st->tile_seen[tile_id]) return;
  if (rgm1_tile2bpp(&st->g, &st->ts, tile_id, maptiles + (uint32_t)tile_id * 16u))
    st->tile_seen[tile_id] = 1;
}

/* Paint the whole 20x18 canvas from st->block_ids + the tileset's blockset,
 * lazily decoding any newly-seen tile id into `maptiles`. */
static void gbmap_paint(GbScreen* gs, GbMapState* st, uint8_t* maptiles) {
  for (int sy = 0; sy < GBSCR_ROWS; sy++) {
    int by = sy / 4, ty = sy % 4;
    for (int sx = 0; sx < GBSCR_COLS; sx++) {
      int bx = sx / 4, tx = sx % 4;
      uint8_t block_id = st->block_ids[by * VBW + bx];
      if (block_id == 0xFF) { gbscr_cell(gs, sx, sy, GBSCR_SRC_BLANK, 0); continue; }
      uint8_t tiles16[16];
      if (!rgm1_block(&st->g, &st->ts, block_id, tiles16)) {
        gbscr_cell(gs, sx, sy, GBSCR_SRC_BLANK, 0);
        continue;
      }
      uint8_t tile_id = tiles16[ty * 4 + tx];
      gbmap_ensure_tile(st, maptiles, tile_id);
      if (!st->tile_seen[tile_id]) { gbscr_cell(gs, sx, sy, GBSCR_SRC_BLANK, 0); continue; }   /* read failed: blank, never scratch bytes (m1 review D9) */
      gbscr_cell(gs, sx, sy, GBSCR_SRC_MAPTILES, tile_id);
    }
  }
  /* L/R are this screen's SIZE toggle, not a pan key (m1 review D6) -- there
   * is no legend slot for that, so the hint lives in the map's own body,
   * overlaid on the last row every repaint. */
  gbscr_text(gs, 0, GBSCR_ROWS - 1, PDNA_GBMAP_HINT);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void pdna_gbmap_gen1(GbSession* s) {
  if (!s || s->gen != GB_GEN1) return;

  /* Player's current map + position -- same GBF_MAP_ID/POS_X/POS_Y offsets
   * gb_fields.c already carries for every other Gen-1 screen (Red/Yellow
   * share one SRAM block, gb_fields.c's own D() derivation, cross-checked
   * live against Red.sav/Yellow.sav by tests/host_romgbmap_test.c). */
  uint32_t moff = gbf_off(GBF_G_RED, GBF_MAP_ID);
  uint32_t xoff = gbf_off(GBF_G_RED, GBF_POS_X);
  uint32_t yoff = gbf_off(GBF_G_RED, GBF_POS_Y);
  uint8_t map_id = 0, px = 0, py = 0;
  if (!moff || gbs_read_field(s, moff, &map_id, 1) != GBS_OK ||
      !xoff || gbs_read_field(s, xoff, &px, 1) != GBS_OK ||
      !yoff || gbs_read_field(s, yoff, &py, 1) != GBS_OK) {
    ui_clear();
    snd_deny();
    s_msg("MAP", UI_WARN, "Could not read the player's", "own position.");
    return;
  }

  uint32_t shell_need = gbscr_tail_need(PDNA_GEN1, 0);
  uint32_t extra = ROM_GBMAP_SCRATCH_MIN > MAPTILE_CACHE_BYTES
                    ? ROM_GBMAP_SCRATCH_MIN : MAPTILE_CACHE_BYTES;
  /* Scratch (locate scan) and the final tile cache are used SEQUENTIALLY
   * (locate finishes fully before the first tile decode), so they SHARE the
   * same `extra` bytes rather than summing -- `extra` is sized to the
   * larger of the two needs. */
  uint32_t need = shell_need + extra;
  uint8_t* tail = gb12_arena_tail(need);

  GbScreen gs;
  const char* reason = 0;
  bool ok = gbscr_open(PDNA_GEN1, &gs, tail, shell_need, 0, &reason);
  if (!ok) {
    gb12_arena_tail_release();
    ui_clear();
    snd_deny();
    s_msg("MAP", UI_WARN, reason ? reason : PDNA_GBSCR_REASON_UNAVAILABLE, 0);
    return;
  }

  uint8_t* scratch_or_cache = tail + shell_need;   /* `extra` bytes, reused  */

  GbMapState st;
  memset(&st, 0, sizeof st);

#ifndef PDNA_DELTA
  char path[GB_ROM_PATH_MAX];
  FIL fil; memset(&fil, 0, sizeof fil);
  bool fil_open = gbmap_resolve_path(path, (int)sizeof path) &&
                  f_open(&fil, path, FA_READ) == FR_OK;
  bool rom_ok = fil_open;
  uint32_t romsz = 0;
  if (rom_ok) { FSIZE_t fsz = f_size(&fil); romsz = (fsz > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)fsz; }
  if (rom_ok) rom_ok = rgm1_open(&st.g, gbmap_sd_read, &fil, romsz, scratch_or_cache, ROM_GBMAP_SCRATCH_MIN);
#else
  const uint8_t* base = 0; uint32_t romsz = 0;
  bool rom_ok = fused_gb_rom(PDNA_GEN1, &base, &romsz);
  FusedGbSlice slice = { base, romsz };
  if (rom_ok) rom_ok = rgm1_open(&st.g, fused_gb_slice_read, &slice, romsz, scratch_or_cache, ROM_GBMAP_SCRATCH_MIN);
#endif

  bool map_ok = rom_ok && rgm1_header(&st.g, map_id, &st.hdr) &&
                rgm1_tileset(&st.g, st.hdr.tileset_id, &st.ts);

#ifndef PDNA_DELTA
  if (fil_open) f_close(&fil); /* even when rgm1_open failed (m1 re-verify N4); rgm1_open's own reads are all done by now -- the
                                 * viewport/tile decode below re-opens per read via
                                 * st.g.ctx, which now dangles: reopen before use  */
#endif

  if (!map_ok) {
    gbscr_close(&gs);
    gb12_arena_tail_release();
    ui_clear();
    snd_deny();
    s_msg("MAP", UI_WARN, rom_ok ? "This map could not be read." : "Could not open the ROM.", 0);
    return;
  }

  /* Reopen the ROM for the viewport/tile reads below -- rgm1_open() above
   * closed over the FIL (SD build) that is no longer valid; a fresh FIL
   * (SD) or the SAME fused slice (delta, still valid -- it is a plain
   * pointer into cartridge space, never closed) backs every read from here
   * on. Same "close after the locate pass, reopen for the decode pass"
   * shape gbscr_decode_pic_gen1() already uses one level up. `fil` (above)
   * is already closed and its lifetime is over by this point -- reused here
   * instead of a second FIL (608 B) so only one is ever live on the stack
   * (m1 review D5). */
#ifndef PDNA_DELTA
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) {
    gbscr_close(&gs);
    gb12_arena_tail_release();
    ui_clear();
    snd_deny();
    s_msg("MAP", UI_WARN, "Could not reopen the ROM.", 0);
    return;
  }
  st.g.read = gbmap_sd_read;
  st.g.ctx = &fil;
#endif

  uint8_t* maptiles = scratch_or_cache;   /* scan scratch's job is done; reuse */
  gs.cache.maptiles = maptiles;
  gs.cache.maptiles_n = MAPTILE_CACHE_TILES;

  int block_px = gbmap_block_of(px), block_py = gbmap_block_of(py);   /* see
                                                * rom_gbmap.h's own gbmap_block_of()
                                                * doc comment for why this is ONE
                                                * halving, not two (m1 review D1). */
  st.vbx = clampi(block_px - VBW / 2, 0, st.hdr.width  > VBW ? st.hdr.width  - VBW : 0);
  st.vby = clampi(block_py - VBH / 2, 0, st.hdr.height > VBH ? st.hdr.height - VBH : 0);

  static const char* const kLegend[4] = { 0, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, 0 };
  gbscr_set_legend(&gs, kLegend);

  gbmap_load_viewport_blocks(&st);
  gbmap_paint(&gs, &st, maptiles);

  for (;;) {
    gbscr_flush(&gs, 0);

    /* Player marker: a frame around the player's own on-screen block, iff
     * it is inside the currently-loaded viewport. */
    int rel_bx = block_px - st.vbx, rel_by = block_py - st.vby;
    if (rel_bx >= 0 && rel_bx < VBW && rel_by >= 0 && rel_by < VBH) {
      int px0, py0, px1, py1;
      gbscr_cell_rect(rel_bx * 4, rel_by * 4, 4, 4, &px0, &py0, &px1, &py1);
      m3_frame(px0, py0, px1, py1, RGB15(31, 4, 4));
    }

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_B | KEY_SELECT);
    if (k & KEY_B) break;
    /* L/R are the shell's own SIZE toggle here (same as SELECT), matching
     * the shell-wide "L/R or SELECT" scale convention every other GB screen
     * uses -- the D-pad ALONE pans this screen, not a separate mode (m1
     * review D6; pdna_map.c's L/R zoom-out/in is Guy's own Gen-3 mapping and
     * stays exactly as-is, this is Gen-1's own screen). */
    if (k & (KEY_SELECT | KEY_L | KEY_R)) { gbscr_toggle_scale(&gs); continue; }

    int nvbx = st.vbx, nvby = st.vby;
    if (k & KEY_LEFT)  nvbx--;
    if (k & KEY_RIGHT) nvbx++;
    if (k & KEY_UP)    nvby--;
    if (k & KEY_DOWN)  nvby++;
    nvbx = clampi(nvbx, 0, st.hdr.width  > VBW ? st.hdr.width  - VBW : 0);
    nvby = clampi(nvby, 0, st.hdr.height > VBH ? st.hdr.height - VBH : 0);
    if (nvbx != st.vbx || nvby != st.vby) {
      st.vbx = nvbx; st.vby = nvby;
      gbmap_load_viewport_blocks(&st);
      gbmap_paint(&gs, &st, maptiles);
      gbscr_mark_all_dirty(&gs);
    }
  }

#ifndef PDNA_DELTA
  f_close(&fil);
#endif
  gbscr_close(&gs);
  gb12_arena_tail_release();
}
