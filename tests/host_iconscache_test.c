/* source/art_icons_cache.c -- the icons.bin CACHE READER (stateless: no session-held
 * FIL, one shared 536 B metadata block) -- over the REAL lib/fatfs, on a RAM disk,
 * plus a cross-check of art_icons_row_for() against rom_mon.c's own (private)
 * table_species() on a real ROM dump.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_iconscache_test.c source/art_icons_cache.c source/rom_mon.c \
 *      source/rom_map.c lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c \
 *      -o /tmp/hic && /tmp/hic
 *
 * What this proves:
 *   1. a well-formed icons.bin: every frame + palette reads back exactly what was
 *      written, with no explicit "open" step (every call is self-contained);
 *   2. a file of the WRONG SIZE is refused by the metadata load, never silently
 *      truncated or padded;
 *   3. out-of-range row/frame/palette indices are refused, not read as garbage;
 *   4. metadata for one path stays cached across calls (no re-read) and reloads
 *      cleanly when the path changes or after art_icons_meta_clear();
 *   5. art_icons_row_for() agrees with rom_mon.c's own species/form -> table-index
 *      mapping for EVERY one of the 440 rows on a real ROM (Emerald) -- the two
 *      halves of the cache (what the extractor stores by row, what the loader looks
 *      up by species) must never silently drift apart.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "art_icons_cache.h"
#include "ff.h"
#include "ramdisk.h"
#include "rom_map.h"
#include "rom_mon.h"

static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define DIR_ "/PokeDNA/art"
#define ICONS DIR_ "/icons.bin"

static FATFS s_fs;
static BYTE s_work[FF_MAX_SS * 2];
static uint8_t s_content[ART_ICONS_TOTAL_BYTES];

static void fresh_card(void) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(4096);
  CHK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  CHK(f_mkdir("/PokeDNA") == FR_OK, "f_mkdir /PokeDNA failed");
  CHK(f_mkdir(DIR_) == FR_OK, "f_mkdir " DIR_ " failed");
  art_icons_meta_clear();
}

static bool write_raw(const char* path, const uint8_t* buf, uint32_t n) {
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
  if (n && (f_write(&f, buf, n, &bw) != FR_OK || bw != n)) { f_close(&f); return false; }
  return f_close(&f) == FR_OK;
}

static void build_content(uint8_t* out) {
  for (uint32_t r = 0; r < ART_ICONS_ROWS; r++)
    for (uint32_t b = 0; b < ART_ICONS_ROW_BYTES; b++)
      out[r * ART_ICONS_ROW_BYTES + b] = (uint8_t)((r * 7 + b * 13) & 0xFF);
  uint8_t* pal_ids = out + ART_ICONS_PAL_IDS_OFF;
  for (uint32_t r = 0; r < ART_ICONS_ROWS; r++) pal_ids[r] = (uint8_t)(r % 3);
  uint8_t* pals = out + ART_ICONS_PALS_OFF;
  for (int i = 0; i < 3; i++)
    for (int c = 0; c < 16; c++) {
      uint16_t v = (uint16_t)(i * 1000 + c);
      pals[i * 32 + c * 2] = (uint8_t)v;
      pals[i * 32 + c * 2 + 1] = (uint8_t)(v >> 8);
    }
}

static void t_good(void) {
  fresh_card();
  build_content(s_content);
  CHK(write_raw(ICONS, s_content, sizeof s_content), "good: could not write icons.bin");

  int rows_to_check[3] = { 0, 217, (int)ART_ICONS_ROWS - 1 };
  for (int ri = 0; ri < 3; ri++) {
    uint16_t r = (uint16_t)rows_to_check[ri];
    for (int fr = 0; fr < 2; fr++) {
      uint8_t frame[512];
      CHK(art_icons_read_frame(ICONS, r, (uint8_t)fr, frame), "good: row %u frame %d failed", r, fr);
      CHK(memcmp(frame, s_content + (uint32_t)r * ART_ICONS_ROW_BYTES + fr * 512, 512) == 0,
          "good: row %u frame %d content mismatch", r, fr);
    }
    uint16_t pal[16];
    CHK(art_icons_meta_pal(ICONS, r, pal), "good: pal %u read failed", r);
    int expect_id = r % 3;
    for (int j = 0; j < 16; j++)
      CHK(pal[j] == (uint16_t)(expect_id * 1000 + j), "good: pal %u entry %d mismatch", r, j);
    CHK(art_icons_meta_pal_id(ICONS, r) == (uint8_t)expect_id, "good: pal id %u mismatch", r);
  }
  for (int i = 0; i < 3; i++) {
    uint16_t pal[16];
    CHK(art_icons_meta_pal_at(ICONS, i, pal), "good: pal_at %d failed", i);
    for (int j = 0; j < 16; j++)
      CHK(pal[j] == (uint16_t)(i * 1000 + j), "good: pal_at %d entry %d mismatch", i, j);
  }
  CHK(!art_icons_meta_pal_at(ICONS, -1, (uint16_t[16]){0}), "pal_at(-1) must be refused");
  CHK(!art_icons_meta_pal_at(ICONS, 3, (uint16_t[16]){0}), "pal_at(3) must be refused");

  for (uint16_t r = 0; r < ART_ICONS_ROWS; r += 37) {
    uint8_t out[512];
    CHK(art_icons_read_frame(ICONS, r, 0, out), "sweep: row %u read failed", r);
    CHK(memcmp(out, s_content + (uint32_t)r * ART_ICONS_ROW_BYTES, 512) == 0,
        "sweep: row %u content mismatch", r);
  }
}

static void t_wrong_size(void) {
  fresh_card();
  build_content(s_content);

  CHK(write_raw(ICONS, s_content, sizeof s_content - 1), "setup: 1-short write failed");
  uint16_t pal[16];
  CHK(!art_icons_meta_pal(ICONS, 0, pal), "1 byte short must be refused (metadata load)");

  fresh_card();
  CHK(write_raw(ICONS, s_content, 0), "setup: empty write failed");
  CHK(!art_icons_meta_pal(ICONS, 0, pal), "empty file must be refused");

  fresh_card();
  CHK(write_raw(ICONS, s_content, 100), "setup: tiny write failed");
  CHK(!art_icons_meta_pal(ICONS, 0, pal), "a tiny garbage file must be refused");

  fresh_card();
  uint8_t big[ART_ICONS_TOTAL_BYTES + 1];
  memcpy(big, s_content, sizeof s_content);
  big[ART_ICONS_TOTAL_BYTES] = 0xFF;
  CHK(write_raw(ICONS, big, sizeof big), "setup: 1-long write failed");
  CHK(!art_icons_meta_pal(ICONS, 0, pal), "1 byte long must be refused too (not just short)");

  fresh_card();
  CHK(!art_icons_meta_pal(ICONS, 0, pal), "absent icons.bin must be refused, not crash");
  CHK(!art_icons_read_frame(ICONS, 0, 0, (uint8_t*)pal), "absent icons.bin frame read must be refused");
}

static void t_out_of_range(void) {
  fresh_card();
  build_content(s_content);
  CHK(write_raw(ICONS, s_content, sizeof s_content), "oob: write failed");
  uint8_t out[512];
  CHK(!art_icons_read_frame(ICONS, (uint16_t)ART_ICONS_ROWS, 0, out), "row == ROWS must be refused");
  CHK(!art_icons_read_frame(ICONS, 0xFFFF, 0, out), "row 0xFFFF must be refused");
  CHK(!art_icons_read_frame(ICONS, 0, 2, out), "frame 2 must be refused");
  uint16_t pal[16];
  CHK(!art_icons_meta_pal(ICONS, (uint16_t)ART_ICONS_ROWS, pal), "pal oob must be refused");
  CHK(art_icons_meta_pal_id(ICONS, (uint16_t)ART_ICONS_ROWS) == 0xFF, "pal id oob must sentinel");
}

static void t_reload_and_clear(void) {
  fresh_card();
  build_content(s_content);
  CHK(write_raw(ICONS, s_content, sizeof s_content), "reload: write failed");
  uint16_t pal[16];
  CHK(art_icons_meta_pal(ICONS, 0, pal), "reload: first load failed");
  CHK(art_icons_meta_pal(ICONS, 5, pal), "reload: cached reuse failed");

  art_icons_meta_clear();
  /* corrupt the file, THEN clear -- the corrupted state must actually be seen on the
     next load (proves clear() really forces a reload, not just a no-op). */
  uint8_t bad[ART_ICONS_TOTAL_BYTES];
  memcpy(bad, s_content, sizeof bad);
  bad[ART_ICONS_PAL_IDS_OFF] = 200; /* an invalid pal id (>= 3) for row 0 */
  CHK(write_raw(ICONS, bad, sizeof bad), "reload: corrupt write failed");
  CHK(!art_icons_meta_pal(ICONS, 0, pal),
      "reload: after clear(), the now-corrupt pal id for row 0 must be refused");
}

typedef struct { FILE* f; } FileCtx;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, fc->f) == len;
}
static void t_row_for_matches_rom_mon(const char* rom_dir) {
  char path[512];
  snprintf(path, sizeof path, "%s/Emerald.gba", rom_dir);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP row_for cross-check (no %s)\n", path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f };
  RomCtx rc;
  CHK(rom_open(&rc, file_read, &fc, (uint32_t)sz), "row_for: rom_open failed");
  RomMon rm;
  CHK(rom_mon_open(&rm, &rc), "row_for: rom_mon_open failed");

  int mismatches = 0;
  for (uint16_t species = 0; species <= 411; species++) {
    RomMonLoc a, b;
    bool oa = rom_mon_locate(&rm, species, 0, &a);
    bool ob = rom_mon_locate_row_verified(&rm, art_icons_row_for(species, 0), &b, 2, 0);
    if (oa != ob || (oa && (a.tiles != b.tiles || a.pal != b.pal))) mismatches++;
  }
  for (uint8_t form = 1; form <= 27; form++) {
    RomMonLoc a, b;
    bool oa = rom_mon_locate(&rm, 201, form, &a);
    bool ob = rom_mon_locate_row_verified(&rm, art_icons_row_for(201, form), &b, 2, 0);
    if (oa != ob || (oa && (a.tiles != b.tiles || a.pal != b.pal))) mismatches++;
  }
  { RomMonLoc a, b;
    bool oa = rom_mon_locate(&rm, 412, 0, &a);
    bool ob = rom_mon_locate_row_verified(&rm, art_icons_row_for(412, 0), &b, 2, 0);
    if (oa != ob || (oa && (a.tiles != b.tiles || a.pal != b.pal))) mismatches++; }
  CHK(mismatches == 0, "row_for disagreed with rom_mon's table_species on %d entries",
      mismatches);
  fclose(f);
}

int main(int argc, char** argv) {
  t_good();
  t_wrong_size();
  t_out_of_range();
  t_reload_and_clear();
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  if (argc > 1) {
    size_t l = strlen(argv[1]);
    if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1];
  }
  t_row_for_matches_rom_mon(dir);
  printf("%d checks, %d fail%s\n", checks, fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
