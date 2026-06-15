/*
 * Host test for the pure Secret-Base parser (gen3_secretbase.c). Runs on the PC:
 *   cc -I../source host_secretbase_test.c ../source/gen3_secretbase.c ../source/gen3_save.c -o /tmp/sbt && /tmp/sbt
 * Builds a synthetic SaveBlock1 with one base, checks every field offset, then
 * the clear path.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gen3_save.h"
#include "gen3_secretbase.h"

static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

int main(void) {
  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  memset(sb1, 0, sizeof sb1);
  uint32_t base = SB1_OFF_SECRET_BASES_EMERALD;
  assert(base == gen3_secret_base_offset(G3_VER_EMERALD));

  /* slot 0 (own) left empty; populate slot 1 (a friend's base) */
  uint8_t* r = sb1 + base + 1 * SB_RECORD;
  r[0x00] = 5;                       /* secretBaseId */
  r[0x01] = 0x10 | 0x20 | 0x40;      /* gender=1, battledToday=1, registryStatus=1 */
  w16(r + 0x09, 0x1234);             /* trainerId low16 */
  w16(r + 0x0E, 9);                  /* numSecretBasesReceived */
  r[0x10] = 7;                       /* numTimesEntered */
  r[0x12] = r[0x13] = r[0x14] = 0x40; /* 3 decorations placed */
  w16(r + 0x7C, 25);  w16(r + 0x7E, 1);   /* party species [0],[1] */
  w16(r + 0x88, 13);                      /* held item [0] */
  r[0x94] = 50; r[0x95] = 10;             /* levels [0],[1] */

  assert(sb_slot_empty(sb1, base, 0));
  assert(!sb_slot_empty(sb1, base, 1));

  SbRecord recs[SB_COUNT];
  int n = sb_read_all(sb1, base, recs);
  assert(n == 1);                          /* only slot 1 is non-empty */
  const SbRecord* b = &recs[0];
  assert(b->slot == 1 && !b->own);
  assert(b->id == 5);
  assert(b->gender == 1);
  assert(b->battledToday);
  assert(b->registryStatus == 1);
  assert(b->trainerId == 0x1234);
  assert(b->numReceived == 9);
  assert(b->numEntered == 7);
  assert(b->decorCount == 3);
  assert(b->partyCount == 2);
  assert(b->party.species[0] == 25 && b->party.species[1] == 1);
  assert(b->party.level[0] == 50 && b->party.level[1] == 10);
  assert(b->party.heldItem[0] == 13);

  /* FR/LG maps to G3_VER_UNKNOWN here (offset 0) -> nothing */
  assert(gen3_secret_base_offset(G3_VER_UNKNOWN) == 0);
  assert(sb_read_all(sb1, 0, recs) == 0);

  /* clear -> slot empty -> no records */
  sb_clear(sb1, base, 1);
  assert(sb_slot_empty(sb1, base, 1));
  assert(sb_read_all(sb1, base, recs) == 0);

  printf("host_secretbase_test: OK (parse + clear, all field offsets)\n");
  return 0;
}
