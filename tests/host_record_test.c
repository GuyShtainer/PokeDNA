/* Host (PC) test for the Emerald Battle Record parser (save sector 31). The Emerald
 * fixture contains a REAL recorded battle (sentinel 9D B3 00 00 at 0x1F000), so this
 * validates sentinel + checksum + field decode against ground truth; the Ruby fixture
 * must scan as "no record". Build + run:
 *   cc -I source tests/host_record_test.c source/gen3_record.c source/gen3_frontier.c \
 *      source/gen3_save.c source/gen3_mon.c -o /tmp/hr && /tmp/hr
 * (gen3_record now delegates its lane offsets to gen3_frontier — one table in the tree.)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_record.h"
#include "gen3_save.h"
#include "gen3_mon.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)

static uint8_t g_buf[G3_SAVE_FILE_SIZE];

static uint32_t load(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 0; }
  uint32_t n = (uint32_t)fread(g_buf, 1, sizeof g_buf, f);
  fclose(f);
  return n;
}

int main(void) {
  /* ---- Emerald fixture: has a real record ---- */
  uint32_t sz = load("tests/fixtures/POKEMON_EMER_BPEE00.sav");
  CHECK(sz == G3_SAVE_FILE_SIZE, "emerald fixture is 128 KiB");
  G3RecordInfo ri;
  bool present = g3_record_scan(g_buf, sz, &ri);
  CHECK(present && ri.present, "emerald: record present (sentinel)");
  CHECK(ri.checksum_ok, "emerald: struct byte-sum checksum matches");
  CHECK(ri.flags_ok, "emerald: battleFlags plausible");
  CHECK(ri.facility <= 6, "emerald: facility in range");
  CHECK(ri.multiplayer_id < 4, "emerald: multiplayerId in range");
  CHECK(ri.names[ri.multiplayer_id][0] != 0, "emerald: recording player has a name");
  printf("record: %s, %s, seed=%08x, oppA=%u oppB=%u, player=\"%s\" (%s)\n",
         g3_record_facility_name(ri.facility), ri.lvl_mode ? "Open Level" : "Level 50",
         ri.rng_seed, ri.opponent_a, ri.opponent_b,
         ri.names[ri.multiplayer_id], ri.genders[ri.multiplayer_id] ? "F" : "M");
  for (int p = 0; p < 4; p++)
    if (ri.names[p][0] || ri.lane_len[p])
      printf("  player %d: \"%s\" inputs=%d bytes\n", p, ri.names[p], ri.lane_len[p]);

  /* both teams decode with the standard box-mon kernel (first 80 B of each 100-B mon) */
  for (int side = 0; side < 2; side++) {
    const uint8_t* party = g3_record_party(g_buf, side);
    int n = 0;
    for (int i = 0; i < 6; i++) {
      PkMon m;
      if (!pk_decode_mon(party + (uint32_t)i * G3_REC_MON_SIZE, false, &m) || !m.species) continue;
      CHECK(!m.isBadEgg, "record mon decodes with a clean checksum");
      CHECK(m.species >= 1 && m.species <= 411, "record mon species in range");
      uint8_t lvl = party[(uint32_t)i * G3_REC_MON_SIZE + 84];   /* plaintext battle level */
      printf("  %s mon %d: species=%u \"%s\" Lv%u\n",
             side ? "opponent" : "player  ", i, m.species, m.nickname, lvl);
      n++;
    }
    CHECK(n >= 1, "each side has at least one mon");
  }

  /* ---- Ruby fixture: must have NO record ---- */
  sz = load("tests/fixtures/POKEMON_RUBY_AXVE02.sav");
  if (sz) {
    G3RecordInfo rr;
    CHECK(!g3_record_scan(g_buf, sz, &rr), "ruby: no record (RS predate the Frontier)");
  }

  /* ---- truncated (64 KiB) save: must refuse before touching 0x1F000 ---- */
  G3RecordInfo rt;
  CHECK(!g3_record_scan(g_buf, 0x10000, &rt), "64 KiB dump: no sector 31, scan refuses");

  if (g_fail) { printf("FAILURES: %d\n", g_fail); return 1; }
  printf("OK: host_record_test (0 failures)\n");
  return 0;
}
