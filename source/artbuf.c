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

EWRAM_BSS uint16_t mon_decomp[MON_DECOMP_BYTES / 2];
