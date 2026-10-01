#include <tonc.h>
#include "lzblob.h"
#include "rumble.h"   /* mute the cart-bus motor toggle while a blit reads ROM */
#include "rom_chrome_gate.h"

#if PDNA_ROM_CHROME_NEEDED
/*
 * The ROM rung's compositor: b->romsrc != NULL means every byte lzblob_blit/
 * lzblob_read would normally page in from a packed LZ77 stream is ALREADY sitting
 * in RAM as a decoded tile + tilemap + palette (rom_chrome.h). No SD/ROM traffic
 * happens here at all — this is a pure RAM->VRAM composite, so unlike the LZ77
 * path above it needs no rumble_io_suspend bracketing.
 *
 * Tile-major, not pixel-major: for each 8x8 cell the rect overlaps, decode its
 * tonc BG_SBB attribute ONCE (and `bg_map`'s, when present), then walk only the
 * sub-rows/sub-cols the rect actually needs. A full 240x160 redraw is at most
 * 600 (or 1,200 with a background layer) attribute decodes, not 38,400 — the
 * same order of work pdna_box.c's wp_blit_tile already spends on a box
 * wallpaper. Only the first 30 columns / 20 rows are ever drawn (a 240x160
 * screen); a wider stored map_w (Ruby pads to 32) just changes the row stride. */
/* BACKLOG #103 (artless speed): one tile row of 8 pixels through a 16-entry palette, written as
 * four 32-bit stores (two RGB15 pixels each). `w` is the tile row's 32-bit word (pixel k =
 * nibble k, LSB first); the 0x7FFF mask is applied per pixel exactly as the per-pixel path
 * below does, so the framebuffer bytes are identical. `d` must be 4-byte aligned. */
static inline void rc_row8(uint32_t* d, const uint16_t* pal, uint32_t w) {
  d[0] = ((uint32_t)pal[w & 15u]         | ((uint32_t)pal[(w >> 4) & 15u]  << 16)) & 0x7FFF7FFFu;
  d[1] = ((uint32_t)pal[(w >> 8) & 15u]  | ((uint32_t)pal[(w >> 12) & 15u] << 16)) & 0x7FFF7FFFu;
  d[2] = ((uint32_t)pal[(w >> 16) & 15u] | ((uint32_t)pal[(w >> 20) & 15u] << 16)) & 0x7FFF7FFFu;
  d[3] = ((uint32_t)pal[(w >> 24) & 15u] | ((uint32_t)pal[(w >> 28) & 15u] << 16)) & 0x7FFF7FFFu;
}

/* The same with a background layer showing through every index-0 pixel of the front row. */
static inline uint32_t rc_px_bg(const uint16_t* pal, const uint16_t* bpal, uint32_t w,
                                uint32_t bw, int k) {
  uint32_t i = (w >> (4 * k)) & 15u;
  return i ? pal[i] : bpal[(bw >> (4 * k)) & 15u];
}
static inline void rc_row8_bg(uint32_t* d, const uint16_t* pal, const uint16_t* bpal,
                              uint32_t w, uint32_t bw) {
  d[0] = (rc_px_bg(pal, bpal, w, bw, 0) | (rc_px_bg(pal, bpal, w, bw, 1) << 16)) & 0x7FFF7FFFu;
  d[1] = (rc_px_bg(pal, bpal, w, bw, 2) | (rc_px_bg(pal, bpal, w, bw, 3) << 16)) & 0x7FFF7FFFu;
  d[2] = (rc_px_bg(pal, bpal, w, bw, 4) | (rc_px_bg(pal, bpal, w, bw, 5) << 16)) & 0x7FFF7FFFu;
  d[3] = (rc_px_bg(pal, bpal, w, bw, 6) | (rc_px_bg(pal, bpal, w, bw, 7) << 16)) & 0x7FFF7FFFu;
}

/* hflip of one tile row: reverse the eight nibbles (swap the nibbles inside each byte, then
 * reverse the byte order) so pixel k of the result is source pixel 7-k. */
static inline uint32_t rc_hflip(uint32_t w) {
  w = ((w & 0x0F0F0F0Fu) << 4) | ((w >> 4) & 0x0F0F0F0Fu);
  return (w << 24) | ((w & 0xFF00u) << 8) | ((w >> 8) & 0xFF00u) | (w >> 24);
}

/* One FULL 8x8 cell (no clipping), at an even destination x, as eight row8 stores. `tw` points
 * at the front tile's 32 bytes, `btw` at the background tile's (NULL = no background layer). */
static void rc_cell_fast(uint16_t* dst0, const uint32_t* tw, int hf, int vf, const uint16_t* pal,
                         const uint32_t* btw, int bhf, int bvf, const uint16_t* bpal) {
  for (int ly = 0; ly < 8; ly++) {
    uint32_t w = tw[vf ? 7 - ly : ly];
    uint32_t* d = (uint32_t*)(void*)(dst0 + ly * 240);
    if (hf) w = rc_hflip(w);
    if (!btw) { rc_row8(d, pal, w); continue; }
    uint32_t bw = btw[bvf ? 7 - ly : ly];
    if (bhf) bw = rc_hflip(bw);
    if (w == 0) rc_row8(d, bpal, bw);        /* whole row transparent: pure background */
    else        rc_row8_bg(d, pal, bpal, w, bw);
  }
}

static void romchrome_blit(const RomChromeSrc* s, uint32_t off, int sw,
                           int x, int y, int w, int h) {
  if (!s || !s->tiles || !s->map || w <= 0 || h <= 0) return;
  uint32_t srcpx = off / 2u;
  int sx0 = (int)(srcpx % (uint32_t)sw), sy0 = (int)(srcpx / (uint32_t)sw);
  int x0 = sx0, y0 = sy0, x1 = sx0 + w, y1 = sy0 + h;
  int tx0 = x0 >> 3, ty0 = y0 >> 3, tx1 = (x1 - 1) >> 3, ty1 = (y1 - 1) >> 3;
  /* The word-wide cell path needs 4-byte-aligned tile data (every caller stages tiles at
   * offset 0 of the 4-aligned mon_decomp, tile i at +32*i) and an even destination x. */
  const int word_ok = (((uintptr_t)s->tiles & 3u) == 0u);
  for (int ty = ty0; ty <= ty1; ty++) {
    if (ty < 0 || ty >= 20) continue;
    for (int tx = tx0; tx <= tx1; tx++) {
      if (tx < 0 || tx >= 30) continue;
      uint16_t e = s->map[(uint32_t)ty * s->map_w + (uint32_t)tx];
      int tile = e & 0x03FF, hf = (e >> 10) & 1, vf = (e >> 11) & 1, bank = (e >> 12) & 0xF;
      const uint16_t* pal = &s->pal[bank * 16];
      /* The optional background cell underneath this SAME 8x8 grid position,
       * decoded once per tile like the front cell above -- see the struct
       * comment in lzblob.h for what this composites and why. */
      int btile = 0, bhf = 0, bvf = 0;
      const uint16_t* bpal = 0;
      if (s->bg_map) {
        uint16_t be = s->bg_map[(uint32_t)ty * s->map_w + (uint32_t)tx];
        int bbank;
        btile = be & 0x03FF; bhf = (be >> 10) & 1; bvf = (be >> 11) & 1; bbank = (be >> 12) & 0xF;
        bpal = &s->pal[bbank * 16];
      }
      int px0 = tx * 8, py0 = ty * 8;
      int lx0 = (px0 < x0) ? (x0 - px0) : 0, lx1 = (px0 + 8 > x1) ? (x1 - px0) : 8;
      int ly0 = (py0 < y0) ? (y0 - py0) : 0, ly1 = (py0 + 8 > y1) ? (y1 - py0) : 8;
      if (word_ok && lx0 == 0 && lx1 == 8 && ly0 == 0 && ly1 == 8 &&
          ((x + px0 - x0) & 1) == 0) {                /* a whole cell at an even x: word path */
        rc_cell_fast(&vid_mem[(y + (py0 - y0)) * 240 + x + (px0 - x0)],
                     (const uint32_t*)(const void*)(s->tiles + (uint32_t)tile * 32u), hf, vf, pal,
                     s->bg_map ? (const uint32_t*)(const void*)(s->tiles + (uint32_t)btile * 32u) : 0,
                     bhf, bvf, bpal);
        continue;
      }
      for (int ly = ly0; ly < ly1; ly++) {
        int sy = vf ? 7 - ly : ly;
        const uint8_t* trow = s->tiles + (uint32_t)tile * 32u + (uint32_t)sy * 4u;
        const uint8_t* btrow = 0;
        if (s->bg_map) {
          int bsy = bvf ? 7 - ly : ly;
          btrow = s->tiles + (uint32_t)btile * 32u + (uint32_t)bsy * 4u;
        }
        int dy = y + (py0 + ly - y0);
        uint16_t* drow = &vid_mem[dy * 240 + x];
        for (int lx = lx0; lx < lx1; lx++) {
          int sxp = hf ? 7 - lx : lx;
          uint8_t b = trow[sxp >> 1];
          uint8_t idx = (uint8_t)((sxp & 1) ? (b >> 4) : (b & 0x0F));
          uint16_t color;
          if (idx == 0 && btrow) {
            int bsxp = bhf ? 7 - lx : lx;
            uint8_t bb = btrow[bsxp >> 1];
            uint8_t bidx = (uint8_t)((bsxp & 1) ? (bb >> 4) : (bb & 0x0F));
            color = bpal[bidx] & 0x7FFFu;
          } else {
            color = pal[idx] & 0x7FFFu;
          }
          drow[px0 + lx - x0] = color;
        }
      }
    }
  }
}

/* Same idea for the small handful of standalone-sprite reads a ROM chrome screen
 * might still want (none, today — card/pokeblock only ever full-blit their
 * tilemap). Kept for symmetry with lzblob_read's signature; a caller asking for a
 * ROM-chrome LzBlob's raw bytes gets zeros rather than reading VRAM staging that
 * was never populated in this format. */
static void romchrome_read(const RomChromeSrc* s, uint32_t off, uint32_t n, void* dst) {
  (void)s; (void)off;
  if (dst && n) __builtin_memset(dst, 0, n);
}

/* The small-sprite overlay compositor (lzblob.h): stars/badges/photo on the
 * trainer card, the bag sprite, the Pokeblock device. Plain 4bpp tile-major
 * picture data, ONE flat palette, index 0 transparent -- no tilemap/bank
 * indirection at all, so this is deliberately simpler than romchrome_blit
 * above (which exists to walk a real BG_SBB tilemap). Clips to the 240x160
 * framebuffer; every caller in this codebase passes an on-screen rect anyway
 * (star_xy/photo_xy/bag_xy/DEVICE_XY are all comfortably inside it), the
 * bounds check is here only so a bad tile id or a future off-screen caller
 * cannot scribble past vid_mem. */
void romchrome_blit_tiles(const uint8_t* tiles, const uint16_t* pal16,
                          const int16_t* tile_ids, int ntx, int nty,
                          int dx, int dy) {
  if (!tiles || !pal16 || ntx <= 0 || nty <= 0) return;
  for (int ty = 0; ty < nty; ty++) {
    for (int tx = 0; tx < ntx; tx++) {
      int tid = tile_ids ? tile_ids[ty * ntx + tx] : (ty * ntx + tx);
      if (tid < 0) continue;                    /* negative = "no tile here" */
      const uint8_t* t = tiles + (uint32_t)tid * 32u;
      int px0 = dx + tx * 8, py0 = dy + ty * 8;
      for (int y = 0; y < 8; y++) {
        int sy = py0 + y;
        if (sy < 0 || sy >= 160) continue;
        uint16_t* drow = &vid_mem[sy * 240];
        const uint8_t* trow = t + y * 4;
        for (int x = 0; x < 8; x++) {
          int sx = px0 + x;
          if (sx < 0 || sx >= 240) continue;
          uint8_t b = trow[x >> 1];
          uint8_t idx = (uint8_t)((x & 1) ? (b >> 4) : (b & 0x0F));
          if (idx) drow[sx] = pal16[idx] & 0x7FFFu;
        }
      }
    }
  }
}
#endif /* PDNA_ROM_CHROME_NEEDED */

/* WHERE THE STAGING PAGE LIVES, and why it is not in EWRAM.
 *
 * A full-screen blit needs no staging at all: a page is 8 whole screen rows,
 * so the BIOS decompresses it straight into its final place in the
 * framebuffer. Only a PARTIAL rect restore (the bag list, the card's
 * selection frame) has to unpack a page and copy a slice out of it.
 *
 * EWRAM could not host that page: this build has 1508 bytes free, and the one
 * 8 KiB buffer that exists — mon_decomp — is already claimed as staging by
 * five other decoders (rom_sprite, gb_sprite_codec, pdna_box, pdna_summary,
 * pdna_origin_art). Borrowing it would make "which screen may draw art while
 * another decode is live" a correctness question in five more places.
 *
 * VRAM has an unclaimed hole instead. Mode 3's framebuffer is
 * 0x06000000..0x06012C00 (240*160*2), and in a bitmap mode OBJ tiles start at
 * tile 512 = 0x06014000 (box_oam.c puts every sprite there). The 5120 bytes
 * between belong to nobody, and one page is 3840 of them. Cost: zero EWRAM,
 * zero coupling.
 *
 * The one rule this buys: lzblob_blit/lzblob_read are MODE 3 ONLY. Every
 * caller (trainer card, bag, Pokeblock case) is a Mode-3 screen; the map
 * viewer, which switches to Mode 0 and owns all of VRAM, never touches a
 * packed blob. */
#define LZBLOB_STAGE ((uint16_t*)(MEM_VRAM + 240 * 160 * 2))

/* Unpack page `pg` into the staging hole, unless it is already there.
 * The cache tag is per-CALL (the caller owns it) rather than a static: a
 * static would have to be invalidated by every mode switch and every other
 * writer of that VRAM hole, and within one blit is where all the reuse is
 * anyway — a 128-row rect restore drops from 128 unpacks to 17. */
static void page_load(const LzBlob* b, uint32_t pg, uint32_t* cached) {
  if (*cached == pg) return;
  LZ77UnCompVram(b->base + b->pages[pg], LZBLOB_STAGE);
  *cached = pg;
}

void lzblob_blit(const LzBlob* b, uint32_t off, int sw,
                 int x, int y, int w, int h) {
  if (!b || w <= 0 || h <= 0) return;
#if PDNA_ROM_CHROME_NEEDED
  if (b->romsrc) { romchrome_blit(b->romsrc, off, sw, x, y, w, h); return; }
#endif
  rumble_io_suspend();                       /* the packed stream lives in ROM */

  /* Fast path — a whole-width, page-aligned run of whole pages goes straight
   * from ROM into the framebuffer with no intermediate copy at all. This is
   * every full-screen background blit; the generators page-align each frame
   * so it always hits. */
  if (x == 0 && w == 240 && sw == 240 && (off % LZBLOB_PAGE) == 0 && (h & 7) == 0) {
    uint32_t pg = off / LZBLOB_PAGE;
    for (int row = 0; row < h && pg < b->npages; row += 8, pg++)
      LZ77UnCompVram(b->base + b->pages[pg], &vid_mem[(y + row) * 240]);
    rumble_io_resume();
    return;
  }

  /* General path — stage the pages the rect overlaps and copy the slices.
   * Everything here is halfword-aligned by construction (RGB15 pixels, an
   * even page size, even source offsets), which VRAM writes require. */
  uint32_t cached = 0xFFFFFFFFu;
  for (int j = 0; j < h; j++) {
    uint32_t src  = off + (uint32_t)j * (uint32_t)sw * 2u;
    uint16_t* dst = &vid_mem[(y + j) * 240 + x];
    uint32_t left = (uint32_t)w * 2u;
    while (left) {
      uint32_t pg = src / LZBLOB_PAGE, in = src % LZBLOB_PAGE;
      uint32_t take = LZBLOB_PAGE - in;
      if (take > left) take = left;
      if (pg >= b->npages) { rumble_io_resume(); return; }   /* past the blob */
      page_load(b, pg, &cached);
      memcpy16(dst, (const uint16_t*)((const uint8_t*)LZBLOB_STAGE + in),
               (uint)(take >> 1));
      dst  += take >> 1;
      src  += take;
      left -= take;
    }
  }
  rumble_io_resume();
}

void lzblob_read(const LzBlob* b, uint32_t off, uint32_t n, void* dst) {
  if (!b || !n) return;
#if PDNA_ROM_CHROME_NEEDED
  if (b->romsrc) { romchrome_read(b->romsrc, off, n, dst); return; }
#endif
  uint32_t cached = 0xFFFFFFFFu;
  uint8_t* out = (uint8_t*)dst;
  rumble_io_suspend();
  while (n) {
    uint32_t pg = off / LZBLOB_PAGE, in = off % LZBLOB_PAGE;
    uint32_t take = LZBLOB_PAGE - in;
    if (take > n) take = n;
    if (pg >= b->npages) break;
    page_load(b, pg, &cached);
    /* 8-bit READS from VRAM are legal (only writes are not), and dst is never
     * VRAM — see lzblob.h. */
    __builtin_memcpy(out, (const uint8_t*)LZBLOB_STAGE + in, take);
    out += take;
    off += take;
    n   -= take;
  }
  rumble_io_resume();
}
