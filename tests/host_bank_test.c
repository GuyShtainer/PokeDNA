/* Host test for the bank's box layout contract (pdna_bank.c stores each bank box
 * as a 4-byte header + 30 raw records, "pc-mini-layout", so the shared box screen
 * and app_mon_menu's pk_box_slot() both address it correctly).
 *
 * It proves the two access paths the bank relies on AGREE:
 *   1. pk_decode_box_raw(buf + 0x0004)  — how the box screen decodes the grid
 *   2. pk_box_slot(buf, 0, slot)        — how app_mon_menu locates a record
 * and that a bank box decodes identically to the same records in the real PC.
 *
 *   cc -std=c11 -I source tests/host_bank_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/gen3_clip.c source/data_tables.c -o /tmp/hb
 *   /tmp/hb tests/fixtures/POKEMON_EMER_BPEE00.sav
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_clip.h"
#include "data_tables.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

static uint8_t g_save[1 << 17];
static uint8_t g_pc[G3_PC_BYTES];
static uint8_t g_bank[0x0004 + 30 * 80];     /* the bank's per-box buffer */

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: %s save.sav\n", argv[0]); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { printf("open FAILED\n"); return 1; }
  size_t n = fread(g_save, 1, sizeof g_save, f); fclose(f);

  Gen3SaveInfo info;
  if (!gen3_parse(g_save, (uint32_t)n, &info)) { printf("parse FAILED\n"); return 1; }
  if (gen3_read_pc_storage(g_save, info.slot, g_pc) != G3_PC_BYTES) { printf("PC FAILED\n"); return 1; }

  /* Copy PC box 0's 30 records into the bank's pc-mini-layout (records at +0x0004),
   * exactly as a migration / load would. */
  memset(g_bank, 0, sizeof g_bank);
  memcpy(g_bank + 0x0004, g_pc + 0x0004, 30 * 80);

  /* (1) pk_box_slot(buf, 0, slot) must address record `slot` at +0x0004+slot*80, the
   * same bytes pk_decode_box_raw reads — check the pointer arithmetic agrees. */
  for (int s = 0; s < 30; s++) {
    uint8_t* via_slot = pk_box_slot(g_bank, 0, s);
    uint8_t* via_raw  = g_bank + 0x0004 + (size_t)s * 80;
    CHECK(via_slot == via_raw, "pk_box_slot offset != raw record offset");
  }

  /* (2) decoding the bank buffer must match decoding the same box in the real PC. */
  PkMon viaPC[30], viaBank[30];
  int occPC   = pk_read_box(g_pc, 0, viaPC);
  int occBank = pk_decode_box_raw(g_bank + 0x0004, viaBank);
  CHECK(occPC == occBank, "occupied count differs PC vs bank");
  for (int s = 0; s < 30; s++) {
    CHECK(viaPC[s].species   == viaBank[s].species,   "species mismatch");
    CHECK(viaPC[s].personality == viaBank[s].personality, "PID mismatch");
    CHECK(viaPC[s].level     == viaBank[s].level,     "computed level mismatch");
  }
  printf("box0: %d occupied, decoded identically via PC and bank layout\n", occBank);

  printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
