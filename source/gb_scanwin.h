#ifndef GB_SCANWIN_INCLUDED
#define GB_SCANWIN_INCLUDED

#include <stdint.h>
#include <string.h>

/*
 * gb_scanwin — the ONE sliding-window rule the three whole-ROM locators share
 * (rom_gbsprite.c scan_multi, rom_gbicon.c scan_one, rom_gbui.c scan_multi).
 * Pure C, header-only, no state of its own: each scanner keeps a GbScanWin on its
 * frame and does its own reads and its own pattern callbacks; this only decides
 * WHICH bytes to read next and which window positions are new.
 *
 * WHY IT EXISTS (2026-09-14, the "stuck choosing the .gbc" hardware bug). The old
 * scanners read a `cap`-byte window at `base`, then re-read from `base + cap -
 * look + 1` -- i.e. every window after the first started BEFORE the previous
 * read's end, at an arbitrary byte offset. Through a FatFs FIL that is the worst
 * possible access pattern, measured on the real lib/fatfs over a RAM disk
 * (tests/host_gbscan_test.c part D): a 2 MB Gold.gbc cost the sprite locator
 * 2,994 f_read calls but 8,592 disk_read TRANSACTIONS and 12,954 sectors (12.6 MB
 * of sector traffic for a 2 MB file) -- every unaligned start refills the FIL's
 * sector buffer, every backward hop across a cluster boundary restarts FatFs'
 * cluster walk from the head of the chain (ff.c f_lseek, "seek to back cluster"),
 * and every partial tail sector is a separate refill. On the EZ-Flash each
 * transaction is a fixed-cost OS-mode round trip, so that scan read as a hang.
 *
 * THE RULE. The window is [carry | fresh]: `ovl` bytes carried over from the
 * previous chunk (at least the longest pattern minus one) followed by a `chunk`
 * of new bytes. `chunk` is the largest power of two that fits in the scratch
 * beside the carry, and the carry is rounded up to a multiple of it (see
 * gb_scanwin_init for why: FAT clusters are powers of two and f_read clips at
 * cluster boundaries). Every read therefore starts at a file offset that is a
 * multiple of `chunk` (>= 512), lands in the scratch at a multiple of `chunk`,
 * and is exactly `chunk` long except the first (carry+chunk) and the last -- so
 * a FatFs-backed GbReadFn hits f_read's direct multi-sector path (no FIL-buffer
 * staging, ONE disk_read per chunk on any volume whose cluster is >= chunk, and
 * lib/fatfs/diskio.c's word-aligned DMA fast path as long as the scratch itself
 * is word-aligned), and the file is only ever read FORWARD, so no seek ever
 * restarts the cluster walk. Same Gold.gbc through an 8 KB scratch (chunk 4 KB):
 * 511 reads, 511 disk_read calls, on a 4 KB- and a 32 KB-cluster volume alike.
 * The fused-ROM and host readers do not care about any of this; alignment is
 * simply harmless for them.
 *
 * Every position 0 .. size-look is offered exactly once, in order: the first
 * window offers [0, have-look], and each later window offers only the positions
 * the new chunk added (ovl-look+1 .. have-look, which is exactly rd_len of them).
 * tests/host_gbscan_test.c part A proves that on synthetic buffers at every
 * awkward size/cap/look combination, including chunks that straddle the end.
 */

#define GB_SCANWIN_ALIGN 512u

typedef struct GbScanWin {
  uint32_t size;    /* bytes in the file                                          */
  uint32_t look;    /* longest pattern any job needs at one position              */
  uint32_t ovl;     /* bytes carried between chunks: >= look-1, a multiple of chunk */
  uint32_t chunk;   /* fresh bytes per read: the largest power of two that fits    */
  uint32_t base;    /* file offset of w[0]                                        */
  uint32_t have;    /* valid bytes in w                                           */
  uint32_t first;   /* first NEW position (relative to w) after the pending read   */
  uint32_t rd_off;  /* the pending read: file offset ...                          */
  uint32_t rd_len;  /*   ... length ...                                           */
  uint32_t rd_dst;  /*   ... and where in w it lands                              */
  uint32_t reads;   /* reads planned so far (0 = the next plan is the first read)  */
} GbScanWin;

/* Sizes the window. Returns 0 (refuse) when the scratch cannot hold the carry plus
 * one aligned chunk, or the file is shorter than the longest pattern. */
static inline int gb_scanwin_init(GbScanWin* sw, uint32_t size, uint32_t cap, uint32_t look) {
  if (!sw || look == 0 || size < look) return 0;
  memset(sw, 0, sizeof *sw);
  uint32_t a = GB_SCANWIN_ALIGN;
  uint32_t ovl_min = ((look - 1u) + a - 1u) / a * a;
  if (cap < ovl_min + a) return 0;
  /* The chunk is the largest POWER OF TWO that fits beside the carry, and the carry
   * is rounded up to a multiple of it, so every read (the first included) starts at
   * a multiple of `chunk`. FAT cluster sizes are powers of two, so a read then never
   * straddles a cluster boundary -- which matters because f_read clips each direct
   * transfer at the end of the current cluster (ff.c "Clip at cluster boundary"):
   * measured on a 4 KB-cluster volume, a 6,656-B chunk (the plain "rest of an 8 KB
   * scratch") cost 2.6 disk_read calls per read; a 4 KB chunk costs exactly one. */
  uint32_t chunk = a;
  for (uint32_t c = 0x40000000u; c >= a; c >>= 1) {
    uint32_t ovl_c = (ovl_min + c - 1u) / c * c;
    if (c <= cap - ovl_min && ovl_c + c <= cap) { chunk = c; break; }
  }
  sw->size = size; sw->look = look;
  sw->ovl = (ovl_min + chunk - 1u) / chunk * chunk;
  sw->chunk = chunk;
  return 1;
}

/* Step 1 of each iteration: slide the carry to the front of `w` (not on the first
 * read) and plan the next read. Returns 0 once the whole file has been covered.
 * On 1 the caller must read sw->rd_len bytes at file offset sw->rd_off into
 * w + sw->rd_dst, then call gb_scanwin_filled(). */
static inline int gb_scanwin_plan(GbScanWin* sw, uint8_t* w) {
  if (sw->reads == 0) {
    sw->base = 0; sw->have = 0; sw->first = 0;
    sw->rd_off = 0; sw->rd_dst = 0;
    sw->rd_len = (sw->size < sw->ovl + sw->chunk) ? sw->size : sw->ovl + sw->chunk;
  } else {
    if (sw->base + sw->have >= sw->size) return 0;          /* covered */
    memmove(w, w + (sw->have - sw->ovl), sw->ovl);
    sw->base += sw->have - sw->ovl;
    sw->have = sw->ovl;
    sw->first = sw->ovl - sw->look + 1u;
    sw->rd_off = sw->base + sw->ovl; sw->rd_dst = sw->ovl;
    uint32_t left = sw->size - sw->rd_off;
    sw->rd_len = (left < sw->chunk) ? left : sw->chunk;
  }
  sw->reads++;
  return 1;
}

/* Step 2, after the planned read succeeded: how many NEW positions to offer,
 * starting at w + sw->first (file offset of position i is sw->base + i). */
static inline uint32_t gb_scanwin_filled(GbScanWin* sw) {
  sw->have += sw->rd_len;
  return sw->have - sw->look + 1u - sw->first;
}

/* True on the window that reaches the end of the file -- for a scanner that wants
 * to sweep the final `look - shortest` positions with its shorter patterns. */
static inline int gb_scanwin_is_last(const GbScanWin* sw) {
  return sw->base + sw->have >= sw->size;
}

#endif /* GB_SCANWIN_INCLUDED */
