/* Host test for the Day-Care compatibility core (pure C, no hardware).
 *   cc -std=c11 -I source tests/host_daycare_test.c source/gen3_daycare.c source/gen3_edit.c \
 *      source/learnsets2.c source/data_tables.c source/evolutions.c source/gen3_mon.c \
 *      source/gen3_save.c source/gen3_trainer.c -o /tmp/hd && /tmp/hd
 * (gen3_daycare.c also carries the take-out growth core, BACKLOG #373, which needs the edit core.)
 * Internal ids: Bulbasaur 1, Charmander 4, Squirtle 7, Magnemite 81, Ditto 132, Mew 151. */
#include <stdio.h>
#include "gen3_daycare.h"

static int fails = 0;
static void eq(const char* what, int got, int want) {
  if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); fails++; }
}

int main(void) {
  /* egg groups: Bulbasaur = MONSTER(1)|GRASS(7); Ditto = DITTO(13); Mew = UNDISCOVERED(15) */
  eq("bulba g1", pk_egg_group(1, 0), 1);
  eq("bulba g2", pk_egg_group(1, 1), 7);
  eq("ditto g1", pk_egg_group(132, 0), 13);
  eq("mew  g1",  pk_egg_group(151, 0), 15);

  /* compatibility verdict (score, gender-independent) */
  eq("two ditto",            pk_daycare_compat(132, 1, 132, 2), DC_INCOMPATIBLE);
  eq("ditto+bulba diff OT",  pk_daycare_compat(132, 1, 1,   2), DC_MED);
  eq("ditto+bulba same OT",  pk_daycare_compat(132, 5, 1,   5), DC_LOW);
  eq("same species diff OT", pk_daycare_compat(1,   1, 1,   2), DC_HIGH);
  eq("same species same OT", pk_daycare_compat(1,   9, 1,   9), DC_MED);
  /* Bulbasaur(MONSTER,GRASS) + Charmander(MONSTER,DRAGON) share MONSTER */
  eq("share grp diff OT",    pk_daycare_compat(1,   1, 4,   2), DC_MED);
  eq("share grp same OT",    pk_daycare_compat(1,   3, 4,   3), DC_LOW);
  /* Bulbasaur + Magnemite(MINERAL,MINERAL): no shared group */
  eq("no shared group",      pk_daycare_compat(1,   1, 81,  2), DC_INCOMPATIBLE);
  /* Mew is UNDISCOVERED -> never compatible */
  eq("undiscovered",         pk_daycare_compat(151, 1, 1,   2), DC_INCOMPATIBLE);
  eq("empty slot",           pk_daycare_compat(0,   0, 1,   1), DC_INCOMPATIBLE);

  /* practical breedability (gender: 0=M 1=F 2=genderless) */
  eq("M+F can breed",        pk_daycare_can_breed(1, 0, 1, 1), 1);
  eq("M+M cannot",           pk_daycare_can_breed(1, 0, 1, 0), 0);
  eq("ditto+mon can",        pk_daycare_can_breed(132, 2, 1, 0), 1);
  eq("ditto+ditto cannot",   pk_daycare_can_breed(132, 2, 132, 2), 0);
  eq("mew+mew cannot",       pk_daycare_can_breed(151, 2, 151, 2), 0);

  printf("OK: %d failure(s)\n", fails);
  return fails ? 1 : 0;
}
