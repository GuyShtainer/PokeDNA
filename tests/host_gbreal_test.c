/* GROUND TRUTH: the Gen-1/2 parsers against Guy's REAL cartridge saves.
 *
 *   cc -std=c11 -I source tests/host_gbreal_test.c source/gen1_save.c source/gen2_save.c -o /tmp/hgbr && /tmp/hgbr
 *
 * Why this file exists, in one sentence: a synthetic fixture built from the same
 * constants as the parser can never falsify those constants — a real save, or a real
 * ROM boot, can. BACKLOG #49 P0 is the worked example: tests/gen12_fixture.c and
 * source/gen2_save.c both carried 0x3D69 as the destination of the G/S backup mirror's
 * second region (sBackupPlayerData2), agreed with each other perfectly, and were both
 * WRONG — the real address, per pokegold's own compiled symbols and a ROM-boot test
 * (tools/gb_roundtrip.py --selftest; see source/gen2_save.c's k_gs_mirror comment for
 * all four witnesses), is 0x3D96.
 *
 * A REAL SAVE'S BACKUP CAN STILL BE STALE, AND THAT IS NOT A PARSER BUG. Guy's own
 * Gold.sav (and its VC twin, byte-identical) is a case in point: at the correct 0x3D96,
 * 253 of the region's 426 bytes disagree with the primary right now (computed backup
 * sum 0xC03D vs the stored 0x7E6D == 0xAEF9), so g2_detect() correctly reports
 * backup_ok == false for this file today.
 *
 * THE STALENESS IS NOT RANDOM (P0 review D9 — a correction to an earlier version of
 * this comment, which overreached and claimed nothing could have produced it). The
 * region's CURRENT bytes at 0x3D96 are byte-for-byte primary[0x222F+45 ..
 * 0x222F+425] — the primary block SHIFTED by exactly 45 — for the first 381 of the
 * region's 426 bytes, and the final 45 (indices 381..425) equal the primary's tail
 * UNSHIFTED (independently re-measured against roms/gb/Gold.sav: both halves match
 * exactly). That is precisely the fingerprint a write of the primary to the WRONG
 * address 0x3D69 (45 bytes early) leaves inside the CORRECT region's own footprint:
 * bytes 0x3D96..0x3F12 sit 45 bytes inside a wrong-address write covering
 * [0x3D69, 0x3F13), so they read back as the primary shifted by 45; bytes
 * 0x3F13..0x3F3F sit outside that footprint and were never touched by it. This file
 * does not know, and does not claim, WHAT wrote that — gen2_save.c is read-only and
 * BACKLOG #49 P0 is the first PokeDNA slice that writes a Game Boy save at all, so
 * whatever produced this predates this design either way. What IS certain: a real
 * boot re-syncs it (TryLoadSaveFile rewrites the backup from WRAM on every successful
 * load, pokegold/engine/menus/save.asm:538-552 — and tools/gb_retail_gate.py's own
 * "mirror sync only" report on this exact file shows 256 backup bytes moving on a
 * boot), but nothing in THIS test suite ever has, and this comment must not imply
 * otherwise. The `gen2()` helper below asserts exactly the CURRENT state rather than
 * the healthier-looking but false claim "the backup always validates".
 *
 * These saves are Guy's own cartridge dumps. They live OUTSIDE the repo (gitignored
 * at gba-toolkit/roms/gb/) and are never copied into it, so a missing corpus SKIPS
 * rather than fails — but a corpus that IS present must parse perfectly.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "gen1_save.h"
#include "gen2_save.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

static uint8_t img[64 * 1024];
static uint32_t load(const char* p) {
  FILE* f = fopen(p, "rb"); if (!f) return 0;
  uint32_t n = (uint32_t)fread(img, 1, sizeof img, f); fclose(f); return n;
}

static void gen1(const char* file) {
  char p[512]; snprintf(p, sizeof p, "%s/%s", ROMS, file);
  uint32_t n = load(p);
  if (!n) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  Gen1Save s;
  Gen1Status st = gen1_open(img, n, &s);
  printf("  %-12s %6u B  %s\n", file, n, gen1_status_text(st));
  CHECK(st == GEN1_OK, "a real Gen-1 save must parse");
  if (st != GEN1_OK) return;
  CHECK(s.checksum_stored == s.checksum_calc, "the stored checksum must match the computed one");
  CHECK(s.party_count <= GEN1_PARTY_CAPACITY, "party count within capacity");
  CHECK(s.current_box < GEN1_NUM_BOXES, "current box in range");
  CHECK(s.player_name[0] != 0, "the player name decodes to something");
  /* every occupied slot in every box must decode, with a dex number that exists */
  int decoded = 0, bad = 0;
  for (int b = 0; b < GEN1_NUM_BOXES; b++)
    for (int i = 0; i < s.box_count[b]; i++) {
      Gen1Mon m;
      if (!gen1_decode_image(&s, img, b, i, &m)) { bad++; continue; }
      decoded++;
      if (m.dex > 151) bad++;
      if (m.level < 1 || m.level > 100) bad++;
      for (int d = 0; d < G1_NSTATS; d++) if (m.dv[d] > 15) bad++;
    }
  printf("      player '%s'  TID %u  party %d  boxed %d\n",
         s.player_name, s.trainer_id, s.party_count, decoded);
  CHECK(bad == 0, "every boxed Pokemon decodes with sane dex/level/DVs");
  CHECK(decoded > 0, "a played save has at least one boxed Pokemon");
}

/* P0 review D7: this used to hardcode "Gold.sav's backup must be unhealthy right now"
 * (expect_backup 0/1) — true when this file was written, but brittle: the moment
 * someone re-dumps Gold.sav after a real boot heals it (TryLoadSaveFile does, on every
 * successful load), that hardcoded expectation flips and the suite breaks for a reason
 * that has nothing to do with a parser regression. Deriving the expectation instead
 * means the assertion is "the streaming scanner's verdict agrees with a direct,
 * non-streaming recompute of the same two numbers" — a cross-oracle that holds
 * regardless of which state the corpus is actually in, and would still catch a REAL
 * regression (the two computations diverging) that a hardcoded true/false cannot tell
 * apart from an honest, expected change in the corpus itself. */
static void gen2(const char* file) {
  char p[512]; snprintf(p, sizeof p, "%s/%s", ROMS, file);
  uint32_t n = load(p);
  if (!n) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  G2Scan sc; g2_scan_begin(&sc);
  for (uint32_t o = 0; o < n; o += 256) {
    uint32_t c = (n - o) < 256 ? (n - o) : 256;
    g2_scan_feed(&sc, o, img + o, c);
  }
  G2Save sv; g2_scan_finish(&sc, n, &sv);
  const char* rej = g2_reject_reason(&sv);
  printf("  %-22s %6u B  %-8s primary=%d backup=%d\n", file, n,
         sv.supported ? "OK" : (rej ? "REJECT" : "?"), sv.primary_ok, sv.backup_ok);
  CHECK(sv.supported, "a real Gen-2 save must be supported");
  CHECK(sv.primary_ok, "the primary checksum must validate");

  uint32_t b_off = g2_checksum_backup_off(sv.version);
  uint16_t b_stored = (uint16_t)(img[b_off] | ((uint16_t)img[b_off + 1] << 8));
  uint16_t b_calc = g2_checksum_backup(img, sv.version);
  bool backup_should_be_ok = (b_stored == b_calc);
  printf("     backup: stored=%#06x computed=%#06x -> %s %s\n", b_stored, b_calc,
        backup_should_be_ok ? "HEALTHY" : "STALE",
        backup_should_be_ok == (bool)sv.backup_ok ? "(scanner agrees)"
                                                  : "(scanner DISAGREES -- a real bug)");
  /* The backup is the assertion that caught the transposed mirror address (BACKLOG #49
   * P0) — derived, never hardcoded or skipped, so a save whose real state changes
   * (healed by a boot, or newly stale) cannot silently start "passing" for the wrong
   * reason either way. */
  CHECK((bool)sv.backup_ok == backup_should_be_ok,
       "the streaming scanner's backup_ok agrees with a direct recompute of the same "
       "stored-vs-computed comparison");
}

int main(void) {
  printf("== real cartridge saves (ground truth for the format constants) ==\n");
  gen1("Red.sav");
  gen2("Gold.sav");
  gen2("Crystal.sav");
  gen2("Gold-VC.sav.dat");
  gen2("Crystal-VC.sav.dat");
  if (!g_ran) { printf("  (no corpus present — nothing verified)\n"); return 0; }
  printf("gb real-save test: %d checks, %d failure(s) over %d save(s)\n", g_check, g_fail, g_ran);
  return g_fail ? 1 : 0;
}
