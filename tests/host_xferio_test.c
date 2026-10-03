/* Host test for source/xfer_io.{c,h} -- the FatFs half of the transfer ledger
 * (BACKLOG #150 S150-6): path resolution (decision 4/D-Q7) and the one-time
 * migration (decision 5), on the REAL lib/fatfs over a RAM disk, exactly like
 * tests/host_savefat_test.c.
 *
 *   cc -std=c11 -Wall -Wextra -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf \
 *      -I tests/hostfat -I lib/fatfs -I source tests/host_xferio_test.c \
 *      source/xfer_io.c source/xfer_rec.c source/gb_sidecar.c source/bank_cell.c \
 *      source/gb_edit.c source/gen1_save.c source/gen1_write.c source/gen2_save.c \
 *      source/gen2_write.c source/gen3_edit.c source/gen3_mon.c source/gen3_box.c \
 *      source/gen3_save.c source/gen3_daycare.c source/data_tables.c \
 *      source/item_map_g2g3.c \
 *      source/savefile.c source/log.c lib/fatfs/ff.c lib/fatfs/ffunicode.c \
 *      tests/hostfat/ramdisk.c -o /tmp/hxio && /tmp/hxio
 *
 * app_can_edit() is stubbed to `true` here (source/pdna_main.c, the real definition,
 * is not linked -- it needs the whole app). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ff.h"
#include "ramdisk.h"
#include "savefile.h"
#include "gb_sidecar.h"
#include "xfer_io.h"

static bool g_can_edit = true;
bool app_can_edit(void) { return g_can_edit; }
/* BACKLOG #213: write_marker() (this file's own SUT) now invalidates the caller-side
 * GB ORIGINAL cache on every successful write -- that cache lives in pdna_main.c,
 * not linked here, so a no-op stub stands in for it exactly like app_can_edit() above. */
void app_xv_cache_invalidate(void) {}

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];

static void fresh_card(unsigned sectors) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(sectors);
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  CHECK(f_mkdir("/PokeDNA") == FR_OK, "f_mkdir /PokeDNA failed");
  CHECK(f_mkdir("/PokeDNA/sidecar") == FR_OK, "f_mkdir /PokeDNA/sidecar failed");
}

static bool write_raw(const char* path, const uint8_t* buf, uint32_t n) {
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
  bool ok = (f_write(&f, buf, n, &bw) == FR_OK) && (bw == n);
  return (f_close(&f) == FR_OK) && ok;
}

static long read_raw(const char* path, uint8_t* out, uint32_t cap) {
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return -1;
  FRESULT r = f_read(&f, out, cap, &br);
  f_close(&f);
  return r == FR_OK ? (long)br : -1;
}

static bool exists(const char* path) {
  FILINFO fi;
  return f_stat(path, &fi) == FR_OK;
}

/* A minimal, valid .pds file -- header + 1 entry -- built with gbsc_* directly. */
static uint32_t build_one(uint8_t* buf, uint32_t cap, uint64_t key, uint8_t fill_byte) {
  uint32_t len = (uint32_t)gbsc_init(buf, key);
  GbscEntry e; memset(&e, 0, sizeof e);
  e.gen = GB_GEN2;
  e.otid16 = 0x1234;
  e.dv4[0] = 1; e.dv4[1] = 2; e.dv4[2] = 3; e.dv4[3] = 4;
  memset(e.otname_written, fill_byte, GB_NAME_BYTES);
  memset(e.nick_written, fill_byte, GB_NAME_BYTES);
  int idx = gbsc_add(buf, &len, cap, &e);
  (void)idx;
  return len;
}

int main(void) {
  static uint8_t scratch[GBSC_FILE_MAX];
  static uint8_t content[GBSC_FILE_MAX];
  static uint8_t content2[GBSC_FILE_MAX];
  static uint8_t readback[GBSC_FILE_MAX];

  uint64_t key = 0x1122334455667788ULL;
  char hex[17];
  gbsc_key_hex(key, hex);
  char sidecar_name[64], sidecar_path[128], xfer_path[128];
  snprintf(sidecar_name, sizeof sidecar_name, "%s.pds", hex);
  snprintf(sidecar_path, sizeof sidecar_path, "/PokeDNA/sidecar/%s", sidecar_name);
  snprintf(xfer_path, sizeof xfer_path, "/PokeDNA/xfer/%s", sidecar_name);

  /* ---- (g) xr_path_for_key: neither file exists ---------------------------- */
  printf("== (g) xr_path_for_key: neither file exists ==\n");
  fresh_card(2048);
  {
    char out[GBSC_PATH_MAX];
    bool found = xr_path_for_key(out, key);
    CHECK(!found, "(g) neither file exists -> false");
    CHECK(strcmp(out, xfer_path) == 0, "(g) still resolves to the xfer path (%s vs %s)", out, xfer_path);
  }

  /* ---- (a) a record in sidecar only is found by xr_open --------------------- */
  printf("== (a) sidecar-only record found by xr_open ==\n");
  uint32_t clen = build_one(content, sizeof content, key, 0x81);
  CHECK(write_raw(sidecar_path, content, clen), "(a) seed the sidecar file");
  {
    char out[GBSC_PATH_MAX];
    CHECK(xr_path_for_key(out, key), "(a) xr_path_for_key finds the sidecar file");
    CHECK(strcmp(out, sidecar_path) == 0, "(a) resolves to the SIDECAR path (%s vs %s)", out, sidecar_path);

    uint32_t len = 0;
    SfStatus st = xr_open(key, readback, sizeof readback, &len, out);
    CHECK(st == SF_OK, "(a) xr_open succeeds (%s)", sf_status_str(st));
    CHECK(len == clen && memcmp(readback, content, clen) == 0, "(a) xr_open returns the sidecar bytes exactly");
  }

  /* ---- (b) migrate: found in xfer, sidecar source untouched ------------------ */
  printf("== (b) xr_migrate_once copies to xfer, leaves sidecar untouched ==\n");
  int copied = xr_migrate_once(scratch, sizeof scratch);
  CHECK(copied == 1, "(b) xr_migrate_once copies exactly 1 file (got %d)", copied);
  CHECK(exists("/PokeDNA/xfer/MIGRATED"), "(b) the marker exists after migration");
  CHECK(exists(xfer_path), "(b) the xfer twin now exists");
  CHECK(exists(sidecar_path), "(b) the sidecar SOURCE still exists");
  {
    long n1 = read_raw(sidecar_path, content, sizeof content);
    long n2 = read_raw(xfer_path, content2, sizeof content2);
    CHECK(n1 == (long)clen && n2 == (long)clen && memcmp(content, content2, clen) == 0,
          "(b) sidecar and xfer are byte-identical (%ld vs %ld)", n1, n2);
  }
  {
    char out[GBSC_PATH_MAX];
    CHECK(xr_path_for_key(out, key), "(b) xr_path_for_key finds a file");
    CHECK(strcmp(out, xfer_path) == 0, "(b) post-migration: resolves to the XFER path (%s vs %s)", out, xfer_path);
  }

  /* ---- (c) review F3: POST-MIGRATION, the sidecar fallback is gated OFF -------- */
  printf("== (c) marker present + xfer copy deleted -- resolves to xfer/false, NOT sidecar (F3) ==\n");
  CHECK(xr_migrated(), "(c) the marker is present (precondition for this case)");
  CHECK(f_unlink(xfer_path) == FR_OK, "(c) delete the xfer copy");
  {
    char out[GBSC_PATH_MAX];
    bool found = xr_path_for_key(out, key);
    CHECK(!found, "(c) xr_path_for_key returns false (no live record) once migrated");
    CHECK(strcmp(out, xfer_path) == 0, "(c) still resolves to the XFER path, not sidecar (%s vs %s)", out, xfer_path);

    uint32_t len = 0; char out2[GBSC_PATH_MAX];
    SfStatus st = xr_open(key, readback, sizeof readback, &len, out2);
    CHECK(st == SF_ERR_OPEN, "(c) xr_open fails (SF_ERR_OPEN) -- does NOT silently return the stale sidecar copy (%s)",
          sf_status_str(st));
  }

  /* ---- (c2) the reviewer's scenario: migrate -> a claim/KEEP write lands ONLY in
   * xfer (the sidecar copy is now STALE) -> delete the xfer copy -> resolve ->
   * must NOT return the (wrong, stale) sidecar bytes. */
  printf("== (c2) a post-migration write to xfer only, then its deletion, never falls back to the stale sidecar ==\n");
  {
    uint8_t updated[GBSC_FILE_MAX];
    memcpy(updated, content, clen);
    CHECK(gbsc_set_claimed(updated, clen, 0, true) == 0, "(c2) simulate a claim write landing in the xfer copy");
    CHECK(memcmp(updated, content, clen) != 0, "(c2) the claimed bytes now differ from the sidecar's frozen copy");
    CHECK(write_raw(xfer_path, updated, clen), "(c2) write the claimed bytes to the xfer copy");

    CHECK(f_unlink(xfer_path) == FR_OK, "(c2) delete the (claimed) xfer copy");
    char out[GBSC_PATH_MAX];
    bool found = xr_path_for_key(out, key);
    CHECK(!found, "(c2) xr_path_for_key returns false -- the claim is not silently lost to a stale sidecar read");
    CHECK(strcmp(out, xfer_path) == 0, "(c2) resolves to the xfer path, not the stale sidecar (%s vs %s)", out, xfer_path);
  }

  /* restore the xfer copy (the ORIGINAL, unclaimed content) for the following cases */
  CHECK(write_raw(xfer_path, content, clen), "(c) restore the xfer copy for the next cases");

  /* ---- (d) a second xr_migrate_once copies 0 files, marker unchanged -------- */
  printf("== (d) idempotent: a second migrate copies 0 files ==\n");
  {
    uint8_t marker_before[16]; long mlen = read_raw("/PokeDNA/xfer/MIGRATED", marker_before, sizeof marker_before);
    int c2 = xr_migrate_once(scratch, sizeof scratch);
    CHECK(c2 == 0, "(d) second xr_migrate_once copies 0 files (got %d)", c2);
    uint8_t marker_after[16]; long mlen2 = read_raw("/PokeDNA/xfer/MIGRATED", marker_after, sizeof marker_after);
    CHECK(mlen == mlen2 && memcmp(marker_before, marker_after, (size_t)mlen) == 0,
          "(d) the marker is unchanged");
  }

  /* ---- (e) a corrupted xfer twin is re-copied -------------------------------- */
  printf("== (e) a corrupted xfer twin is re-copied ==\n");
  {
    /* remove the marker so the migration walk runs again (it is otherwise an O(1)
     * early return) -- this simulates a retry after a partial earlier migration. */
    CHECK(f_unlink("/PokeDNA/xfer/MIGRATED") == FR_OK, "(e) remove the marker to force a re-walk");
    uint8_t corrupt[GBSC_FILE_MAX];
    memcpy(corrupt, content, clen);
    corrupt[10] ^= 0xFFu;   /* flip a byte inside the header/entry span, same length */
    CHECK(write_raw(xfer_path, corrupt, clen), "(e) corrupt the xfer twin (same length)");

    int c3 = xr_migrate_once(scratch, sizeof scratch);
    CHECK(c3 == 1, "(e) the corrupted twin is re-copied (got %d)", c3);
    long n = read_raw(xfer_path, content2, sizeof content2);
    CHECK(n == (long)clen && memcmp(content2, content, clen) == 0,
          "(e) the xfer twin now matches the sidecar source again");
  }

  /* ---- (f) a mid-migration failure leaves no marker and no truncated .pds --- */
  printf("== (f) a mid-migration write failure leaves no marker, no truncated file ==\n");
  {
    CHECK(f_unlink("/PokeDNA/xfer/MIGRATED") == FR_OK, "(f) remove the marker");
    CHECK(f_unlink(xfer_path) == FR_OK, "(f) remove the xfer twin so this file needs copying again");

    /* fail the very first disk write the migration's sf_write_verified issues --
     * its .tmp write never lands, so xr_migrate_once must return -1 with no marker
     * and nothing (not even a .tmp) left at the final xfer name. */
    rd_fail_write_in = 1;
    int c4 = xr_migrate_once(scratch, sizeof scratch);
    rd_fail_write_in = 0;   /* heal -- the knob does this itself after N writes, belt */
    CHECK(c4 == -1, "(f) xr_migrate_once reports failure (got %d)", c4);
    CHECK(!exists("/PokeDNA/xfer/MIGRATED"), "(f) no marker was written");
    CHECK(!exists(xfer_path), "(f) no (truncated) file landed at the final xfer name");
    CHECK(!exists("/PokeDNA/xfer/MIGRATED.tmp"), "(f) no leftover .tmp at the marker name either");
  }

  /* ---- (c3) review R2/decision-(a): the EverDrive shape -- /PokeDNA/xfer exists
   * (the dir was created by an earlier migrate call) but MIGRATED does not (this
   * card can never write, so migration never completed) -- the sidecar fallback
   * must still work and pass 2 must still be reachable. */
  printf("== (c3) marker ABSENT, /PokeDNA/xfer dir exists (EverDrive shape) -- sidecar still resolves ==\n");
  {
    CHECK(!exists("/PokeDNA/xfer/MIGRATED"), "(c3) precondition: no marker (left absent by case (f))");
    FILINFO xdirfi;
    CHECK(f_stat("/PokeDNA/xfer", &xdirfi) == FR_OK, "(c3) precondition: /PokeDNA/xfer dir exists");
    CHECK(!exists(xfer_path), "(c3) precondition: no xfer twin for this key");
    CHECK(exists(sidecar_path), "(c3) precondition: the sidecar source still exists");

    char out[GBSC_PATH_MAX];
    bool found = xr_path_for_key(out, key);
    CHECK(found, "(c3) xr_path_for_key finds a file (falls back to sidecar)");
    CHECK(strcmp(out, sidecar_path) == 0,
          "(c3) resolves to the SIDECAR path when unmigrated, even with /PokeDNA/xfer present (%s vs %s)",
          out, sidecar_path);

    uint32_t len = 0; char out2[GBSC_PATH_MAX];
    SfStatus st = xr_open(key, readback, sizeof readback, &len, out2);
    CHECK(st == SF_OK, "(c3) xr_open succeeds via the sidecar fallback (%s)", sf_status_str(st));
    CHECK(len == clen && memcmp(readback, content, clen) == 0, "(c3) bytes match the sidecar original exactly");
  }

  /* ---- BACKLOG #213: XrMissCache -- pure ring-buffer logic (no FatFs involved),
   * the negative half of the GB ORIGINAL row's cache. The real invariant this
   * backs is "after ANY ledger write, the next lookup goes to the card" -- proven
   * here at the cache's own level: miss -> cached -> (a write happens, the caller
   * calls xr_miss_cache_reset -- app_xv_cache_invalidate() in the real app) ->
   * miss again means a real re-lookup, not a stale cached "no file". */
  printf("== XrMissCache: miss -> cached -> invalidate -> miss again ==\n");
  {
    XrMissCache c; xr_miss_cache_reset(&c);
    uint64_t k1 = 0x1122334455667788ULL, k2 = 0xAABBCCDDEEFF0011ULL;
    CHECK(!xr_miss_cache_has(&c, k1), "fresh cache: k1 not cached");
    xr_miss_cache_remember(&c, k1);
    CHECK(xr_miss_cache_has(&c, k1), "after remember: k1 IS cached (0 f_stat on a repeat open)");
    CHECK(!xr_miss_cache_has(&c, k2), "k2 (never remembered) is NOT cached");
    xr_miss_cache_reset(&c);   /* stands in for a ledger write's app_xv_cache_invalidate() */
    CHECK(!xr_miss_cache_has(&c, k1),
          "after invalidate: k1 no longer cached -- the next lookup goes back to the card");
  }
  printf("== XrMissCache: the 9th remember evicts the oldest (ring, never unbounded -- golden rule 2) ==\n");
  {
    XrMissCache c; xr_miss_cache_reset(&c);
    for (int i = 0; i < XR_MISS_RING_N; i++) xr_miss_cache_remember(&c, (uint64_t)(i + 1));
    for (int i = 0; i < XR_MISS_RING_N; i++)
      CHECK(xr_miss_cache_has(&c, (uint64_t)(i + 1)), "ring holds key %d before the 9th remember", i + 1);
    xr_miss_cache_remember(&c, 999);   /* one past capacity -- evicts slot 0 (key 1) */
    CHECK(!xr_miss_cache_has(&c, 1), "9th remember evicted the oldest key (1)");
    CHECK(xr_miss_cache_has(&c, 999), "9th remember's own key is now cached");
    for (int i = 1; i < XR_MISS_RING_N; i++)
      CHECK(xr_miss_cache_has(&c, (uint64_t)(i + 1)), "keys 2..8 survive the 9th remember");
  }

  /* ---- BACKLOG #392(a): a READ-ONLY card reads an orphan <key>.pds.tmp as the primary, in RAM, never writing ---- */
  printf("== (392a) read-only card: orphan .tmp read as the primary ==\n");
  {
    uint32_t tn = build_one(content, sizeof content, key, 0x5A), len = 0;
    char out[GBSC_PATH_MAX], tmp_path[128];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", xfer_path);
    fresh_card(2048); CHECK(f_mkdir("/PokeDNA/xfer") == FR_OK, "(392a) mkdir xfer");
    CHECK(write_raw(tmp_path, content, tn), "(392a) write orphan .tmp");
    g_can_edit = false;
    CHECK(xr_path_for_key(out, key) && strcmp(out, xfer_path) == 0, "(392a) read-only: orphan .tmp counts as present, path stays the PRIMARY (%s)", out);
    SfStatus st = xr_open(key, readback, sizeof readback, &len, out);
    CHECK(st == SF_OK && len == tn && memcmp(readback, content, tn) == 0, "(392a) read-only: xr_open returns the .tmp bytes (%s)", sf_status_str(st));
    CHECK(!exists(xfer_path) && exists(tmp_path), "(392a) read-only: NOTHING written/renamed");
    g_can_edit = true;
    CHECK(!xr_path_for_key(out, key), "(392a) writable card: the same orphan is NOT claimed present (heal owns it)");
    CHECK(xr_open(key, readback, sizeof readback, &len, out) == SF_ERR_OPEN, "(392a) writable card: xr_open does not read the .tmp");
    /* a .tmp that fails the same test as xh_heal_key (wrong key / torn) reads as absent even read-only */
    uint8_t other[GBSC_FILE_MAX]; uint32_t on = build_one(other, sizeof other, key ^ 1ULL, 0x33);
    fresh_card(2048); CHECK(f_mkdir("/PokeDNA/xfer") == FR_OK, "(392a) mkdir xfer 2");
    CHECK(write_raw(tmp_path, other, on), "(392a) write foreign-key .tmp");
    g_can_edit = false;
    CHECK(xr_open(key, readback, sizeof readback, &len, out) == SF_ERR_OPEN, "(392a) read-only: foreign-key .tmp refused");
    CHECK(write_raw(tmp_path, content, tn / 2), "(392a) write torn .tmp");
    CHECK(xr_open(key, readback, sizeof readback, &len, out) == SF_ERR_OPEN, "(392a) read-only: torn .tmp refused");
    g_can_edit = true;
  }

  printf("\n%d check(s), %s\n", g_check, g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}
