/* Host test for source/xfer_view.{c,h} -- BACKLOG #150 S150-15, step 1: the
 * read-only lookup of a converted Gen-1/2 mon's ORIGINAL (native, pre-loss) bytes.
 * Pure/FatFs mix over a RAM disk, exactly like tests/host_xferio_test.c (same
 * link posture); no PDNA_DELTA code here (that seam is BACKLOG #150 S150-15 step 4,
 * tested separately).
 *
 *   cc -std=c11 -Wall -Wextra -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf \
 *      -I tests/hostfat -I lib/fatfs -I source tests/host_xferview_test.c \
 *      source/xfer_view.c source/xfer_io.c source/xfer_rec.c source/gb_sidecar.c \
 *      source/bank_cell.c source/gb_edit.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c source/gen3_edit.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      source/data_tables.c source/savefile.c source/log.c source/item_map_g2g3.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c \
 *      -o /tmp/hxview && /tmp/hxview
 *
 * source/item_map_g2g3.c: BACKLOG #150 S150-9 (merged into this lane after S150-15
 * landed) added xr_merge_down_sel() to xfer_rec.c, which calls item_g2_to_g3() --
 * xfer_rec.c's own object needs this symbol at link time even though this test
 * never calls xr_merge_down_sel itself.
 *
 * app_can_edit() is stubbed to `true` here (source/pdna_main.c, the real definition,
 * is not linked -- it needs the whole app), same posture as host_xferio_test.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ff.h"
#include "ramdisk.h"
#include "savefile.h"
#include "gb_sidecar.h"
#include "xfer_io.h"
#include "xfer_rec.h"
#include "bank_cell.h"
#include "gb_edit.h"
#include "xfer_view.h"

bool app_can_edit(void) { return true; }
/* BACKLOG #213: xfer_io.c's write_marker() now invalidates the caller-side GB
 * ORIGINAL cache on every successful write; that cache lives in pdna_main.c, not
 * linked here, so a no-op stub stands in for it exactly like app_can_edit() above. */
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
  CHECK(f_mkdir("/PokeDNA/xfer") == FR_OK, "f_mkdir /PokeDNA/xfer failed");
}

/* PLANT_EPOCH's own formula (decision 13), 2026-09-16 00:00:00 -- the brief's own
 * illustrative epoch comment (0x6A400000, "2026-09-16") does not actually decode to
 * day 16 under the shipped bit layout (day is bits 21..17, not 20..16); this is the
 * value that DOES: (26 << 26) | (9 << 22) | (16 << 17) = 0x6A600000. Reported to the
 * orchestrator as a brief-text correction, not a design change -- the DECODE formula
 * quoted by the brief (year = 2000 + (e>>26), month = (e>>22)&15, day = (e>>17)&31)
 * is exactly what pdna_gen12.c's own encoder uses and exactly what xv_format_note
 * mirrors; only the brief's worked EXAMPLE value was arithmetically wrong. */
#define EPOCH_2026_09_16 ((uint32_t)((26u << 26) | (9u << 22) | (16u << 17)))
#define EPOCH_2026_09_10 ((uint32_t)((26u << 26) | (9u << 22) | (10u << 17)))

static void build_gen2_cell(uint8_t out80[80], uint8_t origin_game, uint32_t epoch,
                            uint32_t serial) {
  GbEditMon m; memset(&m, 0, sizeof m);
  m.gen = GB_GEN2;
  gb_set_species(&m, 152 /* Chikorita */, NULL);
  gb_set_level(&m, 12);
  gb_set_dv(&m, GB_ATK, 8); gb_set_dv(&m, GB_DEF, 8);
  gb_set_dv(&m, GB_SPE, 8); gb_set_dv(&m, GB_SPC, 8);
  gb_set_move(&m, 0, 33 /* Tackle */);
  gb_set_otid(&m, 12345);
  int rc = bc_pack(&m, 0, origin_game, epoch, serial, out80);
  if (rc != 0) { fprintf(stderr, "bc_pack failed rc=%d\n", rc); exit(1); }
}

/* Any 80-byte "Gen-3" record -- xr_key_g3 reads only bytes 0..7, so nothing else
 * about the shape matters for this test (decision 6/step 1's own instruction). */
static void build_g3(uint8_t out80[80], uint32_t pid_otid_seed) {
  memset(out80, 0, 80);
  out80[0] = (uint8_t)(pid_otid_seed);
  out80[1] = (uint8_t)(pid_otid_seed >> 8);
  out80[2] = (uint8_t)(pid_otid_seed >> 16);
  out80[3] = (uint8_t)(pid_otid_seed >> 24);
  out80[4] = 0x11; out80[5] = 0x22; out80[6] = 0x33; out80[7] = 0x44;
  out80[8] = 3;    /* language byte -- not read by anything this test touches */
}

static bool write_ledger(uint64_t key, const uint8_t* buf, uint32_t len) {
  char path[GBSC_PATH_MAX];
  if (!xr_path_for_key(path, key)) { /* not-found is fine: the path is written either way */ }
  return sf_write_verified(path, buf, len) == SF_OK;
}

/* (b)'s own read-back, independent of xr_open/xv_* -- a plain FatFs read of whatever
 * file is at g3's own resolved path, so the no-write proof does not lean on the
 * module under test to report its own honesty. */
static long read_raw_g3_ledger(const uint8_t g3rec[80], uint8_t* out, uint32_t cap) {
  char path[GBSC_PATH_MAX];
  xr_path_for_key(path, xr_key_g3(g3rec));
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return -1;
  FRESULT r = f_read(&f, out, cap, &br);
  f_close(&f);
  return r == FR_OK ? (long)br : -1;
}

int main(void) {
  static uint8_t buf[GBSC_FILE_MAX];
  static uint8_t snapshot_before[GBSC_FILE_MAX];
  static uint8_t snapshot_after[GBSC_FILE_MAX];

  uint8_t g3[80];
  build_g3(g3, 0xC0FFEEu);
  uint64_t key = xr_key_g3(g3);

  uint8_t g3_home_original[80]; build_g3(g3_home_original, 0xAAAAAA);
  uint8_t native_a[80]; build_gen2_cell(native_a, BC_ORIGIN_GOLD, EPOCH_2026_09_10, 7u);
  uint8_t native_b[80]; build_gen2_cell(native_b, BC_ORIGIN_CRYSTAL, EPOCH_2026_09_16, 8u);

  /* ================= (a) full walk: HIGHEST NATIVE_HOME index wins ================ */
  printf("== (a) highest-index NATIVE_HOME wins ==\n");
  fresh_card(2048);
  {
    uint32_t len = (uint32_t)gbsc_init(buf, key);
    GbscEntry e;

    memset(&e, 0, sizeof e); e.kind = 0 /* XR_KIND_G3_HOME */;
    memcpy(e.original80, g3_home_original, 80);
    CHECK(gbsc_add(buf, &len, sizeof buf, &e) >= 0, "(a) idx0 G3_HOME added");

    memset(&e, 0, sizeof e); e.kind = XR_KIND_NATIVE_HOME;
    memcpy(e.original80, native_a, 80); e.rtc_epoch = EPOCH_2026_09_10;
    CHECK(gbsc_add(buf, &len, sizeof buf, &e) >= 0, "(a) idx1 NATIVE_HOME (A) added");

    memset(&e, 0, sizeof e); e.kind = XR_KIND_NATIVE_HOME;
    memcpy(e.original80, native_b, 80); e.rtc_epoch = EPOCH_2026_09_16;
    CHECK(gbsc_add(buf, &len, sizeof buf, &e) >= 0, "(a) idx2 NATIVE_HOME (B) added");

    CHECK(write_ledger(key, buf, len), "(a) ledger file written");

    CHECK(xv_has_original(g3), "(a) xv_has_original true");

    XvOriginal o; memset(&o, 0, sizeof o);
    int rc = xv_find_original(g3, &o);
    CHECK(rc == 1, "(a) xv_find_original returns 1 (got %d)", rc);
    CHECK(memcmp(o.original80, native_b, 80) == 0, "(a) HIGHEST index (idx2/B) wins, not idx1/A");
    CHECK(o.rtc_epoch == EPOCH_2026_09_16, "(a) rtc_epoch is B's (%u)", o.rtc_epoch);
    CHECK(o.gen == 2, "(a) gen == 2 (got %u)", o.gen);
    CHECK(o.origin_game == BC_ORIGIN_CRYSTAL, "(a) origin_game == CRYSTAL (got %u)", o.origin_game);

    /* a record with no ledger file at all */
    uint8_t g3_other[80]; build_g3(g3_other, 0xDEADBEEFu);
    CHECK(!xv_has_original(g3_other), "(a) unrelated record: has() false");
    XvOriginal o2;
    CHECK(xv_find_original(g3_other, &o2) == 0, "(a) unrelated record: find() 0");

    /* party-shape 100-byte record, same first 8 bytes -> same entry */
    uint8_t party[100]; memset(party, 0, sizeof party); memcpy(party, g3, 80);
    party[90] = 0xEE; /* party-only tail, must not affect the key */
    XvOriginal o3; memset(&o3, 0, sizeof o3);
    CHECK(xv_find_original(party, &o3) == 1, "(a) party-shape record resolves");
    CHECK(memcmp(o3.original80, native_b, 80) == 0, "(a) party-shape finds the SAME entry");
  }

  /* ================= (b) no-write proof =========================================== */
  /* rd_writes is the RAM disk's own successful-sector-write counter (ramdisk.h) --
   * the tree's existing "did anything land on the card" primitive. A memcmp of the
   * ledger file's own bytes before/after backs it up: together they prove neither
   * the disk's write path NOR the file's own content moved across every xv_* call. */
  printf("== (b) no-write proof ==\n");
  {
    long before = (long)rd_writes;
    memset(snapshot_before, 0, sizeof snapshot_before);
    long n1 = read_raw_g3_ledger(g3, snapshot_before, sizeof snapshot_before);
    CHECK(n1 > 0, "(b) pre-snapshot: ledger file readable (%ld bytes)", n1);

    XvOriginal o;
    (void)xv_has_original(g3);
    (void)xv_find_original(g3, &o);
    (void)xv_has_original(g3);
    (void)xv_find_original(g3, &o);

    long after = (long)rd_writes;
    CHECK(after == before, "(b) rd_writes unchanged across every xv_* call (%ld -> %ld)", before, after);

    memset(snapshot_after, 0, sizeof snapshot_after);
    long n2 = read_raw_g3_ledger(g3, snapshot_after, sizeof snapshot_after);
    CHECK(n2 == n1, "(b) ledger file length unchanged (%ld -> %ld)", n1, n2);
    CHECK(memcmp(snapshot_before, snapshot_after, sizeof snapshot_before) == 0,
          "(b) ledger file bytes byte-identical after every xv_* call");
  }

  /* ================= (c) a G3_HOME-only file: has() true, find() 0 =============== */
  printf("== (c) G3_HOME-only file ==\n");
  fresh_card(2048);
  {
    uint8_t g3c[80]; build_g3(g3c, 0x1000);
    uint64_t keyc = xr_key_g3(g3c);
    uint32_t len = (uint32_t)gbsc_init(buf, keyc);
    GbscEntry e; memset(&e, 0, sizeof e); e.kind = 0;
    memcpy(e.original80, g3_home_original, 80);
    CHECK(gbsc_add(buf, &len, sizeof buf, &e) >= 0, "(c) G3_HOME entry added");
    CHECK(write_ledger(keyc, buf, len), "(c) ledger written");

    CHECK(xv_has_original(g3c), "(c) has() true (decision 4's caveat)");
    XvOriginal o;
    CHECK(xv_find_original(g3c, &o) == 0, "(c) find() 0 -- no NATIVE_HOME entry");
  }

  /* (c2) M2's real target: a G3_HOME-kind entry whose original80 IS a valid native
   * cell (bc_is_native() true) -- only the kind filter can reject this one; deleting
   * it (M2) makes find() wrongly return 1. (c)'s own entry uses a FAKE "Gen-3" blob
   * as original80, which bc_is_native() already rejects on its own -- it cannot tell
   * M2 apart from the untouched code, hence this second case. */
  printf("== (c2) G3_HOME kind but a NATIVE original80 ==\n");
  fresh_card(2048);
  {
    uint8_t g3c2[80]; build_g3(g3c2, 0x1500);
    uint64_t keyc2 = xr_key_g3(g3c2);
    uint32_t len = (uint32_t)gbsc_init(buf, keyc2);
    GbscEntry e; memset(&e, 0, sizeof e); e.kind = 0 /* XR_KIND_G3_HOME */;
    memcpy(e.original80, native_a, 80);          /* a REAL native cell, wrong kind byte */
    CHECK(gbsc_add(buf, &len, sizeof buf, &e) >= 0, "(c2) mis-kinded entry added");
    CHECK(write_ledger(keyc2, buf, len), "(c2) ledger written");

    XvOriginal o;
    CHECK(xv_find_original(g3c2, &o) == 0, "(c2) find() 0 -- kind filter rejects it (M2's target)");
  }

  /* ================= (d) a mis-stamped NATIVE_HOME entry is skipped ============== */
  printf("== (d) mis-stamped NATIVE_HOME entry ==\n");
  fresh_card(2048);
  {
    uint8_t g3d[80]; build_g3(g3d, 0x2000);
    uint64_t keyd = xr_key_g3(g3d);
    uint32_t len = (uint32_t)gbsc_init(buf, keyd);
    GbscEntry e; memset(&e, 0, sizeof e);
    e.kind = XR_KIND_NATIVE_HOME;
    memcpy(e.original80, g3_home_original, 80);   /* GEN-3 bytes under a NATIVE_HOME kind */
    CHECK(gbsc_add(buf, &len, sizeof buf, &e) >= 0, "(d) mis-stamped entry added");
    CHECK(write_ledger(keyd, buf, len), "(d) ledger written");

    XvOriginal o;
    CHECK(xv_find_original(g3d, &o) == 0, "(d) mis-stamped entry skipped by bc_is_native()");
  }

  /* ================= (e) a truncated file -> -1 ================================== */
  printf("== (e) truncated ledger file ==\n");
  fresh_card(2048);
  {
    uint8_t g3e[80]; build_g3(g3e, 0x3000);
    uint64_t keye = xr_key_g3(g3e);
    uint32_t len = (uint32_t)gbsc_init(buf, keye);
    GbscEntry e; memset(&e, 0, sizeof e); e.kind = XR_KIND_NATIVE_HOME;
    memcpy(e.original80, native_a, 80);
    int idx = gbsc_add(buf, &len, sizeof buf, &e);
    CHECK(idx >= 0, "(e) entry added");
    CHECK(write_ledger(keye, buf, len - 1), "(e) TRUNCATED ledger written (len-1 bytes)");

    XvOriginal o;
    int rc = xv_find_original(g3e, &o);
    CHECK(rc == -1, "(e) truncated file -> -1 (got %d)", rc);
  }

  /* ================= (f) xv_origin_name / xv_format_note ========================= */
  printf("== (f) xv_origin_name / xv_format_note ==\n");
  {
    CHECK(xv_origin_name(BC_ORIGIN_UNKNOWN) == NULL, "(f) UNKNOWN -> NULL");
    CHECK(strcmp(xv_origin_name(BC_ORIGIN_CRYSTAL), "CRYSTAL") == 0, "(f) CRYSTAL name");
    CHECK(strcmp(xv_origin_name(BC_ORIGIN_RED), "RED") == 0, "(f) RED name");

    char note[24];
    xv_format_note(note, 2, BC_ORIGIN_CRYSTAL, EPOCH_2026_09_16);
    CHECK(strcmp(note, "CRYSTAL 26-09-16") == 0, "(f) CRYSTAL+date (got \"%s\")", note);
    CHECK(strlen(note) <= 17, "(f) CRYSTAL+date length <= 17 (got %d)", (int)strlen(note));

    xv_format_note(note, 1, BC_ORIGIN_RED, 0);
    CHECK(strcmp(note, "RED (no date)") == 0, "(f) RED no-date (got \"%s\")", note);
    CHECK(strlen(note) <= 17, "(f) RED no-date length <= 17");

    xv_format_note(note, 2, BC_ORIGIN_UNKNOWN, EPOCH_2026_09_16);
    CHECK(strcmp(note, "GEN 2 26-09-16") == 0, "(f) unknown-game+date (got \"%s\")", note);
    CHECK(strlen(note) <= 17, "(f) unknown-game+date length <= 17");

    xv_format_note(note, 2, BC_ORIGIN_CRYSTAL, 0);
    CHECK(strcmp(note, "CRYSTAL (no date)") == 0, "(f) CRYSTAL no-date (got \"%s\")", note);
    CHECK(strlen(note) <= 18, "(f) CRYSTAL no-date length <= 18 (widest worst case)");
  }

  /* ================= M1-M4: mutation proofs (run by hand on a scratch copy) ====== */
  printf("== mutation proofs (see report for FAIL lines from mutated copies) ==\n");
  printf("  M1: flip the walk to first-index-wins -> (a)'s highest-wins assertion must FAIL\n");
  printf("  M2: delete the kind filter -> (c)'s G3_HOME-only file must incorrectly return 1\n");
  printf("  M3: delete the bc_is_native check -> (d)'s mis-stamped entry must incorrectly be returned\n");
  printf("  M4: change >> 26 to >> 25 in the decoder -> (f)'s date checks must FAIL\n");

  printf("\nxferview: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
