#include <tonc.h>
#include "lzblob.h"
#include "rumble.h"   /* mute the cart-bus motor toggle while a blit reads ROM */

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
