/* source/icon_store.c -- the SINGLE owner of every icon byte in RAM -- driven over the
 * REAL lib/fatfs on a RAM disk, counting REAL disk_read sectors, and over Guy's real
 * cartridge dumps for the ROM rung.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_iconstore_test.c source/icon_store.c source/art_icons_cache.c \
 *      source/fastseek.c source/rom_mon.c source/rom_map.c source/log.c lib/fatfs/ff.c \
 *      lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/his
 *
 * WHY THIS TEST AND NOT tests/host_dexicons_test.c. That one counts RomReadFn calls --
 * the ICON RUNG's calls -- and every single one of its assertions is blind to the layer
 * that actually cost Guy his frame rate: the f_open directory walk, the FAT chain, the
 * sector count. On a 21-cell Pokedex page the multiplier between RomReadFn calls and
 * disk_read calls was measured at 2.5x (138 -> 252), and it could not see that at all;
 * nor could it see the icons.bin rung, which it never exercises. THIS test counts
 * SECTORS, at lib/fatfs's own disk_read, which is the number the hardware pays.
 *
 * The three mandates it holds to account, one section each:
 *   BULK      -- a fetch delivers BOTH bob frames in one transaction, so a flip is free;
 *   RETENTION -- a working set that fits the pool costs its rows ONCE, ever;
 *   HONESTY   -- a working set that does NOT fit is the cyclic-sweep pathology, and the
 *                number is printed rather than hidden, because that is the case the
 *                plan/pin step exists to fix.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "art_icons_extract.h"
#include "ff.h"
#include "icon_store.h"
#include "perf.h"
#include "ramdisk.h"
#include "rom_map.h"
#include "rom_mon.h"

static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* --- the two things icon_store.c needs from the cartridge that a host has not ------ */
#if PDNA_PERF
PerfSd    perf_sd;
PerfIcons perf_icons;
#endif
/* The GPIO motor freeze. On hardware this brackets every read icon_store issues (which
 * is itself a fix: box_oam.c's rungs each carried their own and art_fallbacks.c's
 * carried none). Here it only has to exist. */
void rumble_io_suspend(void) {}
void rumble_io_resume(void) {}

#define DIR_ "/PokeDNA/art"
#define ICONS DIR_ "/icons.bin"

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];
static uint8_t s_content[ART_ICONS_TOTAL_BYTES];

static void build_content(void) {
  for (uint32_t r = 0; r < ART_ICONS_ROWS; r++)
    for (uint32_t b = 0; b < ART_ICONS_ROW_BYTES; b++)
      s_content[r * ART_ICONS_ROW_BYTES + b] = (uint8_t)((r * 7 + b * 13) & 0xFF);
  uint8_t* ids = s_content + ART_ICONS_PAL_IDS_OFF;
  for (uint32_t r = 0; r < ART_ICONS_ROWS; r++) ids[r] = (uint8_t)(r % 3);
  uint8_t* pals = s_content + ART_ICONS_PALS_OFF;
  for (int i = 0; i < 3; i++)
    for (int c = 0; c < 16; c++) {
      uint16_t v = (uint16_t)(i * 1000 + c);
      pals[i * 32 + c * 2] = (uint8_t)v;
      pals[i * 32 + c * 2 + 1] = (uint8_t)(v >> 8);
    }
}

static bool write_raw(const char* path, const uint8_t* buf, uint32_t n) {
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
  if (n && (f_write(&f, buf, n, &bw) != FR_OK || bw != n)) { f_close(&f); return false; }
  return f_close(&f) == FR_OK;
}

/* A card with SOME ROOT CLUTTER, deliberately. The old design's cost was dominated by
 * f_open re-walking the directory tree per icon, and that cost scales with how full the
 * user's card root is (measured on real shapes: 5 / 20 / 35 sectors per 512 B frame at
 * 0 / 60 / 120 root entries). The clutter is what makes the held-handle win visible. */
static void fresh_card(int root_entries) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  icon_store_suspend();                       /* never hold a FIL across a re-mkfs */
  f_mount(0, "", 0);
  rd_init(8192);
  CHK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs");
  CHK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount");
  for (int i = 0; i < root_entries; i++) {
    char n[64];
    snprintf(n, sizeof n, "/a-fairly-long-clutter-name-%03d.dat", i);
    write_raw(n, (const uint8_t*)"x", 1);
  }
  CHK(f_mkdir("/PokeDNA") == FR_OK, "mkdir /PokeDNA");
  CHK(f_mkdir(DIR_) == FR_OK, "mkdir " DIR_);
}

static bool row_matches(const uint8_t* p, uint16_t r) {
  return p && memcmp(p, s_content + (uint32_t)r * ART_ICONS_ROW_BYTES,
                     ART_ICONS_ROW_BYTES) == 0;
}

/* ================= BULK: one fetch, both frames, one transaction ================== */
static void t_bulk(void) {
  fresh_card(60);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "bulk: write icons.bin");
  icon_store_reset(ICONS, 0);
  CHK(icon_store_rung() == ICON_RUNG_CACHE, "bulk: the cache rung must be chosen");

  unsigned long r0 = rd_reads;
  const uint8_t* p = icon_store_row(25);
  unsigned long cost = rd_reads - r0;
  CHK(row_matches(p, 25), "bulk: row 25 content");
  printf("  one COLD row (both bob frames), card root = 60 entries: %lu sectors\n", cost);
  /* The row is 1024 B = 2 sectors. Anything much above that is the directory walk the
   * held handle exists to delete -- the old per-frame path measured 20 sectors here,
   * for HALF the data. */
  CHK(cost <= 4, "bulk: a cold row must cost <= 4 sectors, not a directory walk (got %lu)", cost);

  /* Frame 1 is present WITHOUT a second fetch. This is the entire mechanism behind a
   * free bob flip, so it is asserted on bytes, not inferred. */
  CHK(memcmp(p + 512, s_content + 25 * ART_ICONS_ROW_BYTES + 512, 512) == 0,
      "bulk: frame 1 must arrive with frame 0");

  uint16_t pal[16];
  r0 = rd_reads;
  CHK(icon_store_pal(25, pal), "bulk: palette");
  CHK(rd_reads == r0, "bulk: a palette must never touch the card on the cache rung");
  CHK(pal[3] == (uint16_t)((25 % 3) * 1000 + 3), "bulk: palette contents");
}

/* ================= RETENTION: the party bob, which is the reported bug ============ */
static void t_retention_party(void) {
  fresh_card(60);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "party: write");
  icon_store_reset(ICONS, 0);

  /* Six arbitrary, non-adjacent rows -- a real party, not a contiguous span. */
  const uint16_t party[6] = { 3, 91, 150, 201, 330, 412 };
  CHK(icon_store_capacity() >= 6, "party: the pool must hold a full party (cap %u)",
      icon_store_capacity());

  unsigned long r0 = rd_reads;
  for (int i = 0; i < 6; i++) CHK(row_matches(icon_store_row(party[i]), party[i]), "party: paint %d", i);
  unsigned long first_paint = rd_reads - r0;

  /* 20 bob flips over the same six mons. This is what the user watches. */
  r0 = rd_reads;
  for (int flip = 0; flip < 20; flip++)
    for (int i = 0; i < 6; i++) {
      const uint8_t* p = icon_store_row(party[i]);
      CHK(row_matches(p, party[i]), "party: flip %d mon %d", flip, i);
    }
  unsigned long flips = rd_reads - r0;

  printf("  party of 6: first paint %lu sectors, then 20 bob flips = %lu sectors\n",
         first_paint, flips);
  CHK(flips == 0,
      "RETENTION: a 6-mon party bob must cost ZERO sectors after the first paint (got %lu)",
      flips);
  CHK(first_paint <= 6 * 4, "party: first paint stays bounded (got %lu)", first_paint);
}

/* ============ HONESTY: the case that does NOT fit, reported not hidden ============ */
static void t_oversized_working_set(void) {
  fresh_card(0);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "dex: write");
  icon_store_reset(ICONS, 0);

  uint16_t page[21];
  for (int i = 0; i < 21; i++) page[i] = (uint16_t)(40 + i * 5);   /* scattered, not a span */

  for (int i = 0; i < 21; i++) icon_store_row(page[i]);            /* first paint */
  unsigned long r0 = rd_reads;
  for (int i = 0; i < 21; i++) icon_store_row(page[i]);            /* one "flip" */
  unsigned long per_flip = rd_reads - r0;

  printf("  21-cell page against a %u-row pool: %lu sectors per flip "
         "(the cyclic-sweep pathology -- what the plan/pin step exists to fix)\n",
         icon_store_capacity(), per_flip);
  /* Deliberately asserted as a KNOWN, BOUNDED cost rather than papered over. It is
   * still ~20x better than the 420 sectors the old per-frame f_open path spent on the
   * same flip, but it is not zero, and pretending otherwise is how the previous cache
   * shipped a false claim in its own header. */
  CHK(per_flip <= 21 * 4,
      "dex: an oversized set must still be bounded at one row per cell (got %lu)", per_flip);
  CHK(per_flip > 0, "dex: an oversized set is NOT free -- if this ever passes, the "
                    "pool grew and this test's premise needs revisiting");
}

/* ================= failure handling and the session latch ======================== */
static void t_failures(void) {
  fresh_card(0);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "fail: write");
  icon_store_reset(ICONS, 0);

  CHK(icon_store_row(ART_ICONS_ROWS) == 0, "row == ROWS must be refused");
  CHK(icon_store_row(0xFFFF) == 0, "row 0xFFFF must be refused");

  /* A card that fails one read: the row must come back NULL and the slot must NOT be
   * left valid holding a partial fill. The retry proves the slot was invalidated
   * rather than left half-filled and trusted. */
  rd_fail_reads_after = 0;
  const uint8_t* bad = icon_store_row(77);
  rd_fail_reads_after = -1;
  CHK(bad == 0, "a failed read must return NULL");
  CHK(row_matches(icon_store_row(77), 77),
      "fill-then-validate: after a failed read the row must be RE-FETCHED, not served");

  /* THE EXTRACTION LATCH. FF_FS_LOCK is 0, so nothing but this call stops the store
   * holding a FIL across icons.bin being unlinked and renamed under it. */
  icon_store_suspend();
  CHK(icon_store_rung() == ICON_RUNG_NONE, "suspend must retire the rung");
  CHK(icon_store_row(3) == 0, "a suspended store must serve nothing");
  icon_store_reset(ICONS, 0);
  CHK(icon_store_rung() == ICON_RUNG_CACHE, "reset must bring the rung back");
  CHK(row_matches(icon_store_row(3), 3), "reset must serve again");

  /* A wrong-size icons.bin is not a rung: better no icons than icons off an unvalidated
   * file, and better still that the ROM rung below gets its turn. */
  fresh_card(0);
  CHK(write_raw(ICONS, s_content, sizeof s_content - 1), "fail: short write");
  icon_store_reset(ICONS, 0);
  CHK(icon_store_rung() == ICON_RUNG_NONE, "a wrong-size icons.bin must not become a rung");
  CHK(icon_store_row(3) == 0, "...and must serve nothing");
}

/* ================= the ROM rung, on Guy's real dumps ============================= */
typedef struct { FILE* f; long reads; } FileCtx;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  fc->reads++;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, fc->f) == len;
}

static void t_rom_rung(const char* dir, const char* name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s.gba", dir, name);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP ROM rung %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f, 0 };
  RomCtx rc; RomMon rm;
  if (!rom_open(&rc, file_read, &fc, (uint32_t)sz) || !rom_mon_open(&rm, &rc)) {
    printf("SKIP ROM rung %s (no GF header)\n", name); fclose(f); return;
  }

  fresh_card(0);                        /* no icons.bin at all -- the user's own case */
  icon_store_reset(0, &rm);
  CHK(icon_store_rung() == ICON_RUNG_ROM, "[%s] the ROM rung must be chosen", name);

  const uint16_t party[6] = { 3, 91, 150, 201, 330, 412 };
  fc.reads = 0;
  for (int i = 0; i < 6; i++) CHK(icon_store_row(party[i]) != 0, "[%s] rom paint %d", name, i);
  long paint = fc.reads;

  fc.reads = 0;
  for (int flip = 0; flip < 20; flip++)
    for (int i = 0; i < 6; i++) CHK(icon_store_row(party[i]) != 0, "[%s] rom flip", name);
  long flips = fc.reads;

  printf("  [%s] ROM rung, party of 6: first paint %ld RomReadFn calls, "
         "20 bob flips = %ld\n", name, paint, flips);
  CHK(flips == 0, "[%s] RETENTION on the ROM rung: a party bob must cost ZERO reads "
                  "after the first paint (got %ld)", name, flips);
  /* Per row: a verified locate (2 fields x 2 passes = 4) + BOTH frames read twice
   * (2). Six rows = 36. The old ladder spent 8 calls per SINGLE frame. */
  CHK(paint <= 6 * 6 + 2, "[%s] rom first paint stays at <= 6 reads/row (got %ld)", name, paint);

  /* Both frames really are there, off the ROM, in one row. */
  const uint8_t* p = icon_store_row(party[0]);
  CHK(p != 0, "[%s] row present", name);
  uint8_t direct[ART_ICONS_ROW_BYTES];
  RomMonLoc loc;
  CHK(rom_mon_locate_row_verified(&rm, party[0], &loc, 2, 0), "[%s] direct locate", name);
  CHK(rom_mon_icon_at(&rm, &loc, 0, direct) && rom_mon_icon_at(&rm, &loc, 1, direct + 512),
      "[%s] direct read", name);
  CHK(p && memcmp(p, direct, ART_ICONS_ROW_BYTES) == 0,
      "[%s] the stored row must be frame 0 || frame 1, exactly as rom_mon reads them", name);

  uint16_t pal[16];
  CHK(icon_store_pal(party[0], pal), "[%s] rom palette", name);
  CHK(icon_store_pal_id(party[0]) < 3, "[%s] rom palette bank in range", name);

  fclose(f);
}

int main(int argc, char** argv) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  if (argc > 1) { size_t l = strlen(argv[1]); if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1]; }

  t_bulk();
  t_retention_party();
  t_oversized_working_set();
  t_failures();
  t_rom_rung(dir, "Emerald");
  t_rom_rung(dir, "FireRed");

  icon_store_suspend();
  f_mount(0, "", 0);
  rd_free();
  printf("icon_store (bulk / retention / both rungs): %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
