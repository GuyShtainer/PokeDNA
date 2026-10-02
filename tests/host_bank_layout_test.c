/* Host test for source/bank_layout.c (BACKLOG #371): whether a Bank already exists on the
 * card (bml_layout_exists) and where its bank.meta bytes come from (bml_meta_read). The REAL
 * functions over the REAL lib/fatfs on a RAM disk.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_bank_layout_test.c source/bank_layout.c source/savefile.c source/log.c lib/fatfs/ff.c \
 *      lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hbl
 *   /tmp/hbl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "bank_layout.h"
#include "savefile.h"
#include "ramdisk.h"

#define DIR_ "/PokeDNA/bank"
#define META DIR_ "/bank.meta"
#define BAK  META ".bak"
#define TMP  META ".tmp"
#define NEED 176u
#define MAGIC "PKVBNK"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];

static void fresh_card(void) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(4096);
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  CHECK(f_mkdir("/PokeDNA") == FR_OK, "mkdir PokeDNA");
  CHECK(f_mkdir(DIR_) == FR_OK, "mkdir bank");
}

static void put(const char* path, const void* data, unsigned n) {
  FIL f; UINT bw = 0;
  CHECK(f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "open %s", path);
  CHECK(f_write(&f, data, n, &bw) == FR_OK && bw == n, "write %s", path);
  f_close(&f);
}

static void meta_bytes(uint8_t* b, uint8_t tag) {
  memset(b, 0, NEED); memcpy(b, MAGIC, 6); b[8] = 2; b[9] = 16; b[16] = tag;
}

static BmlSource rd(uint8_t* out) { return bml_meta_read(DIR_, 16, out, NEED, NEED, MAGIC); }


/* ---------------- BACKLOG #378: box files ---------------- */
#define BOXN 2400u
#define BX DIR_ "/box03.box"
static uint8_t boxA[BOXN], boxB[BOXN], boxC[BOXN], bout[BOXN];
static void box_fill(uint8_t* b, uint8_t seed) { for (unsigned i = 0; i < BOXN; i++) b[i] = (uint8_t)(seed + i * 7 + 1); }
static BmlBoxSrc brd(uint32_t* sz) { return bml_box_read(BX, bout, BOXN, sz); }
static bool exists(const char* p) { return f_stat(p, 0) == FR_OK; }
static bool file_is(const char* p, const uint8_t* b) {
  FIL f; UINT br = 0; static uint8_t t[BOXN];
  if (f_open(&f, p, FA_READ) != FR_OK) return false;
  FRESULT fr = f_read(&f, t, BOXN, &br); f_close(&f);
  return fr == FR_OK && br == BOXN && memcmp(t, b, BOXN) == 0;
}

static void box_decisions(void) {
  uint32_t sz = 0;
  box_fill(boxA, 1); box_fill(boxB, 2); box_fill(boxC, 3);
  /* genuinely new box: nothing -> NONE, zeroed buffer */
  fresh_card(); memset(bout, 0x5A, BOXN);
  CHECK(brd(&sz) == BML_BOX_NONE && sz == 0, "#378: new box not NONE");
  { unsigned nz = 0; for (unsigned i = 0; i < BOXN; i++) nz += bout[i] != 0; CHECK(nz == 0, "#378: NONE left bytes in buf"); }
  /* primary present wins over both copies */
  fresh_card(); put(BX, boxA, BOXN); put(BX ".tmp", boxB, BOXN); put(BX ".bak", boxC, BOXN);
  CHECK(brd(&sz) == BML_BOX_PRIMARY && memcmp(bout, boxA, BOXN) == 0, "#378: primary must win");
  /* primary absent: the verified .tmp (newest) beats .bak */
  fresh_card(); put(BX ".tmp", boxB, BOXN); put(BX ".bak", boxC, BOXN);
  CHECK(brd(&sz) == BML_BOX_TMP && memcmp(bout, boxB, BOXN) == 0 && sz == BOXN, "#378: .tmp not preferred");
  /* primary absent, no .tmp -> .bak */
  fresh_card(); put(BX ".bak", boxC, BOXN);
  CHECK(brd(&sz) == BML_BOX_BAK && memcmp(bout, boxC, BOXN) == 0, "#378: .bak not used");
  /* a SHORT .tmp (torn / unverified) is not data: falls to .bak */
  fresh_card(); put(BX ".tmp", boxB, 1000); put(BX ".bak", boxC, BOXN);
  CHECK(brd(&sz) == BML_BOX_BAK && memcmp(bout, boxC, BOXN) == 0, "#378: short .tmp trusted");
  /* short .tmp and no .bak -> NONE and the partial read must not leak */
  fresh_card(); put(BX ".tmp", boxB, 1000);
  CHECK(brd(&sz) == BML_BOX_NONE, "#378: short .tmp alone not NONE");
  { unsigned nz = 0; for (unsigned i = 0; i < BOXN; i++) nz += bout[i] != 0; CHECK(nz == 0, "#378: partial tmp leaked into buf"); }
  /* a short .bak is not data */
  fresh_card(); put(BX ".bak", boxC, 100);
  CHECK(brd(&sz) == BML_BOX_NONE, "#378: short .bak trusted");
  /* present-but-short primary: BAD (no silent fallback), as box_load behaved before */
  fresh_card(); put(BX, boxA, 1000); put(BX ".bak", boxC, BOXN);
  CHECK(brd(&sz) == BML_BOX_BAD && sz == 1000, "#378: short primary must be BAD");
  /* READ ERROR on a present primary is NOT absent: no fallback to the older .bak */
  fresh_card(); put(BX, boxA, BOXN); put(BX ".bak", boxC, BOXN);
  rd_fail_reads_after = 0;
  CHECK(brd(&sz) == BML_BOX_READ_ERROR, "#378: read error on primary treated as absent/ok");
  rd_fail_reads_after = -1;
  /* ONE failed read at every step of the probe: a present primary is never mistaken for an absent one,
   * so the older .tmp/.bak copies are never returned in its place */
  { int saw_err = 0;
    for (long n = 0; n < 8; n++) {
      fresh_card(); put(BX, boxA, BOXN); put(BX ".tmp", boxB, BOXN); put(BX ".bak", boxC, BOXN);
      f_mount(0, "", 0); f_mount(&s_fs, "", 1);
      rd_fail_read_at = n;
      BmlBoxSrc r = brd(&sz);
      rd_fail_read_at = -1;
      CHECK(r == BML_BOX_PRIMARY || r == BML_BOX_READ_ERROR, "#378: read fail at %ld returned fallback r=%d", n, (int)r);
      if (r == BML_BOX_READ_ERROR) saw_err++;
    }
    CHECK(saw_err > 0, "#378: no read fault ever reached the probe (vacuous)"); }
  /* primary absent, .tmp present but unreadable: READ_ERROR, never fall back past it to the older .bak */
  fresh_card(); put(BX ".tmp", boxB, BOXN); put(BX ".bak", boxC, BOXN);
  f_mount(0, "", 0); f_mount(&s_fs, "", 1);          /* drop the cached sectors so reads hit the disk */
  rd_fail_reads_after = 0;
  { BmlBoxSrc r = brd(&sz); CHECK(r == BML_BOX_READ_ERROR, "#378: unreadable .tmp fell past to .bak (r=%d)", (int)r); }
  rd_fail_reads_after = -1;
  /* argument validation */
  CHECK(bml_box_read(NULL, bout, BOXN, &sz) == BML_BOX_READ_ERROR, "#378: NULL path");
  CHECK(!bml_box_heal(BX, BML_BOX_PRIMARY, boxA, BOXN), "#378: heal accepted PRIMARY");
  CHECK(!bml_box_heal(BX, BML_BOX_NONE, boxA, BOXN), "#378: heal accepted NONE");
}

static void box_heal_cases(void) {
  uint32_t sz = 0;
  /* TMP heal: rename in place, .tmp consumed, .bak untouched */
  fresh_card(); put(BX ".tmp", boxB, BOXN); put(BX ".bak", boxC, BOXN);
  CHECK(brd(&sz) == BML_BOX_TMP, "heal pre: not TMP");
  CHECK(bml_box_heal(BX, BML_BOX_TMP, bout, BOXN), "#378: TMP heal failed");
  CHECK(file_is(BX, boxB) && !exists(BX ".tmp") && file_is(BX ".bak", boxC), "#378: TMP heal result wrong");
  /* BAK heal: primary created from .bak, .bak byte-identical, still there */
  fresh_card(); put(BX ".bak", boxC, BOXN);
  CHECK(brd(&sz) == BML_BOX_BAK, "heal pre: not BAK");
  CHECK(bml_box_heal(BX, BML_BOX_BAK, bout, BOXN), "#378: BAK heal failed");
  CHECK(file_is(BX, boxC) && file_is(BX ".bak", boxC), "#378: BAK heal result wrong");
  /* a failed heal leaves the copies in place (retry next open) */
  fresh_card(); put(BX ".tmp", boxB, BOXN); put(BX ".bak", boxC, BOXN);
  CHECK(brd(&sz) == BML_BOX_TMP, "heal fail pre");
  rd_fail_all_writes = 1;
  bool hr = bml_box_heal(BX, BML_BOX_TMP, bout, BOXN);
  rd_fail_all_writes = 0;
  (void)hr;                                            /* result depends on whether the dir write is flushed */
  { BmlBoxSrc r = brd(&sz); CHECK(r == BML_BOX_PRIMARY || r == BML_BOX_TMP, "#378: failed heal lost the box (r=%d)", (int)r);
    CHECK(memcmp(bout, boxB, BOXN) == 0, "#378: failed heal changed the recovered bytes"); }
}

/* The point of the lane: sweep a box save (second save of a box = backup + verified write) over
 * every k, fail exactly ONE disk write, then a cold boot (classify + heal) must show ALL the new
 * or ALL the old mons, never an empty/short box, and two ordinary edits afterwards keep it. */
static int sweep_rows, sweep_old_prim_only_bad;
static void cold_boot(void) { f_mount(0, "", 0); CHECK(f_mount(&s_fs, "", 1) == FR_OK, "remount"); }

static void sweep_one(int mode, long k) {
  uint32_t sz = 0; bool backed;
  fresh_card();
  CHECK(sf_save_rolling(BX, boxA, BOXN, &backed) == SF_OK, "sweep seed save A");
  CHECK(sf_save_rolling(BX, boxB, BOXN, &backed) == SF_OK, "sweep seed save B");   /* primary B, .bak A */
  if (mode == 0) rd_fail_at = k; else rd_lie_after = k;      /* mode 0: one failed write; 1: the card lies from write k on */
  (void)sf_save_rolling(BX, boxC, BOXN, &backed);            /* the faulted C save */
  rd_fail_at = -1; rd_lie_after = -1; rd_lie_writes = 0;
  cold_boot();                                               /* power cycle: only what reached the disk */
  sweep_rows++;
  if (f_stat(BX, 0) != FR_OK) sweep_old_prim_only_bad++;     /* the PRE-FIX primary-only read shows an empty box */
  BmlBoxSrc r = brd(&sz);
  bool known = memcmp(bout, boxA, BOXN) == 0 || memcmp(bout, boxB, BOXN) == 0 || memcmp(bout, boxC, BOXN) == 0;
  bool good = (r == BML_BOX_PRIMARY || r == BML_BOX_TMP || r == BML_BOX_BAK) && sz == BOXN && known;
  CHECK(good, "#378 sweep mode %d k=%ld: cold boot shows no box (r=%d sz=%u)", mode, k, (int)r, (unsigned)sz);
  if (!good) return;
  if (r != BML_BOX_PRIMARY)
    CHECK(bml_box_heal(BX, r, bout, BOXN), "#378 sweep mode %d k=%ld: heal failed", mode, k);
  uint8_t rec[BOXN]; memcpy(rec, bout, BOXN);
  uint8_t d[BOXN]; box_fill(d, 9);
  CHECK(sf_save_rolling(BX, d, BOXN, &backed) == SF_OK, "sweep k=%ld: edit D", k);
  CHECK(file_is(BX ".bak", rec), "#378 sweep mode %d k=%ld: after edit D the .bak is not the recovered box", mode, k);
  CHECK(file_is(BX, d), "sweep k=%ld: D not in place", k);
  /* heal invariant: a recovered box reads back as the SAME bytes on the next cold boot */
  cold_boot();
  CHECK(brd(&sz) == BML_BOX_PRIMARY && memcmp(bout, d, BOXN) == 0, "sweep k=%ld: second boot", k);
}

static void box_save_sweep(void) {
  for (int mode = 0; mode < 2; mode++)
    for (long k = 0; k < 120; k++) sweep_one(mode, k);
  CHECK(sweep_old_prim_only_bad > 0, "#378 sweep never produced an absent-primary window (test is vacuous)");
  printf("sweep: %d rows, pre-fix primary-only read lost the box in %d\n", sweep_rows, sweep_old_prim_only_bad);
}

int main(void) {
  uint8_t good[NEED], old[NEED], out[NEED], junk[NEED];
  meta_bytes(good, 0x11); meta_bytes(old, 0x22);
  memset(junk, 0xA5, sizeof junk);

  /* nothing at all: a card with no Bank -> "first run" (migrate may run) */
  fresh_card();
  CHECK(!bml_layout_exists(DIR_, 16), "empty card reported as an existing Bank");
  CHECK(rd(out) == BML_NONE_EMPTY, "empty card: expected NONE_EMPTY");

  /* a missing bank dir entirely is still "no Bank", not a crash */
  CHECK(!bml_layout_exists("/nonexistent", 16), "missing dir reported as a Bank");

  /* primary only */
  fresh_card(); put(META, good, NEED);
  CHECK(bml_layout_exists(DIR_, 16), "primary only: not an existing Bank");
  CHECK(rd(out) == BML_PRIMARY && memcmp(out, good, NEED) == 0, "primary only: wrong source/bytes");

  /* THE #371 CASE: primary absent, good .bak -> existing Bank, restore from .bak */
  fresh_card(); put(BAK, old, NEED);
  CHECK(bml_layout_exists(DIR_, 16), "#371: missing primary + good .bak treated as first run");
  CHECK(rd(out) == BML_BAK_PRIMARY_ABSENT, "#371: expected BAK_PRIMARY_ABSENT");
  CHECK(memcmp(out, old, NEED) == 0, "#371: restored bytes are not the .bak bytes");

  /* corrupt primary + good .bak -> BAK_PRIMARY_BAD (the pre-existing #219a path) */
  fresh_card(); put(META, junk, NEED); put(BAK, old, NEED);
  CHECK(rd(out) == BML_BAK_PRIMARY_BAD && memcmp(out, old, NEED) == 0, "corrupt primary: wrong source");
  /* a short primary is corrupt too */
  fresh_card(); put(META, good, 100); put(BAK, old, NEED);
  CHECK(rd(out) == BML_BAK_PRIMARY_BAD, "short primary: wrong source");

  /* both good: primary wins */
  fresh_card(); put(META, good, NEED); put(BAK, old, NEED);
  CHECK(rd(out) == BML_PRIMARY && memcmp(out, good, NEED) == 0, "both good: primary must win");

  /* neither meta file, but a box file: an existing Bank, defaults in RAM, NOT first run */
  fresh_card(); put(DIR_ "/box07.box", junk, 64);
  CHECK(bml_layout_exists(DIR_, 16), "#371: boxes without any meta treated as first run");
  CHECK(rd(out) == BML_NONE_BOXES, "#371: expected NONE_BOXES");
  /* box15 is the last scanned; box16 is out of range */
  fresh_card(); put(DIR_ "/box15.box", junk, 64);
  CHECK(bml_layout_exists(DIR_, 16), "box15 not scanned");
  fresh_card(); put(DIR_ "/box16.box", junk, 64);
  CHECK(!bml_layout_exists(DIR_, 16), "box16 (out of range) counted");

  /* #379 F3: a verified bank.meta.tmp / .baktmp is the LAST resort: used only when no .bak parses */
  fresh_card(); put(TMP, good, NEED);
  CHECK(!bml_layout_exists(DIR_, 16), "lone bank.meta.tmp counted as a Bank");
  CHECK(rd(out) == BML_LAST_RESORT_ABSENT && memcmp(out, good, NEED) == 0, "#379: lone .tmp not used as the last resort");
  fresh_card(); put(TMP, good, NEED); put(BAK, old, NEED);
  CHECK(rd(out) == BML_BAK_PRIMARY_ABSENT && memcmp(out, old, NEED) == 0, "#379: .bak must be preferred over .tmp");
  fresh_card(); put(META ".baktmp", old, NEED);
  CHECK(rd(out) == BML_LAST_RESORT_ABSENT && memcmp(out, old, NEED) == 0, "#379: lone .baktmp not used");
  fresh_card(); put(TMP, good, NEED); put(META ".baktmp", old, NEED);
  CHECK(rd(out) == BML_LAST_RESORT_ABSENT && memcmp(out, good, NEED) == 0, "#379: .tmp must be preferred over .baktmp");
  fresh_card(); put(TMP, junk, NEED); put(META ".baktmp", old, NEED);
  CHECK(rd(out) == BML_LAST_RESORT_ABSENT && memcmp(out, old, NEED) == 0, "#379: bad-magic .tmp must fall to .baktmp");
  fresh_card(); put(TMP, good, 100);
  CHECK(rd(out) == BML_NONE_EMPTY, "#379: a short (torn) .tmp was trusted");
  fresh_card(); put(META, junk, NEED); put(TMP, good, NEED);
  CHECK(rd(out) == BML_LAST_RESORT_BAD && memcmp(out, good, NEED) == 0, "#379: corrupt primary + only .tmp");
  fresh_card(); put(TMP, good, NEED); put(DIR_ "/box07.box", junk, 64);
  CHECK(rd(out) == BML_LAST_RESORT_ABSENT, "#379: .tmp must beat NONE_BOXES");

  /* #379 F4: a card fault is not "missing" */
  fresh_card(); put(META, good, NEED); put(BAK, old, NEED);
  f_mount(0, "", 0); f_mount(&s_fs, "", 1);
  rd_fail_reads_after = 0;
  { BmlSource r = rd(out); CHECK(r == BML_READ_ERROR, "#379: stat failure on the primary returned %d", (int)r); }
  CHECK(bml_layout_exists(DIR_, 16), "#379: a stat fault read as 'no Bank' (first-run path would write defaults)");
  rd_fail_reads_after = -1;
  { int errs = 0, fallbacks = 0;
    for (long n = 0; n < 8; n++) {
      fresh_card(); put(META, good, NEED); put(BAK, old, NEED);
      f_mount(0, "", 0); f_mount(&s_fs, "", 1);
      rd_fail_read_at = n;
      BmlSource r = rd(out);
      rd_fail_read_at = -1;
      if (r == BML_READ_ERROR) errs++;
      if (r != BML_PRIMARY && r != BML_READ_ERROR) { fallbacks++; printf("  read fault %ld -> source %d\n", n, (int)r); }
    }
    CHECK(fallbacks == 0, "#379: a transient read fault on a PRESENT primary fell back to .bak/NONE (%d)", fallbacks);
    CHECK(errs > 0, "#379: no read fault ever reached bml_meta_read (vacuous)"); }
  /* primary absent, .bak present but its read errors -> READ_ERROR, not .tmp/NONE */
  { int errs = 0, bad = 0;
    for (long n = 0; n < 8; n++) {
      fresh_card(); put(BAK, old, NEED); put(TMP, good, NEED);
      f_mount(0, "", 0); f_mount(&s_fs, "", 1);
      rd_fail_read_at = n;
      BmlSource r = rd(out);
      rd_fail_read_at = -1;
      if (r == BML_READ_ERROR) errs++;
      if (r == BML_LAST_RESORT_ABSENT || r == BML_NONE_BOXES || r == BML_NONE_EMPTY) bad++;
    }
    CHECK(bad == 0, "#379: a read fault on .bak fell through to .tmp/NONE (%d)", bad);
    CHECK(errs > 0, "#379: .bak read fault never reached (vacuous)"); }

  /* a box .bak / other stray files alone do not count */
  fresh_card(); put(DIR_ "/box00.box.bak", junk, 64);
  CHECK(!bml_layout_exists(DIR_, 16), "box00.box.bak alone counted as a Bank");

  /* argument validation */
  CHECK(!bml_layout_exists(NULL, 16), "NULL dir");
  CHECK(bml_meta_read(DIR_, 16, out, 10, NEED, MAGIC) == BML_NONE_EMPTY, "cap < need accepted");

  box_decisions(); box_heal_cases(); box_save_sweep();

  if (fails) { printf("host_bank_layout_test: %d FAILED\n", fails); return 1; }
  printf("host_bank_layout_test: all passed\n");
  return 0;
}
