/* Host test: BACKLOG #175c/#226 review structural finding -- xr_open() (source/
 * xfer_io.c) must defer to a REAL ledger file over bank_plant.c's PDNA_DELTA-only
 * planted-ledger shim once a real file exists, so a genuine save-now (a real,
 * verified write on a VSD-attached delta build) is not shadowed forever by the
 * stale planted bytes -- the exact bug that made the restore's "save-now succeeds
 * -> the restore proceeds" happy path unreachable at either site (source/pdna_box.c
 * pc_bank_restore_up, source/pdna_gen12.c gb_lift_restore).
 *
 *   cc -std=c11 -Wall -Wextra -DPDNA_DELTA -DPDNA_GEN12_HOST -DFF_USE_MKFS=1 \
 *      -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf \
 *      -I tests/hostfat -I lib/fatfs -I source tests/host_xfer_delta_defer_test.c \
 *      source/xfer_io.c source/bank_plant.c source/gb_new_mon.c source/gb_editor.c \
 *      source/gb_session.c source/rom_gblearn.c source/rom_gbbase.c \
 *      source/rom_gbsprite.c source/gb_sprite_codec.c source/ui_font.c \
 *      source/bank_down_convert.c source/gen12_convert.c source/item_map_g1g2.c source/gb_bag.c source/gb_fields.c source/gen3_to_gb.c \
 *      source/xfer_rec.c source/bank_cell.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/gb_sidecar.c source/gen3_save.c source/gen3_mon.c source/gen3_box.c \
 *      source/gen3_edit.c source/gen3_daycare.c source/data_tables.c \
 *      source/evolutions.c source/item_map_g2g3.c source/gb_item_names.c \
 *      source/gen3_clip.c source/gb_moves_legal.c source/savefile.c source/log.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c \
 *      -o /tmp/hxdefer && /tmp/hxdefer
 *
 * app_can_edit()/app_met_game()/app_xv_cache_invalidate() stubbed the same way
 * host_xferio_test.c/host_xferplant_test.c already stub them (source/pdna_main.c,
 * the real definitions, needs the whole app -- not linked here).
 *
 * Sections:
 *   A. plant only, no real file: xr_open() returns the PLANTED bytes (the shim's
 *      pre-existing, still-correct behaviour -- unchanged by this fix).
 *   B. plant AND a real file at the SAME key: xr_open() returns the REAL file's
 *      bytes, not the planted ones -- the NEW deferral behaviour this test exists
 *      to pin. RED PROOF (self-audit #1): this is the exact scratch-mutation the
 *      review demonstrated -- reverting xr_open() to try the shim BEFORE the real
 *      read makes this assertion fail (demonstrated by the orchestrator's own
 *      scratch-copy re-run of the pre-fix source, not re-implemented as a second
 *      code path in-process, per STANDING-RULES' "mutate the REAL source" bar).
 *   C. no plant, no real file: xr_open() returns SF_ERR_OPEN (unaffected case).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ff.h"
#include "ramdisk.h"
#include "savefile.h"
#include "gb_sidecar.h"
#include "xfer_io.h"
#include "xfer_rec.h"
#include "bank_plant.h"

bool app_can_edit(void) { return true; }
uint8_t app_met_game(void) { return 3; }   /* Emerald -- matches bank_plant.c's own tgt.met_game */
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

static bool write_raw(const char* path, const uint8_t* buf, uint32_t n) {
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
  bool ok = (f_write(&f, buf, n, &bw) == FR_OK) && (bw == n);
  return (f_close(&f) == FR_OK) && ok;
}

/* A minimal, valid .pds file -- header + 1 entry, distinguishable from the plant's
 * own content by a fill byte only a REAL write would carry. */
static uint32_t build_real_file(uint8_t* buf, uint32_t cap, uint64_t key, uint8_t fill_byte) {
  uint32_t len = (uint32_t)gbsc_init(buf, key);
  GbscEntry e; memset(&e, 0, sizeof e);
  e.gen = GB_GEN2;
  e.otid16 = 0x5678;
  e.dv4[0] = 9; e.dv4[1] = 9; e.dv4[2] = 9; e.dv4[3] = 9;
  memset(e.otname_written, fill_byte, GB_NAME_BYTES);
  memset(e.nick_written, fill_byte, GB_NAME_BYTES);
  int idx = gbsc_add(buf, &len, cap, &e);
  (void)idx;
  return len;
}

int main(void) {
  static uint8_t g3_out[4][80];
  static uint8_t readback[GBSC_FILE_MAX];
  static uint8_t real_content[GBSC_FILE_MAX];

  fresh_card(2048);

  int n = bank_plant_xfer_seed_all(g3_out);
  CHECK(n >= 1, "bank_plant_xfer_seed_all seeds at least 1 slot (got %d)", n);
  if (n < 1) { printf("host_xfer_delta_defer_test: FAIL (setup)\n"); return 1; }
  uint64_t key = xr_key_g3(g3_out[0]);   /* slot 0: CLAIMED+ALTERED, XR_STATE_PENDING is slot 3 -- key derivation is the same either way */
  char path[GBSC_PATH_MAX];
  bool found = xr_path_for_key(path, key);
  /* xr_path_for_key_hint's own PDNA_DELTA branch (source/xfer_io.c) already treats a
   * planted key as "exists" (bank_plant_xfer_has), independent of any real file --
   * that is #209's EXISTENCE question, unaffected by this fix. Sanity only: the
   * path it resolves to is the real xfer path this test writes to below. */
  CHECK(found, "the planted key resolves as existing (BACKLOG #209, unaffected by this fix)");

  /* ---- A: plant only ---------------------------------------------------- */
  printf("== A: plant only, no real file -- xr_open returns the planted bytes ==\n");
  {
    uint32_t len = 0;
    SfStatus st = xr_open(key, readback, sizeof readback, &len, NULL);
    CHECK(st == SF_OK, "A: xr_open succeeds via the shim (%s)", sf_status_str(st));
    bool has_shim = bank_plant_xfer_has(key);
    CHECK(has_shim, "A: bank_plant_xfer_has confirms this key is a planted one");
  }

  /* ---- B: plant AND a real file at the SAME key -------------------------- */
  printf("== B: plant + a real file at the same key -- xr_open must defer to the REAL file ==\n");
  uint32_t real_len = build_real_file(real_content, sizeof real_content, key, 0xAB);
  CHECK(write_raw(path, real_content, real_len), "B: seed the REAL file at %s", path);
  {
    uint32_t len = 0;
    SfStatus st = xr_open(key, readback, sizeof readback, &len, NULL);
    CHECK(st == SF_OK, "B: xr_open succeeds (%s)", sf_status_str(st));
    bool matches_real = (len == real_len) && (memcmp(readback, real_content, real_len) == 0);
    CHECK(matches_real, "B: xr_open returns the REAL file's bytes, not the stale planted "
                         "ones -- this is the #175c/#226 deferral fix (a real save-now must "
                         "not be shadowed forever)");
  }

  /* ---- C: no plant, no real file (unaffected case) ------------------------ */
  printf("== C: an unplanted, unwritten key -- SF_ERR_OPEN ==\n");
  {
    uint64_t other_key = key ^ 0xFFFFFFFFFFFFFFFFULL;
    CHECK(!bank_plant_xfer_has(other_key), "C: sanity -- this key is not planted");
    uint32_t len = 0;
    SfStatus st = xr_open(other_key, readback, sizeof readback, &len, NULL);
    CHECK(st == SF_ERR_OPEN, "C: xr_open fails cleanly for a key nobody seeded (%s)",
          sf_status_str(st));
  }

  printf("%d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
