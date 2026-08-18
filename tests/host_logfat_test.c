/* source/log.c over the REAL lib/fatfs, on a RAM disk, with a flashcart's failures.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_logfat_test.c source/log.c lib/fatfs/ff.c lib/fatfs/ffunicode.c \
 *      tests/hostfat/ramdisk.c -o /tmp/hlf
 *
 * WHY a second log test. tests/host_log_test.c drives log.c through a stdio shim, which
 * gets the watermark arithmetic right but lies about FAT: POSIX rename() REPLACES an
 * existing target while f_rename returns FR_EXIST, fopen("ab") cannot report a missing
 * directory the way FatFs' FR_NO_PATH does, and a full or write-protected volume is not
 * reachable at all. Every one of those is a way the on-card log goes SILENT -- which is
 * exactly what happened on Guy's DS Lite on 2026-08-18: a run that hung with NOTHING in
 * /PokeDNA/log.txt, not even the boot banner, making every other investigation blind.
 *
 * So this harness compiles the shipped ff.c against the shipped ffconf.h (only
 * FF_USE_MKFS differs, so the test can format its own volume) over a RAM disk whose
 * knobs are the failures an EZ-Flash actually has: write-protected, every write fails
 * (that is an EverDrive), the Nth write fails (EZ-Flash writes have no retry), a volume
 * too small to hold the log (a full card), and -- the nastiest -- a card that ACKs every
 * write and keeps nothing (rd_lie_writes), which no return code anywhere in the stack
 * can reveal. See t_card_that_lies.
 */
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "log.h"
#include "ramdisk.h"

#define P  "/PokeDNA/log.txt"
#define P1 "/PokeDNA/log.prev1.txt"
#define P2 "/PokeDNA/log.prev2.txt"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];

/* A fresh formatted volume. `sectors` * 512 bytes; 140 is the smallest FatFs will
 * format, which makes "the card is full" testable. */
static void fresh_card(unsigned sectors, int with_appdir) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(sectors);
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  if (with_appdir) CHECK(f_mkdir("/PokeDNA") == FR_OK, "f_mkdir /PokeDNA failed");
}

/* Read a whole file. Returns its length, or -1 if it does not exist. */
static long slurp(const char* path, char* out, unsigned cap) {
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return -1;
  if (f_read(&f, out, cap - 1, &br) != FR_OK) { f_close(&f); return -1; }
  f_close(&f);
  out[br] = 0;
  return (long)br;
}

static int occurrences(const char* hay, const char* needle) {
  int n = 0; const char* p = hay; size_t l = strlen(needle);
  while ((p = strstr(p, needle)) != 0) { n++; p += l; }
  return n;
}

/* One "run" of the app: rotate, log a marker line, flush. */
static int run_once(const char* marker) {
  log_clear();
  log_begin_run(P);
  log_line("=== %s ===", marker);
  return log_flush_to_sd(P);
}

/* ---------------------------------------------------------------------------- */

static void t_first_boot(void) {
  char buf[4096];
  fresh_card(4096, 1);
  CHECK(run_once("run1") == 0, "first boot: flush failed");
  CHECK(log_rotation() == LOG_ROT_NONE, "first boot: rotation should have nothing to do (%d)",
        log_rotation());
  CHECK(slurp(P, buf, sizeof buf) > 0, "first boot: no log.txt");
  CHECK(strstr(buf, "=== run1 ===") != 0, "first boot: marker missing");
  CHECK(slurp(P1, buf, sizeof buf) == -1, "first boot: prev1 must not exist");
  CHECK(log_health() == LOG_HEALTH_OK, "first boot: health not ok");
  CHECK(log_flush_count() == 1, "first boot: flush count %lu", log_flush_count());
  /* OK now means "read back", not "the write returned 0". */
  CHECK(log_verified() == LOG_VERIFY_GOOD, "first boot: not read back (%d)", log_verified());
  CHECK(log_card_bytes() == log_expect_bytes() && log_expect_bytes() > 0,
        "first boot: card %lu vs expected %lu", log_card_bytes(), log_expect_bytes());
}

static void t_rotation_depth(void) {
  char buf[4096];
  fresh_card(4096, 1);
  CHECK(run_once("run1") == 0, "rot: run1 flush");
  CHECK(run_once("run2") == 0, "rot: run2 flush");
  CHECK(log_rotation() == LOG_ROT_DONE, "rot: run2 should have rotated (%d)", log_rotation());
  CHECK(slurp(P,  buf, sizeof buf) > 0 && strstr(buf, "run2") && !strstr(buf, "run1"),
        "rot: after run2, log.txt must hold run2 only");
  CHECK(slurp(P1, buf, sizeof buf) > 0 && strstr(buf, "run1"), "rot: prev1 must hold run1");

  CHECK(run_once("run3") == 0, "rot: run3 flush");
  CHECK(slurp(P,  buf, sizeof buf) > 0 && strstr(buf, "run3"), "rot: log.txt=run3");
  CHECK(slurp(P1, buf, sizeof buf) > 0 && strstr(buf, "run2"), "rot: prev1=run2");
  CHECK(slurp(P2, buf, sizeof buf) > 0 && strstr(buf, "run1"), "rot: prev2=run1");

  /* Fourth boot: prev2 is OCCUPIED. FatFs f_rename refuses to replace a name, so this
   * is the step the stdio shim could never test -- rename() would have replaced it. */
  CHECK(run_once("run4") == 0, "rot: run4 flush");
  CHECK(slurp(P,  buf, sizeof buf) > 0 && strstr(buf, "run4"), "rot: log.txt=run4");
  CHECK(slurp(P1, buf, sizeof buf) > 0 && strstr(buf, "run3"), "rot: prev1=run3");
  CHECK(slurp(P2, buf, sizeof buf) > 0 && strstr(buf, "run2"), "rot: prev2=run2 (run1 dropped)");
  CHECK(occurrences(buf, "=== run") == 1, "rot: prev2 must not have accumulated two runs");
}

/* A rotation slot occupied by something f_unlink cannot remove (a non-empty FOLDER
 * called log.prev2.txt -- a user can make one, and a corrupt directory entry can look
 * like one). Old behaviour: rename(log.prev1)->prev2 fails FR_EXIST, prev1 stays, then
 * rename(log.txt)->prev1 fails FR_EXIST too, so rotation froze FOREVER and every later
 * run appended into the previous run's file until it hit the size cap and went quiet.
 * Now the collision is resolved one generation down. */
static void t_rotation_collision(void) {
  char buf[4096];
  FIL f; UINT bw;
  fresh_card(4096, 1);
  CHECK(f_mkdir(P2) == FR_OK, "collide: mkdir prev2 dir");
  CHECK(f_open(&f, "/PokeDNA/log.prev2.txt/keep.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK,
        "collide: fill prev2 dir");
  f_write(&f, "x", 1, &bw); f_close(&f);
  CHECK(f_unlink(P2) != FR_OK, "collide: prev2 dir must be undeletable for this test");

  CHECK(run_once("run1") == 0, "collide: run1");
  CHECK(run_once("run2") == 0, "collide: run2");
  CHECK(log_rotation() == LOG_ROT_DONE, "collide: run2 must still rotate log.txt (%d)",
        log_rotation());
  CHECK(slurp(P, buf, sizeof buf) > 0 && strstr(buf, "run2") && !strstr(buf, "run1"),
        "collide: run2's log.txt must not contain run1");
  CHECK(slurp(P1, buf, sizeof buf) > 0 && strstr(buf, "run1"), "collide: prev1=run1");

  CHECK(run_once("run3") == 0, "collide: run3");
  CHECK(slurp(P, buf, sizeof buf) > 0 && strstr(buf, "run3") && !strstr(buf, "run2"),
        "collide: run3's log.txt must not contain run2");
  CHECK(slurp(P1, buf, sizeof buf) > 0 && strstr(buf, "run2") && !strstr(buf, "run1"),
        "collide: prev1 rotated to run2 despite the blocked prev2 slot");
}

/* No /PokeDNA folder at all (fresh card, or the user deleted it). Every flush used to
 * return FR_NO_PATH and after three of them logging latched off -- a whole run silent,
 * with nothing on the card to say why, because the explanation goes in the log. */
static void t_missing_appdir(void) {
  char buf[4096];
  fresh_card(4096, 0);
  CHECK(run_once("run1") == 0, "no-dir: flush should create /PokeDNA and succeed");
  CHECK(slurp(P, buf, sizeof buf) > 0 && strstr(buf, "run1"), "no-dir: log.txt missing");
  CHECK(log_health() == LOG_HEALTH_OK, "no-dir: health %d", log_health());
}

/* A write-protected volume: f_open(FA_WRITE) is refused before anything is written. The
 * log cannot report this (the report would be a write), so the HEALTH API must. */
static void t_write_protected(void) {
  int r;
  fresh_card(4096, 1);
  CHECK(run_once("run1") == 0, "wp: setup flush");
  rd_protect = 1;
  log_line("after the card went read-only");
  r = log_flush_to_sd(P);
  CHECK(r == FR_WRITE_PROTECTED, "wp: flush returned %d, want %d", r, FR_WRITE_PROTECTED);
  CHECK(log_health() == LOG_HEALTH_FAILING, "wp: health %d after 1 failure", log_health());
  log_line("again"); log_flush_to_sd(P);
  log_line("again"); log_flush_to_sd(P);
  CHECK(log_health() == LOG_HEALTH_OFF, "wp: health %d after 3 failures", log_health());
  CHECK(log_last_result() == FR_WRITE_PROTECTED, "wp: last result %d", log_last_result());
  {
    char badge[16];
    log_health_str(badge, sizeof badge);
    CHECK(strstr(badge, "LOG OFF") != 0, "wp: badge '%s' must shout", badge);
    CHECK(strlen(badge) < 12, "wp: badge '%s' too long for the screen", badge);
  }
  /* Latched: no card traffic at all until the retry tick. */
  {
    unsigned long w = rd_writes, rr = rd_reads;
    int i;
    for (i = 0; i < 20; i++) { log_line("noise %d", i); log_flush_to_sd(P); }
    CHECK(rd_writes == w && rd_reads == rr, "wp: latched log still touched the card");
  }
  /* ...and it comes BACK if the card does. The old latch was permanent for the run. */
  rd_protect = 0;
  {
    int i, ok = -1;
    for (i = 0; i < 40 && ok != 0; i++) { log_line("recovered %d", i); ok = log_flush_to_sd(P); }
    CHECK(ok == 0, "wp: logging never recovered after the card came back");
    CHECK(log_health() == LOG_HEALTH_OK, "wp: health %d after recovery", log_health());
  }
}

/* An EverDrive (every disk_write fails) plus rotation: nothing must be lost or orphaned,
 * and log_begin_run must report LOG_ROT_REFUSED -- the earliest warning available, since
 * it happens before the first flush. */
static void t_rotation_refused(void) {
  char buf[4096];
  fresh_card(4096, 1);
  CHECK(run_once("run1") == 0, "refused: run1");
  rd_fail_all_writes = 1;
  log_clear();
  log_begin_run(P);
  CHECK(log_rotation() == LOG_ROT_REFUSED, "refused: rotation state %d", log_rotation());
  log_line("=== run2 ===");
  CHECK(log_flush_to_sd(P) != 0, "refused: flush should have failed");
  rd_fail_all_writes = 0;
  /* run1's bytes must still exist SOMEWHERE. Note what real FatFs does here and the
   * stdio shim cannot: f_rename reported FR_DISK_ERR (the final sync failed) yet the
   * directory entry HAD already been rewritten in the cached window, so once writes heal
   * the rename takes effect after all. So "which file" is genuinely unknowable from the
   * return code -- but "not destroyed" is the invariant that matters, and it holds. */
  {
    char alt[4096];
    long a = slurp(P, buf, sizeof buf);
    long b = slurp(P1, alt, sizeof alt);
    int here  = (a > 0) && strstr(buf, "run1") != 0;
    int there = (b > 0) && strstr(alt, "run1") != 0;
    CHECK(here || there,
          "refused: run1's log was DESTROYED (this is the FA_CREATE_ALWAYS regression)");
  }
}

/* A flush that fails must NOT advance the watermark: the next successful one re-sends
 * those bytes. A duplicated line in a crash log is harmless; a missing one defeats the
 * file. With real FatFs a sub-sector append fails at f_close (f_sync), not f_write. */
static void t_failed_flush_keeps_bytes(void) {
  char buf[8192];
  fresh_card(4096, 1);
  CHECK(run_once("run1") == 0, "hole: setup");
  log_line("MUST-SURVIVE-A");
  rd_fail_all_writes = 1;
  CHECK(log_flush_to_sd(P) != 0, "hole: flush should have failed");
  rd_fail_all_writes = 0;
  log_line("MUST-SURVIVE-B");
  CHECK(log_flush_to_sd(P) == 0, "hole: recovery flush failed");
  CHECK(slurp(P, buf, sizeof buf) > 0, "hole: no file");
  CHECK(occurrences(buf, "MUST-SURVIVE-A") >= 1, "hole: line A lost");
  CHECK(occurrences(buf, "MUST-SURVIVE-B") >= 1, "hole: line B lost");
  CHECK(strstr(buf, "run1") != 0, "hole: run1 header lost");
}

/* 200 flushes: every line exactly once, in order, no drift. */
static void t_many_flushes(void) {
  char buf[16384];
  int i;
  long n;
  fresh_card(4096, 1);
  log_clear();
  log_begin_run(P);
  for (i = 0; i < 200; i++) {
    char want[32];
    log_line("tick %03d", i);
    CHECK(log_flush_to_sd(P) == 0, "drift: flush %d failed", i);
    snprintf(want, sizeof want, "tick %03d", i);
    (void)want;
  }
  n = slurp(P, buf, sizeof buf);
  CHECK(n > 0, "drift: no file");
  for (i = 0; i < 200; i++) {
    char want[32];
    snprintf(want, sizeof want, "tick %03d\n", i);
    CHECK(occurrences(buf, want) == 1, "drift: '%s' appears %d times", want,
          occurrences(buf, want));
  }
  CHECK(log_flush_count() == 200, "drift: %lu commits", log_flush_count());
  /* A flush with nothing new must cost zero card traffic. */
  {
    unsigned long w = rd_writes, rr = rd_reads;
    CHECK(log_flush_to_sd(P) == 0, "drift: no-op flush errored");
    CHECK(rd_writes == w && rd_reads == rr, "drift: no-op flush touched the card");
  }
}

/* The 8 KiB ring dropping text that never reached the card must SAY so in the file,
 * not silently splice two non-adjacent spans together. */
static void t_ring_wrap_marker(void) {
  char buf[16384];
  int i;
  fresh_card(4096, 1);
  log_clear();
  log_begin_run(P);
  for (i = 0; i < 400; i++) log_line("%03d 12345678901234567890123456789012345678901234567890", i);
  CHECK(log_flush_to_sd(P) == 0, "wrap: flush failed");
  CHECK(slurp(P, buf, sizeof buf) > 0, "wrap: no file");
  CHECK(strstr(buf, "log bytes lost before flush") != 0,
        "wrap: dropped text must be admitted in the file");
  CHECK(strstr(buf, "399 ") != 0, "wrap: the newest line must be there");
}

/* A full card. f_write returns short/denied; nothing already written may be destroyed,
 * and the health API must show the failure. */
static void t_full_card(void) {
  char buf[4096];
  int i, r = 0;
  fresh_card(140, 1);                 /* ~70 KiB volume: smallest FatFs will format */
  CHECK(run_once("run1") == 0, "full: setup flush");
  for (i = 0; i < 4000 && r == 0; i++) {
    log_line("filler %04d 123456789012345678901234567890123456789012345678901234567890", i);
    r = log_flush_to_sd(P);
  }
  CHECK(r != 0, "full: the card never filled up");
  CHECK(log_health() != LOG_HEALTH_OK, "full: health still ok after a full card");
  CHECK(slurp(P, buf, sizeof buf) > 0 && strstr(buf, "run1") != 0,
        "full: the head of the log was destroyed");
}

/* An unrecognised path: rotation is skipped and SAYS so, and logging still works. */
static void t_unsupported_path(void) {
  char buf[2048];
  fresh_card(4096, 1);
  log_clear();
  log_begin_run("/PokeDNA/log.dat");
  CHECK(log_rotation() == LOG_ROT_UNSUPPORTED, "badname: rotation state %d", log_rotation());
  log_line("still logging");
  CHECK(log_flush_to_sd("/PokeDNA/log.dat") == 0, "badname: flush failed");
  CHECK(slurp("/PokeDNA/log.dat", buf, sizeof buf) > 0 &&
        strstr(buf, "no rotation for this path") != 0,
        "badname: the file must explain why it was not rotated");

  /* A path too long for "<base>.prevN.txt" to fit in LOG_ROT_MAX (64): same deal. */
  {
    char deep[80];
    memset(deep, 'a', sizeof deep);
    memcpy(deep, "/PokeDNA/", 9);
    memcpy(deep + 60, ".txt", 5);
    log_clear();
    log_begin_run(deep);
    CHECK(log_rotation() == LOG_ROT_UNSUPPORTED, "longname: rotation state %d", log_rotation());
  }
}

/* The per-run byte budget stops a runaway logger, and -- the fix -- a big STALE file can
 * no longer silence a fresh run: with the old file-size cap, an un-rotated 192 KiB
 * log.txt made the first flush of every later run write one cap line and go quiet. */
static void t_run_budget_not_file_size(void) {
  char buf[1024];
  FIL f; UINT bw; int i; char pad[1024];
  fresh_card(4096, 1);
  memset(pad, 'x', sizeof pad);
  /* Hand-build a 300 KiB log.txt, bigger than the old 192 KiB cap. */
  CHECK(f_open(&f, P, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "budget: create big file");
  for (i = 0; i < 300; i++) f_write(&f, pad, sizeof pad, &bw);
  f_close(&f);
  /* ...and make rotation impossible, so this run must append to it. */
  rd_protect = 1;
  log_clear();
  log_begin_run(P);
  rd_protect = 0;
  log_line("=== run after a huge stale log ===");
  CHECK(log_flush_to_sd(P) == 0, "budget: a stale big file must not silence this run");
  CHECK(log_health() == LOG_HEALTH_OK, "budget: health %d", log_health());
  {
    FIL g; UINT br; long sz;
    CHECK(f_open(&g, P, FA_READ) == FR_OK, "budget: reopen");
    sz = (long)f_size(&g);
    f_lseek(&g, (FSIZE_t)(sz > 200 ? sz - 200 : 0));
    f_read(&g, buf, 199, &br); buf[br] = 0;
    f_close(&g);
    CHECK(strstr(buf, "huge stale log") != 0, "budget: this run's line never landed");
  }
}

/* THE CARD THAT SAYS YES AND KEEPS NOTHING.
 *
 * An EZ-Flash write has no retry and no read-back (flashcartio_write.c returns whatever
 * _EZFO_writeSectors said), so a card, a dying contact, or a bad SD copy that ACKs a
 * sector it never stores is invisible to every layer above it. Before the read-back, this
 * exact boot sequence produced: flush rc=0, health=LOG_HEALTH_OK, badge "log 1" painted
 * in the healthy colour, no boot dialog -- while f_stat on the same mounted volume said
 * FR_NO_FILE. The diagnostic built to answer "did the log reach the card?" answered
 * "yes" in precisely the case it exists to catch. */
static void t_card_that_lies(void) {
  char badge[16];
  FILINFO fi;

  /* (a) A card that lies from the first sector. Note rc == 0: FatFs is NOT wrong here,
   * it faithfully reports what the driver told it. Only a read can tell. */
  fresh_card(4096, 1);
  rd_lie_writes = 1;
  CHECK(run_once("run1") == 0, "lie: FatFs should report success (that IS the bug)");
  CHECK(f_stat(P, &fi) == FR_NO_FILE, "lie: the card must really be empty for this test");
  CHECK(log_health() == LOG_HEALTH_LOST, "lie: health %d, want LOST", log_health());
  CHECK(log_verified() == LOG_VERIFY_LOST, "lie: verify %d", log_verified());
  CHECK(log_card_bytes() == 0 && log_expect_bytes() > 0,
        "lie: card %lu expected %lu", log_card_bytes(), log_expect_bytes());
  log_health_str(badge, sizeof badge);
  CHECK(strcmp(badge, "LOG LOST") == 0, "lie: badge '%s' must shout", badge);
  CHECK(strlen(badge) < 12, "lie: badge '%s' too long for the screen", badge);

  /* (b) A card that STARTS lying after an honest run -- and the reason the check is
   * exact rather than a heuristic. Here the old file survives at its old size, and this
   * run's byte count happens to equal it, so "fsize < bytes this run wrote" would have
   * been satisfied and reported healthy. Holding the card to the size FatFs itself
   * reported after the write (old size + this run's bytes) catches it. */
  fresh_card(4096, 1);
  CHECK(run_once("run1") == 0, "lie2: honest setup run");
  rd_lie_writes = 1;
  CHECK(run_once("run1") == 0, "lie2: FatFs should still report success");
  CHECK(f_stat(P, &fi) == FR_OK, "lie2: the stale file should still be there");
  CHECK((unsigned long)fi.fsize >= 1,  "lie2: stale file empty?");
  CHECK(log_card_bytes() < log_expect_bytes(),
        "lie2: card %lu vs expected %lu -- a size heuristic would have passed this",
        log_card_bytes(), log_expect_bytes());
  CHECK(log_health() == LOG_HEALTH_LOST, "lie2: health %d, want LOST", log_health());

  /* (c) STICKY for the run: a card that lies once has disqualified itself, and a later
   * read-back that happens to agree must not repaint the healthy badge. */
  rd_lie_writes = 0;
  {
    int i;
    for (i = 0; i < 40; i++) { log_line("healed %d", i); log_flush_to_sd(P); }
  }
  CHECK(log_health() == LOG_HEALTH_LOST, "lie3: health %d -- LOST must stick for the run",
        log_health());
  /* ...but a NEW run starts clean, so a one-off does not brand the card forever. */
  CHECK(run_once("run2") == 0, "lie4: next run");
  CHECK(log_health() == LOG_HEALTH_OK, "lie4: health %d on a healthy card", log_health());
  CHECK(log_verified() == LOG_VERIFY_GOOD, "lie4: verify %d", log_verified());
}

/* And the read-back must not become the next silent failure. Sweep a read error across
 * EVERY step of a boot flush -- the directory walk, the FAT, the read-back itself -- and
 * assert the one invariant that matters: whenever the medium actually refused something,
 * the log never claims LOG_HEALTH_OK. A read error during the commit shows as FAILING; a
 * read error during the read-back shows as LOG_HEALTH_UNVERIF ("I cannot vouch"), which
 * is a separate state from "the card lost it" on purpose. */
static void t_readback_cannot_go_silent(void) {
  int k, saw_unverif = 0, saw_failing = 0;
  for (k = 0; k < 40; k++) {
    fresh_card(4096, 1);
    CHECK(run_once("setup") == 0, "blind: setup run (k=%d)", k);
    log_clear();
    log_begin_run(P);
    log_line("=== run under a read error at %d ===", k);
    rd_fail_reads_after = k;
    log_flush_to_sd(P);
    rd_fail_reads_after = -1;
    if (rd_read_fails == 0) continue;              /* the sweep ran past the end */
    CHECK(log_health() != LOG_HEALTH_OK,
          "blind: a refused read at k=%d still reported LOG_HEALTH_OK", k);
    if (log_health() == LOG_HEALTH_UNVERIF) {
      char badge[16];
      saw_unverif = 1;
      log_health_str(badge, sizeof badge);
      CHECK(strncmp(badge, "LOG ?", 5) == 0, "blind: badge '%s'", badge);
      CHECK(strlen(badge) < 12, "blind: badge '%s' too long", badge);
      CHECK(log_verified() == LOG_VERIFY_BLIND, "blind: verify %d", log_verified());
    } else if (log_health() == LOG_HEALTH_FAILING || log_health() == LOG_HEALTH_OFF) {
      saw_failing = 1;
    }
  }
  CHECK(saw_unverif, "blind: no sweep position ever exercised the read-back's own failure");
  CHECK(saw_failing, "blind: no sweep position ever failed the commit itself");
}

int main(void) {
  t_first_boot();
  t_rotation_depth();
  t_rotation_collision();
  t_missing_appdir();
  t_write_protected();
  t_rotation_refused();
  t_failed_flush_keeps_bytes();
  t_many_flushes();
  t_ring_wrap_marker();
  t_full_card();
  t_unsupported_path();
  t_run_budget_not_file_size();
  t_card_that_lies();
  t_readback_cannot_go_silent();
  f_mount(0, "", 0);
  rd_free();
  if (fails) { printf("host_logfat_test: %d FAILURE(S)\n", fails); return 1; }
  printf("host_logfat_test: all checks passed (real lib/fatfs over a RAM disk)\n");
  return 0;
}
