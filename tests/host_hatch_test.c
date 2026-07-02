/* Host test for the egg flag + hatch edit core (pure C, no hardware):
 *   cc -std=c11 -I source tests/host_hatch_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/data_tables.c -o /tmp/hh && /tmp/hh
 *
 * Verifies em_set_egg toggles BOTH egg locations (flags byte bit2 + Misc IV-word bit30) and
 * that em_hatch clears the egg, resets the (hatch-counter) friendship byte, and keeps the
 * species/IVs — i.e. the revealed Pokemon is intact. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "gen3_edit.h"
#include "gen3_mon.h"
#include "data_tables.h"

static int fails = 0;
static void chk(const char* what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }

int main(void) {
  uint8_t rec[80];
  gen3_build_mon(1, 5, 0x12345678u, 0xAABBCCDDu, "TEST", 3, rec);   /* Bulbasaur (species 1), Lv5 */

  /* make it an egg with a hatch-cycle counter (stored in the friendship byte) */
  EditMon e; gen3_edit_load(rec, false, &e);
  em_set_egg(&e, true);
  em_set_iv(&e, PK_ATK, 31);                 /* a distinctive IV to prove it survives the hatch */
  em_set_friendship(&e, 20);
  uint8_t egg[80]; gen3_edit_commit(&e, egg);

  PkMon m; pk_decode_mon(egg, false, &m);
  chk("is egg after set_egg", m.isEgg);
  chk("egg keeps species", m.species == 1);
  chk("egg keeps IV", m.ivs[PK_ATK] == 31);

  /* clearing the egg flag again should un-egg it (round-trip both bits) */
  gen3_edit_load(egg, false, &e); em_set_egg(&e, false);
  uint8_t un[80]; gen3_edit_commit(&e, un);
  pk_decode_mon(un, false, &m);
  chk("not egg after clear", !m.isEgg);

  /* HATCH the egg */
  gen3_edit_load(egg, false, &e);
  em_hatch(&e);
  uint8_t hatched[80]; gen3_edit_commit(&e, hatched);
  pk_decode_mon(hatched, false, &m);
  chk("hatched: not an egg", !m.isEgg && !m.isBadEgg);
  chk("hatched: species intact", m.species == 1);
  chk("hatched: IV intact", m.ivs[PK_ATK] == 31);
  chk("hatched: friendship reset to base 70 (not the hatch counter)", m.friendship == 70);
  chk("hatched: exp = level 5", m.experience == pk_exp_for_level(pk_species_growth(1), 5));

  printf("hatch test: %d failure(s)\n", fails);
  return fails ? 1 : 0;
}
