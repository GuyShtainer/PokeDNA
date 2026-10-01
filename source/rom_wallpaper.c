/* Box wallpapers out of the user's own ROM. See rom_wallpaper.h for the design, the
 * measured pins, and the composition-question note.
 *
 * ---- WHAT THIS COSTS IN EWRAM -------------------------------------------------
 * Nothing new. rom_wallpaper_tiles' worst case (ROM_WP_TILES_MAX_BYTES = 4,096 B)
 * is under HALF of the shared 8,192 B artbuf (source/artbuf.h) every other ROM-art
 * module already uses as its scratch, so pdna_box.c's ROM rung decompresses
 * straight into `mon_decomp` the same way rom_sprite.c / rom_itemart.c do. The
 * per-tile RGB15 expansion (rom_wallpaper_expand_tile) writes 64 u16 -- the SAME
 * 128 B `s_wp_tile` stage buffer pdna_box.c's existing wp_copy_verified path
 * already uses for every source (compiled and ROM alike).
 *
 * ---- WHY THIS MODULE NEVER DEDUPES TILES ---------------------------------------
 * DESIGN.md Sec 4.4 inverts the draw loop (tiles outer, map cells inner) so the SD
 * CACHE can be read strictly forward, one already-deduped 128 B RGB15 tile at a
 * time. That trick exists to avoid ever holding a whole wallpaper's tiles in RAM.
 * This module has the opposite shape: it decompresses the WHOLE raw 4bpp tile
 * array in one shot (it fits, see above) and then serves any tile by INDEX,
 * O(1), with no dedup and no reordering at all -- the caller (pdna_box.c) can
 * therefore keep its existing cell-order loop unmodified for this rung too,
 * just resolving one more attribute (hflip/vflip/bank) per cell than the
 * compiled path does. Simpler, and correct: dedup is an optimisation for a
 * source that cannot hold everything at once, and this source can.
 */
#include "rom_wallpaper.h"

#include <string.h>
#include "map_render.h"   /* mr_lz77 / mr_lz77_size -- do not write another */

typedef struct { const char* code; uint8_t version; RomKind kind; uint32_t table; } RomWpPin;

/* Exactly the three dumps DESIGN.md Sec 1.2 measured -- see rom_wallpaper.h SCOPE. */
static const RomWpPin k_pins[] = {
  { "BPEE", 0, ROM_EMERALD,   0x085775B8u },
  { "BPRE", 1, ROM_FIRERED,   0x083D2A80u },
  { "BPGE", 0, ROM_LEAFGREEN, 0x083D284Cu },
};
#define K_NPINS ((int)(sizeof k_pins / sizeof k_pins[0]))

#define ROW_BYTES 12u   /* { const u32* tiles; const u32* tilemap; const u16* palettes } */

static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Read row `wp`'s three pointers. Returns 1 on a read that stayed inside the image
 * for all three; does NOT validate the pointers are real LZ77 blobs (the open-time
 * self-check does that for row 0; a caller decompressing a later row finds out via
 * mr_lz77 returning 0, same as any other bad-pointer failure in this codebase). */
static int read_row(const RomCtx* rc, uint32_t table, int wp, uint32_t* tiles,
                    uint32_t* tilemap, uint32_t* pals) {
  uint8_t b[ROW_BYTES];
  if (!rom_read_at(rc, table + (uint32_t)wp * ROW_BYTES, b, ROW_BYTES)) return 0;
  *tiles = rd32(b); *tilemap = rd32(b + 4); *pals = rd32(b + 8);
  if (!rom_ptr_ok(rc, *tiles) || !rom_ptr_ok(rc, *tilemap) || !rom_ptr_ok(rc, *pals)) return 0;
  return 1;
}

int rom_wallpaper_open(RomWallpaper* rw, const RomCtx* rc) {
  if (!rw) return 0;
  memset(rw, 0, sizeof *rw);
  if (!rc) return 0;
  rw->rc = rc;
  for (int i = 0; i < K_NPINS; i++) {
    if (rc->kind != k_pins[i].kind) continue;
    if (rc->version != k_pins[i].version) continue;
    if (memcmp(rc->code, k_pins[i].code, 4) != 0) continue;
    uint32_t tiles, tilemap, pals;
    if (!read_row(rc, k_pins[i].table, 0, &tiles, &tilemap, &pals)) return 0;
    if (mr_lz77_size(rc, tilemap) != ROM_WP_MAP_BYTES) return 0;   /* self-check */
    rw->table = k_pins[i].table;
    rw->ok = 1;
    return 1;
  }
  return 0;   /* no pin for this game/revision -- fail closed, same as R/S elsewhere */
}

int rom_wallpaper_map(const RomWallpaper* rw, int wp, uint16_t dst[ROM_WP_MAP_ENTRIES]) {
  if (!rw || !rw->ok || !dst || wp < 0 || wp >= ROM_WP_COUNT) return 0;
  uint32_t tiles, tilemap, pals;
  if (!read_row(rw->rc, rw->table, wp, &tiles, &tilemap, &pals)) return 0;
  /* dst is a properly 2-aligned uint16_t[]; mr_lz77 writes it byte-by-byte, which is
   * always safe regardless of destination "type" (map_render.c does the same into
   * VRAM-adjacent buffers elsewhere) -- no pointer-cast read ever happens here. */
  uint32_t n = mr_lz77(rw->rc, tilemap, (uint8_t*)dst, ROM_WP_MAP_BYTES);
  return n == ROM_WP_MAP_BYTES;
}

int rom_wallpaper_tiles(const RomWallpaper* rw, int wp, uint8_t* dst, uint32_t dst_cap,
                        uint32_t* out_bytes) {
  if (out_bytes) *out_bytes = 0;
  if (!rw || !rw->ok || !dst || wp < 0 || wp >= ROM_WP_COUNT) return 0;
  uint32_t tiles, tilemap, pals;
  if (!read_row(rw->rc, rw->table, wp, &tiles, &tilemap, &pals)) return 0;
  uint32_t n = mr_lz77(rw->rc, tiles, dst, dst_cap);
  if (!n || (n % 32u) != 0) return 0;
  if (out_bytes) *out_bytes = n;
  return 1;
}

int rom_wallpaper_pal(const RomWallpaper* rw, int wp, uint16_t dst[ROM_WP_PAL_BANKS][16]) {
  if (!rw || !rw->ok || !dst || wp < 0 || wp >= ROM_WP_COUNT) return 0;
  uint32_t tiles, tilemap, pals;
  if (!read_row(rw->rc, rw->table, wp, &tiles, &tilemap, &pals)) return 0;
  memset(dst, 0, ROM_WP_PAL_BANKS * 16 * sizeof(uint16_t));
  uint8_t raw[ROM_WP_PAL_BYTES];
  /* The palette blob is plain (uncompressed) raw RGB15, like every other shared
   * palette table this codebase reads (rom_mon_icon_pal, item icon palettes).
   * ROM_WP_PAL_BYTES is exactly the row's real length (64 B = 2 banks, MEASURED:
   * on Emerald wp 0 this blob is immediately followed by the tiles blob's own
   * LZ77 header at +64 -- see rom_wallpaper.h's top-of-file note), so this read
   * never runs into the next blob's compressed bytes. */
  if (!rom_read_at(rw->rc, pals, raw, sizeof raw)) return 0;
  for (int bk = 0; bk < ROM_WP_PAL_BANKS; bk++)
    for (int c = 0; c < 16; c++)
      dst[bk][c] = (uint16_t)(rd16(raw + bk * 32 + c * 2) & 0x7FFF);
  return 1;
}

int rom_wallpaper_pal_bank(int bank) {
  int pb = (bank <= 1) ? 0 : bank - 1;
  if (pb >= ROM_WP_PAL_BANKS) pb = ROM_WP_PAL_BANKS - 1;
  if (pb < 0) pb = 0;
  return pb;
}

/* One tile's pixels, rows/cols flipped as asked. `skip0` = 1 leaves every index-0 pixel
 * of `out` UNTOUCHED (the overlay pass: index 0 is transparent over the base); 0 writes
 * all 64 (the literal expansion rom_wallpaper_expand_tile documents). */
/* BACKLOG #103 (speed): reverse the eight nibbles of a tile row (hflip): swap the nibbles inside
 * each byte, then reverse the byte order. Pixel k of the result is source pixel 7-k. */
static inline uint32_t rev_nibbles(uint32_t w) {
  w = ((w & 0x0F0F0F0Fu) << 4) | ((w >> 4) & 0x0F0F0F0Fu);
  return (w << 24) | ((w & 0xFF00u) << 8) | ((w >> 8) & 0xFF00u) | (w >> 24);
}

static void expand_px(const uint8_t* src, int hflip, int vflip, const uint16_t pal[16],
                      int skip0, uint16_t out[64]) {
  /* BACKLOG #103: the artless box grid spent ~240 of its ~490 ms here (360 cells x 192 pixel
   * operations through a per-pixel branch and two index conversions). With 4-byte-aligned source
   * and destination (every real caller: tiles at +32*tid in the 4-aligned mon_decomp, `out` =
   * the aligned s_wp_tile) a tile row is ONE 32-bit read and, when no pixel is skipped, four
   * 32-bit stores; an overlay row that is wholly transparent is skipped outright and one with
   * no index-0 nibble is written whole. LITTLE-ENDIAN (the GBA, and the host test machines):
   * pixel k of a row is nibble k of the word. Anything unaligned takes the byte-wise path
   * below, which is the original code. */
  if ((((uintptr_t)src | (uintptr_t)out) & 3u) == 0u) {
    const uint32_t* sw = (const uint32_t*)(const void*)src;
    uint32_t* ow = (uint32_t*)(void*)out;
    for (int y = 0; y < 8; y++) {
      uint32_t w = sw[vflip ? (7 - y) : y];
      if (hflip) w = rev_nibbles(w);
      uint32_t* d = &ow[y * 4];
      if (skip0) {
        if (w == 0u) continue;                              /* wholly transparent row */
        if (((w - 0x11111111u) & ~w & 0x88888888u) != 0u) { /* a transparent nibble: per pixel */
          uint16_t* o = &out[y * 8];
          for (int k = 0; k < 8; k++, w >>= 4)
            if (w & 15u) o[k] = pal[w & 15u];
          continue;
        }
      }
      d[0] = (uint32_t)pal[w & 15u]         | ((uint32_t)pal[(w >> 4) & 15u]  << 16);
      d[1] = (uint32_t)pal[(w >> 8) & 15u]  | ((uint32_t)pal[(w >> 12) & 15u] << 16);
      d[2] = (uint32_t)pal[(w >> 16) & 15u] | ((uint32_t)pal[(w >> 20) & 15u] << 16);
      d[3] = (uint32_t)pal[(w >> 24) & 15u] | ((uint32_t)pal[(w >> 28) & 15u] << 16);
    }
    return;
  }
  for (int y = 0; y < 8; y++) {
    int sy = vflip ? (7 - y) : y;
    for (int xb = 0; xb < 4; xb++) {
      uint8_t b = src[sy * 4 + xb];
      uint8_t lo = (uint8_t)(b & 0x0Fu);          /* rule 11: mask back before use */
      uint8_t hi = (uint8_t)((b >> 4) & 0x0Fu);
      int x0 = xb * 2, x1 = xb * 2 + 1;
      if (hflip) { x0 = 7 - x0; x1 = 7 - x1; }
      if (!(skip0 && lo == 0)) out[y * 8 + x0] = pal[lo];
      if (!(skip0 && hi == 0)) out[y * 8 + x1] = pal[hi];
    }
  }
}

int rom_wallpaper_expand_tile(const uint8_t* tiles, uint32_t tiles_bytes, uint16_t tid,
                              int hflip, int vflip, const uint16_t pal[16],
                              uint16_t out[64]) {
  if (!tiles || !pal || !out) return 0;
  uint32_t off = (uint32_t)tid * 32u;
  if (off + 32u > tiles_bytes) return 0;
  expand_px(tiles + off, hflip, vflip, pal, 0, out);
  return 1;
}

/* ---- BACKLOG #294: the backdrop pattern ---------------------------------------------
 * Retail draws each wallpaper's tilemap OVER a tiled backdrop and treats palette index 0
 * as transparent; the single-layer expansion above painted those holes white (the top
 * band, the 12 blank corner cells, every sparse-tile gap). The compiled wallpapers.c
 * (tools/gen_wallpaper.py) composes: base = the wallpaper's bg tile sheet tiled from the
 * region's origin (cell (tx,ty) -> sheet tile (ty%rows)*cols + tx%cols, bg index 0 ->
 * the interior tone = bank-2 entry 1), then the tilemap overlaid with index 0
 * transparent. The bg tiles are the LAST `used` tiles of the row's tile blob (the build
 * `cat`s frame then bg), so this module only needs each wallpaper's sheet shape.
 * Emerald's 16 shapes are below -- measured from the retail row blobs and verified
 * pixel-exact against gen_wallpaper's composite for all 16 (2026-09-30 probe). FireRed /
 * LeafGreen re-ordered and re-drew the set (their blob sizes differ), so no table is
 * claimed for them: they keep the flat interior tone. */
typedef struct { uint8_t cols, rows, used; } BgShape;
static const BgShape k_em_bg[ROM_WP_COUNT] = {
  {4, 2, 8},  {3, 2, 6},  {7, 2, 14}, {3, 8, 23}, {4, 4, 16}, {2, 13, 26}, {2, 11, 22}, {2, 2, 4},
  {3, 8, 23}, {4, 2, 8},  {3, 4, 11}, {4, 4, 16}, {2, 3, 6},  {11, 2, 22}, {2, 4, 8},   {4, 1, 4},
};

int rom_wallpaper_base(const RomWallpaper* rw, int wp, uint32_t tiles_bytes, RomWpBase* out) {
  if (!out) return 0;
  memset(out, 0, sizeof *out);
  if (!rw || !rw->ok || !rw->rc || wp < 0 || wp >= ROM_WP_COUNT) return 0;
  if (rw->rc->kind != ROM_EMERALD) return 1;            /* no table: flat tone, but valid */
  uint32_t total = tiles_bytes / 32u;
  const BgShape* sh = &k_em_bg[wp];
  if (sh->used > total) return 0;                       /* a blob shorter than the sheet: refuse */
  out->first = (uint16_t)(total - sh->used);
  out->cols = sh->cols; out->rows = sh->rows; out->used = sh->used;
  return 1;
}

int rom_wallpaper_cell_key(const RomWpBase* bs, uint16_t e, int tx, int ty) {
  int pi = 0;
  if (bs && bs->cols && bs->rows) pi = (ty % bs->rows) * bs->cols + (tx % bs->cols) + 1;
  return (int)e | (pi << 16);        /* pi 0 = no base tile; fits 16 + 8 bits, never negative */
}

/* One row of 8 pixels through a 16-entry palette as four 32-bit stores (BACKLOG #103). */
static inline void row_store(uint32_t* d, const uint16_t* pal, uint32_t w) {
  d[0] = (uint32_t)pal[w & 15u]         | ((uint32_t)pal[(w >> 4) & 15u]  << 16);
  d[1] = (uint32_t)pal[(w >> 8) & 15u]  | ((uint32_t)pal[(w >> 12) & 15u] << 16);
  d[2] = (uint32_t)pal[(w >> 16) & 15u] | ((uint32_t)pal[(w >> 20) & 15u] << 16);
  d[3] = (uint32_t)pal[(w >> 24) & 15u] | ((uint32_t)pal[(w >> 28) & 15u] << 16);
}

int rom_wallpaper_expand_cell(const uint8_t* tiles, uint32_t tiles_bytes, uint16_t e,
                              int tx, int ty, const RomWpBase* bs,
                              const uint16_t pal[ROM_WP_PAL_BANKS][16], uint16_t out[64]) {
  if (!tiles || !pal || !out || !bs) return 0;
  uint16_t tid = (uint16_t)(e & 0x3FFu);
  if ((uint32_t)tid * 32u + 32u > tiles_bytes) return 0;
  const uint16_t* bg = pal[ROM_WP_PAL_BANKS - 1];
  const uint16_t* fg = pal[rom_wallpaper_pal_bank((e >> 12) & 0xF)];
  uint16_t tone = bg[1];
  /* 1) the base: the tiled bg tile, or (no table / past the sheet) the flat tone. A
   * table-less wallpaper whose tone is 0 keeps the literal index-0 colour -- never paint
   * black where the answer is unknown (City's tone IS black, but City is table-backed). */
  uint16_t fill = tone;
  if (!bs->cols && tone == 0) fill = fg[0];
  const uint8_t* btile = 0;                          /* the backdrop tile, if there is one */
  uint16_t bp[16];
  if (bs->cols && bs->rows) {
    int pi = (ty % bs->rows) * bs->cols + (tx % bs->cols);
    if (pi < bs->used) {
      uint32_t boff = ((uint32_t)bs->first + (uint32_t)pi) * 32u;
      if (boff + 32u <= tiles_bytes) {
        memcpy(bp, bg, sizeof bp);
        bp[0] = tone;                               /* bg index 0 -> interior tone */
        btile = tiles + boff;
      }
    }
  }
  const uint8_t* otile = tiles + (uint32_t)tid * 32u;
  const int hf = (e >> 10) & 1, vf = (e >> 11) & 1;
  /* BACKLOG #103: the fused word path. Per row, in the order base-then-overlay-with-index-0-
   * transparent, the FINAL pixels are: the overlay row alone when it has no index-0 nibble
   * (the base would be fully overwritten, so it is never expanded), the base row alone when the
   * overlay row is wholly transparent, otherwise the base row with the overlay's non-zero
   * pixels over it. Needs 4-byte-aligned tiles and output (every real caller); anything else
   * takes the original two-pass code below. */
  if ((((uintptr_t)out | (uintptr_t)otile | (uintptr_t)btile) & 3u) == 0u) {
    const uint32_t* ow = (const uint32_t*)(const void*)otile;
    const uint32_t* bw = (const uint32_t*)(const void*)btile;
    uint32_t* d = (uint32_t*)(void*)out;
    const uint32_t f2 = (uint32_t)fill | ((uint32_t)fill << 16);
    for (int y = 0; y < 8; y++, d += 4) {
      uint32_t w = ow[vf ? (7 - y) : y];
      if (hf) w = rev_nibbles(w);
      if (w != 0u && ((w - 0x11111111u) & ~w & 0x88888888u) == 0u) { row_store(d, fg, w); continue; }
      if (btile) row_store(d, bp, bw[y]);
      else { d[0] = f2; d[1] = f2; d[2] = f2; d[3] = f2; }
      if (w != 0u) {
        uint16_t* o = &out[y * 8];
        for (int k = 0; k < 8; k++, w >>= 4)
          if (w & 15u) o[k] = fg[w & 15u];
      }
    }
    return 1;
  }
  for (int i = 0; i < 64; i++) out[i] = fill;
  if (btile) expand_px(btile, 0, 0, bp, 0, out);
  /* 2) the tilemap's own tile over it, index 0 transparent */
  expand_px(otile, hf, vf, fg, 1, out);
  return 1;
}
