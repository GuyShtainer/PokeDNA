#ifndef GB_SCAN_GUARD_INCLUDED
#define GB_SCAN_GUARD_INCLUDED

#include <stdint.h>
#include <string.h>

/*
 * gb_scan_guard — the stop rules a long run of SD reads must obey, kept as pure C
 * so tests/host_gbscan_test.c can drive them with a fake clock. gb_art_source.c's
 * read shim consults one of these before EVERY read of a Game Boy ROM.
 *
 * Three ways a scan stops early, latched in the ORDER they happened -- the first
 * reason wins and nothing later overwrites it:
 *
 *   CANCEL   the caller's progress callback said stop (the user pressed B);
 *   TIMEOUT  more than `limit` clock units elapsed since init (0 = no limit). The
 *            clock is the caller's: on the cart it is perf_ticks(), a 16,384 Hz
 *            hardware timer that keeps
 *            counting while the EZ-Flash driver has IRQs off -- a VBlank-counted
 *            frame clock would NOT, since no VBlank IRQ is delivered during a
 *            transfer (pdna_main.c's heartbeat comment), which is exactly when a
 *            timeout matters;
 *   READ_ERR a read failed. FatFs latches fp->err on any ABORTing failure and every
 *            later f_read on that handle returns the same error without touching
 *            the card (lib/fatfs/ff.c ABORT), so retrying is pointless -- and a
 *            scanner's own loops (a bank sweep of up to 127 x 13 decodes) would
 *            otherwise spin through thousands of instant failures before giving up.
 *
 * Once stopped, admit() answers 0 immediately, forever: the handle is never touched
 * again, the scanner unwinds on its first refused read, and the caller reads
 * g->stop to say WHY. `hi` (the high-water mark of off+len) is the honest "bytes
 * covered so far" for a progress bar -- reads are not strictly monotonic (the
 * header parse and the post-scan verifies seek about), so a max, not a sum.
 */

enum {
  GB_SCAN_OK = 0,
  GB_SCAN_STOP_CANCEL,
  GB_SCAN_STOP_TIMEOUT,
  GB_SCAN_STOP_READ_ERR
};

typedef struct GbScanGuard {
  uint32_t total;      /* bytes the operation covers (the ROM size), for progress     */
  uint32_t hi;         /* high-water mark of off+len over admitted reads              */
  uint32_t reads;      /* reads admitted                                              */
  uint32_t t0;         /* clock at init -- any monotonic unit the caller likes         */
  uint32_t limit;      /* in the same unit; 0 = never time out                        */
  uint32_t tick_mask;  /* a tick (poll input / redraw) every tick_mask+1 reads; must  *
                        * be 2^n-1                                                    */
  uint8_t  stop;       /* GB_SCAN_OK or the FIRST reason the run stopped              */
} GbScanGuard;

static inline void gb_scan_guard_init(GbScanGuard* g, uint32_t total, uint32_t now,
                                      uint32_t limit, uint32_t tick_mask) {
  memset(g, 0, sizeof *g);
  g->total = total; g->t0 = now; g->limit = limit; g->tick_mask = tick_mask;
}

static inline uint32_t gb_scan_guard_elapsed(const GbScanGuard* g, uint32_t now) {
  return now - g->t0;          /* unsigned wrap-safe for any span under 2^32 units */
}

/* Ask permission for one read. Returns 1 if it may proceed (and counts it), else 0
 * with g->stop set. *tick is 1 on the first admitted read and every tick_mask+1
 * reads after -- the caller polls input and redraws then, and if it wants to stop
 * it calls gb_scan_guard_cancel() and does NOT perform the read. */
static inline int gb_scan_guard_admit(GbScanGuard* g, uint32_t off, uint32_t len,
                                      uint32_t now, int* tick) {
  if (tick) *tick = 0;
  if (g->stop != GB_SCAN_OK) return 0;
  if (g->limit && gb_scan_guard_elapsed(g, now) > g->limit) {
    g->stop = GB_SCAN_STOP_TIMEOUT;
    return 0;
  }
  if (tick) *tick = (g->reads & g->tick_mask) == 0;
  g->reads++;
  uint32_t end = off + len;
  if (end < off) end = 0xFFFFFFFFu;    /* saturate a wrapped off+len */
  if (end > g->hi) g->hi = end;
  return 1;
}

/* The caller's own stop (B pressed). No-op once already stopped for another reason. */
static inline void gb_scan_guard_cancel(GbScanGuard* g) {
  if (g->stop == GB_SCAN_OK) g->stop = GB_SCAN_STOP_CANCEL;
}

/* A read failed. Same first-reason-wins rule. */
static inline void gb_scan_guard_fail(GbScanGuard* g) {
  if (g->stop == GB_SCAN_OK) g->stop = GB_SCAN_STOP_READ_ERR;
}

#endif /* GB_SCAN_GUARD_INCLUDED */
