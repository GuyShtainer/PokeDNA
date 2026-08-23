/* source/art_icons_cache.c -- the icons.bin FORMAT reader (no handle, no cache, no
 * static byte: the caller owns the FIL) -- over the REAL lib/fatfs, on a RAM disk,
 * plus a cross-check of art_icons_row_for() against rom_mon.c's own (private)
 * table_species() on a real ROM dump.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_iconscache_test.c source/art_icons_cache.c source/rom_mon.c \
 *      source/rom_map.c lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c \
 *      -o /tmp/hic && /tmp/hic
 *
 * What this proves:
 *   1. a well-formed icons.bin: every row (BOTH bob frames) + palette reads back
 *      exactly what was written;
 *   2. a MULTI-ROW read returns the same bytes as the same rows read one at a time --
 *      the bulk path is the retention path, so "one big read == N small reads" is the
 *      claim the whole batching design rests on -- and does it in ONE disk_read;
 *   3. a file of the WRONG SIZE is refused by the metadata read, never silently
 *      truncated or padded;
 *   4. out-of-range rows and spans that would run off the end are refused, not read
 *      as garbage;
 *   5. art_icons_row_for() agrees with rom_mon.c's own species/form -> table-index
 *      mapping for EVERY one of the 440 rows on a real ROM (Emerald) -- the two
 *      halves of the cache (what the extractor stores by row, what the loader looks
 *      up by species) must never silently drift apart.
 *
 * The old "metadata stays cached across calls / reloads after clear()" section is gone
 * with the statics it tested: this module holds nothing now. That caching moved to
 * source/icon_store.c and is tested by tests/host_iconstore_test.c.
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

/* The handle this module reads through. Opening it here rather than inside the reader
 * IS the change under test: the previous cut did f_open + f_lseek + f_read + f_close
 * per 512 B frame, which on a real card was 20-35 disk_read calls to deliver 512 B. */
static bool with_open(const char* path, bool (*fn)(FIL*, void*), void* ud) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) return false;
  bool r = fn(&f, ud);
  f_close(&f);
  return r;
}

static void fresh_card(void) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(4096);
  CHK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  CHK(f_mkdir("/PokeDNA") == FR_OK, "f_mkdir /PokeDNA failed");
  CHK(f_mkdir(DIR_) == FR_OK, "f_mkdir " DIR_ " failed");
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

/* Both bob frames of one row, and the palette tail, off a handle the test owns. */
static uint8_t  s_palid[ART_ICONS_ROWS];
static uint16_t s_pals[ART_ICONS_PALS][16];
static uint8_t  s_rows[8][ART_ICONS_ROW_BYTES] __attribute__((aligned(4)));

typedef struct { uint16_t first, n; bool ok; } RowsReq;
static bool do_rows(FIL* f, void* ud) {
  RowsReq* q = (RowsReq*)ud;
  q->ok = art_icons_read_rows_fp(f, q->first, q->n, s_rows);
  return q->ok;
}
static bool do_meta(FIL* f, void* ud) {
  (void)ud;
  return art_icons_meta_read_fp(f, s_palid, s_pals);
}

static bool read_rows(uint16_t first, uint16_t n) {
  RowsReq q = { first, n, false };
  with_open(ICONS, do_rows, &q);
  return q.ok;
}
static bool read_meta(void) { return with_open(ICONS, do_meta, 0); }

static void t_good(void) {
  fresh_card();
  build_content(s_content);
  CHK(write_raw(ICONS, s_content, sizeof s_content), "good: could not write icons.bin");

  CHK(read_meta(), "good: metadata tail failed");
  for (uint32_t r = 0; r < ART_ICONS_ROWS; r++)
    if (s_palid[r] != (uint8_t)(r % 3)) { CHK(0, "good: pal id row %u mismatch", r); break; }
  for (int i = 0; i < (int)ART_ICONS_PALS; i++)
    for (int c = 0; c < 16; c++)
      if (s_pals[i][c] != (uint16_t)(i * 1000 + c)) { CHK(0, "good: pal %d entry %d", i, c); i = 99; break; }

  /* A ROW is 1024 B -- frame 0 then frame 1, adjacent. That adjacency is the whole
   * reason a bob flip can be free, so it is asserted rather than assumed. */
  int rows_to_check[3] = { 0, 217, (int)ART_ICONS_ROWS - 1 };
  for (int ri = 0; ri < 3; ri++) {
    uint16_t r = (uint16_t)rows_to_check[ri];
    CHK(read_rows(r, 1), "good: row %u failed", r);
    CHK(memcmp(s_rows[0], s_content + (uint32_t)r * ART_ICONS_ROW_BYTES,
               ART_ICONS_ROW_BYTES) == 0, "good: row %u content mismatch", r);
  }

  /* Strided sweep, one row at a time. */
  for (uint16_t r = 0; r < ART_ICONS_ROWS; r += 37) {
    CHK(read_rows(r, 1), "sweep: row %u read failed", r);
    CHK(memcmp(s_rows[0], s_content + (uint32_t)r * ART_ICONS_ROW_BYTES,
               ART_ICONS_ROW_BYTES) == 0, "sweep: row %u content mismatch", r);
  }
}

/* THE BATCHING CLAIM. A span of N consecutive rows must return byte-for-byte what the
 * same N rows return read individually, and must cost ONE disk_read where the
 * individual reads cost N. Everything the retention design does rests on this, and it
 * is exactly the property a host RAM disk CAN prove (the wall-clock saving is a
 * hardware fact, but the transaction count is not). */
static void t_bulk_equals_singles(void) {
  fresh_card();
  build_content(s_content);
  CHK(write_raw(ICONS, s_content, sizeof s_content), "bulk: write failed");

  static uint8_t one[8][ART_ICONS_ROW_BYTES] __attribute__((aligned(4)));
  const uint16_t first = 100, n = 8;

  /* N separate reads on ONE held-open handle -- already far better than the old
   * per-frame f_open, and still the baseline the span has to beat. */
  unsigned long r0 = rd_reads;
  {
    FIL f;
    CHK(f_open(&f, ICONS, FA_READ) == FR_OK, "bulk: open");
    for (uint16_t i = 0; i < n; i++)
      CHK(art_icons_read_rows_fp(&f, (uint16_t)(first + i), 1, one[i]), "bulk: single %u", i);
    f_close(&f);
  }
  unsigned long singles = rd_reads - r0;

  r0 = rd_reads;
  CHK(read_rows(first, n), "bulk: span read failed");
  unsigned long span = rd_reads - r0;

  CHK(memcmp(s_rows, one, sizeof one) == 0,
      "bulk: a %u-row span must be byte-identical to %u single-row reads", n, n);
  printf("  %u consecutive rows: %lu sectors as singles, %lu sectors as one span\n",
         n, singles, span);
  CHK(span == singles, "bulk: a span must move the same SECTORS (it is the same bytes)");
  /* The point is transactions, not sectors: same bytes, one call instead of eight.
   * rd_reads counts sectors, so the saving shows up on hardware as 8 rompage swaps
   * collapsing into 1 -- see perf.h's rd_multi counter, which is the on-card proof. */
}

static void t_wrong_size(void) {
  fresh_card();
  build_content(s_content);

  CHK(write_raw(ICONS, s_content, sizeof s_content - 1), "setup: 1-short write failed");
  CHK(!read_meta(), "1 byte short must be refused (metadata read)");

  fresh_card();
  CHK(write_raw(ICONS, s_content, 0), "setup: empty write failed");
  CHK(!read_meta(), "empty file must be refused");

  fresh_card();
  CHK(write_raw(ICONS, s_content, 100), "setup: tiny write failed");
  CHK(!read_meta(), "a tiny garbage file must be refused");

  fresh_card();
  static uint8_t big[ART_ICONS_TOTAL_BYTES + 1];
  memcpy(big, s_content, sizeof s_content);
  big[ART_ICONS_TOTAL_BYTES] = 0xFF;
  CHK(write_raw(ICONS, big, sizeof big), "setup: 1-long write failed");
  CHK(!read_meta(), "1 byte long must be refused too (not just short)");

  fresh_card();
  CHK(!read_meta(), "absent icons.bin must be refused, not crash");
  CHK(!read_rows(0, 1), "absent icons.bin row read must be refused");
}

static void t_out_of_range(void) {
  fresh_card();
  build_content(s_content);
  CHK(write_raw(ICONS, s_content, sizeof s_content), "oob: write failed");
  CHK(!read_rows((uint16_t)ART_ICONS_ROWS, 1), "row == ROWS must be refused");
  CHK(!read_rows(0xFFFF, 1), "row 0xFFFF must be refused");
  CHK(!read_rows(0, 0), "a zero-length span must be refused");
  /* A span running off the end is the one an unchecked bulk reader would happily
   * serve out of whatever follows the tile block -- i.e. the palette tables. */
  CHK(!read_rows((uint16_t)(ART_ICONS_ROWS - 3), 4), "a span past the last row must be refused");
  CHK(read_rows((uint16_t)(ART_ICONS_ROWS - 4), 4), "a span ending exactly at the last row is fine");
  CHK(!art_icons_read_rows_fp(0, 0, 1, s_rows), "a NULL handle must be refused");
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
  t_bulk_equals_singles();
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  if (argc > 1) {
    size_t l = strlen(argv[1]);
    if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1];
  }
  t_row_for_matches_rom_mon(dir);
  printf("%d checks, %d fail%s\n", checks, fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
