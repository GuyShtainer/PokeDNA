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

/*
 * A live ROM-decoded chrome screen: one 8x8-tile plane (tileset + tilemap +
 * palette), all already resident in RAM for a screen's whole lifetime (never
 * re-read per blit). This is the ROM rung's payload — see rom_chrome.h for how it
 * gets filled and DESIGN.md Sec 4.6 for why a full 240x160 frame is never buffered
 * (76,800 B does not fit an EWRAM budget measured in hundreds of bytes).
 *
 * `tiles` is 4bpp planar, tile i at tiles + 32*i. `map` is `map_w` x 20 raw tonc
 * BG_SBB entries (index bits 0-9, hflip bit10, vflip bit11, palbank bits 12-15);
 * only the first 30 columns of each row are ever drawn (a 240 px screen), so
 * map_w may be wider than 30 to match the ROM's own stored stride (Ruby pads to
 * 32). `pal` holds up to 6 banks (96 colours) flat, bank b / index i at
 * pal[b*16+i]; an unused bank is simply never indexed by `map`.
 *
 * `bg_map` is an OPTIONAL second tilemap, same `tiles`/`map_w`/`pal` and same
 * tile grid, composited UNDERNEATH `map`: wherever a `map` pixel's 4bpp index
 * is 0, the corresponding `bg_map` pixel shows through instead of opaque
 * palette-bank-0 colour 0. This is how the trainer card's card_bg.h "sticker on
 * a background" retail look actually works -- the card sticker's tilemap
 * (`map`) has real pixels only where the card art is, index 0 everywhere else,
 * and the background tilemap paints the border underneath. NULL means `map`'s
 * own index 0 is opaque, exactly as before (every non-card screen, and the
 * Pokeblock case, which has no background layer of its own). */
typedef struct {
  const uint8_t*  tiles;
  const uint16_t* map;
  const uint16_t* bg_map;
  uint16_t        map_w;
  uint16_t        pal[96];
} RomChromeSrc;

typedef struct {
  const uint8_t*  base;            /* packed stream                        */
  const uint32_t* pages;           /* npages+1 offsets into base           */
  uint32_t        npages;
  uint32_t        raw_len;         /* length of the ORIGINAL blob          */
  /* NEW, trailing field. Every EXISTING generator (tools/lzband.py's C emitters)
   * initialises an LzBlob with a 4-value positional initializer, e.g.
   * `{ card_bg_lz, card_bg_pages, 1200u, 4608000u }` — ISO C zero-fills any
   * trailing member a positional initializer does not mention, so every already-
   * generated blob gets romsrc == NULL for free, no generator changes needed.
   * NULL means "a real packed LZ77 stream, use the existing page-cache path";
   * non-NULL means "this BgFrame is actually a RomChromeSrc*, composite tiles
   * instead" — see lzblob.c's dispatch at the top of lzblob_blit/lzblob_read. */
  const RomChromeSrc* romsrc;
} LzBlob;

/* Composite an ntx x nty grid of 8x8 4bpp tiles straight to the Mode-3
 * framebuffer at (dx, dy), colour index 0 skipped (transparent), through ONE
 * flat 16-entry RGB15 palette (no bank field -- unlike RomChromeSrc's `map`,
 * these sprites are plain picture data, never read through a tilemap+bank
 * attribute). `tile_ids` NULL means "sequential, row-major, 0..ntx*nty-1" --
 * a plain WxH picture whose own 4bpp tileset IS the picture in tile-major
 * order, which is exactly the shape rom_chrome.c decodes the trainer photo,
 * the bag sprite, and the Pokeblock device into. A non-NULL `tile_ids` array
 * (ntx*nty entries) is for sprites indirected through a tile-id table (the
 * trainer card's badge icons: 2x2 tiles per badge, picked out of a shared
 * sheet by id, Emerald by formula and Ruby/Sapphire through badges_map.bin --
 * see rom_chrome.h). A negative id skips that cell (no tile there). */
void romchrome_blit_tiles(const uint8_t* tiles, const uint16_t* pal16,
                          const int16_t* tile_ids, int ntx, int nty,
                          int dx, int dy);

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
