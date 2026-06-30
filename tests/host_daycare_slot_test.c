/* Host test for the Day-Care free-slot / occupancy decision (the "DAY-CARE FULL when it
 * isn't" bug). Pure C, no hardware:
 *   cc -std=c11 -I source tests/host_daycare_slot_test.c source/gen3_mon.c source/gen3_save.c -o /tmp/hds && /tmp/hds
 *
 * The bug: deposit decided a slot was occupied by whether pk_decode_mon() returned true.
 * But pk_decode_mon() returns true for a species-0 slot whose stored checksum != 0 (a
 * "dirty-but-empty" slot, common on real saves) — only the all-zero record is its empty
 * sentinel. The game (CountPokemonInDaycare) and the viewer (dc_rescan) treat a slot as
 * empty iff its SPECIES is 0. So a dirty-but-empty slot wrongly read as occupied -> the
 * deposit reported the Day-Care full. The fix: occupancy = real species in 1..411, not bad
 * egg (dc_first_free in pdna_main.c). This test reproduces both the bug and the fix. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "gen3_mon.h"

static int fails = 0;
static void eq(const char* what, int got, int want) {
  if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); fails++; }
}

/* Build a minimal "occupied" 80-byte box record: PID=otId=0 so the XOR key is 0 and the
 * secure block is stored in the clear; substruct order for PID%24==0 is {Growth,Attacks,
 * EVs} = slots {0,1,2}, so Growth (species) sits at the very start of the secure block. */
static void make_mon(uint8_t rec[80], uint16_t species) {
  memset(rec, 0, 80);
  rec[0x20] = (uint8_t)(species & 0xFF);          /* Growth.species lo (secure block word 0) */
  rec[0x21] = (uint8_t)(species >> 8);            /* Growth.species hi */
  /* mon checksum = sum of the 24 decrypted halfwords; only word 0 (=species) is set */
  rec[0x1C] = (uint8_t)(species & 0xFF);
  rec[0x1D] = (uint8_t)(species >> 8);
}

/* A "dirty-but-empty" slot: all zero (species 0) EXCEPT a stray non-zero checksum byte, so
 * pk_decode_mon()'s checksum check fails and it does NOT hit the all-zero empty sentinel. */
static void make_dirty_empty(uint8_t rec[80]) {
  memset(rec, 0, 80);
  rec[0x1C] = 0x01;                               /* stored checksum != computed (0) */
}

/* OLD (buggy) occupancy: "decode returned anything but the empty sentinel". */
static int first_free_old(const uint8_t* slot0, const uint8_t* slot1) {
  const uint8_t* s[2] = { slot0, slot1 };
  for (int i = 0; i < 2; i++) { PkMon m; if (!pk_decode_mon(s[i], false, &m)) return i; }
  return -1;
}

/* NEW (fixed) occupancy: a real species 1..411, not a bad egg (mirrors dc_first_free). */
static int first_free_new(const uint8_t* slot0, const uint8_t* slot1) {
  const uint8_t* s[2] = { slot0, slot1 };
  for (int i = 0; i < 2; i++) {
    PkMon m;
    bool used = pk_decode_mon(s[i], false, &m) && m.species >= 1 && m.species <= 411 && !m.isBadEgg;
    if (!used) return i;
  }
  return -1;
}

int main(void) {
  uint8_t occ[80], empty[80], dirty[80];
  make_mon(occ, 1);                 /* Bulbasaur */
  memset(empty, 0, 80);             /* truly empty (all zero) */
  make_dirty_empty(dirty);

  /* Sanity: our synthetic records decode the way the test assumes. */
  { PkMon m; eq("occ decodes",   pk_decode_mon(occ, false, &m), 1);   eq("occ species", m.species, 1); }
  { PkMon m; eq("empty empty",   pk_decode_mon(empty, false, &m), 0); }
  { PkMon m; eq("dirty decodes", pk_decode_mon(dirty, false, &m), 1); eq("dirty species0", m.species, 0); }

  /* Truly-empty second slot: both old and new agree it's free. */
  eq("old: occ+empty -> slot1", first_free_old(occ, empty), 1);
  eq("new: occ+empty -> slot1", first_free_new(occ, empty), 1);

  /* THE BUG: occupied slot 0 + dirty-but-empty slot 1.
   * Old logic reports FULL (-1); new logic correctly finds slot 1 free. */
  eq("old: occ+dirty -> FULL (bug)", first_free_old(occ, dirty), -1);
  eq("new: occ+dirty -> slot1 (fix)", first_free_new(occ, dirty), 1);

  /* Both slots dirty-but-empty (e.g. a never-used Day-Care with leftover bytes):
   * old logic blocks ALL deposits; new logic offers slot 0. */
  eq("old: dirty+dirty -> FULL (bug)", first_free_old(dirty, dirty), -1);
  eq("new: dirty+dirty -> slot0 (fix)", first_free_new(dirty, dirty), 0);

  /* Genuinely full (two real mons) -> -1 under both. */
  eq("new: occ+occ -> FULL", first_free_new(occ, occ), -1);

  printf("OK: %d failure(s)\n", fails);
  return fails ? 1 : 0;
}
