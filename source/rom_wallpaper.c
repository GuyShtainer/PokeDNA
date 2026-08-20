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

int rom_wallpaper_expand_tile(const uint8_t* tiles, uint32_t tiles_bytes, uint16_t tid,
                              int hflip, int vflip, const uint16_t pal[16],
                              uint16_t out[64]) {
  if (!tiles || !pal || !out) return 0;
  uint32_t off = (uint32_t)tid * 32u;
  if (off + 32u > tiles_bytes) return 0;
  const uint8_t* src = tiles + off;
  for (int y = 0; y < 8; y++) {
    int sy = vflip ? (7 - y) : y;
    for (int xb = 0; xb < 4; xb++) {
      uint8_t b = src[sy * 4 + xb];
      uint8_t lo = (uint8_t)(b & 0x0Fu);          /* rule 11: mask back before use */
      uint8_t hi = (uint8_t)((b >> 4) & 0x0Fu);
      int x0 = xb * 2, x1 = xb * 2 + 1;
      if (hflip) { x0 = 7 - x0; x1 = 7 - x1; }
      out[y * 8 + x0] = pal[lo];
      out[y * 8 + x1] = pal[hi];
    }
  }
  return 1;
}
