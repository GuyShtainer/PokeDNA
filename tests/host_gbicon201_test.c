/* Host (PC) test for BACKLOG #201 -- the Gen-2 menu-icon locator's inline gates
 * (F1), known-ROM table (F2) and pass-1/pass-2 progress hook (F3). Runs the
 * exact production rom_gbicon.c against Guy's own Gold.gbc/Crystal.gbc dumps,
 * same corpus and file-I/O shim as tests/host_romgbicon_test.c.
 *
 *   cc -std=c11 -DROM_GBICON_JOB_COUNTERS=1 -I source tests/host_gbicon201_test.c source/rom_gbicon.c -o /tmp/hgbi201 && /tmp/hgbi201
 *
 * Coverage:
 *   1) F2 table-equals-scanner: with the known-ROM table's own entries POISONED
 *      out (a checksum-flip that forces known_icon_lookup() to miss, exactly
 *      tests/host_gbscan_test.c's own part_b_f5_one() idiom), the live scanner's
 *      located offsets are asserted byte-for-byte equal to source/rom_gbicon_
 *      known.h's hardcoded entries, for both Gold and Crystal. A hand-edited (or
 *      scanner-drifted) table field would fail this immediately -- proven below
 *      by actually swapping two fields in a LOCAL copy of the expected struct and
 *      showing the comparison catches it (never touches the real header).
 *   2) F2 wrong-offset fallback: a RomGbIconLoc with icon_pointers pointed at the
 *      wrong offset is rejected by try_loc() and a full scan runs instead --
 *      final offsets are still the correct ones, and the read count for that
 *      open is much larger than a genuine cache/table hit's handful of reads
 *      (proving the fallback path, not a lucky accidental pass).
 *   3) F1 gate evidence: built with ROM_GBICON_JOB_COUNTERS, the per-callback
 *      invocation counts (post-gate) are compared against the number of window
 *      positions actually offered (pre-gate) -- the gate must reject the large
 *      majority of positions before the callback ever runs.
 *   4) F3 pass-2 hook: a RomGbIconPassFn probe records how many times it fired
 *      (exactly once) and how many reads the SAME FileCtx had already made by
 *      then (pass 1 alone reads a meaningful chunk of the ROM to prove
 *      MonMenuIcons unique) versus the read count at the very end of open()
 *      (pass 2 reads more on top) -- proving the hook lands strictly between
 *      the two passes, not before both or after both.
 *
 * ROMs are Guy's own dumps: they live OUTSIDE the repo and are never copied
 * into it, so a missing corpus SKIPS (exit 0, "NOTHING RAN").
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_gbicon.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
static void chk(const char* who, const char* what, int cond) {
  g_check++;
  if (!cond) { printf("  !! FAIL [%s] %s\n", who, what); g_fail++; }
}

#ifdef ROM_GBICON_JOB_COUNTERS
extern uint32_t g_rgi_cb_calls[2];
#endif

/* ------------------------------------------------------------- file I/O shim */

typedef struct {
  FILE*    f;
  uint32_t poison_off;
  uint8_t  poison_xor;
  uint32_t reads;          /* F3: counts every completed rom_gbicon read      */
  uint32_t overlay_off;    /* D2 chain proof: bytes the READER sees at this   */
  uint32_t overlay_len;    /* offset instead of the file's own (0 = none)     */
  uint8_t  overlay[2 * (ROM_GBICON_MAX_KINDS + 1)];
} FileCtx;

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  if (fc->poison_off && fc->poison_off >= off && fc->poison_off < off + len)
    ((uint8_t*)dst)[fc->poison_off - off] ^= fc->poison_xor;
  for (uint32_t i = 0; i < fc->overlay_len; i++) {
    uint32_t o = fc->overlay_off + i;
    if (o >= off && o < off + len) ((uint8_t*)dst)[o - off] = fc->overlay[i];
  }
  fc->reads++;
  return true;
}

static uint32_t file_size(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return (uint32_t)n;
}

static uint8_t g_scratch[ROM_GBICON_SCRATCH_MIN];

/* ------------------------------------------------------- expected known-ROM table
 * Independently transcribed from source/rom_gbicon_known.h (NOT included -- this
 * file has no access to rom_gbicon.c's file-local RomGbIconKnown typedef), so a
 * comparison against it is a real cross-check, not the header comparing to
 * itself. */
typedef struct { uint32_t id_hash, size, mon_menu_icons, icon_pointers; uint8_t n, icon_bank; } ExpectLoc;
static const ExpectLoc EXPECT_GOLD    = { 0xB60AD0FBU, 0x00200000U, 0x0008E975U, 0x0008EA70U, 38, 0x23 };
static const ExpectLoc EXPECT_CRYSTAL = { 0xA6A48B42U, 0x00200000U, 0x0008EAC4U, 0x0008EBBFU, 38, 0x23 };

static void chk_loc_matches(const char* name, const RomGbIcon* gi, const ExpectLoc* e) {
  chk(name, "id_hash matches rom_gbicon_known.h's own entry", gi->id_hash == e->id_hash);
  chk(name, "size matches",                                    gi->size == e->size);
  chk(name, "mon_menu_icons matches",                          gi->mon_menu_icons == e->mon_menu_icons);
  chk(name, "icon_pointers matches",                            gi->icon_pointers == e->icon_pointers);
  chk(name, "n matches",                                        gi->n == e->n);
  chk(name, "icon_bank matches",                                gi->icon_bank == e->icon_bank);
}

/* Demonstrates the table-equals-scanner test is NOT tautological: swapping two
 * fields of a LOCAL copy of the expected struct (never the real header) must make
 * chk_loc_matches() itself fail -- i.e. the comparison actually discriminates. */
static void mutation_proof_field_swap(void) {
  ExpectLoc mutant = EXPECT_GOLD;
  uint32_t tmp = mutant.mon_menu_icons;
  mutant.mon_menu_icons = mutant.icon_pointers;
  mutant.icon_pointers = tmp;
  int before = g_fail;
  RomGbIcon fake; memset(&fake, 0, sizeof fake);
  fake.id_hash = EXPECT_GOLD.id_hash; fake.size = EXPECT_GOLD.size;
  fake.mon_menu_icons = EXPECT_GOLD.mon_menu_icons; fake.icon_pointers = EXPECT_GOLD.icon_pointers;
  fake.n = EXPECT_GOLD.n; fake.icon_bank = EXPECT_GOLD.icon_bank;
  chk_loc_matches("MUTATION PROOF (swapped mon_menu_icons/icon_pointers)", &fake, &mutant);
  int caught = (g_fail > before);
  printf("  mutation proof (field swap): %s\n", caught ? "CAUGHT (chk_loc_matches is discriminating)"
                                                        : "!! NOT CAUGHT -- the comparison is dead weight");
  if (!caught) g_fail++;
  /* Undo this proof's own deliberate failures so they don't pollute the real tally. */
  g_fail = before;
}

/* --------------------------------------------------------- F2: table == scanner */

static void part_f2_table_equals_scanner(const char* name, const ExpectLoc* expect) {
  char path[256]; snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", name, path); return; }
  g_ran++;
  uint32_t sz = file_size(path);

  /* An UNPOISONED open first -- the known-table fast path engages, and its
   * id_hash is the ROM's real (unpoisoned) header hash, same value EXPECT's
   * own id_hash was transcribed from. */
  RomGbIcon from_table;
  int ok1 = rom_gbicon_open(&from_table, file_read, &fc, sz, g_scratch, sizeof g_scratch, 0, 0);
  chk(name, "F2: the unpoisoned (table fast-path) open locates the tables", ok1);
  if (ok1) chk_loc_matches(name, &from_table, expect);

  /* Poison the header's own global_checksum byte (0x14F) so known_icon_lookup()
   * MISSES -- forces the real whole-ROM scan to run, exactly like tests/
   * host_gbscan_test.c's part_b_f5_one()'s own poison idiom. NOTE (same as that
   * function's own comment): id_hash is FNV1a of the WHOLE header, so the same
   * poison byte that forces the table miss also changes id_hash between the two
   * opens -- a field the table's own contract (title/version/global_checksum,
   * never id_hash) does not claim to match anyway, so it is excluded from this
   * second comparison, not silently right by luck. */
  fc.poison_off = 0x14F;
  fc.poison_xor = 0xFF;
  RomGbIcon from_scan;
  int ok2 = rom_gbicon_open(&from_scan, file_read, &fc, sz, g_scratch, sizeof g_scratch, 0, 0);
  chk(name, "F2: the poisoned (forced-scan) re-open also locates the SAME tables", ok2);
  if (ok2) {
    chk(name, "F2: scanner size matches the table",            from_scan.size == expect->size);
    chk(name, "F2: scanner mon_menu_icons matches the table",  from_scan.mon_menu_icons == expect->mon_menu_icons);
    chk(name, "F2: scanner icon_pointers matches the table",   from_scan.icon_pointers == expect->icon_pointers);
    chk(name, "F2: scanner n matches the table",                from_scan.n == expect->n);
    chk(name, "F2: scanner icon_bank matches the table",        from_scan.icon_bank == expect->icon_bank);
  }
  fc.poison_off = 0;
  fclose(fc.f);
}

/* ---------------------------------------- F2/D1: open_loc(NULL) uses the table */

/* BACKLOG #201 D1 (review fix): open_loc()'s own tail now delegates a cache miss
 * to open() (known-ROM table, THEN the full scan -- see rom_gbicon.c's own
 * comment at that call site), instead of jumping straight to locate()'s full
 * scan as it did before this fix -- every PRODUCTION caller (gb_art_source.c)
 * enters through open_loc(), never open() directly, so THIS was the path that
 * actually mattered and had never exercised the table at all. Regression:
 * open_loc(NULL) must cost close to a table hit (~80 reads for n=38), nowhere
 * near a full scan's ~4,172. */
static void part_d1_openloc_null_uses_table(const char* name) {
  char path[256]; snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", name, path); return; }
  uint32_t sz = file_size(path);

  RomGbIcon gi;
  fc.reads = 0;
  int ok = rom_gbicon_open_loc(&gi, file_read, &fc, sz, g_scratch, sizeof g_scratch, 0, 0, 0);
  chk(name, "D1: open_loc(NULL) succeeds", ok);
  printf("  %s: open_loc(NULL) cost %lu reads (table hit expected, ~80; a full scan is ~4,172)\n",
         name, (unsigned long)fc.reads);
  chk(name, "D1: open_loc(NULL) uses the known-ROM table, not a full scan (<200 reads)",
      fc.reads < 200);
  fclose(fc.f);
}

/* ------------------------------------------------- F2: wrong-offset fallback */

static void part_f2_wrong_offset_fallback(const char* name, const ExpectLoc* expect) {
  char path[256]; snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", name, path); return; }
  uint32_t sz = file_size(path);

  /* A genuine known-table/cache hit costs a HANDFUL of reads (try_loc()'s own
   * id_hash/menu-window/per-kind decode reads: 1 header + n+1 icon_pointers
   * chain reads (D2) + n per-kind tile reads, well under 100 for n=38).
   * Baseline that first. */
  RomGbIcon warm;
  fc.reads = 0;
  int wok = rom_gbicon_open(&warm, file_read, &fc, sz, g_scratch, sizeof g_scratch, 0, 0);
  chk(name, "warm known-table open succeeds", wok);
  uint32_t warm_reads = fc.reads;

  /* (ii) rejected cache -> the known-ROM table (NOT a full scan): a bad .loc
   * (icon_pointers corrupted) is rejected by try_loc(), but D1's fix means the
   * fallback is open()'s OWN table-then-scan chain, which hits the table for
   * these two corpus ROMs -- so this case now costs a TABLE hit's ~80 reads,
   * not a full scan's ~4,172. Both are legitimate ("rejected cache" and "cache
   * miss" collapse to the SAME open() call as of D1), so this proves the
   * REJECTION itself (offsets end up correct, not the corrupted ones) while
   * leaving the "prove a REAL SCAN ran" claim to (iii) below, which forces the
   * table to miss too. */
  RomGbIconLoc bad; memset(&bad, 0, sizeof bad);
  bad.id_hash = warm.id_hash; bad.size = warm.size;
  bad.mon_menu_icons = warm.mon_menu_icons;
  bad.icon_pointers = warm.icon_pointers ^ 0x40;   /* wrong offset entirely */
  bad.n = warm.n; bad.icon_bank = warm.icon_bank;

  RomGbIcon gi;
  fc.reads = 0;
  int ok = rom_gbicon_open_loc(&gi, file_read, &fc, sz, g_scratch, sizeof g_scratch, &bad, 0, 0);
  chk(name, "F2: a rejected (wrong-offset) cache still lands on the correct tables", ok);
  if (ok) chk_loc_matches(name, &gi, expect);
  printf("  %s: warm hit %lu reads, rejected-cache->table %lu reads\n",
         name, (unsigned long)warm_reads, (unsigned long)fc.reads);
  chk(name, "F2: a rejected cache costs about a table hit, not a full scan (<200 reads)",
      fc.reads < 200);
  fclose(fc.f);

  /* (iii) checksum-POISONED wrong-offset case: forces the KNOWN-ROM TABLE to
   * miss too (same 0x14F poison as part_f2_table_equals_scanner), so the
   * corrupted .loc's rejection genuinely falls through to locate()'s full
   * scan -- proving D1's fallback chain still reaches a real scan when the
   * table itself does not apply, not just when it does. */
  FileCtx pfc; memset(&pfc, 0, sizeof pfc);
  pfc.f = fopen(path, "rb");
  if (!pfc.f) { printf("  SKIP %s (poisoned reopen)\n", name); return; }
  pfc.poison_off = 0x14F; pfc.poison_xor = 0xFF;
  RomGbIcon pwarm;
  pfc.reads = 0;
  int pwok = rom_gbicon_open(&pwarm, file_read, &pfc, sz, g_scratch, sizeof g_scratch, 0, 0);
  chk(name, "F2(iii): poisoned open (forced scan) succeeds", pwok);
  uint32_t pwarm_reads = pfc.reads;

  RomGbIconLoc pbad; memset(&pbad, 0, sizeof pbad);
  pbad.id_hash = pwarm.id_hash; pbad.size = pwarm.size;   /* the POISONED id_hash */
  pbad.mon_menu_icons = pwarm.mon_menu_icons;
  pbad.icon_pointers = pwarm.icon_pointers ^ 0x40;
  pbad.n = pwarm.n; pbad.icon_bank = pwarm.icon_bank;

  RomGbIcon pgi;
  pfc.reads = 0;
  int pok = rom_gbicon_open_loc(&pgi, file_read, &pfc, sz, g_scratch, sizeof g_scratch, &pbad, 0, 0);
  chk(name, "F2(iii): a wrong-offset loc on a table-miss ROM falls back to a REAL full scan", pok);
  if (pok) {
    chk(name, "F2(iii): the real scan lands on the correct mon_menu_icons", pgi.mon_menu_icons == expect->mon_menu_icons);
    chk(name, "F2(iii): the real scan lands on the correct icon_pointers",   pgi.icon_pointers == expect->icon_pointers);
    chk(name, "F2(iii): the real scan lands on the correct n",              pgi.n == expect->n);
    chk(name, "F2(iii): the real scan lands on the correct icon_bank",      pgi.icon_bank == expect->icon_bank);
  }
  /* (iii)'s own "warm" baseline is the UNPOISONED warm_reads captured at the
   * top of this function (~79 reads, an ordinary table hit) -- NOT pwarm_reads
   * (the poisoned open() used only to obtain the poisoned id_hash pbad needs
   * to match; that call is itself a full scan, ~4,172 reads, so comparing
   * against IT would trivially never show a jump). */
  printf("  %s: warm (unpoisoned) %lu reads, poisoned-id_hash open() %lu reads, "
         "poisoned wrong-offset (forced real scan) %lu reads (%s)\n",
         name, (unsigned long)warm_reads, (unsigned long)pwarm_reads, (unsigned long)pfc.reads,
         pfc.reads > warm_reads * 4 ? "real scan confirmed: reads jumped" : "SUSPICIOUS: reads did not jump");
  chk(name, "F2(iii): the forced real scan costs far more reads than the unpoisoned warm hit (>4x)",
      pfc.reads > warm_reads * 4);
  pfc.poison_off = 0;
  fclose(pfc.f);
}

/* --------------------------------------- D2: a one-entry-early shift is rejected */

/* BACKLOG #201 D2 (review fix): try_loc() used to accept a candidate icon_
 * pointers shifted ONE ENTRY EARLIER (-2 bytes) at ~78 reads -- entry[0]==
 * entry[1] still coincidentally held in the shifted window, so the old
 * range-only per-entry check never caught it, and 37 of 38 icons would have
 * silently decoded the WRONG tile. Poisons the checksum first (0x14F, same
 * as F2(iii) above) so the known-ROM table ALSO misses -- isolating D2's own
 * chain-check fix from D1's table fallback: a genuinely rejected candidate
 * must fall all the way to a real rescan (>1,000 reads), landing back on the
 * correct (unshifted) icon_pointers, not silently accept the shift. */
static void part_d2_shift_rejected(const char* name) {
  char path[256]; snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", name, path); return; }
  uint32_t sz = file_size(path);

  fc.poison_off = 0x14F; fc.poison_xor = 0xFF;
  RomGbIcon warm;
  int wok = rom_gbicon_open(&warm, file_read, &fc, sz, g_scratch, sizeof g_scratch, 0, 0);
  chk(name, "D2: poisoned warm open (table miss, real scan) succeeds", wok);
  uint32_t correct_ptrs = warm.icon_pointers;

  RomGbIconLoc shifted; memset(&shifted, 0, sizeof shifted);
  shifted.id_hash = warm.id_hash; shifted.size = warm.size;   /* the POISONED id_hash */
  shifted.mon_menu_icons = warm.mon_menu_icons;
  shifted.icon_pointers = warm.icon_pointers - 2;              /* shifted ONE ENTRY EARLIER */
  shifted.n = warm.n; shifted.icon_bank = warm.icon_bank;

  RomGbIcon gi;
  fc.reads = 0;
  int ok = rom_gbicon_open_loc(&gi, file_read, &fc, sz, g_scratch, sizeof g_scratch, &shifted, 0, 0);
  chk(name, "D2: the shifted candidate is rejected and a real rescan succeeds", ok);
  printf("  %s: shifted-by-2 candidate -> %lu reads, icon_pointers=0x%X (correct=0x%X)\n",
         name, (unsigned long)fc.reads, gi.icon_pointers, correct_ptrs);
  chk(name, "D2: the rescan costs far more than a table/cache hit (>1,000 reads)", fc.reads > 1000);
  chk(name, "D2: the rescan lands on the CORRECT (unshifted) icon_pointers, not the shifted one",
      gi.icon_pointers == correct_ptrs);
  fc.poison_off = 0;
  fclose(fc.f);
}

/* --------------------------------------------------------------- F1: gates */

/* (review re-verify) D2's CHAIN check has its own mutant: on both corpus ROMs
 * the two bytes before IconPointers are MonMenuIcons' tail (0x21 0x0E = 0x0E21,
 * out of [0x4000,0x8000)), so part_d2_shift_rejected() above is caught by the
 * k==0 RANGE read alone and passes with the +128 chain lines deleted. Here a
 * FAKE table -- the real entries reversed, entry[0]==entry[1] forced, every
 * entry in range and every target decoding non-uniform -- is overlaid at 0x200
 * and offered as the candidate: only the chain can reject it (measured: without
 * the chain it is accepted at ~231 reads with 37 of 38 kinds decoding the wrong
 * tile). Checksum poisoned so the known table misses and the fallback is a real
 * rescan. */
static void part_d2_chain_only(const char* name) {
  char path[256]; snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", name, path); return; }
  uint32_t sz = file_size(path);
  fc.poison_off = 0x14F; fc.poison_xor = 0xFF;
  RomGbIcon warm;
  int wok = rom_gbicon_open(&warm, file_read, &fc, sz, g_scratch, sizeof g_scratch, 0, 0);
  chk(name, "D2 chain: poisoned warm open succeeds", wok);
  if (!wok) { fclose(fc.f); return; }
  uint8_t real[2 * (ROM_GBICON_MAX_KINDS + 1)];
  fseek(fc.f, (long)warm.icon_pointers, SEEK_SET);
  chk(name, "D2 chain: read the real table", fread(real, 1, ((size_t)warm.n + 1u) * 2u, fc.f) == ((size_t)warm.n + 1u) * 2u);
  for (uint32_t k = 0; k <= warm.n; k++) memcpy(fc.overlay + k * 2u, real + (warm.n - k) * 2u, 2);
  memcpy(fc.overlay, fc.overlay + 2, 2);                 /* entry[0] == entry[1] */
  fc.overlay_off = 0x200; fc.overlay_len = ((uint32_t)warm.n + 1u) * 2u;
  RomGbIconLoc fake; rom_gbicon_save_loc(&warm, &fake); fake.icon_pointers = 0x200;
  RomGbIcon gi; fc.reads = 0;
  int ok = rom_gbicon_open_loc(&gi, file_read, &fc, sz, g_scratch, sizeof g_scratch, &fake, 0, 0);
  printf("  %s: reversed fake table candidate -> %lu reads, icon_pointers=0x%X (correct=0x%X)\n",
         name, (unsigned long)fc.reads, gi.icon_pointers, warm.icon_pointers);
  chk(name, "D2 chain: the fake (unchained) table is rejected and a real rescan runs (>1,000 reads)",
      ok && fc.reads > 1000);
  chk(name, "D2 chain: the rescan lands on the real icon_pointers, not the fake", ok && gi.icon_pointers == warm.icon_pointers);
  fclose(fc.f);
}

static void part_f1_gate_evidence(const char* name) {
#ifndef ROM_GBICON_JOB_COUNTERS
  (void)name;
  printf("  (F1 gate evidence needs -DROM_GBICON_JOB_COUNTERS; see this file's own cc line)\n");
#else
  char path[256]; snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", name, path); return; }
  uint32_t sz = file_size(path);

  /* Poison the table out so the real whole-ROM scan runs and the counters mean
   * something (a table hit does zero scan_one() calls at all). */
  fc.poison_off = 0x14F; fc.poison_xor = 0xFF;
  g_rgi_cb_calls[0] = g_rgi_cb_calls[1] = 0;
  RomGbIcon gi;
  int ok = rom_gbicon_open(&gi, file_read, &fc, sz, g_scratch, sizeof g_scratch, 0, 0);
  chk(name, "F1: scans OK for the gate-evidence run", ok);
  fc.poison_off = 0;
  fclose(fc.f);
  if (!ok) return;

  /* Pass 1 (MonMenuIcons) offers (size - 251 + 1) positions; pass 2 (IconPointers)
   * offers (size - look + 1) positions, look = (n+2)*2. Both are close to `size`
   * for a 2 MiB ROM, so `size` is a fair, simple upper bound on "positions
   * offered" for the before/after ratio -- the gate's whole job is to reject
   * nearly all of them before the callback (menu_icons_cb: an O(251) walk;
   * icon_ptrs_cb: an O(n) walk) ever runs. */
  uint32_t offered = sz;
  printf("  %s: window positions offered ~%lu, menu_icons_cb calls (post-gate) %lu (%.2f%%), "
         "icon_ptrs_cb calls (post-gate) %lu (%.1f%%)\n",
         name, (unsigned long)offered,
         (unsigned long)g_rgi_cb_calls[0], 100.0 * g_rgi_cb_calls[0] / offered,
         (unsigned long)g_rgi_cb_calls[1], 100.0 * g_rgi_cb_calls[1] / offered);
  /* MEASURED (not assumed): the menu-icon triad gate (w[0]==w[1]==w[2] in
   * [1,63]) is very selective on real ROM data (~1% of positions here). The
   * icon-pointer gate was ORIGINALLY just rd16(w)==rd16(w+2) -- markedly
   * weaker in practice, since long runs of 0x00/0xFF padding trivially
   * satisfy "two u16 halves equal" throughout the whole padded region
   * (measured ~25-47% there). BACKLOG #201 D3 (review fix) added the same
   * ROMX-window range check icon_ptrs_cb itself makes right after ([GB_WIN_LO,
   * GB_WIN_HI)), which most padding (0x0000/0xFFFF, both outside the window)
   * fails immediately -- measured post-fix: Gold 4,723/2,097,152 (0.23%),
   * Crystal 6,396/2,097,152 (0.30%), now comparable to the triad gate's own
   * selectivity. Both are still a strict prefix of their own callback's first
   * checks (never a false rejection), and both are asserted here against
   * their OWN measured ceiling, not an assumed one. */
  chk(name, "F1: the menu-icon gate rejects the overwhelming majority of positions (<5%)",
      g_rgi_cb_calls[0] * 20u < offered);
  chk(name, "F1: D3: the icon-pointer gate (with the range check) rejects the overwhelming "
      "majority of positions too (<1%)",
      g_rgi_cb_calls[1] * 100u < offered);
  /* Both callbacks must still fire on the real hit (gate cannot be so tight it
   * starves the true match): at least 1 call each. */
  chk(name, "F1: the menu-icon callback still fires on the real hit", g_rgi_cb_calls[0] >= 1);
  chk(name, "F1: the icon-pointer callback still fires on the real hit", g_rgi_cb_calls[1] >= 1);
#endif
}

/* ------------------------------------------------------------- F3: pass2 hook */

static int      g_pass2_calls = 0;
static uint32_t g_pass2_reads_at_call = 0;
static FileCtx* g_pass2_fc = 0;

static void pass2_probe(void* ctx) {
  (void)ctx;
  g_pass2_calls++;
  g_pass2_reads_at_call = g_pass2_fc->reads;
}

static void part_f3_pass2_hook(const char* name) {
  char path[256]; snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", name, path); return; }
  uint32_t sz = file_size(path);

  fc.poison_off = 0x14F; fc.poison_xor = 0xFF;   /* force the real scan */
  g_pass2_calls = 0;
  g_pass2_reads_at_call = 0;
  g_pass2_fc = &fc;
  RomGbIcon gi;
  int ok = rom_gbicon_open(&gi, file_read, &fc, sz, g_scratch, sizeof g_scratch, pass2_probe, 0);
  uint32_t final_reads = fc.reads;
  fc.poison_off = 0;
  fclose(fc.f);

  chk(name, "F3: scan OK for the pass2-hook run", ok);
  chk(name, "F3: pass2_cb fires EXACTLY ONCE per open()", g_pass2_calls == 1);
  chk(name, "F3: pass 1 alone already made real reads before pass2_cb fired",
      g_pass2_reads_at_call > 0);
  chk(name, "F3: pass 2 makes MORE reads after pass2_cb fired (it has its own scan left to do)",
      final_reads > g_pass2_reads_at_call);
  printf("  %s: pass2_cb fired at read #%lu of %lu total (pass 1 = %lu reads, pass 2 = %lu reads)\n",
         name, (unsigned long)g_pass2_reads_at_call, (unsigned long)final_reads,
         (unsigned long)g_pass2_reads_at_call, (unsigned long)(final_reads - g_pass2_reads_at_call));
}

int main(void) {
  printf("host_gbicon201_test -- BACKLOG #201: gates, known table, pass-2 progress hook\n");

  printf("\n-- mutation self-proof (chk_loc_matches is discriminating) --\n");
  mutation_proof_field_swap();

  printf("\n-- F2: table equals scanner (checksum poisoned to force the real scan) --\n");
  part_f2_table_equals_scanner("Gold.gbc", &EXPECT_GOLD);
  part_f2_table_equals_scanner("Crystal.gbc", &EXPECT_CRYSTAL);

  printf("\n-- D1: open_loc(NULL) uses the known-ROM table (production entry point) --\n");
  part_d1_openloc_null_uses_table("Gold.gbc");
  part_d1_openloc_null_uses_table("Crystal.gbc");

  printf("\n-- F2/D1/D2: a wrong-offset loc is rejected and falls back correctly --\n");
  part_f2_wrong_offset_fallback("Gold.gbc", &EXPECT_GOLD);
  part_f2_wrong_offset_fallback("Crystal.gbc", &EXPECT_CRYSTAL);

  printf("\n-- D2: a one-entry-early icon_pointers shift is rejected, not silently accepted --\n");
  part_d2_shift_rejected("Gold.gbc");
  part_d2_shift_rejected("Crystal.gbc");
  part_d2_chain_only("Gold.gbc");
  part_d2_chain_only("Crystal.gbc");

  printf("\n-- F1: gate evidence (post-gate callback counts vs. positions offered) --\n");
  part_f1_gate_evidence("Gold.gbc");
  part_f1_gate_evidence("Crystal.gbc");

  printf("\n-- F3: pass 1 / pass 2 progress-reset hook fires exactly once, between passes --\n");
  part_f3_pass2_hook("Gold.gbc");
  part_f3_pass2_hook("Crystal.gbc");

  printf("\n%d checks, %d ROMs exercised, %d failures\n", g_check, g_ran, g_fail);
  if (!g_ran) { printf("NOTHING RAN (no ROMs present)\n"); return 0; }
  printf(g_fail ? "FAIL\n" : "ALL PASS\n");
  return g_fail ? 1 : 0;
}
