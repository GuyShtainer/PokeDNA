/* Host test for savefile.c's sf_save_rolling(), the function box_save() (pdna_bank.c)
 * calls for its backup + write (BACKLOG #150 S150-0 review F3). pdna_bank.c itself is
 * not host-linkable (it includes <tonc.h> and "ui.h"), but sf_save_rolling is a plain
 * savefile.c primitive with no such dependency, so this links and calls the REAL
 * function over the REAL lib/fatfs on a RAM disk (tests/hostfat) -- earlier revisions
 * of this test re-typed box_save's decision table by hand, which passed even with the
 * whole fix reverted (F3's finding: zero mutations bitten). `save_ok()` below is only
 * the thin SF_ERR_RENAME/SF_WHERE_TARGET wrapper box_save itself still owns (savefile.h
 * says sf_save_rolling does not decide that on its own) -- everything else under test
 * (the f_stat-absent-is-fine guard, the backup gate, the write) is sf_save_rolling's
 * own code, unmodified.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_bankbackup_test.c source/savefile.c source/log.c lib/fatfs/ff.c \
 *      lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hbb
 *   /tmp/hbb
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "savefile.h"
#include "ramdisk.h"

#define DIR_ "/PokeDNA"
#define BOX  DIR_ "/box00.box"
#define TMP  BOX ".tmp"
#define BAK  BOX ".bak"
#define BAKTMP BOX ".baktmp"
#define BOX_BYTES 256u   /* size is irrelevant to this test; a real box is 2400 B */

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];
static unsigned char s_old[BOX_BYTES], s_new[BOX_BYTES], s_rd[BOX_BYTES];

static void fill(unsigned char* p, unsigned n, unsigned seed) {
  unsigned i;
  for (i = 0; i < n; i++) p[i] = (unsigned char)((i * 31u + seed * 7u + (i >> 5)) & 0xFF);
}

static void fresh_card(unsigned sectors) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(sectors);
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  CHECK(f_mkdir(DIR_) == FR_OK, "f_mkdir " DIR_ " failed");
}

static void remount(void) {
  f_mount(0, "", 0);
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "remount failed");
}

static int write_raw(const char* path, const unsigned char* buf, unsigned n) {
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return 0;
  if (f_write(&f, buf, n, &bw) != FR_OK || bw != n) { f_close(&f); return 0; }
  return f_close(&f) == FR_OK;
}

static long slurp(const char* path, unsigned char* out, unsigned cap) {
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return -1;
  if (f_read(&f, out, cap, &br) != FR_OK) { f_close(&f); return -1; }
  f_close(&f);
  return (long)br;
}

static int holds(const char* path, const unsigned char* want, unsigned n) {
  long got = slurp(path, s_rd, BOX_BYTES);
  return got == (long)n && memcmp(s_rd, want, n) == 0;
}

static int exists(const char* path) { FILINFO fi; return f_stat(path, &fi) == FR_OK; }

/* ---- box_save's own remaining triage: what it does with sf_save_rolling's result.
 * sf_save_rolling itself is the real thing under test below, not re-typed. ---- */

static bool save_ok(const char* path, const uint8_t* buf, uint32_t len, bool* out_backed_up) {
  SfStatus st = sf_save_rolling(path, buf, len, out_backed_up);
  if (st == SF_ERR_RENAME)
    return sf_where_are_the_bytes(path, buf, len) == SF_WHERE_TARGET;
  return st == SF_OK;
}

/* ---- tests ---- */

/* Absent source (a box file exists only after its first save): the backup step must be
 * a no-op, and the FIRST write into a never-used box must still succeed. */
static void t_virgin_box_first_write(void) {
  bool backed_up = true;   /* deliberately wrong default -- sf_save_rolling must clear it */
  fresh_card(4096);
  fill(s_new, BOX_BYTES, 1);
  CHECK(!exists(BOX), "virgin: setup left a box file behind");
  CHECK(save_ok(BOX, s_new, BOX_BYTES, &backed_up), "virgin: first write into a never-used box refused");
  remount();
  CHECK(holds(BOX, s_new, BOX_BYTES), "virgin: the card does not hold the new box");
  CHECK(!exists(BAK), "virgin: a .bak appeared for a box that never existed");
  CHECK(!backed_up,
        "virgin: sf_save_rolling reported a backup for a box that never existed (review G2)");
}

/* Present source: backup is made, box.bak holds the OLD bytes, box holds the NEW ones. */
static void t_present_box_backs_up(void) {
  bool backed_up = false;
  fresh_card(4096);
  fill(s_old, BOX_BYTES, 2);
  fill(s_new, BOX_BYTES, 3);
  CHECK(write_raw(BOX, s_old, BOX_BYTES), "present: could not create the original box");
  CHECK(save_ok(BOX, s_new, BOX_BYTES, &backed_up), "present: save refused on a healthy card");
  remount();
  CHECK(holds(BOX, s_new, BOX_BYTES), "present: the card does not hold the new box");
  CHECK(holds(BAK, s_old, BOX_BYTES), "present: box.bak does not hold the pre-save bytes");
  CHECK(backed_up, "present: sf_save_rolling did not report the backup it made (review G2)");
}

/* Backup fails on a present box: box_save must refuse and the ORIGINAL box must be
 * untouched -- never a half-write on a failed backup. rd_fail_all_writes would be
 * VACUOUS here (review F2): with the backup and the main write both inside
 * sf_save_rolling now, a permanently-dead card fails BOTH of them, so the test cannot
 * tell "the backup gate refused" from "the write itself just couldn't happen either
 * way" -- it would still pass with the gate deleted. rd_fail_at fails exactly ONE
 * write (landing inside copy_file's write into box.baktmp, the very first write this
 * call makes) and then HEALS, so the subsequent main write would succeed if nothing
 * stopped it -- the only thing that CAN stop it is sf_save_rolling noticing the backup
 * failed and returning before ever opening box00.box.tmp. That makes the mutation
 * bite: delete the "backup failed -> return" check and this test starts seeing the
 * NEW bytes in box00.box instead of the old ones. */
static void t_backup_failure_refuses(void) {
  fresh_card(4096);
  fill(s_old, BOX_BYTES, 4);
  fill(s_new, BOX_BYTES, 5);
  CHECK(write_raw(BOX, s_old, BOX_BYTES), "backupfail: could not create the original box");
  rd_fail_at = 0;                         /* fail exactly the first write, then heal */
  bool ok = save_ok(BOX, s_new, BOX_BYTES, NULL);
  rd_fail_at = -1;
  CHECK(!ok, "backupfail: box_save reported success while the backup failed");
  remount();
  CHECK(holds(BOX, s_old, BOX_BYTES), "backupfail: the original box was touched despite the refusal");
  CHECK(!holds(BOX, s_new, BOX_BYTES),
        "backupfail: the NEW bytes landed in box00.box -- the backup-failed gate did not stop the write");
  CHECK(!exists(BAK), "backupfail: a .bak appeared despite the backup failing");
  CHECK(!exists(BAKTMP),
        "backupfail: a stray .baktmp was left behind -- sf_backup_rolling's own cleanup did not run");
}

/* THE review F1 scenario, reproduced directly: f_stat itself fails transiently (not
 * absent -- a card that would not even answer) on a PRESENT box. Before F1's fix this
 * read as "never written", skipped the backup, and overwrote the box unbacked; a fresh
 * remount (drops FatFs' cached directory window, as a reboot would) plus one injected
 * read fault at the very next read reliably lands inside f_stat's own directory lookup
 * (probed by hand: rd_fail_read_at = 0 right after a remount makes f_stat report
 * FR_DISK_ERR on a file that unquestionably exists). The card heals immediately after,
 * so nothing here is "unluckily still broken" -- this is exactly the one-bad-read,
 * then-fine card the guard exists for. */
static void t_stat_fault_refuses_unbacked_write(void) {
  fresh_card(4096);
  fill(s_old, BOX_BYTES, 8);
  fill(s_new, BOX_BYTES, 9);
  CHECK(write_raw(BOX, s_old, BOX_BYTES), "statfault: could not create the original box");
  remount();
  rd_fail_read_at = 0;                    /* lands inside f_stat's own lookup; heals after */
  bool ok = save_ok(BOX, s_new, BOX_BYTES, NULL);
  rd_fail_read_at = -1;
  CHECK(!ok, "statfault: box_save reported success on a card that would not even answer f_stat");
  remount();
  CHECK(holds(BOX, s_old, BOX_BYTES),
        "statfault: the original box was overwritten despite the refusal");
  CHECK(!holds(BOX, s_new, BOX_BYTES),
        "statfault: the NEW bytes landed in box00.box UNBACKED -- the f_stat fault was "
        "mistaken for \"never written\" (review F1's exact bug)");
}

/* A forced rename failure whose bytes actually landed at TARGET must read as success --
 * the box_save triage, not a raw SF_ERR_RENAME refusal. Sweep a single transient read
 * error (the read-side twin of the write sweep in host_savefat_test.c) across the write
 * that follows a healthy backup, and require the sweep to actually reach that exact
 * interleaving at least once. */
static void t_rename_target_counts_as_success(void) {
  long k;
  int saw_rename_target = 0;
  fill(s_old, BOX_BYTES, 6);
  fill(s_new, BOX_BYTES, 7);
  for (k = 0; k < 40; k++) {
    fresh_card(4096);
    CHECK(write_raw(BOX, s_old, BOX_BYTES), "renametarget k=%ld: setup", k);
    remount();
    rd_fail_read_at = k;               /* one transient read error, then a healthy card */
    SfStatus st = sf_write_verified(BOX, s_new, BOX_BYTES);
    rd_fail_read_at = -1;
    remount();
    if (st == SF_ERR_RENAME) {
      SfWhere w = sf_where_are_the_bytes(BOX, s_new, BOX_BYTES);
      if (w == SF_WHERE_TARGET) {
        saw_rename_target = 1;
        /* box_save's own triage, applied to this exact outcome: TARGET => success. */
        CHECK(holds(BOX, s_new, BOX_BYTES),
              "renametarget k=%ld: SF_WHERE_TARGET but the card disagrees", k);
      }
    }
  }
  CHECK(saw_rename_target,
        "renametarget: the sweep never reached SF_ERR_RENAME + SF_WHERE_TARGET -- "
        "the triage's main branch is untested");
}

int main(void) {
  t_virgin_box_first_write();
  t_present_box_backs_up();
  t_backup_failure_refuses();
  t_stat_fault_refuses_unbacked_write();
  t_rename_target_counts_as_success();
  printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
  return fails ? 1 : 0;
}
