#ifndef LZBLOB_INCLUDED
#define LZBLOB_INCLUDED
#include <stdint.h>

/* Paged BIOS-LZ77 art blobs — the reader half of tools/lzband.py.
 *
 * WHY: the trainer-card / bag / Pokeblock backgrounds are pre-composited
 * 240x160 RGB15 frames. Stored raw they were 5.7 MB of a 12.5 MB ROM, and a
 * 12.5 MB file on the EZ-Flash Omega DE fragments often enough to overrun the
 * kernel's unbounded FAT run table and hang the SD loader (see
 * projects/rom-load-lab; the same image always boots from NOR). Flat game art
 * compresses ~7x with the BIOS's own LZ77, so the blobs now ship packed.
 *
 * WHY PAGED, not one stream: the UI restores small rects out of the middle of
 * a frame on every cursor move, so the data must stay randomly addressable.
 * Each LZBLOB_PAGE of the ORIGINAL blob is compressed on its own and the page
 * table gives its offset; a read touches only the pages it overlaps.
 *
 * A page is exactly 16 full screen rows, which buys two things: a full-screen
 * blit is 10 BIOS calls straight into VRAM with no buffer at all, and a
 * partial rect fits the 8192-byte shared scratch (artscratch.h).
 */

/* MUST equal tools/lzband.py's PAGE. The packer and this reader agree on it by
 * convention only, so lzblob.c asserts the pair at link time via the page-count
 * a generator emits. 3840 B = 240 px * 8 rows: the largest page that fits the
 * VRAM hole the staged path unpacks into (see lzblob.c). */
#define LZBLOB_PAGE 3840u          /* 240 px * 8 rows * 2 B */

typedef struct {
  const uint8_t*  base;            /* packed stream                        */
  const uint32_t* pages;           /* npages+1 offsets into base           */
  uint32_t        npages;
  uint32_t        raw_len;         /* length of the ORIGINAL blob          */
} LzBlob;

/* Blit a w x h RGB15 rect to the Mode-3 framebuffer at (x, y).
 * `off` is the byte offset of the rect's top-left pixel in the ORIGINAL
 * (unpacked) blob and `sw` is the source stride in PIXELS, so a full frame is
 * (off = frame base, sw = 240, x = 0, y = 0, w = 240, h = 160) and the bag's
 * 64-wide animation rect is (off = rect base, sw = 64, ...). Clips nothing —
 * callers pass on-screen rects, exactly as the raw-pointer blitters did. */
void lzblob_blit(const LzBlob* b, uint32_t off, int sw,
                 int x, int y, int w, int h);

/* Copy `n` bytes of the ORIGINAL blob at byte offset `off` into `dst`.
 * For the handful of small sprites whose accessors still hand out a pointer
 * (Pokeblock highlight / flavour icons). `dst` must NOT be VRAM. */
void lzblob_read(const LzBlob* b, uint32_t off, uint32_t n, void* dst);

/* ------------------------------------------------------------------------ *
 * BgFrame — what the art accessors hand out now.
 *
 * They used to return `const uint16_t*` straight into the raw blob and the UI
 * indexed it. Packed art cannot be indexed, so the accessors return this
 * (blob, offset, stride) triple instead and the two calls below do the work.
 * `blob == 0` is the art-free build's answer, exactly as NULL used to be. */
typedef struct {
  const LzBlob* blob;
  uint32_t      off;               /* byte offset of the frame in the blob   */
  uint16_t      sw;                /* source stride in pixels                */
} BgFrame;

/* Restore the SCREEN rect (x, y, w, h) out of a full-screen 240x160 frame —
 * i.e. the pixels that belong at (x, y). (0, 0, 240, 160) is a full blit and
 * takes the no-copy fast path. */
static inline void bg_restore(BgFrame f, int x, int y, int w, int h) {
  lzblob_blit(f.blob, f.off + ((uint32_t)y * 240u + (uint32_t)x) * 2u, 240,
              x, y, w, h);
}

/* Blit a STANDALONE w x h rect (the bag's animation frames) to (x, y). */
static inline void bg_blit_rect(BgFrame f, int x, int y, int w, int h) {
  lzblob_blit(f.blob, f.off, f.sw, x, y, w, h);
}

#endif
