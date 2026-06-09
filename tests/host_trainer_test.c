/* Host test for trainer/stats reads (money, Pokedex, game records, HoF time).
 *   cc -std=c11 -I source tests/host_trainer_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_trainer.c source/gen3_edit.c source/data_tables.c -o /tmp/ht
 *   /tmp/ht tests/fixtures/POKEMON_EMER_BPEE00.sav
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_trainer.h"

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof(save), f);
  fclose(f);

  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("parse FAILED\n"); return 1; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  /* SaveBlock2 = section 0 (single sector) */
  static uint8_t sb2[G3_SECTOR_DATA_SIZE];
  int s0 = gen3_find_section(save, info.slot, 0);
  memcpy(sb2, save + (uint32_t)info.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE, G3_SECTOR_DATA_SIZE);

  /* pick game: FRLG by party-offset validity, else version_guess */
  PkMon tmp[6]; bool frlg = false; pk_read_party_auto(sb1, tmp, &frlg);
  PkGame g = frlg ? PK_FRLG : (info.version_guess == G3_VER_RS ? PK_RS : PK_EMERALD);
  const char* GN[3] = { "Ruby/Sapphire", "Emerald", "FireRed/LeafGreen" };

  printf("== %s ==\n", path);
  printf("trainer=%s  TID=%u SID=%u  play=%uh%02um  game=%s\n",
         info.trainer_name, info.tid_public, info.tid_secret, info.play_h, info.play_m, GN[g]);
  printf("money: $%u\n", (unsigned)pk_money(sb1, sb2, g));

  int seen, caught; bool nat;
  pk_pokedex(sb2, &seen, &caught, &nat);
  printf("pokedex: seen=%d caught=%d  national=%s\n", seen, caught, nat ? "yes" : "no");

  uint16_t h; uint8_t m, s;
  if (pk_hof_time(sb1, sb2, g, &h, &m, &s))
    printf("Elite Four cleared at: %uh %02um %02us (first Hall of Fame)\n", h, m, s);
  else
    printf("Elite Four: not yet cleared\n");

  printf("records: steps=%u  battles=%u (wild=%u trainer=%u)  captures=%u  eggs=%u\n",
         (unsigned)pk_game_stat(sb1, sb2, g, PK_STAT_STEPS),
         (unsigned)pk_game_stat(sb1, sb2, g, PK_STAT_TOTAL_BATTLES),
         (unsigned)pk_game_stat(sb1, sb2, g, PK_STAT_WILD_BATTLES),
         (unsigned)pk_game_stat(sb1, sb2, g, PK_STAT_TRAINER_BATTLES),
         (unsigned)pk_game_stat(sb1, sb2, g, PK_STAT_POKEMON_CAPTURES),
         (unsigned)pk_game_stat(sb1, sb2, g, PK_STAT_HATCHED_EGGS));
  return 0;
}
