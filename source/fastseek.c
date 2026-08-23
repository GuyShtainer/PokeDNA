/* See fastseek.h — the single point where FIL.cltbl is assigned. */
#include "fastseek.h"

#include "log.h"

int fastseek_arm(FIL* fp, DWORD* tbl, DWORD items, const char* what) {
  if (!fp || !tbl || items < 4) return 0;   /* 4 DWORDs = the smallest useful table */

  /* RULE 1, mechanical. A write handle must never carry a link map: f_write would
   * take clmt_clust instead of create_chain, and a stale map there writes into
   * another file's clusters. Tested on the handle rather than trusted to call
   * sites, because "we only ever call this on read handles" is exactly the kind of
   * invariant that survives until the day it does not. */
  if (fp->flag & FA_WRITE) {
    log_line("fastseek: REFUSED %s (write handle)", what ? what : "?");
    return 0;
  }

  tbl[0] = items;                            /* caller-supplied capacity, in DWORDs */
  fp->cltbl = tbl;
  FRESULT fr = f_lseek(fp, CREATE_LINKMAP);
  /* FatFs writes the REQUIRED size back BEFORE it checks ours, so this is meaningful
   * even when the call failed for being too small. */
  DWORD need = tbl[0];
  unsigned frags = (need >= 2) ? (unsigned)((need - 2) / 2) : 0;

  if (fr != FR_OK) {
    /* RULE 2. FatFs leaves cltbl set and the table unterminated on
     * FR_NOT_ENOUGH_CORE; leaving it that way makes the next f_read return
     * FR_OK over the wrong sectors. */
    fp->cltbl = 0;
    /* RULE 2b. The ABORTing failures (FR_INT_ERR / FR_DISK_ERR -- including ff.c's new
     * cycle guard) latch into fp->err (ff.c:234), after which EVERY later f_read on this
     * handle returns that error without touching the card, and only f_open can clear it.
     * Building a link map is an OPTIONAL optimisation: its failure must mean "seek the
     * ordinary way", not "this file is dead for the rest of the session".
     * FR_NOT_ENOUGH_CORE already behaved that way; this makes the rest match. */
    fp->err = 0;
    log_line("fastseek: %s frags=%u need=%lu have=%lu FAIL fr=%d", what ? what : "?",
             frags, (unsigned long)need, (unsigned long)items, (int)fr);
    return 0;
  }
  log_line("fastseek: %s frags=%u need=%lu have=%lu ok", what ? what : "?", frags,
           (unsigned long)need, (unsigned long)items);
  return (int)frags;
}
