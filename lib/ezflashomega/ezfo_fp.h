/* ezfo_fp.h -- the EZ-Flash "which page am I on" fingerprint, ONE definition shared by
 * the driver (io_ezfo.c, where it is forced inline into an EWRAM_CODE function and reads
 * the cart window) and the host test (tests/host_fpwalk_test.c, where it walks heap
 * buffers). Pure C: <stdint.h> only, no GBA headers, no division, no memset.
 *
 * An image spanning [lo, hi) is sampled at EZFO_FP_WINS windows of EZFO_FP_BYTES: window
 * i (i < WINS-1) starts (i+1) * (span >> WINS_SHIFT) into the image, the last window is
 * the image's final EZFO_FP_BYTES (so a shorter or partial copy is caught at the tail);
 * an image too small for that gets its first EZFO_FP_BYTES for every window. */
#ifndef EZFO_FP_H
#define EZFO_FP_H

#include <stdint.h>

#define EZFO_FP_WINS       32
#define EZFO_FP_WINS_SHIFT 5                        /* log2(EZFO_FP_WINS)             */
#define EZFO_FP_WORDS      16                       /* 64 B per window                */
#define EZFO_FP_BYTES      (EZFO_FP_WORDS * 4)

#ifndef EZFO_FP_FN
#define EZFO_FP_FN static inline
#endif
/* The 32-bit word the checksum rolls over. The driver passes its own u32 (sys.h: unsigned
 * int), which arm-none-eabi's uint32_t (long unsigned int) is not pointer-compatible
 * with, so the includer may name the type; it must be exactly 32 bits either way. */
#ifndef EZFO_FP_WORD
#define EZFO_FP_WORD uint32_t
#endif
typedef char ezfo_fp_word_must_be_32_bits[sizeof(EZFO_FP_WORD) == 4 ? 1 : -1];

/* Where window i starts, 4-byte aligned, always inside [lo, hi - EZFO_FP_BYTES]. */
EZFO_FP_FN unsigned long ezfo_fp_window(int i, unsigned long lo, unsigned long hi) {
  unsigned long span = hi - lo;
  if (span < (unsigned long)EZFO_FP_BYTES * (EZFO_FP_WINS + 1)) return lo;
  if (i == EZFO_FP_WINS - 1) return (hi - EZFO_FP_BYTES) & ~3ul;
  return (lo + (span >> EZFO_FP_WINS_SHIFT) * (unsigned long)(i + 1)) & ~3ul;
}

/* Walk the windows once. take != 0: store each window's order-sensitive rolling checksum
 * into fp[EZFO_FP_WINS]; take == 0: compare against fp[] and stop at the first mismatch
 * (returns 0). Returns 1 otherwise. Volatile reads: every word is a real cart read. */
EZFO_FP_FN int ezfo_fp_walk(EZFO_FP_WORD* fp, int take, unsigned long lo, unsigned long hi) {
  int i, n;
  for (i = 0; i < EZFO_FP_WINS; i++) {
    const volatile EZFO_FP_WORD* p = (const volatile EZFO_FP_WORD*)ezfo_fp_window(i, lo, hi);
    EZFO_FP_WORD s = 0x9E3779B9u;
    for (n = 0; n < EZFO_FP_WORDS; n++) s = ((s << 1) | (s >> 31)) ^ *p++;
    if (take) fp[i] = s;
    else if (fp[i] != s) return 0;
  }
  return 1;
}

#endif /* EZFO_FP_H */
