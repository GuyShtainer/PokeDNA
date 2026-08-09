/* GROUND TRUTH: the Gen-1/2 parsers against Guy's REAL cartridge saves.
 *
 * Why this file exists, in one sentence: a synthetic fixture built from the same
 * constants as the parser can never falsify those constants. tests/gen12_fixture.c
 * and source/gen2_save.c both carried 0x3D96 as the destination of the G/S backup
 * mirror's second region, agreed with each other perfectly, and were both WRONG —
 * the real value is 0x3D69, a transposed digit. Only a real save could catch it,
 * and it did: four regions mirrored byte-for-byte and one did not, while the stored
 * backup checksum equalled the primary, so the data had to match and the ADDRESS
 * had to be wrong.
 *
 * These saves are Guy's own cartridge dumps. They live OUTSIDE the repo (gitignored
 * at gba-toolkit/roms/gb/) and are never copied into it, so a missing corpus SKIPS
 * rather than fails — but a corpus that IS present must parse perfectly.
 *
 *   cc -std=c11 -I source tests/host_gbreal_test.c source/gen1_save.c source/gen2_save.c -o /tmp/hgbr && /tmp/hgbr
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

static void gen2(const char* file, int expect_backup) {
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
  /* The backup is the assertion that caught the transposed mirror address. */
  if (expect_backup) CHECK(sv.backup_ok, "the BACKUP checksum must validate too");
}

int main(void) {
  printf("== real cartridge saves (ground truth for the format constants) ==\n");
  gen1("Red.sav");
  gen2("Gold.sav", 1);
  gen2("Crystal.sav", 1);
  gen2("Gold-VC.sav.dat", 1);
  gen2("Crystal-VC.sav.dat", 1);
  if (!g_ran) { printf("  (no corpus present — nothing verified)\n"); return 0; }
  printf("gb real-save test: %d checks, %d failure(s) over %d save(s)\n", g_check, g_fail, g_ran);
  return g_fail ? 1 : 0;
}
