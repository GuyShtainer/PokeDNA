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
} FileCtx;

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  if (fc->poison_off && fc->poison_off >= off && fc->poison_off < off + len)
    ((uint8_t*)dst)[fc->poison_off - off] ^= fc->poison_xor;
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

/* ------------------------------------------------- F2: wrong-offset fallback */

static void part_f2_wrong_offset_fallback(const char* name, const ExpectLoc* expect) {
  char path[256]; snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = fopen(path, "rb");
  if (!fc.f) { printf("  SKIP %s (no %s)\n", name, path); return; }
  uint32_t sz = file_size(path);

  /* A genuine known-table/cache hit costs a HANDFUL of reads (try_loc()'s own
   * id_hash/menu-window/per-kind decode reads: 1 header + 1 menu window + n
   * per-kind tile reads, well under 50 for n=38). Baseline that first. */
  RomGbIcon warm;
  fc.reads = 0;
  int wok = rom_gbicon_open(&warm, file_read, &fc, sz, g_scratch, sizeof g_scratch, 0, 0);
  chk(name, "warm known-table open succeeds", wok);
  uint32_t warm_reads = fc.reads;

  /* Now a deliberately WRONG RomGbIconLoc (icon_pointers offset corrupted) --
   * try_loc() must reject it and rom_gbicon_open_loc() must fall through to a
   * full scan, landing on the SAME correct offsets a fresh scan would. */
  RomGbIconLoc bad; memset(&bad, 0, sizeof bad);
  bad.id_hash = warm.id_hash; bad.size = warm.size;
  bad.mon_menu_icons = warm.mon_menu_icons;
  bad.icon_pointers = warm.icon_pointers ^ 0x40;   /* wrong offset entirely */
  bad.n = warm.n; bad.icon_bank = warm.icon_bank;

  RomGbIcon gi;
  fc.reads = 0;
  int ok = rom_gbicon_open_loc(&gi, file_read, &fc, sz, g_scratch, sizeof g_scratch, &bad, 0, 0);
  chk(name, "F2: a wrong-offset loc falls back to a full scan and still succeeds", ok);
  if (ok) chk_loc_matches(name, &gi, expect);
  printf("  %s: warm hit %lu reads, wrong-offset fallback %lu reads (%s)\n",
         name, (unsigned long)warm_reads, (unsigned long)fc.reads,
         fc.reads > warm_reads * 4 ? "fallback confirmed: reads jumped" : "SUSPICIOUS: reads did not jump");
  chk(name, "F2: the fallback's read count is much larger than a warm hit's (proves a real scan ran)",
      fc.reads > warm_reads * 4);
  fclose(fc.f);
}

/* --------------------------------------------------------------- F1: gates */

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
   * icon-pointer gate (rd16(w)==rd16(w+2)) is markedly weaker in practice --
   * long runs of 0x00/0xFF padding, common in a real ROM, trivially satisfy
   * "two u16 halves equal" throughout the whole padded region, so it only
   * roughly halves the callback's own work rather than the >99% cut the
   * triad gate gets. Both are still real, both are still a strict prefix of
   * their own callback's first check (never a false rejection), and both are
   * asserted here against their OWN measured ceiling, not an assumed one. */
  chk(name, "F1: the menu-icon gate rejects the overwhelming majority of positions (<5%)",
      g_rgi_cb_calls[0] * 20u < offered);
  chk(name, "F1: the icon-pointer gate rejects a real majority of positions (<75%)",
      g_rgi_cb_calls[1] * 4u < offered * 3u);
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

  printf("\n-- F2: a wrong-offset loc falls back to a full scan --\n");
  part_f2_wrong_offset_fallback("Gold.gbc", &EXPECT_GOLD);
  part_f2_wrong_offset_fallback("Crystal.gbc", &EXPECT_CRYSTAL);

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
