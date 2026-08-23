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
 * The four mandates it holds to account, one section each:
 *   BULK      -- a fetch delivers BOTH bob frames in one transaction, so a flip is free,
 *                and a DECLARED page of consecutive rows is one transfer per pool-sized
 *                group instead of one per cell;
 *   RETENTION -- a working set that fits the pool costs its rows ONCE, ever;
 *   ORDER     -- a sweep sorts its misses by SOURCE OFFSET and only ever seeks forward,
 *                which is the half of the win that survives on a rung where nothing
 *                merges;
 *   HONESTY   -- a working set that does NOT fit is still the cyclic-sweep pathology in
 *                SECTORS (21 KiB does not fit in 6 KiB and no plan changes that), and
 *                the number is printed rather than hidden. What the plan cuts there is
 *                TRANSACTIONS, which is the size-independent half of the cost, and the
 *                test asserts both numbers separately so neither can hide behind the
 *                other.
 *
 * It also counts TRANSFERS (rd_read_calls), not just sectors. Sectors alone cannot tell
 * 21 single-row reads from 4 bulk ones -- both move 42 sectors -- and on the EZ-Flash
 * path the per-call cost (24 fixed halfword cart writes, IRQs off, plus a card-latency
 * poll per 4-sector chunk) is the part that does not scale with size.
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
  /* 32 KiB CLUSTERS, and that is not a detail. FatFs clips one f_read at the cluster
   * boundary (ff.c:3958-3962), so the cluster size is a hard ceiling on how much a
   * merged run can carry in one disk_read: at f_mkfs's default 2 KiB clusters a 6 KiB
   * bulk read is THREE transfers no matter how contiguous the rows are, and a test on
   * that geometry measures the harness instead of the design. A real SD card of the
   * size Guy uses formats at 32 KiB (measured in
   * docs/analysis-2026-08-23-io/measure-costmodel.md, csize=64 confirmed by f_mkfs), so
   * that is what this volume is. 8 MB gives 255 clusters -- room for a 451 KB icons.bin
   * plus 120 one-byte clutter files, each of which still eats a whole cluster. */
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 32768 };
  icon_store_suspend();                       /* never hold a FIL across a re-mkfs */
  f_mount(0, "", 0);
  rd_init(16384);
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

  /* UNDECLARED first -- the state before icon_store_plan() existed, kept as the control
   * the plan is measured against. 21 cells swept cyclically through a pool that holds
   * `cap` of them is the textbook LRU pathology: every access evicts the entry needed
   * 21 accesses later, so the hit rate is exactly zero and every flip re-reads
   * everything, one transfer per cell. */
  icon_store_plan(0, 0);
  for (int i = 0; i < 21; i++) icon_store_row(page[i]);            /* first paint */
  unsigned long r0 = rd_reads, c0 = rd_read_calls;
  for (int i = 0; i < 21; i++) icon_store_row(page[i]);            /* one "flip" */
  unsigned long per_flip = rd_reads - r0, calls_flip = rd_read_calls - c0;

  printf("  21-cell scattered page against a %u-row pool, UNDECLARED: %lu sectors / "
         "%lu transfers per flip (the cyclic-sweep pathology)\n",
         icon_store_capacity(), per_flip, calls_flip);
  /* Deliberately asserted as a KNOWN, BOUNDED cost rather than papered over. It is
   * still ~20x better than the 420 sectors the old per-frame f_open path spent on the
   * same flip, but it is not zero, and pretending otherwise is how the previous cache
   * shipped a false claim in its own header. */
  CHK(per_flip == 42, "dex: an undeclared oversized set costs both frames of all 21 "
                      "cells, every flip (got %lu sectors)", per_flip);
  CHK(calls_flip == 21, "dex: undeclared, that is ONE TRANSFER PER CELL -- the number "
                        "the plan exists to cut (got %lu)", calls_flip);

  /* DECLARED, same 21 scattered rows. The sectors CANNOT improve -- 21 KiB does not fit
   * in a 6 KiB pool and no amount of planning changes that -- but the TRANSACTIONS can,
   * and they are the expensive half: 24 fixed halfword cart writes with IRQs off plus a
   * card-latency poll each, all size-independent. The sweep fetches the plan in
   * pool-sized bulk groups instead of one row at a time. Nothing here is adjacent, so
   * no group merges; the win is bounded to the group structure, and the ORDER within
   * each group is now ascending by offset. */
  icon_store_plan(page, 21);
  for (int i = 0; i < 21; i++) icon_store_row(page[i]);
  r0 = rd_reads; c0 = rd_read_calls;
  for (int i = 0; i < 21; i++) CHK(row_matches(icon_store_row(page[i]), page[i]),
                                   "dex: declared flip cell %d", i);
  per_flip = rd_reads - r0; calls_flip = rd_read_calls - c0;
  printf("  ...the same page DECLARED: %lu sectors / %lu transfers per flip\n",
         per_flip, calls_flip);
  CHK(per_flip == 42, "dex: declaring cannot shrink 21 KiB into a 6 KiB pool -- the "
                      "sectors must be unchanged (got %lu)", per_flip);
  CHK(calls_flip <= 21, "dex: a declared sweep must NEVER cost more transfers than the "
                        "undeclared one (got %lu vs 21)", calls_flip);
  CHK(icon_store_plan_resident() == false,
      "dex: a 21-row plan against a 6-row pool is NOT resident, and must say so -- this "
      "is the animation gate, and a wrong `true` here is a bob at 42 sectors a flip");
}

/* ====== BULK, DECLARED: the contiguous dex page, which is the common case ========= */
static void t_plan_contiguous_page(void) {
  fresh_card(0);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "plan: write");
  icon_store_reset(ICONS, 0);
  unsigned cap = icon_store_capacity();

  /* The Pokedex's DEFAULT filter + No. sort. National 1-251 map 1:1 and monotonically
   * onto icons.bin rows 1-251, so 12 of the 18 full pages are one perfectly contiguous
   * 21-row span. This is that shape. */
  uint16_t page[21];
  for (int i = 0; i < 21; i++) page[i] = (uint16_t)(60 + i);

  unsigned long c0 = rd_read_calls, r0 = rd_reads;
  rd_read_back = 0; rd_read_last = 0;
  icon_store_plan(page, 21);
  for (int i = 0; i < 21; i++) CHK(row_matches(icon_store_row(page[i]), page[i]),
                                   "plan: contiguous cold cell %d", i);
  unsigned long cold_calls = rd_read_calls - c0, cold_sect = rd_reads - r0;
  unsigned long cold_back = rd_read_back;

  c0 = rd_read_calls; r0 = rd_reads;
  icon_store_plan(page, 21);
  for (int i = 0; i < 21; i++) CHK(row_matches(icon_store_row(page[i]), page[i]),
                                   "plan: contiguous flip cell %d", i);
  unsigned long flip_calls = rd_read_calls - c0, flip_sect = rd_reads - r0;

  printf("  21-cell CONTIGUOUS page, %u-row pool: cold %lu transfers / %lu sectors, "
         "flip %lu / %lu (was 21 / 42 before the plan)\n",
         cap, cold_calls, cold_sect, flip_calls, flip_sect);

  /* THE NUMBER THIS STEP EXISTS FOR. Consecutive rows are consecutive bytes, so a group
   * of `cap` of them is ONE f_read of cap*1024 B -- one disk_read, one set of 24 fixed
   * cart writes -- instead of `cap` separate transactions. 21 rows therefore cost
   * ceil(21/cap) transfers, not 21. */
  {
    /* ceil(21/cap) pool-sized GROUPS, each one f_read -- PLUS at most one extra per
     * group, because FatFs clips a read at the cluster boundary (ff.c:3958-3962) and a
     * cap-row group (12 sectors at cap=6) is far smaller than one 64-sector cluster, so
     * it can straddle at most one boundary. That upper bound is 2x the ideal and still
     * a 2x cut on the 21 this page used to cost; the measured figure is at the bottom
     * of the range. */
    unsigned long groups = (21 + cap - 1) / cap;
    CHK(s_fs.csize >= 2 * cap,
        "plan: this test's premise is a card whose cluster (%u sectors) is bigger than "
        "one bulk group (%u sectors) -- otherwise it measures f_mkfs, not the sweep",
        s_fs.csize, 2 * cap);
    CHK(cold_calls >= groups && cold_calls <= 2 * groups,
        "BULK: a contiguous 21-row page must cost between ceil(21/%u) = %lu and %lu "
        "transfers -- one per pool-sized group, plus cluster splits (got %lu)",
        cap, groups, 2 * groups, cold_calls);
    CHK(cold_calls * 4 <= 21,
        "BULK: and that must be at least a 4x cut on the 21 transfers the same page "
        "cost one-cell-at-a-time (got %lu)", cold_calls);
    CHK(flip_calls == cold_calls,
        "BULK: a flip re-walks the same plan and must cost the same (got %lu vs %lu)",
        flip_calls, cold_calls);
  }
  CHK(cold_sect == 42, "plan: 21 rows is 42 sectors, both bob frames, no waste and no "
                       "re-read (got %lu)", cold_sect);
  CHK(cold_back == 0,
      "plan: a cold sweep of an ascending page must only ever seek FORWARD -- a backward "
      "seek is the one case FatFs restarts the cluster walk from the head of the chain "
      "(got %lu backward)", cold_back);

  /* Every transfer is 512-ALIGNED and multi-sector. An unaligned bulk read is routed
   * through lib/fatfs/diskio.c's fc_bounce and capped at 4 sectors per driver call, so
   * an unaligned 8 KiB read is 4 full driver calls plus 8 KiB of extra memcpy where an
   * aligned one is 1. icons.bin rows are 1024 B, so alignment is free -- as long as the
   * sweep never splits a row, which is what this ratio proves. */
  CHK(cold_sect / cold_calls >= 2,
      "plan: every transfer must carry at least one whole row (2 sectors)");
}

/* ============ the sweep's ORDER, which is what makes a seek cheap ================= */
static void t_plan_sorted_order(void) {
  fresh_card(0);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "order: write");
  icon_store_reset(ICONS, 0);

  /* A-Z sort / type view: the plan arrives in an order that has nothing to do with the
   * row axis. Declared DESCENDING here, which is the worst case the paint loop could
   * hand over -- every fetch in plan order would be a backward seek. */
  uint16_t page[21];
  for (int i = 0; i < 21; i++) page[i] = (uint16_t)(400 - i * 17);

  rd_read_back = 0; rd_read_last = 0;
  unsigned long c0 = rd_read_calls, r0 = rd_reads;
  icon_store_plan(page, 21);
  for (int i = 0; i < 21; i++) CHK(row_matches(icon_store_row(page[i]), page[i]),
                                   "order: cell %d content", i);
  unsigned long calls = rd_read_calls - c0, sect = rd_reads - r0;
  printf("  21-cell DESCENDING page (the A-Z / type-view shape): %lu transfers / "
         "%lu sectors, %lu backward seeks\n", calls, sect, rd_read_back);

  /* Nothing merges here, so the sweep degenerates to one read per row -- which is the
   * documented graceful degeneration, and is never worse than the undeclared path. What
   * it must NOT do is seek backward WITHIN a group: the misses are sorted ascending by
   * source offset before any of them is read. Group boundaries themselves step back
   * (group 2 starts below group 1's end, because the plan is descending), so the bound
   * is one backward step per group, not zero. */
  CHK(calls <= 21, "order: a scattered declared page must not cost more than one "
                   "transfer per row (got %lu)", calls);
  CHK(sect == 42, "order: 21 rows, both frames, once each (got %lu)", sect);
  {
    unsigned cap = icon_store_capacity();
    unsigned long groups = (21 + cap - 1) / cap;
    CHK(rd_read_back <= groups - 1,
        "ASCENDING-OFFSET ORDER: at most one backward step per group boundary (%lu "
        "groups, got %lu backward seeks) -- more means the sort is not happening",
        groups, rd_read_back);
  }
}

/* ====== a failed read inside a MERGED run leaves NOTHING marked valid ============= */
static void t_plan_run_failure(void) {
  fresh_card(0);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "runfail: write");
  icon_store_reset(ICONS, 0);

  uint16_t page[21];
  for (int i = 0; i < 21; i++) page[i] = (uint16_t)(100 + i);   /* one contiguous run */

  /* Kill the card at the first read of the sweep. The whole run is one f_read, and a
   * short read does not say WHERE the bytes stopped -- so not one slot of it may be
   * served. Anything else paints half a Pokemon under another one's name. */
  rd_fail_reads_after = 0;
  int res = icon_store_plan(page, 21);
  rd_fail_reads_after = -1;
  CHK(res == 0, "runfail: a plan whose sweep failed must report ZERO rows resident "
                "(got %d)", res);
  CHK(icon_store_plan_resident() == false, "runfail: ...and must not be `resident`");
  for (int i = 0; i < 21; i++) {
    /* Serve nothing from the failed run. Asking again is a genuine re-fetch, and it
     * must produce the RIGHT bytes -- which is the proof the slot was never validated
     * with a partial fill in it. */
    ;
  }
  for (int i = 0; i < 21; i++)
    CHK(row_matches(icon_store_row(page[i]), page[i]),
        "fill-then-validate over a RUN: row %u must be re-fetched clean after the "
        "failure, never served half-filled", page[i]);
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
  t_plan_contiguous_page();
  t_plan_sorted_order();
  t_plan_run_failure();
  t_failures();
  t_rom_rung(dir, "Emerald");
  t_rom_rung(dir, "FireRed");

  icon_store_suspend();
  f_mount(0, "", 0);
  rd_free();
  printf("icon_store (bulk / retention / both rungs): %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
