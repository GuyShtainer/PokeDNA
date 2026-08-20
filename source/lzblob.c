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
 * tonc BG_SBB attribute ONCE, then walk only the sub-rows/sub-cols the rect
 * actually needs. A full 240x160 redraw is at most 600 attribute decodes, not
 * 38,400 — the same order of work pdna_box.c's wp_blit_tile already spends on a
 * box wallpaper. Only the first 30 columns / 20 rows are ever drawn (a 240x160
 * screen); a wider stored map_w (Ruby pads to 32) just changes the row stride. */
static void romchrome_blit(const RomChromeSrc* s, uint32_t off, int sw,
                           int x, int y, int w, int h) {
  if (!s || !s->tiles || !s->map || w <= 0 || h <= 0) return;
  uint32_t srcpx = off / 2u;
  int sx0 = (int)(srcpx % (uint32_t)sw), sy0 = (int)(srcpx / (uint32_t)sw);
  int x0 = sx0, y0 = sy0, x1 = sx0 + w, y1 = sy0 + h;
  int tx0 = x0 >> 3, ty0 = y0 >> 3, tx1 = (x1 - 1) >> 3, ty1 = (y1 - 1) >> 3;
  for (int ty = ty0; ty <= ty1; ty++) {
    if (ty < 0 || ty >= 20) continue;
    for (int tx = tx0; tx <= tx1; tx++) {
      if (tx < 0 || tx >= 30) continue;
      uint16_t e = s->map[(uint32_t)ty * s->map_w + (uint32_t)tx];
      int tile = e & 0x03FF, hf = (e >> 10) & 1, vf = (e >> 11) & 1, bank = (e >> 12) & 0xF;
      const uint16_t* pal = &s->pal[bank * 16];
      int px0 = tx * 8, py0 = ty * 8;
      int lx0 = (px0 < x0) ? (x0 - px0) : 0, lx1 = (px0 + 8 > x1) ? (x1 - px0) : 8;
      int ly0 = (py0 < y0) ? (y0 - py0) : 0, ly1 = (py0 + 8 > y1) ? (y1 - py0) : 8;
      for (int ly = ly0; ly < ly1; ly++) {
        int sy = vf ? 7 - ly : ly;
        const uint8_t* trow = s->tiles + (uint32_t)tile * 32u + (uint32_t)sy * 4u;
        int dy = y + (py0 + ly - y0);
        uint16_t* drow = &vid_mem[dy * 240 + x];
        for (int lx = lx0; lx < lx1; lx++) {
          int sxp = hf ? 7 - lx : lx;
          uint8_t b = trow[sxp >> 1];
          uint8_t idx = (uint8_t)((sxp & 1) ? (b >> 4) : (b & 0x0F));
          drow[px0 + lx - x0] = pal[idx] & 0x7FFFu;
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
