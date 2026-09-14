/* The 8 KiB EWRAM staging buffer moved out of mon_front.c so it exists in the
 * artless build too. See artbuf.h for the full rationale (docs/analysis-2026-08-19-
 * rom-art/DESIGN.md Sec 0(b) / Phase 0).
 *
 * This file must NOT be added to the artless build's "moved art" list
 * (docs/analysis-2026-08-18/report-artless-speed.md PART 1(a)) -- it is the one
 * thing that has to stay compiled when mon_front.c/mon_back.c and the rest of that
 * list move aside. */
#include "artbuf.h"
#include "sys.h"     /* EWRAM_BSS */

/* aligned(4): gb_art_source.c also uses this as the 8 KB whole-ROM scan window, and
 * lib/fatfs/diskio.c's direct DMA path needs a word-aligned destination (a 2-mod-4
 * one is still correct, but bounces through fc_bounce four sectors at a time). A
 * uint16_t array is only guaranteed 2-aligned; this makes the fast path a fact. */
EWRAM_BSS uint16_t mon_decomp[MON_DECOMP_BYTES / 2] __attribute__((aligned(4)));

/* Plain (IWRAM) .data, NOT EWRAM_BSS -- 4 bytes, and EWRAM has none to spare
 * (docs/SPRITE-ERA-DESIGN.md: 524 B free before this arc, 268 after slice E3's two
 * GB ROM paths). See artbuf.h for the full contract. */
uint32_t artbuf_epoch = 0;
