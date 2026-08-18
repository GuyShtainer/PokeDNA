/* Host test for source/log.c's append/watermark accounting and boot rotation.
 *
 *   cc -std=c11 -I tests/hostff -I source tests/host_log_test.c source/log.c -o /tmp/hl
 *
 * log.c includes only "ff.h" and "sys.h", both shimmed by include path in
 * tests/hostff/, so the file under test compiles here UNCHANGED.
 *
 * What this protects: /PokeDNA/log.txt used to be opened FA_CREATE_ALWAYS, which
 * truncated on every flush -- so the next run destroyed the previous run's log
 * (that is how the 2026-08-18 DS-Lite hang's evidence was lost) AND a flush that
 * failed halfway left an EMPTY file, erasing the record it was called to write.
 * The fix is append-with-a-watermark plus boot rotation, and the watermark
 * arithmetic is the only real logic in it. A duplicated line in a crash log is
 * harmless and visible; a MISSING one defeats the file's whole purpose, so every
 * assertion below is written from that direction.
 *
 * NOTE: log_init() pokes mGBA's MMIO at 0x4FFF780 and must never run here; the
 * tests use log_clear(), and s_mgba stays 0 so mgba_emit() is a no-op.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "log.h"

int hostff_fail_open = 0;
int hostff_fail_writes = 0;
int hostff_fail_close = 0;
int hostff_opens = 0;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define P  "/tmp/pdna_hostlog.txt"
#define P1 "/tmp/pdna_hostlog.prev1.txt"
#define P2 "/tmp/pdna_hostlog.prev2.txt"

static char* slurp(const char* path, long* len) {
  FILE* f = fopen(path, "rb");
  char* b;
  long n;
  if (!f) { *len = -1; return 0; }
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  b = malloc((size_t)n + 1);
  if (fread(b, 1, (size_t)n, f) != (size_t)n) { /* short read: still NUL-terminate */ }
  b[n] = 0;
  fclose(f);
  *len = n;
  return b;
}

static int count_occurrences(const char* hay, const char* needle) {
  int n = 0;
  const char* p = hay;
  size_t l = strlen(needle);
  while ((p = strstr(p, needle)) != 0) { n++; p += l; }
  return n;
}

static void reset_all(void) {
  remove(P); remove(P1); remove(P2);
  hostff_fail_open = hostff_fail_writes = hostff_fail_close = 0;
  log_clear();
}

int main(void) {
  char* txt;
  long n;

  /* ---- T1: two flushes, each line lands exactly once --------------------- */
  reset_all();
  log_line("alpha");
  CHECK(log_flush_to_sd(P) == 0, "T1 first flush failed");
  log_line("bravo");
  CHECK(log_flush_to_sd(P) == 0, "T1 second flush failed");
  txt = slurp(P, &n);
  CHECK(txt != 0, "T1 no file");
  if (txt) {
    CHECK(count_occurrences(txt, "alpha") == 1, "T1 'alpha' x%d", count_occurrences(txt, "alpha"));
    CHECK(count_occurrences(txt, "bravo") == 1, "T1 'bravo' x%d", count_occurrences(txt, "bravo"));
    free(txt);
  }

  /* ---- T2: nothing new logged => zero card traffic ------------------------ */
  hostff_opens = 0;
  CHECK(log_flush_to_sd(P) == 0, "T2 idle flush returned non-zero");
  CHECK(hostff_opens == 0, "T2 idle flush opened the file %d time(s)", hostff_opens);

  /* ---- T3: f_write failure must NOT advance the watermark ----------------- */
  reset_all();
  log_line("keep-me");
  hostff_fail_writes = 1;
  CHECK(log_flush_to_sd(P) != 0, "T3 failing write reported success");
  hostff_fail_writes = 0;
  CHECK(log_flush_to_sd(P) == 0, "T3 recovery flush failed");
  txt = slurp(P, &n);
  CHECK(txt && count_occurrences(txt, "keep-me") == 1,
        "T3 pending text was lost or doubled");
  free(txt);

  /* ---- T3b: f_close failure is the only hardware-reachable flush failure --
   * On real FatFs a sub-512-byte append returns FR_OK from f_write without
   * touching the card; disk_write happens in f_close -> f_sync. So this is THE
   * branch that keeps a hole out of the log. A duplicate here is acceptable. */
  reset_all();
  log_line("close-fail");
  hostff_fail_close = 1;
  CHECK(log_flush_to_sd(P) != 0, "T3b failing close reported success");
  hostff_fail_close = 0;
  log_line("after");
  CHECK(log_flush_to_sd(P) == 0, "T3b recovery flush failed");
  txt = slurp(P, &n);
  CHECK(txt && count_occurrences(txt, "close-fail") >= 1,
        "T3b text pending at a failed close never reached the card");
  CHECK(txt && count_occurrences(txt, "after") == 1, "T3b later line missing/doubled");
  free(txt);

  /* ---- T4: ring wrap with no interleaved flush => a loss marker ----------- */
  reset_all();
  for (int i = 0; i < 400; i++) log_line("filler-%04d-0123456789012345678901234567890123456789", i);
  CHECK(log_flush_to_sd(P) == 0, "T4 flush failed");
  txt = slurp(P, &n);
  CHECK(txt && strstr(txt, "log bytes lost before flush") != 0,
        "T4 no loss marker after a ring wrap");
  CHECK(txt && strstr(txt, "filler-0399") != 0, "T4 newest line missing");
  free(txt);

  /* ---- T5: ring wrap WITH interleaved flushes loses nothing --------------- */
  reset_all();
  for (int i = 0; i < 400; i++) {
    log_line("run-%04d-0123456789012345678901234567890123456789", i);
    CHECK(log_flush_to_sd(P) == 0, "T5 flush %d failed", i);
  }
  txt = slurp(P, &n);
  CHECK(txt && strstr(txt, "log bytes lost before flush") == 0,
        "T5 lost bytes despite flushing every line");
  if (txt) {
    int seen = 0;
    for (int i = 0; i < 400; i++) {
      char pat[32];
      snprintf(pat, sizeof pat, "run-%04d-", i);
      if (count_occurrences(txt, pat) == 1) seen++;
    }
    CHECK(seen == 400, "T5 only %d/400 lines present exactly once", seen);
  }
  free(txt);

  /* ---- T6: size cap writes its line once, then every flush is a no-op ----- */
  reset_all();
  {
    FILE* f = fopen(P, "wb");
    char pad[1024];
    memset(pad, 'x', sizeof pad);
    for (int i = 0; i < 200; i++) fwrite(pad, 1, sizeof pad, f);  /* 200 KiB > 192 KiB */
    fclose(f);
  }
  log_line("past-the-cap");
  CHECK(log_flush_to_sd(P) == -2, "T6 flush past the cap did not report -2");
  log_line("also-past");
  CHECK(log_flush_to_sd(P) == -2, "T6 second flush past the cap did not report -2");
  txt = slurp(P, &n);
  CHECK(txt && count_occurrences(txt, "log size cap reached") == 1,
        "T6 cap line written %d times",
        txt ? count_occurrences(txt, "log size cap reached") : -1);
  CHECK(txt && strstr(txt, "past-the-cap") == 0, "T6 wrote past the cap anyway");
  free(txt);
  /* ...and log_flush_urgent must still get the last words out. */
  CHECK(log_flush_urgent(P) == 0, "T6 urgent flush was blocked by the cap");
  txt = slurp(P, &n);
  CHECK(txt && strstr(txt, "past-the-cap") != 0, "T6 urgent flush wrote nothing");
  free(txt);

  /* ---- T7: rotation shifts log.txt -> prev1 -> prev2, and stops there ----- */
  reset_all();
  log_line("run-A"); log_flush_to_sd(P);
  log_begin_run(P); log_clear();
  log_line("run-B"); log_flush_to_sd(P);
  log_begin_run(P); log_clear();
  log_line("run-C"); log_flush_to_sd(P);
  txt = slurp(P,  &n); CHECK(txt && strstr(txt, "run-C") != 0, "T7 log.txt is not run C"); free(txt);
  txt = slurp(P1, &n); CHECK(txt && strstr(txt, "run-B") != 0, "T7 prev1 is not run B"); free(txt);
  txt = slurp(P2, &n); CHECK(txt && strstr(txt, "run-A") != 0, "T7 prev2 is not run A"); free(txt);
  txt = slurp("/tmp/pdna_hostlog.prev3.txt", &n);
  CHECK(n == -1, "T7 a prev3 file exists — rotation depth is not 2");
  free(txt);

  /* ---- T8: an unrecognised path is refused, loudly, not guessed at -------- */
  reset_all();
  remove("/tmp/pdna_hostlog.dat");
  log_begin_run("/tmp/pdna_hostlog.dat");
  log_line("x");
  log_flush_to_sd("/tmp/pdna_hostlog.dat");
  txt = slurp("/tmp/pdna_hostlog.dat", &n);
  CHECK(txt && strstr(txt, "no rotation for this path") != 0,
        "T8 a skipped rotation left no trace");
  free(txt);
  remove("/tmp/pdna_hostlog.dat");

  /* ---- T9: three failures latch logging off; urgent still gets through ---- */
  reset_all();
  hostff_fail_open = 1;
  for (int i = 0; i < 3; i++) { log_line("try-%d", i); CHECK(log_flush_to_sd(P) != 0, "T9 flush %d 'succeeded'", i); }
  CHECK(log_flush_to_sd(P) == -2, "T9 did not latch after 3 failures");
  hostff_fail_open = 0;
  hostff_opens = 0;
  CHECK(log_flush_to_sd(P) == -2, "T9 latch released itself");
  CHECK(hostff_opens == 0, "T9 latched flush still touched the card");
  CHECK(log_flush_urgent(P) == 0, "T9 urgent flush was blocked by the latch");
  txt = slurp(P, &n);
  CHECK(txt && strstr(txt, "SD writes failing, logging off") != 0,
        "T9 the give-up line never made it into the log");
  CHECK(txt && strstr(txt, "try-0") != 0, "T9 urgent flush dropped the pending text");
  free(txt);

  remove(P); remove(P1); remove(P2);
  printf(fails ? "FAILED (%d)\n" : "all pass\n", fails);
  return fails ? 1 : 0;
}
