/* BACKLOG #91 M1-G2 -- Gen-2 MAP screen (read-only, current map only). See
 * pdna_gbmap2.h for the scope; mirrors pdna_gbmap.c's Gen-1 screen closely,
 * see docs/GB-MAP-DESIGN-G2.md §9 for exactly what transfers unchanged and
 * what differs. */
#include <tonc.h>
#include <string.h>

#ifndef PDNA_DELTA
#include "ff.h"               /* FIL, f_open/f_lseek/f_read/f_close, FA_READ   */
#endif

#include "pdna_gbmap2.h"
#include "rom_gbmap2.h"
#include "gb_lz.h"
#include "gb_fields.h"
#include "pdna_gbscreen.h"
#include "pdna_gen12.h"       /* gb12_arena_tail/gb12_arena_tail_release      */
#include "pdna_origin_art.h"  /* PDNA_GEN2                                    */
#include "pdna_app.h"         /* msg_wait, app_gb_rom_path, app_current_save_*/
#include "gb_art_source.h"    /* GB_ROM_PATH_MAX, gb_rom_path_beside          */
#include "pdna_layout.h"      /* PDNA_GBSCR_ACT_*, PDNA_GBMAP2_WRONG_GAME     */
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
static bool gbmap2_sd_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  FIL* f = (FIL*)ctx;
  UINT br = 0;
  if (!f || !buf) return false;
  if (f_lseek(f, (FSIZE_t)off) != FR_OK) return false;
  if (f_read(f, buf, (UINT)len, &br) != FR_OK) return false;
  return br == len;
}

static bool gbmap2_resolve_path(char* out, int cap) {
  const char* reg = app_gb_rom_path(PDNA_GEN2);
  if (reg && reg[0]) {
    int i = 0; for (; reg[i] && i < cap - 1; i++) out[i] = reg[i]; out[i] = 0;
    return true;
  }
  if (app_current_save_is_gb()) return gb_rom_path_beside(app_current_save_path(), PDNA_GEN2, out, cap);
  return false;
}
#endif

/* Cartridge header title check (design §10 risk 1, brief Step 4.8): the one
 * genuinely NEW silent-corruption risk in this slice. A Gold save with a
 * registered Crystal ROM (or vice-versa) would otherwise locate that ROM's
 * tables successfully and draw a plausible but WRONG map -- this is a
 * REFUSAL gate, never a locate-by-address. Measured titles (both corpus
 * ROMs): Gold "POKEMON_GLD", Crystal "PM_CRYSTAL\0". */
static bool gbmap2_title_matches(GbReadFn read, void* ctx, bool want_crystal) {
  uint8_t title[11];
  memset(title, 0, sizeof title);
  if (!read(ctx, 0x134u, title, sizeof title)) return false;
  if (want_crystal) return memcmp(title, "PM_CRYSTAL", 10) == 0;
  return memcmp(title, "POKEMON_GLD", 11) == 0 || memcmp(title, "POKEMON_SLV", 11) == 0;
}

/* ------------------------------------------------------------ view state */
#define VBW 5   /* viewport width, in BLOCKS  */
#define VBH 5   /* viewport height, in BLOCKS */
#define GBMAP2_META_CACHE_BYTES 2048u   /* max retail blockset (design §4)  */
#define GBMAP2_GFX_CACHE_BYTES  4096u   /* 256 tiles * 16 B (design §5.6)   */

/* A blockset byte is a VRAM tile id, not a decompressed-blob index -- Gen 2
 * splits the BG tile-id space across the GBC's two VRAM banks and Crystal
 * uses both (design §6, the single biggest trap in this slice). WITHOUT
 * this remap, Crystal's ICE_PATH_B2F_MAHOGANY_SIDE renders 86.1% blank. */
static inline uint16_t gbmap2_blob_tile(uint8_t id) {
  return (id < 0x80u) ? (uint16_t)id : (uint16_t)(id - 0x80u + 96u);
}

typedef struct {
  RomGbMap2     g;
  GbMap2Map     map;
  GbMap2Tileset ts;
  int           vbx, vby;               /* viewport origin, in BLOCKS       */
  uint8_t       block_ids[VBW * VBH];   /* this viewport's own block ids    */
  /* M1-G2 colour (BACKLOG #91, design §7.8) -- all NULL/0/false until
   * gbmap2_colour_setup() succeeds; the fixed grey ramp is the fallback
   * whenever it does not (anchor-miss or off-game, never a guess). Both
   * live on the STACK, as GbScreen itself does (see the fresh sizeof
   * measurements in this commit's own report). */
  RomGbMap2Colour colour;
  int             colour_ok;
  uint16_t        colour_pal[8 * 4];                    /* up to 8 palettes x 4 RGB15 */
  uint8_t         colour_palidx[GBSCR_COLS * GBSCR_ROWS]; /* one palette slot per cell  */
  /* review-opus D3: PalMap is one nibble per RAW tile id, 2 ids/byte -- ALL
   * 256 raw ids fit in 128 B. Cached ONCE at open (gbmap2_colour_setup())
   * so panning never re-reads it (was 360 single-byte reads per repaint --
   * one per painted CELL, not per unique tile -- on top of the viewport's
   * own 25; the brief's "panning costs zero ROM reads" promise was being
   * violated by colour alone). */
  uint8_t         palmap[128];
} GbMap2State;

/* Locate the 2 colour anchors and precompute up to 8 DAY palettes for
 * `st->map.environment` (fixed for the whole visit -- design §7.5/§7.8
 * pins DAY, no per-frame re-resolution). Applies the roof override
 * (design §7.4) to whichever precomputed palette's own bg_idx is 6
 * (PAL_BG_ROOF), iff the map's environment is TOWN/ROUTE. `scratch` is the
 * SAME buffer rgm2_open()'s own anchor scan already used and is about to
 * become the blockset cache -- colour locate runs strictly BEFORE that
 * reuse, same sequential-reuse argument the blockset cache itself already
 * documents. Returns false (colour_ok stays 0) on any anchor-miss -- never
 * a partial/guessed palette. */
static bool gbmap2_colour_setup(GbMap2State* st, uint8_t* scratch, uint32_t scratch_len) {
  if (!rgm2_colour_open(&st->colour, st->g.read, st->g.ctx, st->g.size, scratch, scratch_len))
    return false;
  for (int p = 0; p < 8; p++) {
    uint8_t bg_idx = 0;
    if (!rgm2_colour_palette(&st->colour, st->g.read, st->g.ctx, st->g.size,
                              st->map.environment, (uint8_t)p, st->colour_pal + p * 4, &bg_idx)) {
      memset(&st->colour, 0, sizeof st->colour);
      return false;
    }
    /* review-opus D1: PAL_BG_ROOF (=6) is the DESTINATION palette slot --
     * the PalMap NIBBLE `p`, the loop index this function is already
     * iterating over -- not `bg_idx` (the source row into TilesetBGPalette,
     * which the outdoor DAY row never sets to 6: $08,$09,$0a,$28,$0c,$0d,
     * $0e,$0f). Gating on bg_idx made this branch dead on every TOWN/ROUTE
     * map in both games. */
    if (p == 6 && (st->map.environment == 1u || st->map.environment == 2u)) {
      uint16_t roof_day[2] = { 0, 0 };
      if (rgm2_colour_roof(&st->colour, st->g.read, st->g.ctx, st->g.size,
                            st->map.environment, st->map.group, roof_day)) {
        /* review-opus D2: LoadMapPals copies BOTH roof words (`ld bc, 4`)
         * into slot 6's colours 1 and 2 -- word0 (morn) -> colour 1,
         * word1 (day) -> colour 2. Writing one value into both dropped
         * the entry's own first colour. */
        st->colour_pal[p * 4 + 1] = roof_day[0];
        st->colour_pal[p * 4 + 2] = roof_day[1];
      }
    }
  }
  /* review-opus D3: cache the WHOLE PalMap (128 B, all 256 raw ids) here,
   * ONCE, so gbmap2_paint() never issues another SD read for it -- this
   * used to be one single-byte read PER PAINTED CELL (360/repaint), not
   * per unique tile, violating "panning costs zero ROM reads" the moment
   * colour was added. Failure here fails the whole colour setup (never a
   * partial/guessed palette). */
  if (!rgm2_colour_palmap(&st->colour, st->g.read, st->g.ctx, st->g.size,
                           st->ts.pal_off, st->palmap)) {
    memset(&st->colour, 0, sizeof st->colour);
    return false;
  }
  return true;
}

/* Read the VBW*VBH block ids for the CURRENT viewport into st->block_ids.
 * Out-of-map cells get the map's OWN border block (design §9's "visible
 * improvement over Gen 1" -- Gen 1 had no border block and painted blank).
 * One 1-byte read per in-bounds cell, bounded at VBW*VBH == 25 reads. */
static void gbmap2_load_viewport_blocks(GbMap2State* st) {
  for (int i = 0; i < VBW * VBH; i++) st->block_ids[i] = st->map.border_block;
  for (int by = 0; by < VBH; by++) {
    int mby = st->vby + by;
    if (mby < 0 || mby >= st->map.height) continue;
    for (int bx = 0; bx < VBW; bx++) {
      int mbx = st->vbx + bx;
      if (mbx < 0 || mbx >= st->map.width) continue;
      uint8_t id = st->map.border_block;
      st->g.read(st->g.ctx, st->map.blocks_off + (uint32_t)mby * st->map.width + (uint32_t)mbx, &id, 1);
      st->block_ids[by * VBW + bx] = id;
    }
  }
}

/* Paint the whole 20x18 canvas from st->block_ids + the CACHED blockset
 * (`meta_cache`, already fully resident -- no SD read here, design §9's
 * "panning costs zero ROM reads"). Every blockset byte goes through the
 * VRAM tile-id remap before reaching the shell. */
static void gbmap2_paint(GbScreen* gs, GbMap2State* st, const uint8_t* meta_cache, uint32_t meta_len) {
  for (int sy = 0; sy < GBSCR_ROWS; sy++) {
    int by = sy / 4, ty = sy % 4;
    for (int sx = 0; sx < GBSCR_COLS; sx++) {
      int bx = sx / 4, tx = sx % 4;
      int cell = sy * GBSCR_COLS + sx;
      uint8_t block_id = st->block_ids[by * VBW + bx];
      uint32_t block_off = (uint32_t)block_id * 16u;
      if (block_off + 16u > meta_len) { gbscr_cell(gs, sx, sy, GBSCR_SRC_BLANK, 0); continue; }
      uint8_t raw_id = meta_cache[block_off + (uint32_t)(ty * 4 + tx)];
      uint16_t blob_tile = gbmap2_blob_tile(raw_id);
      /* M1-G2 colour (design §7.8): resolve which of the up to 8
       * precomputed palettes this cell uses (design §7.2: palette_index =
       * palmap_nibble(RAW tile id) & 7 -- the raw id, NOT the §6-remapped
       * blob index, since bit 3 of the nibble is the same VRAM-bank
       * selector the remap already encodes) from the CACHED st->palmap
       * (review-opus D3) -- no ROM read here at all, paint or repaint.
       * Only runs when colour located; otherwise the shell's own grey-ramp
       * fallback is unaffected. */
      if (st->colour_ok) {
        uint8_t b = st->palmap[raw_id >> 1];
        uint8_t nib = (raw_id & 1u) ? (uint8_t)((b >> 4) & 0x0Fu) : (uint8_t)(b & 0x0Fu);
        st->colour_palidx[cell] = (uint8_t)(nib & 0x07u);
      }
      gbscr_cell(gs, sx, sy, GBSCR_SRC_MAPTILES, (uint8_t)(blob_tile & 0xFFu));
    }
  }
  gbscr_text(gs, 0, GBSCR_ROWS - 1, PDNA_GBMAP_HINT);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void pdna_gbmap_gen2(GbSession* s) {
  if (!s || s->gen != GB_GEN2) return;

  bool want_crystal = gb_session_is_crystal(s);
  GbGame game = want_crystal ? GBF_G_CRYSTAL : GBF_G_GS;

  uint32_t goff = gbf_off(game, GBF_MAP_GROUP);
  uint32_t noff = gbf_off(game, GBF_MAP_NUMBER);
  uint32_t xoff = gbf_off(game, GBF_POS_X);
  uint32_t yoff = gbf_off(game, GBF_POS_Y);
  uint8_t group = 0, number = 0, px = 0, py = 0;
  if (!goff || gbs_read_field(s, goff, &group, 1) != GBS_OK ||
      !noff || gbs_read_field(s, noff, &number, 1) != GBS_OK ||
      !xoff || gbs_read_field(s, xoff, &px, 1) != GBS_OK ||
      !yoff || gbs_read_field(s, yoff, &py, 1) != GBS_OK) {
    ui_clear();
    snd_deny();
    s_msg("MAP", UI_WARN, "Could not read the player's", "own position.");
    return;
  }

  uint32_t shell_need = gbscr_tail_need(PDNA_GEN2, 0);
  uint32_t scratch_len = ROM_GBMAP2_SCRATCH_MIN > GBMAP2_META_CACHE_BYTES
                          ? ROM_GBMAP2_SCRATCH_MIN : GBMAP2_META_CACHE_BYTES;
  /* Locate-scan scratch and the blockset cache are used SEQUENTIALLY
   * (locate fully finishes before the first blockset read, same argument
   * pdna_gbmap.c makes for Gen 1) so they SHARE `scratch_len` bytes; the
   * decompressed GFX cache is held SEPARATELY because it must stay
   * resident WHILE the blockset cache is also in use during painting
   * (design doc's own "Grilling 2026-09-11" A7 correction). */
  uint32_t extra = scratch_len + GBMAP2_GFX_CACHE_BYTES;
  uint32_t need = shell_need + extra;
  uint8_t* tail = gb12_arena_tail(need);

  GbScreen gs;
  const char* reason = 0;
  bool ok = gbscr_open(PDNA_GEN2, &gs, tail, shell_need, 0, &reason);
  if (!ok) {
    gb12_arena_tail_release();
    ui_clear();
    snd_deny();
    s_msg("MAP", UI_WARN, reason ? reason : PDNA_GBSCR_REASON_UNAVAILABLE, 0);
    return;
  }

  uint8_t* meta_cache = tail + shell_need;                 /* scratch, then blockset cache */
  uint8_t* gfx_cache   = meta_cache + scratch_len;          /* decompressed GFX, held separately */

  GbMap2State st;
  memset(&st, 0, sizeof st);

  bool fil_opened = false;        /* did we manage to open SOME ROM file at all */
  bool title_ok = false;          /* the ROM's own header title matches the save's game */
  bool located = false;           /* rgm2_open() succeeded */

#ifndef PDNA_DELTA
  char path[GB_ROM_PATH_MAX];
  FIL fil; memset(&fil, 0, sizeof fil);
  bool fil_open = gbmap2_resolve_path(path, (int)sizeof path) &&
                  f_open(&fil, path, FA_READ) == FR_OK;
  fil_opened = fil_open;
  uint32_t romsz = 0;
  if (fil_open) { FSIZE_t fsz = f_size(&fil); romsz = (fsz > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)fsz; }
  if (fil_open) title_ok = gbmap2_title_matches(gbmap2_sd_read, &fil, want_crystal);
  if (title_ok) located = rgm2_open(&st.g, gbmap2_sd_read, &fil, romsz, meta_cache, scratch_len);
#else
  const uint8_t* base = 0; uint32_t romsz = 0;
  fil_opened = fused_gb_rom(PDNA_GEN2, &base, &romsz);
  FusedGbSlice slice = { base, romsz };
  if (fil_opened) title_ok = gbmap2_title_matches(fused_gb_slice_read, &slice, want_crystal);
  if (title_ok) located = rgm2_open(&st.g, fused_gb_slice_read, &slice, romsz, meta_cache, scratch_len);
#endif

  bool map_ok = located && rgm2_map(&st.g, group, number, &st.map) &&
                rgm2_tileset(&st.g, st.map.tileset_id, &st.ts);

#ifndef PDNA_DELTA
  if (fil_open) f_close(&fil);
#endif

  if (!map_ok) {
    gbscr_close(&gs);
    gb12_arena_tail_release();
    ui_clear();
    snd_deny();
    const char* why;
    if (!fil_opened) why = "Could not open the ROM.";
    else if (!title_ok) why = PDNA_GBMAP2_WRONG_GAME;
    else if (!located) why = "This ROM could not be read.";
    else why = "This map could not be read.";
    s_msg("MAP", UI_WARN, why, 0);
    return;
  }

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
  st.g.read = gbmap2_sd_read;
  st.g.ctx = &fil;
#endif

  /* Decompress the WHOLE tileset GFX ONCE, here, not lazily -- streaming is
   * impossible (design §5.4: back-references reach absolute offset 0 of the
   * output). This IS the tile cache from here on; panning costs zero ROM
   * reads (design §9). */
  uint32_t gfx_len = gb_lz_decode(st.g.read, st.g.ctx, st.ts.gfx_off,
                                   st.g.size - st.ts.gfx_off, gfx_cache, GBMAP2_GFX_CACHE_BYTES);

  /* M1-G2 colour (BACKLOG #91, design §7.8): locate + precompute BEFORE the
   * blockset cache overwrites `meta_cache` -- this is the SAME
   * scratch-then-cache sequential reuse rgm2_open()'s own anchor scan
   * already establishes, extended by one more sequential user. Anchor-miss
   * (wrong ROM shape) leaves colour_ok false and the shell's own fixed
   * grey ramp paints instead -- never a guessed address. */
  st.colour_ok = gbmap2_colour_setup(&st, meta_cache, scratch_len) ? 1 : 0;

  /* Cache the WHOLE blockset (<=2048 B) too -- block -> 16 tile ids becomes
   * a memory index, not an SD read (design §4/§9). */
  bool meta_ok = st.ts.meta_len > 0 && st.ts.meta_len <= scratch_len &&
                 st.g.read(st.g.ctx, st.ts.meta_off, meta_cache, st.ts.meta_len);

  if (gfx_len == 0 || !meta_ok) {
#ifndef PDNA_DELTA
    f_close(&fil);
#endif
    gbscr_close(&gs);
    gb12_arena_tail_release();
    ui_clear();
    snd_deny();
    s_msg("MAP", UI_WARN, "This map's tile graphics", "could not be decoded.");
    return;
  }

  gs.cache.maptiles = gfx_cache;
  gs.cache.maptiles_n = (uint16_t)(gfx_len / 16u);
  if (st.colour_ok) {
    gs.cache.maptiles_pal = st.colour_pal;
    gs.cache.maptiles_palidx = st.colour_palidx;
  }

  int block_px = gbmap_block_of(px), block_py = gbmap_block_of(py);
  st.vbx = clampi(block_px - VBW / 2, 0, st.map.width  > VBW ? st.map.width  - VBW : 0);
  st.vby = clampi(block_py - VBH / 2, 0, st.map.height > VBH ? st.map.height - VBH : 0);

  static const char* const kLegend[4] = { 0, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, 0 };
  gbscr_set_legend(&gs, kLegend);

  gbmap2_load_viewport_blocks(&st);
  gbmap2_paint(&gs, &st, meta_cache, st.ts.meta_len);

  for (;;) {
    gbscr_flush(&gs, 0);

    int rel_bx = block_px - st.vbx, rel_by = block_py - st.vby;
    if (rel_bx >= 0 && rel_bx < VBW && rel_by >= 0 && rel_by < VBH) {
      int px0, py0, px1, py1;
      gbscr_cell_rect(rel_bx * 4, rel_by * 4, 4, 4, &px0, &py0, &px1, &py1);
      m3_frame(px0, py0, px1, py1, RGB15(31, 4, 4));
    }

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_B | KEY_SELECT);
    if (k & KEY_B) break;
    if (k & (KEY_SELECT | KEY_L | KEY_R)) { gbscr_toggle_scale(&gs); continue; }

    int nvbx = st.vbx, nvby = st.vby;
    if (k & KEY_LEFT)  nvbx--;
    if (k & KEY_RIGHT) nvbx++;
    if (k & KEY_UP)    nvby--;
    if (k & KEY_DOWN)  nvby++;
    nvbx = clampi(nvbx, 0, st.map.width  > VBW ? st.map.width  - VBW : 0);
    nvby = clampi(nvby, 0, st.map.height > VBH ? st.map.height - VBH : 0);
    if (nvbx != st.vbx || nvby != st.vby) {
      st.vbx = nvbx; st.vby = nvby;
      gbmap2_load_viewport_blocks(&st);
      gbmap2_paint(&gs, &st, meta_cache, st.ts.meta_len);
      gbscr_mark_all_dirty(&gs);
    }
  }

#ifndef PDNA_DELTA
  f_close(&fil);
#endif
  gbscr_close(&gs);
  gb12_arena_tail_release();
}
