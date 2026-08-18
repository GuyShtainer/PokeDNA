#ifndef LOG_H
#define LOG_H

/*
 * Three log sinks so we can see what's happening wherever we run:
 *   1. an in-RAM text buffer the UI prints on screen (works on real hardware)
 *   2. mGBA's debug console (only when running under mGBA)
 *   3. a file on the SD card (the persistent artifact for hardware debugging)
 */

void log_init(void);                  /* probe/open the mGBA debug channel   */
int  log_under_mgba(void);            /* 1 if running under mGBA, else 0      */

void log_line(const char* fmt, ...);  /* append a line (RAM buffer + mGBA)    */
void log_clear(void);


/* Rotate the previous runs' logs aside, then start this run's file:
 *     <base>.txt -> <base>.prev1.txt -> <base>.prev2.txt   (older is deleted)
 * Call ONCE, right after the SD is mounted and the app folder exists, BEFORE the
 * first flush. `path` must end in ".txt", or rotation is skipped rather than
 * guessed at (and one line says so).
 *
 * Why this exists: the flush used to open with FA_CREATE_ALWAYS, so the FIRST
 * flush of the NEXT run destroyed the previous run's log -- and the next run is
 * exactly what you do after a hang. The 2026-08-18 DS-Lite hang's evidence was
 * lost that way (docs/analysis-2026-08-18/report-hang.md).
 *
 * Best-effort and silent: on a read-only card, a full card, or an EverDrive
 * (writes are Omega-only and every disk_write fails) it does nothing and logging
 * carries on appending to whatever file is there. Nothing may depend on it. */
void log_begin_run(const char* path);

/* Append everything not yet written to `path` (FA_OPEN_APPEND; the file is created
 * if absent). Returns 0 on success, the FRESULT on a FatFs error, -1 on a short
 * write, -2 once the file hit its size cap or logging has given up.
 *
 * APPEND, not rewrite, for two reasons: (1) a flush that fails halfway leaves
 * everything already on the card intact -- FA_CREATE_ALWAYS truncated FIRST, so a
 * failed flush left an EMPTY file, erasing the evidence it was called to record;
 * (2) a flush costs one short write instead of rewriting the whole 8 KiB buffer.
 *
 * Free (no card traffic at all) when nothing new has been logged since the last
 * call, so repeat callers are cheap. Call after the SD is mounted. */
int  log_flush_to_sd(const char* path);

/* Same, but ignores the "three consecutive failures, stop touching the card"
 * latch and the size cap. For the LAST words of a run only -- halt_msg() and the
 * reboot path -- where a silently dropped line is the difference between a log
 * that explains the halt and a log that just stops. Everything else must use
 * log_flush_to_sd(). */
int  log_flush_urgent(const char* path);

/* --- the log's own health, readable WITHOUT the log -------------------------
 *
 * The failure latch is circular: the "SD writes failing, logging off" line it writes
 * cannot reach a card that is refusing writes. On 2026-08-18 that produced the worst
 * possible artifact -- a run that hung with NOTHING on the card, indistinguishable from
 * a run that never started. So the state is also readable here, and the UI paints it
 * (pdna_main.c's load_phase badge + the boot line): a trail that survives a failure of
 * the trail-writing mechanism.
 *
 * All pure C, no card traffic, safe to call anywhere -- including from a screen drawn
 * between SD transfers (never during one: nothing here touches the cart bus). */
#define LOG_HEALTH_OK       0   /* every flush committed AND the card was read back  */
#define LOG_HEALTH_FAILING  1   /* 1..2 consecutive failures: still trying          */
#define LOG_HEALTH_OFF      2   /* latched off after LOG_MAX_FAILS (retries rarely) */
#define LOG_HEALTH_CAPPED   3   /* this run hit its byte budget: intentionally quiet */
#define LOG_HEALTH_LOST     4   /* the card ACKED bytes it did not keep (read back) */
#define LOG_HEALTH_UNVERIF  5   /* the read-back itself failed: cannot vouch either way */
int  log_health(void);

/* --- the read-back, and why LOG_HEALTH_OK could not be trusted without it -----
 *
 * Every counter above is downstream of a RETURN CODE, and on this hardware a return
 * code is not evidence. An EZ-Flash write has no retry and no read-back:
 * flashcartio_write_sector just hands back _EZFO_writeSectors' verdict, so a card (or a
 * cart contact) that acknowledges a sector it never stored is invisible all the way up
 * -- FatFs returns FR_OK, f_close returns FR_OK, s_fail stays 0, and the badge paints a
 * healthy "log 1" for a card holding nothing. tests/host_logfat_test.c's t_card_that_lies
 * reproduces exactly that over the real FatFs (ramdisk.h's rd_lie_writes), and it is the
 * failure docs/kb/safety-pipeline.md's verified-write pattern exists for.
 *
 * So a committed flush is now followed by a READ: f_stat the log and compare its size
 * with the size FatFs itself reported after the write. A read is safe wherever the
 * flush was (no remount, no second write), and it runs on the run's FIRST commit --
 * the boot flush, whose verdict the boot dialog reads -- then every LOG_VERIFY_EVERY-th
 * commit and on every urgent flush, so a card that starts lying mid-run is still caught.
 *
 * The three outcomes are kept distinct on purpose: "the card kept it", "the card did
 * NOT keep it", and "the read-back could not tell me" are three different things, and
 * collapsing the third into the first is the exact bug this fixes. LOST is STICKY for
 * the run: one proven lie is not undone by a later f_stat that happens to agree. */
#define LOG_VERIFY_UNKNOWN  0   /* nothing committed yet, so nothing to read back   */
#define LOG_VERIFY_GOOD     1   /* the card's copy is at least the size we wrote    */
#define LOG_VERIFY_LOST     2   /* the file is missing or SHORT: acked, not stored  */
#define LOG_VERIFY_BLIND    3   /* f_stat failed for another reason: cannot vouch   */
int  log_verified(void);           /* LOG_VERIFY_*                                  */
int  log_verify_result(void);      /* the FRESULT from the last read-back (0 = ok)  */
unsigned long log_card_bytes(void);   /* bytes the card admitted to at that read-back */
unsigned long log_expect_bytes(void); /* bytes FatFs said the file held after the write */

/* What log_begin_run() managed, which is the earliest signal that this run's log is in
 * trouble -- it happens before the first flush. */
#define LOG_ROT_NONE         0  /* nothing to rotate (first ever boot), or not called */
#define LOG_ROT_DONE         1  /* the previous run's file is now <base>.prev1.txt    */
#define LOG_ROT_REFUSED      2  /* the card refused the rename: writes are in trouble */
#define LOG_ROT_UNSUPPORTED (-1)/* the path is not a ".txt" name that fits           */
int  log_rotation(void);

int  log_last_result(void);        /* the last flush's return value (0 = ok)        */
int  log_fail_count(void);         /* consecutive hard failures right now           */
unsigned long log_flush_count(void); /* flushes fully committed this run             */
unsigned log_pending_bytes(void);  /* buffered bytes not yet on the card            */

/* One short badge for the screen: "log R 12" / "LOG ERR e10" / "LOG OFF e5" /
 * "LOG FULL" / "LOG LOST" / "LOG ?e1". Needs 12 bytes to never truncate. */
void log_health_str(char* out, unsigned cap);

#endif /* LOG_H */
