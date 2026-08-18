#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "sys.h"  /* EWRAM_BSS */

#define LOG_CAP 8192
/* Stop appending past this. A stuck loop logging every frame must not fill the
 * user's card; one line in the file says why it stopped. */
#define LOG_FILE_MAX  (192u * 1024u)
/* After this many consecutive hard failures, stop touching the card entirely.
 * On an EverDrive every write fails by design (flashcartio_write.c's default case
 * returns false), so without this latch each breadcrumb would burn a directory
 * scan for nothing. log_flush_urgent() bypasses it -- see log.h. */
#define LOG_MAX_FAILS 3
/* "/PokeDNA/log.txt" -> "/PokeDNA/log.prev1.txt" needs strlen(path) + 7 bytes. */
#define LOG_ROT_MAX   64

static char EWRAM_BSS s_buf[LOG_CAP];
static unsigned s_len     = 0;   /* bytes in s_buf                                  */
static unsigned s_flushed = 0;   /* bytes of s_buf already committed to the card     */
static unsigned s_lost    = 0;   /* bytes the ring dropped before they were written  */
static int      s_capped  = 0;   /* file hit LOG_FILE_MAX                            */
static int      s_fail    = 0;   /* consecutive hard failures                        */
static int      s_mgba    = 0;

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

void log_begin_run(const char* path) {
  char p1[LOG_ROT_MAX], p2[LOG_ROT_MAX];
  s_flushed = 0; s_lost = 0; s_capped = 0; s_fail = 0;
  if (!rot_name(p1, sizeof p1, path, 1) || !rot_name(p2, sizeof p2, path, 2)) {
    /* Say it out loud. On the card, "rotation was skipped for a name I do not
     * recognise" and "rotation was attempted and failed" look identical, and that
     * ambiguity is exactly what this whole item exists to remove. */
    log_line("log: no rotation for this path (needs a .txt name that fits)");
    return;
  }
  /* Oldest first. Every one of these may fail (first ever boot: FR_NO_FILE;
   * EverDrive / read-only / full card: FR_DISK_ERR or FR_DENIED). All are fine: a
   * failed rotation just means this run appends to the existing file, and the
   * "=== PokeDNA (M0) ===" header line marks the boundary between runs. */
  f_unlink(p2);
  f_rename(p1, p2);
  f_rename(path, p1);
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

/* The one real flush. `urgent` bypasses the failure latch and the size cap; see
 * log_flush_urgent() in log.h for when that is legitimate. */
static int flush_common(const char* path, int urgent) {
  FIL f;
  FRESULT fr, fc;
  UINT want, bw = 0;

  if (!urgent && (s_capped || s_fail >= LOG_MAX_FAILS)) return -2;
  if (s_flushed >= s_len) return 0;            /* nothing new: zero card traffic */

  fr = f_open(&f, path, FA_WRITE | FA_OPEN_APPEND);
  if (fr != FR_OK) { note_fail(); return (int)fr; }

  if (!urgent && f_size(&f) >= LOG_FILE_MAX) { /* runaway logging: say so, once */
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
  fc = f_close(&f);                            /* f_close is what commits the last
                                                * partial sector + the dir entry  */
  if (fr != FR_OK || fc != FR_OK || bw != want) {
    note_fail();
    if (fr != FR_OK) return (int)fr;
    if (fc != FR_OK) return (int)fc;
    return -1;
  }

  /* ONLY a fully-committed flush moves the watermark. If f_close failed, the bytes
   * may or may not be on the card, so the next flush re-sends them: a DUPLICATED
   * line in a crash log is harmless and visible, a MISSING one defeats the whole
   * purpose of the file. */
  s_flushed = s_len;
  s_lost    = 0;
  s_fail    = 0;
  return 0;
}

int log_flush_to_sd(const char* path) { return flush_common(path, 0); }
int log_flush_urgent(const char* path) { return flush_common(path, 1); }
