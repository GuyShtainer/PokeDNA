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

/* ---- the g_pc borrow, modelled exactly (pdna_main.c owns the real one) -----------
 *
 * Tier B's whole safety argument is two sentences -- "refuse when the PC is dirty" and
 * "re-derive g_pc from g_save on release" -- so the harness has to be able to express
 * both, or the tests below prove nothing about the property that matters. It adds a
 * third thing the cartridge does not need: on release the donor is REPAINTED with a
 * byte pattern that appears nowhere in icons.bin's generated content, so a slot the
 * store failed to invalidate hands back 0xA7s and every content check catches it. */
#define HOST_PC_BYTES 35712u
static uint8_t  s_pc[HOST_PC_BYTES];
static bool     s_pc_dirty = false, s_pc_held = false;
static unsigned s_pc_acquires = 0, s_pc_releases = 0;

uint8_t* app_arena_acquire(uint32_t need) {
  if (s_pc_held || need > HOST_PC_BYTES) return NULL;
  if (s_pc_dirty) return NULL;            /* unsaved box moves live ONLY here */
  s_pc_held = true;
  s_pc_acquires++;
  return s_pc;
}
void app_arena_release(void) {
  if (!s_pc_held) return;
  s_pc_held = false;
  s_pc_releases++;
  memset(s_pc, 0xA7, sizeof s_pc);        /* stands in for gen3_read_pc_storage */
}
bool app_arena_held(void) { return s_pc_held; }

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
typedef struct {
  FILE* f;
  long  reads;
  /* The ROM rung's reads do NOT go through lib/fatfs here, so the sweep's ORDER has to
   * be watched at this level instead of at disk_read. `back` counts reads that started
   * at a lower file offset than the previous one -- the only case FatFs restarts a
   * cluster walk from the head of the chain, and therefore the property the sort exists
   * to produce. `fail_in` kills the Nth read, for the fill-then-validate case. */
  long  back, prev;
  long  fail_in;
  /* A cart that READS SUCCESSFULLY AND HANDS BACK DIFFERENT BYTES EACH TIME -- which is
   * the exact failure the double-read verify exists for and the one a single read
   * cannot see. Every read comes back with one byte stirred by a counter, so no two
   * passes ever agree. */
  int   garble;
} FileCtx;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  fc->reads++;
  if (fc->fail_in > 0 && --fc->fail_in == 0) return false;
  if ((long)off < fc->prev) fc->back++;
  fc->prev = (long)off;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  if (fc->garble && len) ((uint8_t*)dst)[0] ^= (uint8_t)fc->reads;
  return true;
}

static void t_rom_rung(const char* dir, const char* name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s.gba", dir, name);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP ROM rung %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f, 0, 0, 0, 0, 0 };
  RomCtx rc; RomMon rm;
  if (!rom_open(&rc, file_read, &fc, (uint32_t)sz) || !rom_mon_open(&rm, &rc)) {
    printf("SKIP ROM rung %s (no GF header)\n", name); fclose(f); return;
  }

  fresh_card(0);                        /* no icons.bin at all -- the user's own case */
  fc.reads = 0;
  icon_store_reset(0, &rm);
  long setup = fc.reads;
  unsigned cap = icon_store_capacity();
  CHK(icon_store_rung() == ICON_RUNG_ROM, "[%s] the ROM rung must be chosen", name);

  /* THE ONE-TIME INDEX LOAD. Three tables, each read twice back to back and accepted
   * only when the two passes agree, plus the 3 palettes the same way: 2 + 2 + 2 + 3*2
   * = 12 RomReadFn calls, once per registration. Bounded is the point -- an unbounded
   * "load everything" at boot is a screen that does not draw. */
  printf("  [%s] ROM index load: %ld RomReadFn calls, pool now %u rows\n",
         name, setup, cap);
  CHK(setup == 12, "[%s] the verified index load must be exactly 12 reads -- 2 per "
                   "table x 3 tables, plus 2 per palette x 3 (got %ld)", name, setup);
  CHK(cap == 4, "[%s] the ROM carve is 1,760 B of table + 4 rows (got %u)", name, cap);

  /* PER-ICON LOCATE COST IS NOW ZERO. Before the tables, every single fetch re-read the
   * icon pointer and the palette id, each twice to verify: 4 of the 6 reads a row cost.
   * A row is now 2 reads -- the payload, and the payload again to verify it, which
   * STAYS because the EZ read path can still return success holding garbage. */
  const uint16_t party[6] = { 3, 91, 150, 201, 330, 412 };
  fc.reads = 0;
  CHK(icon_store_row(party[0]) != 0, "[%s] rom single row", name);
  long one_row = fc.reads;
  CHK(one_row == 2, "[%s] LOCATE IS FREE: one cold ROM row must cost exactly 2 reads -- "
                    "the 1024 B payload and its verify re-read, and NO locate (got %ld)",
      name, one_row);

  const uint16_t party4[4] = { 11, 91, 150, 201 };
  icon_store_plan(0, 0);
  fc.reads = 0;
  for (int i = 0; i < 4; i++) CHK(icon_store_row(party4[i]) != 0, "[%s] rom paint %d", name, i);
  long paint = fc.reads;

  fc.reads = 0;
  for (int flip = 0; flip < 20; flip++)
    for (int i = 0; i < 4; i++) CHK(icon_store_row(party4[i]) != 0, "[%s] rom flip", name);
  long flips = fc.reads;

  printf("  [%s] ROM rung, 4 mons (the pool): first paint %ld RomReadFn calls, "
         "20 bob flips = %ld\n", name, paint, flips);
  CHK(flips == 0, "[%s] RETENTION on the ROM rung: a working set that FITS must cost "
                  "ZERO reads after the first paint (got %ld)", name, flips);
  CHK(paint == 4 * 2, "[%s] rom first paint is exactly 2 reads/row, no locate (got %ld)",
      name, paint);

  /* A 6-mon party no longer fits the ROM carve, and the store must SAY so rather than
   * quietly thrashing -- this is the animation gate. The cost is real and is the price
   * of the index table; the borrowed tier is what buys it back. */
  {
    int res = icon_store_plan(party, 6);
    CHK(res == (int)cap, "[%s] a 6-row plan against a %u-row pool fills %u and reports "
                         "it (got %d)", name, cap, cap, res);
    CHK(icon_store_plan_resident() == false,
        "[%s] ...and must NOT claim residency for the two rows it could not hold", name);
    for (int i = 0; i < 6; i++)
      CHK(icon_store_row(party[i]) != 0,
          "[%s] every row of an over-sized plan must still PAINT -- short does not mean "
          "blank (row %u)", name, party[i]);
  }

  /* THE SWEEP'S ORDER ON THE ROM RUNG. Nothing merges here -- 0 of 439 adjacent table
   * rows have adjacent blobs on any of Guy's dumps -- so the whole win of a sort is
   * that every read inside a group moves FORWARD. Declared in an order chosen to be
   * maximally hostile: descending by row, which is unrelated to file order anyway. */
  {
    uint16_t page[12];
    for (int i = 0; i < 12; i++) page[i] = (uint16_t)(300 - i * 11);
    icon_store_plan(0, 0);
    for (int i = 0; i < 12; i++) icon_store_row(page[i]);      /* warm nothing useful */
    icon_store_reset(0, &rm);                                  /* cold pool, cold order */
    fc.reads = 0; fc.back = 0; fc.prev = 0;
    icon_store_plan(page, 12);
    for (int i = 0; i < 12; i++) CHK(icon_store_row(page[i]) != 0, "[%s] order cell %d",
                                     name, i);
    long groups = (12 + (long)cap - 1) / (long)cap;
    printf("  [%s] 12-row DESCENDING plan: %ld reads, %ld backward seeks (%ld groups)\n",
           name, fc.reads, fc.back, groups);
    /* Two reads per row (payload + verify) means the verify re-read is itself a
     * zero-distance "backward" step at the same offset -- which does not count, since
     * `back` is a strict comparison. What is left is one step per group boundary. */
    CHK(fc.back <= groups - 1,
        "[%s] ASCENDING-OFFSET ORDER on the ROM rung: at most one backward seek per "
        "group boundary (%ld groups, got %ld)", name, groups, fc.back);
    CHK(fc.reads == 24, "[%s] 12 rows x (payload + verify), no locate (got %ld)",
        name, fc.reads);
  }

  /* A FAILED READ MID-SWEEP VALIDATES NOTHING. Kill the very first payload read of a
   * plan's sweep and the whole group must come back empty rather than half-trusted. */
  {
    uint16_t page[4] = { 30, 31, 32, 33 };
    icon_store_reset(0, &rm);
    fc.fail_in = 1;
    int res = icon_store_plan(page, 4);
    fc.fail_in = 0;
    CHK(res < 4, "[%s] a plan whose sweep hit a dead read must report short (got %d)",
        name, res);
    for (int i = 0; i < 4; i++)
      CHK(icon_store_row(page[i]) != 0,
          "[%s] fill-then-validate: row %u must be RE-FETCHED clean after the failure",
          name, page[i]);
  }

  /* A CART THAT READS SUCCESSFULLY AND LIES. Every read comes back changed, so no two
   * passes of the index load ever agree. The load must FAIL CLOSED -- and the rung must
   * survive it: rom_locate falls back to the per-icon verified lookup and the pool takes
   * back the 1,760 B the table would have held. Slow and hazardous, but never dark, and
   * the log says which session it was. */
  {
    fc.garble = 1;
    icon_store_reset(0, &rm);
    fc.garble = 0;
    CHK(icon_store_rung() == ICON_RUNG_ROM,
        "[%s] a failed index load must NOT retire the rung", name);
    CHK(icon_store_capacity() == 6,
        "[%s] ...and the pool takes the table's 1,760 B back as rows (got %u)",
        name, icon_store_capacity());
    CHK(icon_store_row(party[1]) != 0,
        "[%s] ...and the per-icon locate fallback must still serve a row", name);
    CHK(icon_store_pal_id(party[1]) < 3,
        "[%s] ...and still find its palette bank", name);
  }

  icon_store_reset(0, &rm);
  CHK(icon_store_capacity() == 4, "[%s] a clean re-registration reloads the tables", name);

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

  /* The palettes came off the ROM at reset, verified, all three at once -- so asking
   * for one must not touch the ROM at all, on any row, ever again. */
  uint16_t pal[16];
  fc.reads = 0;
  CHK(icon_store_pal(party[0], pal), "[%s] rom palette", name);
  CHK(icon_store_pal_id(party[0]) < 3, "[%s] rom palette bank in range", name);
  CHK(fc.reads == 0, "[%s] a palette must never touch the ROM after the index load "
                     "(got %ld reads)", name, fc.reads);

  /* And the resident tables must AGREE with the per-icon path they replaced, on every
   * one of the 440 rows -- offset for offset and bank for bank. This is the check that
   * would catch a byte-order slip in the in-place widening, which would otherwise show
   * up on hardware as the wrong Pokemon under the right name. */
  {
    int mismatch = 0, unusable = 0;
    for (uint16_t r = 0; r < ART_ICONS_ROWS; r++) {
      RomMonLoc l;
      uint8_t id = icon_store_pal_id(r);
      if (!rom_mon_locate_row_verified(&rm, r, &l, 2, 0)) { unusable++; continue; }
      if (id != l.pal) mismatch++;
    }
    CHK(mismatch == 0, "[%s] the resident palette table must match rom_mon's own lookup "
                       "on all 440 rows (%d differ)", name, mismatch);
    CHK(unusable == 0, "[%s] every row of a real GF ROM must locate (%d did not)",
        name, unusable);
  }

  /* ---- TIER B ON THE RUNG THAT NEEDS IT ------------------------------------------
   * This is the regression payment, measured on Guy's own dump. Tier A here is FOUR
   * rows, because the resident 440-entry offset table costs 1,760 B of the same 6 KiB
   * pool -- so a party of SIX could not be resident, plan_resident() honestly answered
   * "no", and the animation gate left the party static. That is the bug as reported.
   * With the borrow the same party is fully resident and a flip is zero reads. */
  CHK(icon_store_capacity() == 4, "[%s] Tier A on the ROM rung is 4 rows", name);
  CHK(icon_store_plan(party, 6) < 6,
      "[%s] BEFORE the borrow: a 6-mon party does NOT fit 4 slots", name);
  CHK(icon_store_plan_resident() == false,
      "[%s] BEFORE the borrow: the party is not resident -> anim off (the reported bug)",
      name);

  CHK(icon_store_borrow(true), "[%s] the ROM rung must be able to borrow", name);
  CHK(icon_store_capacity() == 4 + 32, "[%s] Tier B adds 32 rows (got %u)",
      name, icon_store_capacity());
  CHK(icon_store_plan(party, 6) == 6, "[%s] AFTER the borrow: all 6 party rows resident",
      name);
  CHK(icon_store_plan_resident(),
      "[%s] REGRESSION REPAID (rom): a 6-mon party IS resident -> anim ON", name);
  {
    long f0 = fc.reads;
    unsigned long c0 = rd_read_calls;
    for (int flip = 0; flip < 20; flip++)
      for (int i = 0; i < 6; i++)
        CHK(icon_store_row(party[i]) != 0, "[%s] rom party flip %d/%d", name, flip, i);
    CHK(fc.reads == f0 && rd_read_calls == c0,
        "[%s] 20 party bob flips on the ROM rung must be 0 ROM reads / 0 disk_read calls "
        "(got %ld / %lu)", name, fc.reads - f0, rd_read_calls - c0);
  }

  /* A whole 21-cell dex page fits too -- 21 rows against 36 slots -- so the dex bob is
   * free here as well. The ROM rung never MERGES (0 of 439 adjacent pairs on any of the
   * three dumps), so the cold cost is one transfer per row and that is the honest
   * number; what the borrow buys is that the page stays resident afterwards. */
  {
    uint16_t page[21];
    for (int i = 0; i < 21; i++) page[i] = (uint16_t)(100 + i);
    long f0 = fc.reads;
    CHK(icon_store_plan(page, 21) == 21, "[%s] a 21-cell page fits Tier B", name);
    CHK(icon_store_plan_resident(), "[%s] ...and is resident -> dex anim ON", name);
    printf("  [%s] borrowed 21-cell page: %ld RomReadFn calls cold (2 per row, no merge)\n",
           name, fc.reads - f0);
    f0 = fc.reads;
    for (int i = 0; i < 21; i++) CHK(icon_store_row(page[i]) != 0, "[%s] dex flip cell %d",
                                     name, i);
    CHK(fc.reads == f0, "[%s] a flip over a resident 21-row page is 0 ROM reads (got %ld)",
        name, fc.reads - f0);
  }

  icon_store_borrow(false);
  CHK(!app_arena_held(), "[%s] the ROM rung must give the arena back too", name);
  CHK(icon_store_capacity() == 4, "[%s] ...and fall back to 4 rows", name);

  fclose(f);
}

/* ================= TIER B: the borrow, and the regression it repays ==============
 *
 * The ROM index table (the previous step) dropped Tier A from 6 rows to 4 on the ROM
 * rung, which is smaller than a party. That is a real, documented regression on exactly
 * the screens Guy reported as broken, and the borrow is the payment. These sections
 * assert the payment landed -- on BOTH rungs, in transfers and in sectors -- and that
 * the memory always goes back.
 */
static void t_borrow_cache(void) {
  fresh_card(0);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "borrow: write");
  icon_store_reset(ICONS, 0);
  unsigned tierA = icon_store_capacity();
  CHK(tierA == 6, "borrow: the cache rung starts at 6 Tier A rows (got %u)", tierA);
  CHK(icon_store_borrowed() == false, "borrow: nothing is borrowed at reset");

  CHK(icon_store_borrow(true), "borrow: the cache rung must be able to borrow");
  CHK(icon_store_borrowed(), "borrow: ...and must say so");
  CHK(icon_store_capacity() == tierA + 32,
      "borrow: Tier B adds 32 rows (got %u)", icon_store_capacity());

  /* (1) THE PARTY, which is the reported bug. Six scattered rows, fully resident, and
   *     twenty bob flips that must not touch the card at all. */
  const uint16_t party[6] = { 3, 91, 150, 201, 330, 412 };
  CHK(icon_store_plan(party, 6) == 6, "borrow: all 6 party rows resident after the plan");
  CHK(icon_store_plan_resident(), "borrow: a 6-row party must be RESIDENT -> anim on");
  unsigned long c0 = rd_read_calls, s0 = rd_reads;
  for (int f = 0; f < 20; f++)
    for (int i = 0; i < 6; i++)
      CHK(row_matches(icon_store_row(party[i]), party[i]), "borrow: party flip %d/%d", f, i);
  CHK(rd_read_calls == c0 && rd_reads == s0,
      "REGRESSION REPAID (cache): 20 party bob flips must be 0 transfers / 0 sectors "
      "(got %lu / %lu)", rd_read_calls - c0, rd_reads - s0);

  /* (2) A WHOLE 21-CELL DEX PAGE, contiguous, cold. 21 rows now FIT, so the flip is
   *     free -- and the cold paint is bounded by ICON_BULK_MAX_ROWS (8 rows per
   *     transfer) and by the one Tier A/Tier B seam, which is 3 transfers, not 21. */
  uint16_t page[21];
  for (int i = 0; i < 21; i++) page[i] = (uint16_t)(100 + i);
  c0 = rd_read_calls; s0 = rd_reads;
  CHK(icon_store_plan(page, 21) == 21, "borrow: all 21 dex rows resident");
  CHK(icon_store_plan_resident(), "borrow: a 21-cell page must be RESIDENT -> anim on");
  for (int i = 0; i < 21; i++)
    CHK(row_matches(icon_store_row(page[i]), page[i]), "borrow: dex cold cell %d", i);
  unsigned long cold_calls = rd_read_calls - c0, cold_sec = rd_reads - s0;
  printf("  borrowed 21-cell CONTIGUOUS page: cold %lu transfers / %lu sectors\n",
         cold_calls, cold_sec);
  CHK(cold_calls <= 3, "borrow: a contiguous 21-row page is 3 transfers (8+8 rows, "
                       "broken once at the tier seam), not 21 (got %lu)", cold_calls);
  CHK(cold_sec == 42, "borrow: 21 rows are 42 sectors, no more (got %lu)", cold_sec);

  c0 = rd_read_calls; s0 = rd_reads;
  for (int f = 0; f < 20; f++)
    for (int i = 0; i < 21; i++)
      CHK(row_matches(icon_store_row(page[i]), page[i]), "borrow: dex flip %d/%d", f, i);
  CHK(rd_read_calls == c0 && rd_reads == s0,
      "borrow: 20 dex bob flips over a resident page must be 0 transfers / 0 sectors "
      "(got %lu / %lu)", rd_read_calls - c0, rd_reads - s0);

  /* (3) A ONE-STEP SCROLL. 20 of the 21 rows are already here; only the arriving one
   *     may move, and it is one transfer. This is the sliding window the pin policy
   *     exists to give, and it only becomes visible once the page fits. */
  uint16_t page2[21];
  for (int i = 0; i < 21; i++) page2[i] = (uint16_t)(101 + i);
  c0 = rd_read_calls; s0 = rd_reads;
  CHK(icon_store_plan(page2, 21) == 21, "borrow: scrolled page resident");
  for (int i = 0; i < 21; i++)
    CHK(row_matches(icon_store_row(page2[i]), page2[i]), "borrow: scrolled cell %d", i);
  printf("  borrowed one-step scroll: %lu transfers / %lu sectors\n",
         rd_read_calls - c0, rd_reads - s0);
  CHK(rd_read_calls - c0 <= 1,
      "borrow: a one-step scroll must re-read ONE row, not the page (got %lu transfers)",
      rd_read_calls - c0);

  /* (4) GIVING IT BACK. The rows that lived in Tier B must not survive as hits: their
   *     bytes are the user's PC storage again the instant app_arena_release() returns,
   *     and the harness repaints the donor to prove a stale hit would be caught. */
  unsigned rel0 = s_pc_releases;
  icon_store_borrow(false);
  CHK(icon_store_borrowed() == false, "borrow: release must clear the flag");
  CHK(s_pc_releases == rel0 + 1, "borrow: release must hand the arena back exactly once");
  CHK(app_arena_held() == false, "borrow: the donor must be free after a release");
  CHK(icon_store_capacity() == tierA, "borrow: capacity must fall back to Tier A (got %u)",
      icon_store_capacity());
  CHK(icon_store_plan_count() == 0, "borrow: a release retires the live plan");
  CHK(icon_store_plan_resident() == false, "borrow: ...so nothing is 'resident' after it");
  /* page[10] lived in a Tier B slot. Re-reading it must go to the card and come back
   * with icons.bin's bytes -- never the 0xA7 the released donor now holds. */
  CHK(row_matches(icon_store_row(page[10]), page[10]),
      "borrow: a Tier B row must be RE-READ after the release, never served stale");
}

/* Every exit path gives the memory back -- and a leak is DETECTABLE, because a leaked
 * borrow means g_pc holds icon tiles instead of the user's boxes. */
static void t_borrow_leak(void) {
  fresh_card(0);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "leak: write");
  icon_store_reset(ICONS, 0);

  /* Idempotent both ways: a screen that acquires twice must not acquire twice, and the
   * unconditional release pdna_main.c fires after every nav case must be a no-op when
   * the screen already released. That pair is what makes the backstop safe to call
   * blindly, which is the only reason it can be unconditional. */
  unsigned a0 = s_pc_acquires, r0 = s_pc_releases;
  CHK(icon_store_borrow(true), "leak: first acquire");
  CHK(icon_store_borrow(true), "leak: second acquire is the same borrow");
  CHK(s_pc_acquires == a0 + 1, "leak: two acquires must take the arena ONCE (got %u)",
      s_pc_acquires - a0);
  icon_store_borrow(false);
  icon_store_borrow(false);
  CHK(s_pc_releases == r0 + 1, "leak: two releases must give it back ONCE (got %u)",
      s_pc_releases - r0);
  CHK(!app_arena_held(), "leak: the arena is free");

  /* A reset must not be able to strand the donor. app_icon_cache_resolve() runs on ROM
   * registration and after an extraction, which are both reachable from screens; if it
   * could leave 33 KB of icon rows sitting in g_pc, the next PC write would commit them. */
  CHK(icon_store_borrow(true), "leak: borrow before a reset");
  icon_store_reset(ICONS, 0);
  CHK(!app_arena_held(), "leak: icon_store_reset must release the borrow");
  CHK(!icon_store_borrowed(), "leak: ...and clear its own flag");

  /* Same for a suspend, which is the art-extraction screen about to rewrite icons.bin. */
  CHK(icon_store_borrow(true), "leak: borrow before a suspend");
  icon_store_suspend();
  CHK(!app_arena_held(), "leak: icon_store_suspend must release the borrow");
  icon_store_reset(ICONS, 0);
}

/* THE REFUSAL, which is not an error path -- it is what a user with an unsaved box move
 * gets, and it must degrade to a static frame rather than to a blank one or to thrash. */
static void t_borrow_dirty(void) {
  fresh_card(0);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "dirty: write");
  icon_store_reset(ICONS, 0);

  s_pc_dirty = true;
  CHK(icon_store_borrow(true) == false, "dirty: the borrow MUST refuse an unsaved PC");
  CHK(!icon_store_borrowed() && !app_arena_held(),
      "dirty: a refused borrow must not half-take the arena");
  CHK(icon_store_capacity() == 6, "dirty: capacity stays Tier A after a refusal");

  /* Seven rows against six slots: honestly NOT resident, so the gate says no and the
   * screen keeps a static frame -- but every row must still PAINT. "No animation" and
   * "no icon" are different outcomes and only one of them is acceptable. */
  const uint16_t seven[7] = { 5, 6, 7, 8, 9, 10, 11 };
  icon_store_plan(seven, 7);
  CHK(icon_store_plan_resident() == false,
      "dirty: 7 rows against 6 slots is NOT resident -> anim off, static frame");
  for (int i = 0; i < 7; i++)
    CHK(row_matches(icon_store_row(seven[i]), seven[i]),
        "dirty: row %d must still paint with the borrow refused", i);

  s_pc_dirty = false;
  CHK(icon_store_borrow(true), "dirty: a saved PC lends again");
  CHK(icon_store_plan(seven, 7) == 7, "dirty: ...and then all 7 fit");
  CHK(icon_store_plan_resident(), "dirty: ...and the gate flips to yes");
  icon_store_borrow(false);
}

/* The animation gate's two edge cases, pinned so nobody "simplifies" them apart. */
static void t_plan_edges(void) {
  fresh_card(0);
  build_content();
  CHK(write_raw(ICONS, s_content, sizeof s_content), "edges: write");

  /* NO RUNG. A flip redraws nothing and costs nothing, so the honest answer is YES --
   * anything else would gate off an animation that is already free. */
  icon_store_reset(0, 0);
  CHK(icon_store_rung() == ICON_RUNG_NONE, "edges: no path and no ROM is no rung");
  CHK(icon_store_plan_resident() == true,
      "edges: with NO rung, a flip is free -- plan_resident must be TRUE");
  CHK(icon_store_plan_count() == 0, "edges: no rung, no plan");
  CHK(icon_store_plan(0, 0) == 0, "edges: retiring a plan with no rung is harmless");

  /* NO PLAN DECLARED, with a live rung. This is FALSE, and it is a different false from
   * "does not fit": nobody told the store what the screen is about to draw, so it
   * cannot possibly promise the flip is free. art_fallbacks.c's gate distinguishes the
   * two with icon_store_plan_count() and says so in the log. */
  icon_store_reset(ICONS, 0);
  CHK(icon_store_plan_count() == 0, "edges: a fresh store has no plan");
  CHK(icon_store_plan_resident() == false,
      "edges: a live rung with NO plan declared must be FALSE, not a silent yes");
  const uint16_t six[6] = { 1, 2, 3, 4, 5, 6 };
  CHK(icon_store_plan(six, 6) == 6, "edges: declare six");
  CHK(icon_store_plan_count() == 6, "edges: the count is the declaration");
  CHK(icon_store_plan_resident(), "edges: six fit six");
  icon_store_plan(0, 0);
  CHK(icon_store_plan_count() == 0, "edges: n <= 0 retires the plan");
  CHK(icon_store_plan_resident() == false, "edges: a retired plan is not resident");
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
  t_borrow_cache();
  t_borrow_leak();
  t_borrow_dirty();
  t_plan_edges();
  t_rom_rung(dir, "Emerald");
  t_rom_rung(dir, "FireRed");

  icon_store_suspend();
  f_mount(0, "", 0);
  rd_free();
  printf("icon_store (bulk / retention / both rungs): %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
