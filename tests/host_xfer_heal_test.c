/* Host test for source/xfer_heal.c (BACKLOG #389): an orphan <key>.pds.tmp (the verified bytes of an
 * interrupted sf_write_verified swap, primary gone) is healed by RENAME before any screen reads the
 * ledger. The REAL xfer_heal.c over the REAL lib/fatfs on a RAM disk, plus a fail_at / lie_after sweep
 * of the real sf_write_verified (the primitive under promote, re-key, Mark finished, restore, migrate).
 *
 *   cc -std=c11 -Wall -Wextra -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf \
 *      -I tests/hostfat -I lib/fatfs -I source tests/host_xfer_heal_test.c source/xfer_heal.c \
 *      source/gb_sidecar.c source/xfer_rec.c source/bank_cell.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c source/gen3_edit.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_save.c source/gen3_daycare.c source/data_tables.c \
 *      source/item_map_g2g3.c source/savefile.c source/log.c lib/fatfs/ff.c lib/fatfs/ffunicode.c \
 *      tests/hostfat/ramdisk.c -o /tmp/hxh && /tmp/hxh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "gb_sidecar.h"
#include "xfer_rec.h"
#include "savefile.h"
#include "xfer_heal.h"
#include "ramdisk.h"

#define XD "/PokeDNA/xfer"
#define KEY_A 0x0019A3F17C0B44E2ULL
#define KEY_B 0x00000000000000B2ULL
#define PA XD "/0019A3F17C0B44E2.pds"
#define PB XD "/00000000000000B2.pds"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];
static uint8_t scr[GBSC_FILE_MAX];

static void fresh_card(void) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(4096);
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs");
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount");
  CHECK(f_mkdir("/PokeDNA") == FR_OK, "mkdir PokeDNA");
  CHECK(f_mkdir(XD) == FR_OK, "mkdir xfer");
}
static void cold_boot(void) { f_mount(0, "", 0); CHECK(f_mount(&s_fs, "", 1) == FR_OK, "remount"); }

static void put(const char* path, const void* data, unsigned n) {
  FIL f; UINT bw = 0;
  CHECK(f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "open %s", path);
  if (n) CHECK(f_write(&f, data, n, &bw) == FR_OK && bw == n, "write %s", path);
  f_close(&f);
}
static bool exists(const char* p) { return f_stat(p, 0) == FR_OK; }
static long slurp(const char* p, uint8_t* out, uint32_t cap) {
  FIL f; UINT br = 0;
  if (f_open(&f, p, FA_READ) != FR_OK) return -1;
  FRESULT fr = f_read(&f, out, cap, &br); f_close(&f);
  return fr == FR_OK ? (long)br : -1;
}
static bool file_is(const char* p, const uint8_t* b, uint32_t n) {
  static uint8_t t[GBSC_FILE_MAX + 8];
  return slurp(p, t, sizeof t) == (long)n && memcmp(t, b, n) == 0;
}

/* a valid ledger: header(key) + `n` entries distinguished by `tag` */
static uint32_t build(uint8_t* buf, uint64_t key, int n, uint8_t tag) {
  uint32_t len = (uint32_t)gbsc_init(buf, key);
  for (int i = 0; i < n; i++) {
    GbscEntry e; memset(&e, 0, sizeof e);
    e.gen = GB_GEN2; e.otid16 = (uint16_t)(0x1234 + tag); e.dv4[0] = tag; e.dv4[1] = (uint8_t)i;
    e.kind = XR_KIND_NATIVE_HOME; e.state = XR_STATE_PENDING; e.direction = XR_DIR_ABROAD_G3;
    CHECK(gbsc_add(buf, &len, GBSC_FILE_MAX, &e) == i, "gbsc_add");
  }
  return len;
}

static XhResult heal(uint64_t k, bool edit) { return xh_heal_key(XD, k, scr, sizeof scr, edit); }

static void decisions(void) {
  uint8_t v[GBSC_FILE_MAX], w[GBSC_FILE_MAX], bad[GBSC_FILE_MAX];
  uint32_t n = build(v, KEY_A, 2, 1), m = build(w, KEY_A, 1, 2);

  /* THE #389 CASE: primary absent, verified .tmp -> renamed into place, .tmp consumed */
  fresh_card(); put(PA ".tmp", v, n);
  CHECK(heal(KEY_A, true) == XH_RESTORED, "orphan tmp not restored");
  CHECK(file_is(PA, v, n) && !exists(PA ".tmp"), "restored bytes/rename wrong");
  CHECK(heal(KEY_A, true) == XH_NONE, "second heal not idempotent");

  /* primary present: a stale .tmp is NEVER touched or preferred */
  fresh_card(); put(PA, w, m); put(PA ".tmp", v, n);
  CHECK(heal(KEY_A, true) == XH_NONE && file_is(PA, w, m) && file_is(PA ".tmp", v, n), "primary+tmp: touched");

  /* a .tmp that does not parse is never trusted: flipped entry byte, flipped header crc, short, long tail */
  memcpy(bad, v, n); bad[GBSC_HEADER + 40] ^= 0x55;
  fresh_card(); put(PA ".tmp", bad, n);
  CHECK(heal(KEY_A, true) == XH_BAD_TMP && !exists(PA), "corrupt entry crc trusted");
  memcpy(bad, v, n); bad[16] ^= 1;
  fresh_card(); put(PA ".tmp", bad, n);
  CHECK(heal(KEY_A, true) == XH_BAD_TMP && !exists(PA), "corrupt header crc trusted");
  fresh_card(); put(PA ".tmp", v, n - 7);
  CHECK(heal(KEY_A, true) == XH_BAD_TMP && !exists(PA), "short tmp trusted");
  fresh_card(); put(PA ".tmp", v, 0);
  CHECK(heal(KEY_A, true) == XH_BAD_TMP && !exists(PA), "empty tmp trusted");
  { uint8_t big[GBSC_FILE_MAX + 20]; memset(big, 0, sizeof big); memcpy(big, v, n);
    fresh_card(); put(PA ".tmp", big, n + 20);
    CHECK(heal(KEY_A, true) == XH_BAD_TMP && !exists(PA), "tmp with a tail trusted"); }
  /* a valid ledger whose embedded key is NOT its file name */
  { uint8_t o[GBSC_FILE_MAX]; uint32_t on = build(o, KEY_B, 1, 3);
    fresh_card(); put(PA ".tmp", o, on);
    CHECK(heal(KEY_A, true) == XH_BAD_TMP && !exists(PA), "key != name trusted"); }

  /* read-only card: recognised, never written */
  fresh_card(); put(PA ".tmp", v, n);
  CHECK(heal(KEY_A, false) == XH_RO_TMP && !exists(PA) && file_is(PA ".tmp", v, n), "read-only wrote");

  /* nothing at all / bad args */
  fresh_card();
  CHECK(heal(KEY_A, true) == XH_NONE, "empty dir not NONE");
  CHECK(xh_heal_key(NULL, KEY_A, scr, sizeof scr, true) == XH_FAILED, "NULL dir accepted");
  CHECK(xh_heal_key(XD, KEY_A, scr, 100, true) == XH_FAILED, "small scratch accepted");

  /* a card fault is not "absent": nothing decided, nothing destroyed */
  fresh_card(); put(PA ".tmp", v, n); cold_boot();
  rd_fail_reads_after = 0;
  { XhResult r = heal(KEY_A, true); CHECK(r == XH_FAILED, "stat fault returned %d", (int)r); }
  rd_fail_reads_after = -1;
  CHECK(file_is(PA ".tmp", v, n) && !exists(PA), "fault run destroyed the tmp");

  /* directory scan: two orphans heal, a stale one with a primary and junk names are left alone */
  fresh_card();
  { uint8_t b[GBSC_FILE_MAX]; uint32_t bn = build(b, KEY_B, 1, 4);
    put(PA ".tmp", v, n); put(PB ".tmp", b, bn);
    put(XD "/0019A3F17C0B44E3.pds", w, m); put(XD "/0019A3F17C0B44E3.pds.tmp", v, n);
    put(XD "/MIGRATED", "0", 1); put(XD "/zzzz.pds.tmp", v, n); put(XD "/0019A3F17C0B44E2.pds.bak", v, n);
    int ro = 7;
    CHECK(xh_heal_dir(XD, scr, sizeof scr, true, &ro) == 2 && ro == 0, "dir heal count");
    CHECK(file_is(PA, v, n) && file_is(PB, b, bn), "dir heal bytes");
    CHECK(file_is(XD "/0019A3F17C0B44E3.pds.tmp", v, n) && exists(XD "/zzzz.pds.tmp"), "dir heal touched a non-orphan");
    CHECK(xh_heal_dir(XD, scr, sizeof scr, true, NULL) == 0, "dir heal not idempotent");
    fresh_card(); put(PA ".tmp", v, n); put(PB ".tmp", b, bn);
    CHECK(xh_heal_dir(XD, scr, sizeof scr, false, &ro) == 0 && ro == 2 && !exists(PA) && !exists(PB), "dir RO scan wrote/miscounted"); }
  CHECK(xh_heal_dir("/nonexistent", scr, sizeof scr, true, NULL) == 0, "missing dir not 0");
  /* more than the per-call candidate cap: the rest heal on the next call */
  fresh_card();
  { int total = 20, got = 0;
    for (int i = 0; i < total; i++) {
      uint8_t b[GBSC_FILE_MAX]; uint64_t k = 0x100ULL + (uint64_t)i; uint32_t bn = build(b, k, 1, 5);
      char p[64]; char hex[17]; gbsc_key_hex(k, hex); snprintf(p, sizeof p, XD "/%s.pds.tmp", hex); put(p, b, bn);
    }
    for (int pass = 0; pass < 4 && got < total; pass++) got += xh_heal_dir(XD, scr, sizeof scr, true, NULL);
    CHECK(got == total, "bounded heal never converged (%d of %d)", got, total); }
}

/* THE SWEEP. The real sf_write_verified rewrites a ledger (the promote / Mark finished / re-key /
 * restore shape: read, change, rewrite) with ONE failed write, or a card that lies from write k on; a
 * cold boot follows. After xh_heal_dir the ledger MUST be present and equal v1 or v2, never absent,
 * never partial. `pre_absent` counts the rows where the PRE-FIX tree (no heal) found no primary: the
 * sweep is vacuous unless that is > 0. */
static int sweep_rows, pre_absent;
static void sweep_one(int mode, long k, int create) {
  uint8_t v1[GBSC_FILE_MAX], v2[GBSC_FILE_MAX];
  uint32_t n1 = build(v1, KEY_A, 1, 1), n2 = build(v2, KEY_A, 2, 2);
  fresh_card();
  if (!create) CHECK(sf_write_verified(PA, v1, n1) == SF_OK, "seed");
  if (mode == 0) rd_fail_at = k; else rd_lie_after = k;
  (void)sf_write_verified(PA, create ? v1 : v2, create ? n1 : n2);
  rd_fail_at = -1; rd_lie_after = -1; rd_lie_writes = 0;
  cold_boot();
  sweep_rows++;
  bool had = exists(PA);
  if (!had && !create) pre_absent++;
  (void)xh_heal_dir(XD, scr, sizeof scr, true, NULL);
  if (create && !exists(PA)) return;                       /* a brand-new record that never completed: nothing to lose */
  long ln = slurp(PA, scr, sizeof scr);
  bool is1 = ln == (long)n1 && memcmp(scr, v1, n1) == 0;
  bool is2 = !create && ln == (long)n2 && memcmp(scr, v2, n2) == 0;
  bool ok = ln > 0 && gbsc_count(scr, (uint32_t)ln) >= 0 && gbsc_file_key(scr, (uint32_t)ln) == KEY_A && (is1 || is2);
  CHECK(ok, "sweep mode %d k=%ld create=%d: record absent or partial after cold boot+heal (ln=%ld had=%d)", mode, k, create, ln, (int)had);
  /* healed means healed: the next boot sees the same primary, and an ordinary rewrite works */
  cold_boot();
  CHECK(exists(PA), "sweep k=%ld: primary absent on the second boot", k);
  CHECK(sf_write_verified(PA, v2, n2) == SF_OK && file_is(PA, v2, n2), "sweep k=%ld: rewrite after heal", k);
}
static void sweep(void) {
  for (int create = 0; create < 2; create++)
    for (int mode = 0; mode < 2; mode++)
      for (long k = 0; k < 80; k++) sweep_one(mode, k, create);
  CHECK(pre_absent > 0, "sweep never produced an absent-primary window (test is vacuous)");
  printf("sweep: %d rows, pre-fix tree lost the primary in %d\n", sweep_rows, pre_absent);
}

/* #389 review D2: resurrect-after-delete. A lying rewrite leaves primary + verified .tmp both present; the
 * user later deletes / consumes the record. Plain f_unlink(primary) lets the next boot's heal bring it back;
 * xh_unlink_ledger (stale .tmp first) must not. Both arms run the same 160 rows; the plain arm MUST
 * resurrect somewhere (else the test is vacuous). */
static void resurrect(void) {
  uint8_t v1[GBSC_FILE_MAX], v2[GBSC_FILE_MAX];
  uint32_t n1 = build(v1, KEY_A, 1, 1), n2 = build(v2, KEY_A, 1, 2);
  int both = 0, plain_rez = 0, fixed_rez = 0;
  for (int arm = 0; arm < 2; arm++)
    for (int mode = 0; mode < 2; mode++)
      for (long k = 0; k < 80; k++) {
        fresh_card();
        CHECK(sf_write_verified(PA, v1, n1) == SF_OK, "rez seed");
        if (mode == 0) rd_fail_at = k; else rd_lie_after = k;
        (void)sf_write_verified(PA, v2, n2);
        rd_fail_at = -1; rd_lie_after = -1; rd_lie_writes = 0;
        cold_boot();
        if (!(exists(PA) && exists(PA ".tmp"))) continue;
        if (arm == 0) both++;
        if (arm == 0) CHECK(f_unlink(PA) == FR_OK, "plain consume unlink");
        else CHECK(xh_unlink_ledger(PA) == FR_OK && !exists(PA) && !exists(PA ".tmp"), "xh_unlink_ledger left a file");
        cold_boot();
        (void)xh_heal_dir(XD, scr, sizeof scr, true, NULL);
        if (exists(PA)) { if (arm == 0) plain_rez++; else { fixed_rez++; CHECK(0, "RESURRECTED after xh_unlink_ledger: mode %d k=%ld", mode, k); } }
      }
  CHECK(both > 0 && plain_rez > 0, "resurrect test vacuous (both=%d plain_rez=%d)", both, plain_rez);
  fresh_card();
  CHECK(xh_unlink_ledger(PA) == FR_NO_FILE, "absent ledger not FR_NO_FILE");
  { char longp[96]; memset(longp, 'a', sizeof longp - 1); longp[sizeof longp - 1] = 0;
    CHECK(xh_unlink_ledger(longp) == FR_INVALID_NAME, "overlong path accepted"); }
  printf("resurrect: %d both-present states, plain unlink resurrected %d, xh_unlink_ledger %d\n", both, plain_rez, fixed_rez);
}

/* za2 F5: the ORDER of xh_unlink_ledger (D2). Staged primary + stale .tmp; power is cut after c sectors of the
 * delete; whatever state survives, the next boot's heal must not bring back a primary that was gone before it. */
static void unlink_order(void) {
  uint8_t v1[GBSC_FILE_MAX], v2[GBSC_FILE_MAX];
  uint32_t n1 = build(v1, KEY_A, 1, 1), n2 = build(v2, KEY_A, 1, 2);
  int staged = 0, cuts = 0, gone = 0;
  for (long k = 0; k < 80; k++) {
    fresh_card();
    CHECK(sf_write_verified(PA, v1, n1) == SF_OK, "order seed");
    rd_lie_after = k; (void)sf_write_verified(PA, v2, n2); rd_lie_after = -1; rd_lie_writes = 0;
    cold_boot();
    if (!(exists(PA) && exists(PA ".tmp"))) continue;
    staged++;
    for (long c = 0; c <= 11; c++) {
      rd_snapshot(); rd_cut_sectors = c; (void)xh_unlink_ledger(PA); rd_cut_sectors = -1; rd_cut_fired = 0;
      cold_boot();
      bool pre = exists(PA);
      (void)xh_heal_dir(XD, scr, sizeof scr, true, NULL);
      cuts++; if (!pre) gone++;
      CHECK(!(!pre && exists(PA)), "unlink order: primary resurrected after a cut at %ld sectors (k=%ld)", c, k);
      rd_restore();
    }
  }
  CHECK(staged > 0 && gone > 0, "unlink-order test vacuous (staged=%d gone=%d)", staged, gone);
  printf("unlink_order: %d staged states, %d cut points, primary already gone in %d\n", staged, cuts, gone);
}

/* za2 F2: a power cut during the FIRST-ever write of a key leaves a torn/empty .tmp and no primary. The writers'
 * absent branch must accept that (fresh ledger) and the next verified write must succeed. */
static void lockout(void) {
  uint8_t v[GBSC_FILE_MAX], nb[GBSC_FILE_MAX]; uint32_t n = build(v, KEY_A, 2, 1), len = 0; int torn = 0;
  for (long c = 1; c <= 20; c++) {
    fresh_card(); rd_snapshot();
    rd_cut_sectors = c; (void)sf_write_verified(PA, v, n); rd_cut_sectors = -1; rd_cut_fired = 0;
    cold_boot();
    if (exists(PA ".tmp") && !exists(PA)) torn++;
    len = 0;
    CHECK(xh_absent_resolve(XD, PA, KEY_A, nb, sizeof nb, true, &len) || exists(PA), "lockout c=%ld: absent_resolve refused", c);
    if (!exists(PA)) CHECK(sf_write_verified(PA, v, n) == SF_OK && file_is(PA, v, n), "lockout c=%ld: next write failed", c);
    rd_restore();
  }
  CHECK(torn > 0, "lockout test vacuous: no cut left a .tmp-only state");
  printf("lockout: %d cut points left a .tmp-only first write\n", torn);
}

/* #389 review D4: the same-session gap. A verified .tmp (2 entries), primary absent, NO boot heal yet: the
 * writers' "absent" branch (xh_absent_resolve) must keep the 2 entries, not gbsc_init over them. */
static void gap(void) {
  uint8_t v[GBSC_FILE_MAX], nb[GBSC_FILE_MAX], bad[GBSC_FILE_MAX]; uint32_t n = build(v, KEY_A, 2, 1), len = 0;
  fresh_card(); put(PA ".tmp", v, n);
  CHECK(xh_absent_resolve(XD, PA, KEY_A, nb, sizeof nb, true, &len) && len == n && memcmp(nb, v, n) == 0 && exists(PA) && !exists(PA ".tmp"),
        "orphan .tmp not healed + read back (len=%u)", (unsigned)len);
  fresh_card(); len = 0;
  CHECK(xh_absent_resolve(XD, PA, KEY_A, nb, sizeof nb, true, &len) && gbsc_count(nb, len) == 0 && gbsc_file_key(nb, len) == KEY_A, "no .tmp: not a fresh ledger");
  fresh_card(); put(PA ".tmp", v, n);
  CHECK(!xh_absent_resolve(XD, PA, KEY_A, nb, sizeof nb, false, &len) && file_is(PA ".tmp", v, n) && !exists(PA), "read-only card: not refused / wrote");
  memcpy(bad, v, n); bad[GBSC_HEADER + 40] ^= 0x55;
  fresh_card(); put(PA ".tmp", bad, n);
  len = 0;
  CHECK(xh_absent_resolve(XD, PA, KEY_A, nb, sizeof nb, true, &len) && gbsc_count(nb, len) == 0 && gbsc_file_key(nb, len) == KEY_A,
        "corrupt .tmp: not a fresh ledger");
  CHECK(file_is(PA ".tmp.bad", bad, n) && !exists(PA ".tmp"), "#394: bad .tmp not set aside byte-identical as .tmp.bad");
  { uint8_t older[GBSC_FILE_MAX]; memcpy(older, v, n); older[0] ^= 0xFF;            /* an older .tmp.bad is replaced */
    fresh_card(); put(PA ".tmp.bad", older, n); put(PA ".tmp", bad, n); len = 0;
    CHECK(xh_absent_resolve(XD, PA, KEY_A, nb, sizeof nb, true, &len) && file_is(PA ".tmp.bad", bad, n), "#394: older .tmp.bad not replaced"); }
  { uint8_t w[GBSC_FILE_MAX]; uint32_t m = build(w, KEY_A, 3, 4); fresh_card(); put(PA, w, m);   /* re-verify-za2b F1 */
    CHECK(!xh_absent_resolve(XD, PA, KEY_A, nb, sizeof nb, true, &len) && file_is(PA, w, m), "primary present: absent_resolve handed back a fresh ledger"); }
  for (long k = 0; k < 40; k++) {   /* a card fault in the heal (XH_FAILED) or the read-back: refuse; entries survive */
    fresh_card(); put(PA ".tmp", v, n); cold_boot(); len = 0; rd_fail_read_at = k;
    bool ok = xh_absent_resolve(XD, PA, KEY_A, nb, sizeof nb, true, &len);
    long left = rd_fail_read_at; rd_fail_read_at = -1;
    if (left >= 0) continue;
    cold_boot();
    CHECK(!ok || (len == n && memcmp(nb, v, n) == 0), "fault at read %ld: accepted a ledger that is not the .tmp's (len=%u)", k, (unsigned)len);
    CHECK(ok || file_is(PA ".tmp", v, n) || file_is(PA, v, n), "fault at read %ld: refused but the entries are gone", k);
  }
  lockout();
  CHECK(!xh_absent_resolve(XD, PA, KEY_A, nb, 100, true, &len), "small buffer accepted");
  printf("gap: orphan entries survive the same-session writer path\n");
}

int main(void) {
  decisions();
  sweep();
  resurrect();
  unlink_order();
  gap();
  printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
  return fails ? 1 : 0;
}
