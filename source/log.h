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

#endif /* LOG_H */
