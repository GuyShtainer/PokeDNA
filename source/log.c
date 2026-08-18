#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "sys.h"  /* EWRAM_BSS */

#define LOG_CAP 8192
/* Stop appending past this MANY BYTES OF THIS RUN. A stuck loop logging every frame
 * must not fill the user's card; one line in the file says why it stopped.
 *
 * Deliberately a per-RUN budget, not a file size. It used to be a file size, and that
 * made a stale file able to silence a whole run: if rotation failed (a read-only card, a
 * name collision) the run appended to a file that was ALREADY at the limit, so the very
 * first flush wrote one cap line and then went quiet -- the exact "it didn't log
 * anything" artifact this file exists to prevent. The file still has a ceiling, an order
 * of magnitude higher, so a card cannot be filled by an un-rotatable log either. */
#define LOG_RUN_MAX   (192u * 1024u)
#define LOG_FILE_MAX  (1024u * 1024u)
/* After this many consecutive hard failures, stop touching the card entirely.
 * On an EverDrive every write fails by design (flashcartio_write.c's default case
 * returns false), so without this latch each breadcrumb would burn a directory
 * scan for nothing. log_flush_urgent() bypasses it -- see log.h. */
#define LOG_MAX_FAILS 3
/* ...but not forever. An EZ-Flash write failure can be transient (a nudged cart, a card
 * that came back), and a latch that never retries turns one bad write into a silent run.
 * After the latch, one call in LOG_RETRY_EVERY actually tries the card: on an EverDrive
 * that is 1 directory scan instead of 32, and on a card that recovered the log resumes. */
#define LOG_RETRY_EVERY 32
/* LOG_VERIFY_EVERY (log.h) reads the card back on the run's FIRST commit -- that is the
 * boot flush, whose verdict the boot dialog shows -- and every Nth commit after it. Not
 * every commit: a read-back is a directory scan, roughly the cost of the f_open the flush
 * already pays, and a breadcrumb-per-phase log would double its card traffic to catch a
 * card that lies once. */
/* "/PokeDNA/log.txt" -> "/PokeDNA/log.prev1.txt" needs strlen(path) + 7 bytes. */
#define LOG_ROT_MAX   64

static char EWRAM_BSS s_buf[LOG_CAP];
static unsigned s_len     = 0;   /* bytes in s_buf                                  */
static unsigned s_flushed = 0;   /* bytes of s_buf already committed to the card     */
static unsigned s_lost    = 0;   /* bytes the ring dropped before they were written  */
static int      s_capped  = 0;   /* this run hit LOG_RUN_MAX (or the file the ceiling) */
static int      s_fail    = 0;   /* consecutive hard failures                        */
static int      s_mgba    = 0;

/* --- health, so the log's own state can be READ WITHOUT THE LOG --------------
 * The failure latch is circular by construction: the "SD writes failing, logging off"
 * line it writes cannot reach a card that is refusing writes. So the state lives in
 * these counters too, and the UI paints them (see log_health_str) -- that is the only
 * trail that survives a failure of the trail-writing mechanism. */
static int           s_last      = 0;   /* result of the last flush attempt          */
static unsigned long s_okn       = 0;   /* flushes fully committed this run          */
static unsigned long s_run_bytes = 0;   /* bytes this run has put on the card        */
static unsigned      s_skips     = 0;   /* refused calls since the latch (retry ctr) */
static int           s_rot       = LOG_ROT_NONE;

/* --- the read-back, because a return code is not evidence --------------------
 * See log.h. s_expect is not a guess: it is f_size() taken from the open handle AFTER
 * the write and BEFORE the close, i.e. the size FatFs itself claims the file now has.
 * Comparing THAT with what the card admits to afterwards makes the check exact for
 * every case -- a fresh file, an append after a refused rotation, or extra marker
 * lines -- instead of only for the tidy one. */
static unsigned long s_expect     = 0;  /* size FatFs reported after the last commit */
static unsigned long s_card_bytes = 0;  /* size the card admitted to at the read-back */
static int           s_verify     = LOG_VERIFY_UNKNOWN;
static int           s_vfr        = 0;  /* FRESULT of the last read-back              */

/* --- mGBA debug interface ------------------------------------------------- */
/* Write 0xC0DE to ENABLE; if it reads back 0x1DEA we're under mGBA. Strings
 * up to 255 bytes go in the buffer at 0x4FFF600, then FLAGS = 0x100 | level. */
#define MGBA_REG_ENABLE (*(volatile unsigned short*)0x4FFF780)
#define MGBA_REG_FLAGS  (*(volatile unsigned short*)0x4FFF700)
#define MGBA_LOG_BUF    ((volatile char*)0x4FFF600)
#define MGBA_LEVEL_INFO 3

void log_init(void) {
  MGBA_REG_ENABLE = 0xC0DE;
  s_mgba = (MGBA_REG_ENABLE == 0x1DEA) ? 1 : 0;
  log_clear();
}

int log_under_mgba(void) { return s_mgba; }

void log_clear(void) {
  s_len = 0;
  s_flushed = 0;                  /* nothing in the (new) buffer is on the card yet */
  s_lost = 0;
  s_capped = 0;                   /* "start over" means the give-up latches too */
  s_fail = 0;
  s_skips = 0;
  s_last = 0;
  s_okn = 0;
  s_run_bytes = 0;
  s_rot = LOG_ROT_NONE;
  s_expect = 0;
  s_card_bytes = 0;
  s_verify = LOG_VERIFY_UNKNOWN;   /* "not checked yet" -- never "checked and fine" */
  s_vfr = 0;
  s_buf[0] = 0;
}

static void mgba_emit(const char* line) {
  if (!s_mgba) return;
  int i = 0;
  while (line[i] && i < 255) {
    MGBA_LOG_BUF[i] = line[i];
    i++;
  }
  MGBA_LOG_BUF[i] = 0;
  MGBA_REG_FLAGS = 0x100 | MGBA_LEVEL_INFO;
}

void log_line(const char* fmt, ...) {
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  mgba_emit(tmp);

  unsigned n = (unsigned)strlen(tmp);
  /* If the buffer would overflow, drop the oldest half -- and move the flush
   * watermark with it so it keeps pointing at the SAME TEXT. Otherwise the next
   * append either re-sends text already on the card (duplicates) or skips text
   * that never got there (a silent hole in a crash log). Anything dropped that had
   * not yet reached the card is genuinely gone: count it, so the FILE can say so
   * instead of silently splicing two non-adjacent spans together. */
  if (s_len + n + 2 >= LOG_CAP) {
    unsigned keep = LOG_CAP / 2;
    unsigned dropped;
    if (s_len > keep) {
      dropped = s_len - keep;
      memmove(s_buf, s_buf + dropped, keep);
      s_len = keep;
    } else {
      dropped = s_len;
      s_len = 0;
    }
    if (s_flushed >= dropped) {
      s_flushed -= dropped;
    } else {
      s_lost += dropped - s_flushed;
      s_flushed = 0;
    }
  }
  memcpy(s_buf + s_len, tmp, n);
  s_len += n;
  s_buf[s_len++] = '\n';
  s_buf[s_len] = 0;
}

/* "/PokeDNA/log.txt" + n -> "/PokeDNA/log.prevN.txt". Returns 0 (rotation skipped)
 * when it would not fit or `path` has no ".txt" tail -- we rotate only names we
 * recognise rather than inventing one. n is 1..9 by contract. */
static int rot_name(char* out, unsigned cap, const char* path, int n) {
  unsigned L = (unsigned)strlen(path);
  if (n < 1 || n > 9) return 0;
  if (L < 4 || L + 7 >= cap) return 0;
  if (strcmp(path + L - 4, ".txt") != 0) return 0;
  memcpy(out, path, L - 4);            /* base, without ".txt"          */
  memcpy(out + L - 4, ".prev", 5);
  out[L + 1] = (char)('0' + n);
  memcpy(out + L + 2, ".txt", 5);      /* copies the terminating NUL too */
  return 1;
}

/* f_rename REFUSES to replace an existing name (FR_EXIST) -- unlike POSIX rename(),
 * which is why the stdio-shim test could not see this. Rotation keeps exactly two
 * generations, so an occupant of the target IS the generation we are entitled to drop:
 * unlink it and retry once. Without this, one undeletable rotation slot froze rotation
 * for good, and every later run appended into the previous run's file. */
static FRESULT rot_move(const char* from, const char* to) {
  FRESULT fr = f_rename(from, to);
  if (fr == FR_EXIST) {
    f_unlink(to);
    fr = f_rename(from, to);
  }
  return fr;
}

void log_begin_run(const char* path) {
  char p1[LOG_ROT_MAX], p2[LOG_ROT_MAX];
  FRESULT fr;
  s_lost = 0; s_capped = 0; s_fail = 0; s_skips = 0;
  s_last = 0; s_okn = 0; s_run_bytes = 0;
  s_expect = 0; s_card_bytes = 0; s_verify = LOG_VERIFY_UNKNOWN; s_vfr = 0;
  s_rot = LOG_ROT_NONE;
  if (!rot_name(p1, sizeof p1, path, 1) || !rot_name(p2, sizeof p2, path, 2)) {
    /* Say it out loud. On the card, "rotation was skipped for a name I do not
     * recognise" and "rotation was attempted and failed" look identical, and that
     * ambiguity is exactly what this whole item exists to remove. */
    s_rot = LOG_ROT_UNSUPPORTED;
    log_line("log: no rotation for this path (needs a .txt name that fits)");
    return;
  }
  /* Oldest first. Every one of these may fail (first ever boot: FR_NO_FILE;
   * EverDrive / read-only / full card: FR_DISK_ERR or FR_DENIED). All are fine: a
   * failed rotation just means this run appends to the existing file, and the
   * "=== PokeDNA (M0) ===" header line marks the boundary between runs. */
  f_unlink(p2);
  rot_move(p1, p2);
  fr = rot_move(path, p1);
  if (fr == FR_OK) {
    s_rot = LOG_ROT_DONE;
    s_flushed = 0;              /* the file we were appending to is gone: resend all */
  } else if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    s_rot = LOG_ROT_NONE;       /* first ever boot: nothing to move aside */
    s_flushed = 0;
  } else {
    /* The card REFUSED a rename. That is the earliest possible warning that this run's
     * log is in trouble -- it happens before the first flush -- so it is worth a
     * distinct state the UI can show, and the watermark stays put: the target file is
     * still there, and anything already in it stayed in it. */
    s_rot = LOG_ROT_REFUSED;
    log_line("log: rotation refused (fr=%d) - appending to the old file", (int)fr);
  }
}

/* Count one hard failure, and the moment logging gives up say so IN the log. That
 * line reaches the card if the card ever comes back, and reaches the screen/mGBA
 * either way -- so a log that just stops can never be mistaken for a run that just
 * stopped. (Without it, "the cart failed three writes" and "the tool hung before
 * the next breadcrumb" leave a byte-identical file.) */
static void note_fail(void) {
  s_fail++;
  if (s_fail == LOG_MAX_FAILS) log_line("log: SD writes failing, logging off");
}

/* The app folder does not exist yet (a fresh card, or the user deleted /PokeDNA), which
 * is the one f_open failure that is trivially self-inflicted AND makes an entire run
 * silent -- every flush returns FR_NO_PATH and after three of them logging latches off,
 * with nothing on the card to say why. So create the parent of the path we were handed
 * (one level, which is what "/PokeDNA/log.txt" needs) and let the caller retry. */
static void ensure_parent_dir(const char* path) {
  char dir[LOG_ROT_MAX];
  unsigned L = (unsigned)strlen(path), i, cut = 0;
  for (i = 0; i < L; i++)
    if (path[i] == '/') cut = i;
  if (cut == 0 || cut >= sizeof dir) return;   /* root-level path, or too long to copy */
  memcpy(dir, path, cut);
  dir[cut] = 0;
  f_mkdir(dir);                                /* FR_EXIST is fine, so is failing */
}

/* Everything that needs FatFs' FIL, in a frame of its own.
 *
 * The split is not cosmetic: FIL carries a 512-byte sector buffer (~560 bytes) and
 * FILINFO carries an LFN name buffer (~290 bytes, FF_LFN_BUF=255), and this all lives on
 * the 12,624-byte IWRAM stack. Nesting the read-back inside this function would have put
 * both on the stack at once for every flush; keeping them in sibling frames leaves the
 * peak exactly where it was before the read-back existed. d95202f is what a careless
 * frame on that stack costs.
 *
 * Returns 0 on a fully committed write (with *wrote = bytes appended and *size_after =
 * the size FatFs reports for the file, taken from the open handle after the write and
 * before the close), -2 if the size cap fired, -1 on a short write, else the FRESULT. */
static int commit_bytes(const char* path, int urgent, UINT* wrote,
                        unsigned long* size_after) {
  FIL f;
  FRESULT fr, fc;
  UINT want, bw = 0;

  *wrote = 0;
  *size_after = 0;

  fr = f_open(&f, path, FA_WRITE | FA_OPEN_APPEND);
  if (fr == FR_NO_PATH) {                      /* missing app folder: make it, once */
    ensure_parent_dir(path);
    fr = f_open(&f, path, FA_WRITE | FA_OPEN_APPEND);
  }
  if (fr != FR_OK) return (int)fr;

  /* Runaway logging: say so, once. Measured on THIS RUN's bytes (plus an absolute file
   * ceiling), so a stale un-rotated file can no longer silence a fresh run. */
  if (!urgent && (s_run_bytes >= LOG_RUN_MAX || f_size(&f) >= LOG_FILE_MAX)) {
    static const char cap_msg[] = "[log size cap reached - logging stopped]\n";
    UINT bx = 0;
    f_write(&f, cap_msg, (UINT)(sizeof cap_msg - 1), &bx);
    f_close(&f);
    s_capped = 1;
    return -2;
  }

  if (s_lost) {                                /* the ring ate un-written text */
    char m[48]; UINT bx = 0;
    int n = snprintf(m, sizeof m, "[%u log bytes lost before flush]\n", s_lost);
    if (n > 0) f_write(&f, m, (UINT)n, &bx);   /* best-effort marker */
  }

  want = (UINT)(s_len - s_flushed);
  fr = f_write(&f, s_buf + s_flushed, want, &bw);
  /* Taken while the handle is still open: this is FatFs' OWN claim about the file's new
   * size, which is what the read-back holds the card to. */
  *size_after = (unsigned long)f_size(&f);
  fc = f_close(&f);                            /* f_close is what commits the last
                                                * partial sector + the dir entry  */
  if (fr != FR_OK || fc != FR_OK || bw != want)
    return (fr != FR_OK) ? (int)fr : (fc != FR_OK) ? (int)fc : -1;

  *wrote = want;
  return 0;
}

/* Ask the CARD what it kept. Pure read: one directory scan, no write, no remount, so it
 * is safe exactly where the flush that preceded it was safe.
 *
 * Three outcomes, kept apart on purpose (log.h):
 *   file present and >= what FatFs claimed  -> GOOD
 *   file MISSING, or SHORT                  -> LOST, and it STICKS for the run
 *   f_stat failed some other way            -> BLIND: this check cannot vouch, and it
 *                                              must not be allowed to read as GOOD.
 * BLIND is the deliberate answer to "what if the read-back itself fails": it is not OK,
 * it shows on the badge, and it trips the boot dialog -- an unverifiable log is a
 * diagnostic that has stopped being a diagnostic, which is the whole complaint.
 *
 * A word on FatFs' one-sector window cache: f_stat of "/PokeDNA/log.txt" walks the ROOT
 * directory first, so the window cannot still be holding the /PokeDNA sector the just-
 * closed f_sync wrote -- both levels come off the medium. A root-level log path would
 * not have that guarantee; PokeDNA's never is (hard rule 9). */
static void verify_on_card(const char* path) {
  FILINFO fi;
  FRESULT fr = f_stat(path, &fi);

  s_vfr = (int)fr;
  if (fr == FR_OK) {
    s_card_bytes = (unsigned long)fi.fsize;
    if (s_card_bytes < s_expect) {
      if (s_verify != LOG_VERIFY_LOST)
        log_line("log: card kept %lu of %lu bytes - it ACKED writes it did not store",
                 s_card_bytes, s_expect);
      s_verify = LOG_VERIFY_LOST;              /* sticky: one proven lie is enough */
    } else if (s_verify != LOG_VERIFY_LOST) {
      s_verify = LOG_VERIFY_GOOD;
    }
    return;
  }
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    s_card_bytes = 0;
    if (s_verify != LOG_VERIFY_LOST)
      log_line("log: %s is NOT on the card after a flush that returned OK", path);
    s_verify = LOG_VERIFY_LOST;
    return;
  }
  /* FR_DISK_ERR, FR_NOT_READY, FR_INVALID_DRIVE, ... The log may well be fine; we
   * simply cannot say, and saying nothing is how this bug happened. */
  s_card_bytes = 0;
  if (s_verify != LOG_VERIFY_LOST) s_verify = LOG_VERIFY_BLIND;
}

/* The one real flush. `urgent` bypasses the failure latch and the size cap; see
 * log_flush_urgent() in log.h for when that is legitimate. */
static int flush_common(const char* path, int urgent) {
  UINT wrote = 0;
  unsigned long size_after = 0;
  int r;

  if (!urgent) {
    if (s_capped) { s_last = -2; return -2; }
    if (s_fail >= LOG_MAX_FAILS) {
      if (++s_skips < LOG_RETRY_EVERY) { s_last = -2; return -2; }
      s_skips = 0;                             /* every 32nd call, try the card again */
    }
  }
  if (s_flushed >= s_len) { s_last = 0; return 0; }  /* nothing new: zero card traffic */

  r = commit_bytes(path, urgent, &wrote, &size_after);
  if (r == -2) { s_last = -2; return -2; }     /* the size cap: intentional, not a fault */
  if (r != 0) { s_last = r; note_fail(); return r; }

  /* ONLY a fully-committed flush moves the watermark. If f_close failed, the bytes
   * may or may not be on the card, so the next flush re-sends them: a DUPLICATED
   * line in a crash log is harmless and visible, a MISSING one defeats the whole
   * purpose of the file. */
  s_flushed = s_len;
  s_lost    = 0;
  s_fail    = 0;
  s_skips   = 0;
  s_run_bytes += wrote;
  s_expect  = size_after;
  s_okn++;
  s_last = 0;

  /* ...and "committed" is still only FatFs' opinion until the card is read back. The
   * first commit of the run is always checked, because that is the boot flush the boot
   * dialog reports on. */
  if (urgent || s_okn == 1 || (s_okn % LOG_VERIFY_EVERY) == 0) verify_on_card(path);
  return 0;
}

int log_flush_to_sd(const char* path) { return flush_common(path, 0); }
int log_flush_urgent(const char* path) { return flush_common(path, 1); }

/* --- health readouts (pure C: the UI paints these, no card needed) --------- */

/* Ordered by what the user must be told FIRST, not by how the state came about.
 * LOST outranks everything because it is the only state that otherwise looks healthy:
 * a card that silently drops writes reports FR_OK forever, so if it is also capped or
 * failing, "the card is not keeping your bytes" is still the sentence that matters.
 * UNVERIF sits below the real failures (they are more specific) but ABOVE OK -- an
 * unverifiable log must never paint the healthy badge. */
int log_health(void) {
  if (s_verify == LOG_VERIFY_LOST) return LOG_HEALTH_LOST;
  if (s_capped) return LOG_HEALTH_CAPPED;
  if (s_fail >= LOG_MAX_FAILS) return LOG_HEALTH_OFF;
  if (s_fail > 0) return LOG_HEALTH_FAILING;
  if (s_verify == LOG_VERIFY_BLIND) return LOG_HEALTH_UNVERIF;
  return LOG_HEALTH_OK;
}

int          log_last_result(void)  { return s_last; }
int          log_fail_count(void)   { return s_fail; }
int          log_rotation(void)     { return s_rot; }
int          log_verified(void)     { return s_verify; }
int          log_verify_result(void) { return s_vfr; }
unsigned long log_card_bytes(void)  { return s_card_bytes; }
unsigned long log_expect_bytes(void) { return s_expect; }
unsigned long log_flush_count(void) { return s_okn; }
unsigned     log_pending_bytes(void) { return (s_len > s_flushed) ? s_len - s_flushed : 0; }

void log_health_str(char* out, unsigned cap) {
  if (!out || cap == 0) return;
  out[0] = 0;
  switch (log_health()) {
    case LOG_HEALTH_LOST:
      /* The badge for the state that used to paint "log 1" in dim grey. */
      snprintf(out, cap, "LOG LOST");
      break;
    case LOG_HEALTH_UNVERIF:
      snprintf(out, cap, "LOG ?e%d", s_vfr);
      break;
    case LOG_HEALTH_CAPPED:
      snprintf(out, cap, "LOG FULL");
      break;
    case LOG_HEALTH_OFF:
      snprintf(out, cap, "LOG OFF e%d", s_last);
      break;
    case LOG_HEALTH_FAILING:
      snprintf(out, cap, "LOG ERR e%d", s_last);
      break;
    default:
      /* Rotation is worth one glyph of its own: "log R" says the previous run's file was
       * moved aside (so log.prev1.txt is the run Guy wants), "log !R" says the card
       * refused the rename -- an early warning that writes are in trouble. */
      snprintf(out, cap, "log %s%lu",
               (s_rot == LOG_ROT_DONE) ? "R " : (s_rot == LOG_ROT_REFUSED) ? "!R " : "",
               s_okn);
      break;
  }
}
